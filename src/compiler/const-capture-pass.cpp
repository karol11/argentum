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
	vector<pin<ast::MkLambda>> lambda_levels;
	unordered_map<pin<ast::Node>, own<LambdaDep>> node_lambdas;
	vector<weak<ast::Call>> calls_to_fix;

	ConstCapturePass(pin<ast::Ast> ast, pin<dom::Dom> dom)
		: dom(dom)
		, ast(ast) {}

	void fix_calls() {
		for (auto& c : calls_to_fix) {
			unordered_set<pin<LambdaDep>> seen;
			auto possible_param_lambdas = std::make_shared<unordered_set<weak<ast::MkLambda>>>();
			function<void(LambdaDep*)> scan = [&](LambdaDep* n) {
				if (auto as_direct = get_if<LambdaDep::tp_direct>(&n->val)) {
					possible_param_lambdas->insert(*as_direct);
				} else if (auto as_pull = get_if<LambdaDep::tp_pull>(&n->val)) {
					if (seen.insert(n).second) {
						for (auto& p : *as_pull) {
							scan(&*p.pinned());
						}
					}
				}
			};
			for (auto& p : c->params) {
				if (auto it = node_lambdas.find(p); it != node_lambdas.end()) {
					scan(&*it->second);
				}
			}
			if (!possible_param_lambdas->empty())
				c->possible_param_lambdas = possible_param_lambdas;
		}
		calls_to_fix.clear();
		node_lambdas.clear();
	}
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


		}
		// turn loops to stars
		// DFS node_lambdas calls to fix
	}*/

	void fix_globals() {
		for (auto& c : ast->classes_in_order) {
			for (auto& f : c->fields) {
				fix(f->initializer);
				fix_calls();
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
		fn.lexical_depth = lambda_levels.size();
		lambda_levels.push_back(&fn);
		fn.has_lambda_params = fix_block(fn, true);
		fix_calls();
		lambda_levels.pop_back();
	}
	bool fix_block(ast::Block& b, bool is_fn = false) {
		bool has_lambda_names = false;
		b.lexical_depth = lambda_levels.size() - 1;
		for (auto& p : b.names) {
			if (p->initializer)
				fix(p->initializer);
			p->lexical_depth = lambda_levels.size() - 1;
			if (auto& l = node_lambdas[p->initializer]) {
				has_lambda_names = true;
				node_lambdas.insert({
					p,
					is_fn
						? new LambdaDep(weak<ast::MkLambda>(nullptr))
						: new LambdaDep(unordered_set{ l.weaked() })
				});
			}
		}
		for (auto& b : b.body)
			fix(b);
		return has_lambda_names;
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
		auto& l = node_lambdas[node.p[0]];
		auto& r = node_lambdas[node.p[1]];
		if (l || r) {
			if (l && r) {
				node_lambdas.insert({
					&node,
					new LambdaDep(unordered_set{
						l.weaked(),
						r.weaked() })
					});
			} else {
				node_lambdas.insert({ &node, l ? l : r });
			}
		}
	}

	void on_get(ast::Get& node) override {
		fix_var_depth(node.var);
		if (dom::isa<ast::TpLambda>(*node.type())) {
			node_lambdas.insert({ &node, node_lambdas[&node]});
		}
	}

	void on_break(ast::Break& node) override {
		if (node.block->lexical_depth != lambda_levels.size() - 1) {
			if (node.block->names.empty() || node.block->names[0]->name != "_x_break") {
				auto x_var = pin<ast::Var>::make();
				x_var->name = "_x_break";
				x_var->is_mutable = true;
				x_var->type = ast->tp_optional(node.result->type());
				fix_var_depth(x_var);
				node.block->names.push_back(x_var);
			}
			node.x_var = node.block->names[0];
			for (int i = lambda_levels.size() - 1; i > node.block->lexical_depth; --i) {
				lambda_levels[i]->xbreaks.push_back(&node);
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
		if (auto& l = node_lambdas[node.var]) {
			get_if<LambdaDep::tp_pull>(&l->val)->insert(node_lambdas[node.val]);
			node_lambdas.insert({ &node, l });
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
		auto type = node.type();
		if (auto as_opt = dom::strict_cast<ast::TpOptional>(type))
			type = as_opt->wrapped;
		if (dom::isa<ast::TpLambda>(*type))
			calls_to_fix.push_back(&node);
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
