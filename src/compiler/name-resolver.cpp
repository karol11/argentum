#include "compiler/name-resolver.h"

#include <unordered_map>
#include <unordered_set>
#include <cassert>
#include <unordered_set>

using std::unordered_map;
using std::unordered_set;
using std::string;
using std::vector;
using std::pair;
using std::move;
using ltm::pin;
using ltm::weak;
using ltm::own;
using ast::Node;

namespace {

struct NameResolver : ast::ActionScanner {
	pin<ast::Var> this_var;
	pin<ast::Class> this_class;
	unordered_map<string, pin<ast::Var>> locals;
	pin<dom::Dom> dom;
	pin<ast::Ast> ast;
	unordered_set<pin<ast::Class>> ordered_classes;
	unordered_set<pin<ast::Class>> active_base_list;

	NameResolver(pin<ast::Ast> ast, pin<dom::Dom> dom)
		: dom(dom)
		, ast(ast) {}

	void order_class(pin<ast::Class> c) {
		if (ordered_classes.count(c))
			return;
		if (active_base_list.count(c))
			c->error("loop in base classes around ", c->get_name());
		if (!c->line)
			c->error("class hasn't been defined ", c->get_name());
		active_base_list.insert(c);
		unordered_set<weak<ast::AbstractClass>> indirect_bases_to_add;
		for (auto& abstract_base : c->overloads) {
			auto base = abstract_base.first->get_implementation();
			if (base->is_interface) {
			} else if (c->base_class) {
				c->error("there might be only one base class in ", c->get_name());
			} else if (c->is_interface) {
				c->error("interface ", c->get_name(), " cannot extend class ", abstract_base.first->get_name());
			} else
				c->base_class = abstract_base.first;
			order_class(base);
			for (auto& i : base->overloads)
				indirect_bases_to_add.insert(i.first);
		}
		if (!c->base_class && c != ast->object) {
			c->base_class = ast->object;
			indirect_bases_to_add.insert(ast->object);
			order_class(ast->object);
		}
		active_base_list.erase(c);
		for (auto& i : indirect_bases_to_add)
			c->overloads[i]; // insert one if it's not there yet.
		ordered_classes.insert(c);
		ast->classes_in_order.push_back(c);
	}

