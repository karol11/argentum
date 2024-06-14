#ifndef AG_WEAK_ARRAY_H_
#define AG_WEAK_ARRAY_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "runtime.h"
#include "array/array-base.h"

void    ag_copy_sys_WeakArray  (AgBaseArray* dst, AgBaseArray* src);
void    ag_dtor_sys_WeakArray  (AgBaseArray* ptr);
void    ag_visit_sys_WeakArray (AgBaseArray* ptr, void(*visitor)(void*, int, void*), void* ctx);

int64_t ag_m_sys_WeakArray_capacity (AgBaseArray* c);
void    ag_m_sys_WeakArray_insert   (AgBaseArray* c, uint64_t at, uint64_t items_count);
void    ag_m_sys_WeakArray_delete   (AgBaseArray* b, uint64_t index, uint64_t count);
bool    ag_m_sys_WeakArray_move     (AgBaseArray* c, uint64_t x, uint64_t y, uint64_t z);
AgWeak* ag_m_sys_WeakArray_getAt    (AgBaseArray* b, uint64_t index);
void    ag_m_sys_WeakArray_setAt    (AgBaseArray* b, uint64_t index, AgWeak* val);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // AG_WEAK_ARRAY_H_
