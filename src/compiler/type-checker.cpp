#include "type-checker.h"

#include <algorithm>
#include <vector>
#include <cassert>
#include "name-resolver.h"

namespace {

using std::vector;
using std::function;
using std::string;
using std::move;
using ltm::own;
using ltm::pin;
using ltm::weak;
using ltm::cast;
using dom::strict_cast;
using ast::Type;

auto type_in_progress = own<ast::TpInt64>::make();

struct Typer : ast::ActionMatcher {
	pin<ast::Ast> ast;
	pin<Type> tp_bool;
	pin<Type> tp_no_ret;
	pin<ast::Class> this_class;
	weak<ast::Var> current_underscore_var;
	pin<ast::Module> current_module;

	Typer(ltm::pin<ast::Ast> ast)
		: ast(ast)
	{
		tp_bool = ast->tp_optional(ast->tp_void());
		tp_no_ret = ast->tp_no_ret();
	}
	bool is_integer(ast::Type& t) {
		return dom::isa<ast::TpInt32>(t) || dom::isa<ast::TpInt64>(t);
	}
	bool is_fp(ast::Type& t) {
		return dom::isa<ast::TpFloat>(t) || dom::isa<ast::TpDouble>(t);
	}
	bool is_numeric(ast::Type& t) {
		return is_integer(t) || is_fp(t);
	}
	void on_int_op(ast::BinaryOp& node, const function<string()>& context) {
		node.type_ = find_type(node.p[0])->type();
		if (!is_integer(*node.type_))
			node.p[0]->error("expected int or short at ", context());
		expect_type(find_type(node.p[1]), node.type(), context);
	}
	void on_int_or_double_op(ast::BinaryOp& node, const function<string()>& context) {
		node.type_ = find_type(node.p[0])->type();
		if (!is_numeric(*node.type_))
			node.p[0]->error("expected int or double at ", context());
		expect_type(find_type(node.p[1]), node.type_, context);
	}

