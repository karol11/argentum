#include "compiler/parser.h"

#include <iostream>
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
using module_text_provider_t = const std::function<string (pin<Name> name)>&;

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
	pin<Name> module_name;
	int32_t pos = 1;
	int32_t line = 1;
	const char* cur = nullptr;
	unordered_set<pin<dom::Name>>& modules_in_dep_path;

	Parser(pin<Ast> ast, pin<Name> module_name, unordered_set<pin<dom::Name>>& modules_in_dep_path)
		: dom(ast->dom)
		, ast(ast)
		, module_name(module_name)
		, modules_in_dep_path(modules_in_dep_path)
	{}

	void parse_fn_def(pin<ast::Function> fn) {
		if (match("(")) {
			while (!match(")")) {
				auto param = make<ast::Var>();
				fn->names.push_back(param);
				param->initializer = parse_type();
				param->name = ast->dom->names()->get(expect_id("parameter name"));
				if (match(")"))
					break;
				expect(",");
			}
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
			expect("{");
		} else {
			if (match(";")) {
				fn->type_expression = make<ast::ConstVoid>();
				fn->is_platform = true;
				return;
			}
			if (match("{")) {
				fn->type_expression = make<ast::ConstVoid>();
			} else {
				fn->type_expression = parse_type();
				if (match(";")) {
					fn->is_platform = true;
					return;
				}
				expect("{");
			}
		}
		parse_statement_sequence(fn->body);
		if (as_method && as_method->is_factory) {
			fn->body.push_back(fn->type_expression);  // this
		}
		expect("}");
	}

	pin<ast::Method> make_method(pin<dom::Name> name, pin<ast::TpClass> cls, bool is_interface) {
		auto method = make<ast::Method>();
		method->name = name;
		auto this_param = make<ast::Var>();
		method->names.push_back(this_param);
		this_param->name = ast->dom->names()->get("this");
		auto this_init = make<ast::MkInstance>();
		this_init->cls = cls;
		this_param->initializer = this_init;
		parse_fn_def(method);
		if (is_interface != method->body.empty()) {
			error(is_interface ? "empty" : "not empty", " body expected");
		}
		return method;
	}

	void parse(module_text_provider_t module_text_provider)
	{
		if (modules_in_dep_path.count(module_name) != 0) {
			string msg = "curcular dependency in modules:";
			for (auto& m : modules_in_dep_path)
				msg += std::to_string(m) + " ";
			error(msg);
		}
		if (ast->module_names.count(module_name) != 0)
				return;
		ast->module_names.insert(module_name);
		modules_in_dep_path.insert(module_name);
		text = module_text_provider(module_name);
		cur = text.c_str();
		match_ws();
		while (match("using")) {
			Parser(ast, expect_domain_name("imported module"), modules_in_dep_path).parse(module_text_provider);
			expect(";");
		}
		for (;;) {
			bool is_test = match("test");
			bool is_interface = match("interface");
			if (is_interface || match("class")) {
				auto cls = ast->get_class(expect_domain_name("class or interface name"));
				cls->is_interface = is_interface;
				cls->is_test = is_test;
				expect("{");
				while (!match("}")) {
					if (match("+")) {
						auto& base_content = cls->overloads[ast->get_class(expect_domain_name("base class or interface"))];
						if (match("{")) {
							if (is_interface)
								error("interface can't have overrides");
							while (!match("}"))
								base_content.push_back(make_method(expect_domain_name("override method name"), cls, is_interface));
						} else {
							expect(";");
						}
					} else {
						auto member_name = expect_domain_name("method or field name");
						if (match("=")) {
							cls->fields.push_back(make<ast::Field>());
							cls->fields.back()->name = member_name;
							cls->fields.back()->initializer = parse_expression();
							expect(";");
						} else {
							cls->new_methods.push_back(make_method(member_name, cls, is_interface));
						}
					}
				}
			} else if (match("fn")) {
				auto fn = make<ast::Function>();
				fn->name = expect_domain_name("function name");
				fn->is_test = is_test;
				auto& fn_ref = ast->functions_by_names[fn->name];
				if (fn_ref)
					error("duplicated function name, ", fn->name.pinned(), " see ", *fn_ref.pinned());
				fn_ref = fn;
				ast->functions.push_back(fn);
				parse_fn_def(fn);
			} else if (is_test) {
				auto fn = make<ast::Function>();
				fn->name = expect_domain_name("function name");
				fn->is_test = true;
				auto& fn_ref = ast->tests_by_names[fn->name];
				if (fn_ref)
					error("duplicated test name, ", fn->name.pinned(), " see ", *fn_ref.pinned());
				fn_ref = fn;
				parse_fn_def(fn);
			} else {
				break;
			}
		}
		ast->entry_point = make<ast::Function>();
		if (*cur)
			parse_statement_sequence(ast->entry_point->body);
		if (*cur)
			error("unexpected statements");
		modules_in_dep_path.erase(module_name);
	}

	void parse_statement_sequence(vector<own<Action>>& body) {
		do {
			if (*cur == '}') {
				body.push_back(make<ast::ConstVoid>());
				break;
			}
			body.push_back(parse_statement());
		} while (match(";"));
	}

	pin<Action> parse_type() {
		if (match("~"))
			return parse_expression();
		if (match("int"))
			return mk_const<ast::ConstInt64>(0);
		if (match("double"))
			return mk_const<ast::ConstDouble>(0.0);
		if (match("bool"))
			return make<ast::ConstBool>();
		if (match("void"))
			return make<ast::ConstVoid>();
		if (match("?")) {
			auto r = make<ast::If>();
			r->p[0] = make<ast::ConstBool>();
			r->p[1] = parse_type();
			return r;
		}
		if (match("&")) {
			auto r = make<ast::MkWeakOp>();
			auto get = make<ast::Get>();
			get->var_name = expect_domain_name("class or interface name");
			r->p = get;
			return r;
		}
		if (match("@")) {
			auto get = make<ast::Get>();
			get->var_name = expect_domain_name("class or interface name");
			return get;
		}
		auto parse_params = [&](pin<ast::MkLambda> fn) {
			if (!match(")")) {
				for (;;) {
					fn->names.push_back(make<ast::Var>());
					fn->names.back()->initializer = parse_type();
					if (match(")"))
						break;
					expect(",");
				}
			}
			fn->body.push_back(parse_type());
			return fn;
		};
		if (match("fn")) {
			expect("(");
			return parse_params(make<ast::Function>());
		}
		if (match("("))
			return parse_params(make<ast::MkLambda>());
		if (auto name = match_domain_name("class or interface name")) {
			auto r = make<ast::RefOp>();
			auto get = make<ast::Get>();
			get->var_name = *name;
			r->p = get;
			return r;
		}
		// TODO &(T,T)T - delegate
		error("Expected type name");
	}

	pin<Action> parse_statement() {
		auto r = parse_expression();
		if (auto as_get = dom::strict_cast<ast::Get>(r)) {
			if (!as_get->var && match("=")) {
				if (as_get->var_name->domain != ast->dom->names())
					error("local var names should not contain '_'");
				auto block = make<ast::Block>();
				auto var = make_at_location<ast::Var>(*r);
				var->name = as_get->var_name;
				var->initializer = parse_expression();
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

	pin<Action> parse_elses() {
		auto r = parse_ifs();
		while (match(":"))
			r = fill(make<ast::Else>(), r, parse_ifs());
		return r;
	}

	pin<Action> parse_ifs() {
		auto r = parse_ors();
		while (match("?")) {
			auto rhs = make<ast::Block>();
			rhs->names.push_back(make<ast::Var>());
			rhs->names.back()->name = ast->dom->names()->get(match("=") ? expect_id("local") : "_");
			rhs->body.push_back(parse_ors());
			r = fill(make<ast::If>(), r, rhs);
		}
		return r;
	}

	pin<Action> parse_ors() {
		auto r = parse_ands();
		while (match("||"))
			r = fill(make<ast::LOr>(), r, parse_ands());
		return r;
	}

	pin<Action> parse_ands() {
		auto r = parse_comparisons();
		while (match("&&")) {
			auto rhs = make<ast::Block>();
			rhs->names.push_back(make<ast::Var>());
			rhs->names.back()->name = ast->dom->names()->get(match("=") ? expect_id("local") : "_");
			rhs->body.push_back(parse_comparisons());
			r = fill(make<ast::LAnd>(), r, rhs);
		}
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

	pin<Action> parse_unar() {
		auto r = parse_unar_head();
		for (;;) {
			if (match("(")) {
				auto call = make<ast::Call>();
				call->callee = r;
				r = call;
				while (!match(")")) {
					call->params.push_back(parse_expression());
					if (match(")"))
						break;
					expect(",");
				}
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
				pin<ast::FieldRef> gf = make<ast::GetField>();
				gf->field_name = expect_domain_name("field name");
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
					}
					gf->base = r;
					r = gf;
				}
			} else if (match(":=")) {
				r = make_set_op(r, [&] { return parse_expression(); });
			} else if (auto op = match_set_op()) {
				r = make_set_op(r, [&] {
					op->p[0] = r;
					op->p[1] = parse_expression();
					return op;
				});
			} else if (match("~")) {
				r = fill(make<ast::CastOp>(), r, parse_unar_head());
			} else
				return r;
		}
	}

	pin<Action> parse_unar_head() {
		if (match("(")) {
			pin<Action> start_expr;
			auto lambda = make<ast::MkLambda>();
			if (!match(")")) {
				start_expr = parse_expression();
				while (!match(")")) {
					expect(",");
					lambda->names.push_back(make<ast::Var>());
					lambda->names.back()->name = ast->dom->names()->get(expect_id("parameter"));
				}
			}
			if (match("{")) {
				if (start_expr) {
					if (auto as_ref = dom::strict_cast<ast::Get>(start_expr)) {
						lambda->names.insert(lambda->names.begin(), make<ast::Var>());
						lambda->names.front()->name = as_ref->var_name;
					} else {
						start_expr->error("lambda definition requires parameter name");
					}
				}
				parse_statement_sequence(lambda->body);
				expect("}");
				return lambda;
			} else if (lambda->names.empty() && start_expr){
				return start_expr;
			}
			lambda->error("expected single expression in parentesis or lambda {body}");
		}
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
		if (auto n = match_num()) {
			if (auto v = get_if<uint64_t>(&*n))
				return mk_const<ast::ConstInt64>(*v);
			if (auto v = get_if<double>(&*n))
				return mk_const<ast::ConstDouble>(*v);
		}
		if (match("{")) {
			auto r = make<ast::Block>();
			parse_statement_sequence(r->body);
			expect("}");
			return r;
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
		if (match("void"))
			return make<ast::ConstVoid>();
		if (match("int"))
			return fill(make<ast::ToIntOp>(), parse_expression_in_parethesis());
		if (match("double"))
			return fill(make<ast::ToFloatOp>(), parse_expression_in_parethesis());
		if (match("loop")) 
			return fill(make<ast::Loop>(), parse_unar());
		if (auto name = match("_")) {
			auto r = make<ast::Get>();
			r->var_name = ast->dom->names()->get("_");
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
		if (match_ns("\"")) {
			auto r = make<ast::ConstString>();
			for (;;) {
				int c = get_utf8(&cur);
				if (!c) {
					error("incomplete string constant");
				} if (c == '"') {
					if (*cur != '"')
						break;
					r->value += '"';
					cur++;
				} else {
					put_utf8(c, &r->value, [](void* ctx, int c) {
						*(string*)ctx += c;
						return 1;
					});
				}
			}
			return r;
		}
		if (auto name = match_domain_name("domain name")) {
			auto r = make<ast::Get>();
			r->var_name = *name;
			return r;
		}
		error("syntax error");
	}

	template<typename T, typename VT>
	pin<Action> mk_const(VT&& v) {
		auto r = pin<T>::make();
		r->value = v;
		return r;
	}

	template<typename T>
	pin<T> make() {
		auto r = pin<T>::make();
		r->line = line;
		r->pos = pos;
		r->module_name = module_name;
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
		if (auto n = match_num()) {
			if (auto v = get_if<uint64_t>(&*n))
				return *v;
			if (auto v = get_if<double>(&*n))
				return *v;
		}
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

	bool match_ws() {
		const char* c = cur;
		for (;; line++, pos = 1) {
			while (*cur == ' ') {
				++cur;
				++pos;
			}
			if (*cur == '\t') {
				error("tabs aren't allowed as white space");
			}
			if (*cur == '/' && cur[1] == '/') {
				while (*cur && *cur != '\n' && *cur != '\r') {
					++cur;
				}
			}
			if (*cur == '\n') {
				if (*++cur == '\r')
					++cur;
			}
			else if (*cur == '\r') {
				if (*++cur == '\n')
					++cur;
			}
			else if (!*cur || *cur > ' ')
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
				error("bad symbols in number");
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

	pin<Name> expect_domain_name(const char* message) {
		auto id = expect_id(message);
		auto name = match_domain_name_tail(id, message);
		return name ? name : ast->dom->names()->get(id);
	}

	optional<pin<Name>> match_domain_name(const char* message) {
		auto id = match_id();
		if (!id)
			return nullopt;
		auto name = match_domain_name_tail(*id, message);
		return name ? name : ast->dom->names()->get(*id);
	}

	pin<Name> match_domain_name_tail(string id, const char* message) {
		if (!match("_"))
			return nullptr;
		pin<Name> r = dom->names()->get(id)->get(expect_id(message));
		while (match("_")) {
			string name_val = expect_id(message);
			r = r->get(name_val);
		}
		return r;
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
	pin<Name> module_name,
	std::unordered_set<ltm::pin<dom::Name>>& modules_in_dep_path,
	module_text_provider_t module_text_provider)
{
	Parser(ast, module_name, modules_in_dep_path).parse(module_text_provider);
}
