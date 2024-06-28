#include "pruner.h"

#include<functional>
#include<unordered_map>
#include<vector>
#include <deque>

using ltm::pin;
using std::unordered_map;
using std::vector;
using std::deque;
using std::function;

namespace {

struct Pruner : ast::ActionScanner {
	pin<dom::Dom> dom;
	pin<ast::Ast> ast;
	deque<function<void()>> tasks;
	unordered_map<pin<ast::Method>, vector<pin<ast::Method>>> overrides;

	Pruner(pin<ast::Ast> ast, pin<dom::Dom> dom)
		: dom(dom)
		, ast(ast) {}

	void use_fn(pin<ast::Function> fn) {
		if (fn->used)
			return;
		fn->used = true;
		tasks.push_back([this, fn] { on_block(*fn); });
	}
	void use_method(pin<ast::Method> m) {
		if (m->base->used)
			return;
		m->base->used = true;
		tasks.push_back([this, m] {
			on_block(*m);
			for (auto& ovr : overrides[m->base])
				on_block(*ovr);
			use_class(m->base->cls);
		});
	}
	void use_class(pin<ast::AbstractClass> a_cls) {
		while (a_cls) {
			if (auto cls = dom::strict_cast<ast::Class>(a_cls)) {
				if (cls->used)
					return;
				cls->used = true;
				if (auto disposer = cls->module->functions.find("dispose" + cls->name); disposer != cls->module->functions.end())
					use_fn(disposer->second);
				if (auto fixer = cls->module->functions.find("afterCopy" + cls->name); fixer != cls->module->functions.end())
					use_fn(fixer->second);
				tasks.push_back([this, cls] {
					for (auto& f : cls->fields)
						fix(f->initializer);
					});
				a_cls = cls->base_class;
			} else if (auto c_inst = dom::strict_cast<ast::ClassInstance>(a_cls)) {
				a_cls = c_inst->params[0];
			} else {
				break;
			}
		}
	}
	void on_make_delegate(ast::MakeDelegate& node) override {
		ast::ActionScanner::on_make_delegate(node);
		use_method(node.method);
	}
	void on_make_fn_ptr(ast::MakeFnPtr& node) override{
		// no-op: ast::ActionScanner::on_make_fn_ptr(node);
		use_fn(node.fn);
	}
	void on_mk_instance(ast::MkInstance& node) override {
		// no-op: ast::ActionScanner::on_mk_instance(node);
		if (node.cls)
			use_class(node.cls);
	}
	void on_cast(ast::CastOp& node) override {
		ast::ActionScanner::on_cast(node);
		if (node.p[1]) {
			if (auto c = ast->extract_class(node.p[1]->type()))
				use_class(c);
		}
	}
	void on_const_string(ast::ConstString& node) override {
		// no-op: ast::ActionScanner::on_const_string(node);
		use_class(ast->string_cls);
	}
	void on_get_field(ast::GetField& node) override {
		ast::ActionScanner::on_get_field(node);
		use_class(node.field->cls);
	}
	void on_set_field(ast::SetField& node) override {
		ast::ActionScanner::on_set_field(node);
		use_class(node.field->cls);
	}
	void prune() {
		for (auto& m : ast->modules) {
			for (auto& c : m.second->classes) {
				for (auto& b : c.second->overloads) {
					for (auto& mt : b.second) {
						if (mt->base != mt)
							overrides[mt->base].push_back(mt);
					}
				}
			}
		}
		for (auto& m : ast->modules) {
			for (auto& c : m.second->constants) {
				fix(c.second->initializer);
			}
		}
		use_fn(ast->starting_module->entry_point);
		while (!tasks.empty()) {
			tasks.front()();
			tasks.pop_front();
		}
	}
};

}  // namespace

void prune(pin<ast::Ast> ast) {
	Pruner(ast, ast->dom).prune();
}
