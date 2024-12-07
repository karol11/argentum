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

	struct Node;  // having file position
	struct Action;  // having result, able to build code
	struct Function;
	struct Class;
	struct Enum;
	struct Ast;
	struct Module;
	struct Var;
	struct Block;

	struct LongName {
		string name;
		weak<Module> module;

		bool operator==(const LongName& other) const {
			return module == other.module && name == other.name;
		}
	};

} // namespace

namespace std {
	template<>
	struct hash<ast::LongName> {
		typedef ast::LongName argument_type;
		typedef std::size_t result_type;
		std::size_t operator()(const ast::LongName& n) const {
			return std::hash<std::string>{}(n.name)* std::hash<ltm::weak<ast::Module>>{}(n.module);
		}
	};
	ostream& operator<< (ostream& dst, const ast::LongName& name);
	string to_string(const ast::LongName& name);
}

namespace ast {

template<typename... T>
string format_str(const T&... t) { return (std::stringstream() << ... << t).str(); }

enum struct Mut {
	MUTATING = 1,
	FROZEN = -1,
	ANY = 0
};

struct Module : dom::DomItem {
	weak<Ast> ast;
	string name;
	unordered_map<string, weak<Module>> direct_imports;
	unordered_map<string, weak<Node>> aliases;
	unordered_map<string, own<Var>> constants;
	unordered_map<string, own<Function>> tests;
	unordered_map<string, own<Class>> classes;
	unordered_map<string, own<Function>> functions;
	unordered_map<string, own<Enum>> enums;
	own<Function> entry_point;