	void on_const_i32(ast::ConstInt32& node) override { node.type_ = ast->tp_int32(); }
	void on_const_i64(ast::ConstInt64& node) override { node.type_ = ast->tp_int64(); }
	void on_const_float(ast::ConstFloat& node) override { node.type_ = ast->tp_float(); }
	void on_const_double(ast::ConstDouble& node) override { node.type_ = ast->tp_double(); }
	void on_const_void(ast::ConstVoid& node) override { node.type_ = ast->tp_void(); }
	void on_const_bool (ast::ConstBool& node) override { node.type_ = tp_bool; }
	void on_mk_lambda(ast::MkLambda& node) override {
		for (auto& p : node.names) {
			if (!p->initializer) {
				auto r = pin<ast::TpColdLambda>::make();
				r->callees.push_back({ weak<ast::MkLambda>(&node), current_underscore_var });
				node.type_ = r;
				return;
			}
		}
		vector<own<ast::Type>> params;
		for (auto& p : node.names)
			params.push_back(find_type(p->initializer)->type());
		for (auto& a : node.body)
			find_type(a);
		params.push_back(node.body.back()->type());
		node.type_ = ast->tp_lambda(move(params));
	}
	void on_const_string(ast::ConstString& node) override {
		node.type_ = ast->get_shared(ast->string_cls.pinned());
	}
	void on_block(ast::Block& node) override {
		auto prev_underscore_param = current_underscore_var;
		for (auto& l : node.names) {
			if (l->initializer)
				l->type = find_type(l->initializer)->type();
			if (l->name == "_")
				current_underscore_var = l;

		}
		if (node.body.empty()) {
			node.type_ = ast->tp_void();
			return;
		}
		pin<ast::Action> prev;
		for (auto& a : node.body) {
			if (prev && dom::isa<ast::TpNoRet>(*prev->type()))
				node.error("unreachable code");
			find_type(a);
			prev = a;
		}
		current_underscore_var = prev_underscore_param;
		if (auto ret_as_get = dom::strict_cast<ast::Get>(node.body.back())) {
			if (std::find(node.names.begin(), node.names.end(), ret_as_get->var) != node.names.end()) {
				node.type_ = ret_as_get->var->type;
				return;
			}
		}
		node.type_ = node.body.back()->type();
		for (auto& b : node.breaks) {
			if (dom::isa<ast::TpNoRet>(*node.type_))
				node.type_ = b->result->type();
			else
				expect_type(b->result, node.type_, [&] { return ast::format_str("ret vs natural result"); });
		}
	}
	void on_break(ast::Break& node) override {
		node.type_ = ast->tp_no_ret();
		if (node.block_name.empty())
			return; // it's a type-only node
		find_type(node.result);
	}
	void check_fn_proto(ast::Action& node, ast::TpFunction& fn, vector<own<ast::Action>>& actual_params, ast::Action& callee) {
		if (fn.params.size() - 1 != actual_params.size())
			node.error("Mismatched params count: expected ", fn.params.size() - 1, " provided ", actual_params.size(), " see function definition:", callee);
		for (size_t i = 0; i < actual_params.size(); i++)
			expect_type(actual_params[i], Type::promote(fn.params[i]), [&] { return ast::format_str("parameter ", i); });
		node.type_ = Type::promote(fn.params.back());
	}
	void fix_fn_result(pin<ast::MkLambda> fn, pin<ast::Type> expected_result_type) {
		if (dom::isa<ast::TpVoid>(*expected_result_type) && !dom::isa<ast::TpVoid>(*fn->body.back()->type())) {
			auto c_void = ast::make_at_location<ast::ConstVoid>(*fn->body.back());
			c_void->type_ = ast->tp_void();
			fn->body.push_back(c_void);
		} else {
			expect_type(fn->body.back(), expected_result_type, [] { return "checking actual result type against fn declaration"; });
		}
	}
	pin<ast::TpLambda> type_cold_lambda( // returns the `lambda_type` or type inferred from act_params and fn result
		pin<ast::TpColdLambda> cold,
		pin<ast::TpLambda> lambda_type, // null if unknown
		vector<own<Type>>* act_params, // actual param types, if `lambda_type` not null, points to its params, if not, it's a separte vector and type_cold_lambda can mutate it 
		ast::Action& node,
		const function<string()>& context)
	{
		if (lambda_type)
			cold->resolved = lambda_type;
		size_t act_params_count = act_params->size() - (lambda_type ? 1 : 0);
		for (auto& cold_callee : cold->callees) {
			auto fn = cold_callee.fn.pinned();
			bool has_underscore_param = fn->names.size() == 1 && fn->names.front()->name == "_";
			if (fn->names.size() != act_params_count) {
				if (has_underscore_param && act_params_count == 0) {
					fn->names.pop_back();
					has_underscore_param = false;
				} else {
					node.error("Mismatched params count: expected ", fn->names.size(), " provided ", act_params_count, " see function definition:", *fn);
				}
			}
			for (size_t i = 0; i < fn->names.size(); i++)
				fn->names[i]->type = Type::promote((*act_params)[i]);
			auto prev_underscore_param = current_underscore_var;
			if (has_underscore_param)
				current_underscore_var = fn->names.front();
			for (auto& p : fn->body)
				find_type(p);
			current_underscore_var = prev_underscore_param;
			auto& fn_result = find_type(fn->body.back())->type();
			if (!lambda_type) {
				act_params->push_back(fn_result);
				if (dom::strict_cast<ast::TpColdLambda>(fn_result) || dom::strict_cast<ast::TpLambda>(fn_result))
					fn->error("Functions cannot return lambdas");
				lambda_type = ast->tp_lambda(move(*act_params));
				act_params = &lambda_type->params;
				cold->resolved = lambda_type;
			} else {
				fix_fn_result(fn, Type::promote(lambda_type->params.back()));
			}
		}
		return lambda_type;
	}
	void type_call(ast::Action& node, pin<ast::Action> callee, vector<own<ast::Action>>& actual_params) {
		pin<Type> callee_type = callee->type();
		if (auto as_fn = dom::strict_cast<ast::TpFunction>(callee_type)) {
			check_fn_proto(node, *as_fn, actual_params, *callee);
		} else if (auto as_lambda = dom::strict_cast<ast::TpLambda>(callee_type)) {
			check_fn_proto(node, *as_lambda, actual_params, *callee);
		} else if (auto as_delegate = dom::strict_cast<ast::TpDelegate>(callee_type)) {
			check_fn_proto(node, *as_delegate, actual_params, *callee);
			if (!dom::isa<ast::MakeDelegate>(*callee))
				node.type_ = ast->tp_optional(node.type_);
		} else if (auto as_cold = dom::strict_cast<ast::TpColdLambda>(callee_type)) {
			vector<own<Type>> param_types;
			for (auto& p : actual_params)
				param_types.push_back(p->type());
			auto lambda_type = type_cold_lambda(as_cold, nullptr, &param_types, node, [] { return ""; });
			callee->type_ = lambda_type;
			node.type_ = lambda_type->params.back();
		} else {
			node.error(callee_type, " is not callable");
		}
	}
	void on_call(ast::Call& node) override {
		for (auto& p : node.params)
			find_type(p);
		auto callee_type = find_type(node.callee)->type();
		if (auto callee_cls = ast->extract_class(callee_type)) {
			if (!callee_cls->get_implementation()->handle_member(
				node,
				ast::LongName{ "call" },
				[&](auto& field) {
					auto r = ast::make_at_location<ast::GetField>(*node.callee);
					r->base = move(node.callee);
					r->field = field;
					node.callee = move(r);
				},
				[&](auto& method) {
					auto r = ast::make_at_location<ast::MakeDelegate>(*node.callee);
					r->base = move(node.callee);
					r->method = method;
					node.callee = move(r);
				},
				[&] () {
					node.error("class ", callee_cls->get_name(), " has ambigous member 'call'");
				})) {
				node.error("class ", callee_cls->get_name(), " has no member 'call'");
			}
		}
		type_call(node, find_type(node.callee), node.params);
		if (auto as_mk_delegate = dom::strict_cast<ast::MakeDelegate>(node.callee)) {
			if (dom::isa<ast::TpWeak>(*as_mk_delegate->base->type()))
				node.error("Weak pointer delegate can be called only async"); // TODO: replace Call(MkDelegate) with Invoke
			if (as_mk_delegate->method->is_factory)
				node.type_ = as_mk_delegate->base->type();  // preserve both own/ref and actual this type.
		}
		node.returns_last_param_if_void = node.returns_last_param_if_void
			&& dom::isa<ast::TpVoid>(*node.type())
			&& !node.params.empty();
		if (node.returns_last_param_if_void)
			node.type_ = node.params.back()->type();
	}
	void on_async_call(ast::AsyncCall& node) override {
		for (auto& p : node.params)
			find_type(p);
		if (auto calle_as_imm = dom::strict_cast<ast::ImmediateDelegate>(node.callee)) {
			for (size_t i = 1; i < calle_as_imm->names.size(); i++) {
				auto& act_p = calle_as_imm->names[i];
				if (!act_p->initializer)
					act_p->type = node.params[i - 1]->type();
			}
		}
		type_call(node, find_type(node.callee), node.params);
		if (!dom::isa<ast::TpDelegate>(*node.callee->type()))
			node.error("only delegates can be called asynchronously");
		node.type_ = ast->tp_void();
	}
	void handle_index_op(ast::GetAtIndex& node, own<ast::Action> opt_value, const string& name) {
		auto indexed = ast->extract_class(find_type(node.indexed)->type());
		if (!indexed)
			node.error("Only objects can be indexed, not ", node.indexed->type());
		auto indexed_cls = indexed->get_implementation();
		if (auto m = dom::peek(indexed_cls->this_names, ast::LongName { name, nullptr })) {
			if (auto method = dom::strict_cast<ast::Method>(m)) {
				auto r = ast::make_at_location<ast::Call>(node).owned();
				auto callee = ast::make_at_location<ast::MakeDelegate>(node);
				r->callee = callee;
				r->returns_last_param_if_void = opt_value != nullptr;
				callee->base = move(node.indexed);
				callee->method = method;
				r->params = move(node.indexes);
				if (opt_value)
					r->params.push_back(move(opt_value));
				fix(r);
				*fix_result = move(r);
				return;
			}
			node.error(name, " is not a method in ", node.indexed);
		}
		if (auto fn = indexed_cls->module->functions[name + indexed_cls->name].pinned()) {
			auto fnref = ast::make_at_location<ast::MakeFnPtr>(node);
			fnref->fn = fn;
			auto r = ast::make_at_location<ast::Call>(node).owned();
			r->returns_last_param_if_void = opt_value != nullptr;
			r->callee = fnref;
			r->params = move(node.indexes);
			r->params.insert(r->params.begin(), move(node.indexed));
			if (opt_value)
				r->params.push_back(move(opt_value));
			fix(r);
			*fix_result = move(r);
			return;
		}
		node.error("function ", indexed_cls->module->name, "_", name, indexed_cls->name, " or method ", node.indexed->type().pinned(), ".", name, " not found");
	}
	void on_to_str(ast::ToStrOp& node) override {
		auto stream_class = ast->extract_class(find_type(node.p[0])->type());
		if (!stream_class)
			node.error("Only objects can be to_string receivers, not ", node.p[1]->type());
		struct TypeNameGenerator : ast::TypeMatcher {
			string result;
			ast::Node& node;
			ast::Ast& ast;
			TypeNameGenerator(ast::Node& node, ast::Ast& ast) :node(node), ast(ast) {}
			void on_int32(ast::TpInt32& type) override { result = "Int32"; }
			void on_int64(ast::TpInt64& type) override { result = "Int"; }
			void on_float(ast::TpFloat& type) override { result = "Float"; }
			void on_double(ast::TpDouble& type) override { result = "Double"; }
			void on_function(ast::TpFunction& type) override { error(type); }
			void on_lambda(ast::TpLambda& type) override { error(type); }
			void on_delegate(ast::TpDelegate& type) override { error(type); }
			void on_cold_lambda(ast::TpColdLambda& type) override { error(type); }
			void on_void(ast::TpVoid& type) override { result = "Void"; }
			void on_no_ret(ast::TpNoRet& type) override { error(type); }
			void on_optional(ast::TpOptional& type) override {
				ast.get_wrapped(&type)->match(*this);
				result = ast::format_str("Opt", result);
			}
			void on_own(ast::TpOwn& type) override { ptr_type(type); }
			void on_ref(ast::TpRef& type) override { ptr_type(type); }
			void on_shared(ast::TpShared& type) override { ptr_type(type); }
			void on_weak(ast::TpWeak& type) override { error(type); }
			void on_frozen_weak(ast::TpFrozenWeak& type) override { error(type); }
			void on_conform_ref(ast::TpConformRef& type) override { ptr_type(type); }
			void on_conform_weak(ast::TpConformWeak& type) override { error(type); }
			void on_enum(ast::TpEnum& type) override { result = ast::format_str(type.def->module->name, type.def->name); }
			void error(ast::Type& type) {
				node.error("Expected printable type, not ", ltm::pin<ast::Type>(&type));
			}
			void ptr_type(ast::TpOwn& type) {
				result = type.target == ast.string_cls.pinned() ? "Str" : "Obj";
			}
		} type_name_gen(node, *ast);
		find_type(node.p[1])->type()->match(type_name_gen);
		auto methodName = ast::format_str("put", type_name_gen.result);
		if (auto m = dom::peek(stream_class->get_implementation()->this_names, ast::LongName { methodName, nullptr })) {
			if (auto method = dom::strict_cast<ast::Method>(m)) {
				auto callee = ast::make_at_location<ast::MakeDelegate>(node);
				callee->base = move(node.p[0]);
				callee->method = method;
				auto r = ast::make_at_location<ast::Call>(node).owned();
				r->callee = callee;
				r->params.push_back(move(node.p[1]));
				fix(r);
				*fix_result = move(r);
				return;
			}
			node.error(methodName, " is not a method in ", node.p[0]->type());
		}
		node.error("method ", stream_class->get_name(), ".", methodName, " not found");
	}
	void on_get_at_index(ast::GetAtIndex& node) override { handle_index_op(node, nullptr, "getAt"); }
	void on_set_at_index(ast::SetAtIndex& node) override { handle_index_op(node, move(node.value), "setAt"); }
	pin<ast::Function> type_fn(pin<ast::Function> fn) {
		if (!fn->type_ || fn->type_ == type_in_progress) {
			bool is_method = dom::isa<ast::Method>(*fn) || dom::isa<ast::ImmediateDelegate>(*fn);
			vector<own<Type>> params;
			pin<ast::Module> prevm = current_module;
			current_module = fn->module;
			for (size_t i = 0; i < fn->names.size(); i++) {
				auto& p = fn->names[i];
				if (!p->type)
					p->type = find_type(p->initializer)->type();
				if (i > 0 || !is_method)
					params.push_back(Type::promote(p->type));
			}
			params.push_back(find_type(fn->type_expression)->type());
			fn->type_ = is_method
				? ast->tp_delegate(move(params))
				: ast->tp_function(move(params));
			current_module = prevm;
		}
		return fn;
	}
	pin<ast::Type> remove_member_type_params(pin<ast::AbstractClass> base_cls, pin<ast::Class> member_cls, pin<ast::Type> type_to_convert) {
		if (member_cls->params.empty())
			return type_to_convert;
		auto base_as_inst = dom::strict_cast<ast::ClassInstance>(base_cls);
		auto base_cls_ctx = dom::peek(base_cls->get_implementation()->base_contexts, member_cls);
		auto ctx = base_cls_ctx
			? dom::strict_cast<ast::ClassInstance>(ast->resolve_params(base_cls_ctx, base_as_inst))
			: base_as_inst;
		if (!ctx)
			return type_to_convert;
		return remove_params(type_to_convert, *ast, ctx);
	}
	void on_make_delegate(ast::MakeDelegate& node) override {
		node.type_ = remove_member_type_params(
			class_from_action(node.base),
			node.method->cls,
			type_fn(node.method)->type_);
		if (dom::isa<ast::TpConformRef>(*node.base->type())) {
			if (node.method->mut != ast::Mut::ANY)
				node.error("cannot call mutating or shared methods on maybe-frozen object");
		} else if (dom::isa<ast::TpShared>(*node.base->type())) {
			if (node.method->mut == ast::Mut::MUTATING)
				node.error("cannot call a mutating method on a shared object");
		} else {
			if (node.method->mut == ast::Mut::FROZEN)
				node.error("cannot call a *method on a non-shared object");
		}
	}
	void expect_numeric(ast::Type& t, ast::Node& node, const char* context) {
		 if(!is_numeric(t))
			node.error(context, " needs int or floating type");
	}
	void on_to_int32(ast::ToInt32Op& node) override {
		node.type_ = ast->tp_int32();
		expect_numeric(*find_type(node.p)->type(), *node.p, "convertion to int32");
	}
	void on_to_int(ast::ToIntOp& node) override {
		node.type_ = ast->tp_int64();
		expect_numeric(*find_type(node.p)->type(), *node.p, "convertion to int");
	}
	void on_to_double(ast::ToDoubleOp& node) override {
		node.type_ = ast->tp_double();
		expect_numeric(*find_type(node.p)->type(), *node.p, "convertion to double");
	}
	void on_to_float(ast::ToFloatOp& node) override {
		node.type_ = ast->tp_float();
		expect_numeric(*find_type(node.p)->type(), *node.p, "convertion to float");
	}
	void on_copy(ast::CopyOp& node) override {
		auto param_type = find_type(node.p)->type();
		if (auto src_cls = ast->extract_class(param_type))
			node.type_ = ast->get_own(src_cls);
		else
			node.error("copy operand should be a reference, not ", param_type.pinned());
	}
	void on_not(ast::NotOp& node) override {
		node.type_ = tp_bool;
		handle_condition(node.p, [] { return "not operator"; });
	}
	void on_neg(ast::NegOp& node) override {
		node.type_ = find_type(node.p)->type();
		if (!is_numeric(*node.type_))
			node.p->error("expected int or double");
	}
	void on_inv(ast::InvOp& node) override {
		node.type_ = find_type(node.p)->type();
		if (!is_integer(*node.type_))
			node.p->error("expected int or int32");
	}
	void on_ref(ast::RefOp& node) override {
		auto as_own = dom::strict_cast<ast::TpOwn>(find_type(node.p)->type());
		if (!as_own)
			node.p->error("expected class or interface, not ", node.p->type().pinned());
		node.type_ = ast->get_ref(as_own->target);
	}
	void on_conform(ast::ConformOp& node) override {
		auto as_own = dom::strict_cast<ast::TpOwn>(find_type(node.p)->type());
		if (!as_own)
			node.p->error("expected class or interface, not ", node.p->type().pinned());
		node.type_ = ast->get_conform_ref(as_own->target);
	}
	void on_freeze(ast::FreezeOp& node) override {
		auto cls = class_from_action(node.p);
		node.type_ = ast->get_shared(cls);
	}
	void on_loop(ast::Loop& node) override {
		find_type(node.p);
		if (auto as_opt = dom::strict_cast<ast::TpOptional>(node.p->type())) {
			node.type_ = ast->get_wrapped(as_opt);
		} else if (!node.has_breaks){
			node.error("loop having no breaks, and body returned ", node.p->type().pinned(), ", that is not bool or optional");
		} else {
			node.type_ = ast->tp_no_ret();
		}
	}
	void on_get(ast::Get& node) override {
		if (!node.var && node.var_name == "_") {
			if (!current_underscore_var)
				node.error("No `_` variable in this scope");
			node.var = current_underscore_var;
		}
		if (node.var->is_const) {
			node.type_ = node.var->type;
		} else {
			node.type_ = ast->convert_maybe_optional(node.var->type, [&](auto tp) {
				if (auto as_own = dom::strict_cast<ast::TpOwn>(tp))
					return cast<ast::Type>(ast->get_ref(as_own->target));
				return tp;
			});
		}
	}
	void on_set(ast::Set& node) override {
		auto value_type = find_type(node.val)->type();
		auto& variable_type = node.var->type;
		if (auto value_as_own = dom::strict_cast<ast::TpOwn>(value_type)) {
			node.type_ = ast->get_ref(value_as_own->target);
			auto variable_as_own = dom::strict_cast<ast::TpOwn>(variable_type);
			if (!variable_as_own)
				value_type = ast->get_ref(value_as_own->target);
		} else {
			node.type_ = value_type;
			if (dom::strict_cast<ast::TpRef>(value_type)) {
				if (auto variable_as_own = dom::strict_cast<ast::TpOwn>(variable_type))
					variable_type = ast->get_ref(variable_as_own->target);
			}
		}
		expect_type(
			*fix_result,
			value_type,
			variable_type,
			[&] { return ast::format_str("assign to vaiable", node.var_name, node.var.pinned()); });
	}
	void on_mk_instance(ast::MkInstance& node) override {
		if (!node.cls)  // delegate type declaration this-param has no cls
			return;
		check_class_params(node.cls);
		node.type_ = ast->get_own(node.cls);
	}
	void on_make_fn_ptr(ast::MakeFnPtr& node) override {
		node.type_ = node.fn->type();
	}
	void on_mk_weak(ast::MkWeakOp& node) override {
		if (auto as_ref = dom::strict_cast<ast::TpRef>(find_type(node.p)->type())) {
			node.type_ = ast->get_weak(as_ref->target);
			return;
		}
		if (auto as_shared = dom::strict_cast<ast::TpShared>(find_type(node.p)->type())) {
			node.type_ = ast->get_frozen_weak(as_shared->target);
			return;
		}
		if (auto as_conform_ref = dom::strict_cast<ast::TpConformRef>(find_type(node.p)->type())) {
			node.type_ = ast->get_conform_weak(as_conform_ref->target);
			return;
		}
		if (auto as_mk_instance = dom::strict_cast<ast::MkInstance>(node.p)) {
			node.type_ = ast->get_weak(as_mk_instance->cls);
			return;
		}
		node.p->error("Expected &ClassName or expression returning reference, not expr of type ", node.p->type().pinned());
	}
	pin<ast::AbstractClass> class_from_action(own<ast::Action>& node, bool include_week = false) {
		auto node_type = find_type(node)->type();
		auto type_as_weak = dom::strict_cast<ast::TpWeak>(node_type);
		auto cls = type_as_weak && include_week
			? type_as_weak->target.pinned()
			: ast->extract_class(node_type);
		if (!cls)
			node->error("Expected pointer to class, not ", node->type().pinned());
		return cls;
	}
	void on_get_field(ast::GetField& node) override {
		if (auto as_enum = dom::strict_cast<ast::ConstEnumTag>(node.base)) {
			if (as_enum->value)
				node.error("unexpected enum tag name");
			auto& enum_def = dom::strict_cast<ast::TpEnum>(as_enum->type_)->def;
			pin<ast::EnumTag> tag;
			if (node.field_module) {
				tag = dom::peek(enum_def->tags, ast::format_str(node.field_module->name, "_", node.field_name));
				if (!tag)
					node.error("Unknown tag ", node.module->name, "_", node.field_name, " in enum", dom::strict_cast<ast::TpEnum>(as_enum->type_)->def->name);
			} else {
				tag = dom::peek(enum_def->tags, ast::format_str(enum_def->module->name, "_", node.field_name));
				if (!tag)
					dom::peek(enum_def->tags, ast::format_str(current_module->name, "_", node.field_name));
				if (!tag)
					node.error("Unknown tag ", enum_def->module->name, "_", node.field_name, " or ", 
						current_module->name, "_", node.field_name, " in enum", dom::strict_cast<ast::TpEnum>(as_enum->type_)->def->name);
			}
			as_enum->value = tag;
			*fix_result = move(node.base);
			return;
		}
		auto base_cls = class_from_action(node.base);
		if (!node.field) {
			if (!base_cls->get_implementation()->handle_member(node, ast::LongName{ node.field_name, node.field_module },
				[&](auto field) { node.field = field; },
				[&](auto method) {
					auto r = ast::make_at_location<ast::MakeDelegate>(node).owned();
					r->base = move(node.base);
					r->method = method;
					find_type(r);
					*fix_result = move(r);
				},
				[&] { node.error("field/method name is ambigiuous, use cast"); }))
				node.error("class ", base_cls->get_name(), " doesn't have field/method ", ast::LongName{node.field_name, node.field_module});
		}
		if (&node != fix_result->pinned())
			return;
		node.type_ = remove_member_type_params(
			base_cls,
			node.field->cls,
			find_type(node.field->initializer)->type());
		if (dom::isa<ast::TpConformRef>(*node.base->type())) {
			node.type_ = ast->convert_maybe_optional(node.type(), [&](auto tp) {
				if (auto as_own = dom::strict_cast<ast::TpOwn>(tp))
					return cast<ast::Type>(ast->get_conform_ref(as_own->target));
				if (auto as_weak = dom::strict_cast<ast::TpWeak>(tp))
					return cast<ast::Type>(ast->get_conform_weak(as_weak->target));
				return tp;
			});

		} else if (dom::isa<ast::TpShared>(*node.base->type())) {
			node.type_ = ast->convert_maybe_optional(node.type(), [&](auto tp) {
				if (auto as_own = dom::strict_cast<ast::TpOwn>(tp))
					return cast<ast::Type>(ast->get_shared(as_own->target));
				if (auto as_weak = dom::strict_cast<ast::TpWeak>(tp))
					return cast<ast::Type>(ast->get_frozen_weak(as_weak->target));
				return tp;
			});
		} else {
			node.type_ = ast->convert_maybe_optional(node.type(), [&](auto tp) {
				if (auto as_own = dom::strict_cast<ast::TpOwn>(tp))
					return cast<ast::Type>(ast->get_ref(as_own->target));
				return tp;
			});
		}
	}
	void check_class_params(pin<ast::AbstractClass> cls) {
		if (cls->inst_mode() == ast::AbstractClass::InstMode::direct)
			return;
		if (auto as_param = dom::strict_cast<ast::ClassParam>(cls)) {
			if (dom::isa<ast::ClassParam>(*as_param->base.pinned()))
				cls->error("Class parameter cannot be bound to another parameter");
			cls = as_param->base;
			return;
		}
		if (auto as_cls = dom::strict_cast<ast::Class>(cls)) {
			if (as_cls != this_class && !as_cls->params.empty())
				cls->error("expected class parameters");
			return;
		}
		auto as_inst = dom::strict_cast<ast::ClassInstance>(cls);
		auto ctr = as_inst->params[0].pinned();
		if (dom::isa<ast::ClassInstance>(*ctr))
			cls->error("Doubly parameterized class");
		if (dom::isa<ast::ClassParam>(*ctr))
			cls->error("Class parameter cannot be a class constructor");
		auto as_cls = dom::strict_cast<ast::Class>(ctr);
		if (as_cls->params.size() != as_inst->params.size() - 1)
			cls->error("Expected ", as_cls->params.size(), " parameters");
		for (size_t i = 0; i < as_cls->params.size(); i++) {
			if (!is_compatible(as_inst->params[i + 1], as_cls->params[i]->base))
				cls->error("Expected ", as_cls->params[i]->base->get_name(), " not ", as_inst->params[i + 1]);
		}
	}
	// Takes a type and returns another type with all associated classes converted with class-bound `remove_params`
	static pin<ast::Type> remove_params(pin<ast::Type> type, ast::Ast& ast, pin<ast::ClassInstance> context) {
		struct ParamRemover : ast::TypeMatcher{
			pin<ast::Type> r;
			pin<ast::ClassInstance> ctx;
			ast::Ast& ast;
			ParamRemover(pin<ast::ClassInstance> ctx, ast::Ast& ast) : ctx(move(ctx)), ast(ast) {}
			vector<own<Type>> convert_params(vector<own<ast::Type>>& params) {
				vector<own<Type>> r;
				for (auto& p : params)
					r.push_back(remove_params(p, ast, ctx));
				return r;
			}
			void on_int32(ast::TpInt32& type) override { r = &type; }
			void on_int64(ast::TpInt64& type) override { r = &type; }
			void on_float(ast::TpFloat& type) override { r = &type; }
			void on_double(ast::TpDouble& type) override { r = &type; }
			void on_function(ast::TpFunction& type) override { r = ast.tp_function(convert_params(type.params)); }
			void on_lambda(ast::TpLambda& type) override { r = ast.tp_lambda(convert_params(type.params)); }
			void on_delegate(ast::TpDelegate& type) override { r = ast.tp_delegate(convert_params(type.params)); }
			void on_cold_lambda(ast::TpColdLambda& type) override {
				if (!type.resolved)
					r = &type;
				else
					type.resolved->match(*this);
			}
			void on_void(ast::TpVoid& type) override { r = &type; }
			void on_no_ret(ast::TpNoRet& type) override { r = &type; }
			void on_optional(ast::TpOptional& type) override { r = ast.tp_optional(remove_params(ast.get_wrapped(&type), ast, ctx)); }
			void on_own(ast::TpOwn& type) override { r = ast.get_own(ast.resolve_params(type.target, ctx)); }
			void on_ref(ast::TpRef& type) override { r = ast.get_ref(ast.resolve_params(type.target, ctx)); }
			void on_shared(ast::TpShared& type) override { r = ast.get_shared(ast.resolve_params(type.target, ctx)); }
			void on_weak(ast::TpWeak& type) override { r = ast.get_weak(ast.resolve_params(type.target, ctx)); }
			void on_frozen_weak(ast::TpFrozenWeak& type) override { r = ast.get_frozen_weak(ast.resolve_params(type.target, ctx)); }
			void on_conform_ref(ast::TpConformRef& type) override { r = ast.get_conform_ref(ast.resolve_params(type.target, ctx)); }
			void on_conform_weak(ast::TpConformWeak& type) override { r = ast.get_conform_weak(ast.resolve_params(type.target, ctx)); }
			void on_enum(ast::TpEnum& type) override { r = &type; }
		};
		ParamRemover pr(move(context), ast);
		type->match(pr);
		return pr.r;
	}
	void resolve_set_field(ast::SetField& node) {
		auto base_cls = class_from_action(node.base);
		if (!node.field) {
			if (!base_cls->get_implementation()->handle_member(node, ast::LongName{node.field_name, node.field_module},
				[&](auto field) { node.field = field; },
				[&](auto method) { node.error("Cannot assign to method"); },
				[&] { node.error("field name is ambiguous, use cast"); }))
				node.error("class ", base_cls->get_name(), " doesn't have field/method ", ast::LongName{node.field_name, node.field_module});
		}
		if (dom::isa<ast::TpShared>(*node.base->type()) && dom::isa<ast::TpConformRef>(*node.base->type()))
			node.error("Cannot assign to a shared/conform object field ", ast::LongName{ node.field_name, node.field_module });
		node.type_ = remove_member_type_params(
			base_cls,
			node.field->cls,
			find_type(node.field->initializer)->type());
		node.type_ = ast->convert_maybe_optional(node.type(), [&](auto tp) {
			if (auto as_own = dom::strict_cast<ast::TpOwn>(tp))
				return cast<ast::Type>(ast->get_ref(as_own->target));
			return tp;
		});
	}
	void on_set_field(ast::SetField& node) override {
		resolve_set_field(node);
		expect_type(find_type(node.val), node.field->initializer->type(), [&] {
			return ast::format_str("assign to field ", ast::LongName{ node.field_name, node.field_module }, *node.field.pinned());
		});
	}
	void on_splice_field(ast::SpliceField& node) override {
		resolve_set_field(node);
		auto ft = node.field->initializer->type();
		if (auto as_opt = dom::strict_cast<ast::TpOptional>(ft))
			ft = as_opt->wrapped;
		if (!dom::isa<ast::TpOwn>(*ft))
			node.error("Field must be @-pointer, not ", node.field->initializer->type().pinned());
		expect_type(find_type(node.val), node.type_, [&] {
			return ast::format_str("splice field ", ast::LongName{ node.field_name, node.field_module }, *node.field.pinned());
		});
		node.type_ = tp_bool;
	}
	void on_cast(ast::CastOp& node) override {
		auto src_cls = class_from_action(node.p[0]);
		auto dst_cls = class_from_action(node.p[1]);
		check_class_params(dst_cls);
		node.type_ = dom::isa<ast::TpOwn>(*node.p[0]->type())
			? (pin<Type>) ast->get_own(dst_cls)
			: (pin<Type>) ast->get_ref(dst_cls);
		if (is_compatible(src_cls, dst_cls))  // no-op conversion
			node.p[1] = nullptr;
		else
			node.type_ = ast->tp_optional(node.type_);
	}
	void on_add(ast::AddOp& node) override { on_int_or_double_op(node, [] { return "add operator"; }); }
	void on_sub(ast::SubOp& node) override { on_int_or_double_op(node, [] { return "subtract operator"; }); }
	void on_mul(ast::MulOp& node) override { on_int_or_double_op(node, [] { return "mul operator"; }); }
	void on_div(ast::DivOp& node) override { on_int_or_double_op(node, [] { return "div operator"; }); }
	void on_mod(ast::ModOp& node) override { on_int_op(node, [] { return "mod operator"; }); }
	void on_and(ast::AndOp& node) override { on_int_op(node, [] { return "bitwise and operator"; }); }
	void on_or(ast::OrOp& node) override { on_int_op(node, [] { return "bitwise or operator"; }); }
	void on_xor(ast::XorOp& node) override { on_int_op(node, [] { return "xor operator"; }); }
	void on_shl(ast::ShlOp& node) override { on_int_op(node, [] { return "shl operator"; }); }
	void on_shr(ast::ShrOp& node) override { on_int_op(node, [] { return "shr operator"; }); }
	void on_lt(ast::LtOp& node) override {
		on_int_or_double_op(node, [] { return "ordering operator"; });
		node.type_ = tp_bool;
	}
	void on_eq(ast::EqOp& node) override {
		auto& t0 = find_type(node.p[0])->type();
		node.type_ = tp_bool;
		expect_type(find_type(node.p[1]), t0, [] { return "equality operator"; });
	}
	pin<ast::TpOptional> handle_condition(own<ast::Action>& action, const function<string()>& context) {
		auto cond = dom::strict_cast<ast::TpOptional>(find_type(action)->type());
		if (cond)
			return cond;
		if (auto as_weak = dom::strict_cast<ast::TpWeak>(action->type())) {
			auto deref = ast::make_at_location<ast::DerefWeakOp>(*action);
			deref->p = move(action);
			deref->type_ = cond = ast->tp_optional(ast->get_ref(as_weak->target));
			action = deref;
			return cond;
		}
		if (auto as_frozen_weak = dom::strict_cast<ast::TpFrozenWeak>(action->type())) {
			auto deref = ast::make_at_location<ast::DerefWeakOp>(*action);
			deref->p = move(action);
			deref->type_ = cond = ast->tp_optional(ast->get_shared(as_frozen_weak->target));
			action = deref;
			return cond;
		}
		if (auto as_conform_weak = dom::strict_cast<ast::TpConformWeak>(action->type())) {
			auto deref = ast::make_at_location<ast::DerefWeakOp>(*action);
			deref->p = move(action);
			deref->type_ = cond = ast->tp_optional(ast->get_conform_ref(as_conform_weak->target));
			action = deref;
			return cond;
		}
		action->context_error(context, "expected optional, weak or bool type, not ", action->type().pinned());
	}
	void handle_maybe_block_with_underscore(own<ast::Action>& action, pin<Type> external_type) {
		auto prev_underscore = current_underscore_var;
		if (auto as_block = dom::strict_cast<ast::Block>(action)) {
			if (!as_block->names.empty() && !as_block->names.front()->initializer) {
				as_block->names.front()->type = external_type;
				if (as_block->names.front()->name == "_") {
					if (dom::isa<ast::TpVoid>(*external_type))
						as_block->names.erase(as_block->names.begin());
					else
						current_underscore_var = as_block->names.front();
				}
			}
		}
		find_type(action);
		current_underscore_var = prev_underscore;
	}
	void on_if(ast::If& node) override {
		auto cond = handle_condition(node.p[0], [] { return "if operator condition"; });
		handle_maybe_block_with_underscore(node.p[1], ast->get_wrapped(cond));
		node.type_ = ast->tp_optional(node.p[1]->type());
	}
	void on_land(ast::LAnd& node) override {
		auto cond = handle_condition(node.p[0], [] { return "1st operand of `logical and` operator"; });
		handle_maybe_block_with_underscore(node.p[1], ast->get_wrapped(cond));
		node.type_ = node.p[1]->type();
		if (!dom::isa<ast::TpOptional>(*node.type_) && !dom::isa<ast::TpWeak>(*node.type_))
			node.p[1]->error("expected bool, weak or optional as a 2nd operand of ``logical and` operator");
	}

