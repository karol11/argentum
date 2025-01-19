#include <stdio.h> // int32_t int64_t
#include "runtime.h"

void ag_fn_sys_assert0(bool v) {
	if (!v) {
		fputs("assert failure\n", stdout);
		exit(-1);
	}
}
