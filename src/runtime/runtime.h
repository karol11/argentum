#ifndef AK_RUNTIME_H_
#define AK_RUNTIME_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#else
typedef int bool;
#endif

//
// Tags in `parent` field
// When not in copy op, all not shared obj->wb pointers have to have 0b00 in two LSB bits
#define AG_F_PARENT ((uintptr_t) 1)
#define AG_IN_STACK 0
#define AG_SHARED   ((uintptr_t) 2)
// Tags in `counter` field (only for shared or conform pointers)
#define AG_CTR_MT ((uintptr_t) 1)
#define AG_CTR_WEAK ((uintptr_t) 2)
#define AG_CTR_STEP ((uintptr_t) 16)

typedef struct ag_thread_tag ag_thread;

#define AG_VISIT_OWN    0
#define AG_VISIT_WEAK   1
#define AG_VISIT_STRING_BUF   2

#define AG_VMT_FIELD_COPY      0 
#define AG_VMT_FIELD_DISPOSE   1 
#define AG_VMT_FIELD_VISIT     2
#define AG_VMT_FIELD_INST_SIZE 3
#define AG_VMT_FIELD_VMT_SIZE  4

typedef struct {
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

typedef struct {
	void**    (*dispatcher) (uint64_t interface_and_method_ordinal);
	uintptr_t ctr_mt;      // number_of_owns_and_refs point here << 2 | 1 if mt
	uintptr_t wb_p;        // pointer_to_weak_block || (pointer_to_parent|AG_F_PARENT)
} AgObject;

typedef struct {
	AgObject*  target;
	uintptr_t  wb_ctr_mt;    // number_of_weaks pointing here << 2 | 1 if mt | 2 to indicate weak
	uintptr_t  org_pointer_to_parent;  // copy of obj->parent
	ag_thread* thread;       // pointer to ag_thread struct
} AgWeak;

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
	AgObject head;
	uint64_t size;
	int64_t* data;
} AgBlob;

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
void      ag_release_own        (AgObject* obj);
void      ag_retain_own         (AgObject* obj, AgObject* parent);
void      ag_set_parent         (AgObject* obj, AgObject* parent);
bool      ag_splice             (AgObject* object, AgObject* parent);  // checks if parent is not already in object hierarchy, sets parent, retains
AgObject* ag_copy               (AgObject* src);
AgObject* ag_freeze             (AgObject* src);
void      ag_release_pin        (AgObject* obj);
// void   ag_retain_pin         (AgObject* obj); // inlined
void      ag_release_shared     (AgObject* obj);
void      ag_retain_shared      (AgObject* obj);
void      ag_dispose_obj        (AgObject* src);
AgObject* ag_allocate_obj       (size_t size);
AgObject* ag_copy_object_field  (AgObject* src, AgObject* parent);
void      ag_fn_sys_make_shared (AgObject* obj);
void      ag_reg_copy_fixer     (AgObject* object, void (*fixer)(AgObject*));
AgObject* ag_fn_sys_getParent   (AgObject* obj);   // obj not null

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
bool      ag_m_sys_String_fromBlob  (AgString* s, AgBlob* b, int at, int count);
int64_t   ag_m_sys_Blob_putChAt     (AgBlob* b, int at, int codepoint);

//
// AgContainer support (both Blobs and Arrays)
//
int64_t ag_m_sys_Container_capacity    (AgBlob* b);
void    ag_m_sys_Container_insertItems (AgBlob* b, uint64_t index, uint64_t count);
bool    ag_m_sys_Container_moveItems   (AgBlob* blob, uint64_t a, uint64_t b, uint64_t c);

