#ifndef AST_H
#define AST_H

#include <sstream>
#include <unordered_set>
#include <sstream>
#include "dom/dom.h"
#include "dom/dom-to-string.h"

namespace ast {

using std::move;
using std::string;
using std::unordered_map;
using std::unordered_set;
using std::vector;
using ltm::weak;
using ltm::own;
using ltm::pin;
using dom::Name;

struct Node;  // having file position
struct Action;  // having result, able to build code

template<typename... T>
string format_str(const T&... t) { return (std::stringstream() << ... << t).str(); }

struct Node: dom::DomItem {
	int32_t line = 0;
	int32_t pos = 0;
	weak<dom::Name> module_name;
	[[noreturn]] void err_out(const std::string& message);
	template<typename... T>
	[[noreturn]] void error(const T&... t) {
		err_out(format_str("Error at ", *this, ": ", t...));
	}
	template<typename... T>
	[[noreturn]] void context_error(std::function<string()> context, const T&... t) {
		err_out(format_str("Error at ", *this, ": ", t..., " in ", context()));
	}
	string get_annotation() override;
};

struct TypeMatcher;
struct ActionMatcher;

struct Type : dom::DomItem {
	Type() { make_shared(); }
	static own<Type>& promote(own<Type>& to_patch);  // replace cold lambda with a resolved lambda type
	virtual void match(TypeMatcher& matcher) = 0;
};
struct TpInt64 : Type {
	void match(TypeMatcher& matcher) override;
	DECLARE_DOM_CLASS(TpInt64);
};
struct TpDouble : Type {
	void match(TypeMatcher& matcher) override;
	DECLARE_DOM_CLASS(TpDouble);
};
struct TpFunction : Type {
	vector<own<Type>> params;  //+result
	void match(TypeMatcher& matcher) override;
	DECLARE_DOM_CLASS(TpFunction);
};
struct TpLambda : TpFunction {
	void match(TypeMatcher& matcher) override;
	DECLARE_DOM_CLASS(TpLambda);
};
struct TpColdLambda : Type {  // never called not istantiated lambdas with unknown param/result types
	own<Type> resolved;  // this can be Lambda or ColdLambda
	vector<weak<struct MkLambda>> callees;
	void match(TypeMatcher& matcher) override;
	DECLARE_DOM_CLASS(TpColdLambda);
};
struct TpDelegate : TpFunction {
	void match(TypeMatcher& matcher) override;
	DECLARE_DOM_CLASS(TpDelegate);
};
struct TpVoid : Type {
	void match(TypeMatcher& matcher) override;
	DECLARE_DOM_CLASS(TpVoid);
};
struct TpOptional : Type {
	own<Type> wrapped;
	int depth = 0;
	void match(TypeMatcher& matcher) override;
	DECLARE_DOM_CLASS(TpOptional);
};
struct Field : Node {  // TODO: combine with Var
	own<dom::Name> name;
	own<Action> initializer;
	int offset = 0;
	DECLARE_DOM_CLASS(Field);
};
struct TpClass : Type {
	own<dom::Name> name;
	vector<own<Field>> fields;
	vector<own<struct Method>> new_methods;  // new methods ordered by source order
	unordered_map<weak<TpClass>, vector<own<struct Method>>> overloads;  // overloads for interfaces and the base class, ordered by source order
	weak<TpClass> base_class;
	unordered_map<own<dom::Name>, weak<Node>> this_names;  // this names - defined and inhrited. ambiguous excluded.
	unordered_map<           
		weak<TpClass>,                       // base interface
		vector<weak<struct Method>>> interface_vmts;   // inherited and overloaded methods in the order of new_methods in that interface
	bool is_interface = false;
	bool is_test = false;

	void match(TypeMatcher& matcher) override;

