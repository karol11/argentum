#ifndef AG_SHARED_ARRAY_H_
#define AG_SHARED_ARRAY_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "runtime/runtime.h"

void	  ag_copy_sys_SharedArray       (AgBlob* dst, AgBlob* src);
void      ag_dtor_sys_SharedArray       (AgBlob* ptr);
void      ag_visit_sys_SharedArray      (AgBlob* ptr, void(*visitor)(void*, int, void*), void* ctx);

AgObject*   ag_m_sys_SharedArray_getAt  (AgBlob* b, uint64_t index);
void        ag_m_sys_SharedArray_setAt  (AgBlob* b, uint64_t index, AgObject* val);
void        ag_m_sys_SharedArray_delete (AgBlob* b, uint64_t index, uint64_t count);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // AG_SHARED_ARRAY_H_
