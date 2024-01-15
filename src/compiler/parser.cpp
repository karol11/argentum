#include "compiler/parser.h"

#include <iostream>
#include <functional>
#include <string>
#include <vector>
#include <unordered_map>
#include <sstream>
#include <algorithm>
#include <variant>
#include <optional>
#include <cfenv>
#include <cmath>
#include "utils/utf8.h"

namespace {

using std::string;
using std::unordered_map;
using std::unordered_set;
using std::vector;
using std::variant;
using std::optional;
using std::nullopt;
using std::function;
using std::get_if;
using std::pow;
using ltm::weak;
using ltm::own;
using ltm::pin;
using dom::Name;
using ast::Ast;
using ast::Node;
using ast::Action;
using ast::make_at_location;
using module_text_provider_t = const std::function<string (string name)>&;

template<typename FN>
struct Guard{
	FN fn;
	Guard(FN fn) : fn(std::move(fn)) {}
	~Guard() { fn(); }
};
template<typename FN>
Guard<FN> mk_guard(FN fn) { return Guard<FN>(std::move(fn)); }

struct Parser {
	pin<dom::Dom> dom;
	pin<Ast> ast;
	string text;
	string module_name;
	pin<ast::Module> module;
	int32_t pos = 1;
	int32_t line = 1;
	const char* cur = nullptr;
	unordered_set<string>& modules_in_dep_path;
	unordered_map<string, pin<ast::ImmediateDelegate>> delegates;
	pin<ast::Class> current_class;  // To match type parameters
	bool underscore_accessed = false;

	Parser(pin<Ast> ast, string module_name, unordered_set<string>& modules_in_dep_path)
		: dom(ast->dom)
		, ast(ast)
		, module_name(module_name)
		, modules_in_dep_path(modules_in_dep_path)
	{}

	void parse_fn_def(pin<ast::Function> fn) {
		fn->break_name = fn->name;
		expect("(");
		while (!match(")")) {
			auto param = make<ast::Var>();
			fn->names.push_back(param);
			param->name = expect_id("parameter name");
			param->initializer = parse_non_void_type();
			if (match(")"))
				break;
			expect(",");
		}
		auto as_method = dom::strict_cast<ast::Method>(fn);
		if (match("this")) {
			if (as_method)
				as_method->is_factory = true;
			else
				error("only methods return this type.");
			auto get_this = make<ast::Get>();
			get_this->var = fn->names[0];
			fn->type_expression = get_this;
		} else {
			fn->type_expression = parse_maybe_void_type();
		}
		if (match(";")) {
			fn->is_platform = true;
			return;
		}
		expect("{");
		parse_statement_sequence(fn->body);
		if (as_method && as_method->is_factory) {
			fn->body.push_back(fn->type_expression);  // this
		}
		expect("}");
	}
	pin<ast::Method> make_method(const ast::LongName& name, pin<ast::Class> cls, bool is_interface) {
		auto method = make<ast::Method>();
		method->name = name.name;
		method->base_module = name.module;
		ast->add_this_param(*method, cls);
		parse_fn_def(method);
		if (is_interface && !method->body.empty()) {
			error("empty body expected");
		}
		return method;
	}

	ast::LongName expect_long_name(const char* message, pin<ast::Module> def_module) {
		auto id = expect_id(message);
		if (!match("_"))
			return { id, def_module };
		if (auto it = module->direct_imports.find(id); it != module->direct_imports.end())
			return { expect_id(message), it->second };
		if (id == module->name)
			error("names of the current module should not be prefixed with a module name");
		else
			error("module ", id, " is not visible from module ", module->name);
	}

	pin<ast::AbstractClass> get_class_by_name(const char* message) {
		auto [c_name, m] = expect_long_name(message, nullptr);
		if (!m) {
			if (current_class) {
				for (auto& p : current_class->params) {
					if (p->name == c_name)
						return p;
				}
			}
			if (auto it = module->aliases.find(c_name); it != module->aliases.end()) {
				if (auto as_cls = dom::strict_cast<ast::Class>(it->second))
					return as_cls;
			}
			m = module;
		}
		return m->get_class(c_name, line, pos);
	}

	pin<ast::AbstractClass> parse_class_with_params(const char* message, bool allow_class_param) {
		auto r = get_class_by_name(message);
		if (dom::isa<ast::ClassParam>(*r) && !allow_class_param)
			error("Class parameter is not allowed as root name: ", r->get_name());
		if (!match("("))
			return r;
		if (dom::isa<ast::ClassParam>(*r))
			error("Class parameter cannot be parameterized: ", r->get_name());
		vector<weak<ast::AbstractClass>> params{ r };
		do
			params.push_back(parse_class_with_params("class parameter", true));
		while (match(","));
		expect(")");
		return ast->get_class_instance(move(params));
	}