	pin<Class> get_class(const string& name, int32_t line, int32_t pos); // gets or creates class
	pin<Class> peek_class(const string& name); // gets class or null
	pin<Enum> get_enum(const string& name, int32_t line, int32_t pos); // gets or creates enum
	pin<Enum> peek_enum(const string& name); // gets enum or null
	DECLARE_DOM_CLASS(Module);
};

struct Node: dom::DomItem {
	int32_t line = 0;
	int32_t pos = 0;
	weak<ast::Module> module;
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
	static own<Type>& promote(own<Type>& to_patch);  // replaces cold lambda with a resolved lambda type
	virtual void match(TypeMatcher& matcher) = 0;
};
struct TpInt32 : Type {
	void match(TypeMatcher& matcher) override;
	DECLARE_DOM_CLASS(TpInt32);
};
struct TpInt64 : Type {
	void match(TypeMatcher& matcher) override;
	DECLARE_DOM_CLASS(TpInt64);
};
struct TpFloat : Type {
	void match(TypeMatcher& matcher) override;
	DECLARE_DOM_CLASS(TpFloat);
};
struct TpDouble : Type {
	void match(TypeMatcher& matcher) override;
	DECLARE_DOM_CLASS(TpDouble);
};
struct TpFunction : Type {
	vector<own<Type>> params;  //+result
	bool can_x_break = false;  // is lambda or has lambda params, so its result has opt-wrapper
	void match(TypeMatcher& matcher) override;
	DECLARE_DOM_CLASS(TpFunction);
};
struct TpLambda : TpFunction {
	void match(TypeMatcher& matcher) override;
	DECLARE_DOM_CLASS(TpLambda);
};
struct TpColdLambda : Type {  // never called not istantiated lambdas with unknown param/result types
	own<Type> resolved;  // this can be Lambda or ColdLambda
	struct callee {
		weak<struct MkLambda> fn;
		weak<Var> outer_underscore;
	};
	vector<callee> callees;
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
struct TpNoRet : Type {
	void match(TypeMatcher& matcher) override;
	DECLARE_DOM_CLASS(TpNoRet);
};
struct TpOptional : Type {
	own<Type> wrapped;
	int depth = 0;
	void match(TypeMatcher& matcher) override;
	DECLARE_DOM_CLASS(TpOptional);
};
struct TpEnum : Type {
	weak<Enum> def;
	void match(TypeMatcher& matcher) override;
	DECLARE_DOM_CLASS(TpEnum);
};
struct EnumTag : Node {
	string name;
	int64_t val = 0;
	DECLARE_DOM_CLASS(EnumTag);
};
struct Enum : Node {
	string name;
	own<TpEnum> enum_type;
	Enum() {
		enum_type = new TpEnum();
		enum_type->def = this;
	}
	unordered_map<string, own<EnumTag>> tags;
	DECLARE_DOM_CLASS(Enum);
};
// Classes are owned by Module::classes
// ClassParams - by Class::params
// ClassInstance - by Ast::class_insts
struct AbstractClass : Node {
	// bool is_instantiated = false;
	// bool is_casted_to = false;
	virtual string get_name();
	virtual pin<Class> get_implementation() {
		error("internal error abstract class has no implementation");
	}
	enum struct InstMode {
		direct,      // this is a well-defined class that can be instantiated with no lookups
		in_context,  // class param or parameterized instance. requires `this` pointer and lookup in vmt
		off,         // parameterized class with no params provided, cannot be instantiated
	};
	virtual InstMode inst_mode() { return InstMode::off; }
	DECLARE_DOM_CLASS(AbstractClass);
};
struct ClassParam: AbstractClass {
	bool is_in = true;
	bool is_out = true;
	int index = 0; // in TpClass::params
	string name;
	weak<AbstractClass> base;
	string get_name() override;
	pin<Class> get_implementation() override {
		return base->get_implementation();
	}
	InstMode inst_mode() override { return InstMode::in_context; }
	DECLARE_DOM_CLASS(ClassParam);
};
struct Field : Node {
	string name;
	own<Action> initializer;
	int offset = 0;
	weak<Class> cls;
	DECLARE_DOM_CLASS(Field);
};

struct Class : AbstractClass {
	string name;
	bool is_interface = false;
	bool is_test = false;
	bool is_defined = false;
	bool used = false;  // instantiated, casted to, or has used descendants
	weak<AbstractClass> base_class; // Class or ClassInstance
	vector<own<ClassParam>> params;
	vector<own<Field>> fields;
	unordered_map<LongName, weak<Node>> this_names;  // memoized this names - defined and inhrited. ambiguous and absent - stored as null nodes.
	vector<own<struct Method>> new_methods;  // new methods ordered by source order
	unordered_map<weak<AbstractClass>, vector<own<struct Method>>> overloads;  // overloads for interfaces and the base class, ordered by source order
	unordered_map<weak<Class>, weak<struct ClassInstance>> base_contexts;  // all generic base interfaces and classes with instantiation parameters key == val.params[0].
	unordered_map<
		weak<Class>,                                   // base interface with stripped parameters, used only in codegen phase
		vector<weak<struct Method>>> interface_vmts;   // inherited and overloaded methods in the order of new_methods in that interface
	string get_name() override;

