#ifndef AG_WEAK_ARRAY_H_
#define AG_WEAK_ARRAY_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "runtime.h"

void      ag_copy_sys_WeakArray    (AgBlob* dst, AgBlob* src);
void      ag_dtor_sys_WeakArray    (AgBlob* ptr);
void      ag_visit_sys_WeakArray   (AgBlob* ptr, void(*visitor)(void*, int, void*), void* ctx);

AgWeak*   ag_m_sys_WeakArray_getAt (AgBlob* b, uint64_t index);
void      ag_m_sys_WeakArray_setAt (AgBlob* b, uint64_t index, AgWeak* val);
void      ag_m_sys_WeakArray_delete(AgBlob* b, uint64_t index, uint64_t count);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // AG_WEAK_ARRAY_H_
