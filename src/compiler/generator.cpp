#include "compiler/generator.h"

#include <functional>
#include <string>
#include <random>
#include <variant>
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "utils/vmt_util.h"
#include "runtime/runtime.h"

#include "llvm/Bitcode/BitcodeWriter.h"

using std::string;
using std::vector;
using std::unordered_map;
using std::unordered_set;
using std::swap;
using std::move;
using std::pair;
using std::function;
using std::variant;
using std::get_if;
using ltm::weak;
using ltm::own;
using ltm::pin;
using std::uintptr_t;
using dom::isa;

const int AG_HEADER_OFFSET = 0; // -1 if dispatcher and counter to be accessed by negative offsets (which speeds up all ffi, but is incompatible with moronic LLVM debug info)

#define AK_STR(X) #X
#define DUMP(X) dump(AK_STR(X), X)
template <typename T>
T* dump(const char* name, T* val) {
	llvm::outs() << "---" << name << ":";
	if (!val)
		llvm::outs() << "null";
	else
		val->print(llvm::outs(), false);
	llvm::outs() << "\n";
	return val;
}

// 
struct OptBranch : ltm::Object {
	own<OptBranch> deeper;
	llvm::BasicBlock* none_bb = nullptr;
	bool is_wrapped = false;
	OptBranch() { make_shared(); }
	LTM_COPYABLE(OptBranch);
};

// Represents temp values. Variables and fields are not values.
struct Val {
	own<ast::Type> type; // used to optinize retain/release for non-optionals, and for optional branches folding
	llvm::Value* data = nullptr;
	struct NonPtr {}; // non-pointer data (or nullptr that can be safely passed to fnRelease), default case
	struct Temp { weak<ast::Var> var; };  // value fetched from local variable or field, see below
	struct Retained {}; // retained temp (fn result, newly constructed object, locked for other reasons), must be released or moved to Var or to Val::RField::to_release.
	struct RField { llvm::Value* to_release; };  // temp raw, that represents child subobject, of Retained temp. It must be locked and converted to Retained if needed, it must destroy its protector in the end. 
	// nonptr fields become NonPtr
	// ptr fields of Retained become RField, other ptr's fields become Temp{null}
	std::variant<NonPtr, Temp, Retained, RField> lifetime;
	// If set, this value is optional and returned as branches.
	// Current branch holds presented value (in wrapped or unwrapped),
	// the optional_br->none_bb is for not present value.
	// Nested optionals represented with `deeper` chain. 
	own<OptBranch> optional_br;
};

// Temp meaning
//		var == null -> it's field of elsewhere retained object
//			act naked in get/setfield
//			Retained in all other cases.
//		var is mutable
//			can be returned from block or fn by transferring lock;
//			act naked in get/setfield;
//			Retained in all other cases.
//		var is immutable
//			can be returned from block if its var is outer
//			can be returned from block or fn by transferring lock
//			Retained in all other cases.
// Temp handling
// If its Var is null, it's field of elsewhere locked object: access it with another getfield or lock (for longterm usage or return).
// Otherwise it's a current value of a var:
//     - if returned from block
//			- not having this var, leave it Temp.
//			- having this var, skip var unlock, mark it Retained.
//	   - if returned from fn
//			- fn having this var if var is mutable, skip var unlock, mark it Retained.
//		    - otherwise Retain.
//	   - if getField, Make new temp having Var=0
//	   - if Var is immutable, keep longterm using it as temp.
//     - otherwise Retain.

struct MethodInfo {
	llvm::FunctionType* type = nullptr;
	size_t ordinal = 0;  // index in vmt
};

struct ClassInfo {
	llvm::StructType* fields = nullptr; // header{disp, counter} + fields. To access dispatcher or counter
										// obj_ptr{dispatcher_fn*, counter}; where dispatcher_fn void*(uint64_t interface_and_method_id)
										// to access vmt: cast dispatcher_fn to vmt and apply offset -1
	llvm::StructType* vmt = nullptr;       // only for class { (dispatcher_fn_used_as_id*, methods*)*, copier_fn*, disposer_fn*, instance_size, vmt_size};
	uint64_t vmt_size = 0;                 // vmt bytes size - used in casts
	llvm::Function* constructor = nullptr; // T*()
	llvm::Function* initializer = nullptr; // void(void*)
	llvm::Function* copier = nullptr;      // void(void* dst, void* src);
	llvm::Function* dispose = nullptr;     // void(void*);
	llvm::Function* dispatcher = nullptr;  // void*(void*obj, uint64 inerface_and_method_ordinal);
	vector<llvm::Constant*> vmt_fields;    // pointers to methods. size <= 2^16, at index 0 - inteface id for dynamic cast
	uint64_t interface_ordinal = 0;        // 48_bit_random << 16
	llvm::ArrayType* ivmt = nullptr;       // only for interface i8*[methods_count+1], ivmt[0]=inteface_id
};

struct Generator : ast::ActionScanner {
	ltm::pin<ast::Ast> ast;
	std::unique_ptr<llvm::LLVMContext> context;
	std::unique_ptr<llvm::Module> module;
	llvm::IntegerType* int_type = nullptr;
	llvm::Type* double_type = nullptr;
	llvm::PointerType* ptr_type = nullptr;
	llvm::Type* void_type = nullptr;
	llvm::IRBuilder<>* builder = nullptr;
	Val* result = nullptr;
	llvm::DataLayout layout;
	unordered_map<pin<ast::TpLambda>, llvm::FunctionType*> lambda_fns; // function having 0th param of ptr_type
	unordered_map<pin<ast::TpFunction>, llvm::FunctionType*> function_types;
	unordered_map<pin<ast::TpClass>, ClassInfo> classes;
	unordered_map<pin<ast::Method>, MethodInfo> methods;
	unordered_map<pin<ast::Function>, llvm::Function*> functions;
	
	llvm::DICompileUnit* di_cu = nullptr;
	unordered_map<weak<dom::Name>, llvm::DIFile*> di_files;
	llvm::DIScope* current_di_scope = nullptr;
	llvm::DIType* current_capture_di_type = nullptr;
	std::unique_ptr<llvm::DIBuilder> di_builder;
	llvm::DIType* di_double = nullptr;
	llvm::DIType* di_int = nullptr;
	llvm::DIType* di_obj_ptr = nullptr;
	llvm::DIType* di_weak_ptr = nullptr;
	llvm::DIType* di_opt_int = nullptr;
	llvm::DIType* di_delegate = nullptr;
	llvm::DICompositeType* di_obj_variants = nullptr;

	llvm::Function* current_function = nullptr;
	unordered_map<weak<ast::Var>, llvm::Value*> locals;
	unordered_map<weak<ast::Var>, int> capture_offsets;
	vector<pair<int, llvm::StructType*>> captures;
	vector<llvm::Value*> capture_ptrs;
	llvm::Type* tp_opt_int = nullptr;
	llvm::Type* tp_opt_double = nullptr;
	llvm::Type* tp_bool = nullptr;
	llvm::Type* tp_opt_bool = nullptr;
	llvm::Type* tp_opt_lambda = nullptr;
	llvm::Type* tp_opt_delegate = nullptr;
	llvm::Type* tp_int_ptr = nullptr;
	llvm::StructType* lambda_struct = nullptr;
	llvm::StructType* delegate_struct = nullptr;
	llvm::StructType* obj_struct = nullptr;
	llvm::StructType* weak_struct = nullptr;
	llvm::StructType* obj_vmt_struct = nullptr;
	llvm::Function* fn_release_pin = nullptr;  // void(Obj*) no_throw
	llvm::Function* fn_release_own = nullptr;  // void(Obj*) no_throw as pin + clears parent
	llvm::Function* fn_retain_own = nullptr;  // void(Obj*, Obj*parent) no_throw as pin + sets parent, used in set_field
	llvm::Function* fn_relase_weak = nullptr;  // void(WB*) no_throw
	llvm::Function* fn_dispose = nullptr;  // void(Obj*) no_throw // used in releaseObj
	llvm::Function* fn_allocate = nullptr; // Obj*(size_t)
	llvm::Function* fn_copy = nullptr;   // Obj*(Obj*)
	llvm::Function* fn_mk_weak = nullptr;   // WB*(Obj*)
	llvm::Function* fn_deref_weak = nullptr;   // intptr_aka_?obj* (WB*)
	llvm::Function* fn_copy_object_field = nullptr;   // Obj* (Obj* src, Obj* parent)
	llvm::Function* fn_copy_weak_field = nullptr;   // void(WB** dst, WB* src)
	llvm::FunctionType* fn_copy_fixer_fn_type = nullptr;  // void (*)(Obj*)
	llvm::Function* fn_reg_copy_fixer = nullptr;      // void (Obj*, fn_fixer_type)
	std::default_random_engine random_generator;
	std::uniform_int_distribution<uint64_t> uniform_uint64_distribution;
	unordered_set<uint64_t> assigned_interface_ids;
	llvm::FunctionType* dispatcher_fn_type = nullptr;
	llvm::Constant* empty_mtable = nullptr; // void_ptr[1] = { null }
	unordered_map<weak<ast::MkLambda>, llvm::Function*> compiled_functions;
	llvm::Constant* null_weak = nullptr;
	llvm::Constant* const_0 = nullptr;
	llvm::Constant* const_1 = nullptr;
	llvm::Constant* const_256 = nullptr;

	Generator(ltm::pin<ast::Ast> ast, bool debug_info_mode)
		: ast(ast)
		, context(new llvm::LLVMContext)
		, layout("")
	{
		module = std::make_unique<llvm::Module>("code", *context);
		if (debug_info_mode)
			make_di_basic();
		int_type = llvm::Type::getInt64Ty(*context);
		double_type = llvm::Type::getDoubleTy(*context);
		ptr_type = llvm::PointerType::getUnqual(*context);
		tp_int_ptr = layout.getIntPtrType(ptr_type);
		void_type = llvm::Type::getVoidTy(*context);
		tp_opt_bool = llvm::Type::getInt8Ty(*context);
		tp_opt_int = llvm::StructType::get(*context, { tp_opt_bool, int_type });
		tp_opt_double = int_type;
		tp_bool = llvm::Type::getInt1Ty(*context);
		tp_opt_lambda = llvm::StructType::get(*context, { tp_int_ptr, tp_int_ptr });
		tp_opt_delegate = tp_opt_lambda;
		lambda_struct = llvm::StructType::get(*context, { ptr_type, ptr_type }); // context, entrypoint
		delegate_struct = lambda_struct;  // also 2 ptrs, but (weak, entrypoint)
		obj_struct = llvm::StructType::get(*context, { ptr_type, tp_int_ptr, tp_int_ptr });  // disp, counter, parent/weak
		weak_struct = llvm::StructType::get(*context, { ptr_type, tp_int_ptr, tp_int_ptr });  // target, w_counter, org_parent
		empty_mtable = make_const_array("empty_mtable", { llvm::Constant::getNullValue(ptr_type) });
		null_weak = llvm::Constant::getNullValue(ptr_type);
		const_0 = llvm::ConstantInt::get(tp_int_ptr, 0);
		const_1 = llvm::ConstantInt::get(tp_int_ptr, 1);
		const_256 = llvm::ConstantInt::get(tp_int_ptr, 256);

		fn_dispose = llvm::Function::Create(
			llvm::FunctionType::get(void_type, { ptr_type }, false),
			llvm::Function::ExternalLinkage,
			"ag_dispose_obj",
			*module);
		fn_retain_own = llvm::Function::Create(
			llvm::FunctionType::get(void_type, { ptr_type, ptr_type }, false),
			llvm::Function::ExternalLinkage,
			"ag_retain_own",
			*module);
		fn_release_pin = llvm::Function::Create(
			llvm::FunctionType::get(void_type, { ptr_type }, false),
			llvm::Function::ExternalLinkage,
			"ag_release",
			*module);
		fn_release_own = llvm::Function::Create(
			llvm::FunctionType::get(void_type, { ptr_type }, false),
			llvm::Function::ExternalLinkage,
			"ag_release_own",
			*module);
		fn_relase_weak = llvm::Function::Create(
			llvm::FunctionType::get(void_type, { ptr_type }, false),
			llvm::Function::ExternalLinkage,
			"ag_release_weak",
			*module);
		fn_allocate = llvm::Function::Create(
			llvm::FunctionType::get(ptr_type, { int_type }, false),
			llvm::Function::ExternalLinkage,
			"ag_allocate_obj",
			*module);
		fn_copy = llvm::Function::Create(
			llvm::FunctionType::get(ptr_type, { ptr_type }, false),
			llvm::Function::ExternalLinkage,
			"ag_copy",
			*module);
		fn_copy_object_field = llvm::Function::Create(
			llvm::FunctionType::get(ptr_type, { ptr_type, ptr_type }, false),
			llvm::Function::ExternalLinkage,
			"ag_copy_object_field",
			*module);
		fn_copy_weak_field = llvm::Function::Create(
			llvm::FunctionType::get(void_type, { ptr_type, ptr_type }, false),
			llvm::Function::ExternalLinkage,
			"ag_copy_weak_field",
			*module);
		fn_copy_fixer_fn_type = llvm::FunctionType::get(void_type, { ptr_type }, false);
		fn_reg_copy_fixer = llvm::Function::Create(
			llvm::FunctionType::get(void_type, { ptr_type, ptr_type }, false),
			llvm::Function::ExternalLinkage,
			"ag_reg_copy_fixer",
			*module);
		fn_mk_weak = llvm::Function::Create(
			llvm::FunctionType::get(ptr_type, { ptr_type }, false),
			llvm::Function::ExternalLinkage,
			"ag_mk_weak",
			*module);
		fn_deref_weak = llvm::Function::Create(
			llvm::FunctionType::get(tp_int_ptr, { ptr_type }, false),
			llvm::Function::ExternalLinkage,
			"ag_deref_weak",
			*module);
	}

