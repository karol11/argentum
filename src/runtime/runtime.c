#include <stddef.h> // size_t
#include <stdint.h> // int32_t
#include <stdio.h> // puts
#include <assert.h>
#include <time.h>  // timespec, timespec_get

inline uint64_t timespec_to_ms(const struct timespec* time) {
	return time->tv_nsec / 1000000 + time->tv_sec * 1000;
}

#ifdef WIN32

#include <windows.h>

// Thread
#define thrd_success 0
#define thrd_error 1
typedef HANDLE thrd_t;
typedef int (*thrd_start_t) (void*);
int thrd_create(thrd_t* thr, thrd_start_t func, void* arg) {
	HANDLE r = CreateThread(
		NULL,                   // default security attributes
		0,                      // use default stack size  
		func,
		arg,          // argument to thread function 
		0,                      // use default creation flags 
		NULL
	);
	if (r == NULL)
		return thrd_error;
	*thr = r;
	return thrd_success;
}
#define thrd_exit ExitThread
int thrd_join(thrd_t thr, int* usused_res) {
	return WaitForSingleObject(thr, INFINITE)
		? thrd_error
		: thrd_success;  // success if 0 (WAIT_OBJECT_0)
}

// Mutex
#define mtx_plain 0
typedef CRITICAL_SECTION mtx_t;
int mtx_init(mtx_t* mutex, int type) {
	return InitializeCriticalSectionAndSpinCount(mutex, 0x00000400)
		? thrd_success
		: thrd_error;
}
#define mtx_destroy DeleteCriticalSection
inline int mtx_lock(mtx_t* mutex) {
	EnterCriticalSection(mutex);
	return thrd_success;
}
inline int mtx_unlock(mtx_t* mutex) {
	LeaveCriticalSection(mutex);
	return thrd_success;
}

// CVar
typedef CONDITION_VARIABLE cnd_t;
inline int cnd_init(cnd_t* cond) {
	InitializeConditionVariable(cond);
	return thrd_success;
}
inline void cnd_destroy(cnd_t* cond) {
	// do nothing
}
inline int cnd_signal(cnd_t* cond) {
	WakeConditionVariable(cond);
	return thrd_success;
}
inline int cnd_broadcast(cnd_t* cond) {
	WakeAllConditionVariable(cond);
	return thrd_success;
}
inline int cnd_timedwait(cnd_t* cond, mtx_t* mutex, const struct timespec* timeout) {
	struct timespec now;
	return SleepConditionVariableCS(
		cond,
		mutex, 
		timespec_get(&now, TIME_UTC)
			? (DWORD)(timespec_to_ms(timeout) - timespec_to_ms(&now))
			: INFINITE
	)
		? thrd_success
		: thrd_error;
}
inline int cnd_wait(cnd_t* cond, mtx_t* mutex) {
	return SleepConditionVariableCS(cond, mutex, INFINITE)
		? thrd_success
		: thrd_error;
}
#define AG_THREAD_LOCAL __declspec(thread)

#else

#include <threads.h>
#define AG_THREAD_LOCAL _Thread_local

#endif

#ifdef NO_DEFAULT_LIB
void ag_zero_mem(void*, size_t);
void ag_memcpy(void*, void*, size_t);
void ag_memmove(void*, void*, size_t);
#else
#include <string.h>
#define ag_zero_mem(P, S) memset(P, 0, S)
#define ag_memcpy memcpy
#define ag_memmove memmove
#endif

#include "../utils/utf8.h"
#include "../runtime/runtime.h"

#ifndef AG_ALLOC
#include <stdlib.h>
#define AG_ALLOC malloc
#define AG_FREE free
#endif

#ifndef __cplusplus
#define true 1
#define false 0
#endif

//
// Tags for copy operation
// AG_TG_NOWEAK_DST should match AG_F_PARENT
// AG_TG_WEAK_BLOCK must be 0
// AG_TG_WEAK_BLOCK - this is object, its wb_p points to next-in-queue wb-item
// AG_TG_NOWEAK_DST - this is object, its wb_p points to its parent, its `counter` points to next-in-queue object. Used for dst objects copied before weaks.
#define AG_TG_WEAK_BLOCK ((uintptr_t) 0)
#define AG_TG_NOWEAK_DST ((uintptr_t) 1)
#define AG_TG_OBJECT     ((uintptr_t) 2)
#define AG_TG_WEAK       ((uintptr_t) 3)

#define AG_PTR_TAG(PTR)            (((uintptr_t)PTR) & 3)
#define AG_TAG_PTR(TYPE, PTR, TAG) ((TYPE*)(((uintptr_t)(PTR)) | TAG))
#define AG_UNTAG_PTR(TYPE, PTR)    ((TYPE*)(((uintptr_t)(PTR)) & ~3))

