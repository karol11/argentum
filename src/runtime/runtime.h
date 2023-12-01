#ifndef AK_RUNTIME_H_
#define AK_RUNTIME_H_

#include <stdint.h>

#ifdef __cplusplus

extern "C" {

#else

typedef int bool;
#define true 1
#define false 0

#endif

#ifndef AG_ALLOC
#include <stdlib.h>
#define AG_ALLOC malloc
#define AG_FREE free
#endif

void* ag_alloc(size_t size);
void ag_free(void* data);

#ifdef NO_DEFAULT_LIB

void ag_zero_mem(void*, size_t);
void ag_memcpy(void*, void*, size_t);
void ag_memmove(void*, void*, size_t);

#else

#include <string.h>
#define ag_zero_mem(P, S) memset(P, 0, S)
#define ag_memcpy memcpy
#define ag_memmove memmove

#endif

//
// Tags in `parent` field
// When not in copy op, all not shared obj->wb pointers have to have 0b00 in two LSB bits
#define AG_F_PARENT ((uintptr_t) 1)
#define AG_IN_STACK 0
// Tags in `counter` field (only for shared objects)
#define AG_CTR_MT     ((uintptr_t) 1)
#define AG_CTR_WEAK   ((uintptr_t) 2)
#define AG_CTR_SHARED ((uintptr_t) 4)
#define AG_CTR_HASH   ((uintptr_t) 8)
#define AG_CTR_STEP   ((uintptr_t) 16)

typedef struct ag_thread_tag ag_thread;

#define AG_VISIT_OWN    0
#define AG_VISIT_WEAK   1
#define AG_VISIT_STRING_BUF   2

#define ag_not_null(OBJ) ((OBJ) && (size_t)(OBJ) >= 256)

// offset from `copy_ref_fields` to `vmt_size`
#define AG_VMT_FIELD_VMT_SIZE 4

typedef uint64_t(*ag_get_hash_fn_t)(void* ptr);
typedef bool(*ag_equals_fn_t)(void* a, void* b);

typedef struct {
	ag_get_hash_fn_t get_hash;
	ag_equals_fn_t   equals_to;
	void   (*copy_ref_fields)  (void* dst, void* src);
	void   (*dispose)          (void* ptr);
	void   (*visit)            (void* ptr,
								void(* visitor)(
									void*,  // field_ptr*
									int,    // type AG_VISIT_*
									void*), // ctx
								void* ctx);
	size_t instance_alloc_size;
	size_t vmt_size;
} AgVmt;

typedef void** (*ag_dispatcher_t) (uint64_t interface_and_method_ordinal);
typedef struct {
	ag_dispatcher_t dispatcher;
	uintptr_t       ctr_mt;      // number_of_owns_and_refs point here << 4 | 1 if mt
	uintptr_t       wb_p;        // pointer_to_weak_block || (pointer_to_parent|AG_F_PARENT)
} AgObject;

#ifdef AG_STANDALONE_COMPILER_MODE
void** ag_disp_sys_String(uint64_t interface_and_method_ordinal);
#else
extern ag_dispatcher_t ag_disp_sys_String;
#endif

typedef struct {
	AgObject*  target;
	uintptr_t  wb_ctr_mt;    // number_of_weaks pointing here << 4 | 1 if mt | 2 to indicate weak
	uintptr_t  org_pointer_to_parent;  // copy of obj->parent
	ag_thread* thread;       // pointer to ag_thread struct
} AgWeak;

typedef struct {
	AgObject head;
	uint64_t size;
	int64_t* data;
} AgBlob;

typedef struct {
	size_t counter_mt; // number_of_strings pointing here << 1 | 1 if mt
	char   data[1];
} AgStringBuffer;

typedef struct {
	AgObject        head;
	const char*     ptr;    // points to current char
	AgStringBuffer* buffer; // 0 for literals
} AgString;

typedef struct {
	AgObject              head;
	struct ag_thread_tag* thread;
} AgThread;

bool ag_leak_detector_ok();
uintptr_t ag_max_mem();

void ag_init();
//
// AgObject support
//
void      ag_release_own_nn     (AgObject* obj);
void      ag_release_own        (AgObject* obj);
void      ag_retain_own_nn      (AgObject* obj, AgObject* parent);
void      ag_retain_own         (AgObject* obj, AgObject* parent);
void      ag_set_parent         (AgObject* obj, AgObject* parent);
bool      ag_splice             (AgObject* object, AgObject* parent);  // checks if parent is not already in object hierarchy, sets parent, retains
AgObject* ag_copy               (AgObject* src);
AgObject* ag_freeze             (AgObject* src);
void      ag_release_pin_nn     (AgObject* obj);
void      ag_release_pin        (AgObject* obj);
void      ag_retain_pin_nn      (AgObject* obj);
void      ag_retain_pin         (AgObject* obj);
void      ag_release_shared     (AgObject* obj);
void      ag_release_shared_nn  (AgObject* obj);
void      ag_retain_shared      (AgObject* obj);
void      ag_retain_shared_nn   (AgObject* obj);
void      ag_dispose_obj        (AgObject* src);
AgObject* ag_allocate_obj       (size_t size);
AgObject* ag_copy_object_field  (AgObject* src, AgObject* parent);
void      ag_reg_copy_fixer     (AgObject* object, void (*fixer)(AgObject*));
bool      ag_eq_mut             (AgObject* a, AgObject* b);
bool      ag_eq_shared          (AgObject* a, AgObject* b);

AgObject* ag_fn_sys_getParent   (AgObject* obj);   // obj not null

// Checks if a weak target exists.
// Works cross-threads.
// Eventually consistent.
// I.e. sometimes can return true for lost objects.
bool      ag_fn_sys_weakExists  (AgWeak* w);

int64_t   ag_m_sys_Object_getHash (AgObject* obj);
bool      ag_m_sys_Object_equals  (AgObject* a, AgObject *b);

//
// AgWeak support
//
void      ag_copy_weak_field (void** dst, AgWeak* src);
void      ag_release_weak    (AgWeak* obj);
void      ag_retain_weak     (AgWeak* obj);
AgWeak*   ag_mk_weak         (AgObject* obj);
AgObject* ag_deref_weak      (AgWeak* w);

//
// AgString support
//
void      ag_copy_sys_String        (AgString* dst, AgString* src);
void      ag_dtor_sys_String        (AgString* str);
void      ag_visit_sys_String       (AgString* ptr, void(*visitor)(void*, int, void*), void* ctx);
int32_t   ag_m_sys_String_getCh     (AgString* s);
int32_t   ag_m_sys_String_peekCh    (AgString* s);
bool      ag_m_sys_String_fromBlob  (AgString* s, AgBlob* b, int at, int count);
int64_t   ag_m_sys_Blob_putChAt     (AgBlob* b, int at, int codepoint);
int64_t   ag_m_sys_String_getHash   (AgObject* obj);
bool      ag_m_sys_String_equals    (AgObject* a, AgObject* b);

static inline int64_t ag_getStringHash(const char* s) {
	int64_t r = 5381;
	for (; *s; s++)
		r = ((r << 5) + r) ^ *s;
	return r;
}
//
// System
//
void      ag_fn_sys_terminate     (int);
bool      ag_fn_sys_setMainObject (AgObject* root); // root must be not owned, returns true on success
void      ag_fn_sys_log           (AgString* s);
int64_t   ag_fn_sys_hash          (AgObject* s);

//
// Thread
//
void      ag_copy_sys_Thread      (AgThread* dst, AgThread* src);
void      ag_dtor_sys_Thread      (AgThread* ptr);
void      ag_visit_sys_Thread     (AgThread* ptr, void(*visitor)(void*, int, void*), void* ctx);
AgThread* ag_m_sys_Thread_start   (AgThread* th, AgObject* root);
AgWeak*   ag_m_sys_Thread_root    (AgThread* th);

//
// Cross-thread FFI interop
//
typedef void (*ag_fn)();

bool ag_fn_sys_postTimer(int64_t at, AgWeak* receiver, ag_fn fn);

typedef void (*ag_trampoline) (AgObject* self, ag_fn entry_point, ag_thread* thread);
// Trampoline is a function that reads parameters from the request queue and calls the actual function.
// Trampoline should:
// 1. call ag_get_thread_param to extract each param,
// 2. call ag_unlock_thread_queue
// 3. if (self != null) execute entry_point(self, params)
// 4. release params

// Posting async messages from FFI depends on what thread is this FFI function works on.
// // If it's an argentum thread, use:
// thread* t = ag_prepare_post_from_ag(receiver_obj, entry_point, trampoline, params_count);
// ag_post_*_param_from_ag...
ag_thread* ag_prepare_post_from_ag      (AgWeak* receiver, ag_fn fn, ag_trampoline tramp, size_t params_count);
void       ag_post_param_from_ag        (uint64_t param);
void       ag_post_weak_param_from_ag   (AgWeak* param);
void       ag_post_own_param_from_ag    (ag_thread* th, AgObject* param);

// If a call is originated from non-ag-thread (one created by FFI functions, or by external library - usual case), use:
// ag_thread* t = ag_prepare_post(ag_retain_weak(receiver_obj), trampoline, entry_point, params_count);
// if (t) {
//    ag_post_*_param...
//    ag_finalize_post();
// }
ag_thread* ag_prepare_post    (AgWeak* receiver, void* tramp, void* entry_point, int64_t params_count);
void       ag_post_param      (ag_thread* th, uint64_t param);
void       ag_post_own_param  (ag_thread* th, AgObject* param);
void       ag_post_weak_param (ag_thread* th, AgWeak* param);
void       ag_finalize_post   (ag_thread* th);
// Foreign function should put (using ag_put_thread_param) the same number of params in the same order
// as the trampoline function invoked on AG-thread is going to read with ag_get_thread_param.

// If a foreign function wants to store an object or a weak pointer in its own thread, it should call `ag_detach_*` while on ag thread.
// Detached objects can be passed as parameters to async messages, but they can't be receivers of async messages.
void       ag_detach_own      (AgObject*);
void       ag_detach_weak     (AgWeak*);

//trampoline api
uint64_t ag_get_thread_param    (ag_thread* th);
void     ag_unlock_thread_queue (ag_thread* th);

// Returns immutable shared tring
AgString* ag_make_str(const char* start, size_t size);

int ag_handle_main_thread();

#ifdef __cplusplus
}  // extern "C"
#endif

#endif // AK_RUNTIME_H_
