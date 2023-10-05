#include "map.h"

void ag_m_sys_Map_init(
    AgMap* map,
    int64_t(*hasher)(AgObject*),
    bool (*comparer)(AgObject*, AgObject*))
{
    map->hasher = hasher;
    map->comparer = comparer;
}

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
        ag_release_own(i->val);
        ag_release_shared(i->key);
        i->key = i->val = 0;
    }
    ag_free(map->buckets);
    map->buckets = 0;
}

static inline int64_t get_hash(AgMap* map, AgObject* key) {
    int64_t hash = map->hasher
        ? map->hasher(key)
        : ag_fn_sys_hash(key);
    return hash & ~(~0ull << 48);
}

// Inserts the prelocked key-value, starting at pos `i`.
// It is guaranteed that `key` not presented in the map
static inline void insert(
    AgMap*       map,
    size_t       i,
    unsigned int psl,
    int64_t      hash,
    AgObject*    key,
    AgObject*    value)
{
    for (;; i++) {
        i &= map->capacity - 1;
        AgMapBucket* b = map->buckets + i;
        if (!b->key) {
            b->hash = hash;
            b->psl = psl;
            b->key = key;
            b->val = value;
            return;
        }
        if (b->psl < psl) {
            AgObject* t_key = b->key;
            b->key = key;
            key = t_key;
            AgObject* t_value = b->val;
            b->val = value;
            value = t_value;
            unsigned int t_psl = b->psl;
            b->psl = psl;
            psl = t_psl;
            size_t t_hash = b->hash;
            b->hash = hash;
            hash = t_hash;
        }
        if (psl < 0xffff)
            psl++;
    }
}

static inline size_t find_index(AgMap* map, AgObject* key, size_t hash) { // returns ~0u if not found
    if (!map->buckets)
        return ~0u;
    for (size_t i = hash;; i++) {
        i &= map->capacity - 1;
        AgMapBucket* b = map->buckets + i;
        if (!b->key)
            return ~0u;
        if (b->hash == hash && (
            map->comparer
            ? map->comparer(b->key, key)
            : key == b->key))
            return i;
    }
}

AgObject* ag_m_sys_Map_get(AgMap* map, AgObject* key) {
    size_t i = find_index(map, key, get_hash(map, key));
    if (i == ~0u)
        return 0;
    ag_retain_pin(map->buckets[i].val);
    return map->buckets[i].val;
}

AgObject* ag_m_sys_Map_set(AgMap* map, AgObject* key, AgObject* value) {
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
                if (i->key)
                   insert(
                       map,
                       i->hash & (map->capacity - 1), // initial insertion position
                       0, // psl
                       i->hash,
                       i->key,
                       i->val);
            }
            ag_free(old_buckets);
        }
    }
    size_t hash = get_hash(map, key);
    ag_retain_own(value, &map->head);
    size_t i = hash;
    for (unsigned int psl = 0;; i++) {
        i &= map->capacity - 1;
        AgMapBucket* b = map->buckets + i;
        if (!b->key) {
            b->hash = hash;
            b->psl = psl;
            ag_retain_shared(key);
            b->key = key;
            b->val = value;
            return 0;
        }
        if (b->hash == hash && (
            map->comparer
                ? map->comparer(b->key, key)
                : key == b->key))
        {
            AgObject* r = b->val;
            b->val = value;
            ag_set_parent(r, 0);
            return r;
        }
        if (b->psl < psl) {
            AgObject* t_key = b->key;
            AgObject* t_value = b->val;
            unsigned int t_psl = b->psl;
            size_t t_hash = b->hash;
            ag_retain_shared(key);
            b->key = key;
            b->val = value;
            b->psl = psl;
            b->hash = hash;
            insert(map, i, t_psl, t_hash, t_key, t_value);
            return 0;
        }
        if (psl < 0xffff)
            psl++;
    }
}

AgObject* ag_m_sys_Map_delete(AgMap* map, AgObject* key) {
    size_t ri = find_index(map, key, get_hash(map, key));
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
        if (next == rb || !next->key || !next->psl) // full loop or empty slot or element on its place
            break;
        cur->hash = next->hash;
        cur->key = next->key;
        cur->val = next->val;
        cur->psl = next->psl - 1;
        next->key = 0;
        cur = next;
    }
    return r;
}

void ag_copy_sys_Map(void* dst, AgMap* src) {
    AgMap* s = (AgMap*)s;
    AgMap* d = (AgMap*)dst;
    d->hasher = s->hasher;
    d->comparer = s->comparer;
    d->capacity = s->capacity;
    d->size = s->size;
    d->buckets = ag_alloc(s->capacity * sizeof(AgMapBucket));
    ag_memcpy(d->buckets, s->buckets, s->capacity * sizeof(AgMapBucket));
    for (AgMapBucket* i = d->buckets, *n = d->buckets + d->capacity; i < n; i++) {
        ag_retain_shared(i->key);
        i->val = ag_copy_object_field(i->val, &d->head);
    }
}

void ag_dtor_sys_Map(void* map) {
    ag_m_sys_Map_clear((AgMap*)map);
}
