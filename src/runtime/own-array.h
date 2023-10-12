#ifndef AG_OWN_ARRAY_H_
#define AG_OWN_ARRAY_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "runtime.h"

void	  ag_copy_sys_Array       (AgBlob* dst, AgBlob* src);
void      ag_dtor_sys_Array       (AgBlob* ptr);
void      ag_visit_sys_Array      (AgBlob* ptr, void(*visitor)(void*, int, void*), void* ctx);

AgObject* ag_m_sys_Array_getAt    (AgBlob* b, uint64_t index);
void      ag_m_sys_Array_setAt    (AgBlob* b, uint64_t index, AgObject* val);
AgObject* ag_m_sys_Array_setOptAt (AgBlob* b, uint64_t index, AgObject* val);
void      ag_m_sys_Array_delete   (AgBlob* b, uint64_t index, uint64_t count);
bool      ag_m_sys_Array_spliceAt (AgBlob* b, uint64_t index, AgObject* val);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // AG_OWN_ARRAY_H_
