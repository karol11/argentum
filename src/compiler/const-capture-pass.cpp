#include "const-capture-pass.h"

#include <unordered_map>
#include <unordered_set>
#include <cassert>
#include <unordered_set>
#include <variant>

using std::unordered_map;
using std::unordered_set;
using std::vector;
using std::move;
using std::variant;
using std::get_if;
using std::function;
using ltm::pin;
using ltm::weak;
using ltm::own;
using ltm::cast;
using ast::Node;

namespace {

struct LambdaDep : ltm::Object {
	using tp_direct = weak<ast::MkLambda>;
	using tp_pull = unordered_set<weak<LambdaDep>>;
	using type = variant<tp_direct, tp_pull>;
	type val;
	LambdaDep(type&& v) : val(v) { make_shared(); }
	LTM_COPYABLE(LambdaDep);
};

struct ConstCapturePass : ast::ActionScanner {
	pin<dom::Dom> dom;
	pin<ast::Ast> ast;
	vector<pin<ast::MkLambda>> lambda_levels;   // Nested lambdas, `back` is the current lambda

	// lambda dependency graph holding data on what lambdas a given node can return
	// can have cycles. built per-callable
	// a node can return either a direct single lambda or combine lambda-deps of the other nodes
	unordered_map<pin<ast::Node>, own<LambdaDep>> node_lambdas;
	vector<weak<ast::Call>> calls_to_fix; // calls of the current function which `activates_lambdas` to fill by tracing the `node_lambdas`

	ConstCapturePass(pin<ast::Ast> ast, pin<dom::Dom> dom)
		: dom(dom)
		, ast(ast) {}