	pin<ast::Module> parse(module_text_provider_t module_text_provider)
	{
		if (modules_in_dep_path.count(module_name) != 0) {
			string msg = "curcular dependency in modules:";
			for (auto& m : modules_in_dep_path)
				msg += m + " ";
			error(msg);
		}
		if (auto it = ast->modules.find(module_name); it != ast->modules.end())
				return it->second;
		module = new ast::Module;
		module->name = module_name;
		if (module_name != "sys")
			module->direct_imports.insert({ "sys", ast->sys });
		ast->modules.insert({ module_name, module });
		modules_in_dep_path.insert(module_name);
		text = module_text_provider(module_name);
		cur = text.c_str();
		match_ws();
		while (match("using")) {
			auto using_name = expect_id("imported module");
			auto used_module = using_name == "sys"
				? ast->sys.pinned()
				: Parser(ast, using_name, modules_in_dep_path).parse(module_text_provider);
			module->direct_imports.insert({ using_name, used_module });
			if (match("{")) {
				do {
					auto my_id = expect_id("alias name");
					auto their_id = my_id;
					if (match("=")) {
						their_id = expect_id("name in package");
					}
					if (auto it = used_module->functions.find(their_id); it != used_module->functions.end())
						module->aliases.insert({ my_id, it->second });
					else if (auto it = used_module->classes.find(their_id); it != used_module->classes.end())
						module->aliases.insert({ my_id, it->second });
					else
						error("unknown name ", their_id, " in module ", using_name);
				} while (match(","));
				expect("}");
			} else {
				expect(";");
			}
		}
		ast->modules_in_order.push_back(module);
		for (;;) {
			if (match("const")) {
				string id = expect_id("const name");
				expect("=");
				auto v = make<ast::Var>();
				v->initializer = parse_expression();
				v->name = id;
				v->is_const = true;
				module->constants.insert({ id, v });
				expect(";");
				continue;
			}
			bool is_test = match("test");
			bool is_interface = match("interface");
			if (is_interface || match("class")) {
				auto cls = dom::strict_cast<ast::Class>(get_class_by_name("class or interface"));
				if (!cls)
					error("interrnal error, class params from outer class");
				current_class = cls;
				bool is_first_time_seen = !cls->is_defined;
				cls->line = line;
				cls->pos = pos;
				cls->is_defined = true;
				// TODO match attributes if existed
				cls->is_interface = is_interface;
				cls->is_test = is_test;
				if (match("(")) {
					if (!is_first_time_seen)
						error("Reopened class must reuse existing type parameters");
					do {
						auto param = make<ast::ClassParam>();
						param->name = expect_id("type parameter name");
						if (match(">"))
							param->is_out = false;
						else if (match("<"))
							param->is_in = false;
						if (*cur != ',' && *cur != ')') {
							param->base = parse_class_with_params("base class for type parameter", false);
							if (dom::strict_cast<ast::ClassParam>(param->base))
								error("Parameter base must be a real class, not parameter");
						} else {
							param->base = ast->object;
						}
						param->index = (int) cls->params.size();
						cls->params.push_back(param);
					} while (match(","));
					expect(")");
				}
				expect("{");
				while (!match("}")) {
					if (match("+")) {
						auto base_class = parse_class_with_params("base class", false); // disallow class param in root
						auto& base_content = cls->overloads[base_class];
						if (match("{")) {
							if (is_interface)
								error("interface can't have overrides");
							while (!match("}"))
								base_content.push_back(make_method(expect_long_name("override method name", nullptr), cls, is_interface));
						} else {
							expect(";");
						}
					} else {
						ast::Mut is_mut =
							match("*") ? ast::Mut::FROZEN :
							match("-") ? ast::Mut::ANY :
							ast::Mut::MUTATING;
						auto member_name = expect_id("method or field name");
						if (match("=")) {
							if (is_mut != ast::Mut::MUTATING)
								error("field can't have '-' or '*' markers");
							cls->fields.push_back(make<ast::Field>());
							cls->fields.back()->name = member_name;
							cls->fields.back()->cls = cls;
							cls->fields.back()->initializer = parse_expression();
							expect(";");
						} else {
							cls->new_methods.push_back(make_method({ member_name, module }, cls, is_interface));
							cls->new_methods.back()->mut = is_mut;
						}
					}
				}
				current_class = nullptr;
			} else if (match("fn")) {
				auto fn = make<ast::Function>();
				fn->name = expect_id("function name");
				fn->is_test = is_test;
				auto& fn_ref = module->functions[fn->name];
				if (fn_ref)
					error("duplicated function name, ", fn->name, " see ", *fn_ref.pinned());
				fn_ref = fn;
				parse_fn_def(fn);
			} else if (is_test) {
				auto fn = make<ast::Function>();
				fn->name = expect_id("test name");
				fn->is_test = true;
				auto& fn_ref = module->tests[fn->name];
				if (fn_ref)
					error("duplicated test name, ", fn->name, " see ", *fn_ref.pinned());
				fn_ref = fn;
				parse_fn_def(fn);
			} else {
				break;
			}
		}
		module->entry_point = make<ast::Function>();
		if (*cur)
			parse_statement_sequence(module->entry_point->body);
		if (*cur)
			error("unexpected statements");
		modules_in_dep_path.erase(module_name);
		return module;
	}