	void on_else(ast::Else& node) override {
		node.type_ = ast->get_wrapped(handle_condition(node.p[0], [] { return "1st operand of `else` operator"; }));
		expect_type(find_type(node.p[1]), node.type_, [] { return "2nd opearnd of else operator"; });
	}

	void on_lor(ast::LOr& node) override {
		node.type_ = handle_condition(node.p[0], [] { return "1st operand of `logical or` operator"; });
		expect_type(find_type(node.p[1]), node.type_, [] { return "2nd operand of `logical or` operator"; });
		assert(dom::strict_cast<ast::TpOptional>(node.type_));
	}

	void on_immediate_delegate(ast::ImmediateDelegate& node) override {
		if (!node.base) {
			type_fn(&node);
			return;
		}
		auto cls = class_from_action(node.base, true); // include week
		resolve_immediate_delegate(ast, node, cls->get_implementation());
		dom::strict_cast<ast::MkInstance>(node.names[0]->initializer)->cls = cls;
		type_fn(&node);
		for (auto& a : node.body)
			find_type(a);
		expect_type(node.body.back(), node.type_expression->type(), [] { return "checking immediate delegate result against declared type"; });
	}

	own<ast::Action>& find_type(own<ast::Action>& node) {
		if (auto type = node->type()) {
			if (type == type_in_progress)
				node->error("cannot deduce type due circular references"); // TODO: switch to Hindley-Milner++
			return node;
		}
		node->type_ = type_in_progress;
		fix(node);
		return node;
	}