	template<typename F, typename M, typename A>
	bool handle_member(ast::Node& node, const pin<dom::Name> name, F on_field, M on_method, A on_anbiguous) {
		if (auto m = dom::peek(this_names, name)) {
			if (!m)
				on_anbiguous();
			else if (auto as_field = dom::strict_cast<ast::Field>(m))
				on_field(as_field);
			else if (auto as_method = dom::strict_cast<ast::Method>(m))
				on_method(as_method);
			else
				node.error("internal error class member is neither method nor field");
			return true;
		}
		return false;
	}
	DECLARE_DOM_CLASS(TpClass);
};
struct TpRef : Type {
	own<TpClass> target;
	void match(TypeMatcher& matcher) override;
	DECLARE_DOM_CLASS(TpRef);
};
struct TpShared : TpRef {
	void match(TypeMatcher& matcher) override;
	DECLARE_DOM_CLASS(TpShared);
};
struct TpWeak : Type {
	own<TpClass> target;
	void match(TypeMatcher& matcher) override;
	DECLARE_DOM_CLASS(TpWeak);
};

struct TypeMatcher {
	virtual ~TypeMatcher() = default;
	virtual void on_int64(TpInt64& type) = 0;
	virtual void on_double(TpDouble& type) = 0;
	virtual void on_function(TpFunction& type) = 0;
	virtual void on_lambda(TpLambda& type) = 0;
	virtual void on_delegate(TpDelegate& type) = 0;
	virtual void on_cold_lambda(TpColdLambda& type) = 0;
	virtual void on_void(TpVoid& type) = 0;
	virtual void on_optional(TpOptional& type) =0;
	virtual void on_class(TpClass& type) = 0;
	virtual void on_ref(TpRef& type) = 0;
	virtual void on_shared(TpShared& type) = 0;
	virtual void on_weak(TpWeak& type) = 0;
};

struct Action: Node {
	own<Type> type_;
	own<Type>& type();
	virtual void match(ActionMatcher& matcher);
	string get_annotation() override;
};

struct Var : Node {
	own<Type> type;
	own<dom::Name> name;
	own<Action> initializer;  // Can be null for lambda parameter. If not null, defines the local initial value or param default value and type.
	size_t lexical_depth = 0;
	bool captured = false;
	bool is_mutable = false;
	string get_annotation() override;
	DECLARE_DOM_CLASS(Var);
};

extern own<dom::Dom> cpp_dom;

struct typelist_hasher {
	size_t operator() (const vector<own<Type>>*) const;
};
struct typelist_comparer {
	bool operator() (const vector<own<Type>>*, const vector<own<Type>>*) const;
};

struct Ast: dom::DomItem {
	string absolute_path;
	own<dom::Dom> dom;
	unordered_map<const vector<own<Type>>*, own<TpLambda>, typelist_hasher, typelist_comparer> lambda_types_;
	unordered_map<const vector<own<Type>>*, own<TpFunction>, typelist_hasher, typelist_comparer> function_types_;
	unordered_map<const vector<own<Type>>*, own<TpDelegate>, typelist_hasher, typelist_comparer> delegate_types_;
	unordered_map<own<Type>, vector<own<TpOptional>>> optional_types_;  // maps base types to all levels of their optionals
	unordered_map<own<dom::Name>, own<TpClass>> classes_by_names;
	unordered_map<own<dom::Name>, weak<struct Function>> functions_by_names;
	unordered_map<own<TpClass>, own<TpRef>> refs;
	unordered_map<own<TpClass>, own<TpShared>> shareds;
	unordered_map<own<TpClass>, own<TpWeak>> weaks;
	unordered_set<pin<dom::Name>> module_names;
	unordered_map<own<dom::Name>, own<struct Function>> tests_by_names;
	Ast();

	own<Function> entry_point;
	weak<TpClass> object;
	weak<TpClass> blob;
	weak<TpClass> utf8;
	weak<TpClass> own_array;
	weak<TpClass> weak_array;
	weak<TpClass> string_cls;
	vector<own<TpClass>> classes;
	vector<own<struct Function>> functions;
	unordered_map<string, void(*)()> platform_exports;

	// Used by platform modules
	pin<Field> mk_field(pin<dom::Name> name, pin<Action> initializer);
	pin<TpClass> mk_class(pin<dom::Name> name, std::initializer_list<pin<Field>> fields = {});
	pin<ast::Function> mk_fn(pin<dom::Name> name, void(*entry_point)(), pin<Action> result_type, std::initializer_list<pin<Type>> params);