#define ag_head(OBJ) ((AgObject*)(OBJ))
#define ag_not_null(OBJ) ((OBJ) && (size_t)(OBJ) >= 256)

#define AG_HEAD_SIZE 0

size_t ag_leak_detector_counter = 0;
size_t ag_current_allocated = 0;
size_t ag_max_allocated = 0;

#ifdef _DEBUG

void* ag_alloc(size_t size) {
	ag_leak_detector_counter++;
	if ((ag_current_allocated += size) > ag_max_allocated)
		ag_max_allocated = ag_current_allocated;
	size_t* r = (size_t*)AG_ALLOC(size + sizeof(size_t));
	if (!r) {  // todo: add more handling
		exit(-42);
	}
	*r = size;
	return r + 1;
}
void ag_free(void* data) {
	if (data) {
		ag_leak_detector_counter--;
		size_t* r = (size_t*)data;
		ag_current_allocated -= r[-1];
		AG_FREE(r - 1);
	}
}

#else

#define ag_alloc AG_ALLOC
#define ag_free AG_FREE

#endif

#define AG_THREAD_QUEUE_SIZE 8192

typedef struct ag_thread_tag {
	int64_t*  queue_start;
	int64_t*  queue_end;
	int64_t*  read_pos;  // 0 if free
	int64_t*  write_pos; // next free if free
	AgObject* root;
	uint64_t  timer_ms;  // todo: replace with pyramid-heap
	ag_fn     timer_proc;
	AgWeak*   timer_proc_param;
	mtx_t     mutex;
	cnd_t     is_not_empty;
//	thrd_t    thread;
} ag_thread;

/*
ag_thread* ag_threads = NULL;
ag_thread* ag_threads_free = NULL;
uint64_t   ag_threads_size = 0;
uint64_t   ag_threads_allocated = 0;
*/
ag_thread ag_main_thread = { 0 };

#define AG_RETAIN_BUFFER_SIZE 8192
AG_THREAD_LOCAL ag_thread** ag_current_thread;
AG_THREAD_LOCAL AgObject** ag_retain_buffer;
AG_THREAD_LOCAL AgObject** ag_retain_pos;
AG_THREAD_LOCAL AgObject** ag_release_pos;

inline void ag_set_parent_nn(AgObject* obj, AgObject* parent) {
	if (obj->wb_p & AG_F_PARENT)
		obj->wb_p = (uintptr_t)parent | AG_F_PARENT;
	else
		((AgWeak*)obj->wb_p)->org_pointer_to_parent = (uintptr_t)parent;
}

inline bool ag_is_shared_nn(AgObject* obj) {
	if ((ag_head(obj)->wb_p & AG_F_PARENT) != 0) {
		return ag_head(obj)->wb_p == (AG_SHARED | AG_F_PARENT);
	} else {
		AgWeak* wb = (AgWeak*)(ag_head(obj)->wb_p);
		return wb->org_pointer_to_parent == AG_SHARED;
	}
}

bool ag_splice(AgObject* obj, AgObject* parent) {
	if (ag_not_null(obj)) {
		for (AgObject* p = parent; p; p = ag_fn_sys_getParent(p)) {
			if (p == obj)
				return false;
		}
		ag_set_parent_nn(obj, parent);
		ag_head(obj)->ctr_mt += AG_CTR_STEP;
	}
	return true;
}

void ag_set_parent(AgObject* obj, AgObject* parent) {
	if (ag_not_null(obj)) {
		ag_set_parent_nn(obj, parent);
	}
}