	void fix_globals() {
		for (auto& c : ast->classes_in_order) {
			for (auto& f : c->fields) {
				fix(f->initializer);
			}
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
		auto prev_node_lambdas = node_lambdas;
		decltype(calls_to_fix) prev_calls_to_fix;
		swap(prev_calls_to_fix, calls_to_fix);
		swap(prev_node_lambdas, node_lambdas);
		fn.lexical_depth = lambda_levels.size();
		lambda_levels.push_back(&fn);
		fix_block(fn, true);
		for (auto& c : calls_to_fix) {
			unordered_set<pin<LambdaDep>> seen;
			function<void(LambdaDep*)> scan = [&](LambdaDep* n) {
				if (auto as_direct = get_if<LambdaDep::tp_direct>(&n->val)) {
					if (&fn != *as_direct) {
						c->activates_lambdas.insert(*as_direct);
						if (*as_direct) {
							for (auto& xb : (*as_direct)->x_breaks) {
								if (xb->block->lexical_depth < fn.lexical_depth)
									fn.x_breaks.insert(xb);
							}
						}
					}
				} else if (auto as_pull = get_if<LambdaDep::tp_pull>(&n->val)) {
					if (seen.insert(n).second) {
						for (auto& p : *as_pull) {
							scan(&*p.pinned());
						}
					}
				}
			};
			if (auto it = node_lambdas.find(c->callee); it != node_lambdas.end()) {
				scan(&*it->second);
			}
			for (auto& p : c->params) {
				if (auto it = node_lambdas.find(p); it != node_lambdas.end()) {
					scan(&*it->second);
				}
			}
		}
		lambda_levels.pop_back();
		calls_to_fix = move(prev_calls_to_fix);
		node_lambdas = move(prev_node_lambdas);
	}
	void fix_block(ast::Block& b, bool is_fn = false) {
		b.lexical_depth = lambda_levels.size() - 1;
		for (auto& p : b.names) {
			if (p->initializer)
				fix(p->initializer);
			p->lexical_depth = lambda_levels.size() - 1;
			if (auto it_l = node_lambdas.find(p->initializer); it_l != node_lambdas.end()) {
				node_lambdas.insert({
					p,
					is_fn
						? new LambdaDep(weak<ast::MkLambda>(nullptr))
						: new LambdaDep(unordered_set{ it_l->second.weaked() })
				});
			}
		}
		for (auto& b : b.body)
			fix(b);
		pin<ast::Block> break_target;
		for (auto br_it = b.breaks.begin(); br_it != b.breaks.end();) {
			auto& br = *br_it;
			if (br->lexical_depth != br->block->lexical_depth) {
				if (!break_target) {
					if (dom::isa<ast::Block>(b)) {
						break_target = &b;
					} else {
						break_target = ast::make_at_location<ast::Block>(b);
						break_target->type_ = cast<ast::TpLambda>(b.type())->params.back();
						break_target->lexical_depth = b.lexical_depth;
						std::swap(b.body, break_target->body);
						b.body.push_back(break_target);
					}
					auto x_var = ast::make_at_location<ast::Var>(b);
					x_var->name = "_x_break";
					x_var->is_mutable = true;
					x_var->captured = true;
					x_var->type = ast->tp_optional(cast<ast::TpLambda>(b.type())->params.back());
					x_var->lexical_depth = b.lexical_depth;
					lambda_levels.back()->captured_locals.push_back(x_var);
					break_target->names.insert(break_target->names.begin(), x_var);
				}
				if (&b != break_target) {
					br->block = break_target;
					break_target->breaks.insert(br);
					br_it = b.breaks.erase(br_it);
				}
			} else {
				++br_it;
			}
		}
	}

	void fix_var_depth(pin<ast::Var> var) {
		if (var->is_const)
			return;
		if (var->lexical_depth != lambda_levels.size() - 1) {
			if (!var->captured) {
				var->captured = true;
				lambda_levels[var->lexical_depth]->captured_locals.push_back(var);
			}
		}
	}
	void on_break(ast::Break& node) override {
		ast::ActionScanner::on_break(node);
		if (node.block) {
			node.lexical_depth = lambda_levels.size() - 1;
			for (size_t i = lambda_levels.size() - 1; i > node.block->lexical_depth; --i) {
				lambda_levels[i]->x_breaks.insert(weak<ast::Break>(&node));
			}
		}
	}
	void on_bin_op(ast::BinaryOp& node) override {
		ast::ActionScanner::on_bin_op(node);
		auto l_it = node_lambdas.find(node.p[0]);
		auto r_it = node_lambdas.find(node.p[1]);
		if (l_it != node_lambdas.end()) {
			if (r_it != node_lambdas.end()) {
				node_lambdas.insert({
					&node,
					new LambdaDep(unordered_set{
						l_it->second.weaked(),
						r_it->second.weaked() })
					});
			} else {
				node_lambdas.insert({ &node, l_it->second });
			}
		} else if (r_it != node_lambdas.end()) {
			node_lambdas.insert({ &node, r_it->second });
		}
	}

	void on_get(ast::Get& node) override {
		fix_var_depth(node.var);
		if (dom::isa<ast::TpLambda>(*node.type())) {
			if (auto it = node_lambdas.find(node.var); it != node_lambdas.end()) {
				node_lambdas.insert({ &node, it->second });
			}
		}
	}

	void on_set(ast::Set& node) override {
		fix(node.val);
		fix_var_depth(node.var);
		if (!node.var->is_mutable) {
			node.var->is_mutable = true;
			lambda_levels[node.var->lexical_depth]->mutables.push_back(node.var);
		}
		if (dom::isa<ast::TpLambda>(*node.val->type())) {
			if (auto var_it = node_lambdas.find(node.var); var_it != node_lambdas.end()) {
				if (auto val_it = node_lambdas.find(node.val); val_it != node_lambdas.end()) {
					get_if<LambdaDep::tp_pull>(&var_it->second->val)->insert(val_it->second);
					node_lambdas.insert({ &node, var_it->second });
				}
			}
		}
	}

	void on_block(ast::Block& node) override {
		fix_block(node);
	}
 
	void on_mk_lambda(ast::MkLambda& node) override {
		fix_fn(node);
		node_lambdas.insert({ &node, new LambdaDep(pin<ast::MkLambda>(&node).weaked()) });
	}

	void on_call(ast::Call& node) override {
		ast::ActionScanner::on_call(node);
		if (cast<ast::TpLambda>(node.callee->type())->can_x_break) {
			calls_to_fix.push_back(&node);
		}
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