	pin<TpInt64> tp_int64();
	pin<TpDouble> tp_double();
	pin<TpVoid> tp_void();
	pin<TpFunction> tp_function(vector<own<Type>>&& params);
	pin<TpLambda> tp_lambda(vector<own<Type>>&& params);
	pin<TpDelegate> tp_delegate(vector<own<Type>>&& params);
	pin<TpOptional> tp_optional(pin<Type> wrapped);
	pin<Type> get_wrapped(pin<TpOptional> opt);
	pin<TpRef> get_ref(pin<TpClass> target);
	pin<TpShared> get_shared(pin<TpClass> target);
	pin<TpWeak> get_weak(pin<TpClass> target);
	pin<TpClass> get_class(pin<dom::Name> name); // gets or creates class
	pin<TpClass> peek_class(pin<dom::Name> name); // gets class or null
	pin<TpClass> extract_class(pin<Type> pointer); // extracts class from own, weak or pin pointer
	DECLARE_DOM_CLASS(Ast);
};

struct ConstInt64: Action {
	int64_t value = 0;
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(ConstInt64);
};

struct ConstString : Action {
	string value;
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(ConstString);
};

struct ConstDouble : Action {
	double value = 0;
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(ConstDouble);
};

struct ConstVoid : Action {
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(ConstVoid);
};

struct ConstBool : Action {  // it produces optional<void>
	bool value = false;
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(ConstBool);
};

struct Block : Action {
	vector<own<Var>> names; // locals or params
	vector<own<Action>> body;
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(Block);
};

struct MkLambda : Block {  // MkLambda locals are params 
	size_t access_depth = 0;  // most nested non-own local used by this lambda, lambda cannot be returned above this level
	size_t lexical_depth = 0;  // its nesting level
	vector<weak<Var>> captured_locals;  // its params and its nested blocks' locals that were captured by nested lambdas
	vector<weak<Var>> mutables;  // its params and its nested blocks locals that were not captured but were modified by Set actions
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(MkLambda);
};

struct Function : MkLambda {  // Cannot be in the tree of ops. Resides in Ast::functions.
	own<dom::Name> name;
	own<Action> type_expression;
	bool is_platform = false;
	bool is_test = false;
	DECLARE_DOM_CLASS(Function);
};

struct ImmediateDelegate : Function {
	own<Action> base;
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(ImmediateDelegate);
};

struct Method : Function {  // Cannot be in the tree of ops. Resides in TpClass::new_methods/overloads.
	weak<Method> ovr;  // direct method that was overridden by this one
	weak<Method> base; // first original class/interface method that was implemented by this one
	weak<TpClass> cls;  // class in which this method is declared.
	int ordinal = 0; // index int cls->new_methods.
	bool is_factory = false; // @-method
	int mut = 1;  // 1=mutable, -1=frozen(*), 0=any(-)
	DECLARE_DOM_CLASS(Method);
};

struct Call : Action {
	own<Action> callee;  // returns lambda
	vector<own<Action>> params;
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(Call);
};
struct GetAtIndex : Action {
	own<Action> indexed;
	vector<own<Action>> indexes;
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(GetAtIndex);
};
struct SetAtIndex : GetAtIndex {
	own<Action> value;
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(SetAtIndex);
};

struct MakeDelegate : Action {
	weak<Method> method;
	own<Action> base;
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(MakeDelegate);
};

struct MakeFnPtr : Action {
	weak<Function> fn;
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(MakeFnPtr);
};

struct DataRef : Action {
	weak<Var> var;
	own<dom::Name> var_name;
	string get_annotation() override;
};

struct Get : DataRef {
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(Get);
};

struct Set : DataRef {
	own<Action> val;
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(Set);
};

struct FieldRef : Action {
	own<Action> base;
	weak<Field> field;
	own<dom::Name> field_name;
	string get_annotation() override;
};

struct GetField : FieldRef {
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(GetField);
};

struct SetField : FieldRef {
	own<Action> val;
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(SetField);
};

struct SpliceField : SetField {
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(SpliceField);
};

struct MkInstance : Action {
	own<TpClass> cls;
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(MkInstance);
};

struct UnaryOp : Action {
	own<Action> p;
};
struct BinaryOp : Action {
	own<Action> p[2];
};

struct CastOp : BinaryOp {
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(CastOp);
};
struct AddOp : BinaryOp {
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(AddOp);
};
struct SubOp : BinaryOp {
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(SubOp);
};
struct MulOp : BinaryOp {
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(MulOp);
};
struct DivOp : BinaryOp {
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(DivOp);
};
struct ModOp : BinaryOp {
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(ModOp);
};
struct AndOp : BinaryOp {
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(AndOp);
};
struct OrOp : BinaryOp {
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(OrOp);
};
struct XorOp : BinaryOp {
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(XorOp);
};
struct ShlOp : BinaryOp {
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(ShlOp);
};
struct ShrOp : BinaryOp {
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(ShrOp);
};
struct EqOp : BinaryOp {
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(EqOp);
};
struct LtOp : BinaryOp {
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(LtOp);
};
struct If : BinaryOp {
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(If);
};
struct LAnd : BinaryOp {
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(LAnd);
};
struct Else : BinaryOp {
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(Else);
};
struct LOr : BinaryOp {
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(LOr);
};
struct CopyOp : UnaryOp {
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(CopyOp);
};
struct MkWeakOp : UnaryOp {
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(MkWeakOp);
};
struct DerefWeakOp : UnaryOp {
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(DerefWeakOp);
};
struct Loop : UnaryOp {
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(Loop);
};

struct ToIntOp : UnaryOp {
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(ToIntOp);
};
struct ToFloatOp : UnaryOp {
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(ToFloatOp);
};
struct NotOp : UnaryOp {
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(NotOp);
};
struct NegOp : UnaryOp {
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(NegOp);
};
struct RefOp : UnaryOp { // converts TpClass to TpRef, used only in type definitions.
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(RefOp);
};
struct FreezeOp : UnaryOp { // converts TpClass/TpRef to TpShared.
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(FreezeOp);
};

struct ActionMatcher {
	virtual void on_unmatched(Action& node);
	virtual void on_bin_op(BinaryOp& node);
	virtual void on_un_op(UnaryOp& node);

