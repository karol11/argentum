#ifndef AK_RUNTIME_H_
#define AK_RUNTIME_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#else
typedef int bool;
#endif

//
// Tags in `counter` field
//
#define AG_CTR_WEAKLESS ((uintptr_t)    1)
#define AG_CTR_FROZEN   ((uintptr_t)    2)
#define AG_CTR_STEP     ((uintptr_t) 0x10)

typedef struct {
	void   (*copy_ref_fields)  (void* dst, void* src);
	void   (*dispose)          (void* ptr);
	size_t instance_alloc_size;
	size_t vmt_size;
} AgVmt;

typedef struct {
	void**    (*dispatcher) (uint64_t interface_and_method_ordinal);
	uintptr_t counter;  // pointer_to_weak_block || (number_of_owns_and_refs * AG_CTR_STEP | AG_CTR_* flags)
} AgHead;

typedef void AgObject;

typedef struct {
	AgObject* target;
	int64_t   wb_counter;   // number_of_weaks pointing here
	int64_t   org_counter;  // copy of obj->counter
} AgWeak;

typedef struct {
	int  counter;
	char data[1];
} AgStringBuffer;

typedef struct {
	const char*     ptr;    // points to current char
	AgStringBuffer* buffer; // 0 for literals
} AgString;

typedef struct {
	uint64_t size;
	int64_t* data;
} AgBlob;

bool ag_leak_detector_ok();
uintptr_t ag_max_mem_ok();
//
// AgObject support
//
void      ag_release  (AgObject* obj);
AgObject* ag_retain   (AgObject* obj);
AgObject* ag_copy     (AgObject* src);
AgObject* ag_allocate_obj      (size_t size);
AgObject* ag_copy_object_field (AgObject* src);
void      ag_make_shared       (AgObject* obj);
void      ag_reg_copy_fixer    (AgObject* object, void (*fixer)(AgObject*));

//
// AgWeak support
//
AgWeak*   ag_retain_weak     (AgWeak* w);
void      ag_release_weak    (AgWeak* w);
void      ag_copy_weak_field (void** dst, AgWeak* src);
AgWeak*   ag_mk_weak         (AgObject* obj);
AgObject* ag_deref_weak         (AgWeak* w);

//
// AgString support
//
int32_t   ag_get_char           (AgString* s);
void      ag_copy_str_fields    (AgString* dst, AgString* src);
void      ag_dispose_str_fields (AgString* str);
bool      ag_to_str             (AgString* s, AgBlob* b, int at, int count);
int64_t   ag_put_ch             (AgBlob* b, int at, int codepoint);

//
// AgContainer support (both Blobs and Arrays)
//
int64_t ag_get_size     (AgBlob* b);
void    ag_insert_items (AgBlob* b, uint64_t index, uint64_t count);
bool    ag_move_items   (AgBlob* blob, uint64_t a, uint64_t b, uint64_t c);

//
// AgBlob support
//
int64_t ag_get_at     (AgBlob* b, uint64_t index);
void    ag_set_at     (AgBlob* b, uint64_t index, int64_t val);
int64_t ag_get_i8_at  (AgBlob* b, uint64_t index);
void    ag_set_i8_at  (AgBlob* b, uint64_t index, int64_t val);
int64_t ag_get_i16_at (AgBlob* b, uint64_t index);
void    ag_set_i16_at (AgBlob* b, uint64_t index, int64_t val);
int64_t ag_get_i32_at (AgBlob* b, uint64_t index);
void    ag_set_i32_at (AgBlob* b, uint64_t index, int64_t val);
bool    ag_blob_copy  (AgBlob* dst, uint64_t dst_index, AgBlob* src, uint64_t src_index, uint64_t bytes);
void    ag_copy_blob_fields  (AgBlob* dst, AgBlob* src);
void    ag_delete_blob_items (AgBlob* b, uint64_t index, uint64_t count);
void    ag_dispose_blob      (AgBlob* ptr);

//
// AgArray support
//
AgObject* ag_get_ref_at         (AgBlob* b, uint64_t index);
void      ag_set_own_at         (AgBlob* b, uint64_t index, AgObject* val);
void	  ag_copy_array_fields  (AgBlob* dst, AgBlob* src);
void      ag_dispose_array      (AgBlob* ptr);
void      ag_delete_array_items (AgBlob* b, uint64_t index, uint64_t count);

//
// AgWeakArray support
//
AgWeak*   ag_get_weak_at (AgBlob* b, uint64_t index);
void      ag_set_weak_at (AgBlob* b, uint64_t index, AgWeak* val);
void      ag_copy_weak_array_fields  (AgBlob* dst, AgBlob* src);
void      ag_dispose_weak_array      (AgBlob* ptr);
void      ag_delete_weak_array_items (AgBlob* b, uint64_t index, uint64_t count);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif // AK_RUNTIME_H_
