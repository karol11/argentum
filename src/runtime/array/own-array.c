#include "own-array.h"

#define AG_NAME(PREFIX, SUFFIX) PREFIX##SUFFIX
#define AG_RELEASE(PTR) ag_release_own((AgObject*)(PTR))
#define AG_RETAIN(PTR) ag_retain_pin((AgObject*)(PTR))
#define AG_RETAIN_OWN(PTR, PARENT) ag_retain_own((AgObject*)(PTR), PARENT)
#define AG_RETAIN_OWN_NN(PTR, PARENT) ag_retain_own_nn((AgObject*)(PTR), PARENT)
#define AG_COPY(TO, FROM, PARENT) (*(AgObject**)(TO) = ag_copy_object_field(*(AgObject**)(FROM), (PARENT)));
#define AG_VISIT_KIND AG_VISIT_OWN
#define AG_ITEM_TYPE AgObject*

#include "array-base-inc.h"

AgObject* ag_m_sys_Array_setOptAt(AgBaseArray* c, uint64_t at, AgObject* val) {
	if (at >= c->items_count)
		return NULL;
	AG_RETAIN_OWN(val, &c->head);
	void* prev = c->items[at];
	ag_set_parent((AgObject*)prev, 0);
	c->items[at] = (void*) val;
	return (AgObject*) prev;
}

bool ag_m_sys_Array_spliceAt(AgBaseArray* c, uint64_t at, AgObject* val) {
	if (at < c->items_count) {
		return ag_splice(val, &c->head, ((AgObject**)c->items) + at);
	}
	return false;
}