	void expect_type(own<ast::Action>& node, pin<Type> expected_type, const function<string()>& context) {
		expect_type(node, node->type(), expected_type, context);
	}
	bool is_compatible(pin<ast::AbstractClass> actual, pin<ast::AbstractClass> expected) {
		if (actual != expected && !actual->get_implementation()->overloads.count(expected->get_implementation()))
			return false;
		auto act_as_inst = dom::strict_cast<ast::ClassInstance>(actual);
		auto exp_as_inst = dom::strict_cast<ast::ClassInstance>(expected);
		if (!act_as_inst)
			return !act_as_inst;
		if (!exp_as_inst)
			return true;
		if (exp_as_inst->params.size() != act_as_inst->params.size())
			return false;
		for (size_t i = 0; i < act_as_inst->params.size(); i++) {
			if (act_as_inst->params[i] != exp_as_inst->params[i])
				return false; // TODO, support co- and contravariance.
		}
		return true;
	}
	void expect_type(own<ast::Action>& node, pin<Type> actual_type, pin<Type> expected_type, const function<string()>& context) {
		if (expected_type == ast->tp_void())
			return;
		if (actual_type == expected_type)
			return;
		if (actual_type == tp_no_ret || expected_type == tp_no_ret)
			return;
		if (auto exp_as_ref = dom::strict_cast<ast::TpRef>(expected_type)) {
			if (dom::isa<ast::TpShared>(*actual_type) || dom::isa<ast::TpConformRef>(*actual_type))
				node->context_error(context, "expected mutable type, not ", actual_type);
			if (auto actual_class = ast->extract_class(actual_type)) {
				if (is_compatible(actual_class, exp_as_ref->target))
					return;
			}
		} else if (auto exp_as_own = dom::strict_cast<ast::TpOwn>(expected_type)) {
			if (auto actual_as_own = dom::strict_cast<ast::TpOwn>(actual_type)) {
				if (is_compatible(actual_as_own->target, exp_as_own->target))
					return;
			}
		} else if (auto exp_as_shared = dom::strict_cast<ast::TpShared>(expected_type)) {
			if (auto actual_as_shared = dom::strict_cast<ast::TpShared>(actual_type)) {
				if (is_compatible(actual_as_shared->target, exp_as_shared->target))
					return;
			}
		} else if (auto exp_as_conform_ref = dom::strict_cast<ast::TpConformRef>(expected_type)) {
			if (auto actual_as_conform_ref = dom::strict_cast<ast::TpConformRef>(actual_type)) {
				if (is_compatible(actual_as_conform_ref->target, exp_as_conform_ref->target))
					return;
			}
			if (auto actual_as_ref = dom::strict_cast<ast::TpRef>(actual_type)) {
				if (is_compatible(actual_as_ref->target, exp_as_conform_ref->target))
					return;
			}
			if (auto actual_as_shared = dom::strict_cast<ast::TpShared>(actual_type)) {
				if (is_compatible(actual_as_shared->target, exp_as_conform_ref->target))
					return;
			}
			if (auto actual_as_own = dom::strict_cast<ast::TpOwn>(actual_type)) {
				if (is_compatible(actual_as_own->target, exp_as_conform_ref->target))
					return;
			}
		} else if (auto exp_as_weak = dom::strict_cast<ast::TpWeak>(expected_type)) {
			if (auto actual_as_weak = dom::strict_cast<ast::TpWeak>(actual_type)) {
				if (is_compatible(actual_as_weak->target, exp_as_weak->target))
					return;
			}
		} else if (auto exp_as_frozen_weak = dom::strict_cast<ast::TpFrozenWeak>(expected_type)) {
			if (auto actual_as_frozen_weak = dom::strict_cast<ast::TpFrozenWeak>(actual_type)) {
				if (is_compatible(actual_as_frozen_weak->target, exp_as_frozen_weak->target))
					return;
			}
		} else if (auto exp_as_conform_weak = dom::strict_cast<ast::TpConformWeak>(expected_type)) {
			if (auto actual_as_conform_weak = dom::strict_cast<ast::TpConformWeak>(actual_type)) {
				if (is_compatible(actual_as_conform_weak->target, exp_as_conform_weak->target))
					return;
			}
			if (auto actual_as_weak = dom::strict_cast<ast::TpWeak>(actual_type)) {
				if (is_compatible(actual_as_weak->target, exp_as_conform_weak->target))
					return;
			}
			if (auto actual_as_frozen_weak = dom::strict_cast<ast::TpFrozenWeak>(actual_type)) {
				if (is_compatible(actual_as_frozen_weak->target, exp_as_conform_weak->target))
					return;
			}
		} else if (auto act_as_cold = dom::strict_cast<ast::TpColdLambda>(actual_type)) {
			if (auto exp_as_cold = dom::strict_cast<ast::TpColdLambda>(expected_type)) {
				assert(!act_as_cold->resolved);  // node->type() skips all resolved cold levels
				act_as_cold->resolved = exp_as_cold;
				for (auto& fn : act_as_cold->callees)
					exp_as_cold->callees.push_back(move(fn));
				act_as_cold->callees.clear();
				return;
			} else if (auto exp_as_lambda = dom::strict_cast<ast::TpLambda>(expected_type)) {
				type_cold_lambda(act_as_cold, exp_as_lambda, &exp_as_lambda->params, *node, context);
				return;
			}
		} else if (auto act_as_lambda = dom::strict_cast<ast::TpLambda>(actual_type)) {
			if (auto exp_as_cold = dom::strict_cast<ast::TpColdLambda>(expected_type)) {
				type_cold_lambda(exp_as_cold, act_as_lambda, &act_as_lambda->params, *node, context);
				return;
			}
		} else if (auto exp_as_opt = dom::strict_cast<ast::TpOptional>(expected_type)) {
			if (auto act_as_opt = dom::strict_cast<ast::TpOptional>(actual_type)) {
				if (act_as_opt->depth == exp_as_opt->depth) {
					expect_type(node, act_as_opt->wrapped, exp_as_opt->wrapped, context);
					return;
				}
			} else {
				expect_type(node, actual_type, exp_as_opt->wrapped, [&] { return ast::format_str("trying to convert to optional while ", context()); });
				for (size_t d = exp_as_opt->depth + 1; d; d--) {
					auto r = ast::make_at_location<ast::If>(*node);
					auto cond = ast::make_at_location<ast::ConstBool>(*node);
					cond->value = true;
					r->p[0] = cond;
					r->p[1] = move(node);
					node = move(r);
				}
				find_type(node);
				return;
			}
		} else if (auto exp_as_lambda = dom::strict_cast<ast::TpLambda>(expected_type)) {
			if (exp_as_lambda->params.size() == 1) {
				auto actual_as_own = dom::strict_cast<ast::TpOwn>(actual_type);
				auto result_as_own = dom::strict_cast<ast::TpOwn>(exp_as_lambda->params.back());
				if (actual_as_own && result_as_own && is_compatible(actual_as_own->target, result_as_own->target)) {
					auto r = ast::make_at_location<ast::MkLambda>(*node);
					r->body.push_back(move(node));
					r->type_ = ast->tp_lambda({ actual_type });
					node = move(r);
					return;
				}
			}
		}
		node->context_error(context, "Expected type: ", expected_type, " not ", actual_type);
	}