	void make_di_basic() {
		di_builder = std::make_unique<llvm::DIBuilder>(*module);
		di_cu = di_builder->createCompileUnit(
			llvm::dwarf::DW_LANG_C_plus_plus,
			di_builder->createFile("sys", ""),
			"sys",
			true, "", 0);
		di_double = di_builder->createBasicType("double", 64, llvm::dwarf::DW_ATE_float);
		di_int = di_builder->createBasicType("int", 64, llvm::dwarf::DW_ATE_signed);
		auto obj_struct = di_builder->createStructType(
			di_cu, "_obj", di_cu->getFile(),
			0, 2 * 64, 0,
			llvm::DINode::DIFlags::FlagZero, nullptr,
			di_builder->getOrCreateArray({
				di_builder->createMemberType(nullptr, "disp", nullptr,    0, 64, 0, 0, llvm::DINode::DIFlags::FlagZero, di_int),
				di_builder->createMemberType(nullptr, "counter", nullptr, 0, 64, 0, 64, llvm::DINode::DIFlags::FlagZero, di_int),
				di_obj_variants = di_builder->createVariantPart(
					nullptr, "_obj_fields", nullptr,
					0, 0, 0,
					llvm::DINode::DIFlags::FlagZero,
					di_builder->createMemberType(nullptr, "_cls_id", nullptr, 0, 64, 0, 0, llvm::DINode::DIFlags::FlagZero, di_int),
					di_builder->getOrCreateArray({})
				)
			})
		);
		di_obj_ptr = di_builder->createPointerType(
			obj_struct,
			64);
		di_weak_ptr = di_builder->createPointerType(
			di_builder->createStructType(
				di_cu, "_weak", di_cu->getFile(),
				0, 3 * 64, 0,
				llvm::DINode::DIFlags::FlagZero, nullptr,
				di_builder->getOrCreateArray({
					di_builder->createMemberType(di_cu, "target", di_cu->getFile(), 0, 64, 0, 0, llvm::DINode::DIFlags::FlagZero, di_obj_ptr),
					di_builder->createMemberType(di_cu, "counter", di_cu->getFile(), 0, 64, 0, 64, llvm::DINode::DIFlags::FlagZero, di_int),
					di_builder->createMemberType(di_cu, "org_ctr", di_cu->getFile(), 0, 64, 0, 64*2, llvm::DINode::DIFlags::FlagZero, di_int) })),
			64);
		di_opt_int = di_builder->createStructType(
				di_cu, "_opt_int", di_cu->getFile(),
				0, 2 * 64, 0,
				llvm::DINode::DIFlags::FlagZero, nullptr,
				di_builder->getOrCreateArray({
					di_builder->createMemberType(di_cu, "opt", di_cu->getFile(), 0, 64, 0, 0, llvm::DINode::DIFlags::FlagZero, di_int),
					di_builder->createMemberType(di_cu, "val", di_cu->getFile(), 0, 64, 0, 64, llvm::DINode::DIFlags::FlagZero, di_int)}));
		di_delegate = di_builder->createStructType(
			di_cu, "_delegate", di_cu->getFile(),
			0, 2 * 64, 0,
			llvm::DINode::DIFlags::FlagZero, nullptr,
			di_builder->getOrCreateArray({
				di_builder->createMemberType(di_cu, "weak", di_cu->getFile(), 0, 64, 0, 0, llvm::DINode::DIFlags::FlagZero, di_weak_ptr),
				di_builder->createMemberType(di_cu, "fn", di_cu->getFile(), 0, 64, 0, 64, llvm::DINode::DIFlags::FlagZero, di_int) }));
	}
	[[noreturn]] void internal_error(ast::Node& n, const char* message) {
		n.error("internal error: ", message);
	}

	[[nodiscard]] Val compile(own<ast::Action>& action) {
		auto prev = result;
		Val r;
		result = &r;
		if (current_di_scope) {
			builder->SetCurrentDebugLocation(llvm::DILocation::get(
				current_di_scope->getContext(),
				action->line,
				action->pos,
				current_di_scope));
		}
		action->match(*this);
		assert(!r.type || r.type == action->type());
		if (!r.type)
			r.type = action->type();
		result = prev;
		return r;
	}

	bool is_ptr(pin<ast::Type> type) {
		auto as_opt = dom::strict_cast<ast::TpOptional>(type);
		if (as_opt)
			type = as_opt->wrapped;
		return dom::strict_cast<ast::TpClass>(type) ||
			dom::strict_cast<ast::TpRef>(type) ||
			dom::strict_cast<ast::TpWeak>(type) ||
			dom::strict_cast<ast::TpDelegate>(type);
	}

	void build_inc(llvm::Value* addr) {
		builder->CreateStore(
			builder->CreateAdd(
				builder->CreateLoad(tp_int_ptr, addr),
				const_1),
			addr);
	}
	void build_retain_not_null(llvm::Value* ptr, const ast::Type& type, llvm::Value* maybe_own_parent = nullptr) {
		if (isa<ast::TpWeak>(type)) {
			build_inc(builder->CreateStructGEP(weak_struct, ptr, 1));
		} else if (isa<ast::TpDelegate>(type)) {
			build_inc(
				builder->CreateStructGEP(
					obj_struct,
					builder->CreateExtractValue(ptr, { 0 }),
					1));
		} else if (isa<ast::TpRef>(type) || (isa<ast::TpClass>(type) && !maybe_own_parent)) {
			build_inc(builder->CreateStructGEP(obj_struct, ptr, 1));
		} else if (isa<ast::TpClass>(type)) {
			builder->CreateCall(fn_retain_own, { cast_to(ptr, ptr_type), maybe_own_parent });
		}
	}
	void build_retain(llvm::Value* ptr, pin<ast::Type> type, llvm::Value* maybe_own_parent = nullptr) {
		if (!is_ptr(type))
			return;
		if (auto as_opt = dom::strict_cast<ast::TpOptional>(type)) {
			if (isa<ast::TpClass>(*as_opt->wrapped) && maybe_own_parent) {
				builder->CreateCall(fn_retain_own, { cast_to(ptr, ptr_type), maybe_own_parent });
				return;
			}
			auto bb_not_null = llvm::BasicBlock::Create(*context, "", current_function);
			auto bb_null = llvm::BasicBlock::Create(*context, "", current_function);
			builder->CreateCondBr(
				builder->CreateCmp(llvm::CmpInst::Predicate::ICMP_UGT,
					cast_to(ptr, tp_int_ptr),
					const_256),
				bb_not_null,
				bb_null);
			builder->SetInsertPoint(bb_not_null);
			build_retain_not_null(ptr, *as_opt->wrapped);
			builder->CreateBr(bb_null);
			builder->SetInsertPoint(bb_null);
		} else {
			build_retain_not_null(ptr, *type);
		}
	}

	void build_release_ptr_not_null(llvm::Value* ptr) {
		llvm::Value* counter_addr = builder->CreateStructGEP(obj_struct, ptr, 1);
		llvm::Value* ctr = builder->CreateSub(
			builder->CreateLoad(tp_int_ptr, counter_addr),
			const_1);
		builder->CreateStore(ctr, counter_addr);
		auto bb_not_zero = llvm::BasicBlock::Create(*context, "", current_function);
		auto bb_zero = llvm::BasicBlock::Create(*context, "", current_function);
		builder->CreateCondBr(
			builder->CreateCmp(llvm::CmpInst::Predicate::ICMP_EQ, ctr, const_0),
			bb_zero,
			bb_not_zero);
		builder->SetInsertPoint(bb_zero);
		builder->CreateCall(fn_dispose, { cast_to(ptr, ptr_type) });
		builder->CreateBr(bb_not_zero);
		builder->SetInsertPoint(bb_not_zero);
	}

	void build_release(llvm::Value* ptr, pin<ast::Type> type) {
		if (!is_ptr(type))
			return;
		if (auto as_opt = dom::strict_cast<ast::TpOptional>(type)) {
			if (isa<ast::TpWeak>(*type)) {
				builder->CreateCall(fn_relase_weak, { cast_to(ptr, ptr_type) });
			} else if (isa<ast::TpDelegate>(*type)) {
				builder->CreateCall(fn_relase_weak, { builder->CreateExtractValue(ptr, {0}) });
			} else if (isa<ast::TpRef>(*type)) {
				builder->CreateCall(fn_release_pin, { cast_to(ptr, ptr_type) });
			} else if (isa<ast::TpClass>(*type) || isa<ast::TpRef>(*type)) {
				builder->CreateCall(fn_release_own, { cast_to(ptr, ptr_type) });
			}
		} else {
			if (isa<ast::TpWeak>(*type)) {
				builder->CreateCall(fn_relase_weak, { cast_to(ptr, ptr_type) });
			} else if (isa<ast::TpDelegate>(*type)) {
				builder->CreateCall(fn_relase_weak, { builder->CreateExtractValue(ptr, {0}) });
			} else if (isa<ast::TpRef>(*type)) {
				build_release_ptr_not_null(ptr);
			} else if (isa<ast::TpClass>(*type)) {
				builder->CreateCall(fn_release_own, { ptr });
			}
		}
	}
	
	llvm::Value* remove_indirection(const ast::Var& var, llvm::Value* val) {
		return var.is_mutable || var.captured
			? builder->CreateLoad(to_llvm_type(*var.type),  val)
			: val;
	}

	void dispose_val(Val&& val) {
		if (auto as_retained = get_if<Val::Retained>(&val.lifetime)) {
			build_release(val.data, val.type);
		} else if (auto as_rfield = get_if<Val::RField>(&val.lifetime)) {
			build_release_ptr_not_null(as_rfield->to_release);
		}
		if (val.optional_br) {
			auto common_bb = llvm::BasicBlock::Create(*context, "", current_function);
			builder->CreateBr(common_bb);
			for (auto& b = val.optional_br; b; b = b->deeper) {
				if (b->none_bb) {
					builder->SetInsertPoint(b->none_bb);
					builder->CreateBr(common_bb);
				}
			}
			builder->SetInsertPoint(common_bb);
			val.optional_br = nullptr;
		}
	}

	void comp_to_void(own<ast::Action>& action) {
		dispose_val(compile(action));
	}

	void remove_branches(Val& val) {
		if (!val.optional_br)
			return;
		auto val_bb = builder->GetInsertBlock();
		auto common_bb = llvm::BasicBlock::Create(*context, "", current_function);
		auto type = dom::strict_cast<ast::TpOptional>(val.type);
		auto wrapped_val = val.optional_br->is_wrapped
			? val.data
			: make_opt_val(val.data, type);
		builder->CreateBr(common_bb);
		builder->SetInsertPoint(common_bb);
		auto phi = builder->CreatePHI(to_llvm_type(*val.type), 2);
		phi->addIncoming(wrapped_val, val_bb);
		for (auto b = val.optional_br;
			b;
			b = b->deeper,
			type = dom::strict_cast<ast::TpOptional>(ast->get_wrapped(type)))
		{
			if (b->none_bb) {
				builder->SetInsertPoint(b->none_bb);
				auto none_val = make_opt_none(type);
				builder->CreateBr(common_bb);
				phi->addIncoming(none_val, b->none_bb);
			}
		}
		builder->SetInsertPoint(common_bb);
		val.data = phi;
		val.optional_br = nullptr;
	}

	void persist_rfield(Val& val, llvm::Value* maybe_own_parent = nullptr) {
		if (auto as_rfield = get_if<Val::RField>(&val.lifetime)) {
			build_retain(val.data, val.type, maybe_own_parent);
			build_release_ptr_not_null(as_rfield->to_release);
			val.lifetime = Val::Retained{};
		}
	}

	// Make value be able to outlive any random field and var assignment. Makes NonPtr, Retained or a var-bounded Temp.
	Val persist_val(Val&& val, llvm::Value* maybe_own_parent = nullptr, bool retain_mutable_locals = true) {
		persist_rfield(val, maybe_own_parent);
		if (auto as_temp = get_if<Val::Temp>(&val.lifetime)) {
			if (!as_temp->var || (as_temp->var->is_mutable && retain_mutable_locals)) {
				build_retain(val.data, val.type, maybe_own_parent);
				val.lifetime = Val::Retained{};
			}
		}
		remove_branches(val);
		return val;
	}
	Val make_retained_or_non_ptr(Val&& src, llvm::Value* maybe_own_parent = nullptr) {
		auto r = persist_val(move(src), maybe_own_parent);
		if (get_if<Val::Temp>(&r.lifetime)) {
			build_retain(r.data, r.type, maybe_own_parent);
			r.lifetime = Val::Retained{};
		}
		return r;
	}

