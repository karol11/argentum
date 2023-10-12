#include "own-array.h"

#define AG_NAME(PREFIX, SUFFIX) PREFIX##SUFFIX
#define AG_RELEASE(PTR) ag_release_own((AgObject*)(PTR))
#define AG_RETAIN(PTR) ag_retain_pin((AgObject*)(PTR))
#define AG_RETAIN_OWN(PTR, PARENT) ag_retain_own((AgObject*)(PTR), PARENT)
#define AG_RETAIN_OWN_NN(PTR, PARENT) ag_retain_own_nn((AgObject*)(PTR), PARENT)
#define AG_COPY(TO, FROM, PARENT) (AgObject*)*(TO) = ag_copy_object_field((AgObject*)*FROM, PARENT);
#define AG_VISIT_KIND AG_VISIT_OWN
#define AG_ITEM_TYPE AgObject*

#include "array-base.inc.h"

AgObject* ag_m_sys_Array_setOptAt(AgBlob* b, uint64_t index, AgObject* val) {
	if (index >= b->size)
		return 0;
	AG_RETAIN_OWN(val, &b->head);
	int64_t prev = b->data[index];
	ag_set_parent((AgObject*)prev, 0);
	b->data[index] = (int64_t) val;
	return (AgObject*) prev;
}

bool ag_m_sys_Array_spliceAt(AgBlob* b, uint64_t index, AgObject* val) {
	if (index < b->size) {
		AgObject** dst = ((AgObject**)(b->data)) + index;
		if (ag_splice(val, &b->head)) {
			ag_release_own(*dst);
			*dst = val;
			return true;
		}
	}
	return false;
}
