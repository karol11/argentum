#include "utils/register_runtime.h"

#include <vector>
#include <cstring>

#include "compiler/ast.h"
#include "runtime/runtime.h"

void register_runtime_content(struct ast::Ast& ast) {
	if (ast.object)
		return;
#ifdef AG_STANDALONE_COMPILER_MODE
#define FN(A) (void(*)())(nullptr)
#else
	using FN = void(*)();
#endif
	auto sys = ast.dom->names()->get("sys");
	ast.object = ast.mk_class(sys->get("Object"));
	auto container = ast.mk_class(sys->get("Container"), {
		ast.mk_field(sys->get("_size"), new ast::ConstInt64),
		ast.mk_field(sys->get("_data"), new ast::ConstInt64) });
	ast.mk_fn(sys->get("Container")->get("size"), FN(ag_fn_sys_Container_size), new ast::ConstInt64, { ast.get_ref(container) });
	ast.mk_fn(sys->get("Container")->get("insert"), FN(&ag_fn_sys_Container_insert), new ast::ConstVoid, { ast.get_ref(container), ast.tp_int64(), ast.tp_int64() });
	ast.mk_fn(sys->get("Container")->get("move"), FN(&ag_fn_sys_Container_move), new ast::ConstBool, { ast.get_ref(container), ast.tp_int64(), ast.tp_int64(), ast.tp_int64() });

	ast.blob = ast.mk_class(sys->get("Blob"));
	ast.blob->overloads[container];
	ast.mk_fn(sys->get("Blob")->get("getAt"), FN(ag_fn_sys_Blob_getAt), new ast::ConstInt64, { ast.get_ref(ast.blob), ast.tp_int64() });
	ast.mk_fn(sys->get("Blob")->get("setAt"), FN(ag_fn_sys_Blob_setAt), new ast::ConstVoid, { ast.get_ref(ast.blob), ast.tp_int64(), ast.tp_int64() });
	ast.mk_fn(sys->get("Blob")->get("getByteAt"), FN(ag_fn_sys_Blob_getByteAt), new ast::ConstInt64, { ast.get_ref(ast.blob), ast.tp_int64() });
	ast.mk_fn(sys->get("Blob")->get("setByteAt"), FN(ag_fn_sys_Blob_setByteAt), new ast::ConstVoid, { ast.get_ref(ast.blob), ast.tp_int64(), ast.tp_int64() });
	ast.mk_fn(sys->get("Blob")->get("get16At"), FN(ag_fn_sys_Blob_get16At), new ast::ConstInt64, { ast.get_ref(ast.blob), ast.tp_int64() });
	ast.mk_fn(sys->get("Blob")->get("set16At"), FN(ag_fn_sys_Blob_set16At), new ast::ConstVoid, { ast.get_ref(ast.blob), ast.tp_int64(), ast.tp_int64() });
	ast.mk_fn(sys->get("Blob")->get("get32At"), FN(ag_fn_sys_Blob_get32At), new ast::ConstInt64, { ast.get_ref(ast.blob), ast.tp_int64() });
	ast.mk_fn(sys->get("Blob")->get("set32At"), FN(ag_fn_sys_Blob_set32At), new ast::ConstVoid, { ast.get_ref(ast.blob), ast.tp_int64(), ast.tp_int64() });
	ast.mk_fn(sys->get("Blob")->get("delete"), FN(ag_fn_sys_Blob_delete), new ast::ConstVoid, { ast.get_ref(ast.blob), ast.tp_int64(), ast.tp_int64() });
	ast.mk_fn(sys->get("Blob")->get("copy"), FN(ag_fn_sys_Blob_copy), new ast::ConstBool, { ast.get_ref(ast.blob), ast.tp_int64(), ast.get_ref(container), ast.tp_int64(), ast.tp_int64() });
	ast.mk_fn(sys->get("Blob")->get("putCh"), FN(ag_fn_sys_Blob_putCh), new ast::ConstInt64, { ast.get_ref(ast.blob), ast.tp_int64(), ast.tp_int64() });
	ast.mk_fn(sys->get("terminate"), FN(ag_fn_sys_terminate), new ast::ConstVoid, { ast.tp_int64() });

	auto inst = new ast::MkInstance;
	inst->cls = ast.object.pinned();
	auto ref_to_object = new ast::RefOp;
	ref_to_object->p = inst;
	auto opt_ref_to_object = new ast::If;
	opt_ref_to_object->p[0] = new ast::ConstBool;
	opt_ref_to_object->p[1] = ref_to_object;
	ast.own_array = ast.mk_class(sys->get("Array"));
	ast.own_array->overloads[container];
	ast.mk_fn(sys->get("Array")->get("getAt"), FN(ag_fn_sys_Array_getAt), opt_ref_to_object, { ast.get_ref(ast.own_array), ast.tp_int64() });
	ast.mk_fn(sys->get("Array")->get("setAt"), FN(ag_fn_sys_Array_setAt), new ast::ConstVoid, { ast.get_ref(ast.own_array), ast.tp_int64(), ast.tp_optional(ast.object) });
	ast.mk_fn(sys->get("Array")->get("delete"), FN(ag_fn_sys_Array_delete), new ast::ConstVoid, { ast.get_ref(ast.own_array), ast.tp_int64(), ast.tp_int64() });
	ast.mk_fn(sys->get("Array")->get("spliceAt"), FN(ag_fn_sys_Array_spliceAt), new ast::ConstBool, { ast.get_ref(ast.own_array), ast.tp_int64(), ast.tp_optional(ast.get_ref(ast.object)) });

	ast.weak_array = ast.mk_class(sys->get("WeakArray"));
	ast.weak_array->overloads[container];
	auto weak_to_object = new ast::MkWeakOp;
	weak_to_object->p = inst;
	ast.mk_fn(sys->get("WeakArray")->get("getAt"), FN(ag_fn_sys_WeakArray_getAt), weak_to_object, { ast.get_ref(ast.weak_array), ast.tp_int64() });
	ast.mk_fn(sys->get("WeakArray")->get("setAt"), FN(ag_fn_sys_WeakArray_setAt), new ast::ConstVoid, { ast.get_ref(ast.weak_array), ast.tp_int64(), ast.get_weak(ast.object) });
	ast.mk_fn(sys->get("WeakArray")->get("delete"), FN(ag_fn_sys_WeakArray_delete), new ast::ConstVoid, { ast.get_ref(ast.weak_array), ast.tp_int64(), ast.tp_int64() });

	ast.string_cls = ast.mk_class(sys->get("String"), {
		ast.mk_field(sys->get("_cursor"), new ast::ConstInt64),
		ast.mk_field(sys->get("_buffer"), new ast::ConstInt64) });
	ast.mk_fn(sys->get("String")->get("fromBlob"), FN(ag_fn_sys_String_fromBlob), new ast::ConstBool, { ast.get_ref(ast.string_cls), ast.get_ref(ast.blob), ast.tp_int64(), ast.tp_int64() });
	ast.mk_fn(sys->get("String")->get("getCh"), FN(ag_fn_sys_String_getCh), new ast::ConstInt64, { ast.get_ref(ast.string_cls) });
	ast.mk_fn(sys->get("getParent"), FN(ag_fn_sys_getParent), opt_ref_to_object, { ast.get_ref(ast.object) });
	ast.mk_fn(sys->get("log"), FN(ag_fn_sys_log), new ast::ConstVoid, { ast.get_ref(ast.string_cls) });

	ast.platform_exports.insert({
		{ "ag_copy", FN(ag_copy) },
		{ "ag_copy_object_field", FN(ag_copy_object_field) },
		{ "ag_copy_weak_field", FN(ag_copy_weak_field) },
		{ "ag_allocate_obj", FN(ag_allocate_obj) },
		{ "ag_mk_weak", FN(ag_mk_weak) },
		{ "ag_deref_weak", FN(ag_deref_weak) },
		{ "ag_reg_copy_fixer", FN(ag_reg_copy_fixer) },
		{ "ag_retain_own", FN(ag_retain_own) },
		{ "ag_release_own", FN(ag_release_own) },
		{ "ag_release", FN(ag_release) },
		{ "ag_release_weak", FN(ag_release_weak) },
		{ "ag_dispose_obj", FN(ag_dispose_obj) },
		{ "ag_set_parent", FN(ag_set_parent) },
		{ "ag_splice", FN(ag_splice) },
		{ "ag_freeze", FN(ag_freeze) },

		{ "ag_copy_sys_Container", FN(ag_copy_sys_Container) },
		{ "ag_dtor_sys_Container", FN(ag_dtor_sys_Container) },
		{ "ag_copy_sys_Blob", FN(ag_copy_sys_Blob) },
		{ "ag_dtor_sys_Blob", FN(ag_dtor_sys_Blob) },
		{ "ag_copy_sys_Array", FN(ag_copy_sys_Array) },
		{ "ag_dtor_sys_Array",FN(ag_dtor_sys_Array) },
		{ "ag_copy_sys_WeakArray", FN(ag_copy_sys_WeakArray) },
		{ "ag_dtor_sys_WeakArray", FN(ag_dtor_sys_WeakArray) },
		{ "ag_copy_sys_String", FN(ag_copy_sys_String) },
		{ "ag_dtor_sys_String", FN(ag_dtor_sys_String) }});
}