	// Make value be able to outlive any random field and var assignment.
	[[nodiscard]] Val comp_to_persistent(own<ast::Action>& action, bool retain_mutable_locals = true) {
		return persist_val(
			compile(action),
			nullptr,  // maybe_own_parent. Null, because comp_to_persistent never used in set_field value
			retain_mutable_locals);
	}

	llvm::Value* cast_to(llvm::Value* value, llvm::Type* expected) {
		return value->getType() == expected
			? value
			: builder->CreateBitOrPointerCast(value, expected);
	}
	llvm::Value* comp_non_ptr(own<ast::Action>& action) {
		auto r = compile(action);
		assert(get_if<Val::NonPtr>(&r.lifetime));
		return r.data;
	}
	void on_const_i64(ast::ConstInt64& node) override { result->data = builder->getInt64(node.value); }
	void on_const_double(ast::ConstDouble& node) override { result->data = llvm::ConstantFP::get(double_type, node.value); }
	void on_const_void(ast::ConstVoid&) override { result->data = llvm::UndefValue::get(void_type); }
	void on_const_bool(ast::ConstBool& node) override { result->data = builder->getInt1(node.value); }
	void on_const_string(ast::ConstString& node) override {
		auto& str = classes[ast->string_cls];
		result->data = builder->CreateCall(str.constructor, {});
		builder->CreateStore(
			builder->CreateGlobalStringPtr(node.value),
			builder->CreateStructGEP(str.fields, result->data, 2));
		result->lifetime = Val::Retained{};
	}

	unordered_map<ast::Type*, llvm::DISubroutineType*> di_fn_types;

	llvm::DISubroutineType* to_di_fn_type(ast::Type& tp) {
		auto& r = di_fn_types[&tp];
		if (!r && (isa<ast::TpFunction>(tp) || isa < ast::TpLambda>(tp) || isa<ast::TpDelegate>(tp))) {
			auto type = (ast::TpFunction*) &tp;
			vector<llvm::Metadata*> params;
			params.reserve(type->params.size());
			params.push_back(to_di_type(*type->params.back()));
			for (auto& p : type->params) {
				if (p != type->params.back())
					params.push_back(to_di_type(*p));
			}
			r = di_builder->createSubroutineType(di_builder->getOrCreateTypeArray(params));
		}
		return r;
	}

	llvm::DIType* to_di_type(ast::Type& tp) {
		struct DiTypeMatcher : ast::TypeMatcher {
			Generator* gen;
			llvm::DIType* result = nullptr;
			DiTypeMatcher(Generator* gen) :gen(gen) {}
			void on_int64(ast::TpInt64& type) override { result = gen->di_int; }
			void on_double(ast::TpDouble& type) override { result = gen->di_double; }
			void on_function(ast::TpFunction& type) override { result = gen->di_int; }   // todo: raw ptr
			void on_lambda(ast::TpLambda& type) override { result = gen->di_int; }  // todo: ptr to { ptr to capture, raw ptr }
			void on_cold_lambda(ast::TpColdLambda& type) override { result = gen->di_builder->createBasicType("void", 64, llvm::dwarf::DW_ATE_unsigned); }
			void on_void(ast::TpVoid&) override { result = gen->di_builder->createBasicType("void", 64, llvm::dwarf::DW_ATE_unsigned); }
			void on_optional(ast::TpOptional& type) {
				if (isa<ast::TpInt64>(*type.wrapped)) result = gen->di_opt_int;
				else if (isa<ast::TpVoid>(*type.wrapped)) result = gen->di_int;
				else type.wrapped->match(*this);
			}
			void on_class(ast::TpClass& type) override { result = gen->di_obj_ptr; }
			void on_ref(ast::TpRef& type) override { result = gen->di_obj_ptr; }
			void on_weak(ast::TpWeak& type) override { result = gen->di_weak_ptr; }
			void on_delegate(ast::TpDelegate& type) override { result = gen->di_delegate; }
		};
		DiTypeMatcher matcher(this);
		tp.match(matcher);
		return matcher.result;
	}

	void insert_di_var(string name, int line, int pos, llvm::DIType* type, llvm::Value* data_addr) {
		if (!di_builder)
			return;
		di_builder->insertDeclare(
			data_addr,
			di_builder->createAutoVariable(
				current_di_scope,
				name,
				current_di_scope->getFile(),
				line,
				type,
				true), // preserve
			di_builder->createExpression(),
			llvm::DILocation::get(
				current_di_scope->getContext(),
				line,
				pos,
				current_di_scope),
			builder->GetInsertBlock());
	}

	void insert_di_var(pin<ast::Var> p, llvm::Value* data_addr) {
		if (di_builder)
			insert_di_var(std::to_string(p->name.pinned()), p->line, p->pos, to_di_type(*p->type), data_addr);
	}

	void compile_fn_body(ast::MkLambda& node, string di_name, llvm::Type* closure_struct = nullptr) {
		unordered_map<weak<ast::Var>, llvm::Value*> outer_locals;
		unordered_map<weak<ast::Var>, int> outer_capture_offsets = capture_offsets;
		swap(outer_locals, locals);
		vector<llvm::Value*> prev_capture_ptrs;
		swap(capture_ptrs, prev_capture_ptrs);
		auto prev_builder = builder;
		llvm::IRBuilder fn_bulder(llvm::BasicBlock::Create(*context, "", current_function));
		this->builder = &fn_bulder;
		llvm::DIScope* prev_di_scope = current_di_scope;
		if (node.module_name) {
			if (auto di_file = di_files[node.module_name]) {
				auto sub = di_builder->createFunction(
					di_cu,
					di_name,
					"",  // linkage name
					di_file,
					node.line,
					to_di_fn_type(*node.type()),
					node.line,
					llvm::DINode::FlagPrototyped,
					llvm::DISubprogram::SPFlagDefinition);
				current_function->setSubprogram(sub);
				current_di_scope = sub;
			}
		}
		bool has_parent_capture_ptr = isa<ast::MkLambda>(node) && closure_struct != nullptr;
		pin<ast::Var> this_source = isa<ast::Method>(node) ? node.names.front().pinned() : nullptr;
		auto prev_capture_di_type = current_capture_di_type;
		llvm::AllocaInst* capture = nullptr;
		if (!node.captured_locals.empty()) {
			if (di_builder) {
				vector<llvm::Metadata*> di_captures;
				size_t offset = 0;
				if (current_capture_di_type) {
					di_captures.push_back(di_builder->createMemberType(
						current_di_scope,
						"parent",
						nullptr,
						0, 64, 0, offset,
						llvm::DINode::DIFlags::FlagZero,
						di_builder->createPointerType(current_capture_di_type, 64)));
					offset += 64;
				}
				for (auto& p : node.captured_locals) {
					di_captures.push_back(di_builder->createMemberType(
						nullptr,
						std::to_string(p->name.pinned()),
						nullptr,
						0, 64, 0, offset,
						llvm::DINode::DIFlags::FlagZero,
						to_di_type(*p->type)));
					offset += to_llvm_type(*p->type)->getScalarSizeInBits();
				}
				current_capture_di_type = di_builder->createStructType(
					nullptr,
					ast::format_str("cap_", di_name),
					nullptr,
					node.line, offset, 0,
					llvm::DINode::DIFlags::FlagZero,
					nullptr,  // parent
					di_builder->getOrCreateArray(move(di_captures)));
			}
			vector<llvm::Type*> captured_local_types;
			if (has_parent_capture_ptr)
				captured_local_types.push_back(ptr_type);  // for outer capture
			for (auto& p : node.captured_locals) {
				capture_offsets.insert({ p, captured_local_types.size() });
				captured_local_types.push_back(p == this_source
					? ptr_type
					: to_llvm_type(*p->type));
			}
			captures.push_back({ node.lexical_depth, llvm::StructType::get(*context, captured_local_types) });
			capture = builder->CreateAlloca(captures.back().second);
			capture_ptrs.push_back(capture);
			if (has_parent_capture_ptr) {
				builder->CreateStore(&*current_function->arg_begin(), builder->CreateStructGEP(captures.back().second, capture, 0));
			}
			if (di_builder) {
				insert_di_var("current_closure", node.line, node.pos, current_capture_di_type, capture);
			}
		}
		for (auto& local : node.mutables) {
			if (local->captured)
				continue;
			locals.insert({
				local,
				builder->CreateAlloca(to_llvm_type(*(local->type))) });
		}
		if (di_builder) {
			auto param_iter = current_function->arg_begin();
			if (isa<ast::MkLambda>(node)) {
				if (closure_struct != nullptr) {
					auto dbg_param_val = builder->CreateAlloca(ptr_type);
					insert_di_var("parent_closure", node.line, node.pos, di_builder->createPointerType(prev_capture_di_type, 64), dbg_param_val);
					builder->CreateStore(&*param_iter, dbg_param_val);
				}
				++param_iter;
			}
			for (auto& p : node.names) {
				if (!p->is_mutable && !p->captured) {
					auto dbg_param_val = builder->CreateAlloca(to_llvm_type(*p->type));
					insert_di_var(p, dbg_param_val);
					builder->CreateStore(&*param_iter, dbg_param_val);
				}
				++param_iter;
			}
		}
		if (has_parent_capture_ptr)
			capture_ptrs.push_back(&*current_function->arg_begin());
		auto param_iter = current_function->arg_begin() + (isa<ast::MkLambda>(node) ? 1 : 0);
		for (auto& p : node.names) {
			auto p_val = &*param_iter;
			auto p_is_ptr = p == this_source
				? true
				: is_ptr(p->type);
			if (p_is_ptr && p->is_mutable)
				build_retain(&*param_iter, p->type);

			if (p->captured) {
				auto addr = builder->CreateStructGEP(captures.back().second, capture, capture_offsets[p]);
				builder->CreateStore(p_val, addr);
				locals.insert({ p, addr });
			} else if (p->is_mutable) {
				insert_di_var(p, locals[p]);
				builder->CreateStore(&*param_iter, locals[p]);
			} else {
				locals.insert({ p, p_val });
			}
			++param_iter;
		}
		for (auto& a : node.body) {
			if (a != node.body.back())
				comp_to_void(a);
		}
		auto fn_result = compile(node.body.back());
		persist_rfield(fn_result);
		auto result_as_temp = get_if<Val::Temp>(&fn_result.lifetime); // null if not temp
		auto make_result_retained = [&](bool actual_retain = true) {
			if (actual_retain)
				build_retain(fn_result.data, fn_result.type);
			fn_result.lifetime = Val::Retained{};
			result_as_temp = nullptr;
		};
		if (result_as_temp && !result_as_temp->var)  // if connected to field
			make_result_retained();
		for (auto& p : node.names) {
			// param\	 |	returned      | non-returned
			// mutable	 |	nothing       | release
			// immutable |  retain result | nothing
			if (result_as_temp && result_as_temp->var == p) {
				make_result_retained(!p->is_mutable);
			} else if (p->is_mutable && is_ptr(p->type)) {
				build_release(remove_indirection(*p, locals[p]), p->type);
			}
		}
		if (get_if<Val::Temp>(&fn_result.lifetime)) {  // if connected to outer local/param
			make_result_retained();
		}
		if (isa<ast::TpVoid>(*fn_result.type))
			builder->CreateRetVoid();
		else
			builder->CreateRet(fn_result.data);
		current_di_scope = prev_di_scope;
		current_capture_di_type = prev_capture_di_type;
		builder = prev_builder;
		if (!captures.empty() && captures.back().first == node.lexical_depth)
			captures.pop_back();
		locals = move(outer_locals);
		capture_offsets = move(outer_capture_offsets);
		capture_ptrs = move(prev_capture_ptrs);
	}

	// `closure_ptr_type` define the type `this` or `closure` parameter.
	llvm::Function* compile_function(ast::MkLambda& node, string name, llvm::Type* closure_ptr_type) {
		if (auto seen = compiled_functions[&node])
			return seen;
		llvm::Function* prev = current_function;
		current_function = llvm::Function::Create(
			lambda_to_llvm_fn(node, node.type()),
			llvm::Function::InternalLinkage,
			name,
			module.get());
		compile_fn_body(node, name, closure_ptr_type);
		swap(prev, current_function);
		compiled_functions[&node] = prev;
		return prev;
	}

	void on_mk_lambda(ast::MkLambda& node) override {
		llvm::Function* function = compile_function(
			node,
			ast::format_str("L_", node.module_name.pinned(), '_', node.line, '_', node.pos),
			captures.empty()
				? nullptr
				: captures.back().second->getPointerTo());
		auto r = builder->CreateInsertValue(
			llvm::UndefValue::get(lambda_struct),
			capture_ptrs.empty()
				? llvm::Constant::getNullValue(ptr_type)
				: cast_to(capture_ptrs.front(), ptr_type),
			{ 0 });
		r = builder->CreateInsertValue(r, function, { 1 });
		result->data = r;
	}

