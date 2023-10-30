#include "shared-map.h"

int64_t ag_m_sys_SharedMap_size(AgMap* map) {
    return map->size;
}

int64_t ag_m_sys_SharedMap_capacity(AgMap* map) {
    return map->capacity;
}

static void val_shared_diposer(AgMapVal v) {
    ag_release_shared(v.ptr_val);
}

void ag_m_sys_SharedMap_clear(AgMap* map) {
    ag_map_clear(map, val_shared_diposer);
}

AgObject* ag_m_sys_SharedMap_getAt(AgMap* map, AgObject* key) {
    size_t i = ag_map_find_index(map, key, ag_fn_sys_hash(key));
    if (i == ~0u)
        return 0;
    AgObject* r = map->buckets[i].val.ptr_val;
    ag_retain_shared(r);
    return r;
}

AgObject* ag_m_sys_SharedMap_setAt(AgMap* map, AgObject* key, AgObject* value) {
    ag_retain_shared(value);
    AgObject* r = ag_map_set_at(map, key, (AgMapVal) { .ptr_val = value }).ptr_val;
    return r;
}

AgObject* ag_m_sys_SharedMap_delete(AgMap* map, AgObject* key) {
    return ag_map_delete(map, key).ptr_val;
}

AgObject* ag_m_sys_SharedMap_keyAt(AgMap* map, uint64_t index) {
    return ag_map_key_at(map, index);
}

AgObject* ag_m_sys_SharedMap_valAt(AgMap* map, uint64_t index) {
    if (index >= map->capacity)
        return 0;
    AgObject* r = map->buckets[index].val.ptr_val;
    ag_retain_shared(r);
    return r;
}

void ag_copy_sys_SharedMap(void* dst, void* src) {
    AG_MAP_COPY(i->val.ptr_val = ag_copy_object_field(i->val.ptr_val, &d->head))
}

void ag_dtor_sys_SharedMap(void* map) {
    ag_m_sys_SharedMap_clear((AgMap*)map);
}

void ag_visit_sys_SharedMap(
    AgMap* map,
    void   (*visitor)(void*, int, void*),
    void* ctx)
{
    AG_MAP_VISIT(if (i->val.ptr_val) visitor(i->val.ptr_val, AG_VISIT_OWN, ctx))
}
