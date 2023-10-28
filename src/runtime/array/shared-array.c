#include "shared-array.h"

#define AG_NAME(PREFIX, SUFFIX) PREFIX##Shared##SUFFIX
#define AG_RELEASE(PTR) ag_release_shared((AgObject*)(PTR))
#define AG_RETAIN(PTR) ag_retain_shared((AgObject*)(PTR))
#define AG_RETAIN_OWN(PTR, PARENT) ag_retain_shared((AgObject*)(PTR))
#define AG_RETAIN_OWN_NN(PTR, PARENT) ag_retain_shared_nn((AgObject*)(PTR))
#define AG_COPY(TO, FROM, PARENT) { *TO = *FROM; ag_retain_shared(*(AgObject**)(FROM)); }
#define AG_VISIT_KIND AG_VISIT_OWN
#define AG_ITEM_TYPE AgObject*

#include "array-base.inc.h"