	virtual void on_const_i64(ConstInt64& node);
	virtual void on_const_string(ConstString& node);
	virtual void on_const_double(ConstDouble& node);
	virtual void on_const_void(ConstVoid& node);
	virtual void on_const_bool(ConstBool& node);
	virtual void on_get(Get& node);
	virtual void on_set(Set& node);
	virtual void on_get_field(GetField& node);
	virtual void on_set_field(SetField& node);
	virtual void on_splice_field(SpliceField& node);
	virtual void on_mk_instance(MkInstance& node);
	virtual void on_mk_lambda(MkLambda& node);
	virtual void on_call(Call& node);
	virtual void on_get_at_index(GetAtIndex& node);
	virtual void on_set_at_index(SetAtIndex& node);
	virtual void on_make_delegate(MakeDelegate& node);
	virtual void on_immediate_delegate(ImmediateDelegate& node);
	virtual void on_make_fn_ptr(MakeFnPtr& node);
	virtual void on_block(Block& node);
	virtual void on_cast(CastOp& node);
	virtual void on_add(AddOp& node);
	virtual void on_sub(SubOp& node);
	virtual void on_mul(MulOp& node);
	virtual void on_div(DivOp& node);
	virtual void on_mod(ModOp& node);
	virtual void on_and(AndOp& node);
	virtual void on_or(OrOp& node);
	virtual void on_xor(XorOp& node);
	virtual void on_shl(ShlOp& node);
	virtual void on_shr(ShrOp& node);
	virtual void on_eq(EqOp& node);
	virtual void on_lt(LtOp& node);
	virtual void on_if(If& node);
	virtual void on_land(LAnd& node);
	virtual void on_else(Else& node);
	virtual void on_lor(LOr& node);
	virtual void on_loop(Loop& node);
	virtual void on_copy(CopyOp& node);
	virtual void on_mk_weak(MkWeakOp& node);
	virtual void on_deref_weak(DerefWeakOp& node);

	virtual void on_to_int(ToIntOp& node);
	virtual void on_to_float(ToFloatOp& node);
	virtual void on_not(NotOp& node);
	virtual void on_neg(NegOp& node);
	virtual void on_ref(RefOp& node);
	virtual void on_freeze(FreezeOp& node);

	own<Action>* fix_result = nullptr;
	void fix(own<Action>& ptr);
};

struct ActionScanner : ActionMatcher {
	void on_bin_op(BinaryOp& node) override;
	void on_un_op(UnaryOp& node) override;
	void on_mk_lambda(MkLambda& node) override;
	void on_call(Call& node) override;
	void on_get_at_index(GetAtIndex& node) override;
	void on_set_at_index(SetAtIndex& node) override;
	void on_make_delegate(MakeDelegate& node) override;
	void on_immediate_delegate(ImmediateDelegate& node) override;
	void on_block(Block& node) override;
	void on_set(Set& node) override;
	void on_get_field(GetField& node) override;
	void on_set_field(SetField& node) override;
	void on_splice_field(SpliceField& node) override;
	// Function and Method are not parts of AST tree
};

template<typename T>
pin<T> make_at_location(Node& src) {
	auto r = pin<T>::make();
	r->line = src.line;
	r->pos = src.pos;
	r->module_name = src.module_name;
	return r;
}

void initialize();

} // namespace ast

namespace std {

std::ostream& operator<< (std::ostream& dst, const ast::Node& n);
inline std::ostream& operator<< (std::ostream& dst, const ltm::pin<ast::Node>& n) { return dst << *n; }

std::ostream& operator<< (std::ostream& dst, const ltm::pin<ast::Type>& t);

}

#endif // AST_H
