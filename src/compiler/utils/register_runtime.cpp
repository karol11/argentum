#include "register_runtime.h"
//TODO: 
#include <vector>
#include <cstring>

#include "ast.h"
#include "../runtime/runtime.h"
#include "../runtime/blob.h"
#include "../runtime/array/own-array.h"
#include "../runtime/array/weak-array.h"
#include "../runtime/array/shared-array.h"
#include "../runtime/map/own-map.h"
#include "../runtime/map/shared-map.h"
#include "../runtime/map/weak-map.h"

void register_runtime_content(struct ast::Ast& ast) {
	if (ast.object)
		return;
#ifdef AG_STANDALONE_COMPILER_MODE
#define FN(A) (void(*)())(nullptr)
#else
	using FN = void(*)();
#endif
	using mut = ast::Mut;
	ast.object = ast.mk_class("Object");
	ast.object->used = true;
	auto obj_get_hash = ast.mk_method(mut::ANY, ast.object, "getHash", FN(ag_m_sys_Object_getHash), new ast::ConstInt64, {});
	obj_get_hash->used = true;
	auto obj_equals = ast.mk_method(mut::ANY, ast.object, "equals", FN(ag_m_sys_Object_equals), new ast::ConstBool, { ast.get_conform_ref(ast.object) });
	obj_equals->used = true;
	ast.blob = ast.mk_class("Blob", {
		ast.mk_field("_count", new ast::ConstInt64()),
		ast.mk_field("_bytes", new ast::ConstInt64())  // ptr
	});
	ast.mk_method(mut::ANY, ast.blob, "capacity", FN(ag_m_sys_Blob_capacity), new ast::ConstInt64, {});
	ast.mk_method(mut::MUTATING, ast.blob, "insert", FN(&ag_m_sys_Blob_insert), new ast::ConstVoid, { ast.tp_int64(), ast.tp_int64() });
	ast.mk_method(mut::MUTATING, ast.blob, "delete", FN(ag_m_sys_Blob_delete), new ast::ConstVoid, { ast.tp_int64(), ast.tp_int64() });
	ast.mk_method(mut::MUTATING, ast.blob, "copy", FN(ag_m_sys_Blob_copy), new ast::ConstBool, { ast.tp_int64(), ast.get_conform_ref(ast.blob), ast.tp_int64(), ast.tp_int64() });
	ast.mk_method(mut::ANY, ast.blob, "get8At", FN(ag_m_sys_Blob_get8At), new ast::ConstInt32, { ast.tp_int64() });
	ast.mk_method(mut::MUTATING, ast.blob, "set8At", FN(ag_m_sys_Blob_set8At), new ast::ConstVoid, { ast.tp_int64(), ast.tp_int32() });
	ast.mk_method(mut::ANY, ast.blob, "get16At", FN(ag_m_sys_Blob_get16At), new ast::ConstInt32, { ast.tp_int64() });
	ast.mk_method(mut::MUTATING, ast.blob, "set16At", FN(ag_m_sys_Blob_set16At), new ast::ConstVoid, { ast.tp_int64(), ast.tp_int32() });
	ast.mk_method(mut::ANY, ast.blob, "get32At", FN(ag_m_sys_Blob_get32At), new ast::ConstInt32, { ast.tp_int64() });
	ast.mk_method(mut::MUTATING, ast.blob, "set32At", FN(ag_m_sys_Blob_set32At), new ast::ConstVoid, { ast.tp_int64(), ast.tp_int32() });
	ast.mk_method(mut::ANY, ast.blob, "get64At", FN(ag_m_sys_Blob_get64At), new ast::ConstInt64, { ast.tp_int64() });
	ast.mk_method(mut::MUTATING, ast.blob, "set64At", FN(ag_m_sys_Blob_set64At), new ast::ConstVoid, { ast.tp_int64(), ast.tp_int64() });
	ast.mk_method(mut::MUTATING, ast.blob, "putChAt", FN(ag_m_sys_Blob_putChAt), new ast::ConstInt64, { ast.tp_int64(), ast.tp_int32() });
	ast.mk_method(mut::MUTATING, ast.blob, "mkStr", FN(ag_m_sys_Blob_mkStr), new ast::ConstString, { ast.tp_int64(), ast.tp_int64() });

	ast.str_builder = ast.mk_class("StrBuilder");
	ast.str_builder->overloads[ast.blob];

	auto inst = new ast::MkInstance;
	inst->cls = ast.object.pinned();
	auto ref_to_object = new ast::RefOp;
	ref_to_object->p = inst;
	auto opt_ref_to_object = new ast::If;
	opt_ref_to_object->p[0] = new ast::ConstBool;
	opt_ref_to_object->p[1] = ref_to_object;
	auto weak_to_object = new ast::MkWeakOp;
	weak_to_object->p = inst;
	auto add_class_param = [&](ltm::pin<ast::Class> cls, const char* name = "T") {
		auto param = ltm::pin<ast::ClassParam>::make();
		param->index = cls->params.size();
		cls->params.push_back(param);
		param->base = ast.object;
		param->name = name;
		return param;
	};
	auto make_ptr_result = [&](ltm::pin<ast::UnaryOp> typer, ltm::pin<ast::AbstractClass> cls) {
		auto inst_t = new ast::MkInstance;
		inst_t->cls = cls;
		typer->p = inst_t;
		return typer;
	};
	auto make_opt_result = [&](ltm::pin<ast::Action> ref) {
		auto opt_ref_to_t = new ast::If;
		opt_ref_to_t->p[0] = new ast::ConstBool;
		opt_ref_to_t->p[1] = ref;
		return opt_ref_to_t;
	};
	auto make_factory = [&](auto m) {
		m->is_factory = true;
		m->type_expression = new ast::ConstVoid;
	};
	ast.own_array = ast.mk_class("Array", {
		ast.mk_field("_itemsCount", new ast::ConstInt64()),
		ast.mk_field("_items", new ast::ConstInt64())  // ptr
	});
	{
		auto t_cls = add_class_param(ast.own_array);
		auto ref_to_t_res = make_ptr_result(new ast::RefOp, t_cls);
		auto opt_ref_to_t_res = make_opt_result(ref_to_t_res);
		auto own_to_t = ast.get_own(t_cls);
		auto opt_own_to_t = ast.tp_optional(own_to_t);
		ast.mk_method(mut::ANY, ast.own_array, "capacity", FN(ag_m_sys_Array_capacity), new ast::ConstInt64, {});
		ast.mk_method(mut::MUTATING, ast.own_array, "insert", FN(&ag_m_sys_Array_insert), new ast::ConstVoid, { ast.tp_int64(), ast.tp_int64() });
		ast.mk_method(mut::MUTATING, ast.own_array, "delete", FN(ag_m_sys_Array_delete), new ast::ConstVoid, { ast.tp_int64(), ast.tp_int64() });
		ast.mk_method(mut::MUTATING, ast.own_array, "move", FN(ag_m_sys_Array_move), new ast::ConstBool, { ast.tp_int64(), ast.tp_int64(), ast.tp_int64() });
		ast.mk_method(mut::ANY, ast.own_array, "getAt", FN(ag_m_sys_Array_getAt), opt_ref_to_t_res, { ast.tp_int64() });
		ast.mk_method(mut::MUTATING, ast.own_array, "setAt", FN(ag_m_sys_Array_setAt), new ast::ConstVoid, { ast.tp_int64(), own_to_t });
		ast.mk_method(mut::MUTATING, ast.own_array, "setOptAt", FN(ag_m_sys_Array_setOptAt), opt_ref_to_t_res, { ast.tp_int64(), opt_own_to_t });
		ast.mk_method(mut::MUTATING, ast.own_array, "spliceAt", FN(ag_m_sys_Array_spliceAt), new ast::ConstBool, { ast.tp_int64(), ast.tp_optional(ast.get_ref(t_cls)) });
	}
	ast.weak_array = ast.mk_class("WeakArray", {
		ast.mk_field("_itemsCount", new ast::ConstInt64()),
		ast.mk_field("_items", new ast::ConstInt64())  // ptr
	});
	{
		auto t_cls = add_class_param(ast.weak_array);
		ast.mk_method(mut::ANY, ast.weak_array, "capacity", FN(ag_m_sys_WeakArray_capacity), new ast::ConstInt64, {});
		ast.mk_method(mut::MUTATING, ast.weak_array, "insert", FN(&ag_m_sys_WeakArray_insert), new ast::ConstVoid, { ast.tp_int64(), ast.tp_int64() });
		ast.mk_method(mut::MUTATING, ast.weak_array, "delete", FN(ag_m_sys_WeakArray_delete), new ast::ConstVoid, { ast.tp_int64(), ast.tp_int64() });
		ast.mk_method(mut::MUTATING, ast.weak_array, "move", FN(ag_m_sys_WeakArray_move), new ast::ConstBool, { ast.tp_int64(), ast.tp_int64(), ast.tp_int64() });
		ast.mk_method(mut::ANY, ast.weak_array, "getAt", FN(ag_m_sys_WeakArray_getAt), make_ptr_result(new ast::MkWeakOp, t_cls), { ast.tp_int64() });
		ast.mk_method(mut::MUTATING, ast.weak_array, "setAt", FN(ag_m_sys_WeakArray_setAt), new ast::ConstVoid, { ast.tp_int64(), ast.get_weak(t_cls) });
	}
	{
		auto shared_array_cls = ast.mk_class("SharedArray", {
			ast.mk_field("_itemsCount", new ast::ConstInt64()),
			ast.mk_field("_items", new ast::ConstInt64())  // ptr
		});
		auto t_cls = add_class_param(shared_array_cls);
		ast.mk_method(mut::ANY, shared_array_cls, "capacity", FN(ag_m_sys_SharedArray_capacity), new ast::ConstInt64, {});
		ast.mk_method(mut::MUTATING, shared_array_cls, "insert", FN(&ag_m_sys_SharedArray_insert), new ast::ConstVoid, { ast.tp_int64(), ast.tp_int64() });
		ast.mk_method(mut::MUTATING, shared_array_cls, "delete", FN(ag_m_sys_SharedArray_delete), new ast::ConstVoid, { ast.tp_int64(), ast.tp_int64() });
		ast.mk_method(mut::MUTATING, shared_array_cls, "move", FN(ag_m_sys_SharedArray_move), new ast::ConstBool, { ast.tp_int64(), ast.tp_int64(), ast.tp_int64() });
		ast.mk_method(mut::ANY, shared_array_cls, "getAt", FN(ag_m_sys_SharedArray_getAt), make_opt_result(make_ptr_result(new ast::FreezeOp, t_cls)), { ast.tp_int64() });
		ast.mk_method(mut::MUTATING, shared_array_cls, "setAt", FN(ag_m_sys_SharedArray_setAt), new ast::ConstVoid, { ast.tp_int64(), ast.get_shared(t_cls) });
	}
	ast.string_cls = ast.mk_class("String", {});
	ast.string_cls->used = true;
	ast.mk_overload(ast.string_cls, FN(ag_m_sys_String_getHash), obj_get_hash);
	ast.mk_overload(ast.string_cls, FN(ag_m_sys_String_equals), obj_equals);
	{
		auto cursor_cls = ast.mk_class("Cursor", {
				ast.mk_field("_cursor", new ast::ConstInt64),
				ast.mk_field("_buffer", new ast::ConstString) });
		ast.mk_method(mut::MUTATING, cursor_cls, "getCh", FN(ag_m_sys_Cursor_getCh), new ast::ConstInt32, {});
		ast.mk_method(mut::ANY, cursor_cls, "peekCh", FN(ag_m_sys_Cursor_peekCh), new ast::ConstInt32, {});
		ast.mk_method(mut::ANY, cursor_cls, "offset", FN(ag_m_sys_Cursor_offset), new ast::ConstInt64, {});
		make_factory(ast.mk_method(mut::MUTATING, cursor_cls, "set", FN(ag_m_sys_Cursor_set), nullptr, { ast.get_shared(ast.string_cls) }));
	}
	{
		auto map_cls = ast.mk_class("Map", {
			ast.mk_field("_buckets", new ast::ConstInt64),
			ast.mk_field("_capacity", new ast::ConstInt64),
			ast.mk_field("_size", new ast::ConstInt64) });
		auto key_cls = add_class_param(map_cls, "K");
		auto val_cls = add_class_param(map_cls, "V");
		auto ref_to_val_res = make_ptr_result(new ast::RefOp, val_cls);
		auto opt_ref_to_val_res = make_opt_result(ref_to_val_res);
		auto opt_shared_to_key_res = make_opt_result(
			make_ptr_result(new ast::FreezeOp, key_cls));
		ast.mk_method(mut::ANY, map_cls, "size", FN(ag_m_sys_Map_size), new ast::ConstInt64, {});
		ast.mk_method(mut::ANY, map_cls, "capacity", FN(ag_m_sys_Map_capacity), new ast::ConstInt64, {});
		ast.mk_method(mut::ANY, map_cls, "clear", FN(ag_m_sys_Map_clear), new ast::ConstVoid, {});
		ast.mk_method(mut::ANY, map_cls, "delete", FN(ag_m_sys_Map_delete), opt_ref_to_val_res, { ast.get_shared(key_cls) });
		ast.mk_method(mut::ANY, map_cls, "getAt", FN(ag_m_sys_Map_getAt), opt_ref_to_val_res, { ast.get_shared(key_cls) });
		ast.mk_method(mut::ANY, map_cls, "setAt", FN(ag_m_sys_Map_setAt), opt_ref_to_val_res, { ast.get_shared(key_cls), ast.get_own(val_cls) });
		ast.mk_method(mut::ANY, map_cls, "keyAt", FN(ag_m_sys_Map_keyAt), opt_shared_to_key_res, { ast.tp_int64() });
		ast.mk_method(mut::ANY, map_cls, "valAt", FN(ag_m_sys_Map_valAt), opt_ref_to_val_res, { ast.tp_int64() });
	}
	{
		auto map_cls = ast.mk_class("SharedMap", {
			ast.mk_field("_buckets", new ast::ConstInt64),
			ast.mk_field("_capacity", new ast::ConstInt64),
			ast.mk_field("_size", new ast::ConstInt64) });
		auto key_cls = add_class_param(map_cls, "K");
		auto val_cls = add_class_param(map_cls, "V");
		auto shared_to_val_res = make_ptr_result(new ast::FreezeOp, val_cls);
		auto opt_shared_to_val_res = make_opt_result(shared_to_val_res);
		auto opt_shared_to_key_res = make_opt_result(
			make_ptr_result(new ast::FreezeOp, key_cls));
		ast.mk_method(mut::ANY, map_cls, "size", FN(ag_m_sys_SharedMap_size), new ast::ConstInt64, {});
		ast.mk_method(mut::ANY, map_cls, "capacity", FN(ag_m_sys_SharedMap_capacity), new ast::ConstInt64, {});
		ast.mk_method(mut::ANY, map_cls, "clear", FN(ag_m_sys_SharedMap_clear), new ast::ConstVoid, {});
		ast.mk_method(mut::ANY, map_cls, "delete", FN(ag_m_sys_SharedMap_delete), opt_shared_to_val_res, { ast.get_shared(key_cls) });
		ast.mk_method(mut::ANY, map_cls, "getAt", FN(ag_m_sys_SharedMap_getAt), opt_shared_to_val_res, { ast.get_shared(key_cls) });
		ast.mk_method(mut::ANY, map_cls, "setAt", FN(ag_m_sys_SharedMap_setAt), opt_shared_to_val_res, { ast.get_shared(key_cls), ast.get_shared(val_cls) });
		ast.mk_method(mut::ANY, map_cls, "keyAt", FN(ag_m_sys_SharedMap_keyAt), opt_shared_to_key_res, { ast.tp_int64() });
		ast.mk_method(mut::ANY, map_cls, "valAt", FN(ag_m_sys_SharedMap_valAt), opt_shared_to_val_res, { ast.tp_int64() });
	}
	{
		auto map_cls = ast.mk_class("WeakMap", {
			ast.mk_field("_buckets", new ast::ConstInt64),
			ast.mk_field("_capacity", new ast::ConstInt64),
			ast.mk_field("_size", new ast::ConstInt64) });
		auto key_cls = add_class_param(map_cls, "K");
		auto val_cls = add_class_param(map_cls, "V");
		auto weak_to_val_res = make_ptr_result(new ast::MkWeakOp, val_cls);
		auto opt_shared_to_key_res = make_opt_result(
			make_ptr_result(new ast::FreezeOp, key_cls));
		ast.mk_method(mut::ANY, map_cls, "size", FN(ag_m_sys_WeakMap_size), new ast::ConstInt64, {});
		ast.mk_method(mut::ANY, map_cls, "capacity", FN(ag_m_sys_WeakMap_capacity), new ast::ConstInt64, {});
		ast.mk_method(mut::ANY, map_cls, "clear", FN(ag_m_sys_WeakMap_clear), new ast::ConstVoid, {});
		ast.mk_method(mut::ANY, map_cls, "delete", FN(ag_m_sys_WeakMap_delete), weak_to_val_res, { ast.get_shared(key_cls) });
		ast.mk_method(mut::ANY, map_cls, "getAt", FN(ag_m_sys_WeakMap_getAt), weak_to_val_res, { ast.get_shared(key_cls) });
		ast.mk_method(mut::ANY, map_cls, "setAt", FN(ag_m_sys_WeakMap_setAt), weak_to_val_res, { ast.get_shared(key_cls), ast.get_weak(val_cls) });
		ast.mk_method(mut::ANY, map_cls, "keyAt", FN(ag_m_sys_WeakMap_keyAt), opt_shared_to_key_res, { ast.tp_int64() });
		ast.mk_method(mut::ANY, map_cls, "valAt", FN(ag_m_sys_WeakMap_valAt), weak_to_val_res, { ast.tp_int64() });
	}
	ast.mk_fn("getParent", FN(ag_fn_sys_getParent), opt_ref_to_object, { ast.get_conform_ref(ast.object) });
	ast.mk_fn("log", FN(ag_fn_sys_log), new ast::ConstVoid, { ast.get_conform_ref(ast.string_cls) });
	ast.mk_fn("hash", FN(ag_fn_sys_hash), new ast::ConstInt64, { ast.get_shared(ast.object) });
	ast.mk_fn("nowMs", FN(ag_fn_sys_nowMs), new ast::ConstInt64, {});
	ast.mk_fn("terminate", FN(ag_fn_sys_terminate), new ast::Break, { ast.tp_int64() });
	ast.mk_fn("setMainObject", FN(ag_fn_sys_setMainObject), new ast::ConstVoid, { ast.tp_optional(ast.get_ref(ast.object))});
	ast.mk_fn("weakExists", FN(ag_fn_sys_weakExists), new ast::ConstBool, { ast.get_weak(ast.object) });
	ast.mk_fn("powDbl", FN(ag_fn_sys_powDbl), new ast::ConstDouble, { ast.tp_double(), ast.tp_double() });
	ast.mk_fn("log10Dbl", FN(ag_fn_sys_log10Dbl), new ast::ConstDouble, { ast.tp_double() });
	ast.mk_fn("postTimer", FN(ag_fn_sys_postTimer), new ast::ConstVoid, {
		ast.tp_int64(),
		ast.tp_delegate({ ast.tp_void() })
	});
	{
		auto thread = ast.mk_class("Thread", {
			ast.mk_field("_internal", new ast::ConstInt64) });
		auto param = add_class_param(thread, "R");
		auto start = ast.mk_method(mut::MUTATING, thread, "start", FN(ag_m_sys_Thread_start), nullptr, { ast.get_ref(param) });
		make_factory(start);
		ast.mk_method(mut::MUTATING, thread, "root", FN(ag_m_sys_Thread_root), make_ptr_result(new ast::MkWeakOp, param), {});
	}

	ast.platform_exports.insert({
		{ "ag_init", FN(ag_init) },
		{ "ag_copy", FN(ag_copy) },
		{ "ag_copy_object_field", FN(ag_copy_object_field) },
		{ "ag_copy_weak_field", FN(ag_copy_weak_field) },
		{ "ag_allocate_obj", FN(ag_allocate_obj) },
		{ "ag_mk_weak", FN(ag_mk_weak) },
		{ "ag_deref_weak", FN(ag_deref_weak) },
		{ "ag_reg_copy_fixer", FN(ag_reg_copy_fixer) },
		{ "ag_retain_own", FN(ag_retain_own) },
		{ "ag_retain_own_nn", FN(ag_retain_own_nn) },
		{ "ag_retain_shared", FN(ag_retain_shared) },
		{ "ag_retain_shared_nn", FN(ag_retain_shared_nn) },
		{ "ag_retain_weak", FN(ag_retain_weak) },
		{ "ag_release_own", FN(ag_release_own) },
		{ "ag_release_own_nn", FN(ag_release_own_nn) },
		{ "ag_release_shared", FN(ag_release_shared) },
		{ "ag_release_shared_nn", FN(ag_release_shared_nn) },
		{ "ag_release_pin", FN(ag_release_pin) },
		{ "ag_release_weak", FN(ag_release_weak) },
		{ "ag_dispose_obj", FN(ag_dispose_obj) },
		{ "ag_set_parent", FN(ag_set_parent) },
		{ "ag_splice", FN(ag_splice) },
		{ "ag_freeze", FN(ag_freeze) },
		{ "ag_eq_mut", FN(ag_eq_mut) },
		{ "ag_eq_shared", FN(ag_eq_shared) },
		{ "ag_unlock_thread_queue", FN(ag_unlock_thread_queue) }, // used in trampoline
		{ "ag_get_thread_param", FN(ag_get_thread_param) }, // used in trampoline
		{ "ag_prepare_post_from_ag", FN(ag_prepare_post_from_ag) }, // used in post~message
		{ "ag_post_param_from_ag", FN(ag_post_param_from_ag) }, // used in post~message
		{ "ag_post_weak_param_from_ag", FN(ag_post_weak_param_from_ag) }, // used in post~message
		{ "ag_post_own_param_from_ag", FN(ag_post_own_param_from_ag) }, // used in post~message
		{ "ag_handle_main_thread", FN(ag_handle_main_thread) },

		{ "ag_copy_sys_Blob", FN(ag_copy_sys_Blob) },
		{ "ag_dtor_sys_Blob", FN(ag_dtor_sys_Blob) },
		{ "ag_visit_sys_Blob", FN(ag_visit_sys_Blob) },
		{ "ag_copy_sys_Map", FN(ag_copy_sys_Map) },
		{ "ag_dtor_sys_Map",FN(ag_dtor_sys_Map) },
		{ "ag_visit_sys_Map", FN(ag_visit_sys_Map) },
		{ "ag_copy_sys_SharedMap", FN(ag_copy_sys_SharedMap) },
		{ "ag_dtor_sys_SharedMap",FN(ag_dtor_sys_SharedMap) },
		{ "ag_visit_sys_SharedMap", FN(ag_visit_sys_SharedMap) },
		{ "ag_copy_sys_WeakMap", FN(ag_copy_sys_WeakMap) },
		{ "ag_dtor_sys_WeakMap",FN(ag_dtor_sys_WeakMap) },
		{ "ag_visit_sys_WeakMap", FN(ag_visit_sys_WeakMap) },
		{ "ag_copy_sys_Array", FN(ag_copy_sys_Array) },
		{ "ag_dtor_sys_Array",FN(ag_dtor_sys_Array) },
		{ "ag_visit_sys_Array",FN(ag_visit_sys_Array) },
		{ "ag_copy_sys_SharedArray", FN(ag_copy_sys_SharedArray) },
		{ "ag_dtor_sys_SharedArray",FN(ag_dtor_sys_SharedArray) },
		{ "ag_visit_sys_SharedArray",FN(ag_visit_sys_SharedArray) },
		{ "ag_copy_sys_WeakArray", FN(ag_copy_sys_WeakArray) },
		{ "ag_dtor_sys_WeakArray", FN(ag_dtor_sys_WeakArray) },
		{ "ag_visit_sys_WeakArray", FN(ag_visit_sys_WeakArray) },
		{ "ag_copy_sys_Thread", FN(ag_copy_sys_Thread) },
		{ "ag_dtor_sys_Thread", FN(ag_dtor_sys_Thread) },
		{ "ag_visit_sys_Thread", FN(ag_visit_sys_Thread) } });
}