	void fix_globals() {
		size_t cls_cnt = 0;
		for (auto& m : ast->modules) {
			cls_cnt += m.second->classes.size();
			for (auto& c : m.second->classes) {
				order_class(c.second);
			}
		}
		// Now classes are ordered in base first order, and cls.overloads contains all base classes and interfeces - direct and indirect.
		assert(cls_cnt == ast->classes_in_order.size());
		for (auto& cw : ast->classes_in_order) {
			auto c = cw.pinned();
			// fill own methods
			uint32_t ordinal = 0;
			for (auto& m : c->new_methods) {
				m->ordinal = ordinal++;
				m->ovr = m->base = m;
				m->cls = c;
			}
			// fill inherited/implemented vmts for bases and non-conflicting names
			// Name conflicts: any of base class methods or interface methods with the same names == conflict.
			// Conflict can be overridden by new method of this class.
			if (!c->is_interface) {
				if (c->base_class)
					c->interface_vmts = c->base_class->get_implementation()->interface_vmts;
				for (auto& abstract_overload : c->overloads) {
					auto overload = abstract_overload.first->get_implementation();
					if (!overload->is_interface)
						continue;
					auto& vmt = c->interface_vmts[overload];
					if (vmt.empty()) {
						for (auto& m : overload->new_methods)
							vmt.push_back(m);
					}
					for (auto& base_name : overload->this_names) {
						if (c->this_names.count(base_name.first) != 0)
							c->this_names[base_name.first] = nullptr;  // mark ambiguous
						else
							c->this_names[base_name.first] = base_name.second;
					}
					for (auto& ovr_method : abstract_overload.second) {
						ovr_method->cls = c;
						if (!overload->handle_member(*ovr_method, { ovr_method->name, ovr_method->base_module },
							[&](auto& field) { ovr_method->error("method overriding field:", field); },
							[&](auto& base_method) {
								if (ovr_method == base_method) // each class member regustered in this_names twice with short and long name.
									return;
								ovr_method->ordinal = base_method->ordinal;
								if (base_method->cls == c)
									ovr_method->error("method is already implemented here", base_method);
								ovr_method->base = base_method->base;
								ovr_method->ovr = base_method;
								ovr_method->mut = base_method->mut;
								assert(ovr_method->ordinal < vmt.size());
								vmt[ovr_method->ordinal] = ovr_method;
								if (auto& named = c->this_names[ast::LongName{ ovr_method->name, ovr_method->module }])
									named = ovr_method;
								if (auto& named = c->this_names[ast::LongName{ ovr_method->name, nullptr }])
									named = ovr_method;
							},
							[&]() { ovr_method->error("override is ambiguous"); }))
							ovr_method->error("no method to override");
					}
					for (auto& m : vmt) {
						if (m->body.empty())
							m->error("method is not implemented in class ", c);
					}
				}
			}
			unordered_set<ast::LongName> not_inherited_names;
			for (auto& f : c->fields) {
				ast::LongName n{ f->name, f->module };
				if (not_inherited_names.count(n))
					f->error("Field name redefinition");
				not_inherited_names.insert(n);
				c->this_names[n] = f;
				c->this_names[{f->name, nullptr}] = f;
			}
			for (auto& m : c->new_methods) {
				ast::LongName n{ m->name, m->module };
				if (not_inherited_names.count(n))
					m->error("Method name redefinition");
				not_inherited_names.insert(n);
				c->this_names[n] = m;
				c->this_names[{m->name, nullptr}] = m;
			}
			if (c->base_class) {
				for (auto& n : c->base_class->get_implementation()->this_names) {
					if (c->this_names.count(n.first) == 0)
						c->this_names.insert(n);
				}
			}
			this_class = nullptr;
			for (auto& f : c->fields)
				fix(f->initializer);
			this_class = c;
			for (auto& m : c->new_methods)
				fix_fn(m);
			for (auto& b : c->overloads)
				for (auto& m : b.second)
					fix_fn(m);
		}
		this_class = nullptr;
		for (auto& m : ast->modules) {
			for (auto& c : m.second->constants)
				fix(c.second->initializer);
			for (auto& f : m.second->functions)
				fix_fn(f.second);
			for (auto& t : m.second->tests)
				fix_fn(t.second);
			if (m.second->entry_point)
				fix(m.second->entry_point);
		}
	}
	void fix_fn(pin<ast::Function> fn) {
		this_var = dom::isa<ast::Method>(*fn) || dom::isa<ast::ImmediateDelegate>(*fn)
			? fn->names.front().pinned()
			: nullptr;
		if (auto as_method = dom::strict_cast<ast::Method>(fn)) {
			if (as_method->mut == ast::Mut::FROZEN) {
				auto freeze = ast::make_at_location<ast::FreezeOp>(*this_var->initializer);
				freeze->p = move(this_var->initializer);
				this_var->initializer = freeze;
			}
		}
		for (auto& p : fn->names) {
			if (p->initializer)
				fix(p->initializer);
		}
		fix(fn->type_expression);
		fix_with_params(fn->names, fn->body);
		this_var = nullptr;
	}
	void fix_with_params(const vector<own<ast::Var>>& params, vector<own<ast::Action>>& body) {
		vector<pair<string, pin<ast::Var>>> prev;
		prev.reserve(params.size());
		for (auto& p : params) {
			if (p->initializer)
				fix(p->initializer);
			auto& dst = locals[p->name];
			prev.push_back({ p->name, dst });
			dst = p;
		}
		for (auto& b : body)
			fix(b);
		for (auto& p : prev) {
			if (p.second)
				locals[p.first] = p.second;
			else
				locals.erase(p.first);
		}
	}

	template<typename F, typename M, typename C, typename FN>
	void handle_data_ref(ast::DataRef& node, F on_field, M on_method, C on_class, FN on_function) {
		if (node.var)
			return;
		if (!node.var_module) {
			if (auto it = locals.find(node.var_name); it != locals.end()) {
				node.var = it->second;
				return;
			}
		}
		bool is_ambigous = false;
		if (this_class && this_class->handle_member(node, { node.var_name, node.var_module }, move(on_field), move(on_method), [&] { is_ambigous = true; }) && !is_ambigous)
			return;
		auto target_module = node.var_module ? node.var_module : node.module;
		if (auto c = target_module->peek_class(node.var_name)) {
			on_class(c);
			return;
		}
		if (auto fi = target_module->functions.find(node.var_name); fi != target_module->functions.end()) {
			on_function(fi->second);
			return;
		}
		if (auto it = target_module->constants.find(node.var_name); it != target_module->constants.end()) {
			node.var = it->second;
			return;
		}
		if (!node.var_module) {
			if (auto al = node.module->aliases.find(node.var_name); al != node.module->aliases.end()) {
				if (auto as_cls = dom::strict_cast<ast::Class>(al->second.pinned()))
					on_class(as_cls);
				else if (auto as_fn = dom::strict_cast<ast::Function>(al->second))
					on_function(as_fn);
				else
					node.error("internal: alias name is nor class nor function");
				return;
			}
		}
		node.error(is_ambigous ? "ambigous name " : "unresolved name ", ast::LongName{ node.var_name, node.var_module });
	}