	template<typename F, typename M, typename A>
	bool handle_member(Node& node, const LongName& name, F on_field, M on_method, A on_anbiguous) {
		if (auto m = dom::peek(this_names, name)) {
			if (!m)
				on_anbiguous();
			else if (auto as_field = dom::strict_cast<ast::Field>(m))
				on_field(as_field);
			else if (auto as_method = dom::strict_cast<ast::Method>(m))
				on_method(as_method);
			else
				node.error("internal error, class member nor field nor method");
			return true;
		}
		return false;
	}
	pin<Class> get_implementation() override {
		return this;
	}
	InstMode inst_mode() override { return params.empty() ? InstMode::direct : InstMode::off; }
	DECLARE_DOM_CLASS(Class);
};

struct ClassInstance : AbstractClass {
	vector<weak<AbstractClass>> params;  // parameterized class + parameters
	InstMode inst_cache = InstMode::off;
	string get_name() override;
	pin<Class> get_implementation() override {
		return params[0]->get_implementation();
	}
	InstMode inst_mode() override {
		if (inst_cache != InstMode::off)
			return inst_cache;
		for (auto& p : params) {
			if (p->inst_mode() == InstMode::in_context)
				return inst_cache = InstMode::in_context;
		}
		return inst_cache = InstMode::direct;
	}
	DECLARE_DOM_CLASS(ClassInstance);
};

struct TpOwn : Type {
	weak<AbstractClass> target;
	void match(TypeMatcher& matcher) override;
	DECLARE_DOM_CLASS(TpOwn);
};
struct TpRef : TpOwn {
	void match(TypeMatcher& matcher) override;
	DECLARE_DOM_CLASS(TpRef);
};
struct TpShared : TpOwn {
	void match(TypeMatcher& matcher) override;
	DECLARE_DOM_CLASS(TpShared);
};
struct TpWeak : TpOwn {
	void match(TypeMatcher& matcher) override;
	DECLARE_DOM_CLASS(TpWeak);
};
struct TpFrozenWeak : TpOwn {
	void match(TypeMatcher& matcher) override;
	DECLARE_DOM_CLASS(TpFrozenWeak);
};
struct TpConformRef : TpOwn {
	void match(TypeMatcher& matcher) override;
	DECLARE_DOM_CLASS(TpConformRef);
};
struct TpConformWeak : TpOwn {
	void match(TypeMatcher& matcher) override;
	DECLARE_DOM_CLASS(TpConformWeak);
};

struct TypeMatcher {
	virtual ~TypeMatcher() = default;
	virtual void on_int32(TpInt32& type) = 0;
	virtual void on_int64(TpInt64& type) = 0;
	virtual void on_float(TpFloat& type) = 0;
	virtual void on_double(TpDouble& type) = 0;
	virtual void on_function(TpFunction& type) = 0;
	virtual void on_lambda(TpLambda& type) = 0;
	virtual void on_delegate(TpDelegate& type) = 0;
	virtual void on_cold_lambda(TpColdLambda& type) = 0;
	virtual void on_void(TpVoid& type) = 0;
	virtual void on_no_ret(TpNoRet& type) = 0;
	virtual void on_optional(TpOptional& type) =0;
	virtual void on_own(TpOwn& type) = 0;
	virtual void on_ref(TpRef& type) = 0;
	virtual void on_shared(TpShared& type) = 0;
	virtual void on_weak(TpWeak& type) = 0;
	virtual void on_frozen_weak(TpFrozenWeak& type) = 0;
	virtual void on_conform_ref(TpConformRef& type) = 0;
	virtual void on_conform_weak(TpConformWeak& type) = 0;
	virtual void on_enum(TpEnum& type) = 0;
};

struct Action: Node {
	own<Type> type_;
	own<Type>& type();
	virtual void match(ActionMatcher& matcher);
	string get_annotation() override;
};

struct Var : Node {
	own<Type> type;
	string name;
	own<Action> initializer;  // Can be null for lambda parameter. If not null, defines the local initial value or param default value and type.
	size_t lexical_depth = 0; // lexical depth at which var is declared
	bool captured = false;
	bool is_mutable = false;
	bool is_const = false;
	string get_annotation() override;
	DECLARE_DOM_CLASS(Var);
};

extern own<dom::Dom> cpp_dom;

template<typename T>
struct ptr_vec_hasher {
	size_t operator() (const vector<T>* v) const {
		size_t r = 0;
		for (const auto& p : *v)
			r += std::hash<void*>()(p);
		return r;
	}
};

template<typename T>
struct ptr_vec_comparer {
	bool operator() (const vector<T>* a, const vector<T>* b) const {
		return *a == *b;
	}
};

struct Ast: dom::DomItem {
	string absolute_path;
	own<dom::Dom> dom;
	unordered_map<
		const vector<weak<AbstractClass>>*,
		own<ClassInstance>,
		ptr_vec_hasher<weak<AbstractClass>>,
		ptr_vec_comparer<weak<AbstractClass>>> class_instances_;
	unordered_map<
		const vector<own<Type>>*,
		own<TpLambda>,
		ptr_vec_hasher<own<Type>>,
		ptr_vec_comparer<own<Type>>> lambda_types_;
	unordered_map<
		const vector<own<Type>>*,
		own<TpFunction>,
		ptr_vec_hasher<own<Type>>,
		ptr_vec_comparer<own<Type>>> function_types_;
	unordered_map<
		const vector<own<Type>>*,
		own<TpDelegate>,
		ptr_vec_hasher<own<Type>>,
		ptr_vec_comparer<own<Type>>> delegate_types_;
	unordered_map<own<Type>, vector<own<TpOptional>>> optional_types_;  // maps base types to all levels of their optionals
	unordered_map<weak<AbstractClass>, own<TpOwn>> owns;
	unordered_map<weak<AbstractClass>, own<TpRef>> refs;
	unordered_map<weak<AbstractClass>, own<TpShared>> shareds;
	unordered_map<weak<AbstractClass>, own<TpWeak>> weaks;
	unordered_map<weak<AbstractClass>, own<TpFrozenWeak>> frozen_weaks;
	unordered_map<weak<AbstractClass>, own<TpConformRef>> conform_refs;
	unordered_map<weak<AbstractClass>, own<TpConformWeak>> conform_weaks;
	unordered_map<string, void(*)()> platform_exports;  // used only in JIT
	weak<Class> object;
	weak<Class> blob;
	weak<Class> str_builder;
	weak<Class> own_array;
	weak<Class> weak_array;
	weak<Class> string_cls;
	weak<Module> sys;
	vector<weak<Class>> classes_in_order; // all classes from all modules in the base-first and class-before-class-instance order

