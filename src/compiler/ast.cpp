#include <cassert>
#include <sstream>

#include "utils/register_runtime.h"
#include "dom/dom-to-string.h"
#include "compiler/ast.h"

namespace ast {

using dom::TypeInfo;
using dom::CppClassType;
using dom::TypeWithFills;
using dom::CField;
using dom::Kind;

own<dom::Dom> cpp_dom;
own<TypeWithFills> Ast::dom_type_;
own<TypeWithFills> Var::dom_type_;
own<TypeWithFills> Module::dom_type_;

own<TypeWithFills> ConstInt64::dom_type_;
own<TypeWithFills> ConstString::dom_type_;
own<TypeWithFills> ConstDouble::dom_type_;
own<TypeWithFills> ConstVoid::dom_type_;
own<TypeWithFills> ConstBool::dom_type_;
own<TypeWithFills> MkLambda::dom_type_;
own<TypeWithFills> Call::dom_type_;
own<TypeWithFills> GetAtIndex::dom_type_;
own<TypeWithFills> SetAtIndex::dom_type_;
own<TypeWithFills> MakeDelegate::dom_type_;
own<TypeWithFills> MakeFnPtr::dom_type_;
own<TypeWithFills> Get::dom_type_;
own<TypeWithFills> Set::dom_type_;
own<TypeWithFills> GetField::dom_type_;
own<TypeWithFills> SetField::dom_type_;
own<TypeWithFills> SpliceField::dom_type_;
own<TypeWithFills> MkInstance::dom_type_;

own<TypeWithFills> ToIntOp::dom_type_;
own<TypeWithFills> ToFloatOp::dom_type_;
own<TypeWithFills> NotOp::dom_type_;
own<TypeWithFills> NegOp::dom_type_;
own<TypeWithFills> RefOp::dom_type_;
own<TypeWithFills> FreezeOp::dom_type_;
own<TypeWithFills> Block::dom_type_;
own<TypeWithFills> CastOp::dom_type_;
own<TypeWithFills> AddOp::dom_type_;
own<TypeWithFills> SubOp::dom_type_;
own<TypeWithFills> MulOp::dom_type_;
own<TypeWithFills> DivOp::dom_type_;
own<TypeWithFills> ModOp::dom_type_;
own<TypeWithFills> AndOp::dom_type_;
own<TypeWithFills> OrOp::dom_type_;
own<TypeWithFills> XorOp::dom_type_;
own<TypeWithFills> ShlOp::dom_type_;
own<TypeWithFills> ShrOp::dom_type_;
own<TypeWithFills> EqOp::dom_type_;
own<TypeWithFills> LtOp::dom_type_;
own<TypeWithFills> If::dom_type_;
own<TypeWithFills> LAnd::dom_type_;
own<TypeWithFills> Else::dom_type_;
own<TypeWithFills> LOr::dom_type_;
own<TypeWithFills> Loop::dom_type_;
own<TypeWithFills> CopyOp::dom_type_;
own<TypeWithFills> MkWeakOp::dom_type_;
own<TypeWithFills> DerefWeakOp::dom_type_;

own<TypeWithFills> TpInt64::dom_type_;
own<TypeWithFills> TpDouble::dom_type_;
own<TypeWithFills> TpFunction::dom_type_;
own<TypeWithFills> TpLambda::dom_type_;
own<TypeWithFills> TpColdLambda::dom_type_;
own<TypeWithFills> TpDelegate::dom_type_;
own<TypeWithFills> TpVoid::dom_type_;
own<TypeWithFills> TpOptional::dom_type_;
own<TypeWithFills> TpClass::dom_type_;
own<TypeWithFills> TpRef::dom_type_;
own<TypeWithFills> TpShared::dom_type_;
own<TypeWithFills> TpWeak::dom_type_;
own<TypeWithFills> Field::dom_type_;
own<TypeWithFills> Method::dom_type_;
own<TypeWithFills> Function::dom_type_;
own<TypeWithFills> ImmediateDelegate::dom_type_;

namespace {
	template<typename CLS>
	void make_bin_op(const char* name, const pin<TypeInfo>& op_array_2) {
		CLS::dom_type_ = (new CppClassType<CLS>(cpp_dom, { "m0", name }))
			->field("p", pin<CField<&BinaryOp::p>>::make(op_array_2));
	}
}

void initialize() {
	if (cpp_dom)
		return;
	cpp_dom = new dom::Dom;
	auto weak_type = cpp_dom->mk_type(Kind::WEAK);
	auto own_type = cpp_dom->mk_type(Kind::OWN);
	auto size_t_type = cpp_dom->mk_type(Kind::UINT, sizeof(size_t));
	pin<dom::TypeInfo> own_vector_type = new dom::VectorType<own<dom::DomItem>>(own_type);
	pin<dom::TypeInfo> string_type = cpp_dom->mk_type(Kind::STRING);
	pin<dom::TypeInfo> str_own_map_type = new dom::UnorderedMapType<string, own<dom::DomItem>>(string_type, own_type);
	pin<dom::TypeInfo> str_weak_map_type = new dom::UnorderedMapType<string, weak<dom::DomItem>>(string_type, weak_type);
	pin<dom::TypeInfo> weak_set_type = new dom::UnorderedSetType<weak<dom::DomItem>>(weak_type);
	auto op_array_2 = cpp_dom->mk_type(Kind::FIX_ARRAY, 2, own_type);
	Ast::dom_type_ = (new CppClassType<Ast>(cpp_dom, { "m0", "Ast" }))
		->field("src_path", pin<CField<&Ast::absolute_path>>::make(string_type))
		->field("modules", pin<CField<&Ast::modules>>::make(str_own_map_type))
		->field("starting", pin<CField<&Ast::starting_module>>::make(weak_type));
	Module::dom_type_ = (new CppClassType<Module>(cpp_dom, { "m0", "Module" }))
		->field("imports", pin<CField<&Module::direct_imports>>::make(str_weak_map_type))
		->field("aliases", pin<CField<&Module::aliases>>::make(str_weak_map_type))
		->field("tests", pin<CField<&Module::tests>>::make(str_own_map_type))
		->field("classes", pin<CField<&Module::classes>>::make(str_own_map_type))
		->field("functions", pin<CField<&Module::functions>>::make(str_own_map_type))
		->field("entry", pin<CField<&Module::entry_point>>::make(own_type));
	Var::dom_type_ = (new CppClassType<Var>(cpp_dom, { "m0", "Var" }))
		->field("name", pin<CField<&Var::name>>::make(string_type))
		->field("initializer", pin<CField<&Var::initializer>>::make(own_type));
	ConstInt64::dom_type_ = (new CppClassType<ConstInt64>(cpp_dom, {"m0", "Int"}))
		->field("value", pin<CField<&ConstInt64::value>>::make(
			cpp_dom->mk_type(Kind::INT, sizeof(int64_t))));
	ConstString::dom_type_ = (new CppClassType<ConstString>(cpp_dom, { "m0", "Str" }))
		->field("value", pin<CField<&ConstString::value>>::make(
			string_type));
	ConstDouble::dom_type_ = (new CppClassType<ConstDouble>(cpp_dom, { "m0", "Double" }))
		->field("value", pin<CField<&ConstDouble::value>>::make(
			cpp_dom->mk_type(Kind::FLOAT, sizeof(double))));
	ConstVoid::dom_type_ = (new CppClassType<ConstVoid>(cpp_dom, { "m0", "VoidVal" }));
	ConstBool::dom_type_ = (new CppClassType<ConstBool>(cpp_dom, { "m0", "BoolVal" }))
		->field("val", pin<CField<&ConstBool::value>>::make(cpp_dom->mk_type(Kind::BOOL)));
	Get::dom_type_ = (new CppClassType<Get>(cpp_dom, { "m0", "Get" }))
		->field("var", pin<CField<&Get::var>>::make(weak_type));
	Set::dom_type_ = (new CppClassType<Set>(cpp_dom, { "m0", "Set" }))
		->field("var", pin<CField<&Get::var>>::make(weak_type))
		->field("val", pin<CField<&Set::val>>::make(own_type));
	MkInstance::dom_type_ = (new CppClassType<MkInstance>(cpp_dom, { "m0", "MkInstance" }))
		->field("class", pin<CField<&MkInstance::cls>>::make(weak_type));
	GetField::dom_type_ = (new CppClassType<GetField>(cpp_dom, { "m0", "GetField" }))
		->field("field", pin<CField<&FieldRef::field>>::make(weak_type))
		->field("base", pin<CField<&FieldRef::base>>::make(own_type));
	SetField::dom_type_ = (new CppClassType<SetField>(cpp_dom, { "m0", "SetField" }))
		->field("field", pin<CField<&FieldRef::field>>::make(weak_type))
		->field("base", pin<CField<&FieldRef::base>>::make(own_type))
		->field("val", pin<CField<&SetField::val>>::make(own_type));
	SpliceField::dom_type_ = (new CppClassType<SpliceField>(cpp_dom, { "m0", "SliceField" }))
		->field("field", pin<CField<&FieldRef::field>>::make(weak_type))
		->field("base", pin<CField<&FieldRef::base>>::make(own_type))
		->field("val", pin<CField<&SetField::val>>::make(own_type));
	ToIntOp::dom_type_ = (new CppClassType<ToIntOp>(cpp_dom, { "m0", "ToInt" }))
		->field("p", pin<CField<&UnaryOp::p>>::make(own_type));
	ToFloatOp::dom_type_ = (new CppClassType<ToFloatOp>(cpp_dom, { "m0", "ToFloat" }))
		->field("p", pin<CField<&UnaryOp::p>>::make(own_type));
	NotOp::dom_type_ = (new CppClassType<NotOp>(cpp_dom, { "m0", "Not" }))
		->field("p", pin<CField<&UnaryOp::p>>::make(own_type));
	NegOp::dom_type_ = (new CppClassType<NegOp>(cpp_dom, { "m0", "Neg" }))
		->field("p", pin<CField<&UnaryOp::p>>::make(own_type));
	RefOp::dom_type_ = (new CppClassType<RefOp>(cpp_dom, { "m0", "Ref" }))
		->field("p", pin<CField<&UnaryOp::p>>::make(own_type));
	FreezeOp::dom_type_ = (new CppClassType<FreezeOp>(cpp_dom, { "m0", "Freeze" }))
		->field("p", pin<CField<&UnaryOp::p>>::make(own_type));
	Loop::dom_type_ = (new CppClassType<Loop>(cpp_dom, { "m0", "Loop" }))
		->field("p", pin<CField<&UnaryOp::p>>::make(own_type));
	CopyOp::dom_type_ = (new CppClassType<CopyOp>(cpp_dom, { "m0", "Copy" }))
		->field("p", pin<CField<&UnaryOp::p>>::make(own_type));
	MkWeakOp::dom_type_ = (new CppClassType<MkWeakOp>(cpp_dom, { "m0", "MkWeak" }))
		->field("p", pin<CField<&UnaryOp::p>>::make(own_type));
	DerefWeakOp::dom_type_ = (new CppClassType<DerefWeakOp>(cpp_dom, { "m0", "DerefWeak" }))
		->field("p", pin<CField<&UnaryOp::p>>::make(own_type));
	Block::dom_type_ = (new CppClassType<Block>(cpp_dom, { "m0", "Block" }))
		->field("body", pin<CField<&Block::body>>::make(own_vector_type))
		->field("locals", pin<CField<&Block::names>>::make(own_vector_type));
	MkLambda::dom_type_ = (new CppClassType<MkLambda>(cpp_dom, { "m0", "MkLambda" }))
		->field("body", pin<CField<&Block::body>>::make(own_vector_type))
		->field("params", pin<CField<&Block::names>>::make(own_vector_type));
	Function::dom_type_ = (new CppClassType<Function>(cpp_dom, { "m0", "Function" }))
		->field("name", pin<CField<&Function::name>>::make(string_type))
		->field("is_external", pin<CField<&Function::is_platform>>::make(cpp_dom->mk_type(Kind::BOOL)))
		->field("is_test", pin<CField<&Function::is_platform>>::make(cpp_dom->mk_type(Kind::BOOL)))
		->field("body", pin<CField<&Block::body>>::make(own_vector_type))
		->field("params", pin<CField<&Block::names>>::make(own_vector_type));
	ImmediateDelegate::dom_type_ = (new CppClassType<ImmediateDelegate>(cpp_dom, { "m0", "ImmediateDelegate" }))
		->field("name", pin<CField<&Function::name>>::make(string_type))
		->field("body", pin<CField<&Block::body>>::make(own_vector_type))
		->field("params", pin<CField<&Block::names>>::make(own_vector_type))
		->field("base", pin<CField<&ImmediateDelegate::base>>::make(own_type));
	Call::dom_type_ = (new CppClassType<Call>(cpp_dom, { "m0", "Call" }))
		->field("callee", pin<CField<&Call::callee>>::make(own_type))
		->field("params", pin<CField<&Call::params>>::make(own_vector_type));
	GetAtIndex::dom_type_ = (new CppClassType<GetAtIndex>(cpp_dom, { "m0", "GetAtIndex" }))
		->field("indexed", pin<CField<&GetAtIndex::indexed>>::make(own_type))
		->field("indexes", pin<CField<&GetAtIndex::indexes>>::make(own_vector_type));
	SetAtIndex::dom_type_ = (new CppClassType<SetAtIndex>(cpp_dom, { "m0", "SetAtIndex" }))
		->field("indexed", pin<CField<&GetAtIndex::indexed>>::make(own_type))
		->field("indexes", pin<CField<&GetAtIndex::indexes>>::make(own_vector_type))
		->field("value", pin<CField<&SetAtIndex::value>>::make(own_type));
	MakeDelegate::dom_type_ = (new CppClassType<MakeDelegate>(cpp_dom, { "m0", "MkDelegate" }))
		->field("method", pin<CField<&MakeDelegate::method>>::make(weak_type))
		->field("base", pin<CField<&MakeDelegate::base>>::make(own_type));
	MakeFnPtr::dom_type_ = (new CppClassType<MakeFnPtr>(cpp_dom, { "m0", "MkFnPtr" }))
		->field("fn", pin<CField<&MakeFnPtr::fn>>::make(weak_type));
	make_bin_op<CastOp>("Cast", op_array_2);
	make_bin_op<AddOp>("Add", op_array_2);
	make_bin_op<SubOp>("Sub", op_array_2);
	make_bin_op<MulOp>("Mul", op_array_2);
	make_bin_op<DivOp>("Div", op_array_2);
	make_bin_op<ModOp>("Mod", op_array_2);
	make_bin_op<AndOp>("And", op_array_2);
	make_bin_op<OrOp>("Or", op_array_2);
	make_bin_op<XorOp>("Xor", op_array_2);
	make_bin_op<ShlOp>("Shl", op_array_2);
	make_bin_op<ShrOp>("Shr", op_array_2);
	make_bin_op<EqOp>("Eq", op_array_2);
	make_bin_op<LtOp>("Lt", op_array_2);
	make_bin_op<If>("If", op_array_2);
	make_bin_op<LAnd>("LAnd", op_array_2);
	make_bin_op<Else>("Else", op_array_2);
	make_bin_op<LOr>("LOr", op_array_2);
	TpInt64::dom_type_ = new CppClassType<TpInt64>(cpp_dom, {"m0", "Type", "Int64"});
	TpDouble::dom_type_ = new CppClassType<TpDouble>(cpp_dom, { "m0", "Type", "Double" });
	TpFunction::dom_type_ = (new CppClassType<TpFunction>(cpp_dom, { "m0", "Type", "Function" }))
		->field("params", pin<CField<&TpFunction::params>>::make(own_vector_type));
	TpLambda::dom_type_ = (new CppClassType<TpLambda>(cpp_dom, { "m0", "Type", "Lambda" }))
		->field("params", pin<CField<&TpFunction::params>>::make(own_vector_type));
	TpDelegate::dom_type_ = (new CppClassType<TpDelegate>(cpp_dom, { "m0", "Type", "Delegate" }))
		->field("params", pin<CField<&TpFunction::params>>::make(own_vector_type));
	TpColdLambda::dom_type_ = (new CppClassType<TpColdLambda>(cpp_dom, { "m0", "Type", "ColdLambda" }))
		->field("resolved", pin<CField<&TpColdLambda::resolved>>::make(own_type));
	TpVoid::dom_type_ = (new CppClassType<TpVoid>(cpp_dom, { "m0", "Type", "Void" }));
	TpOptional::dom_type_ = (new CppClassType<TpOptional>(cpp_dom, { "m0", "Type", "Optional" }))
		->field("wrapped", pin<CField<&TpOptional::wrapped>>::make(own_type));
	Field::dom_type_ = (new CppClassType<Field>(cpp_dom, { "m0", "Field" }))
		->field("name", pin<CField<&Field::name>>::make(string_type))
		->field("type", pin<CField<&Field::initializer>>::make(own_type));
	Method::dom_type_ = (new CppClassType<Method>(cpp_dom, { "m0", "Method" }))
		->field("name", pin<CField<&Function::name>>::make(string_type))
		->field("body", pin<CField<&Block::body>>::make(own_vector_type))
		->field("result_type", pin<CField<&Function::type_expression>>::make(own_type))
		->field("is_factory", pin<CField<&Method::is_factory>>::make(cpp_dom->mk_type(Kind::BOOL)))
		->field("mut", pin<CField<&Method::mut>>::make(cpp_dom->mk_type(Kind::INT)))
		->field("params", pin<CField<&Block::names>>::make(own_vector_type));
	TpClass::dom_type_ = (new CppClassType<TpClass>(cpp_dom, { "m0", "Type", "Class" }))
		->field("name", pin<CField<&TpClass::name>>::make(string_type))
		->field("is_interface", pin<CField<&TpClass::is_interface>>::make(cpp_dom->mk_type(Kind::BOOL)))
		->field("is_test", pin<CField<&TpClass::is_test>>::make(cpp_dom->mk_type(Kind::BOOL)))
		->field("base", pin<CField<&TpClass::base_class>>::make(weak_type))
		->field("fields", pin<CField<&TpClass::fields>>::make(own_vector_type))
		->field("methods", pin<CField<&TpClass::new_methods>>::make(own_vector_type));
	TpRef::dom_type_ = (new CppClassType<TpRef>(cpp_dom, { "m0", "Type", "Ref" }))
		->field("target", pin<CField<&TpRef::target>>::make(own_type));
	TpShared::dom_type_ = (new CppClassType<TpShared>(cpp_dom, { "m0", "Type", "Shared" }))
		->field("target", pin<CField<&TpRef::target>>::make(own_type));
	TpWeak::dom_type_ = (new CppClassType<TpWeak>(cpp_dom, { "m0", "Type", "Weak" }))
		->field("target", pin<CField<&TpRef::target>>::make(own_type));
}

own<Type>& Type::promote(own<Type>& to_patch) {
	for (;;) {
		auto as_cold = dom::strict_cast<TpColdLambda>(to_patch);
		if (!as_cold || !as_cold->resolved)
			return to_patch;
		to_patch = as_cold->resolved;
	}
}
own<Type>& Action::type() {
	return Type::promote(type_);
}

void Action::match(ActionMatcher& matcher) { matcher.on_unmatched(*this); };
void ConstInt64::match(ActionMatcher& matcher) { matcher.on_const_i64(*this); }
void ConstString::match(ActionMatcher& matcher) { matcher.on_const_string(*this); }
void ConstDouble::match(ActionMatcher& matcher) { matcher.on_const_double(*this); }
void ConstVoid::match(ActionMatcher& matcher) { matcher.on_const_void(*this); }
void ConstBool::match(ActionMatcher& matcher) { matcher.on_const_bool(*this); }
void Get::match(ActionMatcher& matcher) { matcher.on_get(*this); }
void Set::match(ActionMatcher& matcher) { matcher.on_set(*this); }
void GetField::match(ActionMatcher& matcher) { matcher.on_get_field(*this); }
void SetField::match(ActionMatcher& matcher) { matcher.on_set_field(*this); }
void SpliceField::match(ActionMatcher& matcher) { matcher.on_splice_field(*this); }
void MkInstance::match(ActionMatcher& matcher) { matcher.on_mk_instance(*this); }
void MkLambda::match(ActionMatcher& matcher) { matcher.on_mk_lambda(*this); }
void Call::match(ActionMatcher& matcher) { matcher.on_call(*this); }
void GetAtIndex::match(ActionMatcher& matcher) { matcher.on_get_at_index(*this); }
void SetAtIndex::match(ActionMatcher& matcher) { matcher.on_set_at_index(*this); }
void MakeDelegate::match(ActionMatcher& matcher) { matcher.on_make_delegate(*this); }
void ImmediateDelegate::match(ActionMatcher& matcher) { matcher.on_immediate_delegate(*this); }
void MakeFnPtr::match(ActionMatcher& matcher) { matcher.on_make_fn_ptr(*this); }
void ToIntOp::match(ActionMatcher& matcher) { matcher.on_to_int(*this); }
void ToFloatOp::match(ActionMatcher& matcher) { matcher.on_to_float(*this); }
void NotOp::match(ActionMatcher& matcher) { matcher.on_not(*this); }
void NegOp::match(ActionMatcher& matcher) { matcher.on_neg(*this); }
void RefOp::match(ActionMatcher& matcher) { matcher.on_ref(*this); }
void FreezeOp::match(ActionMatcher& matcher) { matcher.on_freeze(*this); }
void Loop::match(ActionMatcher& matcher) { matcher.on_loop(*this); }
void CopyOp::match(ActionMatcher& matcher) { matcher.on_copy(*this); }
void MkWeakOp::match(ActionMatcher& matcher) { matcher.on_mk_weak(*this); }
void DerefWeakOp::match(ActionMatcher& matcher) { matcher.on_deref_weak(*this); }
void Block::match(ActionMatcher& matcher) { matcher.on_block(*this); }
void CastOp::match(ActionMatcher& matcher) { matcher.on_cast(*this); }
void AddOp::match(ActionMatcher& matcher) { matcher.on_add(*this); }
void SubOp::match(ActionMatcher& matcher) { matcher.on_sub(*this); }
void MulOp::match(ActionMatcher& matcher) { matcher.on_mul(*this); }
void DivOp::match(ActionMatcher& matcher) { matcher.on_div(*this); }
void ModOp::match(ActionMatcher& matcher) { matcher.on_mod(*this); }
void AndOp::match(ActionMatcher& matcher) { matcher.on_and(*this); }
void OrOp::match(ActionMatcher& matcher) { matcher.on_or(*this); }
void XorOp::match(ActionMatcher& matcher) { matcher.on_xor(*this); }
void ShlOp::match(ActionMatcher& matcher) { matcher.on_shl(*this); }
void ShrOp::match(ActionMatcher& matcher) { matcher.on_shr(*this); }
void EqOp::match(ActionMatcher& matcher) { matcher.on_eq(*this); }
void LtOp::match(ActionMatcher& matcher) { matcher.on_lt(*this); }
void If::match(ActionMatcher& matcher) { matcher.on_if(*this); }
void LAnd::match(ActionMatcher& matcher) { matcher.on_land(*this); }
void Else::match(ActionMatcher& matcher) { matcher.on_else(*this); }
void LOr::match(ActionMatcher& matcher) { matcher.on_lor(*this); }

void ActionMatcher::on_unmatched(Action& node) {}
void ActionMatcher::on_un_op(UnaryOp& node) { on_unmatched(node); }
void ActionMatcher::on_bin_op(BinaryOp& node) { on_unmatched(node); }
void ActionMatcher::on_const_i64(ConstInt64& node) { on_unmatched(node); }
void ActionMatcher::on_const_string(ConstString& node) { on_unmatched(node); }
void ActionMatcher::on_const_double(ConstDouble& node) { on_unmatched(node); }
void ActionMatcher::on_const_void(ConstVoid& node) { on_unmatched(node); }
void ActionMatcher::on_const_bool(ConstBool& node) { on_unmatched(node); }
void ActionMatcher::on_get(Get& node) { on_unmatched(node); }
void ActionMatcher::on_set(Set& node) { on_unmatched(node); }
void ActionMatcher::on_get_field(GetField& node) { on_unmatched(node); }
void ActionMatcher::on_set_field(SetField& node) { on_unmatched(node); }
void ActionMatcher::on_splice_field(SpliceField& node) { on_unmatched(node); }
void ActionMatcher::on_mk_lambda(MkLambda& node) { on_unmatched(node); }
void ActionMatcher::on_mk_instance(MkInstance& node) { on_unmatched(node); }
void ActionMatcher::on_call(Call& node) { on_unmatched(node); }
void ActionMatcher::on_get_at_index(GetAtIndex& node) { on_unmatched(node); }
void ActionMatcher::on_set_at_index(SetAtIndex& node) { on_unmatched(node); }
void ActionMatcher::on_make_delegate(MakeDelegate& node) { on_unmatched(node); }
void ActionMatcher::on_immediate_delegate(ImmediateDelegate& node) { on_unmatched(node); }
void ActionMatcher::on_make_fn_ptr(MakeFnPtr& node) { on_unmatched(node); }
void ActionMatcher::on_to_int(ToIntOp& node) { on_un_op(node); }
void ActionMatcher::on_to_float(ToFloatOp& node) { on_un_op(node); }
void ActionMatcher::on_not(NotOp& node) { on_un_op(node); }
void ActionMatcher::on_neg(NegOp& node) { on_un_op(node); }
void ActionMatcher::on_ref(RefOp& node) { on_un_op(node); }
void ActionMatcher::on_freeze(FreezeOp& node) { on_un_op(node); }
void ActionMatcher::on_loop(Loop& node) { on_un_op(node); }
void ActionMatcher::on_copy(CopyOp& node) { on_un_op(node); }
void ActionMatcher::on_mk_weak(MkWeakOp& node) { on_un_op(node); }
void ActionMatcher::on_deref_weak(DerefWeakOp& node) { on_un_op(node); }
void ActionMatcher::on_block(Block& node) { on_unmatched(node); }
void ActionMatcher::on_cast(CastOp& node) { on_bin_op(node); }
void ActionMatcher::on_add(AddOp& node) { on_bin_op(node); }
void ActionMatcher::on_sub(SubOp& node) { on_bin_op(node); }
void ActionMatcher::on_mul(MulOp& node) { on_bin_op(node); }
void ActionMatcher::on_div(DivOp& node) { on_bin_op(node); }
void ActionMatcher::on_mod(ModOp& node) { on_bin_op(node); }
void ActionMatcher::on_and(AndOp& node) { on_bin_op(node); }
void ActionMatcher::on_or(OrOp& node) { on_bin_op(node); }
void ActionMatcher::on_xor(XorOp& node) { on_bin_op(node); }
void ActionMatcher::on_shl(ShlOp& node) { on_bin_op(node); }
void ActionMatcher::on_shr(ShrOp& node) { on_bin_op(node); }
void ActionMatcher::on_eq(EqOp& node) { on_bin_op(node); }
void ActionMatcher::on_lt(LtOp& node) { on_bin_op(node); }
void ActionMatcher::on_if(If& node) { on_bin_op(node); }
void ActionMatcher::on_land(LAnd& node) { on_bin_op(node); }
void ActionMatcher::on_else(Else& node) { on_bin_op(node); }
void ActionMatcher::on_lor(LOr& node) { on_bin_op(node); }

void ActionMatcher::fix(own<Action>& ptr) {
	auto saved = fix_result;
	fix_result = &ptr;
	ptr.pinned()->match(*this);
	fix_result = saved;
}

void ActionScanner::on_un_op(UnaryOp& node) { fix(node.p); }
void ActionScanner::on_bin_op(BinaryOp& node) {
	fix(node.p[0]);
	fix(node.p[1]);
}
void ActionScanner::on_set(Set& node) { fix(node.val); }
void ActionScanner::on_mk_lambda(MkLambda& node) { on_block(node); }
void ActionScanner::on_call(Call& node) {
	for (auto& p : node.params)
		fix(p);
	fix(node.callee);
}
void ActionScanner::on_get_at_index(GetAtIndex& node) {
	for (auto& p : node.indexes)
		fix(p);
	fix(node.indexed);
}
void ActionScanner::on_set_at_index(SetAtIndex& node) {
	on_get_at_index(node);
	fix(node.value);
}
void ActionScanner::on_make_delegate(MakeDelegate& node) { fix(node.base); }
void ActionScanner::on_immediate_delegate(ImmediateDelegate& node) {
	on_block(node);
	fix(node.type_expression);
	fix(node.base);
}
void ActionScanner::on_block(Block& node) {
	for (auto& l : node.names)
		fix(l->initializer);
	for (auto& p : node.body)
		fix(p);
}
void ActionScanner::on_get_field(GetField& node) { fix(node.base); }
void ActionScanner::on_set_field(SetField& node) {
	fix(node.base);
	fix(node.val);
}
void ActionScanner::on_splice_field(SpliceField& node) { on_set_field(node); }

void TpInt64::match(TypeMatcher& matcher) { matcher.on_int64(*this); }
void TpDouble::match(TypeMatcher& matcher) { matcher.on_double(*this); }
void TpFunction::match(TypeMatcher& matcher) { matcher.on_function(*this); }
void TpLambda::match(TypeMatcher& matcher) { matcher.on_lambda(*this); }
void TpColdLambda::match(TypeMatcher& matcher) { matcher.on_cold_lambda(*this); }
void TpDelegate::match(TypeMatcher& matcher) { matcher.on_delegate(*this); }
void TpVoid::match(TypeMatcher& matcher) { matcher.on_void(*this); }
void TpOptional::match(TypeMatcher& matcher) { matcher.on_optional(*this); }
void TpClass::match(TypeMatcher& matcher) { matcher.on_class(*this); }
void TpRef::match(TypeMatcher& matcher) { matcher.on_ref(*this); }
void TpShared::match(TypeMatcher& matcher) { matcher.on_shared(*this); }
void TpWeak::match(TypeMatcher& matcher) { matcher.on_weak(*this); }

size_t typelist_hasher::operator() (const vector<own<Type>>* v) const {
	size_t r = 0;
	for (const auto& p : *v)
		r += std::hash<void*>()(p);
	return r;
}

bool typelist_comparer::operator() (const vector<own<Type>>* a, const vector<own<Type>>* b) const {
	return *a == *b;
}

pin<Field> Ast::mk_field (string name, pin<Action> initializer) {
	auto f = pin<Field>::make();
	f->name = move(name);
	f->module = sys;
	f->initializer = initializer;
	return f;
};

pin<TpClass> Ast::mk_class(string name, std::initializer_list<pin<Field>> fields) {
	auto r = new TpClass;
	sys->classes.insert({ name, r });
	r->module = sys;
	r->name = move(name);
	for (auto& f : fields)
		r->fields.push_back(f);
	return r;
};

pin<ast::Function> Ast::mk_fn(string name, void(*entry_point)(), pin<Action> result_type, std::initializer_list<pin<Type>> params) {
	auto fn = pin<ast::Function>::make();
	sys->functions.insert({ name, fn });
	fn->module = sys;
	fn->name = name;
	fn->is_platform = true;
	fn->type_expression = result_type;
	int numerator = 0;
	for (auto& p : params) {
		fn->names.push_back(new Var);
		fn->names.back()->type = p;
		fn->names.back()->name = ast::format_str("p", numerator++);
	}
	if (entry_point)
		platform_exports.insert({ ast::format_str("ag_fn_sys_", name), entry_point});
	return fn;
};

Ast::Ast()
	: dom(new dom::Dom(cpp_dom)) {
	auto s = pin<Module>::make();
	s->name = "sys";
	modules.insert({"sys", s});
	sys = s;
	register_runtime_content(*this);
}

pin<TpInt64> Ast::tp_int64() {
	static auto r = own<TpInt64>::make();
	return r;
}
pin<TpDouble> Ast::tp_double() {
	static auto r = own<TpDouble>::make();
	return r;
}
pin<TpVoid> Ast::tp_void() {
	static auto r = own<TpVoid>::make();
	return r;
}
pin<TpFunction> Ast::tp_function(vector<own<Type>>&& params) {
	auto at = function_types_.find(&params);
	if (at != function_types_.end())
		return at->second;
	auto r = pin<TpFunction>::make();
	r->params = move(params);
	function_types_.insert({ &r->params, r });
	return r;
}
pin<TpLambda> Ast::tp_lambda(vector<own<Type>>&& params) {
	auto at = lambda_types_.find(&params);
	if (at != lambda_types_.end())
		return at->second;
	auto r = pin<TpLambda>::make();
	r->params = move(params);
	lambda_types_.insert({&r->params, r});
	return r;
}
pin<TpDelegate> Ast::tp_delegate(vector<own<Type>>&& params) {
	auto at = delegate_types_.find(&params);
	if (at != delegate_types_.end())
		return at->second;
	auto r = pin<TpDelegate>::make();
	r->params = move(params);
	delegate_types_.insert({ &r->params, r });
	return r;
}
pin<TpOptional> Ast::tp_optional(pin<Type> wrapped) {
	int depth = 0;
	if (auto as_optional = dom::strict_cast<ast::TpOptional>(wrapped)) {
		depth = as_optional->depth + 1;
		wrapped = as_optional->wrapped;
	}
	auto& depths = optional_types_[wrapped];
	assert(depth <= depths.size());
	if (depth == depths.size()) {
		depths.push_back(pin<TpOptional>::make());
		depths.back()->wrapped = wrapped;
		depths.back()->depth = depth;
	}
	return depths[depth];
}

pin<Type> Ast::get_wrapped(pin<TpOptional> opt) {
	return opt->depth == 0
		? opt->wrapped
		: optional_types_[opt->wrapped][size_t(opt->depth) - 1];
}

pin<TpRef> Ast::get_ref(pin<TpClass> target) {
	auto& r = refs[target];
	if (!r) {
		r = new TpRef;
		r->target = target;
	}
	return r;
}

pin<TpShared> Ast::get_shared(pin<TpClass> target) {
	auto& r = shareds[target];
	if (!r) {
		r = new TpShared;
		r->target = target;
	}
	return r;
}

pin<TpWeak> Ast::get_weak(pin<TpClass> target) {
	auto& w = weaks[target];
	if (!w) {
		w = new TpWeak;
		w->target = target;
	}
	return w;
}

pin<TpClass> Module::get_class(const string& name) {
	if (auto r = peek_class(name))
		return r;
	auto r = pin<TpClass>::make();
	r->name = name;
	r->module = this;
	classes.insert({ string(name), r });
	return r;
}

pin<TpClass> Module::peek_class(const string& name) {
	auto it = classes.find(name);
	return it == classes.end() ? nullptr : it->second;
}

pin<TpClass> Ast::extract_class(pin<Type> pointer) {
	if (auto as_ref = dom::strict_cast<ast::TpRef>(pointer))
		return as_ref->target;
	if (auto as_shared = dom::strict_cast<ast::TpShared>(pointer))
		return as_shared->target;
	if (auto as_class = dom::strict_cast<ast::TpClass>(pointer))
		return as_class;
	// no weaks here, their targets are not directly accessible without a null check
	return nullptr;
}

void Node::err_out(const std::string& message) {
	std::cerr << message;
	throw 1;
}

string Node::get_annotation() {
	return (std::stringstream() << *this).str();
}

string Action::get_annotation() {
	std::stringstream r;
	r << Node::get_annotation();
	if (type_)
		r << " tp=" << type_.pinned();
	return r.str();
}

string Var::get_annotation() {
	std::stringstream r;
	r << Node::get_annotation();
	if (type)
		r << " tp=" << type.pinned();
	if (lexical_depth)
		r << " depth=" << lexical_depth;
	return r.str();
}

string DataRef::get_annotation() {
	return
		var ? Node::get_annotation() :
		var_module ? format_str(Node::get_annotation(), " var_name=", var_module->name, "_", var_name) :
		format_str(Node::get_annotation(), " var_name=", var_name);
}

string FieldRef::get_annotation() {
	return
		field ? Node::get_annotation() :
		field_module ? format_str(Node::get_annotation(), " field_name=", field_module->name, "_", field_name) :
		format_str(Node::get_annotation(), " field_name=", field_name);
}

} // namespace ast