	Val handle_block(ast::Block& node, Val parameter) {
		auto prev_di_scope = current_di_scope;
		current_di_scope = current_di_scope
			? di_builder->createLexicalBlock(current_di_scope, current_di_scope->getFile(), node.line, node.pos)
			: nullptr;
		vector<Val> to_dispose; // mutable ? addr : initializer_value
		for (auto& l : node.names) {
			to_dispose.push_back(l->initializer
				? comp_to_persistent(l->initializer)
				: parameter);
			auto& initializer = to_dispose.back();
			if (l->is_mutable || l->captured) {
				auto& addr = locals[l];
				if (l->is_mutable)
					initializer = make_retained_or_non_ptr(move(initializer));
				if (l->captured) {
					addr = builder->CreateStructGEP(
						captures.back().second,
						capture_ptrs.front(),
						capture_offsets[l]);
				}
				builder->CreateStore(initializer.data, addr);
				if (l->is_mutable)
					initializer.data = addr;
			} else {
				locals.insert({ l, initializer.data });
			}
			if (di_builder && !l->captured) {
				if (l->is_mutable) {
					insert_di_var(l, locals[l]);
				} else {
					auto addr = builder->CreateAlloca(to_llvm_type(*l->type));
					builder->CreateStore(initializer.data, addr);
					insert_di_var(l, addr);
				}
			}
		}
		for (auto& a : node.body) {
			if (a != node.body.back())
				comp_to_void(a);
		}
		auto r = compile(node.body.back());
		persist_rfield(r);
		auto result_as_temp = get_if<Val::Temp>(&r.lifetime);
		weak<ast::Var> temp_var = result_as_temp ? result_as_temp->var : nullptr;
		auto val_iter = to_dispose.begin();
		for (auto& p : node.names) {
			if (temp_var == p) { // result is locked by the dying temp ptr.
				if (!get_if<Val::Retained>(&val_iter->lifetime))
					build_retain(r.data, p->type);
				r.type = temp_var->type;  // revert own->pin cohersion
				r.lifetime = Val::Retained{};
			} else if (is_ptr(p->type)) {
				if (p->is_mutable) {
					build_release(builder->CreateLoad(to_llvm_type(*p->type), val_iter->data), p->type);
				} else {
					dispose_val(move(*val_iter));
				}
			}
			val_iter++;
		}
		current_di_scope = prev_di_scope;
		return r;
	}

	void on_block(ast::Block& node) override {
		*result = handle_block(node, Val{});
	}
	void on_make_delegate(ast::MakeDelegate& node) override {
		auto base = compile(node.base);
		auto method = node.method->base.pinned();
		auto m_ordinal = methods[method].ordinal;
		auto disp = builder->CreateLoad(ptr_type, builder->CreateConstGEP2_32(obj_struct, base.data, AG_HEADER_OFFSET, 0));
		result->data = builder->CreateInsertValue(
			builder->CreateInsertValue(
				llvm::UndefValue::get(delegate_struct),
				builder->CreateCall(fn_mk_weak, { base.data }),
				{ 0 }),
			method->cls->is_interface
				? (llvm::Value*) builder->CreateCall(
					llvm::FunctionCallee(dispatcher_fn_type, disp),
					{ builder->getInt64(classes[method->cls].interface_ordinal | m_ordinal) })
				: (llvm::Value*) builder->CreateLoad(
					ptr_type,
					builder->CreateConstGEP2_32(classes[method->cls].vmt, disp, -1, m_ordinal)),
			{ 1 });
		result->lifetime = Val::Retained{};
		dispose_val(move(base));
	}

	void on_immediate_delegate(ast::ImmediateDelegate& node) override {
		auto dl_fn = llvm::Function::Create(
			lambda_to_llvm_fn(node, node.type()),
			llvm::Function::InternalLinkage,
			ast::format_str("ag_dl_", node.name.pinned()),
			module.get());
		auto prev_fn = current_function;
		current_function = dl_fn;
		compile_fn_body(node, ast::format_str("dl_", node.name.pinned()));
		current_function = prev_fn;
		auto base = compile(node.base);
		result->data = builder->CreateInsertValue(
			builder->CreateInsertValue(
				llvm::UndefValue::get(delegate_struct),
				builder->CreateCall(fn_mk_weak, { base.data }),
				{ 0 }),
			dl_fn,
			{ 1 });
		result->lifetime = Val::Retained{};
		dispose_val(move(base));
	}

	void on_make_fn_ptr(ast::MakeFnPtr& node) {
		result->data = functions[node.fn];
	}

	void on_call(ast::Call& node) override {
		vector<llvm::Value*> params;
		vector<Val> to_dispose;
		if (auto calle_as_method = dom::strict_cast<ast::MakeDelegate>(node.callee)) {
			params.push_back(nullptr);
			auto method = calle_as_method->method->base.pinned();
			auto& m_info = methods[method];
			auto pt = m_info.type->params().begin() + 1;
			for (auto& p : node.params) {
				to_dispose.push_back(comp_to_persistent(p));
				params.push_back(cast_to(to_dispose.back().data, *pt++));
			}
			to_dispose.push_back(comp_to_persistent(
				calle_as_method->base,
				false ));  // perist_mutable_locals
			auto receiver = to_dispose.back().data;
			params.front() = cast_to(receiver, ptr_type);
			if (method->cls->is_interface) {
				auto entry_point = builder->CreateCall(
					llvm::FunctionCallee(
						dispatcher_fn_type,
						builder->CreateLoad(ptr_type, builder->CreateConstGEP2_32(obj_struct, receiver, AG_HEADER_OFFSET, 0))
					),
					{ builder->getInt64(classes[method->cls].interface_ordinal | m_info.ordinal) });
				result->data = builder->CreateCall(
					llvm::FunctionCallee(m_info.type, entry_point),
					move(params));
			} else {
				result->data = builder->CreateCall(
					llvm::FunctionCallee(
						m_info.type,
						builder->CreateLoad(  // load ptr to fn
							ptr_type,
							builder->CreateConstGEP2_32(
								classes[method->cls].vmt,
								builder->CreateLoad(ptr_type, builder->CreateConstGEP2_32(obj_struct, receiver, AG_HEADER_OFFSET, 0)),
								-1,
								m_info.ordinal))),
					move(params));
			}
		} else if (auto as_delegate_type = dom::strict_cast<ast::TpDelegate>(node.callee->type())) {
			auto result_type = dom::strict_cast<ast::TpOptional>(node.type());
			to_dispose.push_back(compile(node.callee));
			llvm::Value* retained_receiver_pin = builder->CreateCall(fn_deref_weak,
				{ builder->CreateExtractValue(to_dispose.front().data, {0}) });
			params.push_back(cast_to(retained_receiver_pin, ptr_type)); 
			auto pt = as_delegate_type->params.begin();
			for (auto& p : node.params) {
				to_dispose.push_back(comp_to_persistent(p));
				params.push_back(cast_to(to_dispose.back().data, to_llvm_type(**(pt++))));
			}
			*result = compile_if(
				*result_type,
				builder->CreateCmp(llvm::CmpInst::Predicate::ICMP_NE,
					retained_receiver_pin,
					llvm::ConstantInt::get(tp_int_ptr, 0)),
				[&] {
					Val r{
						result->type,
						make_opt_val(
							builder->CreateCall(
								llvm::FunctionCallee(
									lambda_to_llvm_fn(*node.callee, node.callee->type()),
									builder->CreateExtractValue(to_dispose.front().data, {1})),
								move(params)),
							result_type),
						Val::NonPtr{} };
					build_release_ptr_not_null(retained_receiver_pin);
					return move(r);
				},
				[&] { return Val{ result->type, make_opt_none(result_type), Val::NonPtr{} }; });
		} else {
			bool is_fn = isa<ast::TpFunction>(*node.callee->type());
			if (!is_fn)
				params.push_back(nullptr);
			auto function_type = is_fn				
					? function_to_llvm_fn(*node.callee, node.callee->type())
					: lambda_to_llvm_fn(*node.callee, node.callee->type());
			auto pt = function_type->params().begin() + (is_fn ? 0 : 1);
			for (auto& p : node.params) {
				to_dispose.push_back(comp_to_persistent(p));
				params.push_back(cast_to(to_dispose.back().data, *pt++));
			}
			auto callee = compile(node.callee);
			assert(get_if<Val::NonPtr>(&callee.lifetime));
			if (!is_fn)
				params.front() = builder->CreateExtractValue(callee.data, { 0 });
			result->data = builder->CreateCall(
				llvm::FunctionCallee(
					function_type,
					is_fn
						? callee.data
						: builder->CreateExtractValue(callee.data, { 1 })),
				move(params));
		}
		if (is_ptr(node.type()))
			result->lifetime = Val::Retained{};
		for (; !to_dispose.empty(); to_dispose.pop_back())
			dispose_val(move(to_dispose.back()));
	}