	void process_method(own<ast::Method>& m) {
		pin<ast::Module> prevm = current_module;
		current_module = m->module;
		m->names[0]->type = find_type(m->names[0]->initializer)->type();
		if (m->mut == ast::Mut::FROZEN) {
			m->names[0]->type = ast->get_shared(ast->extract_class(m->names[0]->type));
		} else if (m->mut == ast::Mut::ANY) {
			m->names[0]->type = ast->get_conform_ref(ast->extract_class(m->names[0]->type));
		}
		for (auto& a : m->body)
			find_type(a);
		if (!m->body.empty())  // empty == interface method
			fix_fn_result(m, m->type_expression->type());
		if (m->base)
			expect_type(m, m->base->type(), [] { return "checking method type against overridden"; });
		current_module = prevm;
	}

	void process() {
		for (auto& c : ast->classes_in_order) {
			this_class = c;
			for (auto& m : c->new_methods)
				type_fn(m);
			for (auto& b : c->overloads)
				for (auto& m : b.second)
					type_fn(m);
		}
		for (auto& m : ast->modules) {
			current_module = m.second;
			for (auto& ct : m.second->constants) {
				auto tp = ct.second->type = find_type(ct.second->initializer)->type();
				if (auto as_opt = dom::strict_cast<ast::TpOptional>(tp))
					tp = as_opt->wrapped;
				else if (dom::isa<ast::TpVoid>(*tp))
					ct.second->error("Constant cannot be of type void.");
				if (dom::isa<ast::TpOwn>(*tp)
					|| dom::isa<ast::TpRef>(*tp)
					|| dom::isa<ast::TpWeak>(*tp)
					|| dom::isa<ast::TpConformRef>(*tp)
					|| dom::isa<ast::TpConformWeak>(*tp)) {
					ct.second->error("Constants cannot be mutable objects @own, ref or &weak. Make them *frozen or &*frozen weaks.");
				}
			}
			for (auto& fn : m.second->functions)
				type_fn(fn.second);
		}
		for (auto& c : ast->classes_in_order) {
			this_class = c;
			for (auto& f : c->fields) {
				current_module = f->module;
				find_type(f->initializer);
				if (dom::isa<ast::TpRef>(*f->initializer->type()) || dom::isa<ast::TpConformRef>(*f->initializer->type())) {
					f->error("Fields cannot be temp-references. Make it @own, *shared or &weak");
				}
			}
			for (auto& m : c->new_methods)
				process_method(m);
			for (auto& b : c->overloads)
				for (auto& m : b.second)
					process_method(m);
		}
		for (auto& m : ast->modules) {
			current_module = m.second;
			for (auto& fn : m.second->functions) {
				for (auto& a : fn.second->body)
					find_type(a);
				if (!fn.second->is_platform)
					fix_fn_result(fn.second, fn.second->type_expression->type());
			}
		}
		expect_type(find_type(ast->starting_module->entry_point), ast->tp_lambda({ ast->tp_void() }), []{ return "main fn return value"; });
		for (auto& m : ast->modules) {
			current_module = m.second;
			for (auto& t : m.second->tests) {
				type_fn(t.second);
				if (!t.second->names.empty())
					t.second->error("tests should not have parameters");
				for (auto& a : t.second->body)
					find_type(a);
			}
		}
	}
};

}  // namespace

void check_types(ltm::pin<ast::Ast> ast) {
	Typer(ast).process();
}
