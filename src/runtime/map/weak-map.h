#ifndef AK_WEAK_MAP_H_
#define AK_WEAK_MAP_H_

#include "map-base.h"

#ifdef __cplusplus
extern "C" {
#endif

    int64_t   ag_m_sys_WeakMap_size(AgMap* map);
    int64_t   ag_m_sys_WeakMap_capacity(AgMap* map);
    void      ag_m_sys_WeakMap_clear(AgMap* map);
    AgWeak*   ag_m_sys_WeakMap_getAt(AgMap* map, AgObject* key);                  // returns &T
    AgWeak*   ag_m_sys_WeakMap_setAt(AgMap* map, AgObject* key, AgWeak* value);   // returns previous object as &T
    AgWeak*   ag_m_sys_WeakMap_delete(AgMap* map, AgObject* key);                 // returns previous object as &T
    AgObject* ag_m_sys_WeakMap_keyAt(AgMap* map, uint64_t index);
    AgWeak*   ag_m_sys_WeakMap_valAt(AgMap* map, uint64_t index);

    void      ag_copy_sys_WeakMap(void* dst, void* src);
    void      ag_dtor_sys_WeakMap(void* map);
    void      ag_visit_sys_WeakMap(
        AgMap* map,
        void    (*visitor)(void*, int, void*),
        void* ctx);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif // AK_WEAK_MAP_H_