	llvm::Value* get_data_ref(const weak<ast::Var>& var) {
		auto it = locals.find(var);
		if (it != locals.end())
			return it->second;
		int ptr_index = 0;
		auto var_depth = var->lexical_depth;
		int d = int(captures.size()) - 1;
		for (;; d--) {
			assert(d >= 0);
			if (captures[d].first == var_depth)
				break;
			if (++ptr_index >= capture_ptrs.size())
				capture_ptrs.push_back(
					builder->CreateLoad(ptr_type,
						builder->CreateStructGEP(captures[d].second, capture_ptrs.back(), { 0 })));
		}
		return locals[var] = builder->CreateStructGEP(
			captures[d].second,
			capture_ptrs[ptr_index],
			capture_offsets[var]);
	}
	void on_get(ast::Get& node) override {
		result->data = remove_indirection(*node.var.pinned(), get_data_ref(node.var));
		if (is_ptr(node.type()))
			result->lifetime = Val::Temp{ node.var };
	}
	void on_set(ast::Set& node) override {
		*result = make_retained_or_non_ptr(compile(node.val));
		result->type = nullptr;
		if (is_ptr(node.var->type)) {
			auto addr = get_data_ref(node.var);
			build_release(builder->CreateLoad(ptr_type, addr), node.var->type);
			builder->CreateStore(result->data, addr);
			result->lifetime = Val::Temp{ node.var };
		} else {
			builder->CreateStore(result->data, get_data_ref(node.var));
		}
	}
	void on_get_field(ast::GetField& node) override {
		auto base = compile(node.base);
		auto class_fields = classes[ast->extract_class(base.type)].fields;
		result->data = builder->CreateLoad(
			class_fields->getElementType(node.field->offset),
			builder->CreateStructGEP(class_fields, base.data, node.field->offset));
		if (is_ptr(node.type())) {
			if (get_if<Val::Retained>(&base.lifetime)) {
				result->lifetime = Val::RField{ base.data };
			} else if (auto as_rfield = get_if<Val::RField>(&base.lifetime)) {
				result->lifetime = Val::RField{ as_rfield->to_release };				
			} else {
				result->lifetime = Val::Temp{};
			}
		} else { // leave lifetime = Val::NonPtr
			dispose_val(move(base));
		}
	}
	void on_set_field(ast::SetField& node) override {
		auto class_fields = classes[ast->extract_class(node.base->type())].fields;
		if (is_ptr(node.type())) {
			auto base = comp_to_persistent(node.base);
			*result = make_retained_or_non_ptr(compile(node.val), base.data);
			auto addr = builder->CreateStructGEP(class_fields, base.data, node.field->offset);
			build_release(builder->CreateLoad(ptr_type, addr), node.type());
			builder->CreateStore(result->data, addr);
			if (get_if<Val::Retained>(&base.lifetime)) {
				result->lifetime = Val::RField{ base.data };
			} else if (auto base_as_rfield = get_if<Val::RField>(&base.lifetime)) {
				result->lifetime = Val::RField{ base_as_rfield->to_release };
			} else {
				result->lifetime = Val::Temp{};
				dispose_val(move(base));
			}
		} else {
			*result = make_retained_or_non_ptr(compile(node.val));
			auto base = compile(node.base);
			builder->CreateStore(
				result->data,
				builder->CreateStructGEP(
					class_fields,
					base.data,
					node.field->offset));
			dispose_val(move(base));
		}
	}
	void on_mk_instance(ast::MkInstance& node) override {
		result->data = builder->CreateCall(classes[node.cls].constructor, {});
		result->lifetime = Val::Retained{};
	}
	void on_to_int(ast::ToIntOp& node) override {
		result->data = builder->CreateFPToSI(comp_non_ptr(node.p), int_type);
	}
	void on_to_float(ast::ToFloatOp& node) override {
		result->data = builder->CreateSIToFP(comp_non_ptr(node.p), double_type);
	}
	void on_not(ast::NotOp& node) override {
		result->data = builder->CreateNot(
			check_opt_has_val(
				comp_non_ptr(node.p),
				dom::strict_cast<ast::TpOptional>(node.p->type())));
	}
	void on_neg(ast::NegOp& node) override {
		result->data = builder->CreateNeg(comp_non_ptr(node.p));
	}
	void on_ref(ast::RefOp& node) override {
		internal_error(node, "ref cannot be compiled");
	}
	void on_cast(ast::CastOp& node) override {
		if (!node.p[1]) {
			*result = compile(node.p[0]);
			result->data = cast_to(result->data, to_llvm_type(*node.type()));
			result->type = nullptr;
			return;
		}
		auto result_type = dom::strict_cast<ast::TpOptional>(node.type());
		assert(result_type);
		auto cls = ast->extract_class(result_type->wrapped);
		assert(cls); 
		*result = compile(node.p[0]);
		auto& cls_info = classes[cls];
		if (cls->is_interface) {
			auto interface_ordinal = builder->getInt64(cls_info.interface_ordinal);
			auto id = builder->CreateCall(
				llvm::FunctionCallee(
					dispatcher_fn_type,
					builder->CreateLoad(ptr_type, builder->CreateConstGEP2_32(obj_struct, result->data, AG_HEADER_OFFSET, 0))
				),
				{ interface_ordinal });
			*result = compile_if(
				*result_type,
				builder->CreateCmp(llvm::CmpInst::Predicate::ICMP_EQ,
					builder->CreateBitOrPointerCast(interface_ordinal, ptr_type),
					id),
				[&] { return Val{ result->type, make_opt_val(result->data, result_type), result->lifetime }; },
				[&] { return Val{ result->type, make_opt_none(result_type), Val::NonPtr{} }; });
			return;
		}
		auto vmt_ptr = builder->CreateLoad(ptr_type, builder->CreateConstGEP2_32(obj_struct, result->data, AG_HEADER_OFFSET, 0));
		auto vmt_ptr_bb = builder->GetInsertBlock();
		*result = compile_if(
			*result_type,
			builder->CreateCmp(llvm::CmpInst::Predicate::ICMP_ULE,
				builder->getInt64(classes[cls].vmt_size),
				builder->CreateLoad(tp_int_ptr,
					builder->CreateConstGEP2_32(obj_vmt_struct, vmt_ptr, -1, 3))),
			[&] {
				return compile_if(
					*result_type,
					builder->CreateCmp(llvm::CmpInst::Predicate::ICMP_EQ,
						cls_info.dispatcher,
						builder->CreateLoad(ptr_type,
							builder->CreateConstGEP2_32(cls_info.vmt, vmt_ptr, -1, 0))),
					[&] { return Val{ result->type, make_opt_val(result->data, result_type), result->lifetime }; },
					[&] { return Val{ result->type, make_opt_none(result_type), Val::NonPtr{} }; });
			},
			[&] { return Val{ result_type, make_opt_none(result_type), Val::NonPtr{} }; });
	}
	void on_add(ast::AddOp& node) override {
		auto lhs = comp_non_ptr(node.p[0]);
		auto rhs = comp_non_ptr(node.p[1]);
		result->data = node.type_ == ast->tp_int64()
			? builder->CreateAdd(lhs, rhs)
			: builder->CreateFAdd(lhs, rhs);
	}
	void on_lt(ast::LtOp& node) override {
		auto lhs = comp_non_ptr(node.p[0]);
		auto rhs = comp_non_ptr(node.p[1]);
		result->data = node.p[0]->type() == ast->tp_int64()
			? builder->CreateCmp(llvm::CmpInst::Predicate::ICMP_SLT, lhs, rhs)
			: builder->CreateCmp(llvm::CmpInst::Predicate::FCMP_OLT, lhs, rhs);
	}
	void on_eq(ast::EqOp& node) override {
		struct Comparer : ast::TypeMatcher {
			llvm::Value* lhs;
			llvm::Value* rhs;
			Generator& gen;
			void compare_scalar() {
				gen.result->data = gen.builder->CreateCmp(
					llvm::CmpInst::Predicate::ICMP_EQ,
					gen.cast_to(lhs, rhs->getType()),
					rhs);
			}
			void compare_pair() {
				auto cond1 = gen.builder->CreateCmp(
					llvm::CmpInst::Predicate::ICMP_EQ,
					gen.builder->CreateExtractValue(lhs, { 0 }),
					gen.builder->CreateExtractValue(rhs, { 0 }));
				auto on_eq = llvm::BasicBlock::Create(*gen.context, "", gen.current_function);
				auto on_ne = llvm::BasicBlock::Create(*gen.context, "", gen.current_function);
				auto prev_bb = gen.builder->GetInsertBlock();
				gen.builder->CreateCondBr(cond1, on_eq, on_ne);
				gen.builder->SetInsertPoint(on_eq);
				auto cond2 = gen.builder->CreateCmp(
					llvm::CmpInst::Predicate::ICMP_EQ,
					gen.builder->CreateExtractValue(lhs, { 1 }),
					gen.builder->CreateExtractValue(rhs, { 1 }));
				gen.builder->CreateBr(on_ne);
				gen.builder->SetInsertPoint(on_ne);
				auto phi = gen.builder->CreatePHI(gen.tp_bool, 2);
				phi->addIncoming(cond1, prev_bb);
				phi->addIncoming(cond2, on_eq);
				gen.result->data = phi;
			}
			Comparer(llvm::Value* lhs, llvm::Value* rhs, Generator& gen) : lhs(lhs), rhs(rhs), gen(gen) {}
			void on_int64(ast::TpInt64& type) override { compare_scalar(); }
			void on_double(ast::TpDouble& type) override { gen.result->data = gen.builder->CreateCmp(llvm::CmpInst::Predicate::FCMP_OEQ, lhs, rhs); }
			void on_function(ast::TpFunction& type) override { compare_scalar(); }
			void on_lambda(ast::TpLambda& type) override { compare_pair(); }
			void on_delegate(ast::TpDelegate& type) override { compare_pair(); }
			void on_cold_lambda(ast::TpColdLambda& type) override {}
			void on_void(ast::TpVoid& type) override { gen.result->data = gen.builder->getInt1(0); }
			void on_optional(ast::TpOptional& type) override {
				struct OptComparer : ast::TypeMatcher {
					Comparer& c;
					OptComparer(Comparer& c) : c(c) {}
					void on_int64(ast::TpInt64& type) override { c.compare_pair(); }
					void on_double(ast::TpDouble& type) override { c.compare_scalar(); }
					void on_function(ast::TpFunction& type) override { c.compare_scalar(); }
					void on_lambda(ast::TpLambda& type) override { c.compare_pair(); }
					void on_delegate(ast::TpDelegate& type) override { c.compare_pair(); }
					void on_cold_lambda(ast::TpColdLambda& type) override { c.compare_scalar(); }
					void on_void(ast::TpVoid& type) override { c.compare_scalar(); }
					void on_optional(ast::TpOptional& type) override { assert(false); }
					void on_class(ast::TpClass& type) override { c.compare_scalar(); }
					void on_ref(ast::TpRef& type) override { c.compare_scalar(); }
					void on_weak(ast::TpWeak& type) override { c.compare_scalar(); }
				};
				OptComparer opt_comparer{ *this };
				type.wrapped->match(opt_comparer);
			}
			void on_class(ast::TpClass& type) override { compare_scalar(); }
			void on_ref(ast::TpRef& type) override { compare_scalar(); }
			void on_weak(ast::TpWeak& type) override { compare_scalar(); }
		};
		auto lhs = compile(node.p[0]);
		auto rhs = compile(node.p[1]);
		Comparer comparer{ lhs.data, rhs.data, *this };
		node.p[0]->type()->match(comparer);

	}
	void on_sub(ast::SubOp& node) override {
		auto lhs = comp_non_ptr(node.p[0]);
		auto rhs = comp_non_ptr(node.p[1]);
		result->data = node.type_ == ast->tp_int64()
			? builder->CreateSub(lhs, rhs)
			: builder->CreateFSub(lhs, rhs);
	}
	void on_mul(ast::MulOp& node) override {
		auto lhs = comp_non_ptr(node.p[0]);
		auto rhs = comp_non_ptr(node.p[1]);
		result->data = node.type_ == ast->tp_int64()
			? builder->CreateMul(lhs, rhs)
			: builder->CreateFMul(lhs, rhs);
	}
	void on_div(ast::DivOp& node) override {
		auto lhs = comp_non_ptr(node.p[0]);
		auto rhs = comp_non_ptr(node.p[1]);
		result->data = node.type_ == ast->tp_int64()
			? builder->CreateSDiv(lhs, rhs)
			: builder->CreateFDiv(lhs, rhs);
	}
	void on_mod(ast::ModOp& node) override {
		auto lhs = comp_non_ptr(node.p[0]);
		auto rhs = comp_non_ptr(node.p[1]);
		result->data = builder->CreateSRem(lhs, rhs);
	}
	void on_shl(ast::ShlOp& node) override {
		auto lhs = comp_non_ptr(node.p[0]);
		auto rhs = comp_non_ptr(node.p[1]);
		result->data = builder->CreateShl(lhs, rhs);
	}
	void on_shr(ast::ShrOp& node) override {
		auto lhs = comp_non_ptr(node.p[0]);
		auto rhs = comp_non_ptr(node.p[1]);
		result->data = builder->CreateAShr(lhs, rhs);
	}
	void on_and(ast::AndOp& node) override {
		auto lhs = comp_non_ptr(node.p[0]);
		auto rhs = comp_non_ptr(node.p[1]);
		result->data = builder->CreateAnd(lhs, rhs);
	}
	void on_or(ast::OrOp& node) override {
		auto lhs = comp_non_ptr(node.p[0]);
		auto rhs = comp_non_ptr(node.p[1]);
		result->data = builder->CreateOr(lhs, rhs);
	}
	void on_xor(ast::XorOp& node) override {
		auto lhs = comp_non_ptr(node.p[0]);
		auto rhs = comp_non_ptr(node.p[1]);
		result->data = builder->CreateXor(lhs, rhs);
	}

	// TODO: add branch-aware compiler/action_scanner
	// that accepts: on_some, on_none blocks and avoids optionals
	llvm::Value* make_opt_val(llvm::Value* val, pin<ast::TpOptional> type) {
		struct ValMaker : ast::TypeMatcher {
			llvm::Value* val;
			Generator* gen;
			int depth;
			ValMaker(llvm::Value* val, Generator* gen, int depth) : val(val), gen(gen), depth(depth) {}
			void pack_to_int_ptr_pair() {
				val = depth > 0
					? val
					: gen->builder->CreateInsertValue(
						gen->builder->CreateInsertValue(
							llvm::UndefValue::get(gen->tp_opt_lambda),
							gen->builder->CreateBitCast(gen->builder->CreateExtractValue(val, { 0 }), gen->tp_int_ptr),
							{ 0 }),
						gen->builder->CreateBitCast(gen->builder->CreateExtractValue(val, { 1 }), gen->tp_int_ptr),
						{ 1 });
			}
			void on_int64(ast::TpInt64& type) override {
				val = depth > 0
					? val
					: gen->builder->CreateInsertValue(
						gen->builder->CreateInsertValue(
							llvm::UndefValue::get(gen->tp_opt_int),
							gen->builder->getInt8(0),
							{ 0 }),
						val,
						{ 1 });
			}
			void on_double(ast::TpDouble& type) override {
				val = depth > 0
					? val 
					: gen->builder->CreateBitCast(val, gen->tp_opt_double);
			}
			void on_function(ast::TpFunction& type) override {
				val = depth > 0
					? val
					: gen->builder->CreatePtrToInt(val, gen->tp_int_ptr);
			}
			void on_lambda(ast::TpLambda& type) override { pack_to_int_ptr_pair(); }
			void on_delegate(ast::TpDelegate& type) override { pack_to_int_ptr_pair(); }
			void on_cold_lambda(ast::TpColdLambda& type) override {
				val = depth > 0
					? val
					: gen->builder->getInt1(true);
			}
			void on_void(ast::TpVoid& type) override {
				val = depth > 1 ? val
					: depth == 0 ? gen->builder->getInt1(true)
					: gen->builder->getInt8(1);
			}
			void on_optional(ast::TpOptional& type) override { assert(false); }
			void on_class(ast::TpClass& type) override {
				val = depth > 0
					? val
					: gen->builder->CreatePtrToInt(val, gen->tp_int_ptr);
			}
			void on_ref(ast::TpRef& type) override {
				val = depth > 0
					? val
					: gen->builder->CreatePtrToInt(val, gen->tp_int_ptr);
			}
			void on_weak(ast::TpWeak& type) override {
				val = depth > 0
					? val
					: gen->builder->CreatePtrToInt(val, gen->tp_int_ptr);
			}
		};
		ValMaker val_maker(val, this, type->depth);
		type->wrapped->match(val_maker);
		return val_maker.val;
	}