void ag_release_pin(AgObject * obj) {
	if (ag_not_null(obj)) {
		if ((ag_head(obj)->ctr_mt -= AG_CTR_STEP) == 0)
			ag_dispose_obj(obj);
	}
}
inline AgObject* ag_retain_pin(AgObject* obj) {
	if (ag_not_null(obj))
		ag_head(obj)->ctr_mt += AG_CTR_STEP;
	return obj;
}
inline void ag_release_weak(AgWeak* w) {
	if (!ag_not_null(w))
		return;
	if (w->wb_ctr_mt & AG_CTR_MT) {
		*ag_release_pos = (AgObject*)w;
		if (++ag_release_pos == ag_retain_pos)
			ag_flush_retain_release();
	} else if ((w->wb_ctr_mt -= AG_CTR_STEP) == AG_CTR_WEAK) {
		ag_free(w);
	}
}
inline AgWeak* ag_retain_weak_nn(AgWeak* w) {
	if (w->wb_ctr_mt & AG_CTR_MT) {
		*ag_retain_pos = (AgObject*)w;
		if (--ag_retain_pos == ag_release_pos)
			ag_flush_retain_release();
	} else {
		w->wb_ctr_mt += AG_CTR_STEP;
	}
	return w;
}
AgWeak* ag_retain_weak(AgWeak* w) {
	return ag_not_null(w)
		? ag_retain_weak_nn(w)
		: 0;
}
void ag_release_own(AgObject* obj) {
	if (ag_not_null(obj)) {
		assert((ag_head(obj)->ctr_mt & AG_CTR_MT) == 0);
		if ((ag_head(obj)->ctr_mt -= AG_CTR_STEP) == 0)
			ag_dispose_obj(obj);
		else
			ag_set_parent_nn(obj, AG_IN_STACK);
	}
}
void ag_retain_own(AgObject* obj, AgObject* parent) {
	if (ag_not_null(obj)) {
		assert((ag_head(obj)->ctr_mt & AG_CTR_MT) == 0);
		ag_head(obj)->ctr_mt += AG_CTR_STEP;
		ag_set_parent_nn(obj, parent);
	}
}
void ag_release_shared(AgObject* obj) {
	if (!ag_not_null(obj))
		return;
	if (ag_head(obj)->ctr_mt & AG_CTR_MT) {
		*ag_release_pos = obj;
		if (++ag_release_pos == ag_retain_pos)
			ag_flush_retain_release();
	} else if ((ag_head(obj)->ctr_mt -= AG_CTR_STEP) == 0) {
		ag_dispose_obj(obj);
	}
}
void ag_retain_shared(AgObject* obj) {
	if (!ag_not_null(obj))
		return;
	if (ag_head(obj)->ctr_mt & AG_CTR_MT) {
		*ag_retain_pos = obj;
		if (--ag_retain_pos == ag_release_pos)
			ag_flush_retain_release();
	} else {
		ag_head(obj)->ctr_mt += AG_CTR_STEP;
	}
}

bool ag_leak_detector_ok() {
	return ag_leak_detector_counter == 0;
}
uintptr_t ag_max_mem() {
	return ag_max_allocated;
}

typedef struct {
	AgObject* data;
	void (*fixer)(AgObject*);
} AgCopyFixer;

AgObject*    ag_copy_head = 0;          // must be threadlocal
size_t       ag_copy_fixers_count = 0;
size_t       ag_copy_fixers_alloc = 0;
AgCopyFixer* ag_copy_fixers;            // Used only for objects with manual afterCopy operators.
bool         ag_copy_freeze = false;

void ag_dispose_obj(AgObject* obj) {
	((AgVmt*)(ag_head(obj)->dispatcher))[-1].dispose(obj);
	AgWeak* wb = (AgWeak*)(ag_head(obj)->wb_p);
	if (((uintptr_t)wb & AG_F_PARENT) == 0) {
		wb->target = 0;
		ag_release_weak(wb);
	}
	ag_free(ag_head(obj));
}

AgObject* ag_allocate_obj(size_t size) {
	AgObject* r = (AgObject*) ag_alloc(size + AG_HEAD_SIZE);
	if (!r) {  // todo: add more handling
		exit(-42);
	}
	ag_zero_mem(r, size);
	r->ctr_mt = AG_CTR_STEP;
	r->wb_p = AG_IN_STACK | AG_F_PARENT;
	return r;
}
AgObject* ag_freeze(AgObject* src) {
	if (AG_SHARED == (src->wb_p & AG_F_PARENT
		? src->wb_p & ~AG_F_PARENT
		: ((AgWeak*)src->wb_p)->org_pointer_to_parent))
	{
		src->ctr_mt += AG_CTR_STEP;
		return src;
	}
	ag_copy_freeze = true;
	AgObject* r = ag_copy(src); // todo make optimized version
	ag_copy_freeze = false;
	return r;
}
AgObject* ag_copy(AgObject* src) {
	AgObject* dst = ag_copy_object_field(src, 0);
	for (AgObject* obj = ag_copy_head; obj;) {
		if (AG_PTR_TAG(obj) == AG_TG_NOWEAK_DST) {
			AgObject* dst = AG_UNTAG_PTR(AgObject, obj);
			obj = (AgObject*) dst->ctr_mt;
			dst->ctr_mt = AG_CTR_STEP;  // it's a copy, so no mt
		} else {
			assert(AG_PTR_TAG(obj) == AG_TG_OBJECT);
			obj = AG_UNTAG_PTR(AgObject, obj);
			assert(AG_PTR_TAG(obj->wb_p) == AG_TG_WEAK_BLOCK);
			AgWeak* wb = (AgWeak*) obj->wb_p;
			void* next = wb->target;
			wb->target = obj;
			while (AG_PTR_TAG(next) == AG_TG_WEAK) {
				void** w = AG_UNTAG_PTR(void*, next);
				ag_retain_weak(wb);
				next = *w;
				*w = wb;
			}
			obj = (AgObject*) next;
		}
	}
	ag_copy_head = 0;
	while (ag_copy_fixers_count) {  // TODO retain objects in the copy_fixers vector.
		AgCopyFixer* f = ag_copy_fixers + --ag_copy_fixers_count;
		f->fixer(f->data);
	}
	return dst;
}

