#ifndef AK_RUNTIME_H_
#define AK_RUNTIME_H_

#include <stdint.h>

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

typedef struct {
	void   (*copy_ref_fields)  (void* dst, void* src);
	void   (*dispose)          (void* ptr);
	size_t instance_alloc_size;
	size_t vmt_size;
} AgVmt;

typedef struct AgObject_tag {
	void**    (*dispatcher) (uint64_t interface_and_method_ordinal);
	uintptr_t counter;      // number_of_owns_and_refs point here
	uintptr_t wb_p;       // pointer_to_weak_block || (pointer_to_parent|AG_F_PARENT)
} AgObject;

typedef struct {
	AgObject* target;
	uintptr_t wb_counter;   // number_of_weaks pointing here
	uintptr_t org_pointer_to_parent;  // copy of obj->parent
} AgWeak;

typedef struct {
	size_t counter;
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

bool ag_leak_detector_ok();
uintptr_t ag_max_mem();

//
// AgObject support
//
void      ag_release_own        (AgObject* obj);
void      ag_retain_own         (AgObject* obj, AgObject* parent);
void      ag_set_parent         (AgObject* obj, AgObject* parent);
bool      ag_splice             (AgObject* object, AgObject* parent);  // checks if parent is not already in object hierarchy, sets parent, retains
AgObject* ag_copy               (AgObject* src);
AgObject* ag_freeze             (AgObject* src);
void      ag_release            (AgObject* obj);
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
AgWeak*   ag_mk_weak         (AgObject* obj);
AgObject* ag_deref_weak      (AgWeak* w);
void      ag_release_weak    (AgWeak* obj);

//
// AgString support
//
int32_t   ag_fn_sys_getCh           (AgString* s);
void      ag_copy_sys_String        (AgString* dst, AgString* src);
void      ag_dtor_sys_String        (AgString* str);
bool      ag_fn_sys_stringFromBlob  (AgString* s, AgBlob* b, int at, int count);
int64_t   ag_fn_sys_putCh           (AgBlob* b, int at, int codepoint);

//
// AgContainer support (both Blobs and Arrays)
//
int64_t ag_fn_sys_getSize          (AgBlob* b);
void    ag_fn_sys_insertItems      (AgBlob* b, uint64_t index, uint64_t count);
bool    ag_fn_sys_moveItems        (AgBlob* blob, uint64_t a, uint64_t b, uint64_t c);

//
// AgBlob support
//
int64_t ag_fn_sys_get8At         (AgBlob* b, uint64_t index);
void    ag_fn_sys_set8At         (AgBlob* b, uint64_t index, int64_t val);
int64_t ag_fn_sys_get16At        (AgBlob* b, uint64_t index);
void    ag_fn_sys_set16At        (AgBlob* b, uint64_t index, int64_t val);
int64_t ag_fn_sys_get32At        (AgBlob* b, uint64_t index);
void    ag_fn_sys_set32At        (AgBlob* b, uint64_t index, int64_t val);
int64_t ag_fn_sys_get64At        (AgBlob* b, uint64_t index);
void    ag_fn_sys_set64At        (AgBlob* b, uint64_t index, int64_t val);
bool    ag_fn_sys_copyBytes      (AgBlob* dst, uint64_t dst_index, AgBlob* src, uint64_t src_index, uint64_t bytes);
void    ag_copy_sys_Container    (AgBlob* dst, AgBlob* src);
void    ag_copy_sys_Blob         (AgBlob* dst, AgBlob* src);
void    ag_fn_sys_deleteBytes    (AgBlob* b, uint64_t index, uint64_t count);
void    ag_dtor_sys_Container    (AgBlob* ptr);
void    ag_dtor_sys_Blob         (AgBlob* ptr);
void    ag_make_blob_fit         (AgBlob* b, size_t required_size);

//
// AgArray support
//
AgObject* ag_fn_sys_getAtArray    (AgBlob* b, uint64_t index);
AgObject* ag_fn_sys_setAtArray    (AgBlob* b, uint64_t index, AgObject* val);
void      ag_fn_sys_setOptAt      (AgBlob* b, uint64_t index, AgObject* val);
bool      ag_fn_sys_spliceAt      (AgBlob* b, uint64_t index, AgObject* val);
void	  ag_copy_sys_Array       (AgBlob* dst, AgBlob* src);
void      ag_dtor_sys_Array       (AgBlob* ptr);
void      ag_fn_sys_deleteItems   (AgBlob* b, uint64_t index, uint64_t count);

//
// AgWeakArray support
//
AgWeak*   ag_fn_sys_getAtWeakArray (AgBlob* b, uint64_t index);
void      ag_fn_sys_setAtWeakArray (AgBlob* b, uint64_t index, AgWeak* val);
void      ag_copy_sys_WeakArray    (AgBlob* dst, AgBlob* src);
void      ag_dtor_sys_WeakArray    (AgBlob* ptr);
void      ag_fn_sys_deleteWeaks    (AgBlob* b, uint64_t index, uint64_t count);

void      ag_fn_sys_terminate (int);
void      ag_fn_sys_log       (AgString* s);
int64_t   ag_fn_sys_readFile  (AgString* name, AgBlob* content);  // returns bytes read or -1
bool      ag_fn_sys_writeFile (AgString* name, int64_t at, int64_t byte_size, AgBlob* content);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif // AK_RUNTIME_H_