	llvm::Value* make_opt_none(pin<ast::TpOptional> type) {
		struct NoneMaker : ast::TypeMatcher {
			llvm::Value* val = nullptr;
			Generator* gen;
			int depth;
			NoneMaker(Generator* gen, int depth) : gen(gen), depth(depth) {}
			void make_int_ptr_pair() {
				val = gen->builder->CreateInsertValue(
					gen->builder->CreateInsertValue(
						llvm::UndefValue::get(gen->tp_opt_lambda),
						llvm::ConstantInt::get(gen->tp_int_ptr, depth),
						{ 0 }),
					llvm::ConstantInt::get(gen->tp_int_ptr, 0),
					{ 1 });
			}
			void on_int64(ast::TpInt64& type) override {
				val = gen->builder->CreateInsertValue(
					gen->builder->CreateInsertValue(
						llvm::UndefValue::get(gen->tp_opt_int),
						gen->builder->getInt8(depth + 1),
						{ 0 }),
					gen->builder->getInt64(0),
					{ 1 });
			}
			void on_double(ast::TpDouble& type) override { val = llvm::ConstantInt::get(gen->tp_opt_double, depth); }
			void on_function(ast::TpFunction& type) override { val = llvm::ConstantInt::get(gen->tp_int_ptr, depth); }
			void on_lambda(ast::TpLambda& type) override { make_int_ptr_pair(); }
			void on_delegate(ast::TpDelegate& type) override { make_int_ptr_pair(); }
			void on_cold_lambda(ast::TpColdLambda& type) override { val = gen->builder->getInt8(depth ? depth + 1 : 0); }
			void on_void(ast::TpVoid& type) override { val = depth == 0 ? gen->builder->getInt1(false) : gen->builder->getInt8(depth + 1); }
			void on_optional(ast::TpOptional& type) override { assert(false); }
			void on_class(ast::TpClass& type) override { val = llvm::ConstantInt::get(gen->tp_int_ptr, depth); }
			void on_ref(ast::TpRef& type) override { val = llvm::ConstantInt::get(gen->tp_int_ptr, depth); }
			void on_weak(ast::TpWeak& type) override { val = llvm::ConstantInt::get(gen->tp_int_ptr, depth); }
		};
		NoneMaker none_maker(this, type->depth);
		type->wrapped->match(none_maker);
		return none_maker.val;
	}

	llvm::Value* check_opt_has_val(llvm::Value* val, pin<ast::TpOptional> type) {
		struct OptChecker : ast::TypeMatcher {
			llvm::Value* val;
			Generator* gen;
			int depth;
			OptChecker(llvm::Value* val, Generator* gen, int depth) : val(val), gen(gen), depth(depth) {}
			void on_int64(ast::TpInt64& type) override {
				val = gen->builder->CreateICmpNE(
					gen->builder->CreateExtractValue(val, { 0 }),
					gen->builder->getInt8(depth + 1));
			}
			void on_double(ast::TpDouble& type) override {
				val = gen->builder->CreateICmpNE(
					val,
					llvm::ConstantInt::get(gen->tp_opt_double, depth));
			}
			void on_function(ast::TpFunction& type) override {
				val = gen->builder->CreateICmpNE(
					val,
					llvm::ConstantInt::get(gen->tp_int_ptr, depth));
			}
			void on_lambda(ast::TpLambda& type) override {
				val = gen->builder->CreateICmpNE(
					gen->builder->CreateExtractValue(val, { 0 }),
					llvm::ConstantInt::get(gen->tp_int_ptr, depth));
			}
			void on_delegate(ast::TpDelegate& type) override {
				val = gen->builder->CreateICmpNE(
					gen->builder->CreateExtractValue(val, { 0 }),
					llvm::ConstantInt::get(gen->tp_int_ptr, depth));
			}
			void on_cold_lambda(ast::TpColdLambda& type) override {
				val = gen->builder->CreateICmpNE(
					val,
					gen->builder->getInt8(depth ? depth + 1 : 0));
			}
			void on_void(ast::TpVoid& type) override {
				val = depth == 0
					? val
					: gen->builder->CreateICmpNE(
						val,
						gen->builder->getInt8(depth + 1));
			}
			void on_optional(ast::TpOptional& type) override { assert(false); }
			void on_class(ast::TpClass& type) override {
				val = gen->builder->CreateICmpNE(
					val,
					llvm::ConstantInt::get(gen->tp_int_ptr, depth));
			}
			void on_ref(ast::TpRef& type) override {
				val = gen->builder->CreateICmpNE(
					val,
					llvm::ConstantInt::get(gen->tp_int_ptr, depth));
			}
			void on_weak(ast::TpWeak& type) override {
				val = gen->builder->CreateICmpNE(
					val,
					llvm::ConstantInt::get(gen->tp_int_ptr, depth));
			}
		};
		OptChecker checker(val, this, type->depth);
		type->wrapped->match(checker);
		return checker.val;
	}

	llvm::Value* extract_opt_val(llvm::Value* val, pin<ast::TpOptional> type) {
		struct ValMaker : ast::TypeMatcher {
			llvm::Value* val;
			Generator* gen;
			int depth;
			ValMaker(llvm::Value* val, Generator* gen, int depth) : val(val), gen(gen), depth(depth) {}
			void handle_ptr_pair_struct(llvm::StructType* struct_tp) {
				if (depth == 0) {
					val = gen->builder->CreateInsertValue(
						gen->builder->CreateInsertValue(
							llvm::UndefValue::get(struct_tp),
							gen->builder->CreateBitOrPointerCast(gen->builder->CreateExtractValue(val, { 0 }), gen->ptr_type),
							{ 0 }),
						gen->builder->CreateBitOrPointerCast(gen->builder->CreateExtractValue(val, { 1 }), gen->ptr_type),
						{ 1 });
				}
			}
			void on_int64(ast::TpInt64& type) override {
				val = depth > 0
					? val
					: gen->builder->CreateExtractValue(val, { 1 });
			}
			void on_double(ast::TpDouble& type) override {
				val = depth > 0
					? val
					: gen->builder->CreateBitCast(val, gen->double_type);
			}
			void on_function(ast::TpFunction& type) override {
				val = depth > 0
					? val
					: gen->builder->CreateBitOrPointerCast(val, gen->to_llvm_type(type));
			}
			void on_lambda(ast::TpLambda& type) override { handle_ptr_pair_struct(gen->lambda_struct); }
			void on_delegate(ast::TpDelegate& type) override { handle_ptr_pair_struct(gen->delegate_struct); }
			void on_cold_lambda(ast::TpColdLambda& type) override {
				val = depth > 0
					? val
					: llvm::UndefValue::get(gen->void_type);
			}
			void on_void(ast::TpVoid& type) override {
				val = depth > 1 ? val
					: depth == 1 ? gen->cast_to(val, gen->tp_bool)
					: gen->builder->getInt8(1);
			}
			void on_optional(ast::TpOptional& type) override { assert(false); }
			void on_class(ast::TpClass& type) override {
				val = depth > 0
					? val
					: gen->builder->CreateBitOrPointerCast(val, gen->to_llvm_type(type));
			}
			void on_ref(ast::TpRef& type) override {
				val = depth > 0
					? val
					: gen->builder->CreateBitOrPointerCast(val, gen->to_llvm_type(type));
			}
			void on_weak(ast::TpWeak& type) override {
				val = depth > 0
					? val
					: gen->builder->CreateBitOrPointerCast(val, gen->to_llvm_type(type));
			}
		};
		ValMaker val_maker(val, this, type->depth);
		type->wrapped->match(val_maker);
		return val_maker.val;
	}

	Val compile_if(
		ast::Type& type,
		llvm::Value* cond,
		const std::function<Val ()>& then_action,
		const std::function<Val ()>& else_action)
	{
		auto then_bb = llvm::BasicBlock::Create(*context, "", current_function);
		auto else_bb = llvm::BasicBlock::Create(*context, "", current_function);
		auto joined_bb = llvm::BasicBlock::Create(*context, "", current_function);
		builder->CreateCondBr(cond, then_bb, else_bb);
		builder->SetInsertPoint(then_bb);
		auto then_val = then_action();
		then_bb = builder->GetInsertBlock();
		builder->SetInsertPoint(else_bb);
		auto else_val = else_action();
		else_bb = builder->GetInsertBlock();
		Val result;
		// any lifetime | others lifetime | result lifetime | action
		// non_ptr         *                use others        none
		// retained        *                retained          make other retained
		// rfield	- cant be because other can assign. so both branches must be persistent
		// temp{anything}  *				temp{0}           none
		if (get_if<Val::NonPtr>(&then_val.lifetime)) {
			result.lifetime = else_val.lifetime;
		} else if (get_if<Val::NonPtr>(&else_val.lifetime)) {
			result.lifetime = then_val.lifetime;
		} else if (get_if<Val::Retained>(&then_val.lifetime)) {
			else_val = make_retained_or_non_ptr(move(else_val));
			result.lifetime = Val::Retained{};
		} else if (get_if<Val::Retained>(&else_val.lifetime)) {
			builder->SetInsertPoint(then_bb);
			then_val = make_retained_or_non_ptr(move(then_val));
			builder->SetInsertPoint(else_bb);
			result.lifetime = Val::Retained{};
		} else {
			assert(get_if<Val::Temp>(&then_val.lifetime) && get_if<Val::Temp>(&else_val.lifetime));
			result.lifetime = Val::Temp{ nullptr };
		}
		builder->CreateBr(joined_bb);
		builder->SetInsertPoint(then_bb);
		builder->CreateBr(joined_bb);
		builder->SetInsertPoint(joined_bb);
		if (!dom::isa<ast::TpVoid>(type)) {
			auto phi = builder->CreatePHI(to_llvm_type(type), 2);
			phi->addIncoming(then_val.data, then_bb);
			phi->addIncoming(else_val.data, else_bb);
			result.data = phi;
		}
		return result;
	}

	void on_if(ast::If& node) override {
		auto node_type = dom::strict_cast<ast::TpOptional>(node.type());
		if (auto as_bool_const = dom::strict_cast<ast::ConstBool>(node.p[0])) {
			if (as_bool_const->value) {
				*result = compile(node.p[1]);
				result->data = make_opt_val(result->data, node_type);
				result->type = ast->tp_optional(result->type);
			} else {
				result->data = make_opt_none(node_type);
				// marked as not_pointer == static lifetime
			}
		} else {
			auto cond_type = dom::strict_cast<ast::TpOptional>(node.p[0]->type());
			auto cond_opt_val = compile(node.p[0]);
			*result = compile_if(
				*node_type,
				check_opt_has_val(cond_opt_val.data, cond_type),
				[&] {
					auto as_block = dom::strict_cast<ast::Block>(node.p[1]);
					auto val = as_block && !as_block->names.empty() && !as_block->names.front()->initializer
						? handle_block(*as_block, Val{ as_block->names.front()->type, extract_opt_val(cond_opt_val.data, cond_type), cond_opt_val.lifetime })
						: compile(node.p[1]);
					return Val{ node_type, make_opt_val(val.data, node_type), val.lifetime };
				},
				[&] {
					return Val{ node_type, make_opt_none(node_type), Val::NonPtr{} };
				});
		}
	}
	void on_land(ast::LAnd& node) override {
		auto node_type = dom::strict_cast<ast::TpOptional>(node.type());
		auto cond_type = dom::strict_cast<ast::TpOptional>(node.p[0]->type());
		auto cond_opt_val = compile(node.p[0]);
		*result = compile_if(
			*node_type,
			check_opt_has_val(cond_opt_val.data, cond_type),
			[&] {
				auto as_block = dom::strict_cast<ast::Block>(node.p[1]);
				return as_block && !as_block->names.empty() && !as_block->names.front()->initializer
					? handle_block(*as_block, Val{ as_block->names.front()->type, extract_opt_val(cond_opt_val.data, cond_type), cond_opt_val.lifetime })
					: compile(node.p[1]);
			},
			[&] {
				return Val{ node_type, make_opt_none(node_type), Val::NonPtr{} };
			});
	}
	void on_else(ast::Else& node) override {
		auto cond_type = dom::strict_cast<ast::TpOptional>(node.p[0]->type());
		auto cond_opt = compile(node.p[0]);
		*result = compile_if(
			*node.type(),
			check_opt_has_val(cond_opt.data, cond_type),
			[&] {
				return Val{ node.p[1]->type(), extract_opt_val(cond_opt.data, cond_type), cond_opt.lifetime };
			}, [&] {
				return compile(node.p[1]);
			});
	}
	void on_lor(ast::LOr& node) override {
		auto cond_type = dom::strict_cast<ast::TpOptional>(node.p[0]->type());
		auto cond_opt = compile(node.p[0]);
		*result = compile_if(
			*node.type(),
			check_opt_has_val(cond_opt.data, cond_type),
			[&] { return cond_opt; },
			[&] { return compile(node.p[1]); });
	}
	void on_loop(ast::Loop& node) override {
		auto loop_body = llvm::BasicBlock::Create(*context, "", current_function);
		auto after_loop = llvm::BasicBlock::Create(*context, "", current_function);
		builder->CreateBr(loop_body);
		builder->SetInsertPoint(loop_body);
		*result = compile(node.p);
		auto r_type = dom::strict_cast<ast::TpOptional>(node.p->type());
		builder->CreateCondBr(
			check_opt_has_val(result->data, r_type),
			after_loop,
			loop_body);
		builder->SetInsertPoint(after_loop);
		result->data = extract_opt_val(result->data, r_type);
		result->type = node.type();
	}
	void on_copy(ast::CopyOp& node) override {
		auto src = compile(node.p);
		result->data = builder->CreateCall(fn_copy, { src.data });
		result->lifetime = Val::Retained{};
		dispose_val(move(src));
	}
	void on_mk_weak(ast::MkWeakOp& node) override {
		if (dom::strict_cast<ast::MkInstance>(node.p)) {
			result->data = null_weak;
			return;
		}
		auto src = compile(node.p);
		result->data = builder->CreateCall(fn_mk_weak, { src.data });
		result->lifetime = Val::Retained{};
		dispose_val(move(src));
	}
	void on_deref_weak(ast::DerefWeakOp& node) override {
		auto src = compile(node.p);
		result->data = builder->CreateCall(fn_deref_weak, { src.data });
		result->lifetime = Val::Retained{};
		dispose_val(move(src));
	}