AgObject* ag_copy_object_field(AgObject* src, AgObject* parent) {
	if (ag_copy_freeze)
		parent = (AgObject*) AG_SHARED;
	if (!src || (size_t)src < 256)
		return src;
	AgVmt* vmt = ((AgVmt*)(ag_head(src)->dispatcher)) - 1;
	AgObject* dh = (AgObject*) ag_alloc(vmt->instance_alloc_size + AG_HEAD_SIZE);
	if (!dh) { exit(-42); }
	ag_memcpy(dh, ag_head(src), vmt->instance_alloc_size + AG_HEAD_SIZE);
	dh->ctr_mt = AG_CTR_STEP;
	dh->wb_p = (uintptr_t) AG_TAG_PTR(AgObject, parent, AG_F_PARENT);  //NO_WEAK also makes it AG_TG_NOWEAK_DST
	vmt->copy_ref_fields((AgObject*)(dh + AG_HEAD_SIZE), src);
	if ((ag_head(src)->wb_p & AG_F_PARENT) == 0) { // has weak block
		AgWeak* wb = (AgWeak*) ag_head(src)->wb_p;
		if (wb->thread != ag_current_thread) {
			// TODO: implement hashmap lookup or tree splicing array lookup
			// So far cross-thread weak pointers to shared objects are not maintain topology.
			// I'm not sure if this should be a bug or feature.
		} else {
			if (wb->target == src) { // no weak copied yet
				dh->ctr_mt = (uintptr_t)ag_copy_head;  // AG_TG_NOWEAK_DST uses counter as link
				ag_copy_head = AG_TAG_PTR(AgObject, src, AG_TG_OBJECT);
			} else {
				AgWeak* dst_wb = (AgWeak*)ag_alloc(sizeof(AgWeak));
				dst_wb->org_pointer_to_parent = (uintptr_t)parent;
				dst_wb->thread = ((AgWeak*)(ag_head(src)->wb_p))->thread;
				dh->wb_p = (uintptr_t)dst_wb;  // also clears NO_WEAK
				void* i = ((AgWeak*)(ag_head(src)->wb_p))->target;
				uintptr_t dst_wb_locks = AG_CTR_STEP;
				while (AG_PTR_TAG(i) == AG_TG_WEAK) {
					AgWeak** w = AG_UNTAG_PTR(AgWeak*, i);
					i = *w;
					*w = dst_wb;
					dst_wb_locks++;
				}
				dst_wb->wb_ctr_mt = dst_wb_locks;
				dst_wb->target = (AgObject*)i;
			}
			wb->target = AG_TAG_PTR(AgObject, dh + AG_HEAD_SIZE, AG_TG_OBJECT);
		}
	}
	return (AgObject*)(dh + AG_HEAD_SIZE);
}

void ag_fn_sys_make_shared(AgObject* obj) {  // TODO: implement hierarchy freeze
	if ((ag_head(obj)->wb_p & AG_F_PARENT) != 0) {
		ag_head(obj)->wb_p = (uintptr_t)AG_SHARED | AG_F_PARENT;
	} else {
		AgWeak* wb = (AgWeak*)(ag_head(obj)->wb_p);
		wb->org_pointer_to_parent = AG_SHARED;
	}
}

void ag_copy_weak_field(void** dst, AgWeak* src) {
	if (!src || (size_t)src < 256) {
		*dst = src;
	} else if (!src->target || src->thread != ag_current_thread) {
		ag_retain_weak_nn(src);
		*dst = src;
	} else {
		switch (AG_PTR_TAG(src->target)) {
		case AG_TG_WEAK_BLOCK: // tagWB == 0, so this object hasn't been copied yet or not copied at all
			*dst = ag_copy_head;
			ag_copy_head = AG_TAG_PTR(AgObject, src->target, AG_TG_OBJECT);
			src->target = AG_TAG_PTR(AgObject, dst, AG_TG_WEAK);
			break;
		case AG_TG_WEAK: // already accessed by weak in this copy
			*dst = src->target;
			src->target = AG_TAG_PTR(AgObject, dst, AG_TG_WEAK);
			break;
		case AG_TG_OBJECT: { // already copied
			AgObject* copy = AG_UNTAG_PTR(AgObject, src->target);
			AgWeak* cwb;
			if (ag_head(copy)->wb_p & AG_F_PARENT) {
				cwb = (AgWeak*) ag_alloc(sizeof(AgWeak));
				cwb->thread = src->thread;
				cwb->org_pointer_to_parent = ag_head(copy)->wb_p & ~AG_F_PARENT;
				cwb->wb_ctr_mt = AG_CTR_STEP | AG_CTR_WEAK;
				cwb->target = (AgObject*)(ag_head(copy)->ctr_mt);
				ag_head(copy)->wb_p = (uintptr_t) cwb;
			} else
				cwb = AG_UNTAG_PTR(AgWeak, ag_head(copy)->wb_p);
			cwb->wb_ctr_mt += AG_CTR_STEP;  // dst is not shared mt
			*dst = cwb;
			break; }
		}
	}
}