//
// AgBlob support
//
void    ag_copy_sys_Container    (AgBlob* dst, AgBlob* src);
void    ag_dtor_sys_Container    (AgBlob* ptr);
void    ag_visit_sys_Container   (void* ptr, void(*visitor)(void*, int, void*), void* ctx);
void    ag_copy_sys_Blob         (AgBlob* dst, AgBlob* src);
void    ag_dtor_sys_Blob         (AgBlob* ptr);
void    ag_visit_sys_Blob        (void* ptr, void(*visitor)(void*, int, void*), void* ctx);
int64_t ag_m_sys_Blob_get8At     (AgBlob* b, uint64_t index);
void    ag_m_sys_Blob_set8At     (AgBlob* b, uint64_t index, int64_t val);
int64_t ag_m_sys_Blob_get16At    (AgBlob* b, uint64_t index);
void    ag_m_sys_Blob_set16At    (AgBlob* b, uint64_t index, int64_t val);
int64_t ag_m_sys_Blob_get32At    (AgBlob* b, uint64_t index);
void    ag_m_sys_Blob_set32At    (AgBlob* b, uint64_t index, int64_t val);
int64_t ag_m_sys_Blob_get64At    (AgBlob* b, uint64_t index);
void    ag_m_sys_Blob_set64At    (AgBlob* b, uint64_t index, int64_t val);
bool    ag_m_sys_Blob_copyBytesTo(AgBlob* dst, uint64_t dst_index, AgBlob* src, uint64_t src_index, uint64_t bytes);
void    ag_m_sys_Blob_deleteBytes(AgBlob* b, uint64_t index, uint64_t count);
void    ag_make_blob_fit         (AgBlob* b, size_t required_size);

//
// AgArray support
//
void	  ag_copy_sys_Array       (AgBlob* dst, AgBlob* src);
void      ag_dtor_sys_Array       (AgBlob* ptr);
void      ag_visit_sys_Array      (AgBlob* ptr, void(*visitor)(void*, int, void*), void* ctx);
AgObject* ag_m_sys_Array_getAt    (AgBlob* b, uint64_t index);
AgObject* ag_m_sys_Array_setAt    (AgBlob* b, uint64_t index, AgObject* val);
void      ag_m_sys_Array_setOptAt (AgBlob* b, uint64_t index, AgObject* val);
bool      ag_m_sys_Array_spliceAt (AgBlob* b, uint64_t index, AgObject* val);
void      ag_m_sys_Array_delete   (AgBlob* b, uint64_t index, uint64_t count);

//
// AgWeakArray support
//
void      ag_copy_sys_WeakArray    (AgBlob* dst, AgBlob* src);
void      ag_dtor_sys_WeakArray    (AgBlob* ptr);
void      ag_visit_sys_WeakArray   (AgBlob* ptr, void(*visitor)(void*, int, void*), void* ctx);
AgWeak*   ag_m_sys_WeakArray_getAt (AgBlob* b, uint64_t index);
void      ag_m_sys_WeakArray_setAt (AgBlob* b, uint64_t index, AgWeak* val);
void      ag_m_sys_WeakArray_delete(AgBlob* b, uint64_t index, uint64_t count);

//
// System
//
void      ag_fn_sys_terminate     (int);
bool      ag_fn_sys_setMainObject (AgObject* root); // root must be not owned, returns true on success
void      ag_fn_sys_log           (AgString* s);

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

// Foreign function that wants to call a callback from a random thread should:
// 1. call ag_prepare_post_message and check its result for null (null means receiver is no longer exists)
// 2. call ag_put_thread_param for each 64-bit parameter (some parameters, like optInt and delegate require two ag_put_thread_param calls).
// 3. call ag_finalize_post_message
// Foreign function should put (using ag_put_thread_param) the same number of params in the same order
// as the trampoline function invoked on AG-thread is going to read with ag_get_thread_param.
ag_thread* ag_prepare_post_message      (AgWeak* receiver, ag_fn fn, ag_trampoline tramp, size_t params_count);
void       ag_put_thread_param          (ag_thread* th, uint64_t param);
void       ag_put_thread_param_weak_ptr (ag_thread* th, AgWeak* param);
void       ag_put_thread_param_own_ptr  (ag_thread* th, AgObject* param);
void       ag_finalize_post_message     (ag_thread* th);

//trampoline api
uint64_t ag_get_thread_param    (ag_thread* th);
void     ag_unlock_thread_queue (ag_thread* th);

int ag_handle_main_thread();

#ifdef __cplusplus
}  // extern "C"
#endif

#endif // AK_RUNTIME_H_
