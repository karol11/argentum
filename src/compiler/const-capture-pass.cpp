#include "compiler/const-capture-pass.h"

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
using std::shared_ptr;
using ltm::pin;
using ltm::weak;
using ltm::own;
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
	unordered_map<pin<ast::Node>, own<LambdaDep>> node_lambdas;  // lambda dependency graph holding data on what lambdas a given node can return, can have cycles. built per-callable
	vector<weak<ast::Call>> calls_to_fix; // calls of the current function which `activates_lambdas` to fill by tracing the `node_lambdas`

	ConstCapturePass(pin<ast::Ast> ast, pin<dom::Dom> dom)
		: dom(dom)
		, ast(ast) {}

/*  // Todo Assess if a Loop->Star and full scan approach works better than multiple DFS
	void fix_calls() {
		unordered_set<pin<LambdaDep>> seen;
		unordered_set<pin<LambdaDep>> path_set;
		vector<pin<LambdaDep>> path_vec;
		unordered_map<
			pin<LambdaDep>,
			shared_ptr<unordered_set<pin<LambdaDep>>>> loops;
		function<void(pin<LambdaDep>)> find_loops = [&](pin<LambdaDep>& n) {
			if (seen.count(n)) return;
			seen.insert(n);
			if (auto as_pull = get_if<LambdaDep::tp_pull>(&n->val)) {
				if (!path_set.insert(n).second) {
					unordered_set<shared_ptr<unordered_set<pin<LambdaDep>>>> intersections;
					for (int i = path_vec.size() - 1;; --i) {
						if (auto it = loops.find(path_vec[i]); it != loops.end()) {
							intersections.insert(it->second);
						}
						if (path_vec[i] == n)
							break;
					}
					auto loop = intersections.size() == 1
						? *intersections.begin()
						: std::make_shared<unordered_set<pin<LambdaDep>>>();
					for (int i = path_vec.size() - 1;; --i) {
						if (loop->insert(path_vec[i]).second)
							loops.insert({ path_vec[i], loop });
						if (path_vec[i] == n)
							break;
					}
					if (intersections.size() > 1) {
						for (auto& l : intersections) {
							for (auto& dep : *l) {
								loop->insert(dep);
								loops.insert({ dep, loop });
							}
						}
					}
				}
				path_vec.push_back(n);
				for (auto& i : *as_pull)
					find_loops(i);
				path_vec.pop_back();
				path_set.erase(n);
			}
		};
		for (auto& n : node_lambdas)	
			find_loops(n.second);
		while (!loops.empty()) {
			auto star = new LambdaDep(unordered_set<weak<LambdaDep>>());
			auto l = loops.begin()->second;
			// turn loops to stars
		}
		// DFS node_lambdas calls to fix
	}*/

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
		auto prev_calls_to_fix = move(calls_to_fix);
		auto prev_node_lambdas = move(node_lambdas);
		fn.lexical_depth = lambda_levels.size();
		lambda_levels.push_back(&fn);
		fix_block(fn, true);
		for (auto& c : calls_to_fix) {
			unordered_set<pin<LambdaDep>> seen;
			function<void(LambdaDep*)> scan = [&](LambdaDep* n) {
				if (auto as_direct = get_if<LambdaDep::tp_direct>(&n->val)) {
					if (&fn != *as_direct) {
						c->activates_lambdas.insert(*as_direct);
						if (!as_direct) {
							for (auto& t : (*as_direct)->x_targets) {
								if (t->lexical_depth < fn.lexical_depth)
									fn.x_targets.insert(t);
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
		bool has_lambda_names = false;
		b.lexical_depth = lambda_levels.size() - 1;
		for (auto& p : b.names) {
			if (p->initializer)
				fix(p->initializer);
			p->lexical_depth = lambda_levels.size() - 1;
			if (auto it_l = node_lambdas.find(p->initializer); it_l != node_lambdas.end()) {
				has_lambda_names = true;
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
			if (auto it = node_lambdas.find(node.var); it != node_lambdas.end())
				node_lambdas.insert({ &node, it->second });
		}
	}

	void on_break(ast::Break& node) override {
		if (node.block->lexical_depth != lambda_levels.size() - 1) {
			if (!dom::isa<ast::Block>(*node.block.pinned())) {
				if (node.block->body.size() == 1 && dom::isa<ast::Block>(*node.block->body.back())) {
					node.block = node.block->body.back().cast<ast::Block>();
				} else {
					auto block = ast::make_at_location<ast::Block>(*node.block.pinned());
					std::swap(block->body, node.block->body);
					node.block->body.push_back(block);
					block->type_ = node.block->type().cast<ast::TpLambda>()->params.back();
					block->lexical_depth = node.block->lexical_depth;
					node.block = block;
				}
			}
			if (node.block->names.empty() || node.block->names[0]->name != "_x_break") {
				auto x_var = pin<ast::Var>::make();
				x_var->name = "_x_break";
				x_var->is_mutable = true;
				x_var->type = ast->tp_optional(node.result->type());
				fix_var_depth(x_var);
				node.block->names.insert(node.block->names.begin(), x_var);
			}
			node.x_var = node.block->names[0];
			for (size_t i = lambda_levels.size() - 1; i > node.block->lexical_depth; --i) {
				lambda_levels[i]->x_targets.insert(node.block);
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
		if (auto var_it = node_lambdas.find(node.var); var_it != node_lambdas.end()) {
			if (auto val_it = node_lambdas.find(node.val); val_it != node_lambdas.end())
				get_if<LambdaDep::tp_pull>(&var_it->second->val)->insert(val_it->second);
			else
				node.error("internal: inconsistent lambdas at assignment");
			node_lambdas.insert({ &node, var_it->second });
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
		if (node.callee->type().cast<ast::TpLambda>()->can_x_break) {
			LambdaDep::tp_pull set;
			calls_to_fix.push_back(&node);
			auto add_dep = [&](own<ast::Action>& n) {
				if (auto it = node_lambdas.find(n); it != node_lambdas.end())
					set.insert(it->second);
			};
			add_dep(node.callee);
			for (auto& p : node.params)
				add_dep(p);
			node_lambdas.insert({
				&node,
				new LambdaDep(move(set))});
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
