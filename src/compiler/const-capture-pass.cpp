#include "compiler/const-capture-pass.h"

#include <unordered_map>
#include <unordered_set>
#include <cassert>
#include <unordered_set>

using std::unordered_map;
using std::unordered_set;
using std::vector;
using std::move;
using ltm::pin;
using ltm::weak;
using ltm::own;
using ast::Node;

namespace {

struct ConstCapturePass : ast::ActionScanner {
	pin<dom::Dom> dom;
	pin<ast::Ast> ast;
	vector<pin<ast::MkLambda>> lambda_levels;

	ConstCapturePass(pin<ast::Ast> ast, pin<dom::Dom> dom)
		: dom(dom)
		, ast(ast) {}

	void fix_globals() {
		for (auto& c : ast->classes_in_order) {
			for (auto& f : c->fields)
				fix(f->initializer);
			for (auto& m : c->new_methods)
				fix_fn(*m);
			for (auto& b : c->overloads)
				for (auto& m : b.second)
					fix_fn(*m);
		}
		for (auto& m : ast->modules) {
			for (auto& f : m.second->functions)
				fix_fn(*f.second);
			for (auto& t : m.second->tests)
				fix_fn(*t.second);
			if (m.second->entry_point)
				fix_fn(*m.second->entry_point);
		}
	}
	void fix_fn(ast::MkLambda& fn) {
		fn.lexical_depth = lambda_levels.size();
		lambda_levels.push_back(&fn);
		fix_block(fn);
		lambda_levels.pop_back();
	}
	void fix_block(ast::Block& b) {
		b.lexical_depth = lambda_levels.size() - 1;
		for (auto& p : b.names) {
			if (p->initializer)
				fix(p->initializer);
			p->lexical_depth = lambda_levels.size() - 1;
		}
		for (auto& b : b.body)
			fix(b);
	}

	void fix_var_depth(pin<ast::Var> var) {
		if (var->is_const)
			return;
		if (var->lexical_depth != lambda_levels.size() - 1) {
			if (!var->captured) {
				var->captured = true;
				lambda_levels[var->lexical_depth]->captured_locals.push_back(var);
			}
			if (lambda_levels.back()->access_depth < var->lexical_depth)
				lambda_levels.back()->access_depth = var->lexical_depth;
		}
	}

	void on_get(ast::Get& node) override {
		fix_var_depth(node.var);
	}

	void on_break(ast::Break& node) override {
		if (node.block->lexical_depth != lambda_levels.size() - 1)
			node.error("internal: break through lambdas aren't supported yet");
	}

	void on_set(ast::Set& node) override {
		fix(node.val);
		fix_var_depth(node.var);
		if (!node.var->is_mutable) {
			node.var->is_mutable = true;
			lambda_levels[node.var->lexical_depth]->mutables.push_back(node.var);
		}
	}

	void on_block(ast::Block& node) override {
		fix_block(node);
	}
 
	void on_mk_lambda(ast::MkLambda& node) override {
		fix_fn(node);
	}

	void on_immediate_delegate(ast::ImmediateDelegate& node) override {
		if (node.base) // can be null if used as type def
			fix(node.base);
		vector<pin<ast::MkLambda>> prev_ll;
		swap(prev_ll, lambda_levels);
		fix_fn(node);
		lambda_levels = move(prev_ll);
	}
};

}  // namespace

void const_capture_pass(ltm::pin<ast::Ast> ast) {
	ConstCapturePass(ast, ast->dom).fix_globals();
}
