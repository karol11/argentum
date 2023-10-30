#ifndef AK_SHARED_MAP_H_
#define AK_SHARED_MAP_H_

#include "map-base.h"

#ifdef __cplusplus
extern "C" {
#endif

    int64_t   ag_m_sys_SharedMap_size(AgMap* map);
    int64_t   ag_m_sys_SharedMap_capacity(AgMap* map);
    void      ag_m_sys_SharedMap_clear(AgMap* map);
    AgObject* ag_m_sys_SharedMap_getAt(AgMap* map, AgObject* key);                  // returns ?*T
    AgObject* ag_m_sys_SharedMap_setAt(AgMap* map, AgObject* key, AgObject* value); // returns previous object as ?*T
    AgObject* ag_m_sys_SharedMap_delete(AgMap* map, AgObject* key);                 // returns previous object as ?*T
    AgObject* ag_m_sys_SharedMap_keyAt(AgMap* map, uint64_t index);
    AgObject* ag_m_sys_SharedMap_valAt(AgMap* map, uint64_t index);

    void      ag_copy_sys_SharedMap(void* dst, void* src);
    void      ag_dtor_sys_SharedMap(void* map);
    void      ag_visit_sys_SharedMap(
        AgMap* map,
        void    (*visitor)(void*, int, void*),
        void* ctx);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif // AK_SHARED_MAP_H_