	unordered_map<string, own<Module>> modules;
	vector<weak<Module>> modules_in_order; // imports of module M placed before M
	weak<Module> starting_module;

	Ast();

	// Used by platform modules, always populate sys module
	pin<Field> mk_field(string name, pin<Action> initializer);
	pin<Class> mk_class(string name, std::initializer_list<pin<Field>> fields = {});
	pin<Function> mk_fn(string name, void(*entry_point)(), pin<Action> result_type, std::initializer_list<pin<Type>> params);
	pin<Method> mk_method(Mut mut, pin<Class> cls, string m_name, void(*entry_point)(), pin<Action> result_type, std::initializer_list<pin<Type>> params);
	pin<Method> mk_overload(pin<Class> cls, void(*entry_point)(), pin<Method> overloaded);
	void add_this_param(ast::Function& fn, pin<ast::Class> cls);

	pin<TpInt64> tp_int64();
	pin<TpInt32> tp_int32();
	pin<TpDouble> tp_double();
	pin<TpFloat> tp_float();
	pin<TpVoid> tp_void();
	pin<TpNoRet> tp_no_ret();
	pin<TpFunction> tp_function(vector<own<Type>>&& params);
	pin<TpLambda> tp_lambda(vector<own<Type>>&& params);
	pin<TpDelegate> tp_delegate(vector<own<Type>>&& params);
	pin<TpOptional> tp_optional(pin<Type> wrapped);
	pin<Type> get_wrapped(pin<TpOptional> opt);
	pin<TpOwn> get_own(pin<AbstractClass> target);
	pin<TpRef> get_ref(pin<AbstractClass> target);
	pin<TpShared> get_shared(pin<AbstractClass> target);
	pin<TpWeak> get_weak(pin<AbstractClass> target);
	pin<TpFrozenWeak> get_frozen_weak(pin<AbstractClass> target);
	pin<TpConformRef> get_conform_ref(pin<AbstractClass> target);
	pin<TpConformWeak> get_conform_weak(pin<AbstractClass> target);
	pin<AbstractClass> extract_class(pin<Type> pointer); // extracts class from pointer types
	pin<Type> convert_maybe_optional(pin<Type> src, std::function<pin<Type>(pin<Type>)> converter);  // for non-opt type calls converter, for opt - converts inner type and repack to same level opt
	pin<ClassInstance> get_class_instance(vector<weak<AbstractClass>>&& params);

