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
	pin<ast::TpClass> this_class;
	unordered_map<own<dom::Name>, pin<ast::Var>> locals;
	pin<dom::Dom> dom;
	pin<ast::Ast> ast;
	vector<pin<ast::MkLambda>> lambda_levels;
	unordered_set<pin<ast::TpClass>> ordered_classes;
	unordered_set<pin<ast::TpClass>> active_base_list;
	vector<own<ast::TpClass>> classes_in_order;

	NameResolver(pin<ast::Ast> ast, pin<dom::Dom> dom)
		: dom(dom)
		, ast(ast) {}

	void order_class(pin<ast::TpClass> c) {
		if (ordered_classes.count(c))
			return;
		if (active_base_list.count(c)) {
			std::cerr << "loop in base classes around" << c->name << std::endl;
			throw 1;
		}
		active_base_list.insert(c);
		unordered_set<weak<ast::TpClass>> indirect_bases_to_add;
		for (auto& base : c->overloads) {
			if (base.first->is_interface) {
			} else if (c->base_class) {
				std::cerr << "there might be only one base class in " << c->name.pinned() << std::endl;  // TODO: use base->error, after base will be MkInstance
				throw 1;
			} else if (c->is_interface) {
				std::cerr << "interface " << c->name.pinned() <<  " cannot extend class " << base.first->name.pinned() << std::endl;  // TODO: use base->error, after base will be MkInstance
				throw 1;
			} else
				c->base_class = base.first;
			order_class(base.first);
			for (auto& i : base.first->overloads)
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
		classes_in_order.push_back(move(c));
	}

	void fix_globals() {
		for (auto& c : ast->classes) {
			if (c)
				order_class(c);
		}
		// Now classes are ordered in base first order, and cls.overloads contains all base classes and interfeces - direct and indirect.
		assert(ast->classes.size() == classes_in_order.size());
		std::swap(ast->classes, classes_in_order);
		for (auto& c : ast->classes) {
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
					c->interface_vmts = c->base_class->interface_vmts;
				for (auto& overload : c->overloads) {
					if (!overload.first->is_interface)
						continue;
					auto& vmt = c->interface_vmts[overload.first];
					if (vmt.empty()) {
						for (auto& m : overload.first->new_methods)
							vmt.push_back(m);
					}
					for (auto& base_name : overload.first->this_names) {
						if (c->this_names.count(base_name.first) != 0)
							c->this_names[base_name.first] = nullptr;  // mark ambiguous
						else
							c->this_names[base_name.first] = base_name.second;
					}
					for (auto& ovr_method : overload.second) {
						ovr_method->cls = c;
						if (!overload.first->handle_member(*ovr_method, ovr_method->name,
							[&](auto& field) { ovr_method->error("method overriding field:", field); },
							[&](auto& base_method) {
								ovr_method->ordinal = base_method->ordinal;
								if (base_method->cls == c)
									ovr_method->error("method is already implemented here", base_method);
								ovr_method->base = base_method->base;
								ovr_method->ovr = base_method;
								ovr_method->mut = base_method->mut;
								assert(ovr_method->ordinal < vmt.size());
								vmt[ovr_method->ordinal] = ovr_method;
								auto& named = c->this_names[ovr_method->name];
								if (named)
									named = ovr_method;
							},
							[&]() { ovr_method->error("override is disambiguous"); }))
							ovr_method->error("no method to override");
					}
					for (auto& m : vmt) {
						if (m->body.empty())
							m->error("method is not implemented in class ", c->name.pinned());
					}
				}
			}
			unordered_set<own<dom::Name>> this_class_names;
			for (auto& f : c->fields) {
				if (this_class_names.count(f->name))
					f->error("Field name redefinition");
				this_class_names.insert(f->name);
				c->this_names[f->name] = f;
			}
			for (auto& m : c->new_methods) {
				if (this_class_names.count(m->name))
					m->error("Method name redefinition");
				this_class_names.insert(m->name);
				c->this_names[m->name] = m;
			}
			if (c->base_class) {
				for (auto& n : c->base_class->this_names) {
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
		for (auto& f : ast->functions)
			fix_fn(f);
		for (auto& t : ast->tests_by_names)
			fix_fn(t.second);
		fix(ast->entry_point);
	}
	void fix_fn(pin<ast::Function> fn) {
		fn->lexical_depth = lambda_levels.size();
		lambda_levels.push_back(fn);
		this_var = dom::isa<ast::Method>(*fn) || dom::isa<ast::ImmediateDelegate>(*fn)
			? fn->names.front().pinned()
			: nullptr;
		if (auto as_method = dom::strict_cast<ast::Method>(fn)) {
			if (as_method->mut == -1) {
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
		lambda_levels.pop_back();
	}
	void fix_with_params(const vector<own<ast::Var>>& params, vector<own<ast::Action>>& body) {
		vector<pair<own<dom::Name>, pin<ast::Var>>> prev;
		prev.reserve(params.size());
		for (auto& p : params) {
			if (p->initializer)
				fix(p->initializer);
			p->lexical_depth = lambda_levels.size() - 1;
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

	void fix_var_depth(pin<ast::Var> var) {
		if (var->lexical_depth != lambda_levels.size() - 1) {
			if (!var->captured) {
				var->captured = true;
				lambda_levels[var->lexical_depth]->captured_locals.push_back(var);
			}
			if (lambda_levels.back()->access_depth < var->lexical_depth)
				lambda_levels.back()->access_depth = var->lexical_depth;
		}
	}

	template<typename F, typename M, typename C, typename FN>
	void handle_data_ref(ast::DataRef& node, F on_field, M on_method, C on_class, FN on_function) {
		if (node.var)
			return;
		if (auto it = locals.find(node.var_name); it != locals.end()) {
			fix_var_depth(node.var = it->second);
			return;
		}
		bool is_ambigous = false;
		if (this_class && this_class->handle_member(node, node.var_name, move(on_field), move(on_method), [&] { is_ambigous = true; }) && !is_ambigous)
			return;
		if (auto c = ast->peek_class(node.var_name)) {
			on_class(c);
			return;
		}
		if (auto fi = ast->functions_by_names.find(node.var_name); fi != ast->functions_by_names.end()) {
			on_function(fi->second);
			return;
		}
		node.error(is_ambigous ? "ambigous name " : "unresolved name ", node.var_name.pinned());
	}

	void on_get(ast::Get& node) override {
		handle_data_ref(node,
			[&](pin<ast::Field> field) {
				auto get_field = ast::make_at_location<ast::GetField>(node);
				get_field->field = field;
				get_field->field_name = node.var_name;
				auto this_ref = ast::make_at_location<ast::Get>(node);
				this_ref->var = this_var;
				fix_var_depth(this_var);
				get_field->base = this_ref;
				*fix_result = get_field;
			},
			[&](pin<ast::Method> method) {
				auto mk_delegate = ast::make_at_location<ast::MakeDelegate>(node);
				mk_delegate->method = method;
				auto this_ref = ast::make_at_location<ast::Get>(node);
				this_ref->var = this_var;
				fix_var_depth(this_var);
				mk_delegate->base= this_ref;
				*fix_result = mk_delegate;
			},
			[&](pin<ast::TpClass> cls) {
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
				set_field->val = move(node.val);
				auto this_ref = ast::make_at_location<ast::Get>(node);
				this_ref->var = this_var;
				fix_var_depth(this_var);
				set_field->base = this_ref;
				*fix_result = set_field;
			},
			[&](pin<ast::Method> method) {
				node.error("Method is not assignable");
			},
			[&](pin<ast::TpClass> cls) {
				node.error("Class is not assignable");
			},
			[&](pin<ast::Function> fn) {
				node.error("Function is not assignable");
			});
		if (node.var && !node.var->is_mutable) {
			node.var->is_mutable = true;
			lambda_levels[node.var->lexical_depth]->mutables.push_back(node.var);
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
		node.lexical_depth = lambda_levels.size();
		lambda_levels.push_back(&node);
		fix_with_params(node.names, node.body);
		lambda_levels.pop_back();
	}

	void on_immediate_delegate(ast::ImmediateDelegate& node) override {
		if (node.base) // can be null if used as type def
			fix(node.base);
	}

	void fix_immediate_delegate(ast::ImmediateDelegate& node, pin<ast::TpClass> cls) {
		vector<pin<ast::MkLambda>> prev_ll;
		swap(prev_ll, lambda_levels);
		this_var = node.names.front();
		this_class = cls;
		fix_fn(&node);
		lambda_levels = move(prev_ll);
	}
};

}  // namespace

void resolve_names(pin<ast::Ast> ast) {
	NameResolver(ast, ast->dom).fix_globals();
}

void resolve_immediate_delegate(pin<ast::Ast> ast, ast::ImmediateDelegate& node, pin<ast::TpClass> cls)
{
	NameResolver(ast, ast->dom).fix_immediate_delegate(node, cls);
}