AgWeak* ag_mk_weak(AgObject* obj) { // obj can't be null
	if (ag_head(obj)->wb_p & AG_F_PARENT) {
		AgWeak* w = (AgWeak*) ag_alloc(sizeof(AgWeak));
		w->org_pointer_to_parent = ag_head(obj)->wb_p & ~AG_F_PARENT;
		w->target = obj;
		w->wb_ctr_mt = AG_CTR_STEP * 2; // one from obj and one from `mk_weak` result
		w->thread = ag_current_thread;
		ag_head(obj)->wb_p = (uintptr_t) w;
		return w;
	}
	AgWeak* w = (AgWeak*)(ag_head(obj)->wb_p);
	ag_retain_weak_nn(w);
	return w;
}

AgObject* ag_deref_weak(AgWeak* w) {
	if (!w || (size_t)w < 256 || !w->target || w->thread != ag_current_thread) {
		return 0;
	}
	w->target->ctr_mt += AG_CTR_STEP;
	return w->target;
}

void ag_reg_copy_fixer(AgObject* object, void (*fixer)(AgObject*)) {
	if (ag_copy_fixers_count == ag_copy_fixers_alloc) {  // TODO retain objects in copy_fixers vector.
		ag_copy_fixers_alloc = ag_copy_fixers_alloc * 2 + 16;
		AgCopyFixer* new_dt = (AgCopyFixer*) AG_ALLOC(sizeof(AgCopyFixer) * ag_copy_fixers_alloc);
		if (ag_copy_fixers) {
			ag_memcpy(new_dt, ag_copy_fixers, sizeof(AgCopyFixer) * ag_copy_fixers_count);
			AG_FREE(ag_copy_fixers);
		}
		ag_copy_fixers = new_dt;
	}
	AgCopyFixer* f = ag_copy_fixers + ag_copy_fixers_count++;
	f->fixer = fixer;
	f->data = object;
}

int32_t ag_m_sys_String_getCh(AgString* s) {
	return s->ptr && *s->ptr
		? get_utf8(&s->ptr)
		: 0;
}

void ag_copy_sys_String(AgString* d, AgString* s) {
	d->ptr = s->ptr;
	d->buffer = s->buffer;
	if (d->buffer)
		d->buffer->counter++;
}

void ag_dtor_sys_String(AgString* s) {
	if (s->buffer && --s->buffer->counter == 0)
		ag_free(s->buffer);
}

int64_t ag_m_sys_Container_capacity(AgBlob* b) {
	return b->size;
}

void ag_m_sys_Container_insertItems(AgBlob* b, uint64_t index, uint64_t count) {
	if (!count || index > b->size)
		return;
	int64_t* new_data = (int64_t*) ag_alloc(sizeof(int64_t) * (b->size + count));
	ag_memcpy(new_data, b->data, sizeof(int64_t) * index);
	ag_zero_mem(new_data + index, sizeof(int64_t) * count);
	ag_memcpy(new_data + index + count, b->data + index, sizeof(int64_t) * (b->size - index));
	ag_free(b->data);
	b->data = new_data;
	b->size += count;
}

void ag_m_sys_Blob_deleteBytes(AgBlob* b, uint64_t index, uint64_t bytes_count) {
	if (!bytes_count || index > b->size * sizeof(int64_t) || index + bytes_count > b->size * sizeof(int64_t))
		return;
	size_t new_byte_size = (b->size * sizeof(int64_t) - bytes_count + 7) & ~7;
	int64_t* new_data = (int64_t*) ag_alloc(new_byte_size);
	ag_memcpy(new_data, b->data, index);
	ag_memcpy((char*)new_data + index, (char*)b->data + index + bytes_count, (b->size * sizeof(int64_t) - index));
	ag_free(b->data);
	b->data = new_data;
	b->size -= new_byte_size >> 3;
}

void ag_m_sys_Array_delete(AgBlob* b, uint64_t index, uint64_t count) {
	if (!count || index > b->size || index + count > b->size)
		return;
	AgObject** data = ((AgObject**)(b->data)) + index;
	for (uint64_t i = count; i != 0; i--, data++) {
		ag_release_own(*data);
	}
	ag_m_sys_Blob_deleteBytes(b, index * sizeof(int64_t), count * sizeof(int64_t));
}

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

