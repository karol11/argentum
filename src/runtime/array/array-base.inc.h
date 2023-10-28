#include <stdint.h>
#include "runtime/blob.h"

void AG_NAME(ag_m_sys_, Array_delete) (AgBlob* b, uint64_t index, uint64_t count) {
	if (!count || index > b->size || index + count > b->size)
		return;
	int64_t* data = b->data + index;
	for (uint64_t i = count; i != 0; i--, data++) {
		AG_RELEASE(*data);
	}
	ag_m_sys_Blob_deleteBytes(b, index * sizeof(int64_t), count * sizeof(int64_t));
}

AG_ITEM_TYPE AG_NAME(ag_m_sys_, Array_getAt) (AgBlob* b, uint64_t index) {
	if (index >= b->size)
		return 0;
	AG_RETAIN(b->data[index]);
	return (AG_ITEM_TYPE) b->data[index];
}

void AG_NAME(ag_m_sys_, Array_setAt) (AgBlob* b, uint64_t index, AG_ITEM_TYPE val) {
	if (index < b->size) {
        int64_t* dst = b->data + index;
        AG_RETAIN_OWN_NN(val, &b->head);
        AG_RELEASE(*dst);
        *dst = (int64_t) val;
    }
}

void AG_NAME(ag_dtor_sys_, Array) (AgBlob* p) {
    int64_t* ptr = p->data;
    int64_t* to = ptr + p->size;
	for (; ptr < to; ptr++) {
		AG_RELEASE(*ptr);
	}
	ag_free(p->data);
}

void AG_NAME(ag_copy_sys_, Array) (AgBlob* d, AgBlob* s) {
	d->size = s->size;
	d->data = (int64_t*) ag_alloc(sizeof(int64_t) * d->size);
    int64_t* from = s->data;
	int64_t* to = d->data;
	int64_t* term = from + d->size;
	for (; from < term; from++, to++) {
		AG_COPY(to, from, &d->head);
	}
}

void AG_NAME(ag_visit_sys_, Array)(
	AgBlob* arr,
	void(*visitor)(void*, int, void*),
	void* ctx)
{
	if (ag_not_null(arr)) {
		int64_t* i = arr->data;
		int64_t* term = i + arr->size;
		for (; i < term; i++)
			visitor((void*)i, AG_VISIT_KIND, ctx);
	}
}