	void parse_statement_sequence(vector<own<Action>>& body) {
		do {
			if (*cur == '}' || !*cur) {
				body.push_back(make<ast::ConstVoid>());
				break;
			}
			body.push_back(parse_statement());
		} while (match(";"));
	}
	pin<ast::Action> mk_get(const char* kind) {
		auto n = expect_long_name(kind, nullptr);
		if (!n.module && current_class) {
			for (auto& p : current_class->params) {
				if (n.name == p->name) {
					auto inst = make<ast::MkInstance>();
					inst->cls = p;
					return inst;
				}
			}
		}
		auto get = make<ast::Get>();
		get->var_name = n.name;
		get->var_module = n.module;
		return get;
	};
	pin<Action> parse_non_void_type() {
		auto r = parse_maybe_void_type();
		if (dom::isa<ast::ConstVoid>(*r))
			error("Expected type name");
		return r;
	}
	pin<Action> parse_maybe_void_type() {
		if (match("~"))
			return parse_expression();
		if (match("int"))
			return mk_const<ast::ConstInt64>(0);
		if (match("double"))
			return mk_const<ast::ConstDouble>(0.0);
		if (match("bool"))
			return make<ast::ConstBool>();
		if (match("?")) {
			auto r = make<ast::If>();
			r->p[0] = make<ast::ConstBool>();
			r->p[1] = parse_non_void_type();  // use bool for ?void
			return r;
		}
		auto parse_params = [&](pin<ast::MkLambda> fn) {
			if (!match(")")) {
				for (;;) {
					fn->names.push_back(make<ast::Var>());
					fn->names.back()->initializer = parse_non_void_type();
					if (match(")"))
						break;
					expect(",");
				}
			}
			return fn;
		};
		auto parse_pointer = [&]() {
			auto inst = make<ast::MkInstance>();
			inst->cls = parse_class_with_params(
				"class or interface name",
				true);  // allow class param
			return inst;
		};
		if (match("&")) {
			if (match("-")) {
				return fill(
					make<ast::MkWeakOp>(),
					fill(make<ast::ConformOp>(), parse_pointer()));
			}
			if (match("*")) {
				return fill(
					make<ast::MkWeakOp>(),
					fill(make<ast::FreezeOp>(), parse_pointer()));
			}
			if (match("(")) {
				auto fn = make<ast::ImmediateDelegate>();
				ast->add_this_param(*fn, nullptr);  // type to be set at the type resolution pass
				parse_params(fn);
				fn->type_expression = parse_maybe_void_type();
				return fn;
			}
			return fill(make<ast::MkWeakOp>(), parse_pointer());
		}
		if (match("-")) {
			return fill(make<ast::ConformOp>(), parse_pointer());
		}
		if (match("*")) {
			return fill(make<ast::FreezeOp>(), parse_pointer());
		}
		if (match("@")) {
			return parse_pointer();
		}
		if (match("fn")) {
			expect("(");
			auto fn = make<ast::Function>();
			parse_params(fn);
			fn->type_expression = parse_maybe_void_type();
			return fn;
		}
		if (match("(")) {
			auto fn = parse_params(make<ast::MkLambda>());
			fn->body.push_back(parse_maybe_void_type());
			return fn;
		}
		if (is_id_head(*cur)) {
			return fill(make<ast::RefOp>(), parse_pointer());
		}
		return make<ast::ConstVoid>();
	}

	pin<Action> parse_statement() {
		auto r = parse_expression();
		if (auto as_get = dom::strict_cast<ast::Get>(r)) {
			if (!as_get->var && match("=")) {
				if (as_get->var_module)
					error("local var names should not contain '_'");
				auto block = make<ast::Block>();
				auto var = make_at_location<ast::Var>(*r);
				var->name = as_get->var_name;
				auto initializer = make<ast::Block>();
				var->initializer = initializer;
				initializer->body.push_back(parse_expression());
				initializer->break_name = var->name;
				block->names.push_back(var);
				expect(";");
				parse_statement_sequence(block->body);
				return block;
			}
		}
		return r;
	}

	pin<Action> parse_expression() {
		return parse_elses();
	}