bool ag_m_sys_Container_moveItems(AgBlob* blob, uint64_t a, uint64_t b, uint64_t c) {
	if (a >= b || b >= c || c > blob->size)
		return false;
	uint64_t* temp = (uint64_t*) ag_alloc(sizeof(uint64_t) * (b - a));
	ag_memmove(temp, blob->data + a, sizeof(uint64_t) * (b - a));
	ag_memmove(blob->data + a, blob->data + b, sizeof(uint64_t) * (c - b));
	ag_memmove(blob->data + a + (c - b), temp, sizeof(uint64_t) * (b - a));
	ag_free(temp);
	return true;
}
int64_t ag_m_sys_Blob_get8At(AgBlob* b, uint64_t index) {
	return index / sizeof(int64_t) < b->size
		? ((uint8_t*)(b->data))[index]
		: 0;
}

void ag_m_sys_Blob_set8At(AgBlob* b, uint64_t index, int64_t val) {
	if (index / sizeof(int64_t) < b->size)
		((uint8_t*)(b->data))[index] = (uint8_t)val;
}

int64_t ag_m_sys_Blob_get16At(AgBlob* b, uint64_t index) {
	return index / sizeof(int64_t) * sizeof(int16_t) < b->size
		? ((uint16_t*)(b->data))[index]
		: 0;
}

void ag_m_sys_Blob_set16At(AgBlob* b, uint64_t index, int64_t val) {
	if (index / sizeof(int64_t) * sizeof(int16_t) < b->size)
		((uint16_t*)(b->data))[index] = (uint16_t)val;
}

int64_t ag_m_sys_Blob_get32At(AgBlob* b, uint64_t index) {
	return index / sizeof(int64_t) * sizeof(int32_t) < b->size
		? ((uint32_t*)(b->data))[index]
		: 0;
}

void ag_m_sys_Blob_set32At(AgBlob* b, uint64_t index, int64_t val) {
	if (index / sizeof(int64_t) * sizeof(int32_t) < b->size)
		((uint32_t*)(b->data))[index] = (uint32_t)val;
}

int64_t ag_m_sys_Blob_get64At(AgBlob* b, uint64_t index) {
	return index < b->size ? b->data[index] : 0;
}

void ag_m_sys_Blob_set64At(AgBlob* b, uint64_t index, int64_t val) {
	if (index < b->size)
		b->data[index] = val;
}

bool ag_m_sys_Blob_copyBytesTo(AgBlob* dst, uint64_t dst_index, AgBlob* src, uint64_t src_index, uint64_t bytes) {
	if ((src_index + bytes) / sizeof(int64_t) >= src->size || (dst_index + bytes) / sizeof(int64_t) >= dst->size)
		return false;
	ag_memmove(((uint8_t*)(dst->data)) + dst_index, ((uint8_t*)(src->data)) + src_index, bytes);
	return true;
}

AgObject* ag_m_sys_Array_getAt(AgBlob* b, uint64_t index) {
	return index < b->size
		? ag_retain(((AgObject*)(b->data)[index]))
		: 0;
}

AgWeak* ag_m_sys_WeakArray_getAt(AgBlob* b, uint64_t index) {
	return index < b->size
		? ag_retain_weak((AgWeak*)(b->data[index]))
		: 0;
}

void ag_m_sys_Array_setOptAt(AgBlob* b, uint64_t index, AgObject* val) {
	if (index < b->size) {
		AgObject** dst = ((AgObject**)(b->data)) + index;
		ag_retain_own(val, &b->head);
		ag_release_own(*dst);
		*dst = val;
	}
}

