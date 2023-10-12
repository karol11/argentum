#include "weak-array.h"

#define AG_NAME(PREFIX, SUFFIX) PREFIX##Weak##SUFFIX
#define AG_RELEASE(PTR) ag_release_weak((AgWeak*)(PTR))
#define AG_RETAIN(PTR) ag_retain_weak((AgWeak*)(PTR))
#define AG_RETAIN_OWN(PTR, PARENT) ag_retain_weak((AgWeak*)(PTR))
#define AG_RETAIN_OWN_NN(PTR, PARENT) ag_retain_weak_nn((AgWeak*)(PTR))
#define AG_COPY(TO, FROM, PARENT) ag_copy_weak_field((void**)(TO), (AgWeak*)*FROM);
#define AG_VISIT_KIND AG_VISIT_WEAK
#define AG_ITEM_TYPE AgWeak*

#include "array-base.inc.h"

/*
void ag_m_sys_WeakArray_delete(AgBlob* b, uint64_t index, uint64_t count) {
	if (!count || index > b->size || index + count > b->size)
		return;
	AgWeak** data = ((AgWeak**)(b->data)) + index;
	for (uint64_t i = count; i != 0; i--, data++) {
		ag_release_weak(*data);
		*data = 0;
	}
	ag_m_sys_Blob_deleteBytes(b, index * sizeof(int64_t), count * sizeof(int64_t));
}

AgWeak* ag_m_sys_WeakArray_getAt(AgBlob* b, uint64_t index) {
	if (index >= b->size)
		return 0;
	AgWeak* r = (AgWeak*)(b->data[index]);
	ag_retain_weak(r);
	return r;
}

void ag_m_sys_WeakArray_setAt(AgBlob* b, uint64_t index, AgWeak* val) {
	if (index < b->size) {
		AgWeak** dst = ((AgWeak**)(b->data)) + index;
		ag_retain_weak(val);
		ag_release_weak(*dst);
		*dst = val;
	}
}

void ag_copy_sys_WeakArray(AgBlob* d, AgBlob* s) {
	d->size = s->size;
	d->data = (int64_t*) ag_alloc(sizeof(int64_t) * d->size);
	void** to = (void**)(d->data);
	for (AgWeak
			**from = (AgWeak**)(s->data),
			**term = from + d->size;
		from < term;
		from++, to++)
	{
		ag_copy_weak_field(to, *from);
	}
}

void ag_visit_sys_WeakArray(
	AgBlob* arr,
	void(*visitor)(void*, int, void*),
	void* ctx) {
	if (ag_not_null(arr)) {
		AgObject** i = (AgObject**)(arr->data);
		AgObject** term = i + arr->size;
		while (i < term)
			visitor(i++, AG_VISIT_WEAK, ctx);
	}
}

void ag_dtor_sys_WeakArray(AgBlob* p) {
	for (AgWeak
			**ptr = (AgWeak**)(p->data),
			**to = ptr + p->size;
		ptr < to;
		ptr++)
	{
		ag_release_weak(*ptr);
	}
	ag_free(p->data);
}
*/