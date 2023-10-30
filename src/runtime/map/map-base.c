#include "map-base.h"

AgObject* ag_map_key_at(AgMap* map, uint64_t index) {
    if (index >= map->capacity)
        return NULL;
    AgObject* r = map->buckets[index].key;
    ag_retain_shared(r);
    return r;
}

size_t ag_map_find_index(AgMap* map, AgObject* key, size_t hash) { // returns ~0u if not found
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

void ag_map_insert(
    AgMap* map,
    size_t     i,
    uint64_t   dist,
    int64_t    hash,
    AgObject*  key,
    AgMapVal   value)
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
            AgMapVal t_value = b->val;
            b->val = value;
            value = t_value;
            uint64_t t_dist = b->dist;
            b->dist = dist;
            dist = t_dist;
        }
    }
}

void ag_map_clear(
    AgMap* map,
    void(*val_disposer)(AgMapVal))
{
    if (map->buckets == 0)
        return;
    for (AgMapBucket* i = map->buckets, *j = map->buckets + map->capacity; i < j; i++) {
        if (!i->key) continue;
        ag_release_shared_nn(i->key);
        val_disposer(i->val);
        i->key = NULL;
        i->val.int_val = 0;
    }
    ag_free(map->buckets);
    map->buckets = 0;
    map->capacity = map->size = 0;
}

AgMapVal ag_map_set_at(
    AgMap* map,
    AgObject* key,
    AgMapVal value)
{
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
                    ag_map_insert(
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
    uint64_t dist = 0;
    for (size_t i = hash;; i++, dist++) {
        i &= map->capacity - 1;
        AgMapBucket* b = map->buckets + i;
        if (!b->key) {
            b->dist = dist;
            ag_retain_shared(key);
            b->key = key;
            b->val = value;
            return (AgMapVal) { 0 };
        }
        if (ag_eq_shared(b->key, key)) {
            AgMapVal r = b->val;
            b->val = value;
            return r;
        }
        if (b->dist < dist) {
            AgObject* t_key = b->key;
            AgMapVal t_value = b->val;
            uint64_t t_dist = b->dist;
            ag_retain_shared(key);
            b->key = key;
            b->val = value;
            b->dist = dist;
            ag_map_insert(map, i, t_dist, ag_fn_sys_hash(t_key), t_key, t_value);
            return (AgMapVal){ 0 };
        }
    }
}

AgMapVal ag_map_delete(AgMap* map, AgObject* key) {
    size_t ri = ag_map_find_index(map, key, ag_fn_sys_hash(key));
    if (ri == ~0u)
        return (AgMapVal){ 0 };
    AgMapBucket* rb = map->buckets + ri;
    ag_release_shared(rb->key);
    AgMapVal r = rb->val;
    rb->key = 0;
    rb->val.int_val = 0;
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
        next->key = 0;
        next->val.int_val = 0;
        next->dist = 0;
        cur = next;
    }
    return r;
}