	llvm::FunctionType* lambda_to_llvm_fn(ast::Node& n, pin<ast::Type> tp) {  // also delegate and method
		if (isa<ast::TpLambda>(*tp) || isa<ast::TpDelegate>(*tp)) {
			pin<ast::TpLambda> as_lambda = tp.cast<ast::TpLambda>();
			auto& fn = lambda_fns[as_lambda];
			if (!fn) {
				vector<llvm::Type*> params{ ptr_type }; // closure struct or this
				for (size_t i = 0; i < as_lambda->params.size() - 1; i++)
					params.push_back(to_llvm_type(*as_lambda->params[i]));
				fn = llvm::FunctionType::get(to_llvm_type(*as_lambda->params.back()), move(params), false);
			}
			return fn;
		}
		internal_error(n, "type is not a lambda");
	}
	llvm::FunctionType* function_to_llvm_fn(ast::Node& n, pin<ast::Type> tp) {
		if (auto as_func = dom::strict_cast<ast::TpFunction>(tp)) {
			auto& fn = function_types[as_func];
			if (!fn) {
				vector<llvm::Type*> params;
				for (size_t i = 0; i < as_func->params.size() - 1; i++)
					params.push_back(to_llvm_type(*as_func->params[i]));
				fn = llvm::FunctionType::get(to_llvm_type(*as_func->params.back()), move(params), false);
			}
			return fn;
		}
		internal_error(n, "type is not a function");
	}

	llvm::Type* to_llvm_type(ast::Type& t) {
		struct Matcher :ast::TypeMatcher {
			Generator* gen;
			llvm::Type* result = nullptr;
			Matcher(Generator* gen) :gen(gen) {}

			void on_int64(ast::TpInt64& type) override { result = gen->int_type; }
			void on_double(ast::TpDouble& type) override { result = gen->double_type; }
			void on_function(ast::TpFunction& type) override { result = gen->ptr_type; }
			void on_lambda(ast::TpLambda& type) override { result = gen->lambda_struct; }
			void on_delegate(ast::TpDelegate& type) override { result = gen->delegate_struct; }
			void on_cold_lambda(ast::TpColdLambda& type) override { type.resolved->match(*this); }
			void on_void(ast::TpVoid&) override { result = gen->void_type; }
			void on_optional(ast::TpOptional& type) {
				struct OptionalMatcher :ast::TypeMatcher {
					Generator* gen;
					int depth;
					llvm::Type* result = nullptr;
					OptionalMatcher(Generator* gen, int depth) :gen(gen), depth(depth) {}

					void on_int64(ast::TpInt64& type) override { result = gen->tp_opt_int; }
					void on_double(ast::TpDouble& type) override { result = gen->tp_opt_double; }
					void on_function(ast::TpFunction& type) override { result = gen->tp_int_ptr; }
					void on_lambda(ast::TpLambda& type) override { result = gen->tp_opt_lambda; }
					void on_delegate(ast::TpDelegate& type) override { result = gen->tp_opt_delegate; }
					void on_cold_lambda(ast::TpColdLambda& type) override { result = gen->tp_opt_bool; }
					void on_void(ast::TpVoid&) override { result = depth == 0 ? gen->tp_bool : gen->tp_opt_bool; }
					void on_optional(ast::TpOptional& type) { assert(false); };
					void on_class(ast::TpClass& type) override { result = gen->tp_int_ptr; }
					void on_ref(ast::TpRef& type) override { result = gen->tp_int_ptr; }
					void on_weak(ast::TpWeak& type) override { result = gen->tp_int_ptr; }
				};
				OptionalMatcher matcher(gen, type.depth);
				type.wrapped->match(matcher);
				result = matcher.result;
			}
			void on_class(ast::TpClass& type) override { result = gen->ptr_type; }
			void on_ref(ast::TpRef& type) override { result = gen->ptr_type; }
			void on_weak(ast::TpWeak& type) override { result = gen->ptr_type; }
		} matcher(this);
		t.match(matcher);
		return matcher.result;
	}

	llvm::Value* build_i_table(
		string prefix_name,
		llvm::IRBuilder<>& builder,
		unordered_map<uint64_t, llvm::Constant*> vmts,
		llvm::Value* interface_and_method)
	{
		if (vmts.size() < 2) {
			return cast_to(
				vmts.empty()
					? empty_mtable
					: vmts.begin()->second,
				ptr_type);
		}
		auto best = vmt_util::find_best_fit(vmts);
		bool has_splinter = best.width - best.pos == best.splinter;
		llvm::Value* current_interface_index = interface_and_method;
		if (best.pos != 63 || has_splinter)
			current_interface_index = builder.CreateAnd(
				current_interface_index,
				builder.getInt64(
					1ull << best.splinter |
					(~0ull >> (64 - best.width)) << best.pos));
		if (has_splinter)
			current_interface_index = builder.CreateAdd(
				current_interface_index,
				builder.getInt64(
					(~0ull >> (64 - (best.pos + best.width - best.splinter)) << best.splinter)));
		current_interface_index = builder.CreateLShr(current_interface_index, builder.getInt64(best.pos - best.width + 1));
		if (best.spread == vmts.size()) {  // exact match
			vector<llvm::Constant*> i_table(1 << best.width, empty_mtable);
			for (auto& ord : vmts)
				i_table[vmt_util::extract_key_bits(ord.first, best.pos, best.width, best.splinter)] = llvm::ConstantExpr::getBitCast(ord.second, ptr_type);

			auto llvm_itable = make_const_array(prefix_name, move(i_table));
			return builder.CreateLoad(
				ptr_type,
				builder.CreateGEP(
					ptr_type,
					llvm_itable,
					current_interface_index));
		}
		vector<unordered_map<uint64_t, llvm::Constant*>> indirect_table;
		indirect_table.resize(1 << best.width);
		for (auto& ord : vmts)
			indirect_table[vmt_util::extract_key_bits(ord.first, best.pos, best.width, best.splinter)].insert({ ord.first, ord.second });
		vector<llvm::BasicBlock*> dst_table;
		vector<llvm::Constant*> jump_table;
		int i = 0;
		auto combined_block = llvm::BasicBlock::Create(*context, "", builder.GetInsertPoint()->getFunction());
		llvm::IRBuilder<> combined_builder(combined_block);
		auto combined_result = combined_builder.CreatePHI(ptr_type, indirect_table.size());
		for (auto& submap : indirect_table) {
			dst_table.push_back(llvm::BasicBlock::Create(*context, "", builder.GetInsertPoint()->getFunction()));
			llvm::IRBuilder<> b(dst_table.back());
			auto val = build_i_table(ast::format_str(prefix_name, "_", i++), b, submap, interface_and_method);
			b.CreateBr(combined_block);
			combined_result->addIncoming(val, dst_table.back());
			jump_table.push_back(llvm::BlockAddress::get(dst_table.back()));
		}
		size_t jump_table_size = jump_table.size();
		auto llvm_itable = make_const_array(prefix_name, move(jump_table));
		auto br = builder.CreateIndirectBr(
			builder.CreateLoad(
				ptr_type,
				builder.CreateGEP(
					ptr_type,
					llvm_itable,
					{ current_interface_index })),
			jump_table_size);
		for (auto& d : dst_table)
			br->addDestination(d);
		return combined_result;
	}

