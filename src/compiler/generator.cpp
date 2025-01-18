#include "generator.h"

#include <functional>
#include <string>
#include <random>
#include <variant>
#include <list>
#include <vector>
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "utils/vmt_util.h"
#include "runtime.h"

#include "llvm/Bitcode/BitcodeWriter.h"

using std::string;
using std::vector;
using std::list;
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
using ltm::cast;
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
	own<ast::Type> type; // used to optionize retain/release for non-optionals, and for optional branches folding
	llvm::Value* data = nullptr;
	struct Static {}; // non-pointer data, nullptr, string literal, named constant - thing that need no disposal but can be safely passed to disposeVal, default case
	struct Temp { weak<ast::Var> var; };  // value fetched from local variable or field, see below
	struct Retained {}; // retained temp (fn result, newly constructed object, locked for other reasons), must be released or moved to Var or to Val::RField::to_release.
	struct RField { llvm::Value* to_release; };  // temp raw, that represents child subobject, of Retained temp. It must be locked and converted to Retained if needed, it must destroy its protector in the end. 
	// nonptr fields become Static
	// ptr fields of Retained become RField, other ptr's fields become Temp{null}
	using lifetime_t = std::variant<Static, Temp, Retained, RField>;
	lifetime_t lifetime;
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

// ClassInstance has own vmt and dispatcher and share with its inplementation class all other fields.
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
	llvm::Function* visit = nullptr;       // void(void* dst, void (*visitor) (void* field, int type, void* ctx));
	llvm::Function* dispatcher = nullptr;  // void*(void*obj, uint64 inerface_and_method_ordinal);
	vector<llvm::Constant*> vmt_fields;    // pointers to methods. size <= 2^16, at index 0 - inteface id for dynamic cast
	uint64_t interface_ordinal = 0;        // 48_bit_random << 16
	llvm::ArrayType* ivmt = nullptr;       // only for interface i8*[methods_count+1], ivmt[0]=inteface_id
	llvm::DICompositeType* di_cls = 0;
	llvm::DIType* di_ptr = 0;
};

template<typename T>
struct vec_ptr_hasher {
	size_t operator() (const vector<T*>& v) const {
		size_t r = 0;
		for (const auto& p : v)
			r += std::hash<void*>()(p);
		return r;
	}
};

struct Generator : ast::ActionScanner {
	ltm::pin<ast::Ast> ast;
	std::unique_ptr<llvm::LLVMContext> context;
	std::unique_ptr<llvm::Module> module;
	llvm::IntegerType* int_type = nullptr;
	llvm::IntegerType* int32_type = nullptr;
	llvm::Type* float_type = nullptr;
	llvm::Type* double_type = nullptr;
	llvm::PointerType* ptr_type = nullptr;
	llvm::Type* void_type = nullptr;
	llvm::IRBuilder<>* builder = nullptr;
	Val* result = nullptr;
	llvm::DataLayout layout;
	unordered_map<pin<ast::TpLambda>, llvm::FunctionType*> lambda_fns; // function having 0th param of ptr_type
	unordered_map<pin<ast::TpFunction>, llvm::FunctionType*> function_types;
	unordered_map<pin<ast::AbstractClass>, ClassInfo> classes;  // for classes and class instances
	unordered_map<pin<ast::Method>, MethodInfo> methods;
	unordered_map<pin<ast::Function>, llvm::Function*> functions;
	vector<function<void()>> execute_in_global_scope;
	llvm::BasicBlock* bb_for_captures = nullptr;
	struct BreakTrace {
		llvm::BasicBlock* bb;
		Val result;
		weak<ast::Block> target;
		weak<ast::Action> origin; // debug only, break or call
	};
	vector<BreakTrace> active_breaks;
	
	llvm::DICompileUnit* di_cu = nullptr;
	unordered_map<string, llvm::DIFile*> di_files;
	llvm::DIScope* current_di_scope = nullptr;
	llvm::DIType* current_capture_di_type = nullptr;
	std::unique_ptr<llvm::DIBuilder> di_builder;
	llvm::DIType* di_float = nullptr;
	llvm::DIType* di_double = nullptr;
	llvm::DIType* di_int32 = nullptr;
	llvm::DIType* di_void = nullptr;
	llvm::DIType* di_int = nullptr;
	llvm::DIType* di_byte = nullptr;
	llvm::DIType* di_obj_ptr = nullptr;
	llvm::DIType* di_weak_ptr = nullptr;
	llvm::DIType* di_opt_int = nullptr;
	llvm::DIType* di_delegate = nullptr;
	llvm::DIType* di_fn_ptr = nullptr;
	llvm::DISubroutineType* di_fn_type = nullptr;
	llvm::DIType* di_lambda = nullptr;
	llvm::DICompositeType* di_obj_struct = nullptr;

	llvm::Function* current_ll_fn = nullptr;
	pin<ast::MkLambda> current_function;
	unordered_map<weak<ast::Var>, llvm::Value*> locals;
	unordered_map<weak<ast::Var>, llvm::Value*> globals;
	unordered_map<weak<ast::Var>, int> capture_offsets;
	vector<pair<int, llvm::StructType*>> captures;
	vector<llvm::Value*> capture_ptrs;
	llvm::Type* tp_opt_int = nullptr;
	llvm::Type* tp_bool = nullptr;
	llvm::IntegerType* tp_opt_bool = nullptr;
	llvm::Type* tp_opt_lambda = nullptr;
	llvm::Type* tp_opt_delegate = nullptr;
	llvm::Type* tp_int_ptr = nullptr;
	llvm::StructType* lambda_struct = nullptr;
	llvm::StructType* delegate_struct = nullptr;
	llvm::StructType* obj_struct = nullptr;
	llvm::StructType* weak_struct = nullptr;
	llvm::StructType* obj_vmt_struct = nullptr;
	llvm::Function* fn_init = nullptr;   // void()
	llvm::Function* fn_set_parent = nullptr;   // void(Obj*, Obj* parent)  // used when retained object gets assigned to field
	llvm::Function* fn_splice = nullptr;   // bool(Obj*, Obj* parent)  // checks loops, retains, sets parent
	llvm::Function* fn_release_pin = nullptr;  // void(Obj*) no_throw // used for pins and local owns, doesn't clear parent
	llvm::Function* fn_release_pin_nn = nullptr;  // same as above but without null-check
	llvm::Function* fn_release_shared = nullptr;  // void(Obj*) no_throw // used for shared-frozen
	llvm::Function* fn_release_shared_nn = nullptr;  // same as above but without null-check
	llvm::Function* fn_release_own = nullptr;  // void(Obj*) no_throw as pin + clears parent
	llvm::Function* fn_release_own_nn = nullptr;  // same as above but without null-check
	llvm::Function* fn_release_weak = nullptr;  // void(WB*) no_throw
	llvm::Function* fn_retain_shared = nullptr;  // void(Obj*) no_throw, handles mt, used in shared and conform
	llvm::Function* fn_retain_shared_nn = nullptr;  // same as above but without null-check
	llvm::Function* fn_retain_own = nullptr;  // void(Obj*, Obj*parent) no_throw as pin + sets parent, used in set_field
	llvm::Function* fn_retain_own_nn = nullptr;  // same as above but without null-check
	llvm::Function* fn_retain_weak = nullptr;  // void(WB*) no_throw 
	llvm::Function* fn_dispose = nullptr;  // void(Obj*) no_throw // used in releaseObj
	llvm::Function* fn_allocate = nullptr; // Obj*(size_t)
	llvm::Function* fn_copy = nullptr;   // Obj*(Obj*)
	llvm::Function* fn_freeze = nullptr;   // Obj*(Obj*)
	llvm::Function* fn_mk_weak = nullptr;   // WB*(Obj*)
	llvm::Function* fn_deref_weak = nullptr;   // intptr_aka_?obj* (WB*)
	llvm::Function* fn_copy_object_field = nullptr;   // Obj* (Obj* src, Obj* parent)
	llvm::Function* fn_copy_weak_field = nullptr;   // void(WB** dst, WB* src)
	llvm::FunctionType* fn_copy_fixer_fn_type = nullptr;  // void (*)(Obj*)
	llvm::FunctionType* trampoline_fn_type = nullptr; // void (Obj* self, ag_fn entry_point, ag_thread* th)
	llvm::Function* fn_reg_copy_fixer = nullptr;      // void (Obj*, fn_fixer_type)
	llvm::Function* fn_unlock_thread_queue = nullptr;   // void(ag_hread*)
	llvm::Function* fn_get_thread_param = nullptr;   // in64 (ag_hread*)
	llvm::Function* fn_prepare_post_message = nullptr;   // ?ag_hread* (weak*, fn, tramp, int params)
	llvm::Function* fn_put_thread_param = nullptr;   // void (int64 val)
	llvm::Function* fn_put_thread_param_own_ptr = nullptr;   // void (?ag_hread*, Obj* val)
	llvm::Function* fn_put_thread_param_weak_ptr = nullptr;   // void (?ag_hread*, Weak* val)
	llvm::Function* fn_handle_main_thread = nullptr;   // int (void)
	llvm::Function* fn_eq_mut = nullptr;   // bool(Obj*, Obj*)
	llvm::Function* fn_eq_shared = nullptr;   // bool(Obj*, Obj*)
	std::default_random_engine random_generator;
	std::uniform_int_distribution<uint64_t> uniform_uint64_distribution;
	unordered_set<uint64_t> assigned_interface_ids;
	std::unordered_map<own<ast::TpDelegate>, pair<llvm::Function*, size_t>> trampolines;
	llvm::FunctionType* dispatcher_fn_type = nullptr;
	llvm::Constant* empty_mtable = nullptr; // void_ptr[1] = { null }
	unordered_map<weak<ast::MkLambda>, llvm::Function*> compiled_functions;
	llvm::Constant* null_weak = nullptr;
	llvm::Constant* const_0 = nullptr;
	llvm::Constant* const_1 = nullptr;
	llvm::Constant* const_256 = nullptr;
	llvm::Constant* const_ctr_step = nullptr;
	llvm::Constant* const_ctr_static = nullptr;  // SHARED|MT|0
	llvm::Constant* const_ctr_str_literal = nullptr;  // SHARED|MT|HASH|0
	llvm::Constant* const_null_ptr = nullptr;
	unordered_map<string, llvm::GlobalVariable*> string_literals;
	unordered_map<
		vector<llvm::Constant*>,
		llvm::Constant*,
		vec_ptr_hasher<llvm::Constant>> table_cache;