	// Takes a parameterized class and its instantiation context and returns a class with all substituted parameters from context.
	pin<ast::AbstractClass> resolve_params(pin<ast::AbstractClass> cls, pin<ast::ClassInstance> context);

	DECLARE_DOM_CLASS(Ast);
};

struct ConstInt32: Action {
	int32_t value = 0;
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(ConstInt32);
};
struct ConstInt64: Action {
	int64_t value = 0;
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(ConstInt64);
};
struct ConstEnumTag : Action {
	weak<EnumTag> value;
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(ConstEnumTag);
};

struct ConstString : Action {
	string value;
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(ConstString);
};

struct ConstFloat : Action {
	float value = 0;
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(ConstFloat);
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

struct Break : Action {
	weak<Block> block;
	own<Action> result;
	string block_name;
	size_t lexical_depth = 0;  // nesting level of fn they resides in
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(Break);
};

struct Block : Action {
	size_t lexical_depth = 0; // nesting level of its lambda
	string break_name;
	vector<own<Var>> names; // locals or params
	vector<own<Action>> body;
	unordered_set<weak<Break>> breaks;
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(Block);
};

struct MkLambda : Block {  // MkLambda locals are params 
	size_t lexical_depth = 0;  // its nesting level
	vector<weak<Var>> captured_locals;  // its params and its nested blocks' locals that were captured by nested lambdas
	vector<weak<Var>> mutables;  // its params and its nested blocks locals that were not captured but were modified by Set actions
	unordered_set<weak<Break>> x_breaks; // all its breaks, and breaks of called lambdas, that go outside of this lambda
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(MkLambda);
};

struct Function : MkLambda {  // Cannot be in the tree of ops. Resides in Ast::functions.
	string name;
	own<Action> type_expression;
	bool is_platform = false;
	bool is_test = false;
	bool used = false;  // there is a get(Function), of for mk_delegate(method) used is stored in method->base
	unordered_map<weak<EnumTag>, own<Block>> enum_dispatch;
	DECLARE_DOM_CLASS(Function);
};

struct ImmediateDelegate : Function {
	own<Action> base;
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(ImmediateDelegate);
};

struct Method : Function {  // Cannot be in the tree of ops. Resides in Class::new_methods/overloads.
	weak<Method> ovr;  // direct method that was overridden by this one
	weak<Method> base; // first original class/interface method that was implemented by this one
	weak<Class> cls;  // class in which this method is declared.
	weak<Module> base_module; // along with `name` defines the name of the method as it is declared, null if name is short
	int ordinal = 0; // index int cls->new_methods.
	bool is_factory = false; // @-method, its ret val is void
	Mut mut = Mut::MUTATING;
	DECLARE_DOM_CLASS(Method);
};

struct Call : Action {
	own<Action> callee;  // returns lambda
	vector<own<Action>> params;
	unordered_set<weak<ast::MkLambda>> activates_lambdas;  // null entry means dep. on fn. param
	bool returns_last_param_if_void = false;  // used in arr[i]:=v
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(Call);
};
struct AsyncCall : Call {
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(AsyncCall);
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
	string var_name;
	weak<Module> var_module;  // for classes, fields
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
	string field_name;
	weak<Module> field_module;
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
	weak<AbstractClass> cls;  // Null indicates thistype in immediate delegates
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(MkInstance);
};

struct UnaryOp : Action {
	own<Action> p;

	pin<ast::Action> fill(pin<ast::Action> param) {
		p = param;
		return this;
	}
};
struct BinaryOp : Action {
	own<Action> p[2];

