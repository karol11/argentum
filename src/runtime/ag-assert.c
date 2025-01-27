#include <stdio.h> // int32_t int64_t
#include "runtime.h"

void ag_fn_tests_assert(bool v) {
	if (!v) {
		fputs("assert failure\n", stdout);
		exit(-1);
	}
}

static int64_t foreign_test_function_state = 0;

int64_t ag_fn_tests_ffiTestFn(int64_t delta) {
	return foreign_test_function_state += delta;
}

void ag_fn_tests_ffiTestReset() {
    foreign_test_function_state = 0;
}

void void_void_trampoline(AgObject* self, ag_fn entry_point, ag_thread* th) {
    // extract params here, if any
    ag_unlock_thread_queue(th);
    if (self)
        ((void (*)(AgObject*)) entry_point)(self);
    // release params here, if any
}
void ag_fn_tests_asyncFfiCallbackInvoker(AgWeak* cb_data, ag_fn cb_entry_point) {
    ag_retain_weak(cb_data);
    ag_thread* th = ag_prepare_post_from_ag(cb_data, cb_entry_point, void_void_trampoline, 0);
    // post params here if any
}
