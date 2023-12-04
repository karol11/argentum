#ifndef AG_OWN_ARRAY_H_
#define AG_OWN_ARRAY_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "runtime/runtime.h"
#include "runtime/array/array-base.h"

void	  ag_copy_sys_Array       (AgBaseArray* dst, AgBaseArray* src);
void      ag_dtor_sys_Array       (AgBaseArray* ptr);
void      ag_visit_sys_Array      (AgBaseArray* ptr, void(*visitor)(void*, int, void*), void* ctx);

int64_t   ag_m_sys_Array_capacity (AgBaseArray* c);
void      ag_m_sys_Array_insert   (AgBaseArray* c, uint64_t at, uint64_t items_count);
void      ag_m_sys_Array_delete   (AgBaseArray* b, uint64_t index, uint64_t count);
AgObject* ag_m_sys_Array_getAt    (AgBaseArray* b, uint64_t index);
void      ag_m_sys_Array_setAt    (AgBaseArray* b, uint64_t index, AgObject* val);
AgObject* ag_m_sys_Array_setOptAt (AgBaseArray* b, uint64_t index, AgObject* val);
bool      ag_m_sys_Array_spliceAt (AgBaseArray* b, uint64_t index, AgObject* val);

// Moves elements within a container
// x-y-z splits array in 4 spans  0-x, x-y, y-z, x-count
// `move` swaps x-y and y-z spans
bool      ag_m_sys_Array_move     (AgBaseArray* c, uint64_t x, uint64_t y, uint64_t z);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // AG_OWN_ARRAY_H_