	void on_get(ast::Get& node) override {
		handle_data_ref(node,
			[&](pin<ast::Field> field) {
				auto get_field = ast::make_at_location<ast::GetField>(node);
				get_field->field = field;
				get_field->field_name = node.var_name;
				get_field->field_module = node.var_module;
				auto this_ref = ast::make_at_location<ast::Get>(node);
				this_ref->var = this_var;
				get_field->base = this_ref;
				*fix_result = get_field;
			},
			[&](pin<ast::Method> method) {
				auto mk_delegate = ast::make_at_location<ast::MakeDelegate>(node);
				mk_delegate->method = method;
				auto this_ref = ast::make_at_location<ast::Get>(node);
				this_ref->var = this_var;
				mk_delegate->base= this_ref;
				*fix_result = mk_delegate;
			},
			[&](pin<ast::Class> cls) {
				auto mk_instance = ast::make_at_location<ast::MkInstance>(node);
				mk_instance->cls = cls;
				*fix_result = mk_instance;
			},
			[&](pin<ast::Function> fn) {
				auto fn_ref = ast::make_at_location<ast::MakeFnPtr>(node);
				fn_ref->fn = fn;
				*fix_result = fn_ref;
			});
	}

	void on_set(ast::Set& node) override {
		fix(node.val);
		handle_data_ref(node,
			[&](pin<ast::Field> field) {
				auto set_field = ast::make_at_location<ast::SetField>(node);
				set_field->field = field;
				set_field->field_name = node.var_name;
				set_field->field_module = node.var_module;
				set_field->val = move(node.val);
				auto this_ref = ast::make_at_location<ast::Get>(node);
				this_ref->var = this_var;
				set_field->base = this_ref;
				*fix_result = set_field;
			},
			[&](pin<ast::Method> method) {
				node.error("Method is not assignable");
			},
			[&](pin<ast::Class> cls) {
				node.error("Class is not assignable");
			},
			[&](pin<ast::Function> fn) {
				node.error("Function is not assignable");
			});
		if (node.var && node.var->is_const)
			node.error("Constant is not assignable");
	}

	void on_call(ast::Call& node) override {
		ast::ActionScanner::on_call(node);
		if (auto callee_as_mk_inst = dom::strict_cast<ast::MkInstance>(node.callee)) {
			vector<weak<ast::AbstractClass>> params{ callee_as_mk_inst->cls };
			for (auto& p : node.params) {
				if (auto p_as_inst = dom::strict_cast<ast::MkInstance>(p))
					params.push_back(p_as_inst->cls);
				else
					p->error("Class param should be a class");
			}
			callee_as_mk_inst->cls = ast->get_class_instance(move(params));
			*fix_result = move(callee_as_mk_inst);
		}
	}

	void on_block(ast::Block& node) override {
		fix_with_params(node.names, node.body);
		if (node.body.size() == 1) {
			if (node.names.empty())
				*fix_result = move(node.body[0]);
			else if (auto child_as_block = dom::strict_cast<ast::Block>(node.body[0])) {
				for (auto& l : child_as_block->names)
					node.names.push_back(move(l));
				node.body = move(child_as_block->body);
			}
		}
	}

	void on_mk_lambda(ast::MkLambda& node) override {
		fix_with_params(node.names, node.body);
	}

	void on_immediate_delegate(ast::ImmediateDelegate& node) override {
		if (node.base) // can be null if used as type def
			fix(node.base);
	}

	void fix_immediate_delegate(ast::ImmediateDelegate& node, pin<ast::Class> cls) {
		unordered_map<string, pin<ast::Var>> prev_locals;
		swap(prev_locals, locals);
		this_var = node.names.front();
		this_class = cls;
		fix_fn(&node);
		locals = move(prev_locals);
	}
};

}  // namespace

void resolve_names(pin<ast::Ast> ast) {
	NameResolver(ast, ast->dom).fix_globals();
}

void resolve_immediate_delegate(pin<ast::Ast> ast, ast::ImmediateDelegate& node, pin<ast::Class> cls)
{
	NameResolver(ast, ast->dom).fix_immediate_delegate(node, cls);
}