	pin<ast::MkLambda> maybe_parse_lambda() {
		if (*cur != '`' && *cur != '\\' && *cur != '{')
			return nullptr;
		auto r = make<ast::MkLambda>();
		if (*cur == '`') {
			while (match("`")) {
				r->names.push_back(make<ast::Var>());
				r->names.back()->name = expect_id("lambda parameter name");
			}
			if (match("{"))
				parse_block(r);
			else
				r->body.push_back(parse_expression());
		} else {
			bool prev_underscore = underscore_accessed;
			underscore_accessed = false;
			if (match("{"))
				parse_block(r);
			else {
				expect("\\");
				r->body.push_back(parse_expression());
			}
			if (underscore_accessed) {
				r->names.push_back(make<ast::Var>());
				r->names.back()->name = "_";
			}
			underscore_accessed = underscore_accessed || prev_underscore; // we accumulate all underscores upstream
		}
		return r;
	}

	pin<Action> parse_lambda_0_params(function<pin<ast::Action>()> single_expression_parser) {
		if (match("`"))
			error("expected lambda without parameters or an expression");
		return match("\\")
			? parse_expression()
			: single_expression_parser();
	}

	pin<ast::Block> parse_lambda_1_param(function<pin<ast::Action>()> single_expression_parser) {
		auto r = make<ast::Block>();
		r->names.push_back(make<ast::Var>());
		if (match("`")) {
			r->names.back()->name = expect_id("lambda parameter");
			if (match("`"))
				error("expected single-parameter lambda");
			r->body.push_back(parse_unar());
		} else {
			r->names.back()->name = "_";
			r->body.push_back(match("\\")
				? parse_expression()
				: single_expression_parser());
		}
		return r;
	}

	pin<Action> parse_elses() {
		auto r = parse_ifs();
		while (match(":"))
			r = fill(
				make<ast::Else>(),
				r,
				parse_lambda_0_params([&] { return parse_ifs(); }));
		return r;
	}

	pin<Action> parse_ifs() {
		auto r = parse_ors();
		if (match("&&"))
			return fill(
				make<ast::LAnd>(),
				r,
				parse_lambda_1_param([&] { return parse_ifs(); }));
		if (match("?")) {
			return fill(
				make<ast::If>(),
				r,
				parse_lambda_1_param([&] { return parse_ifs(); }));
		}
		return r;
	}

	pin<Action> parse_ors() {
		auto r = parse_comparisons();
		while (match("||"))
			r = fill(
				make<ast::LOr>(),
				r,
				parse_lambda_0_params([&] { return parse_comparisons(); }));
		return r;
	}

	pin<Action> parse_comparisons() {
		auto r = parse_adds();
		if (match("==")) return fill(make<ast::EqOp>(), r, parse_adds());
		if (match(">=")) return fill(make<ast::NotOp>(), fill(make<ast::LtOp>(), r, parse_adds()));
		if (match("<=")) return fill(make<ast::NotOp>(), fill(make<ast::LtOp>(), parse_adds(), r));
		if (match("<")) return fill(make<ast::LtOp>(), r, parse_adds());
		if (match(">")) return fill(make<ast::LtOp>(), parse_adds(), r);
		if (match("!=")) return fill(make<ast::NotOp>(), fill(make<ast::EqOp>(), r, parse_adds()));
		return r;
	}

	pin<Action> parse_adds() {
		auto r = parse_muls();
		for (;;) {
			if (match("+")) r = fill(make<ast::AddOp>(), r, parse_muls());
			else if (match("-")) r = fill(make<ast::SubOp>(), r, parse_muls());
			else break;
		}
		return r;
	}

	pin<Action> parse_muls() {
		auto r = parse_unar();
		for (;;) {
			if (match("*")) r = fill(make<ast::MulOp>(), r, parse_unar());
			else if (match("/")) r = fill(make<ast::DivOp>(), r, parse_unar());
			else if (match("%")) r = fill(make<ast::ModOp>(), r, parse_unar());
			else if (match("<<")) r = fill(make<ast::ShlOp>(), r, parse_unar());
			else if (match(">>")) r = fill(make<ast::ShrOp>(), r, parse_unar());
			else if (match_and_not("&", '&')) r = fill(make<ast::AndOp>(), r, parse_unar());
			else if (match_and_not("|", '|')) r = fill(make<ast::OrOp>(), r, parse_unar());
			else if (match("^")) r = fill(make<ast::XorOp>(), r, parse_unar());
			else break;
		}
		return r;
	}

	pin<Action> parse_expression_in_parethesis() {
		expect("(");
		auto r = parse_expression();
		expect(")");
		return r;
	}

