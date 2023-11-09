#include "weak-map.h"

int64_t ag_m_sys_WeakMap_size(AgMap* map) {
    return map->size;
}

int64_t ag_m_sys_WeakMap_capacity(AgMap* map) {
    return map->capacity;
}

static void val_weak_diposer(AgMapVal v) {
    ag_release_weak(v.weak_val);
}

void ag_m_sys_WeakMap_clear(AgMap* map) {
    ag_map_clear(map, val_weak_diposer);
}

AgWeak* ag_m_sys_WeakMap_getAt(AgMap* map, AgObject* key) {
    size_t i = ag_map_find_index(map, key, ag_fn_sys_hash(key));
    if (i == ~0u)
        return 0;
    AgWeak* r = map->buckets[i].val.weak_val;
    ag_retain_weak(r);
    return r;
}

AgWeak* ag_m_sys_WeakMap_setAt(AgMap* map, AgObject* key, AgWeak* value) {
    ag_retain_weak(value);
    return ag_map_set_at(map, key, (AgMapVal) { .weak_val = value }).weak_val;
}

AgWeak* ag_m_sys_WeakMap_delete(AgMap* map, AgObject* key) {
    return ag_map_delete(map, key).weak_val;
}

AgObject* ag_m_sys_WeakMap_keyAt(AgMap* map, uint64_t index) {
    return ag_map_key_at(map, index);
}

AgWeak* ag_m_sys_WeakMap_valAt(AgMap* map, uint64_t index) {
    if (index >= map->capacity || !map->buckets[index].key)
        return 0;
    AgWeak* r = map->buckets[index].val.weak_val;
    ag_retain_weak(r);
    return r;
}

void ag_copy_sys_WeakMap(void* dst, void* src) {
    AG_MAP_COPY(ag_copy_weak_field((void**)&i->val.weak_val, i->val.weak_val))
}

void ag_dtor_sys_WeakMap(void* map) {
    ag_m_sys_WeakMap_clear((AgMap*)map);
}

void ag_visit_sys_WeakMap(
    AgMap* map,
    void   (*visitor)(void*, int, void*),
    void* ctx)
{
    AG_MAP_VISIT(if (i->val.weak_val) visitor(i->val.weak_val, AG_VISIT_WEAK, ctx))
}