	Generator(ltm::pin<ast::Ast> ast, bool debug_info_mode)
		: ast(ast)
		, context(new llvm::LLVMContext)
		, layout("")
	{
		module = std::make_unique<llvm::Module>("code", *context);
		if (debug_info_mode)
			make_di_basic();
		int_type = llvm::Type::getInt64Ty(*context);
		int32_type = llvm::Type::getInt32Ty(*context);
		float_type = llvm::Type::getFloatTy(*context);
		double_type = llvm::Type::getDoubleTy(*context);
		ptr_type = llvm::PointerType::getUnqual(*context);
		tp_int_ptr = layout.getIntPtrType(ptr_type);
		void_type = llvm::Type::getVoidTy(*context);
		tp_opt_bool = llvm::Type::getInt8Ty(*context);
		tp_opt_int = llvm::StructType::get(*context, { tp_opt_bool, int_type });
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
		const_ctr_step = llvm::ConstantInt::get(tp_int_ptr, AG_CTR_STEP);
		const_ctr_static = llvm::ConstantInt::get(tp_int_ptr, AG_CTR_SHARED | AG_CTR_MT);
		const_ctr_str_literal = llvm::ConstantInt::get(tp_int_ptr, AG_CTR_SHARED | AG_CTR_MT | AG_CTR_HASH);
		const_null_ptr = llvm::ConstantPointerNull::get(ptr_type);

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
		fn_retain_own_nn = llvm::Function::Create(
			llvm::FunctionType::get(void_type, { ptr_type, ptr_type }, false),
			llvm::Function::ExternalLinkage,
			"ag_retain_own_nn",
			*module);
		fn_retain_weak = llvm::Function::Create(
			llvm::FunctionType::get(void_type, { ptr_type }, false),
			llvm::Function::ExternalLinkage,
			"ag_retain_weak",
			*module);
		fn_retain_shared = llvm::Function::Create(
			llvm::FunctionType::get(void_type, { ptr_type }, false),
			llvm::Function::ExternalLinkage,
			"ag_retain_shared",
			*module);
		fn_retain_shared_nn = llvm::Function::Create(
			llvm::FunctionType::get(void_type, { ptr_type }, false),
			llvm::Function::ExternalLinkage,
			"ag_retain_shared_nn",
			*module);
		fn_set_parent = llvm::Function::Create(
			llvm::FunctionType::get(void_type, { ptr_type, ptr_type}, false),
			llvm::Function::ExternalLinkage,
			"ag_set_parent",
			*module);
		fn_splice = llvm::Function::Create(
			llvm::FunctionType::get(tp_bool, { ptr_type, ptr_type, ptr_type }, false),
			llvm::Function::ExternalLinkage,
			"ag_splice",
			*module);
		fn_release_pin = llvm::Function::Create(
			llvm::FunctionType::get(void_type, { ptr_type }, false),
			llvm::Function::ExternalLinkage,
			"ag_release_pin",
			*module);
		fn_release_shared = llvm::Function::Create(
			llvm::FunctionType::get(void_type, { ptr_type }, false),
			llvm::Function::ExternalLinkage,
			"ag_release_shared",
			*module);
		fn_release_shared_nn = llvm::Function::Create(
			llvm::FunctionType::get(void_type, { ptr_type }, false),
			llvm::Function::ExternalLinkage,
			"ag_release_shared_nn",
			*module);
		fn_release_own = llvm::Function::Create(
			llvm::FunctionType::get(void_type, { ptr_type }, false),
			llvm::Function::ExternalLinkage,
			"ag_release_own",
			*module);
		fn_release_own_nn = llvm::Function::Create(
			llvm::FunctionType::get(void_type, { ptr_type }, false),
			llvm::Function::ExternalLinkage,
			"ag_release_own_nn",
			*module);
		fn_release_weak = llvm::Function::Create(
			llvm::FunctionType::get(void_type, { ptr_type }, false),
			llvm::Function::ExternalLinkage,
			"ag_release_weak",
			*module);
		fn_allocate = llvm::Function::Create(
			llvm::FunctionType::get(ptr_type, { int_type }, false),
			llvm::Function::ExternalLinkage,
			"ag_allocate_obj",
			*module);
		fn_init = llvm::Function::Create(
			llvm::FunctionType::get(void_type, {}, false),
			llvm::Function::ExternalLinkage,
			"ag_init",
			*module);
		fn_copy = llvm::Function::Create(
			llvm::FunctionType::get(ptr_type, { ptr_type }, false),
			llvm::Function::ExternalLinkage,
			"ag_copy",
			*module);
		fn_freeze = llvm::Function::Create(
			llvm::FunctionType::get(ptr_type, { ptr_type }, false),
			llvm::Function::ExternalLinkage,
			"ag_freeze",
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
		trampoline_fn_type = llvm::FunctionType::get(void_type, { ptr_type, ptr_type, ptr_type }, false);
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
		fn_eq_mut = llvm::Function::Create(
			llvm::FunctionType::get(tp_bool, { ptr_type, ptr_type }, false),
			llvm::Function::ExternalLinkage,
			"ag_eq_mut",
			*module);
		fn_eq_shared = llvm::Function::Create(
			llvm::FunctionType::get(tp_bool, { ptr_type, ptr_type }, false),
			llvm::Function::ExternalLinkage,
			"ag_eq_shared",
			*module);
		fn_unlock_thread_queue = llvm::Function::Create(
			llvm::FunctionType::get(void_type, { ptr_type }, false),
			llvm::Function::ExternalLinkage,
			"ag_unlock_thread_queue",
			*module);
		fn_get_thread_param = llvm::Function::Create(
			llvm::FunctionType::get(int_type, { ptr_type }, false),
			llvm::Function::ExternalLinkage,
			"ag_get_thread_param",
			*module);
		fn_prepare_post_message = llvm::Function::Create(
			llvm::FunctionType::get(ptr_type, { ptr_type, ptr_type, ptr_type, int_type }, false),
			llvm::Function::ExternalLinkage,
			"ag_prepare_post_from_ag",
			*module);
		fn_put_thread_param = llvm::Function::Create(
			llvm::FunctionType::get(void_type, { int_type }, false),
			llvm::Function::ExternalLinkage,
			"ag_post_param_from_ag",
			*module);
		fn_put_thread_param_own_ptr = llvm::Function::Create(
			llvm::FunctionType::get(void_type, { ptr_type, ptr_type }, false),
			llvm::Function::ExternalLinkage,
			"ag_post_own_param_from_ag",
			*module);
		fn_put_thread_param_weak_ptr = llvm::Function::Create(
			llvm::FunctionType::get(void_type, { ptr_type }, false),
			llvm::Function::ExternalLinkage,
			"ag_post_weak_param_from_ag",
			*module);
		fn_handle_main_thread = llvm::Function::Create(
			llvm::FunctionType::get(int_type, {}, false),
			llvm::Function::ExternalLinkage,
			"ag_handle_main_thread",
			*module);
	}

	void make_di_basic() {
		di_builder = std::make_unique<llvm::DIBuilder>(*module);
		di_cu = di_builder->createCompileUnit(
			llvm::dwarf::DW_LANG_C_plus_plus,
			di_builder->createFile("sys", ""),
			"sys",
			true, "", 0);
		di_float = di_builder->createBasicType("float", 32, llvm::dwarf::DW_ATE_float);
		di_double = di_builder->createBasicType("double", 64, llvm::dwarf::DW_ATE_float);
		di_int32 = di_builder->createBasicType("int", 32, llvm::dwarf::DW_ATE_signed);
		di_int = di_builder->createBasicType("int32", 64, llvm::dwarf::DW_ATE_signed);
		di_byte = di_builder->createBasicType("byte", 8, llvm::dwarf::DW_ATE_signed);
		di_fn_type = di_builder->createSubroutineType(di_builder->getOrCreateTypeArray({}));
		di_fn_ptr = di_builder->createPointerType(di_fn_type, 64);
		di_void = di_builder->createBasicType("void", 64, llvm::dwarf::DW_ATE_unsigned);
		auto di_ptr = di_builder->createPointerType(di_int, 64);
		di_obj_struct = di_builder->createStructType(
			di_cu, "_obj", di_cu->getFile(),
			0, 3 * 64, 0,
			llvm::DINode::DIFlags::FlagZero, nullptr,
			di_builder->getOrCreateArray({
				di_builder->createMemberType(nullptr, "disp", nullptr,    0, 64, 0, 0, llvm::DINode::DIFlags::FlagZero, di_fn_ptr),
				di_builder->createMemberType(nullptr, "counter", nullptr, 0, 64, 0, 64, llvm::DINode::DIFlags::FlagZero, di_int),
				di_builder->createMemberType(nullptr, "wb_p", nullptr, 0, 64, 0, 2 * 64, llvm::DINode::DIFlags::FlagZero, di_int),
				}));
		di_obj_ptr = di_builder->createObjectPointerType(di_obj_struct);
		di_weak_ptr = di_builder->createPointerType(
			di_builder->createStructType(
				di_cu, "_weak", di_cu->getFile(),
				0, 3 * 64, 0,
				llvm::DINode::DIFlags::FlagZero, nullptr,
				di_builder->getOrCreateArray({
					di_builder->createMemberType(di_cu, "target", di_cu->getFile(), 0, 64, 0, 0, llvm::DINode::DIFlags::FlagZero, di_obj_ptr),
					di_builder->createMemberType(di_cu, "counter", di_cu->getFile(), 0, 64, 0, 64, llvm::DINode::DIFlags::FlagZero, di_int),
					di_builder->createMemberType(di_cu, "org_parent", di_cu->getFile(), 0, 64, 0, 2 * 64, llvm::DINode::DIFlags::FlagZero, di_int) })),
					64);
		di_opt_int = di_builder->createStructType(
			di_cu, "_opt_int", di_cu->getFile(),
			0, 2 * 64, 0,
			llvm::DINode::DIFlags::FlagZero, nullptr,
			di_builder->getOrCreateArray({
				di_builder->createMemberType(di_cu, "opt", di_cu->getFile(), 0, 64, 0, 0, llvm::DINode::DIFlags::FlagZero, di_int),
				di_builder->createMemberType(di_cu, "val", di_cu->getFile(), 0, 64, 0, 64, llvm::DINode::DIFlags::FlagZero, di_int) }));
		di_delegate = di_builder->createStructType(
			di_cu, "_delegate", di_cu->getFile(),
			0, 2 * 64, 0,
			llvm::DINode::DIFlags::FlagZero, nullptr,
			di_builder->getOrCreateArray({
				di_builder->createMemberType(di_cu, "weak", di_cu->getFile(), 0, 64, 0, 0, llvm::DINode::DIFlags::FlagZero, di_weak_ptr),
				di_builder->createMemberType(di_cu, "fn", di_cu->getFile(), 0, 64, 0, 64, llvm::DINode::DIFlags::FlagZero, di_fn_ptr) }));
		di_lambda = di_builder->createStructType(
			di_cu, "_lambda", di_cu->getFile(),
			0, 2 * 64, 0,
			llvm::DINode::DIFlags::FlagZero, nullptr,
			di_builder->getOrCreateArray({
				di_builder->createMemberType(di_cu, "context", di_cu->getFile(), 0, 64, 0, 0, llvm::DINode::DIFlags::FlagZero, di_ptr),
				di_builder->createMemberType(di_cu, "fn", di_cu->getFile(), 0, 64, 0, 64, llvm::DINode::DIFlags::FlagZero, di_fn_ptr) }));
	}
	void make_di_clases() {
		if (!di_builder)
			return;
		for (auto& c : ast->classes_in_order) {
			if (c->is_interface || !c->used)
				continue;
			auto& ci = classes.at(c);
			ci.di_cls = di_builder->createClassType(
				nullptr,
				ast::format_str("ag_cls_", c->get_name()),
				di_cu->getFile(),
				0,  // line
				layout.getTypeAllocSize(ci.fields) * 8,
				0,  // align: layout.getABITypeAlign(field_type).value() * 8,
				0,  // offset
				llvm::DINode::DIFlags::FlagNonTrivial,
				c->base_class ? classes.at(c->base_class).di_cls : di_obj_struct,
				nullptr,  // fields
				di_obj_struct);
			ci.di_ptr = di_builder->createPointerType(ci.di_cls, 64);
		}
		for (auto& c : ast->classes_in_order) {
			if (c->is_interface || !c->used)
				continue;
			auto& ci = classes.at(c);
			vector<llvm::Metadata*> di_fields;
			auto base_fields = c->base_class ? classes.at(c->base_class).fields : obj_struct;
			size_t base_size = layout.getTypeAllocSize(base_fields) * 8;
			auto struct_layout = layout.getStructLayout(ci.fields);
			size_t i = 0;
			while (i < ci.fields->getNumElements() && struct_layout->getElementOffsetInBits(i) < base_size)
				i++; // skip base fields
			di_fields.push_back(di_builder->createInheritance(
				ci.di_cls,
				c->base_class ? classes.at(c->base_class).di_cls : di_obj_struct,
				0,  // base offset
				0,  // vptr offset
				llvm::DINode::DIFlags::FlagZero));
			if (c == ast->string_cls) {
				di_fields.push_back(di_builder->createMemberType(
					di_cu,
					"text",
					di_cu->getFile(),
					0,  // line
					layout.getPointerSizeInBits(),
					0,  // align
					struct_layout->getElementOffsetInBits(i),
					llvm::DINode::DIFlags::FlagZero,
					di_builder->createBasicType("asciiz", 8, llvm::dwarf::DW_ATE_UTF)));
			} else if (c == ast->own_array->base_class) { // container, add no fields
			} else if (c == ast->own_array || c == ast->weak_array || c == ast->blob) {
				di_fields.push_back(di_builder->createMemberType(
					di_cu,
					"count",
					di_cu->getFile(),
					0,  // line
					layout.getPointerSizeInBits(),
					0,  // align
					struct_layout->getElementOffsetInBits(i - 2),
					llvm::DINode::DIFlags::FlagZero,
					di_int));
				// llvm::SmallVector<uint64_t, 4> ops;
				// ops.push_back(llvm::dwarf::DW_OP_push_object_address);
				// llvm::DIExpression::appendOffset(ops, struct_layout->getElementOffset(i));
				// ops.push_back(llvm::dwarf::DW_OP_deref);
				di_fields.push_back(di_builder->createMemberType(
					di_cu,
					"items",
					di_cu->getFile(),
					0,  // line
					layout.getPointerSizeInBits(),
					0,  // align
					struct_layout->getElementOffsetInBits(i - 1),
					llvm::DINode::DIFlags::FlagZero,
					di_builder->createPointerType(
						di_builder->createArrayType(
							20 * (c == ast->blob ? 64 : layout.getPointerSizeInBits()), // array size
							0, // align
							c == ast->blob ? di_int :
								ast->weak_array ? di_weak_ptr :
							    di_obj_ptr, // item type
							di_builder->getOrCreateArray({
								di_builder->getOrCreateSubrange(
									0,
									20) //di_builder->createExpression(move(ops))
							})),
						layout.getPointerSizeInBits())));
			} else {
				for (auto& f : c->fields) {
					auto field_type = ci.fields->getElementType(i);
					di_fields.push_back(di_builder->createMemberType(
						di_cu,
						f->name,
						di_cu->getFile(),
						0,  // line
						layout.getTypeSizeInBits(field_type),
						0,  // align: layout.getABITypeAlign(field_type).value() * 8,
						struct_layout->getElementOffsetInBits(i),
						llvm::DINode::DIFlags::FlagZero,
						to_di_type(*f->initializer->type())));
					i++;
				}
			}
			di_builder->replaceArrays(ci.di_cls, di_builder->getOrCreateArray(move(di_fields)));
		}
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
		return dom::strict_cast<ast::TpOwn>(type) ||
			dom::strict_cast<ast::TpRef>(type) ||
			dom::strict_cast<ast::TpWeak>(type) ||
			dom::strict_cast<ast::TpShared>(type) ||
			dom::strict_cast<ast::TpDelegate>(type);
	}

	void build_inc(llvm::Constant* step, llvm::Value* addr) {
		builder->CreateStore(
			builder->CreateAdd(
				builder->CreateLoad(tp_int_ptr, addr),
				step),
			addr);
	}
	void build_retain(llvm::Value* ptr, pin<ast::Type> type, llvm::Value* maybe_own_parent = nullptr) {
		if (!is_ptr(type))
			return;
		auto as_opt = dom::strict_cast<ast::TpOptional>(type);
		if (as_opt) 
			type = as_opt->wrapped;
		if (isa<ast::TpOwn>(*type) && maybe_own_parent) {
			builder->CreateCall(as_opt ? fn_retain_own : fn_retain_own_nn, { cast_to(ptr, ptr_type), maybe_own_parent });
		} else if (isa<ast::TpDelegate>(*type)) {
			builder->CreateCall(fn_retain_weak, {
				builder->CreateExtractValue(ptr, { 0 }),
			});
		} else if (isa<ast::TpWeak>(*type) || isa<ast::TpConformWeak>(*type) || isa<ast::TpFrozenWeak>(*type)) {
			builder->CreateCall(fn_retain_weak, { cast_to(ptr, ptr_type) });
		} else if (isa<ast::TpShared>(*type) || isa<ast::TpConformRef>(*type)) {
			builder->CreateCall(as_opt ? fn_retain_shared : fn_retain_shared_nn, { cast_to(ptr, ptr_type) });
		} else if (as_opt) {  // inlined fn_retain_pin
			auto bb_not_null = llvm::BasicBlock::Create(*context, "", current_ll_fn);
			auto bb_null = llvm::BasicBlock::Create(*context, "", current_ll_fn);
			builder->CreateCondBr(
				builder->CreateCmp(llvm::CmpInst::Predicate::ICMP_UGT,
					cast_to(ptr, tp_int_ptr),
					const_256),
				bb_not_null,
				bb_null);
			builder->SetInsertPoint(bb_not_null);
			build_inc(const_ctr_step, builder->CreateStructGEP(obj_struct, cast_to(ptr, ptr_type), 1));
			builder->CreateBr(bb_null);
			builder->SetInsertPoint(bb_null);
		} else {  // inlined fn_retain_pin_nn
			build_inc(const_ctr_step, builder->CreateStructGEP(obj_struct, cast_to(ptr, ptr_type), 1));
		}
	}

	void build_release_ptr_not_null(llvm::Value* ptr) {
		llvm::Value* counter_addr = builder->CreateStructGEP(obj_struct, ptr, 1);
		llvm::Value* ctr = builder->CreateSub(
			builder->CreateLoad(tp_int_ptr, counter_addr),
			const_ctr_step);
		builder->CreateStore(ctr, counter_addr);
		auto bb_not_zero = llvm::BasicBlock::Create(*context, "", current_ll_fn);
		auto bb_zero = llvm::BasicBlock::Create(*context, "", current_ll_fn);
		builder->CreateCondBr(
			builder->CreateCmp(llvm::CmpInst::Predicate::ICMP_ULT, ctr, const_ctr_step),
			bb_zero,
			bb_not_zero);
		builder->SetInsertPoint(bb_zero);
		builder->CreateCall(fn_dispose, { cast_to(ptr, ptr_type) });
		builder->CreateBr(bb_not_zero);
		builder->SetInsertPoint(bb_not_zero);
	}

	void build_release(llvm::Value* ptr, pin<ast::Type> type, bool is_local = true) {
		if (!is_ptr(type))
			return;
		if (auto as_opt = dom::strict_cast<ast::TpOptional>(type)) {
			if (isa<ast::TpWeak>(*as_opt->wrapped) || isa<ast::TpConformWeak>(*as_opt->wrapped) || isa<ast::TpFrozenWeak>(*as_opt->wrapped)) {
				builder->CreateCall(fn_release_weak, { cast_to(ptr, ptr_type) });
			} else if (isa<ast::TpDelegate>(*as_opt->wrapped)) {
				builder->CreateCall(fn_release_weak, {
					builder->CreateExtractValue(
						cast_to(ptr, delegate_struct),
						{0}) });
			} else if (isa<ast::TpRef>(*as_opt->wrapped) || (isa<ast::TpOwn>(*as_opt->wrapped) && is_local)) {
				builder->CreateCall(fn_release_pin, { cast_to(ptr, ptr_type) });
			} else if (isa<ast::TpShared>(*as_opt->wrapped) || isa<ast::TpConformRef>(*as_opt->wrapped)) {
				builder->CreateCall(fn_release_shared, { cast_to(ptr, ptr_type) });
			} else if (isa<ast::TpOwn>(*as_opt->wrapped)) {
				builder->CreateCall(fn_release_own, { cast_to(ptr, ptr_type) });
			}
		} else {
			if (isa<ast::TpWeak>(*type) || isa<ast::TpConformWeak>(*type) || isa<ast::TpFrozenWeak>(*type)) {
				builder->CreateCall(fn_release_weak, { cast_to(ptr, ptr_type) });
			} else if (isa<ast::TpDelegate>(*type)) {
				builder->CreateCall(fn_release_weak, {
					builder->CreateExtractValue(
						cast_to(ptr, delegate_struct),
						{0})});
			} else if (isa<ast::TpRef>(*type) || (isa<ast::TpOwn>(*type) && is_local)) {
				build_release_ptr_not_null(ptr);  // inlined ag_release_pin_nn
			} else if (isa<ast::TpShared>(*type) || isa<ast::TpConformRef>(*type)) {
				builder->CreateCall(fn_release_shared_nn, { ptr });
			} else if (isa<ast::TpOwn>(*type)) {
				builder->CreateCall(fn_release_own_nn, { ptr });
			}
		}
	}
	
	llvm::Value* remove_indirection(const ast::Var& var, llvm::Value* val) {
		return var.is_mutable || var.captured || var.is_const
			? builder->CreateLoad(to_llvm_type(*var.type),  val)
			: val;
	}

	void dispose_val_in_current_bb(Val& val, bool is_local = true) {
		if (get_if<Val::Retained>(&val.lifetime)) {
			build_release(val.data, val.type, is_local);
		} else if (auto as_rfield = get_if<Val::RField>(&val.lifetime)) {
			build_release_ptr_not_null(as_rfield->to_release);
		}
		if (val.optional_br) {
			auto common_bb = llvm::BasicBlock::Create(*context, "", current_ll_fn);
			builder->CreateBr(common_bb);
			for (auto& b = val.optional_br; b; b = b->deeper) {
				if (b->none_bb) {
					builder->SetInsertPoint(b->none_bb);
					builder->CreateBr(common_bb);
				}
			}
			builder->SetInsertPoint(common_bb);
		}
	}
	void dispose_in_active_breaks(Val& val, size_t newer_than_break, bool is_local = true) {
		if (!active_breaks.empty()) {
			auto cur_bb = builder->GetInsertBlock();
			for (size_t i = active_breaks.size(); i > newer_than_break;) {
				auto& brk = active_breaks[--i];
				builder->SetInsertPoint(brk.bb);
				dispose_val_in_current_bb(val, is_local);
				brk.bb = builder->GetInsertBlock();
			}
			builder->SetInsertPoint(cur_bb);
		}
	}
	void dispose_val(Val& val, size_t newer_than_break, bool is_local = true) {
		dispose_val_in_current_bb(val, is_local);
		dispose_in_active_breaks(val, newer_than_break, is_local);
	}

	void comp_to_void(own<ast::Action>& action) {
		auto val = compile(action);
		dispose_val(val, active_breaks.size());
	}

	void remove_branches(Val& val) {
		if (!val.optional_br)
			return;
		auto val_bb = builder->GetInsertBlock();
		auto common_bb = llvm::BasicBlock::Create(*context, "", current_ll_fn);
		auto type = dom::strict_cast<ast::TpOptional>(val.type);
		auto wrapped_val = val.optional_br->is_wrapped
			? val.data
			: make_opt_val(val.data, type);
		builder->CreateBr(common_bb);
		builder->SetInsertPoint(common_bb);
		auto phi = builder->CreatePHI(to_llvm_type(*val.type), 2);
		phi->addIncoming(wrapped_val, val_bb);
		for (pin<OptBranch> b = val.optional_br;
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

	// Make value be able to outlive any random field and var assignment. Makes Static, Retained or a var-bounded Temp.
	void persist_val(Val& val, llvm::Value* maybe_own_parent = nullptr, bool retain_mutable_locals = true) {
		persist_rfield(val, maybe_own_parent);
		if (auto as_temp = get_if<Val::Temp>(&val.lifetime)) {
			if (!as_temp->var || (as_temp->var->is_mutable && retain_mutable_locals)) {
				build_retain(val.data, val.type, maybe_own_parent);
				val.lifetime = Val::Retained{};
			}
		}
		remove_branches(val);
	}
	void make_retained_or_non_ptr(Val& src, llvm::Value* maybe_own_parent = nullptr) {
		if (maybe_own_parent && get_if<Val::Retained>(&src.lifetime)) {
			auto& type = src.type;
			if (auto as_opt = dom::strict_cast<ast::TpOptional>(src.type))
				type = as_opt->wrapped;
			if (isa<ast::TpOwn>(*type))
				builder->CreateCall(fn_set_parent, { cast_to(src.data, ptr_type), maybe_own_parent });
		}
		persist_val(src, maybe_own_parent);
		if (get_if<Val::Temp>(&src.lifetime)) {
			build_retain(src.data, src.type, maybe_own_parent);
			src.lifetime = Val::Retained{};
		}
	}

	// Make value be able to outlive any random field and var assignment.
	[[nodiscard]] Val comp_to_persistent(own<ast::Action>& action, bool retain_mutable_locals = true) {
		auto r = compile(action);
		persist_val(
			r,
			nullptr,  // maybe_own_parent. Null, because comp_to_persistent never used in set_field value
			retain_mutable_locals);
		return r;
	}

	llvm::Value* cast_to(llvm::Value* value, llvm::Type* expected) {
		return value->getType() == expected
			? value
			: builder->CreateBitOrPointerCast(value, expected);
	}
	llvm::Value* comp_non_ptr(own<ast::Action>& action) {
		auto r = compile(action);
		assert(get_if<Val::Static>(&r.lifetime));
		return r.data;
	}
	void on_const_i32(ast::ConstInt32& node) override { result->data = builder->getInt32(node.value); }
	void on_const_i64(ast::ConstInt64& node) override { result->data = builder->getInt64(node.value); }
	void on_const_enum_tag(ast::ConstEnumTag& node) override { result->data = builder->getInt64(node.value->val); }
	void on_const_float(ast::ConstFloat& node) override { result->data = llvm::ConstantFP::get(float_type, node.value); }
	void on_const_double(ast::ConstDouble& node) override { result->data = llvm::ConstantFP::get(double_type, node.value); }
	void on_const_void(ast::ConstVoid&) override { result->data = llvm::UndefValue::get(void_type); }
	void on_const_bool(ast::ConstBool& node) override { result->data = builder->getInt1(node.value); }
	void on_const_string(ast::ConstString& node) override {
		auto& str = string_literals[node.value];
		if (!str) {
			auto str_name = ast::format_str("ag_str_", &node);
			auto& cls = classes.at(ast->string_cls);
			llvm::Constant* str_constant = llvm::ConstantDataArray::getString(*context, node.value);
			auto* str_type = llvm::StructType::get(obj_struct, str_constant->getType());
			module->getOrInsertGlobal(str_name, str_type);
			str = module->getGlobalVariable(str_name);
			vector<llvm::Constant*> obj_fields = {
				llvm::ConstantExpr::getBitCast(cls.dispatcher, ptr_type),
				const_ctr_str_literal, // shared|mt|hash|0
				llvm::ConstantInt::get(tp_int_ptr, ag_getStringHash(node.value.c_str()) | 1) };    // parent/weak/hash field
			str->setInitializer(
				llvm::ConstantStruct::get(str_type, {
					llvm::ConstantStruct::get(obj_struct, obj_fields),
					str_constant
				}));
			str->setLinkage(llvm::GlobalValue::InternalLinkage);
		}
		result->data = str;
		result->lifetime = Val::Static{};
	}

	unordered_map<ast::Type*, llvm::DISubroutineType*> di_fn_types;

	llvm::DISubroutineType* to_di_fn_type(ast::Type& tp) {
		auto& r = di_fn_types[&tp];
		if (!r && (isa<ast::TpFunction>(tp) || isa < ast::TpLambda>(tp) || isa<ast::TpDelegate>(tp))) {
			auto type = (ast::TpFunction*) &tp;
			vector<llvm::Metadata*> params;
			params.reserve(type->params.size());
			params.push_back(to_di_type(*type->params.back()));
			for (auto pi = type->params.begin(), pit = type->params.end() - 1; pi != pit; ++pi) {
				params.push_back(to_di_type(**pi));
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
			llvm::DIType* to_class_ptr(ast::TpOwn& type) {
				auto cls = type.target->get_implementation();
				return cls->is_interface
					? gen->di_obj_ptr
					: gen->classes.at(cls).di_ptr;
			}
			void on_int32(ast::TpInt32& type) override { result = gen->di_int32; }
			void on_int64(ast::TpInt64& type) override { result = gen->di_int; }
			void on_float(ast::TpFloat& type) override { result = gen->di_float; }
			void on_double(ast::TpDouble& type) override { result = gen->di_double; }
			void on_function(ast::TpFunction& type) override { result = gen->di_fn_ptr; }   // todo: raw ptr
			void on_lambda(ast::TpLambda& type) override { result = gen->di_lambda; }  // todo: ptr to { ptr to capture, raw ptr }
			void on_cold_lambda(ast::TpColdLambda& type) override { result = gen->di_builder->createBasicType("void", 64, llvm::dwarf::DW_ATE_unsigned); }
			void on_void(ast::TpVoid&) override { result = gen->di_void; }
			void on_optional(ast::TpOptional& type) override {
				if (isa<ast::TpInt64>(*type.wrapped)) result = gen->di_opt_int;
				else if (isa<ast::TpVoid>(*type.wrapped)) result = gen->di_byte;
				else type.wrapped->match(*this);
			}
			void on_own(ast::TpOwn& type) override { result = to_class_ptr(type); }
			void on_ref(ast::TpRef& type) override { result = to_class_ptr(type); }
			void on_shared(ast::TpShared& type) override { result = to_class_ptr(type); }
			void on_weak(ast::TpWeak& type) override { result = gen->di_weak_ptr; }
			void on_frozen_weak(ast::TpFrozenWeak& type) override { result = gen->di_weak_ptr; }
			void on_conform_ref(ast::TpConformRef& type) override { result = to_class_ptr(type); }
			void on_conform_weak(ast::TpConformWeak& type) override { result = gen->di_weak_ptr; }
			void on_delegate(ast::TpDelegate& type) override { result = gen->di_delegate; }
			void on_enum(ast::TpEnum& type) override { result = gen->di_int; }
			void on_no_ret(ast::TpNoRet& type) override { result = gen->di_void; }
		};
		DiTypeMatcher matcher(this);
		tp.match(matcher);
		return matcher.result;
	}

	void insert_di_var(ast::Module* module, string name, int line, int pos, llvm::DIType* type, llvm::Value* data_addr) {
		if (!di_builder)
			return;
		if (name == "this")
			name = "this_";
		di_builder->insertDeclare(
			data_addr,
			di_builder->createAutoVariable(
				current_di_scope,
				module
					? ast::format_str(module->name, "_", name)
					: name,
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
			insert_di_var(nullptr, p->name, p->line, p->pos, to_di_type(*p->type), data_addr);
	}

	pin<ast::Type> get_fn_result_t(pin<ast::TpFunction> fn_type) {
		auto fn_result = fn_type->params.back();
		if (!fn_type->can_x_break)
			return fn_result;
		if (isa<ast::TpNoRet>(*fn_result))
			return ast->tp_void();
		return ast->tp_optional(fn_result);
	}

	void compile_fn_body(ast::MkLambda& node, string name,
		llvm::Type* closure_struct = nullptr,
		bool handle_consts = false,
		bool handle_threads = false)
	{
		vector<BreakTrace> prev_breaks = move(active_breaks);
		active_breaks.clear();
		unordered_map<weak<ast::Var>, llvm::Value*> outer_locals;
		unordered_map<weak<ast::Var>, int> outer_capture_offsets = capture_offsets;
		swap(outer_locals, locals);
		vector<llvm::Value*> prev_capture_ptrs;
		swap(capture_ptrs, prev_capture_ptrs);
		auto prev_builder = builder;
		llvm::IRBuilder fn_bulder(llvm::BasicBlock::Create(*context, "", current_ll_fn));
		this->builder = &fn_bulder;
		auto prev_fn = current_function;
		current_function = &node;
		llvm::DIScope* prev_di_scope = current_di_scope;
		if (node.module) {
			if (auto di_file = di_files[node.module->name]) {
				auto sub = di_builder->createFunction(
					di_cu,
					name,
					name,  // linkage name
					di_file,
					node.line,
					to_di_fn_type(*node.type()),
					node.line,
					llvm::DINode::FlagPrototyped,
					llvm::DISubprogram::SPFlagDefinition);
				current_ll_fn->setSubprogram(sub);
				current_di_scope = sub;
			}
		}
		bool has_parent_capture_ptr = isa<ast::MkLambda>(node) && closure_struct != nullptr;
		pin<ast::Var> this_source = isa<ast::Method>(node) ? node.names.front().pinned() : nullptr;
		auto prev_capture_di_type = current_capture_di_type;
		llvm::AllocaInst* capture = nullptr;
		if (!node.captured_locals.empty()) {
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
			if (di_builder) {
				auto capture_struct = captures.back().second;
				auto capture_layout = layout.getStructLayout(capture_struct);
				vector<llvm::Metadata*> di_captures;
				size_t i = 0;
				if (has_parent_capture_ptr) {
					di_captures.push_back(di_builder->createMemberType(
						current_di_scope,
						"parent",
						nullptr,
						0, // line
						layout.getTypeSizeInBits(capture_struct->getElementType(i)),
						0, // align
						capture_layout->getElementOffsetInBits(i),
						llvm::DINode::DIFlags::FlagZero,
						di_builder->createPointerType(current_capture_di_type, 64)));
					i++;
				}
				for (auto& p : node.captured_locals) {
					di_captures.push_back(di_builder->createMemberType(
						nullptr,
						p->name == "this" ? "this_" : p->name,
						nullptr,
						0, // line
						layout.getTypeSizeInBits(capture_struct->getElementType(i)),
						0, // align
						capture_layout->getElementOffsetInBits(i),
						llvm::DINode::DIFlags::FlagZero,
						to_di_type(*p->type)));
					i++;
				}
				current_capture_di_type = di_builder->createStructType(
					nullptr,
					ast::format_str("cap_", name),
					nullptr,
					node.line,
					layout.getTypeSizeInBits(capture_struct),
					0,  // align
					llvm::DINode::DIFlags::FlagZero,
					nullptr,  // parent
					di_builder->getOrCreateArray(move(di_captures)));
			}
			capture = builder->CreateAlloca(captures.back().second);
			capture_ptrs.push_back(capture);
			if (has_parent_capture_ptr) {
				builder->CreateStore(&*current_ll_fn->arg_begin(), builder->CreateStructGEP(captures.back().second, capture, 0));
			}
			if (di_builder) {
				insert_di_var(nullptr, "closure", node.line, node.pos, current_capture_di_type, capture);
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
			auto param_iter = current_ll_fn->arg_begin();
			if (isa<ast::MkLambda>(node)) {
				if (closure_struct != nullptr) {
					auto dbg_param_val = builder->CreateAlloca(ptr_type);
					insert_di_var(nullptr, "parent_closure", node.line, node.pos, di_builder->createPointerType(prev_capture_di_type, 64), dbg_param_val);
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
			capture_ptrs.push_back(&*current_ll_fn->arg_begin());
		auto param_iter = current_ll_fn->arg_begin() + (isa<ast::MkLambda>(node) ? 1 : 0);
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
		vector<Val> consts_to_dispose; // addr of const, todo: optimize with const pass
		if (handle_consts) {
			builder->CreateCall(fn_init, {});
			for (auto& m : ast->modules_in_order) {
				for (auto& c : m->constants) {
					auto addr = globals[c.second];
					auto val = compile(c.second->initializer);
					make_retained_or_non_ptr(val);
					builder->CreateStore(val.data, addr);
					if (is_ptr(val.type) && !get_if<Val::Static>(&val.lifetime)) {
						builder->CreateStore(
							const_ctr_static,
							builder->CreateStructGEP(
								obj_struct,
								cast_to(val.data, ptr_type),
								1));
						val.data = addr;
						consts_to_dispose.push_back(move(val));
					}
				}
			}
		}
		auto prev_bb_for_captures = bb_for_captures;
		llvm::BasicBlock* body_bb = builder->GetInsertBlock();
		if (has_parent_capture_ptr) {
			bb_for_captures = body_bb;
			body_bb = llvm::BasicBlock::Create(*context, "", current_ll_fn);
			builder->SetInsertPoint(body_bb);
		} else {
			bb_for_captures = nullptr;
		}
		for (auto& a : node.body) {
			if (a != node.body.back())
				comp_to_void(a);
		}
		auto fn_type = cast<ast::TpLambda>(node.type());
		auto result_type = get_fn_result_t(fn_type);
		auto fn_result = compile(node.body.back());
		persist_rfield(fn_result);
		if (fn_type->can_x_break) {
			if (auto as_opt = dom::strict_cast<ast::TpOptional>(result_type))
				fn_result.data = make_opt_val(fn_result.data, as_opt);
			else
				fn_result.data = nullptr;
			fn_result.type = result_type;
			fn_result.lifetime = Val::Static{};
		}
		if (isa<ast::TpVoid>(*result_type)) {
			fn_result.data = nullptr;
		}
		auto release_params = [&](Val& result) {
			auto result_as_temp = get_if<Val::Temp>(&result.lifetime); // null if not temp
			auto make_result_retained = [&](bool actual_retain = true) {
				if (actual_retain)
					build_retain(result.data, result.type);
				result.lifetime = Val::Retained{};
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
			if (get_if<Val::Temp>(&result.lifetime)) {  // if connected to outer local/param
				make_result_retained();
			}
		};
		release_params(fn_result);
		auto dispose_consts = [&] {
			if (handle_consts) {
				for (; !consts_to_dispose.empty(); consts_to_dispose.pop_back()) {
					builder->CreateCall(fn_dispose, {
						builder->CreateLoad(ptr_type, consts_to_dispose.back().data)
					});
				}
			}
		};
		if (handle_threads) {
			auto main_ret_val = builder->CreateCall(fn_handle_main_thread, {});
			dispose_consts();
			builder->CreateRet(main_ret_val);
		} else {
			dispose_consts();
			if (fn_result.data) {
				builder->CreateRet(fn_result.data);
			} else {
				builder->CreateRetVoid();
			}
		}
		if (!active_breaks.empty()) {
			for (auto& brk : active_breaks) {
				builder->SetInsertPoint(brk.bb);
				assert(brk.target == current_function);
				if (brk.result.type != fn_result.type) { // it might be a local break to can-break fn
					auto opt_t = ast->tp_optional(brk.result.type);
					brk.result.type = opt_t;
					assert(brk.result.type == fn_result.type);
					brk.result.data = make_opt_val(brk.result.data, opt_t);
				}
				release_params(brk.result);
				if (fn_result.data)
					builder->CreateRet(brk.result.data);
				else
					builder->CreateRetVoid();
			}
			active_breaks.clear();
		}
		if (bb_for_captures) {
			builder->SetInsertPoint(bb_for_captures);
			builder->CreateBr(body_bb);
		}
		bb_for_captures = prev_bb_for_captures;
		active_breaks = move(prev_breaks);
		current_di_scope = prev_di_scope;
		current_function = prev_fn;
		current_capture_di_type = prev_capture_di_type;
		builder = prev_builder;
		if (!captures.empty() && captures.back().first == (int) node.lexical_depth)
			captures.pop_back();
		locals = move(outer_locals);
		capture_offsets = move(outer_capture_offsets);
		capture_ptrs = move(prev_capture_ptrs);
	}

	// `closure_ptr_type` define the type of `this` or `closure` parameter.
	llvm::Function* compile_function(ast::MkLambda& node, string name, llvm::Type* closure_ptr_type, bool is_external) {
		if (auto seen = compiled_functions[&node])
			return seen;
		llvm::Function* prev = current_ll_fn;
		current_ll_fn = llvm::Function::Create(
			lambda_to_llvm_fn(node, node.type()),
			is_external
				? llvm::Function::ExternalLinkage
				: llvm::Function::InternalLinkage,
			name,
			module.get());
		if (!is_external) {
			compile_fn_body(node, name, closure_ptr_type);
		}
		swap(prev, current_ll_fn);
		compiled_functions[&node] = prev;
		return prev;
	}

	void on_mk_lambda(ast::MkLambda& node) override {
		llvm::Function* function = compile_function(
			node,
			ast::format_str("L_", node.module->name, '_', node.line, '_', node.pos),
			captures.empty()
				? nullptr
				: captures.back().second->getPointerTo(),
			false);  // is_external
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
		size_t initial_acive_breaks = active_breaks.size();
		auto prev_di_scope = current_di_scope;
		current_di_scope = current_di_scope
			? di_builder->createLexicalBlock(current_di_scope, current_di_scope->getFile(), node.line, node.pos)
			: nullptr;
		vector<pair<
			Val,                  // mutable ? addr : value
			size_t>> to_dispose;  // active_breaks marker
		for (auto& l : node.names) {
			to_dispose.push_back({
				l->initializer ? comp_to_persistent(l->initializer) :
				parameter.data ? parameter :
				Val{
					l->type,
					make_opt_none(cast<ast::TpOptional>(l->type)),
					is_ptr(l->type)
						? Val::lifetime_t(Val::Retained{})
						: Val::lifetime_t(Val::Static{})
				},
				0 });
			to_dispose.back().second = active_breaks.size();
			auto& initializer = to_dispose.back().first;
			if (l->is_mutable || l->captured) {
				auto& addr = locals[l];
				if (l->is_mutable)
					make_retained_or_non_ptr(initializer);
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
				} else if (l->type && l->type != ast->tp_void()) {
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
		auto dispose_block_params = [&](Val& result, size_t break_mark) {
			auto result_as_temp = get_if<Val::Temp>(&result.lifetime);
			weak<ast::Var> temp_var = result_as_temp ? result_as_temp->var : nullptr;
			auto val_iter = to_dispose.begin();
			for (auto& p : node.names) {
				if (val_iter->second <= break_mark) {
					if (temp_var == p) { // result is locked by the dying temp ptr.
						if (!get_if<Val::Retained>(&val_iter->first.lifetime))
							build_retain(result.data, p->type);
						result.type = node.type();  // revert own->pin cohersion
						result.lifetime = Val::Retained{};
						temp_var = nullptr;
					} else if (is_ptr(p->type)) {
						if (p->is_mutable) {
							build_release(builder->CreateLoad(to_llvm_type(*p->type), val_iter->first.data), p->type);
						} else {
							dispose_val_in_current_bb(val_iter->first);
						}
					}
				}
				val_iter++;
			}
			return temp_var; // return out-of-block temp-var to which this block result by this branch is bounded
		};
		weak<ast::Var> result_temp_var;
		if (!dom::isa<ast::TpNoRet>(*r.type)) {
			persist_rfield(r);
			result_temp_var = dispose_block_params(r, initial_acive_breaks);
		}
		auto body_bb = builder->GetInsertBlock();
		size_t breaks_count = 0;
		for (size_t i = active_breaks.size(); i;) {
			auto& brk = active_breaks[--i];
			builder->SetInsertPoint(brk.bb);
			auto brk_temp_var = dispose_block_params(brk.result, i);
			brk.bb = builder->GetInsertBlock();
			if (brk.target == &node) {
				breaks_count++;
				if (result_temp_var != brk_temp_var)
					result_temp_var = nullptr;  // different vars, must be returned as retained
			}
		}
		builder->SetInsertPoint(body_bb);
		if (!result_temp_var)
			make_retained_or_non_ptr(r);
		if (breaks_count > 0) {
			auto exit_bb = llvm::BasicBlock::Create(*context, "", current_ll_fn);
			if (!dom::isa<ast::TpNoRet>(*r.type))
				builder->CreateBr(exit_bb);
			auto r_bb = builder->GetInsertBlock();
			builder->SetInsertPoint(exit_bb);
			auto phi = builder->CreatePHI(to_llvm_type(*node.type()), breaks_count + 1);
			if (!dom::isa<ast::TpNoRet>(*r.type)) {
				phi->addIncoming(r.data, r_bb);
			}
			for (auto it = active_breaks.begin(); it != active_breaks.end();) {
				auto& brk = *it;
				if (brk.target != &node) {
					++it;
					continue;
				}
				builder->SetInsertPoint(brk.bb);
				if (!result_temp_var)
					make_retained_or_non_ptr(brk.result);
				builder->CreateBr(exit_bb);
				phi->addIncoming(brk.result.data, builder->GetInsertBlock());
				it = active_breaks.erase(it);
			}
			r.data = phi;
			r.type = node.type();
			builder->SetInsertPoint(exit_bb);
		}
		current_di_scope = prev_di_scope;
		return r;
	}

	void on_block(ast::Block& node) override {
		*result = handle_block(node, Val{});
	}
	void on_break(ast::Break& node) override {
		auto r = compile(node.result);
		auto target = node.block;
		if (node.lexical_depth != node.block->lexical_depth) {
			make_retained_or_non_ptr(r);
			auto t = ast->tp_optional(r.type);
			r.type = t;
			r.data = make_opt_val(r.data, t);
			builder->CreateStore(r.data, get_data_ref(node.block->names[0]));
			r.type = get_fn_result_t(cast<ast::TpLambda>(current_function->type()));
			if (auto as_opt = dom::strict_cast<ast::TpOptional>(r.type)) {
				r.data = make_opt_none(as_opt);
			} else {
				r.data = llvm::UndefValue::get(void_type);
			}
			target = current_function;
		}
		active_breaks.push_back({ builder->GetInsertBlock(), move(r), target, &node });
		builder->SetInsertPoint((llvm::BasicBlock*)nullptr);
		result->type = ast->tp_no_ret();
	}
	void on_make_delegate(ast::MakeDelegate& node) override {
		auto base = compile(node.base);
		auto method = node.method->base.pinned();
		auto m_ordinal = methods.at(method).ordinal;
		auto build_non_null_pin_to_entry_point_code = [&] (llvm::Value* base_pin) {
			auto disp = builder->CreateLoad(ptr_type, builder->CreateConstGEP2_32(obj_struct, base_pin, AG_HEADER_OFFSET, 0));
			return method->cls->is_interface
				? (llvm::Value*)builder->CreateCall(
					llvm::FunctionCallee(dispatcher_fn_type, disp),
					{ builder->getInt64(classes.at(method->cls).interface_ordinal | m_ordinal) })
				: (llvm::Value*)builder->CreateLoad(
					ptr_type,
					builder->CreateConstGEP2_32(classes.at(method->cls).vmt, disp, -1, m_ordinal));
		};
		result->data = builder->CreateInsertValue(
			builder->CreateInsertValue(
				llvm::UndefValue::get(delegate_struct),
				builder->CreateCall(fn_mk_weak, { base.data }),
				{ 0 }),
			build_non_null_pin_to_entry_point_code(base.data),
			{ 1 });
		dispose_val(base, active_breaks.size());
		result->lifetime = Val::Retained{};
	}

	void on_immediate_delegate(ast::ImmediateDelegate& node) override {
		if (dom::isa<ast::MkInstance>(*node.base)) {
			result->data = builder->CreateInsertValue(
				builder->CreateInsertValue(
					llvm::UndefValue::get(delegate_struct),
					null_weak,
					{ 0 }),
				null_weak,
				{ 1 });
			return;
		}
		auto dl_fn = llvm::Function::Create(
			lambda_to_llvm_fn(node, node.type()),
			llvm::Function::InternalLinkage,
			ast::format_str("ag_dl_", node.module->name, "_", node.name),
			module.get());
		execute_in_global_scope.push_back([&, dl_fn] {
			auto prev_fn = current_ll_fn;
			current_ll_fn = dl_fn;
			compile_fn_body(node, ast::format_str("ag_dl_", node.module->name, "_", node.name));
			current_ll_fn = prev_fn;
		});
		auto base = compile(node.base);
		llvm::Value* base_weak_val = nullptr;
		if (dom::isa<ast::TpWeak>(*base.type)) {
			make_retained_or_non_ptr(base);
			base_weak_val = base.data;
		} else {
			base_weak_val = builder->CreateCall(fn_mk_weak, { base.data });
			dispose_val(base, active_breaks.size());
		}
		result->data = builder->CreateInsertValue(
			builder->CreateInsertValue(
				llvm::UndefValue::get(delegate_struct),
				base_weak_val,
				{ 0 }),
			dl_fn,
			{ 1 });
		result->lifetime = Val::Retained{};
	}

	void on_make_fn_ptr(ast::MakeFnPtr& node) override {
		result->data = functions.at(node.fn);
	}

	void on_call(ast::Call& node) override {
		vector<llvm::Value*> params;
		vector<pair<Val, size_t>> to_dispose; // val and active_breaks_mark at the moment val is succeeded
		Val last_param_to_return;
		auto reg_disposable = [&](pin<ast::Action> src, Val&& val) {
			if (node.returns_last_param_if_void && src == node.params.back().pinned())
				last_param_to_return = val;
			to_dispose.push_back({ move(val), active_breaks.size() });
			return to_dispose.back().first;
		};
		if (auto calle_as_method = dom::strict_cast<ast::MakeDelegate>(node.callee)) {
			params.push_back(nullptr);
			auto method = calle_as_method->method->base.pinned();
			auto& m_info = methods.at(method);
			auto pt = m_info.type->params().begin() + 1;
			for (auto& p : node.params) {
				params.push_back(cast_to(
					reg_disposable(p, comp_to_persistent(p)).data,
					*pt++));
			}
			llvm::Value* receiver;
			if (method->is_factory) {
				*result = comp_to_persistent(
					calle_as_method->base,
					false);
				receiver = result->data;
			} else {
				receiver = reg_disposable(
					calle_as_method->base,
					comp_to_persistent(
					calle_as_method->base,
					false) // perist_mutable_locals
				).data;
			}
			params.front() = cast_to(receiver, ptr_type);
			if (method->cls->is_interface) {
				auto entry_point = builder->CreateCall(
					llvm::FunctionCallee(
						dispatcher_fn_type,
						builder->CreateLoad(ptr_type, builder->CreateConstGEP2_32(obj_struct, receiver, AG_HEADER_OFFSET, 0))
					),
					{ builder->getInt64(classes.at(method->cls).interface_ordinal | m_info.ordinal) });
				result->data = builder->CreateCall(
					llvm::FunctionCallee(m_info.type, entry_point),
					move(params));
			} else {
				auto res = builder->CreateCall(
					llvm::FunctionCallee(
						m_info.type,
						builder->CreateLoad(  // load ptr to fn
							ptr_type,
							builder->CreateConstGEP2_32(
								classes.at(method->cls).vmt,
								builder->CreateLoad(ptr_type, builder->CreateConstGEP2_32(obj_struct, receiver, AG_HEADER_OFFSET, 0)),
								-1,
								m_info.ordinal))),
					move(params));
				if (!method->is_factory)
					result->data = res;
			}
		} else if (auto as_delegate_type = dom::strict_cast<ast::TpDelegate>(node.callee->type())) {
			auto result_type = dom::strict_cast<ast::TpOptional>(node.type());
			reg_disposable(node.callee, compile(node.callee));
			llvm::Value* retained_receiver_pin = builder->CreateCall(fn_deref_weak,
				{ builder->CreateExtractValue(to_dispose.front().first.data, {0}) });
			params.push_back(cast_to(retained_receiver_pin, ptr_type)); 
			auto pt = as_delegate_type->params.begin();
			for (auto& p : node.params) {
				params.push_back(cast_to(
					reg_disposable(p, comp_to_persistent(p)).data,
					to_llvm_type(**(pt++))));
			}
			*result = compile_if(
				*result_type,
				builder->CreateCmp(llvm::CmpInst::Predicate::ICMP_NE,
					retained_receiver_pin,
					llvm::ConstantInt::get(tp_int_ptr, 0)),
				[&] {
					Val r{
						result_type,
						make_opt_val(
							builder->CreateCall(
								llvm::FunctionCallee(
									lambda_to_llvm_fn(*node.callee, node.callee->type()),
									builder->CreateExtractValue(to_dispose.front().first.data, {1})),
								move(params)),
							result_type),
						Val::Static{} };
					build_release_ptr_not_null(cast_to(retained_receiver_pin, ptr_type));
					return r;
				},
				[&] {
					return Val{ result_type, make_opt_none(result_type), Val::Static{}};
				});
		} else {
			bool is_fn = isa<ast::TpFunction>(*node.callee->type());
			if (!is_fn)
				params.push_back(nullptr);
			auto function_type = is_fn				
					? function_to_llvm_fn(*node.callee, node.callee->type())
					: lambda_to_llvm_fn(*node.callee, node.callee->type());
			auto pt = function_type->params().begin() + (is_fn ? 0 : 1);
			for (auto& p : node.params) {
				params.push_back(cast_to(
					reg_disposable(p, comp_to_persistent(p)).data,
					*pt++));
			}
			auto callee = compile(node.callee);
			assert(get_if<Val::Static>(&callee.lifetime));
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
		if (is_ptr(node.type()) && get_if<Val::Static>(&result->lifetime))
			result->lifetime = Val::Retained{};
		auto dispose_params = [&] {
			for (; !to_dispose.empty(); to_dispose.pop_back()) {
				if (to_dispose.back().first.data == last_param_to_return.data) {
					*result = move(last_param_to_return);
				}
				else {
					dispose_val(
						to_dispose.back().first,
						to_dispose.back().second,
						true);
				}
			}
		};
		auto callee = cast<ast::TpLambda>(node.callee->type());
		if (!callee->can_x_break) {
			dispose_params();
			return;
		}
		unordered_set<pin<ast::Block>> x_targets;
		bool has_outer_break = false;
		for (auto& l : node.activates_lambdas) {
			if (!l) {
				has_outer_break = true;
				continue;
			}
			for (auto& b : l->x_breaks) {
				if (current_function->lexical_depth == b->block->lexical_depth) {
					x_targets.insert(b->block);
				} else {
					assert(current_function->lexical_depth > b->block->lexical_depth);
					has_outer_break = true;
				}
			}
		}
		auto call_result_t = dom::isa<ast::TpNoRet>(*node.type())
			? nullptr
			: ast->tp_optional(node.type());
		if (x_targets.empty() && !has_outer_break) { // calle is-a or takes lambdas, but this call don't break
			if (call_result_t) {
				dispose_params();
				result->data = extract_opt_val(result->data, call_result_t);
				result->type = ast->get_wrapped(call_result_t);
			} else {
				builder->CreateUnreachable();
			}
			return;
		}
		dispose_params();
		auto norm_bb = !call_result_t // noret
			? nullptr
			: llvm::BasicBlock::Create(*context, "", current_ll_fn);
		if (norm_bb) {
			auto break_bb = llvm::BasicBlock::Create(*context, "", current_ll_fn);
			builder->CreateCondBr(
				check_opt_has_val(result->data, call_result_t),
				norm_bb,
				break_bb);
			builder->SetInsertPoint(break_bb);
		}
		size_t destinations_left = x_targets.size() + (has_outer_break ? 1 : 0);
		for (auto& b : x_targets) {
			auto& var = b->names.front();
			pin<ast::TpOptional> opt_t = cast<ast::TpOptional>(var->type);
			auto val = remove_indirection(*var, get_data_ref(var.weaked()));
			auto add_active_break = [&] {
				active_breaks.push_back({
					builder->GetInsertBlock(),
					Val{
						opt_t->wrapped,
						extract_opt_val(val, opt_t),
						Val::Temp{ var }
					},
					b,
					&node });
			};
			if (destinations_left > 1) {
				auto this_bb = llvm::BasicBlock::Create(*context, "", current_ll_fn);
				auto not_this_bb = llvm::BasicBlock::Create(*context, "", current_ll_fn);
				builder->CreateCondBr(
					check_opt_has_val(val, opt_t),
					this_bb,
					not_this_bb);
				builder->SetInsertPoint(this_bb);
				add_active_break();
				builder->SetInsertPoint(not_this_bb);
			} else {
				add_active_break();
			}
			--destinations_left;
		}
		if (has_outer_break) {
			assert(destinations_left == 1);
			auto fn_result_type = get_fn_result_t(cast<ast::TpLambda>(current_function->type()));
			active_breaks.push_back({
				builder->GetInsertBlock(),
				Val{
					fn_result_type,
					isa<ast::TpOptional>(*fn_result_type)
						? make_opt_none(cast<ast::TpOptional>(fn_result_type))
						: llvm::UndefValue::get(void_type),
					Val::Static{}
				},
				current_function,
				&node });
		}
		if (norm_bb) {
			builder->SetInsertPoint(norm_bb);
			result->data = extract_opt_val(result->data, call_result_t);
			result->type = ast->get_wrapped(call_result_t);
		}
	}

	llvm::Function* build_trampoline(pin<ast::TpDelegate> type, size_t& params_size_out) {
		auto& tramp = trampolines[type];
		if (tramp.first) {
			params_size_out = tramp.second;
			return tramp.first;
		}
		tramp.first = llvm::Function::Create(
			trampoline_fn_type,
			llvm::Function::InternalLinkage,
			ast::format_str("ag_tr_", (void*)type),
			module.get());
		llvm::Function* prev = current_ll_fn;
		current_ll_fn = tramp.first;
		auto prev_builder = builder;
		llvm::IRBuilder fn_bulder(llvm::BasicBlock::Create(*context, "", current_ll_fn));
		this->builder = &fn_bulder;
		auto self = tramp.first->arg_begin();
		auto entry_point = tramp.first->arg_begin() + 1;
		auto thread = tramp.first->arg_begin() + 2;
		vector<llvm::Value*> params{ self };
		for (auto pti = type->params.begin(), ptt = type->params.end() - 1; pti != ptt; ++pti) {
			struct TypeDeserializer : ast::TypeMatcher {
				vector<llvm::Value*>& params;
				Generator& gen;
				llvm::Value* thread;
				size_t& params_size_out;
				TypeDeserializer(vector<llvm::Value*>& params, Generator& gen, llvm::Value* thread, size_t& params_size_out)
					: params(params), gen(gen), thread(thread), params_size_out(params_size_out) {}
				void handle_64_bit(ast::Type& type) {
					params_size_out++;
					params.push_back(gen.cast_to(
						gen.builder->CreateCall(gen.fn_get_thread_param, { thread }),
						gen.to_llvm_type(type)));
				}
				void handle_32_bit(ast::Type& type) {
					params_size_out++;
					params.push_back(gen.cast_to(
						gen.builder->CreateTrunc(
							gen.builder->CreateCall(gen.fn_get_thread_param, { thread }),
							gen.int_type),
						gen.to_llvm_type(type)));
				}
				void handle_pair(ast::Type& type) {
					params_size_out += 2;
					auto result_type = gen.to_llvm_type(type);
					auto first = gen.cast_to(
						gen.builder->CreateCall(gen.fn_get_thread_param, { thread }),
						result_type->getStructElementType(0));
					auto second = gen.cast_to(
						gen.builder->CreateCall(gen.fn_get_thread_param, { thread }),
						result_type->getStructElementType(1));
					params.push_back(
						gen.builder->CreateInsertValue(
							gen.builder->CreateInsertValue(
								llvm::UndefValue::get(result_type),
								first,
								{ 0 }),
							second,
							{ 1 }));
				}
				void on_int64(ast::TpInt64& type) override { handle_64_bit(type); }
				void on_int32(ast::TpInt32& type) override { handle_32_bit(type); }
				void on_float(ast::TpFloat& type) override { handle_32_bit(type); }
				void on_double(ast::TpDouble& type) override { handle_64_bit(type); }
				void on_function(ast::TpFunction& type) override { handle_64_bit(type); }
				void on_lambda(ast::TpLambda& type) override { assert(false); }
				void on_delegate(ast::TpDelegate& type) override { handle_pair(type); }
				void on_cold_lambda(ast::TpColdLambda& type) override {}
				void on_void(ast::TpVoid& type) override {
					params_size_out++;
					params.push_back(llvm::UndefValue::get(gen.void_type));
				}
				void on_optional(ast::TpOptional& type) override {
					if (dom::isa<ast::TpInt64>(*type.wrapped)) {
						handle_pair(type);
					} else if (dom::isa<ast::TpVoid>(*type.wrapped)) {
						handle_64_bit(type);
					} else {
						type.wrapped->match(*this);
					}
				}
				void on_own(ast::TpOwn& type) override { handle_64_bit(type); }
				void on_ref(ast::TpRef& type) override { handle_64_bit(type); }
				void on_shared(ast::TpShared& type) override { handle_64_bit(type); }
				void on_weak(ast::TpWeak& type) override { handle_64_bit(type); }
				void on_frozen_weak(ast::TpFrozenWeak& type) override { handle_64_bit(type); }
				void on_conform_ref(ast::TpConformRef& type) override { handle_64_bit(type); }
				void on_conform_weak(ast::TpConformWeak& type) override { handle_64_bit(type); }
				void on_no_ret(ast::TpNoRet& type) override { assert(false); }
				void on_enum(ast::TpEnum& type) override { handle_64_bit(type); }
			};
			TypeDeserializer td(params, *this, thread, params_size_out);
			(*pti)->match(td);
		}
		builder->CreateCall(fn_unlock_thread_queue, { thread });
		auto bb_not_null = llvm::BasicBlock::Create(*context, "", current_ll_fn);
		auto bb_null = llvm::BasicBlock::Create(*context, "", current_ll_fn);
		builder->CreateCondBr(
			builder->CreateCmp(llvm::CmpInst::Predicate::ICMP_EQ, self, const_null_ptr),
			bb_null,
			bb_not_null);
		builder->SetInsertPoint(bb_not_null);
		vector<llvm::Type*> param_types;
		for (auto& p : params)
			param_types.push_back(p->getType());
		build_release(
			builder->CreateCall(
				llvm::FunctionCallee(
					llvm::FunctionType::get(
						to_llvm_type(*type->params.back()),
						move(param_types),
						false),  // isVararg
					entry_point),
				{ params }),
			type->params.back());
		builder->CreateBr(bb_null);
		builder->SetInsertPoint(bb_null);
		auto pti = type->params.begin();
		for (auto pi = params.begin() + 1; pi != params.end(); ++pi)
			build_release(*pi, *(pti++));
		builder->CreateRetVoid();
		swap(prev, current_ll_fn);
		builder = prev_builder;
		tramp.second = params_size_out;
		return tramp.first;
	}

	void on_async_call(ast::AsyncCall& node) override {
		Val callee = compile(node.callee);
		make_retained_or_non_ptr(callee);
		auto calle_type = dom::strict_cast<ast::TpDelegate>(callee.type);
		size_t params_size = 0;
		auto tramp = build_trampoline(calle_type, params_size);
		vector<Val> params;
		for (auto& p : node.params) {
			params.push_back(compile(p));
			make_retained_or_non_ptr(params.back());
		}
		llvm::Value* thread_ptr = builder->CreateCall( fn_prepare_post_message, {
			builder->CreateExtractValue(callee.data, { 0 }),
			builder->CreateExtractValue(callee.data, { 1 }),
			tramp,
			builder->getInt64(params_size) });
		for (auto& v : params) {
			struct TypeSerializer : ast::TypeMatcher {
				Generator& gen;
				llvm::Value* thread;
				llvm::Value* value;
				TypeSerializer(Generator& gen, llvm::Value* thread, llvm::Value* value) : gen(gen), thread(thread), value(value) {}
				void handle_64_bit() {
					gen.builder->CreateCall(gen.fn_put_thread_param, { gen.cast_to(value, gen.int_type) });
				}
				void handle_own() {
					gen.builder->CreateCall(gen.fn_put_thread_param_own_ptr, { thread, gen.cast_to(value, gen.ptr_type) });
				}
				void handle_weak() {
					gen.builder->CreateCall(gen.fn_put_thread_param_weak_ptr, { gen.cast_to(value, gen.ptr_type) });
				}
				void on_int32(ast::TpInt32& type) override {
					gen.builder->CreateCall(gen.fn_put_thread_param, {
						gen.builder->CreateZExt(value, gen.int_type)
					});
				}
				void on_int64(ast::TpInt64& type) override { handle_64_bit(); }
				void on_enum(ast::TpEnum& type) override { handle_64_bit(); }
				void on_float(ast::TpFloat& type) override {
					gen.builder->CreateCall(gen.fn_put_thread_param, {		
						gen.builder->CreateZExt(
							gen.cast_to(value, gen.int32_type),
							gen.int_type)
					});
				}
				void on_double(ast::TpDouble& type) override { handle_64_bit(); }
				void on_function(ast::TpFunction& type) override { handle_64_bit(); }
				void on_lambda(ast::TpLambda& type) override { assert(false); }
				void on_delegate(ast::TpDelegate& type) override {
					gen.builder->CreateCall(gen.fn_put_thread_param_weak_ptr, {
						gen.cast_to(gen.builder->CreateExtractValue(value, { 0 }), gen.ptr_type)
					});
					gen.builder->CreateCall(gen.fn_put_thread_param, {
						gen.cast_to(gen.builder->CreateExtractValue(value, { 1 }), gen.int_type)
					});
				}
				void on_cold_lambda(ast::TpColdLambda& type) override { assert(false); }
				void on_void(ast::TpVoid& type) override { assert(false); }
				void on_optional(ast::TpOptional& type) override {
					if (dom::isa<ast::TpInt64>(*type.wrapped)) {
						gen.builder->CreateCall(gen.fn_put_thread_param, {
							gen.cast_to(gen.builder->CreateExtractValue(value, { 0 }), gen.int_type)
						});
						gen.builder->CreateCall(gen.fn_put_thread_param, {
							gen.cast_to(gen.builder->CreateExtractValue(value, { 1 }), gen.int_type)
						});
					} else if (dom::isa<ast::TpVoid>(*type.wrapped)) {
						handle_64_bit();
					} else {
						type.wrapped->match(*this);
					}
				}
				void on_own(ast::TpOwn& type) override { handle_own(); }
				void on_ref(ast::TpRef& type) override { handle_own(); }
				void on_shared(ast::TpShared& type) override { handle_own(); }
				void on_weak(ast::TpWeak& type) override { handle_weak(); }
				void on_frozen_weak(ast::TpFrozenWeak& type) override { handle_weak(); }
				void on_conform_ref(ast::TpConformRef& type) override { handle_own(); }
				void on_conform_weak(ast::TpConformWeak& type) override { handle_weak(); }
				void on_no_ret(ast::TpNoRet& type) override { assert(false); }
			};
			TypeSerializer ts(*this, thread_ptr, v.data);
			v.type->match(ts);
		}
	}

	llvm::Value* get_data_ref(const weak<ast::Var>& var) {
		if (auto it = locals.find(var); it != locals.end())
			return it->second;
		if (auto it = globals.find(var); it != globals.end())
			return it->second;
		int ptr_index = 0;
		auto var_depth = var->lexical_depth;
		int d = int(captures.size()) - 1;
		for (;; d--) {
			assert(d >= 0);
			if (captures[d].first == (int) var_depth)
				break;
			if (++ptr_index >= (int)capture_ptrs.size()) {
				auto saved = builder->GetInsertBlock();
				builder->SetInsertPoint(bb_for_captures);
				capture_ptrs.push_back(
					builder->CreateLoad(ptr_type,
						builder->CreateStructGEP(captures[d].second, capture_ptrs.back(), { 0 })));
				builder->SetInsertPoint(saved);
			}
		}
		return locals[var] = builder->CreateStructGEP(
			captures[d].second,
			capture_ptrs[ptr_index],
			capture_offsets[var]);
	}
	void on_get(ast::Get& node) override {
		result->data = remove_indirection(*node.var.pinned(), get_data_ref(node.var));
		if (is_ptr(node.type()) && !node.var->is_const)
			result->lifetime = Val::Temp{ node.var };
	}
	void on_set(ast::Set& node) override {
		*result = compile(node.val);
		make_retained_or_non_ptr(*result);
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
		auto class_fields = classes.at(ast->extract_class(base.type)->get_implementation()).fields;
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
		} else { // leave lifetime = Val::Static
			dispose_val(base, active_breaks.size());
		}
	}
	void on_set_field(ast::SetField& node) override {
		auto class_fields = classes.at(ast->extract_class(node.base->type())->get_implementation()).fields;
		if (is_ptr(node.type())) {
			auto base = comp_to_persistent(node.base);
			size_t breaks_after_base = active_breaks.size();
			*result = compile(node.val);
			make_retained_or_non_ptr(*result, base.data);
			result->type = node.type();  // @T->T and base-dependent conversions
			auto addr = builder->CreateStructGEP(class_fields, base.data, node.field->offset);
			build_release(
				builder->CreateLoad(class_fields->getElementType(node.field->offset), addr),
				node.field->initializer->type(),
				false);  // not local, clear parent
			builder->CreateStore(result->data, addr);
			if (get_if<Val::Retained>(&base.lifetime)) {
				result->lifetime = Val::RField{ base.data };
			} else if (auto base_as_rfield = get_if<Val::RField>(&base.lifetime)) {
				result->lifetime = Val::RField{ base_as_rfield->to_release };
				dispose_in_active_breaks(base, breaks_after_base);
			} else {
				result->lifetime = Val::Temp{};
				dispose_val(base, breaks_after_base);
			}
		} else {
			*result = compile(node.val);
			make_retained_or_non_ptr(*result);
			auto base = compile(node.base);
			builder->CreateStore(
				result->data,
				builder->CreateStructGEP(
					class_fields,
					base.data,
					node.field->offset));
			dispose_val(base, active_breaks.size());
		}
	}
	void on_splice_field(ast::SpliceField& node) override {
		auto class_fields = classes.at(ast->extract_class(node.base->type())->get_implementation()).fields;
		auto base = comp_to_persistent(node.base);
		size_t breaks_after_base = active_breaks.size();
		auto val = compile(node.val);
		auto addr = builder->CreateStructGEP(class_fields, base.data, node.field->offset);
		result->data = builder->CreateCall(fn_splice, {
				cast_to(val.data, ptr_type),
				base.data,
				cast_to(addr, ptr_type) });
		dispose_val(val, active_breaks.size());
		dispose_val(base, breaks_after_base);
	}
	void on_mk_instance(ast::MkInstance& node) override {
		result->data = builder->CreateCall(classes.at(node.cls->get_implementation()).constructor, {});
		result->lifetime = Val::Retained{};
	}
	void on_to_int32(ast::ToInt32Op& node) override {
		auto& t = *node.p->type();
		auto param = comp_non_ptr(node.p);
		if (dom::isa<ast::TpInt64>(t)) {
			result->data = builder->CreateTrunc(param, int32_type);
		} else if (dom::isa<ast::TpDouble>(t) || dom::isa<ast::TpFloat>(t)) {
			result->data = builder->CreateFPToSI(param, int32_type);
		} else { // int32
			result->data = param;
		}
	}
	void on_to_int(ast::ToIntOp& node) override {
		auto& t = *node.p->type();
		auto param = comp_non_ptr(node.p);
		if (dom::isa<ast::TpInt32>(t)) {
			result->data = builder->CreateSExt(param, int_type);
		} else if (dom::isa<ast::TpDouble>(t) || dom::isa<ast::TpFloat>(t)) {
			result->data = builder->CreateFPToSI(param, int_type);
		} else { // int64
			result->data = param;
		}
	}
	void on_to_float(ast::ToFloatOp& node) override {
		auto& t = *node.p->type();
		auto param = comp_non_ptr(node.p);
		if (dom::isa<ast::TpInt32>(t) || dom::isa<ast::TpInt64>(t)) {
			result->data = builder->CreateSIToFP(param, float_type);
		} else if (dom::isa<ast::TpDouble>(t)) {
			result->data = builder->CreateFPTrunc(param, float_type);
		} else { // float
			result->data = param;
		}
	}
	void on_to_double(ast::ToDoubleOp& node) override {
		auto& t = *node.p->type();
		auto param = comp_non_ptr(node.p);
		if (dom::isa<ast::TpInt32>(t) || dom::isa<ast::TpInt64>(t)) {
			result->data = builder->CreateSIToFP(param, double_type);
		} else if (dom::isa<ast::TpFloat>(t)) {
			result->data = builder->CreateFPExt(param, double_type);
		} else { // double
			result->data = param;
		}
	}
	void on_not(ast::NotOp& node) override {
		Val param = compile(node.p);
		result->data = builder->CreateNot(
			check_opt_has_val(
				param.data,
				dom::strict_cast<ast::TpOptional>(node.p->type())));
		dispose_val(param, active_breaks.size());
	}
	void on_neg(ast::NegOp& node) override {
		if (is_integer(*node.p->type()))
			result->data = builder->CreateNeg(comp_non_ptr(node.p));
		else
			result->data = builder->CreateFNeg(comp_non_ptr(node.p));
	}
	void on_inv(ast::InvOp& node) override {
		result->data = builder->CreateNot(comp_non_ptr(node.p));
	}
	void on_ref(ast::RefOp& node) override {
		internal_error(node, "ref cannot be compiled");
	}
	void on_freeze(ast::FreezeOp& node) override {
		auto src = compile(node.p);
		result->data = builder->CreateCall(fn_freeze, { src.data });
		result->lifetime = Val::Retained{};
		dispose_val(src, active_breaks.size());
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
		auto cls = dom::strict_cast<ast::Class>(ast->extract_class(result_type->wrapped));
		if (!cls) node.error("internal error, in this iteration cast to inst or param is not supported");
		assert(cls); 
		*result = compile(node.p[0]);
		auto& cls_info = classes.at(cls);
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
				[&] { return Val{ result->type, make_opt_none(result_type), Val::Static{} }; });
			return;
		}
		auto vmt_ptr = builder->CreateLoad(ptr_type, builder->CreateConstGEP2_32(obj_struct, result->data, AG_HEADER_OFFSET, 0));
		//auto vmt_ptr_bb = builder->GetInsertBlock();
		*result = compile_if(
			*result_type,
			builder->CreateCmp(llvm::CmpInst::Predicate::ICMP_ULE,
				builder->getInt64(classes.at(cls).vmt_size),
				builder->CreateLoad(tp_int_ptr,
					builder->CreateConstGEP2_32(obj_vmt_struct, vmt_ptr, -1, AG_VMT_FIELD_VMT_SIZE))),
			[&] {
				return compile_if(
					*result_type,
					builder->CreateCmp(llvm::CmpInst::Predicate::ICMP_EQ,
						cls_info.dispatcher,
						builder->CreateLoad(ptr_type,
							builder->CreateConstGEP2_32(cls_info.vmt, vmt_ptr, -1, 0))),
					[&] { return Val{ result->type, make_opt_val(result->data, result_type), result->lifetime }; },
					[&] { return Val{ result->type, make_opt_none(result_type), Val::Static{} }; });
			},
			[&] { return Val{ result_type, make_opt_none(result_type), Val::Static{}}; });
	}
	bool is_integer(ast::Type& t) {
		return dom::isa<ast::TpInt32>(t) || dom::isa<ast::TpInt64>(t);
	}
	void on_add(ast::AddOp& node) override {
		auto lhs = comp_non_ptr(node.p[0]);
		auto rhs = comp_non_ptr(node.p[1]);
		result->data = is_integer(*node.type_)
			? builder->CreateAdd(lhs, rhs)
			: builder->CreateFAdd(lhs, rhs);
	}
	void on_lt(ast::LtOp& node) override {
		auto lhs = comp_non_ptr(node.p[0]);
		auto rhs = comp_non_ptr(node.p[1]);
		result->data = is_integer(*node.p[0]->type())
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
				auto on_eq = llvm::BasicBlock::Create(*gen.context, "", gen.current_ll_fn);
				auto on_ne = llvm::BasicBlock::Create(*gen.context, "", gen.current_ll_fn);
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
			void call_comparer(llvm::Function* comparer) {
				gen.result->data = gen.builder->CreateCall(comparer, {
					gen.cast_to(lhs, gen.ptr_type),
					gen.cast_to(rhs, gen.ptr_type) });
			}
			Comparer(llvm::Value* lhs, llvm::Value* rhs, Generator& gen) : lhs(lhs), rhs(rhs), gen(gen) {}
			void on_int32(ast::TpInt32& type) override { compare_scalar(); }
			void on_int64(ast::TpInt64& type) override { compare_scalar(); }
			void on_enum(ast::TpEnum& type) override { compare_scalar(); }
			void on_float(ast::TpFloat& type) override { gen.result->data = gen.builder->CreateCmp(llvm::CmpInst::Predicate::FCMP_OEQ, lhs, rhs); }
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
					void on_int32(ast::TpInt32& type) override { c.compare_scalar(); }
					void on_int64(ast::TpInt64& type) override { c.compare_pair(); }
					void on_enum(ast::TpEnum& type) override { c.compare_scalar(); }
					void on_float(ast::TpFloat& type) override { c.compare_scalar(); }
					void on_double(ast::TpDouble& type) override { c.compare_scalar(); }
					void on_function(ast::TpFunction& type) override { c.compare_scalar(); }
					void on_lambda(ast::TpLambda& type) override { c.compare_pair(); }
					void on_delegate(ast::TpDelegate& type) override { c.compare_pair(); }
					void on_cold_lambda(ast::TpColdLambda& type) override { c.compare_scalar(); }
					void on_void(ast::TpVoid& type) override { c.compare_scalar(); }
					void on_optional(ast::TpOptional& type) override { assert(false); }
					void on_own(ast::TpOwn& type) override { c.call_comparer(c.gen.fn_eq_mut); }
					void on_ref(ast::TpRef& type) override { c.call_comparer(c.gen.fn_eq_mut); }
					void on_shared(ast::TpShared& type) override { c.call_comparer(c.gen.fn_eq_shared); }
					void on_weak(ast::TpWeak& type) override { c.compare_scalar(); }
					void on_frozen_weak(ast::TpFrozenWeak& type) override { c.compare_scalar(); }
					void on_conform_ref(ast::TpConformRef& type) override { c.call_comparer(c.gen.fn_eq_shared); }
					void on_conform_weak(ast::TpConformWeak& type) override { c.compare_scalar(); }
					void on_no_ret(ast::TpNoRet& type) override { assert(false); }
				};
				OptComparer opt_comparer{ *this };
				type.wrapped->match(opt_comparer);
			}
			void on_own(ast::TpOwn& type) override { call_comparer(gen.fn_eq_mut); }
			void on_ref(ast::TpRef& type) override { call_comparer(gen.fn_eq_mut); }
			void on_shared(ast::TpShared& type) override { call_comparer(gen.fn_eq_shared); }
			void on_weak(ast::TpWeak& type) override { compare_scalar(); }
			void on_frozen_weak(ast::TpFrozenWeak& type) override { compare_scalar(); }
			void on_conform_ref(ast::TpConformRef& type) override { call_comparer(gen.fn_eq_shared); }
			void on_conform_weak(ast::TpConformWeak& type) override { compare_scalar(); }
			void on_no_ret(ast::TpNoRet& type) override { assert(false); }
		};
		auto lhs = compile(node.p[0]);
		size_t break_mark_after_lhs = active_breaks.size();
		auto rhs = compile(node.p[1]);
		Comparer comparer{ lhs.data, rhs.data, *this };
		node.p[0]->type()->match(comparer);
		dispose_val(rhs, active_breaks.size());
		dispose_val(lhs, break_mark_after_lhs);
	}
	void on_sub(ast::SubOp& node) override {
		auto lhs = comp_non_ptr(node.p[0]);
		auto rhs = comp_non_ptr(node.p[1]);
		result->data = is_integer(*node.type_)
			? builder->CreateSub(lhs, rhs)
			: builder->CreateFSub(lhs, rhs);
	}
	void on_mul(ast::MulOp& node) override {
		auto lhs = comp_non_ptr(node.p[0]);
		auto rhs = comp_non_ptr(node.p[1]);
		result->data = is_integer(*node.type_)
			? builder->CreateMul(lhs, rhs)
			: builder->CreateFMul(lhs, rhs);
	}
	void on_div(ast::DivOp& node) override {
		auto lhs = comp_non_ptr(node.p[0]);
		auto rhs = comp_non_ptr(node.p[1]);
		result->data = is_integer(*node.type_)
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
			void on_int32(ast::TpInt32& type) override {
				val = depth > 0
					? val
					: gen->builder->CreateZExt(val, gen->int_type);
			}
			void on_enum(ast::TpEnum& type) override {
				// val is already bit and type compatible
			}
			void on_float(ast::TpFloat& type) override {
				val = depth > 0
					? val
					: gen->builder->CreateBitCast(val, gen->int32_type);
			}
			void on_double(ast::TpDouble& type) override {
				val = depth > 0
					? val 
					: gen->builder->CreateBitCast(val, gen->int_type);
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
					: gen->builder->CreateIntCast(val, gen->tp_opt_bool, false);
			}
			void on_optional(ast::TpOptional& type) override { assert(false); }
			void handle_ptr() {
				val = depth > 0
					? val
					: gen->builder->CreatePtrToInt(val, gen->tp_int_ptr);
			}
			void on_own(ast::TpOwn& type) override { handle_ptr(); }
			void on_ref(ast::TpRef& type) override { handle_ptr(); }
			void on_shared(ast::TpShared& type) override { handle_ptr(); }
			void on_weak(ast::TpWeak& type) override { handle_ptr(); }
			void on_frozen_weak(ast::TpFrozenWeak& type) override { handle_ptr(); }
			void on_conform_ref(ast::TpConformRef& type) override { handle_ptr(); }
			void on_conform_weak(ast::TpConformWeak& type) override { handle_ptr(); }
			void on_no_ret(ast::TpNoRet& type) override { assert(false); }
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
			void on_int32(ast::TpInt32& type) override {
				val = llvm::ConstantInt::get(gen->int_type, (depth + 1ull) << 32);
			}
			void on_float(ast::TpFloat& type) override { val = gen->builder->getInt32(depth | 0x7fc00000); } //    0_111 1111 1_10(0)...
			void on_double(ast::TpDouble& type) override { val = gen->builder->getInt64(depth | 0x7ff8000000000000); } // 0_111 1111 1111 _10(0)...
			void on_function(ast::TpFunction& type) override { val = llvm::ConstantInt::get(gen->tp_int_ptr, depth); }
			void on_lambda(ast::TpLambda& type) override { make_int_ptr_pair(); }
			void on_delegate(ast::TpDelegate& type) override { make_int_ptr_pair(); }
			void on_cold_lambda(ast::TpColdLambda& type) override { val = gen->builder->getInt8(depth ? depth + 1 : 0); }
			void on_void(ast::TpVoid& type) override {
				val = depth == 0 
					? gen->builder->getInt1(false)
					: gen->builder->getInt8(depth + 1);
			}
			void on_optional(ast::TpOptional& type) override { assert(false); }
			void on_own(ast::TpOwn& type) override { val = llvm::ConstantInt::get(gen->tp_int_ptr, depth); }
			void on_ref(ast::TpRef& type) override { val = llvm::ConstantInt::get(gen->tp_int_ptr, depth); }
			void on_shared(ast::TpShared& type) override { val = llvm::ConstantInt::get(gen->tp_int_ptr, depth); }
			void on_weak(ast::TpWeak& type) override { val = llvm::ConstantInt::get(gen->tp_int_ptr, depth); }
			void on_frozen_weak(ast::TpFrozenWeak& type) override { val = llvm::ConstantInt::get(gen->tp_int_ptr, depth); }
			void on_conform_ref(ast::TpConformRef& type) override { val = llvm::ConstantInt::get(gen->tp_int_ptr, depth); }
			void on_conform_weak(ast::TpConformWeak& type) override { val = llvm::ConstantInt::get(gen->tp_int_ptr, depth); }
			void on_no_ret(ast::TpNoRet& type) override { assert(false); }
			void on_enum(ast::TpEnum& type) override { val = llvm::ConstantInt::get(gen->int_type, depth); }
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
			void on_int32(ast::TpInt32& type) override {
				val = gen->builder->CreateICmpNE(
					val,
					gen->builder->getInt64((depth + 1ull) << 32));
			}
			void on_float(ast::TpFloat& type) override {
				val = gen->builder->CreateICmpNE(
					val,
					gen->builder->getInt32(depth | 0x7fc00000));
			}
			void on_double(ast::TpDouble& type) override {
				val = gen->builder->CreateICmpNE(
					val,
					gen->builder->getInt64(depth | 0x7ff8000000000000));
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
			void handle_ptr() {
				val = gen->builder->CreateICmpNE(
					val,
					llvm::ConstantInt::get(gen->tp_int_ptr, depth));
			}
			void on_own(ast::TpOwn& type) override { handle_ptr(); }
			void on_ref(ast::TpRef& type) override { handle_ptr(); }
			void on_shared(ast::TpShared& type) override { handle_ptr(); }
			void on_weak(ast::TpWeak& type) override { handle_ptr(); }
			void on_frozen_weak(ast::TpFrozenWeak& type) override { handle_ptr(); }
			void on_conform_ref(ast::TpConformRef& type) override { handle_ptr(); }
			void on_conform_weak(ast::TpConformWeak& type) override { handle_ptr(); }
			void on_no_ret(ast::TpNoRet& type) override { assert(false); }
			void on_enum(ast::TpEnum& type) override {
				val = gen->builder->CreateICmpNE(
					val,
					llvm::ConstantInt::get(gen->int_type, depth));
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
			void on_int32(ast::TpInt32& type) override {
				val = depth > 0
					? val
					: gen->builder->CreateTrunc(val, gen->int32_type);
			}
			void on_int64(ast::TpInt64& type) override {
				val = depth > 0
					? val
					: gen->builder->CreateExtractValue(val, { 1 });
			}
			void on_float(ast::TpFloat& type) override {
				val = depth > 0
					? val
					: gen->builder->CreateBitCast(val, gen->float_type);
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
				val = depth == 0 ? llvm::UndefValue::get(gen->void_type)
					: depth == 1 ? gen->builder->CreateTrunc(val, gen->tp_bool)
					: val;
			}
			void on_optional(ast::TpOptional& type) override { assert(false); }
			void handle_ptr(ast::Type& type) {
				val = depth > 0
					? val
					: gen->builder->CreateBitOrPointerCast(val, gen->to_llvm_type(type));
			}
			void on_own(ast::TpOwn& type) override { handle_ptr(type); }
			void on_ref(ast::TpRef& type) override { handle_ptr(type); }
			void on_shared(ast::TpShared& type) override { handle_ptr(type); }
			void on_weak(ast::TpWeak& type) override { handle_ptr(type); }
			void on_frozen_weak(ast::TpFrozenWeak& type) override { handle_ptr(type); }
			void on_conform_ref(ast::TpConformRef& type) override { handle_ptr(type); }
			void on_conform_weak(ast::TpConformWeak& type) override { handle_ptr(type); }
			void on_no_ret(ast::TpNoRet& type) override { assert(false); }
			void on_enum(ast::TpEnum& type) override {} // no-op
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
		auto then_bb = llvm::BasicBlock::Create(*context, "", current_ll_fn);
		auto else_bb = llvm::BasicBlock::Create(*context, "", current_ll_fn);
		auto joined_bb = llvm::BasicBlock::Create(*context, "", current_ll_fn);
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
		if (get_if<Val::Static>(&then_val.lifetime)) {
			result.lifetime = else_val.lifetime;
		} else if (get_if<Val::Static>(&else_val.lifetime)) {
			result.lifetime = then_val.lifetime;
		} else if (get_if<Val::Retained>(&then_val.lifetime)) {
			make_retained_or_non_ptr(else_val);
			result.lifetime = Val::Retained{};
		} else if (get_if<Val::Retained>(&else_val.lifetime)) {
			builder->SetInsertPoint(then_bb);
			make_retained_or_non_ptr(then_val);
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
			auto comp_then = [&] { // has copy in on_else
				auto as_block = dom::strict_cast<ast::Block>(node.p[1]);
				return as_block && !as_block->names.empty() && !as_block->names.front()->initializer
					? handle_block(*as_block, Val{ as_block->names.front()->type, extract_opt_val(cond_opt_val.data, cond_type), cond_opt_val.lifetime })
					: compile(node.p[1]);
			};
			if (dom::isa<ast::TpNoRet>(*node.p[1]->type())) {
				auto then_bb = llvm::BasicBlock::Create(*context, "", current_ll_fn);
				auto else_bb = llvm::BasicBlock::Create(*context, "", current_ll_fn);
				builder->CreateCondBr(check_opt_has_val(cond_opt_val.data, cond_type), then_bb, else_bb);
				builder->SetInsertPoint(then_bb);
				comp_then();
				builder->SetInsertPoint(else_bb);
				*result = Val{ node_type, make_opt_none(node_type), Val::Static{} };
			} else {
				*result = compile_if(
					*node_type,
					check_opt_has_val(cond_opt_val.data, cond_type),
					[&] {
						auto val = comp_then();
						return Val{ node_type, make_opt_val(val.data, node_type), val.lifetime };
					},
					[&] {
						return Val{ node_type, make_opt_none(node_type), Val::Static{} };
					});
			}
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
				return Val{ node_type, make_opt_none(node_type), Val::Static{} };
			});
	}
	void on_else(ast::Else& node) override {
		if (dom::isa<ast::TpNoRet>(*node.p[1]->type())) {
			auto cond_type = dom::strict_cast<ast::TpOptional>(node.p[0]->type());
			auto cond_opt = compile(node.p[0]);
			auto then_bb = llvm::BasicBlock::Create(*context, "", current_ll_fn);
			auto else_bb = llvm::BasicBlock::Create(*context, "", current_ll_fn);
			builder->CreateCondBr(check_opt_has_val(cond_opt.data, cond_type), then_bb, else_bb);
			builder->SetInsertPoint(else_bb);
			auto unused = compile(node.p[1]);
			builder->SetInsertPoint(then_bb);
			*result = Val{ node.type(), extract_opt_val(cond_opt.data, cond_type), cond_opt.lifetime };
			return;
		}
		if (auto as_if = dom::strict_cast<ast::If>(node.p[0])) {
			auto cond_type = dom::strict_cast<ast::TpOptional>(as_if->p[0]->type());
			auto cond_opt_val = compile(as_if->p[0]);
			*result = compile_if(
				*node.type(),
				check_opt_has_val(cond_opt_val.data, cond_type),
				[&] {
					auto as_block = dom::strict_cast<ast::Block>(as_if->p[1]);
					return as_block && !as_block->names.empty() && !as_block->names.front()->initializer
						? handle_block(*as_block, Val{ as_block->names.front()->type, extract_opt_val(cond_opt_val.data, cond_type), cond_opt_val.lifetime })
						: compile(as_if->p[1]);
				}, [&] {
					return compile(node.p[1]);
				});
			return;
		}
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
		auto loop_body = llvm::BasicBlock::Create(*context, "", current_ll_fn);
		builder->CreateBr(loop_body);
		builder->SetInsertPoint(loop_body);
		*result = compile(node.p);
		auto r_type = dom::strict_cast<ast::TpOptional>(node.p->type());
		if (r_type) {
			auto after_loop = llvm::BasicBlock::Create(*context, "", current_ll_fn);
			builder->CreateCondBr(
				check_opt_has_val(result->data, r_type),
				after_loop,
				loop_body);
			builder->SetInsertPoint(after_loop);
			result->data = extract_opt_val(result->data, r_type);
		} else {
			builder->CreateBr(loop_body);
			builder->SetInsertPoint((llvm::BasicBlock*)nullptr);
		}
		result->type = node.type();
	}
	void on_copy(ast::CopyOp& node) override {
		auto src = compile(node.p);
		result->data = builder->CreateCall(fn_copy, { src.data });
		result->lifetime = Val::Retained{};
		dispose_val(src, active_breaks.size());
	}
	void on_mk_weak(ast::MkWeakOp& node) override {
		if (dom::strict_cast<ast::MkInstance>(node.p)) {
			result->data = null_weak;
			return;
		}
		auto src = compile(node.p);
		result->data = builder->CreateCall(fn_mk_weak, { src.data });
		result->lifetime = Val::Retained{};
		dispose_val(src, active_breaks.size());
	}
	void on_deref_weak(ast::DerefWeakOp& node) override {
		auto src = compile(node.p);
		result->data = builder->CreateCall(fn_deref_weak, { src.data });
		result->lifetime = Val::Retained{};
		dispose_val(src, active_breaks.size());
	}

	llvm::FunctionType* lambda_to_llvm_fn(ast::Node& n, pin<ast::Type> tp) {  // also delegate and method
		if (isa<ast::TpLambda>(*tp) || isa<ast::TpDelegate>(*tp)) {
			pin<ast::TpLambda> as_lambda = cast<ast::TpLambda>(tp);
			auto& fn = lambda_fns[as_lambda];
			if (!fn) {
				vector<llvm::Type*> params{ ptr_type }; // closure struct or this
				for (size_t i = 0; i < as_lambda->params.size() - 1; i++)
					params.push_back(to_llvm_type(*as_lambda->params[i]));
				auto result_type = as_lambda->params.back().pinned();
				if (as_lambda->can_x_break && !isa<ast::TpNoRet>(*result_type))
					result_type = ast->tp_optional(result_type);
				fn = llvm::FunctionType::get(to_llvm_type(*result_type), move(params), false);
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
				auto result_type = as_func->params.back().pinned();
				if (as_func->can_x_break)
					result_type = ast->tp_optional(result_type);
				fn = llvm::FunctionType::get(to_llvm_type(*result_type), move(params), false);
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

			void on_int32(ast::TpInt32& type) override { result = gen->int32_type; }
			void on_int64(ast::TpInt64& type) override { result = gen->int_type; }
			void on_float(ast::TpFloat& type) override { result = gen->float_type; }
			void on_double(ast::TpDouble& type) override { result = gen->double_type; }
			void on_function(ast::TpFunction& type) override { result = gen->ptr_type; }
			void on_lambda(ast::TpLambda& type) override { result = gen->lambda_struct; }
			void on_delegate(ast::TpDelegate& type) override { result = gen->delegate_struct; }
			void on_cold_lambda(ast::TpColdLambda& type) override { type.resolved->match(*this); }
			void on_void(ast::TpVoid&) override { result = gen->void_type; }
			void on_optional(ast::TpOptional& type) override {
				struct OptionalMatcher :ast::TypeMatcher {
					Generator* gen;
					int depth;
					llvm::Type* result = nullptr;
					OptionalMatcher(Generator* gen, int depth) :gen(gen), depth(depth) {}

					void on_int32(ast::TpInt32& type) override { result = gen->int_type; } // depth+1 << 32 = optional_nones, val & ffffffff = opt value
					void on_int64(ast::TpInt64& type) override { result = gen->tp_opt_int; }
					void on_float(ast::TpFloat& type) override { result = gen->int32_type; } // bitcasted float where NaNs are nones
					void on_double(ast::TpDouble& type) override { result = gen->int_type; } // bitcasted double where NaNs are nones
					void on_function(ast::TpFunction& type) override { result = gen->tp_int_ptr; }
					void on_lambda(ast::TpLambda& type) override { result = gen->tp_opt_lambda; }
					void on_delegate(ast::TpDelegate& type) override { result = gen->tp_opt_delegate; }
					void on_cold_lambda(ast::TpColdLambda& type) override { result = gen->tp_opt_bool; }
					void on_void(ast::TpVoid&) override { result = depth == 0 ? gen->tp_bool : gen->tp_opt_bool; }
					void on_optional(ast::TpOptional& type) override { assert(false); };
					void on_own(ast::TpOwn& type) override { result = gen->tp_int_ptr; }
					void on_ref(ast::TpRef& type) override { result = gen->tp_int_ptr; }
					void on_shared(ast::TpShared& type) override { result = gen->tp_int_ptr; }
					void on_weak(ast::TpWeak& type) override { result = gen->tp_int_ptr; }
					void on_frozen_weak(ast::TpFrozenWeak& type) override { result = gen->tp_int_ptr; }
					void on_conform_ref(ast::TpConformRef& type) override { result = gen->tp_int_ptr; }
					void on_conform_weak(ast::TpConformWeak& type) override { result = gen->tp_int_ptr; }
					void on_no_ret(ast::TpNoRet& type) override { result = gen->void_type; }
					void on_enum(ast::TpEnum& type) override { result = gen->int_type; }
				};
				OptionalMatcher matcher(gen, type.depth);
				type.wrapped->match(matcher);
				result = matcher.result;
			}
			void on_own(ast::TpOwn& type) override { result = gen->ptr_type; }
			void on_ref(ast::TpRef& type) override { result = gen->ptr_type; }
			void on_shared(ast::TpShared& type) override { result = gen->ptr_type; }
			void on_weak(ast::TpWeak& type) override { result = gen->ptr_type; }
			void on_frozen_weak(ast::TpFrozenWeak& type) override { result = gen->ptr_type; }
			void on_conform_ref(ast::TpConformRef& type) override { result = gen->ptr_type; }
			void on_conform_weak(ast::TpConformWeak& type) override { result = gen->ptr_type; }
			void on_no_ret(ast::TpNoRet& type) override { result = gen->void_type; }
			void on_enum(ast::TpEnum& type) override { result = gen->int_type; }
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
			vector<llvm::Constant*> i_table(1ull << best.width, empty_mtable);
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
		indirect_table.resize(1ull << best.width);
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

	llvm::orc::ThreadSafeModule build(string entry_point_name) {
		std::unordered_set<pin<ast::Class>> special_copy_and_dispose = {
			ast->blob,
			ast->own_array,
			ast->weak_array,
			ast->modules["sys"]->peek_class("SharedArray"),
			ast->modules["sys"]->peek_class("Thread"),
			ast->modules["sys"]->peek_class("Map"),
			ast->modules["sys"]->peek_class("SharedMap"),
			ast->modules["sys"]->peek_class("WeakMap"),
		};
		dispatcher_fn_type = llvm::FunctionType::get(ptr_type, { int_type }, false);
		auto dispose_fn_type = llvm::FunctionType::get(void_type, { ptr_type }, false);
		auto copier_fn_type = llvm::FunctionType::get(
			void_type,
			{
				ptr_type, // dst
				ptr_type  // src
			},
			false);  // varargs
		auto visitor_fn_type = llvm::FunctionType::get(
			void_type,
			{
				ptr_type, // field addr
				int_type, // field_type (see AG_VISIT_* in runtime.h)
				ptr_type  // context
			},
			false);  // varargs
		auto visit_fn_type = llvm::FunctionType::get(
			void_type,
			{
				ptr_type,        // object
				visitor_fn_type->getPointerTo(), // called on each pointer field and containter item
				ptr_type         // context
			},
			false);  // varargs
		obj_vmt_struct = llvm::StructType::get(
			*context,
			{
				copier_fn_type->getPointerTo(),
				dispose_fn_type->getPointerTo(),
				visit_fn_type->getPointerTo(),
				int_type,  // instance alloc size
				int_type   // obj vmt size (used in casts)
			});
		auto initializer_fn_type = llvm::FunctionType::get(void_type, { ptr_type }, false);
		if (di_builder) {
			for (auto& m : ast->modules) {
				string dir = "";
				string name = m.second->path;
				if (auto p = name.find_last_of("/\\"); p != string::npos) {
					dir = name.substr(0, p);
					name = name.substr(p + 1);
				}
				di_files.insert({
					m.first,
					di_builder->createFile( name, dir)
				});
			}
		}
		// Make LLVM types for classes
		for (auto& cls : ast->classes_in_order) {
			if (!cls->used)
				continue;
			auto c_name = ast::format_str("ag_cls_", cls->get_name());
			auto& info = classes[cls];
			if (cls->is_interface) {
				uint64_t id = 0;
				do
					id = uniform_uint64_distribution(random_generator) << 16;
				while (assigned_interface_ids.count(id) != 0);
				assigned_interface_ids.insert(id);
				info.interface_ordinal = id;
				continue;
			} else {
				auto disp_name = ast::format_str("ag_disp_", cls->get_name());
				info.dispatcher = llvm::Function::Create(
					dispatcher_fn_type,
					llvm::Function::ExternalLinkage,
					disp_name,
					module.get());
				if (di_builder) {
					info.dispatcher->setSubprogram(
						di_builder->createFunction(
							di_cu,
							disp_name,
							disp_name,  // linkage name
							nullptr,
							0,
							di_fn_type,
							0,
							llvm::DINode::FlagPrototyped,
							llvm::DISubprogram::SPFlagDefinition));
					/*
					di_builder->createGlobalVariableExpression(
						di_cu,
						disp_name,
						disp_name,
						di_files[cls->module->name],
						cls->line,
						di_int,
						false); // is_local  */
				}
			}
			info.fields = llvm::StructType::create(*context, c_name);
			info.constructor = llvm::Function::Create(
				llvm::FunctionType::get(info.fields->getPointerTo(), {}, false),
				llvm::Function::InternalLinkage,
				c_name + "_ctor",
				module.get());
			info.initializer = llvm::Function::Create(
				initializer_fn_type,
				llvm::Function::InternalLinkage,
				c_name + "_init",
				module.get());
		}
		// Make llvm types for fields.
		// Fill llvm structs for classes with fields.
		for (auto& cls : ast->classes_in_order) {
			if (!cls->used)
				continue;
			auto& info = classes.at(cls);
			if (cls->is_interface)
				continue;
			vector<llvm::Type*> fields;
			if (cls->base_class) {
				const auto& base_fields = classes.at(cls->base_class).fields->elements();
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
			if (cls == ast->string_cls)
				fields.push_back(llvm::Type::getInt8Ty(*context));
			info.fields->setBody(fields);
		}
		make_di_clases();
		// Make llvm types for methods.
		// Define llvm types for vmts.
		for (auto& cls : ast->classes_in_order) {
			if (!cls->used)
				continue;
			auto& info = classes.at(cls);
			vector<llvm::Type*> vmt_content{ dispatcher_fn_type->getPointerTo()};  // interface/class id for casts
			for (auto& m : cls->new_methods) {
				if (!m->base->used)
					continue;
				vector<llvm::Type*> params;
				for (auto& p : m->names)
					params.push_back(to_llvm_type(*p->type));
				params[0] = ptr_type;  // this
				auto& m_info = methods[m];
				auto result_type = m->type_expression->type();
				if (cast<ast::TpLambda>(m->type())->can_x_break)
					result_type = ast->tp_optional(result_type);
				m_info.type = llvm::FunctionType::get(to_llvm_type(*result_type), move(params), false);
				m_info.ordinal = vmt_content.size();
				vmt_content.push_back(m_info.type->getPointerTo());
			}
			if (cls->base_class) {  // !interface && !Object
				auto& base_info = classes.at(cls->base_class);
				size_t base_index = vmt_content.size();
				for (auto& mt : base_info.vmt->elements())
					vmt_content.push_back(mt);
				for (auto& m : cls->overloads[cls->base_class]) {
					if (!m->base->used)
						continue;
					auto& m_info = methods[m];
					auto& m_overridden = methods.at(m->ovr);
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
		for (auto& m : ast->modules_in_order) {
			for (auto& c : m->constants) {
				auto name = ast::format_str("ag_const_", c.second->module->name, "_", c.first);
				auto type = to_llvm_type(*c.second->type);
				module->getOrInsertGlobal(name, type);
				auto addr = module->getGlobalVariable(name);
				addr->setLinkage(llvm::GlobalValue::InternalLinkage);
				addr->setInitializer(llvm::Constant::getNullValue(type));
				globals.insert({ c.second, addr });
			}
		}
		for (auto& m : ast->modules) {
			for (auto& fn : m.second->functions) {
				if (!fn.second->used)
					continue;
				auto f = llvm::Function::Create(
					function_to_llvm_fn(*fn.second, fn.second->type()),
					fn.second->is_platform
					? llvm::Function::ExternalLinkage
					: llvm::Function::InternalLinkage,
					ast::format_str("ag_fn_", m.first, "_", fn.first), module.get());
				if (dom::isa<ast::TpNoRet>(*cast<ast::TpLambda>(fn.second->type())->params.back())) {
					f->addFnAttr(llvm::Attribute::NoReturn);
				}
				functions.insert({ fn.second, f });
			}
		}
		// From this point it is possible to build code that access fields and methods.
		// Make llvm functions for standalone ast functions.
		// Build class contents - initializer, dispatcher, disposer, copier, methods.
		for (auto& cls : ast->classes_in_order) {
			if (cls->is_interface)
				continue;
			if (!cls->used)
				continue;
			auto& info = classes.at(cls);
			ClassInfo* base_info = cls->base_class && cls->base_class != ast->object ? &classes.at(cls->base_class) : nullptr;
			info.dispose = llvm::Function::Create(
				dispose_fn_type,
				special_copy_and_dispose.count(cls) == 0
					? llvm::Function::InternalLinkage
					: llvm::Function::ExternalLinkage,
				ast::format_str("ag_dtor_", cls->module->name, "_", cls->name), module.get());
			// Initializer
			llvm::IRBuilder<> builder(llvm::BasicBlock::Create(*context, "", info.initializer));
			current_ll_fn = info.initializer;
			this->builder = &builder;
			if (base_info)
				builder.CreateCall(base_info->initializer, { info.initializer->arg_begin() });
			auto result = builder.CreateBitOrPointerCast(info.initializer->arg_begin(),
				info.fields->getPointerTo());
			for (auto& field : cls->fields) {
				auto initializer = compile(field->initializer);
				make_retained_or_non_ptr(initializer, result);
				builder.CreateStore(
					initializer.data,
					builder.CreateStructGEP(info.fields, result, field->offset));
			}
			builder.CreateRetVoid();
			// Constructor
			builder.SetInsertPoint(llvm::BasicBlock::Create(*context, "", info.constructor));
			current_ll_fn = info.constructor;
			result = builder.CreateCall(fn_allocate, {
				builder.getInt64(layout.getTypeAllocSize(info.fields)) });
			builder.CreateCall(info.initializer, { result });
			auto typed_result = builder.CreateBitOrPointerCast(result, info.fields->getPointerTo());
			builder.CreateStore(cast_to(info.dispatcher, ptr_type), builder.CreateConstGEP2_32(obj_struct, result, AG_HEADER_OFFSET, 0));
			builder.CreateRet(typed_result);
			// Disposer
			if (special_copy_and_dispose.count(cls) == 0) {
				builder.SetInsertPoint(llvm::BasicBlock::Create(*context, "", info.dispose));
				current_ll_fn = info.dispose;
				if (auto manual_disposer_fn = cls->module->functions.find("dispose" + cls->name); manual_disposer_fn != cls->module->functions.end()) {
					// TODO: check prototype
					builder.CreateCall(
						functions.at(manual_disposer_fn->second.pinned()),
						{ cast_to(info.dispose->getArg(0), info.fields->getPointerTo()) });
				}
				if (base_info)
					builder.CreateCall(base_info->dispose, { info.dispose->getArg(0) });
				result = builder.CreateBitOrPointerCast(info.dispose->getArg(0), info.fields->getPointerTo());
				for (auto& field : cls->fields) {
					build_release(
						builder.CreateLoad(
							info.fields->getElementType(field->offset),
							builder.CreateStructGEP(info.fields, result, field->offset)),
						field->initializer->type(),
						false);  // not local, clear parent
				}
				builder.CreateRetVoid();
			}
			// Visitor
			info.visit = llvm::Function::Create(
				visit_fn_type,
				special_copy_and_dispose.count(cls) == 0
					? llvm::Function::InternalLinkage
					: llvm::Function::ExternalLinkage,
				ast::format_str("ag_visit_", cls->module->name, "_", cls->name), module.get());
			if (special_copy_and_dispose.count(cls) == 0) {
				builder.SetInsertPoint(llvm::BasicBlock::Create(*context, "", info.visit));
				current_ll_fn = info.visit;
				if (base_info)
					builder.CreateCall(base_info->visit, { info.visit->getArg(0), info.visit->getArg(1), info.visit->getArg(2) });
				result = builder.CreateBitOrPointerCast(info.visit->getArg(0), info.fields->getPointerTo());
				for (auto& field : cls->fields) {
					auto field_type = field->initializer->type().pinned();
					if (!is_ptr(field_type))
						continue;
					if (auto as_opt = dom::strict_cast<ast::TpOptional>(field_type))
						field_type = as_opt->wrapped;
					llvm::Value* field_addr = builder.CreateStructGEP(info.fields, result, field->offset);
					auto type_id = const_0;  // AG_VISIT_OWN
					if (isa<ast::TpWeak>(*field_type) || isa<ast::TpFrozenWeak>(*field_type)) {
						type_id = const_1;  // AG_VISIT_WEAK
					} else if (isa<ast::TpDelegate>(*field_type)) {
						type_id = const_1;  // AG_VISIT_WEAK;
						field_addr = builder.CreateStructGEP(delegate_struct, result, { 0 });
					} else if (isa<ast::TpShared>(*field_type) || isa<ast::TpOwn>(*field_type)) {
						type_id = const_0;  // AG_VISIT_OWN
					} else {
						continue;
					}
					builder.CreateCall(
						llvm::FunctionCallee(
							visitor_fn_type,
							info.visit->getArg(1)),
						{ field_addr, type_id, info.visit->getArg(2) });
				}
				builder.CreateRetVoid();
			}
			// Copier
			info.copier = llvm::Function::Create(
				copier_fn_type,
				special_copy_and_dispose.count(cls) == 0
					? llvm::Function::InternalLinkage
					: llvm::Function::ExternalLinkage,
				ast::format_str("ag_copy_", cls->get_name()), module.get());
			if (special_copy_and_dispose.count(cls) == 0) {
				builder.SetInsertPoint(llvm::BasicBlock::Create(*context, "", info.copier));
				current_ll_fn = info.copier;
				if (auto manual_fixer_fn = cls->module->functions.find("afterCopy" + cls->name); manual_fixer_fn != cls->module->functions.end()) {
					// TODO: check prototype
					builder.CreateCall(
						fn_reg_copy_fixer,
						{
							cast_to(info.copier->getArg(0), ptr_type),
							cast_to(functions.at(manual_fixer_fn->second.pinned()), ptr_type)
						});
				}
				if (base_info)
					builder.CreateCall(
						base_info->copier,
						{ info.copier->getArg(0), info.copier->getArg(1) });
				auto dst = builder.CreateBitOrPointerCast(info.copier->getArg(0), info.fields->getPointerTo());
				auto src = builder.CreateBitOrPointerCast(info.copier->getArg(1), info.fields->getPointerTo());
				for (auto& f : cls->fields) {
					pin<ast::Type> type = f->initializer->type();
					if (auto as_opt = dom::strict_cast<ast::TpOptional>(type))
						type = as_opt->wrapped;
					if (isa<ast::TpWeak>(*type) || isa<ast::TpFrozenWeak>(*type)) {
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
					} else if (isa<ast::TpOwn>(*type)) {
						builder.CreateStore(
							builder.CreateCall(fn_copy_object_field, {
								builder.CreateLoad(
									ptr_type,
									builder.CreateStructGEP(info.fields, src, f->offset)),
								dst }),
							builder.CreateStructGEP(info.fields, dst, f->offset));
					} else if (isa<ast::TpShared>(*type)) {
						build_retain(
							builder.CreateLoad(
								ptr_type,
								builder.CreateStructGEP(info.fields, src, f->offset)),
							f->initializer->type());
					}
				}
				builder.CreateRetVoid();
			}
			// Class methods
			info.vmt_fields.push_back(info.dispatcher);  // class id for casts
			for (auto& m : cls->new_methods) {
				if (!m->base->used)
					continue;
				//auto& m_info = methods.at(m);
				info.vmt_fields.push_back(compile_function(*m,
					ast::format_str("ag_m_", cls->get_name(), '_', ast::LongName{ m->name, m->base_module }),
					info.fields->getPointerTo(),
					m->is_platform));
			}
			if (cls->base_class) {
				auto& base_vmt = classes.at(cls->base_class).vmt_fields;
				for (size_t i = 0, j = base_vmt.size() - 1; i < j; i++)
					info.vmt_fields.push_back(base_vmt[i]);
				for (auto& m : cls->overloads[cls->base_class]) { // for overrides
					if (!m->base->used)
						continue;
					auto& m_info = methods.at(m);
					info.vmt_fields[m_info.ordinal] = compile_function(*m,
						ast::format_str("ag_m_", cls->get_name(), '_', ast::LongName{ m->name, m->base_module }),
						info.fields->getPointerTo(),
						m->is_platform);
				}
			}
			info.vmt_fields.push_back(llvm::ConstantStruct::get(obj_vmt_struct, {
				info.copier,
				info.dispose,
				info.visit,
				builder.getInt64(layout.getTypeStoreSize(info.fields)),
				builder.getInt64(info.vmt_size) }));
			info.dispatcher->setPrefixData(llvm::ConstantStruct::get(info.vmt, move(info.vmt_fields)));
			// Interface methods
			unordered_map<uint64_t, llvm::Constant*> vmts;  // interface_id->vmt_struct
			for (auto& i : cls->interface_vmts) {
				if (!i.first->used)
					continue;
				vector<llvm::Constant*> methods {
					llvm::ConstantExpr::getIntegerValue(ptr_type, llvm::APInt(64, classes.at(i.first).interface_ordinal, false)) };
				methods.reserve(i.second.size() + 1);
				for (auto& m : i.second) {
					if (!m->base->used)
						continue;
					methods.push_back(llvm::ConstantExpr::getBitCast(
						compile_function(
							*m.pinned(),
							ast::format_str("ag_m_", cls->get_name(), '_', i.first->get_name(), '_', ast::LongName{ m->name, m->base_module }),
							info.fields->getPointerTo(),
							m->is_platform),
						ptr_type));
				}
				vmts.insert({
					classes.at(i.first).interface_ordinal,
					make_const_array(
						ast::format_str("mt_", cls->get_name(), "_", i.first->get_name()),
						move(methods))});
			}
			builder.SetInsertPoint(llvm::BasicBlock::Create(*context, "", info.dispatcher));
			auto mtable_ptr = build_i_table(ast::format_str("it_", cls->get_name()), builder, vmts, &*info.dispatcher->arg_begin());
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
		for (auto& m : ast->modules) {
			for (auto& fn : m.second->functions) {
				if (fn.second->used && !fn.second->is_platform) {
					current_ll_fn = functions.at(fn.second);
					compile_fn_body(*fn.second, ast::format_str("ag_fn_", m.first, "_", fn.first));
				}
			}
		}
		current_ll_fn = llvm::Function::Create(
			llvm::FunctionType::get(int_type, {}, false),
			llvm::Function::ExternalLinkage,
			entry_point_name, module.get());
		if (ast->test_mode) {
			// Compile tests
			for (auto& m : ast->modules) {
				for (auto& test : m.second->tests) {
					auto fn = llvm::Function::Create(
						llvm::FunctionType::get(void_type, {}, false),
						llvm::Function::ExternalLinkage,
						ast::format_str("ag_test_", m.first, "_", test.first),
						module.get());
					current_ll_fn = fn;
					compile_fn_body(
						*test.second,
						ast::format_str("ag_test_", m.first, "_", test.first),
						nullptr, // closure type
						false,   // handle consts
						true);   // handle threads
				}
			}
		}
		// Main
		compile_fn_body(*ast->starting_module->entry_point, entry_point_name,
			nullptr, // closure type
			true, // handle consts
			true); // handle threads
		while (!execute_in_global_scope.empty()) {
			auto fn = move(execute_in_global_scope.back());
			execute_in_global_scope.pop_back();
			fn();
		}
		if (di_builder)
			di_builder->finalize();
		module->addModuleFlag(llvm::Module::Warning, "Debug Info Version", llvm::DEBUG_METADATA_VERSION);
		module->addModuleFlag(llvm::Module::Warning, "CodeView", 1);
#ifndef WIN32
		module->addModuleFlag(llvm::Module::Max, "Dwarf Version", 4);
#endif
		return llvm::orc::ThreadSafeModule(std::move(module), std::move(context));
	}

	llvm::Constant* make_const_array(string name, vector<llvm::Constant*> content) {
		auto& cached = table_cache[content];
		if (cached)
			return cached;
		auto type = llvm::ArrayType::get(ptr_type, content.size());
		module->getOrInsertGlobal(name, type);
		auto result = module->getGlobalVariable(name);
		result->setInitializer(llvm::ConstantArray::get(type, move(content)));
		result->setConstant(true);
		result->setLinkage(llvm::GlobalValue::InternalLinkage);
		return cached = result;
	}
};

llvm::orc::ThreadSafeModule generate_code(ltm::pin<ast::Ast> ast, bool add_debug_info, string entry_point_name) {
	Generator gen(ast, add_debug_info);
	return gen.build(entry_point_name);
}

#ifdef AG_STANDALONE_COMPILER_MODE
// `ag_disp_sys_String` defined by linker
#else
ag_dispatcher_t ag_disp_sys_String;
#endif

int64_t execute(llvm::orc::ThreadSafeModule& module, ast::Ast& ast, bool dump_ir) {
#ifdef AG_STANDALONE_COMPILER_MODE
	return -1;
#else
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
	for (auto& i : ast.platform_exports) {
		runtime_exports.insert({
			es.intern(i.first),
			{
				llvm::orc::ExecutorAddr::fromPtr(i.second),
				llvm::JITSymbolFlags::Callable
			} });
	}
	check(lib->define(llvm::orc::absoluteSymbols(move(runtime_exports))));
	check(jit->addIRModule(std::move(module)));
	auto f_str_disp = check(jit->lookup("ag_disp_sys_String"));
	ag_disp_sys_String = f_str_disp.toPtr<void**(uint64_t)>();
	auto f_main = check(jit->lookup("main"));
	auto main_addr = f_main.toPtr<void()>();
	for (auto& m : ast.modules) {
		for (auto& test : m.second->tests) {
			std::cout << "Test:" << m.first << "_" << test.first << "\n";
			auto test_fn = check(jit->lookup(ast::format_str("ag_test_", m.first, "_", test.first)));
			auto addr = test_fn.toPtr<void()>();
			addr();
			assert(ag_leak_detector_ok());
			std::cout << " passed" << std::endl;
		}
	}
	main_addr();
	assert(ag_leak_detector_ok());
	ag_disp_sys_String = nullptr;
	return 0;
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
	auto module = generate_code(ast, add_debug_info, "main");
	return execute(module, *ast, dump_ir);
}
