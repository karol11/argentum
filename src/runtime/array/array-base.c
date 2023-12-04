#include "runtime/array/array-base.h"

void ag_insert_into_container(AgBaseArray* c, uint64_t at, uint64_t count){
	if (!count || at > c->items_count)
		return;
	void** new_data = (void**)ag_alloc((c->items_count + count) * sizeof(void*));
	ag_memcpy(new_data, c->items, at * sizeof(void*));
	ag_zero_mem(new_data + at, count * sizeof(void*));
	ag_memcpy(new_data + at + count, c->items + at, (c->items_count - at) * sizeof(void*));
	ag_free(c->items);
	c->items = new_data;
	c->items_count += count;
}

bool ag_move_container_items(AgBaseArray* c, uint64_t x, uint64_t y, uint64_t z) {
	if (x >= y || y >= z || z > c->items_count)
		return false;
	void** temp = (void**)ag_alloc(sizeof(void*) * (y - x));
	ag_memmove(temp, c->items + x, sizeof(void*) * (y - x));
	ag_memmove(c->items + x, c->items + y, sizeof(void*) * (z - y));
	ag_memmove(c->items + x + (z - y), temp, sizeof(void*) * (y - x));
	ag_free(temp);
	return true;
}

void ag_delete_container_items(AgBaseArray* c, uint64_t at, uint64_t count) {
	void** new_data = (void**)ag_alloc((c->items_count - count) * sizeof(void*));
	ag_memcpy(new_data, c->items, at * sizeof(void*));
	ag_memcpy(new_data + at, c->items + at + count, (c->items_count - at - count) * sizeof(void*));
	ag_free(c->items);
	c->items = new_data;
	c->items_count -= count;
}
