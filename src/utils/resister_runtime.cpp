#include "utils/runtime.h"

#include <vector>
#include <cstring>

#include "compiler/ast.h"
#include "runtime/runtime.h"

void register_content(struct ast::Ast& ast) {
	if (ast.object)
		return;
#ifdef STANDALONE_COMPILER_MODE
#define FN(A) (void(*)())(nullptr)
#else
	using FN = void(*)();
#endif
	auto sys = ast.dom->names()->get("sys");
	ast.object = ast.mk_class(sys->get("Object"));
	auto container = ast.mk_class(sys->get("Container"), {
		ast.mk_field(sys->get("_size"), new ast::ConstInt64),
		ast.mk_field(sys->get("_data"), new ast::ConstInt64) });
	ast.mk_fn(sys->get("Container")->get("size"), FN(&Blob::get_size), new ast::ConstInt64, { ast.get_ref(container) });
	ast.mk_fn(sys->get("Container")->get("insert"), FN(&Blob::insert_items), new ast::ConstVoid, { ast.get_ref(container), ast.tp_int64(), ast.tp_int64() });
	ast.mk_fn(sys->get("Container")->get("move"), FN(&Blob::move_array_items), new ast::ConstBool, { ast.get_ref(container), ast.tp_int64(), ast.tp_int64(), ast.tp_int64() });

	ast.blob = ast.mk_class(sys->get("Blob"));
	ast.blob->overloads[container];
	ast.mk_fn(sys->get("Blob")->get("getAt"), FN(&Blob::get_at), new ast::ConstInt64, { ast.get_ref(ast.blob), ast.tp_int64() });
	ast.mk_fn(sys->get("Blob")->get("setAt"), FN(&Blob::set_at), new ast::ConstVoid, { ast.get_ref(ast.blob), ast.tp_int64(), ast.tp_int64() });
	ast.mk_fn(sys->get("Blob")->get("getByteAt"), FN(&Blob::get_i8_at), new ast::ConstInt64, { ast.get_ref(ast.blob), ast.tp_int64() });
	ast.mk_fn(sys->get("Blob")->get("setByteAt"), FN(&Blob::set_i8_at), new ast::ConstVoid, { ast.get_ref(ast.blob), ast.tp_int64(), ast.tp_int64() });
	ast.mk_fn(sys->get("Blob")->get("get16At"), FN(&Blob::get_i16_at), new ast::ConstInt64, { ast.get_ref(ast.blob), ast.tp_int64() });
	ast.mk_fn(sys->get("Blob")->get("set16At"), FN(&Blob::set_i16_at), new ast::ConstVoid, { ast.get_ref(ast.blob), ast.tp_int64(), ast.tp_int64() });
	ast.mk_fn(sys->get("Blob")->get("get32At"), FN(&Blob::get_i32_at), new ast::ConstInt64, { ast.get_ref(ast.blob), ast.tp_int64() });
	ast.mk_fn(sys->get("Blob")->get("set32At"), FN(&Blob::set_i32_at), new ast::ConstVoid, { ast.get_ref(ast.blob), ast.tp_int64(), ast.tp_int64() });
	ast.mk_fn(sys->get("Blob")->get("delete"), FN(&Blob::delete_blob_items), new ast::ConstVoid, { ast.get_ref(ast.blob), ast.tp_int64(), ast.tp_int64() });
	ast.mk_fn(sys->get("Blob")->get("copy"), FN(&Blob::blob_copy), new ast::ConstBool, { ast.get_ref(ast.blob), ast.tp_int64(), ast.get_ref(container), ast.tp_int64(), ast.tp_int64() });
	ast.mk_fn(sys->get("Blob")->get("putCh"), FN(&Blob::put_ch), new ast::ConstInt64, { ast.get_ref(ast.blob), ast.tp_int64(), ast.tp_int64() });
	ast.mk_fn(sys->get("terminate"), FN(&std::quick_exit), new ast::ConstVoid, {});

	auto inst = new ast::MkInstance;
	inst->cls = ast.object.pinned();
	auto ref_to_object = new ast::RefOp;
	ref_to_object->p = inst;
	auto opt_ref_to_object = new ast::If;
	opt_ref_to_object->p[0] = new ast::ConstBool;
	opt_ref_to_object->p[1] = ref_to_object;
	ast.own_array = ast.mk_class(sys->get("Array"));
	ast.own_array->overloads[container];
	ast.mk_fn(sys->get("Array")->get("getAt"), FN(&Blob::get_ref_at), opt_ref_to_object, { ast.get_ref(ast.own_array), ast.tp_int64() });
	ast.mk_fn(sys->get("Array")->get("setAt"), FN(&Blob::set_own_at), new ast::ConstVoid, { ast.get_ref(ast.own_array), ast.tp_int64(), ast.tp_optional(ast.object) });
	ast.mk_fn(sys->get("Array")->get("delete"), FN(&Blob::delete_array_items), new ast::ConstVoid, { ast.get_ref(ast.own_array), ast.tp_int64(), ast.tp_int64() });

	ast.weak_array = ast.mk_class(sys->get("WeakArray"));
	ast.weak_array->overloads[container];
	auto weak_to_object = new ast::MkWeakOp;
	weak_to_object->p = inst;
	ast.mk_fn(sys->get("WeakArray")->get("getAt"), FN(&Blob::get_weak_at), weak_to_object, { ast.get_ref(ast.weak_array), ast.tp_int64() });
	ast.mk_fn(sys->get("WeakArray")->get("setAt"), FN(&Blob::set_weak_at), new ast::ConstVoid, { ast.get_ref(ast.weak_array), ast.tp_int64(), ast.get_weak(ast.object) });
	ast.mk_fn(sys->get("WeakArray")->get("delete"), FN(&Blob::delete_weak_array_items), new ast::ConstVoid, { ast.get_ref(ast.weak_array), ast.tp_int64(), ast.tp_int64() });

	ast.string_cls = ast.mk_class(sys->get("String"), {
		ast.mk_field(sys->get("_cursor"), new ast::ConstInt64),
		ast.mk_field(sys->get("_buffer"), new ast::ConstInt64) });
	ast.mk_fn(sys->get("String")->get("fromBlob"), FN(&Blob::to_str), new ast::ConstBool, { ast.get_ref(ast.string_cls), ast.get_ref(ast.blob), ast.tp_int64(), ast.tp_int64() });
	ast.mk_fn(sys->get("String")->get("getCh"), FN(&StringObj::get_char), new ast::ConstInt64, { ast.get_ref(ast.string_cls) });
	ast.mk_fn(sys->get("makeShared"), FN(&Object::make_shared), new ast::ConstVoid, { ast.get_ref(ast.object) });  // TODO: its a hack till frozen objects were introduced

	ast.platform_exports.insert({
		{ "copy", FN(&Object::copy) },
		{ "copy_object_field", FN(&Object::copy_object_field) },
		{ "copy_weak_field", FN(&Object::copy_weak_field) },
		{ "release_weak", FN(&Object::release_weak) },
		{ "release", FN(&Object::release) },
		{ "alloc", FN(&Object::allocate) },
		{ "mk_weak", FN(&Object::mk_weak) },
		{ "deref_weak", FN(&Object::deref_weak) },
		{ "reg_copy_fixer", FN(&Object::reg_copy_fixer) },

		{ "sys_Container!copy", FN(&Blob::copy_container_fields) },
		{ "sys_Container!dtor", FN(&Blob::dispose_container) },
		{ "sys_Blob!copy", FN(&Blob::copy_container_fields) },
		{ "sys_Blob!dtor", FN(&Blob::dispose_container) },
		{ "sys_Array!copy", FN(&Blob::copy_array_fields) },
		{ "sys_Array!dtor",FN(&Blob::dispose_array) },
		{ "sys_WeakArray!copy", FN(&Blob::copy_weak_array_fields) },
		{ "sys_WeakArray!dtor", FN(&Blob::dispose_weak_array) },
		{ "sys_String!copy", FN(&StringObj::copy_fields) },
		{ "sys_String!dtor", FN(&StringObj::dispose_fields) }});
}