AgObject* ag_m_sys_Array_setAt(AgBlob* b, uint64_t index, AgObject* val) {
	if (index >= b->size) {
		ag_head(val)->ctr_mt += AG_CTR_STEP;
		return val;
	}
	AgObject** dst = ((AgObject**)(b->data)) + index;
	ag_head(val)->ctr_mt += AG_CTR_STEP * 2;
	ag_set_parent_nn(val, &b->head);
	ag_release_own(*dst);
	*dst = val;
	return val;
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

void ag_m_sys_WeakArray_setAt(AgBlob* b, uint64_t index, AgWeak* val) {
	if (index < b->size) {
		AgWeak** dst = ((AgWeak**)(b->data)) + index;
		ag_retain_weak(val);
		ag_release_weak(*dst);
		*dst = val;
	}
}

void ag_copy_sys_Blob(AgBlob* d, AgBlob* s) {
	d->size = s->size;
	d->data = (int64_t*) ag_alloc(sizeof(int64_t) * d->size);
	ag_memcpy(d->data, s->data, sizeof(int64_t) * d->size);
}

void ag_copy_sys_Container(AgBlob* d, AgBlob* s) {
	ag_copy_sys_Blob(d, s);
}

void ag_copy_sys_Array(AgBlob* d, AgBlob* s) {
	d->size = s->size;
	d->data = (int64_t*) ag_alloc(sizeof(int64_t) * d->size);
	for (AgObject
			**from = (AgObject**) (s->data),
			**to =   (AgObject**) (d->data),
			**term = from + d->size;
		from < term;
		from++, to++)
	{
		*to = ag_copy_object_field(*from, &d->head);
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

void ag_dtor_sys_Blob(AgBlob* p) {
	ag_free(p->data);
}
void ag_dtor_sys_Container(AgBlob* p) {
	ag_dtor_sys_Blob(p);
}

void ag_dtor_sys_Array(AgBlob* p) {
	for (AgObject
			**ptr = (AgObject**)(p->data),
			**to = ptr + p->size;
		ptr < to;
		ptr++)
	{
		ag_release_own(*ptr);
	}
	ag_free(p->data);
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

bool ag_m_sys_String_fromBlob(AgString* s, AgBlob* b, int at, int count) {
	if ((at + count) / sizeof(uint64_t) >= b->size)
		return false;
	if (s->buffer && --s->buffer->counter == 0)
		ag_free(s->buffer);
	s->buffer = (AgStringBuffer*) ag_alloc(sizeof(AgStringBuffer) + count);
	s->buffer->counter = 1;
	ag_memcpy(s->buffer->data, ((char*)(b->data)) + at, count);
	s->buffer->data[count] = 0;
	s->ptr = s->buffer->data;
	return true;
}

static int ag_put_fn(void* ctx, int b) {
	char** c = (char**)ctx;
	**c = b;
	(*c)++;
	return 1;
}

int64_t ag_m_sys_Blob_putChAt(AgBlob* b, int at, int codepoint) {
	char* cursor = ((char*)(b->data)) + at;
	if (at + 5 > b->size * sizeof(uint64_t))
		return 0;
	put_utf8(codepoint, &cursor, ag_put_fn);
	return cursor - (char*)(b->data);
}

AgObject* ag_fn_sys_getParent(AgObject* obj) {  // obj not null, result is nullable
	uintptr_t r = obj->wb_p & AG_F_PARENT
		? obj->wb_p & ~AG_F_PARENT
		: ((AgWeak*)obj->wb_p)->org_pointer_to_parent;
	return r <= AG_SHARED
		? 0
		: ag_retain((AgObject*)(r));
}

void ag_fn_sys_terminate(int result) {
	exit(result);
}

void ag_fn_sys_log(AgString* s) {
	fputs(s->ptr, stdout);
}

void ag_make_blob_fit(AgBlob* b, size_t required_size) {
	required_size = (required_size + sizeof(int64_t) - 1) / sizeof(int64_t);
	if (b->size < required_size)
		ag_m_sys_Container_insertItems(b, b->size, required_size - b->size);
}

int64_t ag_fn_sys_readFile(AgString* name, AgBlob* content) {
/*	FILE* f = fopen(name->ptr, "rb");
	fseek(f, 0, SEEK_END);
	int64_t size = ftell(f);
	fseek(f, 0, SEEK_SET);
	ag_make_blob_fit(content, size);
	int64_t read_size = fread(content->data, 1, size, f);
	fclose(f);
	return read_size == size ? size : -1;
*/
	return -1;
}
bool ag_fn_sys_writeFile(AgString* name, int64_t at, int64_t byte_size, AgBlob* content) {
/*	FILE* f = fopen(name->ptr, "wb");
	if (!f) return -1;
	int64_t size_written = fwrite((char*)content->data + at, 1, byte_size, f);
	fclose(f);
	return size_written == byte_size;
*/
	return false;
}

bool ag_fn_sys_setMainObject(AgObject* s) {
	ag_thread* th = &ag_main_thread; // todo reuse this code in Thread.launch
	if (!th->queue_start) {
		th->queue_start = AG_ALLOC(sizeof(int64_t) * AG_THREAD_QUEUE_SIZE);
		th->queue_end = th->queue_start + AG_THREAD_QUEUE_SIZE;
		mtx_init(&th->mutex, mtx_plain);
		cnd_init(&th->is_not_empty);
		th->read_pos = th->write_pos = th->queue_start;
	}
	ag_release(th->root);
	if (s && ag_fn_sys_getParent(s)) {
		th->root = NULL;
		return false;
	}
	th->root = ag_retain(s);
	return true;
}

uint64_t ag_get_thread_param(ag_thread* th) {
	uint64_t r = *th->read_pos;
	if (++th->read_pos == th->queue_end)
		th->read_pos = th->queue_start;
	return r;
}

void ag_unlock_thread_queue(ag_thread* th) {
	mtx_unlock(&th->mutex);
}

bool ag_fn_sys_postTimer(int64_t at, AgWeak* receiver, ag_fn fn) {
	ag_thread* th = receiver->thread;
	if (!th)
		return false;
	th->timer_ms = at;
	th->timer_proc = fn;
	th->timer_proc_param = receiver;
	ag_finalize_post_message(th);
	return true;
}

static ag_thread* ag_lock_thread(AgWeak* receiver) {
	ag_thread* th = (ag_thread*)receiver->thread;
	if (!th)
		return NULL;
	mtx_lock(&th->mutex);
	if (receiver->thread != th) { // thread had died or object moved while we were locking it
		mtx_unlock(&th->mutex);
		return NULL;
	}
	return th;
}
// returns locked thread or NULL if receiver's thread is dead
ag_thread* ag_prepare_post_message(AgWeak* receiver, ag_fn fn, ag_trampoline tramp, size_t params_count) {
	ag_thread* th = ag_lock_thread(receiver);
	if (!th)
		return NULL;
	size_t free_space = th->read_pos > th->write_pos
		? th->write_pos - th->read_pos
		: (th->queue_end - th->queue_start) - (th->write_pos - th->read_pos);
	if (free_space < params_count + 3) { // params + trampoline + entry_point + receiver_weak
		uint64_t new_size = (th->queue_end - th->queue_start) * 2 + params_count + 3;
		uint64_t* new_buf = AG_ALLOC(sizeof(uint64_t) * new_size);
		if (!new_buf)
			exit(-42);
		if (th->read_pos > th->write_pos) {
			size_t r_size = th->queue_end - th->read_pos;
			size_t w_size = th->write_pos - th->queue_start;
			memcpy(new_buf + new_size - r_size, th->read_pos, sizeof(uint64_t) * (r_size));
			memcpy(new_buf, th->queue_start, sizeof(uint64_t) * (w_size));
			AG_FREE(th->queue_start);
			th->read_pos = new_buf + new_size - r_size;
			th->write_pos = new_buf + w_size;
		} else {
			size_t size = th->write_pos - th->read_pos;
			memcpy(new_buf, th->read_pos, sizeof(uint64_t) * size);
			AG_FREE(th->queue_start);
			th->read_pos = new_buf;
			th->write_pos = new_buf + size;
		}
		th->queue_start = new_buf;
		th->queue_end = new_buf + new_size;
	}
	ag_put_thread_param(th, (uint64_t) tramp);
	ag_put_thread_param(th, (uint64_t) receiver); // It comes prelocked. Caller doesn't release it
	ag_put_thread_param(th, (uint64_t) fn);
	return th;
}

void ag_put_thread_param(ag_thread* th, uint64_t param) {
	*th->write_pos = param;
	if (++th->write_pos == th->queue_end)
		th->write_pos = th->queue_start;
}

void ag_finalize_post_message(ag_thread* th) {
	mtx_unlock(&th->mutex);
	cnd_signal(&th->is_not_empty);
}

void ag_thread_proc(ag_thread* th) {
	struct timespec now;
	mtx_lock(&th->mutex);
	while (th->root) {
		if (th->read_pos != th->write_pos) {
			uint64_t tramp = ag_get_thread_param(th);
			if (!tramp) {
				mtx_unlock(&th->mutex);
				AgObject* r = th->root;
				th->root = NULL;
				ag_release(r);
				ag_get_thread_param(th); // skip
				if (ag_get_thread_param(th)) // hard quit
					return;
				if (th->timer_ms) {
					ag_release_weak(th->timer_proc_param);
					th->timer_ms = 0;
				}
			} else {
				AgWeak* w_receiver = (AgWeak*)ag_get_thread_param(th);
				ag_fn entry_point = (ag_fn)ag_get_thread_param(th);
				AgObject* receiver = ag_deref_weak(w_receiver);
				((ag_trampoline)tramp)(receiver, entry_point, th); // it unlocks mutex internally
				ag_release(receiver);
				ag_release_weak(w_receiver);
			}
			mtx_lock(&th->mutex);
		} else if (th->timer_ms && timespec_get(&now, TIME_UTC) && timespec_to_ms(&now) <= th->timer_ms) {
			th->timer_ms = 0;
			AgObject* timer_object = ag_deref_weak(th->timer_proc_param);
			if (timer_object) {
				mtx_unlock(&th->mutex);
				th->timer_proc(timer_object);
				ag_release(timer_object);
				mtx_lock(&th->mutex);
			}
		} else if (th->timer_ms) {
			struct timespec timeout;
			timeout.tv_sec = th->timer_ms / 1000;
			timeout.tv_nsec = th->timer_ms % 1000 * 1000000;
			cnd_timedwait(&th->is_not_empty, &th->mutex, &timeout);
		} else {
			cnd_wait(&th->is_not_empty, &th->mutex);
		}
	}
	mtx_unlock(&th->mutex);
}

int ag_handle_main_thread() {
	if (ag_main_thread.root)
		ag_thread_proc(&ag_main_thread);
	return 0;
}

#ifdef AG_ENTRY_POINT

extern void ag_main();

int main() {
	ag_main();
	return ag_handle_main_thread();
}
#endif
