#include <stdint.h>
#include "runtime/array/array-base.h"

int64_t AG_NAME(ag_m_sys_, Array_capacity)(AgBaseArray* c) {
	return c->items_count;
}

void AG_NAME(ag_m_sys_, Array_insert)(AgBaseArray* c, uint64_t at, uint64_t items_count) {
	ag_insert_into_container(c, at, items_count);
}

void AG_NAME(ag_m_sys_, Array_delete) (AgBaseArray* c, uint64_t index, uint64_t count) {
	if (!count || index > c->items_count || index + count > c->items_count)
		return;
	void** data = c->items + index;
	for (uint64_t i = count; i != 0; i--, data++) {
		AG_RELEASE(*data);
	}
	ag_delete_container_items(c, index, count);
}

bool AG_NAME(ag_m_sys_, Array_move)(AgBaseArray* c, uint64_t x, uint64_t y, uint64_t z) {
	return ag_move_container_items(c, x, y, z);
}

AG_ITEM_TYPE AG_NAME(ag_m_sys_, Array_getAt) (AgBaseArray* c, uint64_t index) {
	if (index >= c->items_count)
		return 0;
	AG_RETAIN(c->items[index]);
	return (AG_ITEM_TYPE)c->items[index];
}

void AG_NAME(ag_m_sys_, Array_setAt) (AgBaseArray* c, uint64_t index, AG_ITEM_TYPE val) {
	if (index < c->items_count) {
        void** dst = c->items + index;
        AG_RETAIN_OWN_NN(val, &c->head);
        AG_RELEASE(*dst);
        *dst = (void*) val;
    }
}

void AG_NAME(ag_dtor_sys_, Array) (AgBaseArray* c) {
    void** ptr = c->items;
    void** to = ptr + c->items_count;
	for (; ptr < to; ptr++) {
		AG_RELEASE(*ptr);
	}
	ag_free(c->items);
}

void AG_NAME(ag_copy_sys_, Array) (AgBaseArray* d, AgBaseArray* s) {
	d->items_count = s->items_count;
	d->items = (void**) ag_alloc(sizeof(void*) * d->items_count);
    void** from = s->items;
	void** to = d->items;
	void** term = from + d->items_count;
	for (; from < term; from++, to++) {
		AG_COPY(to, from, &d->head);
	}
}

void AG_NAME(ag_visit_sys_, Array)(
	AgBaseArray* c,
	void(*visitor)(void*, int, void*),
	void* ctx)
{
	if (ag_not_null(c)) {
		void** i = c->items;
		void** term = i + c->items_count;
		for (; i < term; i++)
			visitor((void*)i, AG_VISIT_KIND, ctx);
	}
}
