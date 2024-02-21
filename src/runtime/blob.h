#ifndef AG_BLOB_H_
#define AG_BLOB_H_

#include "runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	AgObject head;
	uint64_t bytes_count;
	int8_t* bytes;
} AgBlob;

void    ag_copy_sys_Blob         (AgBlob* dst, AgBlob* src);
void    ag_dtor_sys_Blob         (AgBlob* ptr);
void    ag_visit_sys_Blob        (void* ptr, void(*visitor)(void*, int, void*), void* ctx);

int64_t   ag_m_sys_Blob_capacity   (AgBlob* b);
void      ag_m_sys_Blob_insert     (AgBlob* b, uint64_t index, uint64_t bytes_count);
void      ag_m_sys_Blob_delete     (AgBlob* b, uint64_t index, uint64_t count);
bool      ag_m_sys_Blob_copy       (AgBlob* dst, uint64_t dst_index, AgBlob* src, uint64_t src_index, uint64_t bytes);
int64_t   ag_m_sys_Blob_get8At     (AgBlob* b, uint64_t index);
void      ag_m_sys_Blob_set8At     (AgBlob* b, uint64_t index, int64_t val);
int64_t   ag_m_sys_Blob_get16At    (AgBlob* b, uint64_t index);
void      ag_m_sys_Blob_set16At    (AgBlob* b, uint64_t index, int64_t val);
int64_t   ag_m_sys_Blob_get32At    (AgBlob* b, uint64_t index);
void      ag_m_sys_Blob_set32At    (AgBlob* b, uint64_t index, int64_t val);
int64_t   ag_m_sys_Blob_get64At    (AgBlob* b, uint64_t index);
void      ag_m_sys_Blob_set64At    (AgBlob* b, uint64_t index, int64_t val);
AgString* ag_m_sys_Blob_mkStr      (AgBlob* b, int at, int count);
int64_t   ag_m_sys_Blob_putChAt    (AgBlob* b, int at, int codepoint);

void    ag_make_blob_fit          (AgBlob* b, size_t size);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif // AG_BLOB_H_