namespace std {

ostream& operator<< (ostream& dst, const ast::LongName& name) {
	if (name.module) {
		dst << name.module->name << '_';
	}
	return dst << name.name;
}

string to_string(const ast::LongName& name) {
	return (std::stringstream() << name).str();
}

std::ostream& operator<< (std::ostream& dst, const ast::Node& n) {
	return dst << '(' << n.module->name << ':' << n.line << ':' << n.pos << ')';
}

std::ostream& operator<< (std::ostream& dst, const ltm::pin<ast::Type>& t) {
	struct TypePrinter : ast::TypeMatcher {
		std::ostream& dst;
		TypePrinter(std::ostream& dst) : dst(dst) {}
		void on_int64(ast::TpInt64& type) override { dst << "int"; }
		void on_double(ast::TpDouble& type) override { dst << "double"; }
		void on_void(ast::TpVoid& type) override { dst << "void"; }
		void out_proto(ast::TpFunction& type) {
			size_t i = 0, last = type.params.size() - 1;
			for (auto& p : type.params) {
				dst << (i == 0
					? i == last
						? "(){"
						: "("
					: i == last
						? "){"
						: ",")
					<< p.pinned();
				i++;
			}
			dst << "}";
		}
		void on_function(ast::TpFunction& type) override {
			dst << "fn";
			out_proto(type);
		}
		void on_lambda(ast::TpLambda& type) override {
			out_proto(type);
		}
		void on_cold_lambda(ast::TpColdLambda& type) override {
			dst << "[" << type.callees.size() << "]";
			if (type.resolved)
				dst << type.resolved.pinned();
			else
				dst << "[~]";
		}
		void on_delegate(ast::TpDelegate& type) override {
			dst << "&";
			out_proto(type);
		}
		void on_optional(ast::TpOptional& type) override {
			if (dom::strict_cast<ast::TpVoid>(type.wrapped)) {
				for (int i = type.depth; --i >= 0;)
					dst << "?";
				dst << "bool";
			} else {
				for (int i = type.depth + 1; --i >= 0;)
					dst << "?";
				dst << type.wrapped.pinned();
			}
		}
		void on_class(ast::TpClass& type) override {
			dst << "@" <<  type.get_name();
		}
		void on_ref(ast::TpRef& type) override {
			dst << type.target->get_name();
		}
		void on_shared(ast::TpShared& type) override {
			dst << "*" << type.target->get_name();
		}
		void on_weak(ast::TpWeak& type) override {
			dst << "&" << type.target->get_name();
		}
	};
	TypePrinter printer(dst);
	t->match(printer);
	return dst;
}

}  // namespace std
