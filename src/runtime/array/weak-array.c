#include "weak-array.h"

#define AG_NAME(PREFIX, SUFFIX) PREFIX##Weak##SUFFIX
#define AG_RELEASE(PTR) ag_release_weak((AgWeak*)(PTR))
#define AG_RETAIN(PTR) ag_retain_weak((AgWeak*)(PTR))
#define AG_RETAIN_OWN(PTR, PARENT) ag_retain_weak((AgWeak*)(PTR))
#define AG_RETAIN_OWN_NN(PTR, PARENT) ag_retain_weak((AgWeak*)(PTR))
#define AG_COPY(TO, FROM, PARENT) ag_copy_weak_field((void**)(TO), (AgWeak*)*(FROM));
#define AG_VISIT_KIND AG_VISIT_WEAK
#define AG_ITEM_TYPE AgWeak*

#include "array-base.inc.h"