	pin<ast::Action> fill(pin<ast::Action> param1, pin<ast::Action> param2) {
		p[0] = param1;
		p[1] = param2;
		return this;
	}
};

struct CastOp : BinaryOp { // p[0] can be null to indicate no-op conversion of p[1] to this.type()
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
	bool has_breaks = false;
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(Loop);
};

struct ToInt32Op : UnaryOp {
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(ToInt32Op);
};
struct ToIntOp : UnaryOp {
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(ToIntOp);
};
struct ToFloatOp : UnaryOp {
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(ToFloatOp);
};
struct ToDoubleOp : UnaryOp {
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(ToDoubleOp);
};
struct ToStrOp : BinaryOp {
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(ToStrOp);
};
struct NotOp : UnaryOp {
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(NotOp);
};
struct InvOp : UnaryOp {
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(InvOp);
};
struct NegOp : UnaryOp {
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(NegOp);
};
struct RefOp : UnaryOp { // converts TpClass to TpRef, used only in type definitions.
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(RefOp);
};
struct ConformOp : UnaryOp { // converts TpClass to TpConformRef, used only in type definitions.
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(ConformOp);
};
struct FreezeOp : UnaryOp { // converts TpClass/TpRef to TpShared.
	void match(ActionMatcher& matcher) override;
	DECLARE_DOM_CLASS(FreezeOp);
};

struct ActionMatcher {
	virtual void on_unmatched(Action& node);
	virtual void on_bin_op(BinaryOp& node);
	virtual void on_un_op(UnaryOp& node);

	virtual void on_const_enum_tag(ConstEnumTag& node);
	virtual void on_const_i32(ConstInt32& node);
	virtual void on_const_i64(ConstInt64& node);
	virtual void on_const_string(ConstString& node);
	virtual void on_const_float(ConstFloat& node);
	virtual void on_const_double(ConstDouble& node);
	virtual void on_const_void(ConstVoid& node);
	virtual void on_const_bool(ConstBool& node);
	virtual void on_break(Break& node);
	virtual void on_get(Get& node);
	virtual void on_set(Set& node);
	virtual void on_get_field(GetField& node);
	virtual void on_set_field(SetField& node);
	virtual void on_splice_field(SpliceField& node);
	virtual void on_mk_instance(MkInstance& node);
	virtual void on_mk_lambda(MkLambda& node);
	virtual void on_call(Call& node);
	virtual void on_async_call(AsyncCall& node);
	virtual void on_get_at_index(GetAtIndex& node);
	virtual void on_set_at_index(SetAtIndex& node);
	virtual void on_make_delegate(MakeDelegate& node);
	virtual void on_immediate_delegate(ImmediateDelegate& node);
	virtual void on_make_fn_ptr(MakeFnPtr& node);
	virtual void on_block(Block& node);
	virtual void on_cast(CastOp& node);
	virtual void on_to_str(ToStrOp& node);
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

	virtual void on_to_int32(ToInt32Op& node);
	virtual void on_to_int(ToIntOp& node);
	virtual void on_to_float(ToFloatOp& node);
	virtual void on_to_double(ToDoubleOp& node);
	virtual void on_not(NotOp& node);
	virtual void on_neg(NegOp& node);
	virtual void on_inv(InvOp& node);
	virtual void on_ref(RefOp& node);
	virtual void on_conform(ConformOp& node);
	virtual void on_freeze(FreezeOp& node);

	own<Action>* fix_result = nullptr;
	void fix(own<Action>& ptr);
};

struct ActionScanner : ActionMatcher {
	void on_bin_op(BinaryOp& node) override;
	void on_un_op(UnaryOp& node) override;
	void on_mk_lambda(MkLambda& node) override;
	void on_break(Break& node) override;
	void on_call(Call& node) override;
	void on_async_call(AsyncCall& node) override;
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
	r->module = src.module;
	return r;
}

void initialize();

} // namespace ast

namespace std {

std::ostream& operator<< (std::ostream& dst, const ast::Node& n);
inline std::ostream& operator<< (std::ostream& dst, const ltm::pin<ast::Node>& n) { return dst << *n; }

std::ostream& operator<< (std::ostream& dst, const ltm::pin<ast::Type>& t);

}

[[noreturn]] void panic();

#endif // AST_H