	pin<ast::BinaryOp> match_set_op() {
		if (match("+=")) return make<ast::AddOp>();
		if (match("-=")) return make<ast::SubOp>();
		if (match("*=")) return make<ast::MulOp>();
		if (match("/=")) return make<ast::DivOp>();
		if (match("%=")) return make<ast::ModOp>();
		if (match("<<=")) return make<ast::ShlOp>();
		if (match(">>=")) return make<ast::ShrOp>();
		if (match("&=")) return make<ast::AndOp>();
		if (match("|=")) return make<ast::OrOp>();
		if (match("^=")) return make<ast::XorOp>();
		return nullptr;
	}
	pin<Action> make_set_op(pin<Action> assignee, std::function<pin<Action>()> val) {
		if (auto as_get = dom::strict_cast<ast::Get>(assignee)) {
			auto set = make<ast::Set>();
			set->var = as_get->var;
			set->var_name = as_get->var_name;
			set->val = val();
			return set;
		}
		error("expected variable name in front of <set>= operator");
	}
	pin<Action> parse_call(pin<Action> callee, pin<ast::Call> call) {
		call->callee = callee;
		while (!match(")")) {
			call->params.push_back(parse_expression());
			if (match(")"))
				break;
			expect(",");
		}
		if (auto extra_param = maybe_parse_lambda())
			call->params.push_back(extra_param);
		return call;
	}
	pin<Action> parse_unar() {
		auto r = parse_unar_head();
		for (;;) {
			if (match("(")) {
				r = parse_call(r, make<ast::Call>());
			} else if (match("[")) {
				auto gi = make<ast::GetAtIndex>();
				do
					gi->indexes.push_back(parse_expression());
				while (match(","));
				expect("]");
				if (auto op = match_set_op()) {
					auto block = make_at_location<ast::Block>(*gi);
					block->names.push_back(make_at_location<ast::Var>(*r));
					block->names.back()->initializer = r;
					auto indexed = make_at_location<ast::Get>(*gi);
					indexed->var = block->names.back();
					for (auto& var : gi->indexes) {
						block->names.push_back(make_at_location<ast::Var>(*var));
						block->names.back()->initializer = move(var);
						auto index = make_at_location<ast::Get>(*block->names.back()->initializer);
						index->var = block->names.back();
						var = index;
					}
					auto si = make_at_location<ast::SetAtIndex>(*gi);
					si->indexed = indexed;
					gi->indexed = indexed;
					si->indexes = gi->indexes;
					si->value = op;
					op->p[0] = gi;
					op->p[1] = parse_expression();
					block->body.push_back(si);
					r = block;
				} else {
					if (match(":=")) {
						auto si = make_at_location<ast::SetAtIndex>(*gi);
						si->indexes = move(gi->indexes);
						si->value = parse_expression();
						gi = si;
					}
					gi->indexed = r;
					r = gi;
				}
			} else if (match(".")) {
				if (*cur == '`' || *cur == '\\' || *cur == '{') {
					auto block = parse_lambda_1_param([&] { return parse_unar(); });
					block->names.front()->initializer = r;
					auto ret = make<ast::Get>();
					ret->var = block->names.front();
					block->body.push_back(ret);
					r = block;
				} else if (match("&")) {
					auto d = make<ast::ImmediateDelegate>();
					d->base = r;
					d->name = expect_id("delegate name");
					auto& d_ref = delegates[d->name];
					if (d_ref)
						error("duplicated delegate name, ", d->name, " see ", *d_ref);
					d_ref = d;
					ast->add_this_param(*d, nullptr);  // this type to be patched at the type resolver pass
					parse_fn_def(d);
					r = d;
				} else {
					pin<ast::FieldRef> gf = make<ast::GetField>();
					auto field_n = expect_long_name("field name", nullptr);
					gf->field_name = field_n.name;
					gf->field_module = field_n.module;
					if (auto op = match_set_op()) {
						auto block = make_at_location<ast::Block>(*gf);
						block->names.push_back(make_at_location<ast::Var>(*r));
						block->names.back()->initializer = r;
						auto field_base = make_at_location<ast::Get>(*gf);
						field_base->var = block->names.back();
						auto sf = make_at_location<ast::SetField>(*gf);
						sf->field_name = gf->field_name;
						sf->base = field_base;
						gf->base = field_base;
						sf->val = op;
						op->p[0] = gf;
						op->p[1] = parse_expression();
						block->body.push_back(sf);
						r = block;
					} else {
						if (match(":=")) {
							auto sf = make_at_location<ast::SetField>(*gf);
							sf->field_name = gf->field_name;
							sf->val = parse_expression();
							gf = sf;
						} else if (match("@=")) {
							auto sf = make_at_location<ast::SpliceField>(*gf);
							sf->field_name = gf->field_name;
							sf->val = parse_expression();
							gf = sf;
						}
						gf->base = r;
						r = gf;
					}
				}
			} else if (match(":=")) {
				r = make_set_op(r, [&] { return parse_expression(); });
			} else if (auto op = match_set_op()) {
				r = make_set_op(r, [&] {
					op->p[0] = r;
					op->p[1] = parse_expression();
					return op;
					});
			} else if (match("->")) {
				auto block = parse_lambda_1_param([&] { return parse_unar(); });
				block->names.front()->initializer = r;
				r = block;
			} else if (match("~")) {
				if (match("("))
					r = parse_call(r, make<ast::AsyncCall>());
				else
					r = fill(make<ast::CastOp>(), r, parse_unar_head());
			} else if (auto lambda_tail = maybe_parse_lambda()) {
				auto call = make_at_location<ast::Call>(*lambda_tail);
				call->callee = r;
				call->params.push_back(lambda_tail);
				r = call;
			} else
				return r;
		}
	}
	void parse_block(pin<ast::Block> block) {
		if (match("="))
			block->break_name = expect_id("name for breaks");
		parse_statement_sequence(block->body);
		expect("}");
	}

