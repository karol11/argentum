#include "compiler/type-checker.h"

#include <algorithm>
#include <vector>
#include <cassert>
#include "name-resolver.h"

namespace {

	using std::vector;
using ltm::own;
using ltm::pin;
using ltm::weak;
using dom::strict_cast;
using ast::Type;
using std::function;
using std::string;

auto type_in_progress = own<ast::TpInt64>::make();

struct Typer : ast::ActionMatcher {
	Typer(ltm::pin<ast::Ast> ast)
		: ast(ast)
	{
		tp_bool = ast->tp_optional(ast->tp_void());
	}

	void on_int_op(ast::BinaryOp& node, const function<string()>& context) {
		node.type_ = ast->tp_int64();
		expect_type(find_type(node.p[0]), node.type(), context);
		expect_type(find_type(node.p[1]), node.type(), context);
	}

	void on_int_or_double_op(ast::BinaryOp& node, const function<string()>& context) {
		auto& t0 = find_type(node.p[0])->type();
		if (!dom::strict_cast<ast::TpInt64>(t0) && !dom::strict_cast<ast::TpDouble>(t0))
			node.p[0]->error("expected int or double");
		node.type_ = t0;
		expect_type(find_type(node.p[1]), t0, context);
	}

	void on_const_i64(ast::ConstInt64& node) override { node.type_ = ast->tp_int64(); }
	void on_const_double(ast::ConstDouble& node) override { node.type_ = ast->tp_double(); }
	void on_const_void(ast::ConstVoid& node) override { node.type_ = ast->tp_void(); }
	void on_const_bool (ast::ConstBool& node) override { node.type_ = tp_bool; }
	void on_mk_lambda(ast::MkLambda& node) override {
		auto r = pin<ast::TpColdLambda>::make();
		r->callees.push_back(weak<ast::MkLambda>(&node));
		node.type_ = r;
	}
	void on_const_string(ast::ConstString& node) override {
		node.type_ = ast->string_cls.pinned();
	}
	void on_block(ast::Block& node) override {
		if (node.body.empty()) {
			node.type_ = ast->tp_void();
			return;
		}
		for (auto& l : node.names) {
			if (l->initializer)
				l->type = find_type(l->initializer)->type();
		}
		for (auto& a : node.body)
			find_type(a);
		if (auto ret_as_get = dom::strict_cast<ast::Get>(node.body.back())) {
			if (std::find(node.names.begin(), node.names.end(), ret_as_get->var) != node.names.end()) {
				node.type_ = ret_as_get->var->type;
				return;
			}
		}
		node.type_ = node.body.back()->type();
	}
	void check_fn_proto(ast::Action& node, ast::TpFunction& fn, vector<own<ast::Action>>& actual_params, ast::Action& callee) {
		if (fn.params.size() - 1 != actual_params.size())
			node.error("Mismatched params count: expected ", fn.params.size() - 1, " provided ", actual_params.size(), " see function definition:", callee);
		for (size_t i = 0; i < actual_params.size(); i++)
			expect_type(actual_params[i], Type::promote(fn.params[i]), [&] { return ast::format_str("parameter ", i); });
		node.type_ = Type::promote(fn.params.back());
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
			own<ast::TpLambda> lambda_type;
			for (auto& weak_fn : as_cold->callees) {
				auto fn = weak_fn.pinned();
				if (fn->names.size() != actual_params.size())
					node.error("Mismatched params count: expected ", fn->names.size(), " provided ", actual_params.size(), " see function definition:", *fn);
				if (!lambda_type) {
					vector<own<ast::Type>> param_types;
					auto fn_param = fn->names.begin();
					for (auto& p : actual_params) {
						param_types.push_back(p->type());
						(*fn_param++)->type = param_types.back();
					}
					for (auto& p : fn->body)
						find_type(p);
					auto& fn_result = find_type(fn->body.back())->type();
					param_types.push_back(fn_result);
					if (dom::strict_cast<ast::TpColdLambda>(fn_result) || dom::strict_cast<ast::TpLambda>(fn_result))
						fn->error("So far functions cannot return lambdas");
					lambda_type = ast->tp_lambda(move(param_types));
					as_cold->resolved = lambda_type;
					callee->type_ = lambda_type;
				} else {
					for (size_t i = 0; i < fn->names.size(); i++)
						fn->names[i]->type = Type::promote(lambda_type->params[i]);
					for (auto& p : fn->body)
						find_type(p);
					expect_type(fn->body.back(), Type::promote(lambda_type->params.back()), [&] { return "lambda result"; });
				}
			}
			node.type_ = Type::promote(lambda_type->params.back());
		} else {
			node.error(callee_type, " is not callable");
		}
	}
	void on_call(ast::Call& node) override {
		for (auto& p : node.params)
			find_type(p);
		type_call(node, find_type(node.callee), node.params);
		if (auto as_mk_delegate = dom::strict_cast<ast::MakeDelegate>(node.callee)) {
			if (as_mk_delegate->method->is_factory)
				node.type_ = as_mk_delegate->base->type();  // preserve both own/ref and actual this type.
		}
	}
	void handle_index_op(ast::GetAtIndex& node, own<ast::Action> opt_value, const string& name) {
		auto indexed = ast->extract_class(find_type(node.indexed)->type());
		if (!indexed)
			node.error("Only objects can be indexed, not ", node.indexed->type());
		if (auto m = dom::peek(indexed->this_names, ast::LongName{ name, nullptr })) {
			if (auto method = dom::strict_cast<ast::Method>(m)) {
				auto r = ast::make_at_location<ast::Call>(node).owned();
				auto callee = ast::make_at_location<ast::MakeDelegate>(node);
				r->callee = callee;
				callee->base = node.indexed;
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
		if (auto fn = indexed->module->functions[name + indexed->name].pinned()) {
			auto fnref = ast::make_at_location<ast::MakeFnPtr>(node);
			fnref->fn = fn;
			auto r = ast::make_at_location<ast::Call>(node).owned();
			r->callee = fnref;
			r->params = move(node.indexes);
			r->params.insert(r->params.begin(), move(node.indexed));
			if (opt_value)
				r->params.push_back(move(opt_value));
			fix(r);
			*fix_result = move(r);
			return;
		}
		node.error("function ", indexed->module->name, "_", name, indexed->name, " or method ", node.indexed->type().pinned(), ".", name, " not found");
	}
	void on_get_at_index(ast::GetAtIndex& node) override { handle_index_op(node, nullptr, "getAt"); }
	void on_set_at_index(ast::SetAtIndex& node) override { handle_index_op(node, move(node.value), "setAt"); }
	pin<ast::Function> type_fn(pin<ast::Function> fn) {
		if (!fn->type_ || fn->type_ == type_in_progress) {
			bool is_method = dom::isa<ast::Method>(*fn) || dom::isa<ast::ImmediateDelegate>(*fn);
			vector<own<ast::Type>> params;
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
		}
		return fn;
	}
	void on_make_delegate(ast::MakeDelegate& node) override {
		class_from_action(node.base);
		node.type_ = type_fn(node.method)->type_;
		if (dom::isa<ast::TpShared>(*node.base->type())) {
			if (node.method->mut == 1)
				node.error("cannot call a mutating method on a shared object");
		} else {
			if (node.method->mut == -1)
				node.error("cannot call a *method on a non-shared object");
		}
	}
	void on_to_int(ast::ToIntOp& node) override {
		node.type_ = ast->tp_int64();
		expect_type(find_type(node.p), ast->tp_double(), [] { return "double to int conversion"; });
	}
	void on_copy(ast::CopyOp& node) override {
		auto param_type = find_type(node.p)->type();
		if (auto param_as_ref = dom::strict_cast<ast::TpRef>(param_type))
			node.type_ = param_as_ref->target;
		else
			node.error("copy operand should be a reference, not ", param_type);
	}
	void on_to_float(ast::ToFloatOp& node) override {
		node.type_ = ast->tp_double();
		expect_type(find_type(node.p), ast->tp_int64(), [] { return "int to double conversion"; });
	}
	void on_not(ast::NotOp& node) override {
		node.type_ = tp_bool;
		handle_condition(node.p, [] { return "not operator"; });
	}
	void on_neg(ast::NegOp& node) override {
		auto& t = find_type(node.p)->type();
		if (!dom::strict_cast<ast::TpInt64>(t) && !dom::strict_cast<ast::TpDouble>(t))
			node.p->error("expected int or double");
		node.type_ = t;
	}
	void on_ref(ast::RefOp& node) override {
		auto as_class = dom::strict_cast<ast::TpClass>(find_type(node.p)->type());
		if (!as_class)
			node.p->error("expected own class or interface, not ", node.p->type());
		node.type_ = ast->get_ref(as_class);
	}
	void on_freeze(ast::FreezeOp& node) override {
		auto cls = class_from_action(node.p);
		node.type_ = ast->get_shared(cls);
	}
	void on_loop(ast::Loop& node) override {
		find_type(node.p);
		if (auto as_opt = dom::strict_cast<ast::TpOptional>(node.p->type())) {
			node.type_ = ast->get_wrapped(as_opt);
		} else {
			node.error("loop body returned ", node.p->type().pinned(), ", that is not bool or optional");
		}
	}
	void on_get(ast::Get& node) override {
		if (auto as_class = dom::strict_cast<ast::TpClass>(node.var->type)) {
			node.type_ = ast->get_ref(as_class);
		} else {
			node.type_ = node.var->type;
		}
	}
	void on_set(ast::Set& node) override {
		auto value_type = find_type(node.val)->type();
		auto& variable_type = node.var->type;
		if (auto value_as_class = dom::strict_cast<ast::TpClass>(value_type)) {
			node.type_ = ast->get_ref(value_as_class);
			auto variable_as_class = dom::strict_cast<ast::TpClass>(variable_type);
			if (!variable_as_class)
				value_type = ast->get_ref(value_as_class);
		} else {
			node.type_ = value_type;
			if (dom::strict_cast<ast::TpRef>(value_type)) {
				if (auto variable_as_class = dom::strict_cast<ast::TpClass>(variable_type))
					variable_type = ast->get_ref(variable_as_class);
			}
		}
		expect_type(
			*fix_result,
			value_type,
			variable_type,
			[&] { return ast::format_str("assign to vaiable", node.var_name, node.var.pinned()); });
	}
	void on_mk_instance(ast::MkInstance& node) override {
		node.type_ = node.cls;
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
		if (auto as_mk_instance = dom::strict_cast<ast::MkInstance>(node.p)) {
			node.type_ = ast->get_weak(as_mk_instance->cls);
			return;
		}
		node.p->error("Expected &ClassName or expression returning reference, not expr of type ", node.p->type().pinned());
	}
	pin<ast::TpClass> class_from_action(own<ast::Action>& node) {
		if (auto cls = ast->extract_class(find_type(node)->type()))
			return cls;
		node->error("Expected pointer to class, not ", node->type().pinned());
	}
	void on_get_field(ast::GetField& node) override {
		if (!node.field) {
			auto cls = class_from_action(node.base);
			if (!cls->handle_member(node, ast::LongName{ node.field_name, node.field_module },
				[&](auto field) { node.field = field; },
				[&](auto method) {
					auto r = ast::make_at_location<ast::MakeDelegate>(node).owned();
					r->base = move(node.base);
					r->method = method;
					find_type(r);
					*fix_result = move(r);
				},
				[&] { node.error("field/method name is ambigiuous, use cast"); }))
				node.error("class ", cls, " doesn't have field/method ", ast::LongName{ node.field_name, node.field_module });
		}
		if (&node == fix_result->pinned()) {
			find_type(node.base);
			node.type_ = find_type(node.field->initializer)->type();
			if (dom::isa<ast::TpShared>(*node.base->type())) {
				if (auto as_class = dom::strict_cast<ast::TpClass>(node.type())) {
					node.type_ = ast->get_shared(as_class);
				} else if (auto as_weak = dom::strict_cast<ast::TpWeak>(node.type())) {
					node.type_ = ast->get_frozen_weak(as_weak->target);
				}
			} else {
				if (auto as_class = dom::strict_cast<ast::TpClass>(node.type())) {
					node.type_ = ast->get_ref(as_class);
				}
			}
		}
	}
	void resolve_set_field(ast::SetField& node) {
		auto cls = class_from_action(node.base);
		if (!node.field) {
			if (!cls->handle_member(node, ast::LongName{ node.field_name, node.field_module },
				[&](auto field) { node.field = field; },
				[&](auto method) { node.error("Cannot assign to method"); },
				[&] { node.error("field name is ambiguous, use cast"); }))
				node.error("class ", cls->name, " doesn't have field/method ", ast::LongName{ node.field_name, node.field_module });
		}
		if (dom::isa<ast::TpShared>(*node.base->type()))
			node.error("Cannot assign to a shared object field ", ast::LongName{ node.field_name, node.field_module });
		node.type_ = find_type(node.field->initializer)->type();
		if (auto as_class = dom::strict_cast<ast::TpClass>(node.type())) {
			node.type_ = ast->get_ref(as_class);
		}
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
		if (!dom::isa<ast::TpClass>(*ft))
			node.error("Field must be @-pointer, not ", node.field->initializer->type().pinned());
		expect_type(find_type(node.val), node.type_, [&] {
			return ast::format_str("splice field ", ast::LongName{ node.field_name, node.field_module }, *node.field.pinned());
		});
		node.type_ = tp_bool;
	}
	void on_cast(ast::CastOp& node) override {
		auto src_cls = class_from_action(node.p[0]);
		auto dst_cls = class_from_action(node.p[1]);
		node.type_ = dom::strict_cast<ast::TpClass>(node.p[0]->type())
			? (pin<ast::Type>) dst_cls
			: ast->get_ref(dst_cls);
		if (src_cls->overloads.count(dst_cls))  // no-op conversion
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
		action->context_error(context, "expected optional, weak or bool type, not ", action->type().pinned());
	}
	void on_if(ast::If& node) override {
		auto cond = handle_condition(node.p[0], [] { return "if operator"; });
		if (auto as_block = dom::strict_cast<ast::Block>(node.p[1])) {
			if (!as_block->names.empty() && !as_block->names.front()->initializer) {
				as_block->names.front()->type = ast->get_wrapped(cond);
			}
		}
		node.type_ = ast->tp_optional(find_type(node.p[1])->type());
		assert(dom::strict_cast<ast::TpOptional>(node.type_));
	}
	void on_land(ast::LAnd& node) override {
		auto cond = handle_condition(node.p[0], [] { return "1st operand of `logical and` operator"; });
		if (auto as_block = dom::strict_cast<ast::Block>(node.p[1])) {
			if (!as_block->names.empty() && !as_block->names.front()->initializer) {
				as_block->names.front()->type = ast->get_wrapped(cond);
			}
		}
		node.type_ = find_type(node.p[1])->type();
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
		auto cls = ast->extract_class(find_type(node.base)->type());
		if (!cls)
			node.error("delegate should be connected to class pointer, not to ", node.base->type());
		resolve_immediate_delegate(ast, node, cls);
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

	void unify(pin<ast::TpColdLambda> cold, pin<ast::TpLambda> lambda, ast::Action& node, const function<string()>& context) {
		cold->resolved = lambda;
		for (auto& w_fn : cold->callees) {
			auto fn = w_fn.pinned();
			if (fn->names.size() != lambda->params.size() - 1)
				node.error("Mismatched params count: expected ", fn->names.size(), " provided ", lambda->params.size() - 1, " see function definition:", *fn);
			for (size_t i = 0; i < fn->names.size(); i++)
				fn->names[i]->type = Type::promote(lambda->params[i]);
			for (auto& a : fn->body)
				find_type(a);
			auto result_type = Type::promote(lambda->params.back());
			if (dom::isa<ast::TpVoid>(*result_type) && !dom::isa<ast::TpVoid>(*fn->body.back()->type())) {
				auto c_void = ast::make_at_location<ast::ConstVoid>(*fn->body.back());
				c_void->type_ = result_type;
				fn->body.push_back(c_void);
			} else {
				expect_type(fn->body.back(), result_type, [&] { return ast::format_str("lambda result in ", context()); });
			}
		}
	}
	void expect_type(own<ast::Action>& node, pin<ast::Type> expected_type, const function<string()>& context) {
		expect_type(node, node->type(), expected_type, context);
	}
	void expect_type(own<ast::Action>& node, pin<ast::Type> actual_type, pin<ast::Type> expected_type, const function<string()>& context) {
		if (expected_type == ast->tp_void())
			return;
		if (actual_type == expected_type)
			return;
		if (auto exp_as_ref = dom::strict_cast<ast::TpRef>(expected_type)) {
			if (auto actual_class = ast->extract_class(actual_type)) {
				if (actual_class == exp_as_ref->target || actual_class->overloads.count(exp_as_ref->target))
					return;
			}
		} else if (auto exp_as_class = dom::strict_cast<ast::TpClass>(expected_type)) {
			if (auto actual_class = dom::strict_cast<ast::TpClass>(actual_type)) {
				if (actual_class->overloads.count(exp_as_class))
					return;
			}
		} else if (auto exp_as_shared = dom::strict_cast<ast::TpShared>(expected_type)) {
			if (auto actual_as_shared = dom::strict_cast<ast::TpShared>(actual_type)) {
				if (actual_as_shared->target->overloads.count(exp_as_shared->target))
					return;
			}
		} else if (auto exp_as_weak = dom::strict_cast<ast::TpWeak>(expected_type)) {
			if (auto actual_as_weak = dom::strict_cast<ast::TpWeak>(actual_type)) {
				if (actual_as_weak->target->overloads.count(exp_as_weak->target))
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
				unify(act_as_cold, exp_as_lambda, *node, context);
				return;
			}
		} else if (auto act_as_lambda = dom::strict_cast<ast::TpLambda>(actual_type)) {
			if (auto exp_as_cold = dom::strict_cast<ast::TpColdLambda>(expected_type)) {
				unify(exp_as_cold, act_as_lambda, *node, context);
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
				auto actual_as_class = dom::strict_cast<ast::TpClass>(actual_type);
				auto result_as_class = dom::strict_cast<ast::TpClass>(exp_as_lambda->params.back());
				if (actual_as_class && result_as_class && (actual_as_class == result_as_class || actual_as_class->overloads.count(result_as_class))) {
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
		m->names[0]->type = find_type(m->names[0]->initializer)->type();
		for (auto& a : m->body)
			find_type(a);
		if (!m->body.empty())  // empty == interface method
			expect_type(m->body.back(), m->type_expression->type(), [] { return "checking method result type against declartion"; });
		if (m->base)
			expect_type(m, m->base->type(), [] { return "checking method type against overridden"; });
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
			for (auto& fn : m.second->functions)
				type_fn(fn.second);
		}
		for (auto& c : ast->classes_in_order) {
			this_class = c;
			for (auto& f : c->fields) {
				find_type(f->initializer);
				if (dom::strict_cast<ast::TpRef>(f->initializer->type())) {
					f->error("Fields cannot be temp-references. Make it @own or &weak");
				}
			}
			for (auto& m : c->new_methods)
				process_method(m);
			for (auto& b : c->overloads)
				for (auto& m : b.second)
					process_method(m);
		}
		for (auto& m : ast->modules) {
			for (auto& fn : m.second->functions) {
				for (auto& a : fn.second->body)
					find_type(a);
				if (!fn.second->is_platform)
					expect_type(fn.second->body.back(), fn.second->type_expression->type(), [] { return "checking actual result tupe against fn declaration"; });
			}
		}
		expect_type(find_type(ast->starting_module->entry_point), ast->tp_lambda({ ast->tp_void() }), []{ return "main fn return value"; });
		for (auto& m : ast->modules) {
			for (auto& t : m.second->tests) {
				type_fn(t.second);
				if (!t.second->names.empty())
					t.second->error("tests should not have parameters");
				for (auto& a : t.second->body)
					find_type(a);
			}
		}
	}

	pin<ast::Ast> ast;
	pin<Type> tp_bool;
	pin<ast::TpClass> this_class;
};

}  // namespace

void check_types(ltm::pin<ast::Ast> ast) {
	Typer(ast).process();
}
