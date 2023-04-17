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
	ast.object = ast.mk_class("Object");
	auto container = ast.mk_class("Container", {
		ast.mk_field("_size", new ast::ConstInt64),
		ast.mk_field("_data", new ast::ConstInt64) });
	ast.mk_fn("getSize", FN(ag_fn_sys_getSize), new ast::ConstInt64, { ast.get_ref(container) });
	ast.mk_fn("insertItems", FN(&ag_fn_sys_insertItems), new ast::ConstVoid, { ast.get_ref(container), ast.tp_int64(), ast.tp_int64() });
	ast.mk_fn("moveItems", FN(&ag_fn_sys_moveItems), new ast::ConstBool, { ast.get_ref(container), ast.tp_int64(), ast.tp_int64(), ast.tp_int64() });

	ast.blob = ast.mk_class("Blob");
	ast.blob->overloads[container];
	ast.mk_fn("get8At", FN(ag_fn_sys_get8At), new ast::ConstInt64, { ast.get_ref(ast.blob), ast.tp_int64() });
	ast.mk_fn("set8At", FN(ag_fn_sys_set8At), new ast::ConstVoid, { ast.get_ref(ast.blob), ast.tp_int64(), ast.tp_int64() });
	ast.mk_fn("get16At", FN(ag_fn_sys_get16At), new ast::ConstInt64, { ast.get_ref(ast.blob), ast.tp_int64() });
	ast.mk_fn("set16At", FN(ag_fn_sys_set16At), new ast::ConstVoid, { ast.get_ref(ast.blob), ast.tp_int64(), ast.tp_int64() });
	ast.mk_fn("get32At", FN(ag_fn_sys_get32At), new ast::ConstInt64, { ast.get_ref(ast.blob), ast.tp_int64() });
	ast.mk_fn("set32At", FN(ag_fn_sys_set32At), new ast::ConstVoid, { ast.get_ref(ast.blob), ast.tp_int64(), ast.tp_int64() });
	ast.mk_fn("get64At", FN(ag_fn_sys_get64At), new ast::ConstInt64, { ast.get_ref(ast.blob), ast.tp_int64() });
	ast.mk_fn("set64At", FN(ag_fn_sys_set64At), new ast::ConstVoid, { ast.get_ref(ast.blob), ast.tp_int64(), ast.tp_int64() });
	ast.mk_fn("deleteBytes", FN(ag_fn_sys_deleteBytes), new ast::ConstVoid, { ast.get_ref(ast.blob), ast.tp_int64(), ast.tp_int64() });
	ast.mk_fn("copyBytes", FN(ag_fn_sys_copyBytes), new ast::ConstBool, { ast.get_ref(ast.blob), ast.tp_int64(), ast.get_ref(ast.blob), ast.tp_int64(), ast.tp_int64() });
	ast.mk_fn("putCh", FN(ag_fn_sys_putCh), new ast::ConstInt64, { ast.get_ref(ast.blob), ast.tp_int64(), ast.tp_int64() });

	auto inst = new ast::MkInstance;
	inst->cls = ast.object.pinned();
	auto ref_to_object = new ast::RefOp;
	ref_to_object->p = inst;
	auto opt_ref_to_object = new ast::If;
	opt_ref_to_object->p[0] = new ast::ConstBool;
	opt_ref_to_object->p[1] = ref_to_object;
	ast.own_array = ast.mk_class("Array");
	ast.own_array->overloads[container];
	ast.mk_fn("getAt", FN(ag_fn_sys_getAt), opt_ref_to_object, { ast.get_ref(ast.own_array), ast.tp_int64() });
	ast.mk_fn("setAt", FN(ag_fn_sys_setAt), new ast::ConstVoid, { ast.get_ref(ast.own_array), ast.tp_int64(), ast.tp_optional(ast.object) });
	ast.mk_fn("deleteItems", FN(ag_fn_sys_deleteItems), new ast::ConstVoid, { ast.get_ref(ast.own_array), ast.tp_int64(), ast.tp_int64() });
	ast.mk_fn("spliceAt", FN(ag_fn_sys_spliceAt), new ast::ConstBool, { ast.get_ref(ast.own_array), ast.tp_int64(), ast.tp_optional(ast.get_ref(ast.object)) });

	ast.weak_array = ast.mk_class("WeakArray");
	ast.weak_array->overloads[container];
	auto weak_to_object = new ast::MkWeakOp;
	weak_to_object->p = inst;
	ast.mk_fn("getWeakAt", FN(ag_fn_sys_getWeakAt), weak_to_object, { ast.get_ref(ast.weak_array), ast.tp_int64() });
	ast.mk_fn("setWeakAt", FN(ag_fn_sys_setWeakAt), new ast::ConstVoid, { ast.get_ref(ast.weak_array), ast.tp_int64(), ast.get_weak(ast.object) });
	ast.mk_fn("deleteWeakAt", FN(ag_fn_sys_deleteWeakAt), new ast::ConstVoid, { ast.get_ref(ast.weak_array), ast.tp_int64(), ast.tp_int64() });

	ast.string_cls = ast.mk_class("String", {
		ast.mk_field("_cursor", new ast::ConstInt64),
		ast.mk_field("_buffer", new ast::ConstInt64) });
	ast.mk_fn("stringFromBlob", FN(ag_fn_sys_stringFromBlob), new ast::ConstBool, { ast.get_ref(ast.string_cls), ast.get_ref(ast.blob), ast.tp_int64(), ast.tp_int64() });
	ast.mk_fn("getCh", FN(ag_fn_sys_getCh), new ast::ConstInt64, { ast.get_ref(ast.string_cls) });
	ast.mk_fn("getParent", FN(ag_fn_sys_getParent), opt_ref_to_object, { ast.get_ref(ast.object) });
	ast.mk_fn("log", FN(ag_fn_sys_log), new ast::ConstVoid, { ast.get_ref(ast.string_cls) });
	ast.mk_fn("terminate", FN(ag_fn_sys_terminate), new ast::ConstVoid, { ast.tp_int64() });

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