	pin<Action> parse_unar_head() {
		if (match("(")) {
			pin<Action> expr  = parse_expression();
			expect(")");
			return expr;
		}
		if (match("{")) {
			auto r = make<ast::Block>();
			parse_block(r);
			return r;
		}
		if (auto r = maybe_parse_lambda())
			return r;
		if (match("*"))
			return fill(make<ast::FreezeOp>(), parse_unar());
		if (match("@"))
			return fill(make<ast::CopyOp>(), parse_unar());
		if (match("&"))
			return fill(make<ast::MkWeakOp>(), parse_unar());
		if (match("!"))
			return fill(make<ast::NotOp>(), parse_unar());
		if (match("-"))
			return fill(make<ast::NegOp>(), parse_unar());
		if (match("~"))
			return fill(make<ast::XorOp>(),
				parse_unar(),
				mk_const<ast::ConstInt64>(-1));
		if (match("^")) {
			auto r = make<ast::Break>();
			r->block_name = expect_id("block to break");
			if (match("="))
				r->result = parse_expression();
			else
				r->result = make<ast::ConstVoid>();
			return r;
		}
		if (auto n = match_num()) {
			if (auto v = get_if<uint64_t>(&*n))
				return mk_const<ast::ConstInt64>(*v);
			if (auto v = get_if<double>(&*n))
				return mk_const<ast::ConstDouble>(*v);
		}
		bool matched_true = match("+");
		if (matched_true || match("?")) {
			auto r = make<ast::If>();
			auto cond = make<ast::ConstBool>();
			cond->value = matched_true;
			r->p[0] = cond;
			r->p[1] = parse_unar();
			return r;
		}
		matched_true = match("true");
		if (matched_true || match("false")) {
			auto r = make<ast::ConstBool>();
			r->value = matched_true;
			return r;
		}
		if (match("int"))
			return fill(make<ast::ToIntOp>(), parse_expression_in_parethesis());
		if (match("double"))
			return fill(make<ast::ToFloatOp>(), parse_expression_in_parethesis());
		if (match("loop")) 
			return fill(make<ast::Loop>(), parse_unar());
		if (auto name = match("_")) {
			auto r = make<ast::Get>();
			r->var_name = "_";
			underscore_accessed = true;
			return r;
		}
		if (match_ns("'")) {
			auto r = make<ast::ConstInt64>();
			r->value = get_utf8(&cur);
			if (!r->value)
				error("incomplete character constant");
			expect("'");
			return r;
		}
		if (match("utf32_")) {
			expect("(");
			auto r = make<ast::ConstString>();
			do {
				auto param = parse_expression();
				if (auto param_as_int = dom::strict_cast<ast::ConstInt64>(param)) {
					put_utf8(param_as_int->value, &r->value, [](void* dst, int byte) {
						*((string*)dst) += (char)byte;
						return 1;
					});
				} else {  // Todo: remove after const evaluation pass
					error("so far only literal numbers are supported as utf32_ parameter");
				}
			} while (match(","));
			expect(")");
			return r;
		}
		if (match_ns("$\""))
			return StringParser(this, "${").handle_single_line();
		if (match_ns("\""))
			return StringParser(this, "{").handle_maybe_multiline();
		if (is_id_head(*cur))
			return mk_get("name");
		error("syntax error");
	}
	struct StringParser {
		Parser& p;
		bool stop_on_quote = true;
		string prefix;
		int tabstops = 0;
		string open_escape_str;
		char close_escape_char = '}';
		string eoln;
		string last_suffix;
		vector<pin<Action>> parts;
		pin<ast::ConstString> current_part;

		StringParser(Parser* p, string open_escape_str)
			: p(*p), open_escape_str(move(open_escape_str)) {
			current_part = p->make<ast::ConstString>();
		}

		pin<Action> handle_single_line() {
			if (!parse_single_line())
				p.error("string literal is not closed with \"");
			return finalize();
		}

