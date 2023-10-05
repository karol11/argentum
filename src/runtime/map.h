#ifndef AK_MAP_H_
#define AK_MAP_H_

#include "runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

//
// Open addressed
// Pow_2 grow
// Linear probe
// Robinhood hashing
//
typedef struct {
    AgObject*    key;   // 0 - empty
    AgObject*    val;
    uint64_t     hash : 48;
    unsigned int psl : 16;  // probe_sequence_length
} AgMapBucket;

typedef struct {
    AgObject     head;
    uint64_t     (*hasher)   (AgObject* item);
    bool         (*comparer) (AgObject* a, AgObject* b);
    AgMapBucket* buckets;
    size_t       capacity; // must be power of 2
    size_t       size;
} AgMap;

void      ag_m_sys_Map_init (
                                AgMap*  map,
                                int64_t (*hasher)   (AgObject*),
                                bool    (*comparer) (AgObject*,AgObject*));
int64_t   ag_m_sys_Map_size     (AgMap* map);
void      ag_m_sys_Map_clear    (AgMap* map);
AgObject* ag_m_sys_Map_get      (AgMap* map, AgObject* key);                  // returns ?T
AgObject* ag_m_sys_Map_set      (AgMap* map, AgObject* key, AgObject* value); // returns previous object as ?T
AgObject* ag_m_sys_Map_delete   (AgMap* map, AgObject* key);                  // returns previous object as ?T
int64_t   ag_m_sys_Map_capacity (AgMap* map);
AgObject* ag_m_sys_Map_keyAt    (AgMap* map, uint64_t index);
AgObject* ag_m_sys_Map_valAt    (AgMap* map, uint64_t index);

void      ag_copy_sys_Map       (void* dst, void* src);
void      ag_dtor_sys_Map       (void* map);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif // AK_MAP_H_