	llvm::orc::ThreadSafeModule build() {
		std::unordered_set<pin<ast::TpClass>> special_copy_and_dispose = { ast->blob->base_class, ast->blob, ast->own_array, ast->weak_array, ast->string_cls };
		dispatcher_fn_type = llvm::FunctionType::get(ptr_type, { int_type }, false);
		auto dispos_fn_type = llvm::FunctionType::get(void_type, { ptr_type }, false);
		auto copier_fn_type = llvm::FunctionType::get(
			void_type,
			{
				ptr_type, // dst
				ptr_type  // src
			},
			false);  // varargs
		obj_vmt_struct = llvm::StructType::get(
			*context,
			{
				copier_fn_type->getPointerTo(),
				dispos_fn_type->getPointerTo(),
				int_type,  // instance alloc size
				int_type   // obj vmt size (used in casts)
			});
		auto initializer_fn_type = llvm::FunctionType::get(void_type, { ptr_type }, false);
		if (di_builder) {
			for (auto& m : ast->module_names) {
				di_files.insert({
					m.weaked(),
					di_builder->createFile(
							m->name + ".ag",
							ast->absolute_path)});
			}
		}
		// Make LLVM types for classes
		for (auto& cls : ast->classes) {
			auto& info = classes[cls];
			if (cls->is_interface) {
				uint64_t id = 0;
				do
					id = uniform_uint64_distribution(random_generator) << 16;
				while (assigned_interface_ids.count(id) != 0);
				assigned_interface_ids.insert(id);
				info.interface_ordinal = id;
				continue;
			}
			info.fields = llvm::StructType::create(*context, std::to_string(cls->name.pinned()));
			info.constructor = llvm::Function::Create(
				llvm::FunctionType::get(info.fields->getPointerTo(), {}, false),
				llvm::Function::InternalLinkage,
				std::to_string(cls->name.pinned()) + "!ctor", module.get());
			info.initializer = llvm::Function::Create(
				initializer_fn_type,
				llvm::Function::InternalLinkage,
				std::to_string(cls->name.pinned()) + "!init",
				module.get());
		}
		// Make llvm types for methods and fields.
		// Fill llvm structs for classes with fields.
		// Define llvm types for vmts.
		for (auto& cls : ast->classes) {
			auto& info = classes[cls];
			if (!cls->is_interface) {  // handle fields
				vector<llvm::Type*> fields;
				if (cls->base_class) {
					auto& base_fields = classes[cls->base_class].fields->elements();
					for (auto& f : base_fields)
						fields.push_back(f);
				} else {
					fields.push_back(ptr_type);    // disp
					fields.push_back(tp_int_ptr);  // counter
					fields.push_back(tp_int_ptr);  // weak/parent
				}
				for (auto& field : cls->fields) {
					field->offset = fields.size();
					fields.push_back(to_llvm_type(*field->initializer->type()));
				}
				info.fields->setBody(fields);
			}
			vector<llvm::Type*> vmt_content{ dispatcher_fn_type->getPointerTo()};  // interface/class id for casts
			for (auto& m : cls->new_methods) {
				vector<llvm::Type*> params;
				for (auto& p : m->names)
					params.push_back(to_llvm_type(*p->type));
				params[0] = ptr_type;  // this
				auto& m_info = methods[m];
				m_info.type = llvm::FunctionType::get(to_llvm_type(*m->type_expression->type()), move(params), false);
				m_info.ordinal = vmt_content.size();
				vmt_content.push_back(m_info.type->getPointerTo());
			}
			if (cls->base_class) {  // interfaces have no base class
				auto& base_info = classes[cls->base_class];
				size_t base_index = vmt_content.size();
				for (auto& mt : base_info.vmt->elements())
					vmt_content.push_back(mt);
				for (auto& m : cls->overloads[cls->base_class]) {
					auto& m_info = methods[m];
					auto& m_overridden = methods[m->ovr];
					m_info.ordinal = m_overridden.ordinal + base_index;
					m_info.type = m_overridden.type;
				}
			} else if (!cls->is_interface)
				vmt_content.push_back(obj_vmt_struct);
			if (cls->is_interface) {
				info.ivmt = llvm::ArrayType::get(ptr_type, vmt_content.size());
			} else {
				info.vmt = llvm::StructType::get(*context, vmt_content);
				info.vmt_size = layout.getTypeStoreSize(info.vmt);
			}
		}
		// From this point it is possible to build code that access fleds and methods.
		// Make llvm functions for standalone ast functions.
		for (auto& fn : ast->functions) {
			functions.insert({fn, llvm::Function::Create(
				function_to_llvm_fn(*fn, fn->type()),
				fn->is_platform
					? llvm::Function::ExternalLinkage
					: llvm::Function::InternalLinkage,
				ast::format_str("ag_fn_", fn->name.pinned()), module.get())});
		}
		// Build class contents - initializer, dispatcher, disposer, copier, methods.
		for (auto& cls : ast->classes) {
			if (cls->is_interface)
				continue;
			auto& info = classes[cls];
			ClassInfo* base_info = cls->base_class ? &classes[cls->base_class] : nullptr;
			info.dispose = llvm::Function::Create(
				dispos_fn_type,
				special_copy_and_dispose.count(cls) == 0
					? llvm::Function::InternalLinkage
					: llvm::Function::ExternalLinkage,
				ast::format_str("ag_dtor_", cls->name.pinned()), module.get());
			info.dispatcher = llvm::Function::Create(dispatcher_fn_type, llvm::Function::InternalLinkage,
				std::to_string(cls->name.pinned()) + "!disp", module.get());
			// Initializer
			llvm::IRBuilder<> builder(llvm::BasicBlock::Create(*context, "", info.initializer));
			current_function = info.initializer;
			this->builder = &builder;
			if (base_info)
				builder.CreateCall(base_info->initializer, { info.initializer->arg_begin() });
			auto result = builder.CreateBitOrPointerCast(info.initializer->arg_begin(),
				info.fields->getPointerTo());
			for (auto& field : cls->fields) {
				builder.CreateStore(
					make_retained_or_non_ptr(compile(field->initializer)).data,
					builder.CreateStructGEP(info.fields, result, field->offset));
			}
			builder.CreateRetVoid();
			// Constructor
			builder.SetInsertPoint(llvm::BasicBlock::Create(*context, "", info.constructor));
			current_function = info.constructor;
			result = builder.CreateCall(fn_allocate, {
				builder.getInt64(layout.getTypeAllocSize(info.fields)) });
			builder.CreateCall(info.initializer, { result });
			auto typed_result = builder.CreateBitOrPointerCast(result, info.fields->getPointerTo());
			builder.CreateStore(cast_to(info.dispatcher, ptr_type), builder.CreateConstGEP2_32(obj_struct, result, AG_HEADER_OFFSET, 0));
			builder.CreateRet(typed_result);
			// Disposer
			if (special_copy_and_dispose.count(cls) == 0) {
				builder.SetInsertPoint(llvm::BasicBlock::Create(*context, "", info.dispose));
				current_function = info.dispose;
				if (auto manual_disposer_name = cls->name->peek("dispose")) {
					if (auto manual_disposer_fn = ast->functions_by_names.find(manual_disposer_name); manual_disposer_fn != ast->functions_by_names.end()) {
						builder.CreateCall(
							functions[manual_disposer_fn->second.pinned()],
							{ cast_to(info.dispose->getArg(0), info.fields->getPointerTo()) });
					}
				}
				if (base_info)
					builder.CreateCall(base_info->dispose, { info.dispose->getArg(0) });
				result = builder.CreateBitOrPointerCast(info.dispose->getArg(0), info.fields->getPointerTo());
				for (auto& field : cls->fields) {
					if (is_ptr(field->initializer->type()))
						build_release(
							builder.CreateLoad(
								ptr_type,
								builder.CreateStructGEP(info.fields, result, field->offset)),
							field->initializer->type());
				}
				builder.CreateRetVoid();
			}
			// Copier
			info.copier = llvm::Function::Create(
				copier_fn_type,
				special_copy_and_dispose.count(cls) == 0
					? llvm::Function::InternalLinkage
					: llvm::Function::ExternalLinkage,
				ast::format_str("ag_copy_", cls->name.pinned()), module.get());
			if (special_copy_and_dispose.count(cls) == 0) {
				builder.SetInsertPoint(llvm::BasicBlock::Create(*context, "", info.copier));
				current_function = info.copier;
				if (auto manual_fixer_name = cls->name->peek("afterCopy")) {
					if (auto manual_fixer_fn = ast->functions_by_names.find(manual_fixer_name); manual_fixer_fn != ast->functions_by_names.end()) {
						builder.CreateCall(
							fn_reg_copy_fixer,
							{
								cast_to(info.copier->getArg(0), ptr_type),
								cast_to(functions[manual_fixer_fn->second.pinned()], ptr_type)
							});
					}
				}
				if (base_info)
					builder.CreateCall(
						base_info->copier,
						{ info.copier->getArg(0), info.copier->getArg(1) });
				auto dst = builder.CreateBitOrPointerCast(info.copier->getArg(0), info.fields->getPointerTo());
				auto src = builder.CreateBitOrPointerCast(info.copier->getArg(1), info.fields->getPointerTo());
				for (auto& f : cls->fields) {
					auto type = f->initializer->type();
					if (auto as_opt = dom::strict_cast<ast::TpOptional>(type))
						type = as_opt->wrapped;
					if (isa<ast::TpWeak>(*type)) {
						builder.CreateCall(fn_copy_weak_field, {
							builder.CreateStructGEP(info.fields, dst, f->offset),
							builder.CreateLoad(
								ptr_type,
								builder.CreateStructGEP(info.fields, src, f->offset)) });
					} else if (isa<ast::TpDelegate>(*type)) {
						builder.CreateCall(fn_copy_weak_field, {
							builder.CreateStructGEP(
								delegate_struct,
								builder.CreateStructGEP(info.fields, dst, f->offset),
								0),
							builder.CreateLoad(
								ptr_type,
								builder.CreateStructGEP(
									delegate_struct,
									builder.CreateStructGEP(info.fields, src, f->offset),
									0)) });
					} else if (isa<ast::TpClass>(*type) || isa<ast::TpRef>(*type)) {
						builder.CreateStore(
							builder.CreateCall(fn_copy_object_field, {
								builder.CreateLoad(
									ptr_type,
									builder.CreateStructGEP(info.fields, src, f->offset)),
								dst }),
							builder.CreateStructGEP(info.fields, dst, f->offset));
					}
				}
				builder.CreateRetVoid();
			}
			// Class methods
			info.vmt_fields.push_back(info.dispatcher);  // class id for casts
			for (auto& m : cls->new_methods) {
				auto& m_info = methods[m];
				info.vmt_fields.push_back(compile_function(*m,
					ast::format_str("M_", cls->name.pinned(), '_', m->name.pinned()),
					info.fields->getPointerTo()));
			}
			size_t base_index = info.vmt_fields.size();
			if (cls->base_class) {
				auto& base_vmt = classes[cls->base_class].vmt_fields;
				for (size_t i = 0, j = base_vmt.size() - 1; i < j; i++)
					info.vmt_fields.push_back(base_vmt[i]);
				for (auto& m : cls->overloads[cls->base_class]) { // for overrides
					auto& m_info = methods[m];
					info.vmt_fields[base_index + m_info.ordinal] = compile_function(*m,
						ast::format_str("M_", cls->name.pinned(), '_', m->name.pinned()),
						info.fields->getPointerTo());
				}
			}
			info.vmt_fields.push_back(llvm::ConstantStruct::get(obj_vmt_struct, {
				info.copier,
				info.dispose,
				builder.getInt64(layout.getTypeStoreSize(info.fields)),
				builder.getInt64(info.vmt_size) }));
			info.dispatcher->setPrefixData(llvm::ConstantStruct::get(info.vmt, move(info.vmt_fields)));
			size_t interfaces_count = cls->interface_vmts.size();
			// Interface methods
			unordered_map<uint64_t, llvm::Constant*> vmts;  // interface_id->vmt_struct
			for (auto& i : cls->interface_vmts) {
				vector<llvm::Constant*> methods {
					llvm::ConstantExpr::getIntegerValue(ptr_type, llvm::APInt(64, classes[i.first].interface_ordinal, false)) };
				methods.reserve(i.second.size() + 1);
				for (auto& m : i.second) {
					methods.push_back(llvm::ConstantExpr::getBitCast(
						compile_function(
							*m.pinned(),
							ast::format_str("IM_", cls->name.pinned(), '_', i.first->name.pinned(), '_', m->name.pinned()),
							info.fields->getPointerTo()),
						ptr_type));
				}
				vmts.insert({
					classes[i.first].interface_ordinal,
					make_const_array(
						ast::format_str("mt_", cls->name.pinned(), "_", i.first->name.pinned()),
						move(methods))});
			}
			builder.SetInsertPoint(llvm::BasicBlock::Create(*context, "", info.dispatcher));
			auto mtable_ptr = build_i_table(ast::format_str("it_", cls->name.pinned()), builder, vmts, &*info.dispatcher->arg_begin());
			builder.CreateRet(
				builder.CreateLoad(
					ptr_type,
					builder.CreateGEP(
						ptr_type,
						builder.CreateBitOrPointerCast(mtable_ptr, ptr_type),
						{
							builder.CreateAnd(
								&*info.dispatcher->arg_begin(),
								builder.getInt64(0xffff))
						})));
		}
		// Compile standalone functions.
		for (auto& fn : ast->functions) {
			if (!fn->is_platform) {
				current_function = functions[fn];
				compile_fn_body(*fn, ast::format_str("fn_", fn->name.pinned()));
			}
		}
		current_function = llvm::Function::Create(
			llvm::FunctionType::get(void_type, {}, false),
			llvm::Function::ExternalLinkage,
			"main", module.get());
		compile_fn_body(*ast->entry_point, "main");
		// Compile tests
		for (auto& test : ast->tests_by_names) {
			auto fn = llvm::Function::Create(
				llvm::FunctionType::get(void_type, {}, false),
				llvm::Function::ExternalLinkage,
				ast::format_str("ag_test_", test.second->name.pinned()),
				module.get());
			current_function = fn;
			compile_fn_body(*test.second, ast::format_str("test_", test.second->name.pinned()));
		}
		if (di_builder) {
			vector<llvm::Metadata*> cls_di_parts;
			for (auto& c : ast->classes) {
				if (c->is_interface)
					continue;
				vector<llvm::Metadata*> cls_di_fields;
				std::function<size_t(ast::TpClass& cl)> field_builder = [&](ast::TpClass& cl) {
					size_t offset = cl.base_class
						? field_builder(*cl.base_class.pinned())
						: 0;
					for (auto& f : cl.fields) {
						size_t width = to_llvm_type(*f->initializer->type())
							->getScalarSizeInBits();
						cls_di_fields.push_back(di_builder->createMemberType(
							di_cu,
							std::to_string(f->name.pinned()),
							di_cu->getFile(),
							0,  // line
							width,
							0,  // align
							offset,
							llvm::DINode::DIFlags::FlagZero,
							to_di_type(*f->initializer->type())));
						offset += width;
					}
					return offset;
				};
				size_t size = field_builder(*c);
				cls_di_parts.push_back(di_builder->createVariantMemberType(
					di_obj_variants,
					ast::format_str("tag_", c->name.pinned()),
					di_cu->getFile(),
					0,  // line
					size,
					0,  // align
					0,  // offset
					classes[c].dispatcher,
					llvm::DINode::DIFlags::FlagZero,
					di_builder->createStructType(
						nullptr,
						std::to_string(c->name.pinned()),
						nullptr,
						0,  // line
						size,
						0, // align
						llvm::DINode::DIFlags::FlagZero,
						nullptr,  // base
						di_builder->getOrCreateArray(move(cls_di_fields)))));
			}
			di_builder->replaceArrays(di_obj_variants, di_builder->getOrCreateArray(move(cls_di_parts)));
		}
		if (di_builder)
			di_builder->finalize();
		module->addModuleFlag(llvm::Module::Warning, "Debug Info Version", llvm::DEBUG_METADATA_VERSION);
		module->addModuleFlag(llvm::Module::Warning, "CodeView", 1);
		return llvm::orc::ThreadSafeModule(std::move(module), std::move(context));
	}

	llvm::Constant* make_const_array(string name, vector<llvm::Constant*> content) {
		auto type = llvm::ArrayType::get(ptr_type, content.size());
		module->getOrInsertGlobal(name, type);
		auto result = module->getGlobalVariable(name);
		result->setInitializer(llvm::ConstantArray::get(type, move(content)));
		result->setConstant(true);
		result->setLinkage(llvm::GlobalValue::InternalLinkage);
		return result;
	}
};

llvm::orc::ThreadSafeModule generate_code(ltm::pin<ast::Ast> ast, bool add_debug_info) {
	Generator gen(ast, add_debug_info);
	return gen.build();
}

int64_t execute(llvm::orc::ThreadSafeModule& module, ast::Ast& ast, bool dump_ir) {
#ifndef AG_STANDALONE_COMPILER_MODE
	if (dump_ir) {
		module.withModuleDo([](llvm::Module& m) {
			m.print(llvm::outs(), nullptr);
		});
	}
	llvm::ExitOnError check;
	llvm::InitializeNativeTarget();
	llvm::InitializeNativeTargetAsmPrinter();
	auto jit = check(llvm::orc::LLJITBuilder().create());
	auto& es = jit->getExecutionSession();
	auto* lib = es.getJITDylibByName("main");
	llvm::orc::SymbolMap runtime_exports;
	for (auto& i : ast.platform_exports)
		runtime_exports.insert({ es.intern(i.first), { llvm::pointerToJITTargetAddress(i.second), llvm::JITSymbolFlags::Callable} });
	check(lib->define(llvm::orc::absoluteSymbols(move(runtime_exports))));
	check(jit->addIRModule(std::move(module)));
	auto f_main = check(jit->lookup("main"));
	auto main_addr = f_main.toPtr<void()>();
	for (auto& test : ast.tests_by_names) {
		std::cout << "Test:" << std::to_string(test.first.pinned());
	 	auto test_fn = check(jit->lookup(ast::format_str("ag_test_", test.second->name.pinned())));
		auto addr = test_fn.toPtr<void()>();
		addr();
		assert(ag_leak_detector_ok());
		std::cout << " passed" << std::endl;
	}
	main_addr();
	assert(ag_leak_detector_ok());
	return 0;
#else
	return -1;
#endif
}

static bool llvm_inited = false;
static const char* arg = "";
static const char** argv = &arg;
static int argc = 0;

int64_t generate_and_execute(ltm::pin<ast::Ast> ast, bool add_debug_info, bool dump_ir) {
	if (!llvm_inited)
		llvm::InitLLVM X(argc, argv);
	llvm_inited = true;
	auto module = generate_code(ast, add_debug_info);
	std::cout << "LLVM Working" << std::endl;
	return execute(module, *ast, dump_ir);
}