		pin<Action> finalize() {
			current_part->value += last_suffix;
			if (!current_part->value.empty() || parts.empty())
				parts.push_back(current_part);
			if (parts.size() == 1)
				return parts[0];
			auto inst = make_at_location<ast::MkInstance>(*parts[0]);
			inst->cls = p.ast->str_builder.pinned();
			pin<Action> r = inst;
			for (auto& part : parts)
				r = p.fill(make_at_location<ast::ToStrOp>(*part), r, part);
			auto delegate = p.make<ast::GetField>();
			delegate->base = r;
			delegate->field_name = "toStr";
			auto call = make_at_location<ast::Call>(*parts[0]);
			call->callee = delegate;
			return call;
		}

		pin<Action> handle_maybe_multiline() {
			const char* start = p.cur;
			if (parse_single_line())
				return finalize();
			if (!parts.empty())
				p.error("expected \"");
			p.skip_spaces();
			parse_string_format(start);
			stop_on_quote = false;
			auto base_indent = p.pos;
			current_part->value = prefix;
			for (;;) {
				parse_single_line();
				p.skip_spaces();
				if (p.pos < base_indent)
					break;
				current_part->value += eoln;
				current_part->value += prefix;
				if (p.pos > base_indent) {
					char c = ' ';
					int count = p.pos - base_indent;
					if (tabstops) {
						if (count % tabstops != 0)
							p.error("Indent is not aligned to tab width");
						count /= tabstops;
						c = '\t';
					}
					for (count++; --count;)
						current_part->value += c;
				}
			}
			p.expect("\"");
			return finalize();
		}

		bool parse_single_line() {  // returns true if matched quote
			for (; !p.match_eoln(); p.cur++) {
				if (!*p.cur)
					p.error("string constant is not terminated");
				if (!open_escape_str.empty() && p.match_ns(open_escape_str.c_str())) {
					if (*p.cur == close_escape_char) {
						p.cur++;
						current_part->value += open_escape_str;
					} else {
						p.match_ws();
						auto expression = p.parse_expression();
						if (auto expr_as_string = dom::strict_cast<ast::ConstString>(expression)) {
							current_part->value += expr_as_string->value;
						} else {
							if (!current_part->value.empty())
								parts.push_back(current_part);
							parts.push_back(expression);
							current_part = p.make<ast::ConstString>();
						}
						if (*p.cur != close_escape_char)
							p.error("Expected ", close_escape_char);
					}
				} else if (stop_on_quote && p.match("\"")) {
					return true;
				} else {
					current_part->value += *p.cur;
				}
			}
			return false;
		}

		void parse_string_format(const char* c) {
			for (;; c++) {
				if (*c == '.') prefix += ' ';
				else if (*c == 't') prefix += '\t';
				else break;
			}
			for (; *c >= '0' && *c <= '9'; c++)
				tabstops = tabstops * 10 + *c - '0';
			open_escape_str = "";
			close_escape_char = 0;
			while (*c >= '#' && *c <= '&')
				open_escape_str += *c++;
			if (*c == '{') {
				open_escape_str += '{';
				close_escape_char = '}';
			} else if (*c == '(') {
				open_escape_str += '(';
				close_escape_char = ')';
			} else if (*c == '[') {
				open_escape_str += '[';
				close_escape_char = ']';
			} else if (!open_escape_str.empty()) {
				p.error("Expected {[( at the end of the open escape sequence");
			}
			if (close_escape_char != 0) {
				if (c[1] != close_escape_char)
					p.error("Expected ", close_escape_char, " at format string ", c);
				c += 2;
			}
			for (;; c++) {
				if (*c == 'n') eoln += "\n";
				else if (*c == 'r') eoln += "\r";
				else break;
			}
			if (eoln.empty())
				eoln = "\n";
			while (*c == '\\') {
				last_suffix += eoln;
				if (*++c == '+') {
					c++;
					last_suffix += prefix;
				}
			}
			if (*c != '\n' && *c != '\r')
				p.error("Unexpected symbols in format string at ", c);
		}
	};

	template<typename T, typename VT>
	pin<Action> mk_const(VT&& v) {
		auto r = make<T>();
		r->value = v;
		return r;
	}

	template<typename T>
	pin<T> make() {
		auto r = pin<T>::make();
		r->line = line;
		r->pos = pos;
		r->module = module;
		return r;
	}

	pin<ast::Action> fill(pin<ast::UnaryOp> op, pin<ast::Action> param) {
		op->p = param;
		return op;
	}

	pin<ast::Action> fill(pin<ast::BinaryOp> op, pin<ast::Action> param1, pin<ast::Action> param2) {
		op->p[0] = param1;
		op->p[1] = param2;
		return op;
	}

	optional<string> match_id() {
		if (!is_id_head(*cur))
			return nullopt;
		string result;
		while (is_id_body(*cur)) {
			result += *cur++;
			++pos;
		}
		match_ws();
		return result;
	}

	string expect_id(const char* message) {
		if (auto r = match_id())
			return *r;
		error("expected ", message);
	}

