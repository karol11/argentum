#ifndef AG_SHARED_ARRAY_H_
#define AG_SHARED_ARRAY_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "runtime.h"
#include "array/array-base.h"

void	  ag_copy_sys_SharedArray       (AgBaseArray* dst, AgBaseArray* src);
void      ag_dtor_sys_SharedArray       (AgBaseArray* ptr);
void      ag_visit_sys_SharedArray      (AgBaseArray* ptr, void(*visitor)(void*, int, void*), void* ctx);

int64_t   ag_m_sys_SharedArray_capacity (AgBaseArray* c);
void      ag_m_sys_SharedArray_insert   (AgBaseArray* c, uint64_t at, uint64_t items_count);
void      ag_m_sys_SharedArray_delete   (AgBaseArray* b, uint64_t index, uint64_t count);
bool      ag_m_sys_SharedArray_move     (AgBaseArray* c, uint64_t x, uint64_t y, uint64_t z);
AgObject* ag_m_sys_SharedArray_getAt    (AgBaseArray* b, uint64_t index);
void      ag_m_sys_SharedArray_setAt    (AgBaseArray* b, uint64_t index, AgObject* val);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // AG_SHARED_ARRAY_H_
