#ifndef AG_BLOB_H_
#define AG_BLOB_H_

#include "runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

// TODO: move AgBlob definition here after String separation from runtime.h

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

#ifdef __cplusplus
}  // extern "C"
#endif

#endif // AG_BLOB_H_
