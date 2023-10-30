#ifndef AK_OWN_MAP_H_
#define AK_OWN_MAP_H_

#include "map-base.h"

#ifdef __cplusplus
extern "C" {
#endif

int64_t   ag_m_sys_Map_size(AgMap* map);
int64_t   ag_m_sys_Map_capacity(AgMap* map);
void      ag_m_sys_Map_clear(AgMap* map);
AgObject* ag_m_sys_Map_getAt(AgMap* map, AgObject* key);                  // returns ?T
AgObject* ag_m_sys_Map_setAt(AgMap* map, AgObject* key, AgObject* value); // returns previous object as ?T
AgObject* ag_m_sys_Map_delete(AgMap* map, AgObject* key);                 // returns previous object as ?T
AgObject* ag_m_sys_Map_keyAt(AgMap* map, uint64_t index);
AgObject* ag_m_sys_Map_valAt(AgMap* map, uint64_t index);

void      ag_copy_sys_Map(void* dst, void* src);
void      ag_dtor_sys_Map(void* map);
void      ag_visit_sys_Map(
             AgMap* map,
             void    (*visitor)(void*, int, void*),
             void* ctx);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif // AK_OWN_MAP_H_