	variant<uint64_t, double> expect_number() {
		if (auto n = match_num())
			return *n;
		error("expected number");
	}

	int match_length(const char* str) { // returns 0 if not matched, length to skip if matched
		int i = 0;
		for (; str[i]; i++) {
			if (str[i] != cur[i])
				return 0;
		}
		return i;
	}

	bool match_ns(const char* str) {
		int i = match_length(str);
		if (i == 0 || (is_id_body(str[i - 1]) && is_id_body(cur[i])))
			return false;
		cur += i;
		pos += i;
		return true;
	}

	bool match(const char* str) {
		if (match_ns(str)) {
			match_ws();
			return true;
		}
		return false;
	}

	bool match_and_not(const char* str, char after) {
		if (int i = match_length(str)) {
			if (cur[i] != after) {
				cur += i;
				pos += i;
				match_ws();
				return true;
			}
		}
		return false;
	}

	void expect(const char* str) {
		if (!match(str))
			error(string("expected '") + str + "'");
	}

	bool match_eoln() {
		if (*cur == '\n') {
			if (*++cur == '\r')
				++cur;
		} else if (*cur == '\r') {
			if (*++cur == '\n')
				++cur;
		} else
			return false;
		line++;
		pos = 1;
		return true;
	}

	void skip_spaces() {
		while (*cur == ' ') {
			++cur;
			++pos;
		}
		if (*cur == '\t') {
			error("tabs aren't allowed as white space");
		}
	}

	bool match_ws() {
		const char* c = cur;
		for (;;) {
			skip_spaces();
			if (*cur == '/' && cur[1] == '/') {
				while (*cur && *cur != '\n' && *cur != '\r') {
					++cur;
				}
			}
			if (!match_eoln() && (*cur == 0 || *cur > ' '))
				return c != cur;
		}
	}

	static bool is_id_head(char c) {
		return
			(c >= 'a' && c <= 'z') ||
			(c >= 'A' && c <= 'Z');
	};

	static bool is_num(char c) {
		return c >= '0' && c <= '9';
	};

	static bool is_id_body(char c) {
		return is_id_head(c) || is_num(c);
	};

	static int get_digit(char c) {
		if (c >= '0' && c <= '9')
			return c - '0';
		if (c >= 'a' && c <= 'f')
			return c - 'a' + 10;
		if (c >= 'A' && c <= 'F')
			return c - 'A' + 10;
		return 255;
	}

	optional<variant<uint64_t, double>> match_num() {
		if (!is_num(*cur))
			return nullopt;
		int radix = 10;
		if (*cur == '0') {
			switch (cur[1]) {
			case 'x':
				radix = 16;
				cur += 2;
				break;
			case 'o':
				radix = 8;
				cur += 2;
				break;
			case 'b':
				radix = 2;
				cur += 2;
				break;
			default:
				break;
			}
		}
		uint64_t result = 0;
		for (;; cur++, pos++) {
			if (*cur == '_')
				continue;
			int digit = get_digit(*cur);
			if (digit == 255)
				break;
			if (digit >= radix)
				error("digit with value ", digit, " is not allowed in ", radix, "-base number");
			uint64_t next = result * radix + digit;
			if (next / radix != result)
				error("overflow");
			result = next;
		}
		if (*cur != '.' && *cur != 'e' && *cur != 'E') {
			match_ws();
			return result;
		}
		std::feclearexcept(FE_ALL_EXCEPT);
		double d = double(result);
		if (match_ns(".")) {
			for (double weight = 0.1; is_num(*cur); weight *= 0.1)
				d += weight * (*cur++ - '0');
		}
		if (match_ns("E") || match_ns("e")) {
			int sign = match_ns("-") ? -1 : (match_ns("+"), 1);
			int exp = 0;
			for (; *cur >= '0' && *cur < '9'; cur++)
				exp = exp * 10 + *cur - '0';
			d *= pow(10, exp * sign);
		}
		if (std::fetestexcept(FE_OVERFLOW | FE_UNDERFLOW))
			error("numeric overflow");
		match_ws();
		return d;
	}

	template<typename... T>
	[[noreturn]] void error(const T&... t) {
		std::cerr << "error " << ast::format_str(t...) << " " << module_name << ":" << line << ":" << pos << std::endl;
		throw 1;
	}

	bool is_eof() {
		return !*cur;
	}
};

}  // namespace

void parse(
	pin<Ast> ast,
	string start_module_name,
	module_text_provider_t module_text_provider)
{
	std::unordered_set<string> modules_in_dep_path;
	ast->starting_module = Parser(ast, start_module_name, modules_in_dep_path).parse(module_text_provider);
	if (!ast->starting_module->entry_point || ast->starting_module->entry_point->body.empty()) {
		std::cerr << "error starting module has no entry point" << std::endl;
		throw 1;
	}
}
