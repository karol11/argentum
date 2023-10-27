#include "map.h"

int64_t ag_m_sys_Map_size(AgMap* map) {
    return map->size;
}
int64_t ag_m_sys_Map_capacity(AgMap* map) {
    return map->capacity;
}
AgObject* ag_m_sys_Map_keyAt(AgMap* map, uint64_t index) {
    if (index >= map->capacity)
        return 0;
    AgObject* r = map->buckets[index].key;
    ag_retain_shared(r);
    return r;
}
AgObject* ag_m_sys_Map_valAt(AgMap* map, uint64_t index) {
    if (index >= map->capacity)
        return 0;
    AgObject* r = map->buckets[index].val;
    ag_retain_pin(r);
    return r;
}

void ag_m_sys_Map_clear(AgMap* map) {
    if (map->buckets == 0)
        return;
    for (AgMapBucket* i = map->buckets, *j = map->buckets + map->capacity; i < j; i++) {
        if (!i->key) continue;
        if (i->key) {
            ag_release_shared_nn(i->key);
            i->key = 0;
        }
        if (i->val) {
            ag_release_own_nn(i->val);
            i->val = 0;
        }
    }
    ag_free(map->buckets);
    map->buckets = 0;
    map->capacity = map->size = 0;
}

// Inserts the prelocked key-value, starting at pos `i`.
// It is guaranteed that `key` is not in the map
static inline void insert(
    AgMap*       map,
    size_t       i,
    uint64_t     dist,
    int64_t      hash,
    AgObject*    key,
    AgObject*    value)
{
    for (;; i++, dist++) {
        i &= map->capacity - 1;
        AgMapBucket* b = map->buckets + i;
        if (!b->key) {
            b->dist = dist;
            b->key = key;
            b->val = value;
            return;
        }
        if (b->dist < dist) {
            AgObject* t_key = b->key;
            b->key = key;
            key = t_key;
            AgObject* t_value = b->val;
            b->val = value;
            value = t_value;
            uint64_t t_dist = b->dist;
            b->dist = dist;
            dist = t_dist;
        }
    }
}

static inline size_t find_index(AgMap* map, AgObject* key, size_t hash) { // returns ~0u if not found
    if (!map->buckets)
        return ~0u;
    uint64_t dist = 0;
    for (size_t i = hash;; i++) {
        i &= map->capacity - 1;
        AgMapBucket* b = map->buckets + i;
        if (!b->key || b->dist < dist)
            return ~0u;
        if (ag_eq_shared(b->key, key))
            return i;
        dist = b->dist;
    }
}

AgObject* ag_m_sys_Map_getAt(AgMap* map, AgObject* key) {
    size_t i = find_index(map, key, ag_fn_sys_hash(key));
    if (i == ~0u)
        return 0;
    ag_retain_pin(map->buckets[i].val);
    return map->buckets[i].val;
}

AgObject* ag_m_sys_Map_setAt(AgMap* map, AgObject* key, AgObject* value) {
    if (map->size >= map->capacity * 3 / 4) {  // rehash
        size_t old_cap = map->capacity;
        AgMapBucket* old_buckets = map->buckets;
        map->capacity = map->capacity
            ? map->capacity << 1
            : 16;
        map->buckets = ag_alloc(sizeof(AgMapBucket) * map->capacity);
        ag_zero_mem(map->buckets, sizeof(AgMapBucket) * map->capacity);
        if (old_buckets) {
            for (AgMapBucket* i = old_buckets, *j = old_buckets + old_cap; i < j; i++) {
                if (i->key) {
                    uint64_t hash = ag_fn_sys_hash(i->key);
                    insert(
                        map,
                        hash & (map->capacity - 1), // initial insertion position
                        0, // dist
                        hash,
                        i->key,
                        i->val);
                }
            }
            ag_free(old_buckets);
        }
    }
    uint64_t hash = ag_fn_sys_hash(key);
    ag_retain_own(value, &map->head);
    uint64_t dist = 0;
    for (size_t i = hash;; i++, dist++) {
        i &= map->capacity - 1;
        AgMapBucket* b = map->buckets + i;
        if (!b->key) {
            b->dist = dist;
            ag_retain_shared(key);
            b->key = key;
            b->val = value;
            return 0;
        }
        if (ag_eq_shared(b->key, key)) {
            AgObject* r = b->val;
            b->val = value;
            ag_set_parent(r, 0);
            return r;
        }
        if (b->dist < dist) {
            AgObject* t_key = b->key;
            AgObject* t_value = b->val;
            uint64_t t_dist = b->dist;
            ag_retain_shared(key);
            b->key = key;
            b->val = value;
            b->dist = dist;
            insert(map, i, t_dist, ag_fn_sys_hash(t_key), t_key, t_value);
            return 0;
        }
    }
}

AgObject* ag_m_sys_Map_delete(AgMap* map, AgObject* key) {
    size_t ri = find_index(map, key, ag_fn_sys_hash(key));
    if (ri == ~0u)
        return 0;
    AgMapBucket* rb = map->buckets + ri;
    ag_release_shared(rb->key);
    AgObject* r = rb->val;
    ag_set_parent(r, 0);
    rb->key = rb->val = 0;
    AgMapBucket* cur = map->buckets + ri;
    AgMapBucket* term = map->buckets + map->capacity;
    for (;;) {
        AgMapBucket* next = ++cur;
        if (next == term)
            next = map->buckets;
        if (next == rb || !next->key || next->dist == 0) // full loop or empty slot or element on its place
            break;
        cur->key = next->key;
        cur->val = next->val;
        cur->dist = next->dist - 1;
        next->key = next->val = 0;
        next->dist = 0;
        cur = next;
    }
    return r;
}

void ag_copy_sys_Map(void* dst, void* src) {
    AgMap* s = (AgMap*)src;
    AgMap* d = (AgMap*)dst;
    d->capacity = s->capacity;
    d->size = s->size;
    d->buckets = ag_alloc(s->capacity * sizeof(AgMapBucket));
    ag_memcpy(d->buckets, s->buckets, s->capacity * sizeof(AgMapBucket));
    for (AgMapBucket* i = d->buckets, *n = d->buckets + d->capacity; i < n; i++) {
        ag_retain_shared(i->key);
        if (i->val)
            i->val = ag_copy_object_field(i->val, &d->head);
    }
}

void ag_dtor_sys_Map(void* map) {
    ag_m_sys_Map_clear((AgMap*)map);
}

void ag_visit_sys_Map(
    AgMap* map,
    void   (*visitor)(void*, int, void*),
    void*  ctx)
{
    if (ag_not_null(map) && map->buckets) {
        AgMapBucket* i = map->buckets;
        AgMapBucket* term = i + map->capacity;
        for (; i < term; i++) {
            if (i->key)
                visitor(i->key, AG_VISIT_OWN, ctx);
            if (i->val)
                visitor(i->val, AG_VISIT_OWN, ctx);
        }
    }
}
