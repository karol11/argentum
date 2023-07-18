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
		arg,                    // argument to thread function 
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

typedef struct ag_queue_tag {
	int64_t* start;
	int64_t* end;
	int64_t* read_pos;
	int64_t* write_pos;
} ag_queue;

typedef struct ag_thread_tag {
	ag_queue  in;
	ag_queue  out;
	AgObject* root;     // 0 if free
	uint64_t  timer_ms;  // todo: replace with pyramid-heap
	ag_fn     timer_proc;
	AgWeak*   timer_proc_param; // next free if free
	mtx_t     mutex;
	cnd_t     is_not_empty;
	thrd_t    thread;
} ag_thread;

// Ag_threads never deallocated.
// We allocate ag_threads by pages, we hold deallocated pages in a list using write_pos field
ag_thread*  ag_alloc_thread = NULL;    // next free ag_thread in page
uint64_t    ag_alloc_threads_left = 0; // number of free ag_threads left in page
ag_thread*  ag_thread_free = NULL;     // head of freed ag_thread chain
mtx_t ag_threads_mutex;

ag_thread ag_main_thread = { 0 };

#define AG_RETAIN_BUFFER_SIZE 8192
AG_THREAD_LOCAL ag_thread* ag_current_thread = NULL;
AG_THREAD_LOCAL uintptr_t* ag_retain_buffer = NULL;
AG_THREAD_LOCAL uintptr_t* ag_retain_pos;
AG_THREAD_LOCAL uintptr_t* ag_release_pos;
mtx_t ag_retain_release_mutex;

void ag_flush_retain_release() {
	mtx_lock(&ag_retain_release_mutex);
	uintptr_t* i = ag_retain_buffer;
	uintptr_t* term = ag_retain_pos;
	for (; i != term; ++i) {
		if (*i & 1)
			((AgStringBuffer*)(*i & ~1))->counter_mt += 2;
		else
			((AgObject*)*i)->ctr_mt += AG_CTR_STEP;
	}
	i = ag_release_pos;
	term = ag_retain_buffer + AG_RETAIN_BUFFER_SIZE;
	AgObject* root = NULL;
	for (; i != term; ++i) {
		if (*i & 1) {
			AgStringBuffer* str = (AgStringBuffer*)(*i & ~1);
			if ((str->counter_mt -= 2) < 2)
				ag_free(str);

		} else {
			AgObject* obj = (AgObject*)*i;
			if ((obj->ctr_mt -= AG_CTR_STEP) < AG_CTR_STEP) {
				obj->ctr_mt = (obj->ctr_mt & AG_CTR_WEAK) | ((intptr_t)root);
				root = obj;
			}
		}
	}
	ag_retain_pos = ag_retain_buffer;
	ag_release_pos = ag_retain_buffer + AG_RETAIN_BUFFER_SIZE;
	mtx_unlock(&ag_retain_release_mutex);
	while (root) {
		AgObject* n = AG_UNTAG_PTR(AgObject, root->ctr_mt);
		if (root->ctr_mt & AG_CTR_WEAK)
			ag_free(root);
		else
			ag_dispose_obj(AG_UNTAG_PTR(AgObject, root));
		root = n;
	}
}

inline void ag_set_parent_nn(AgObject* obj, AgObject* parent) {
	if (obj->wb_p & AG_F_PARENT)
		obj->wb_p = (uintptr_t)parent | AG_F_PARENT;
	else
		((AgWeak*)obj->wb_p)->org_pointer_to_parent = (uintptr_t)parent;
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
		assert((ag_head(obj)->ctr_mt & AG_CTR_MT) == 0);  // pin cannot be shared and as such mt
		if ((ag_head(obj)->ctr_mt -= AG_CTR_STEP) == 0)
			ag_dispose_obj(obj);
	}
}
inline AgObject* ag_retain_pin(AgObject* obj) {
	if (ag_not_null(obj)) {
		assert((ag_head(obj)->ctr_mt & AG_CTR_MT) == 0);  // pin cannot be shared and as such mt
		ag_head(obj)->ctr_mt += AG_CTR_STEP;
	}
	return obj;
}
inline void ag_reg_mt_release(uintptr_t p) {
	if (--ag_release_pos == ag_retain_pos)
		ag_flush_retain_release();
	*ag_release_pos = p;
}
inline void ag_reg_mt_retain(uintptr_t p) {
	*ag_retain_pos = p;
	if (++ag_retain_pos == ag_release_pos)
		ag_flush_retain_release();
}
void ag_release_weak(AgWeak* w) {
	if (!ag_not_null(w))
		return;
	if (w->wb_ctr_mt & AG_CTR_MT) {
		ag_reg_mt_release((uintptr_t)w);
	} else if ((w->wb_ctr_mt -= AG_CTR_STEP) == AG_CTR_WEAK) {
		ag_free(w);
	}
}
inline AgWeak* ag_retain_weak_nn(AgWeak* w) {
	if (w->wb_ctr_mt & AG_CTR_MT) {
		ag_reg_mt_retain((uintptr_t)w);
	} else {
		w->wb_ctr_mt += AG_CTR_STEP;
	}
	return w;
}
void ag_retain_weak(AgWeak* w) {
	if (ag_not_null(w))
		ag_retain_weak_nn(w);
}
// TODO: separate into
// - `assign_own`, that handles previous val, and parent ptr, but doesn't handle mt
// - `dispose_own` that handles mt but not parent ptr
void ag_release_own(AgObject* obj) {
	if (ag_not_null(obj)) {
		if (ag_head(obj)->ctr_mt & AG_CTR_MT)  // when in field it can point to a frozen shared mt.
			ag_reg_mt_release((uintptr_t) obj);
		else if ((ag_head(obj)->ctr_mt -= AG_CTR_STEP) < AG_CTR_STEP)
			ag_dispose_obj(obj);
		else
			ag_set_parent_nn(obj, AG_IN_STACK);
	}
}
void ag_retain_own(AgObject* obj, AgObject* parent) {
	if (ag_not_null(obj)) {
		if (ag_head(obj)->ctr_mt & AG_CTR_MT) {  // when in field it can point to a frozen shared mt
			ag_reg_mt_retain((uintptr_t)obj);
		} else {
			ag_head(obj)->ctr_mt += AG_CTR_STEP;
			ag_set_parent_nn(obj, parent);
		}
	}
}
void ag_release_shared(AgObject* obj) {
	if (ag_not_null(obj)) {
		if (ag_head(obj)->ctr_mt & AG_CTR_MT)
			ag_reg_mt_release((uintptr_t)obj);
		else if ((ag_head(obj)->ctr_mt -= AG_CTR_STEP) == 0)
			ag_dispose_obj(obj);
	}
}
void ag_retain_shared(AgObject* obj) {
	if (ag_not_null(obj)) {
		if (ag_head(obj)->ctr_mt & AG_CTR_MT)
			ag_reg_mt_retain((uintptr_t)obj);
		else
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

AG_THREAD_LOCAL AgObject*    ag_copy_head = 0;
AG_THREAD_LOCAL size_t       ag_copy_fixers_count = 0;
AG_THREAD_LOCAL size_t       ag_copy_fixers_alloc = 0;
AG_THREAD_LOCAL AgCopyFixer* ag_copy_fixers = 0;        // Used only for objects with manual afterCopy operators.
AG_THREAD_LOCAL bool         ag_copy_freeze = false;

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
			// So far cross-thread weak pointers to shared objects do not maintain topology.
			// I'm not sure if this should be a bug.
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
				uintptr_t dst_wb_locks = AG_CTR_STEP | AG_CTR_WEAK;
				while (AG_PTR_TAG(i) == AG_TG_WEAK) {
					AgWeak** w = AG_UNTAG_PTR(AgWeak*, i);
					i = *w;
					*w = dst_wb;
					dst_wb_locks += AG_CTR_STEP;
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
		w->wb_ctr_mt = AG_CTR_WEAK | (AG_CTR_STEP * 2); // one from obj and one from `mk_weak` result
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
		if (!new_dt)
			exit(-42);
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
	if (d->buffer) {
		if (d->buffer->counter_mt & 1)
			ag_reg_mt_retain((uintptr_t)d->buffer | 1);
		else
			d->buffer->counter_mt += 2;
	}
}

void ag_visit_sys_String(
	AgString* s,
	void(*visitor)(void*, int, void*),
	void* ctx)
{
	if (ag_not_null(s) && s->buffer)
		visitor(s->buffer, AG_VISIT_STRING_BUF, ctx);
}

void ag_dtor_sys_String(AgString* s) {
	if (s->buffer) {
		if (s->buffer->counter_mt & 1)
			ag_reg_mt_release((uintptr_t)s->buffer | 1);
		else if ((s->buffer->counter_mt -= 2) < 2)
			ag_free(s->buffer);
	}
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
		? ag_retain_pin(((AgObject*)(b->data)[index]))
		: 0;
}

AgWeak* ag_m_sys_WeakArray_getAt(AgBlob* b, uint64_t index) {
	if (index >= b->size)
		return 0;
	AgWeak* r = (AgWeak*)(b->data[index]);
	ag_retain_weak(r);
	return r;
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

void ag_visit_sys_Blob(
	void* ptr,
	void(*visitor)(void*, int, void*),
	void* ctx)
{}

void ag_copy_sys_Container(AgBlob* d, AgBlob* s) {
	ag_copy_sys_Blob(d, s);
}

void ag_visit_sys_Container(
	void* ptr,
	void(*visitor)(void*, int, void*),
	void* ctx)
{}

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

void ag_visit_sys_Array(
	AgBlob* arr,
	void(*visitor)(void*, int, void*),
	void* ctx)
{
	if (ag_not_null(arr)) {
		for (AgObject
				** from = (AgObject**)(arr->data),
				** to = (AgObject**)(arr->data),
				** term = from + arr->size;
			from < term;
			from++, to++)
		{
			visitor(from, AG_VISIT_OWN, ctx);
		}
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
		for (AgObject
			** from = (AgObject**)(arr->data),
			**to = (AgObject**)(arr->data),
			**term = from + arr->size;
			from < term;
			from++, to++) {
			visitor(from, AG_VISIT_WEAK, ctx);
		}
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

void ag_release_str(AgString* s) {
	if (!s->buffer)
		return;
	if (s->buffer->counter_mt & 1)
		ag_reg_mt_release((uintptr_t)s);
	else if ((s->buffer->counter_mt -= 2) < 2)
		ag_free(s->buffer);
}

bool ag_m_sys_String_fromBlob(AgString* s, AgBlob* b, int at, int count) {
	if ((at + count) / sizeof(uint64_t) >= b->size)
		return false;
	ag_release_str(s);
	s->buffer = (AgStringBuffer*) ag_alloc(sizeof(AgStringBuffer) + count);
	s->buffer->counter_mt = 2;
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
		: ag_retain_pin((AgObject*)(r));
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

static void ag_init_queue(ag_queue* q) {
	q->read_pos = q->write_pos = q->start = AG_ALLOC(sizeof(int64_t) * AG_THREAD_QUEUE_SIZE);
	q->end = q->start + AG_THREAD_QUEUE_SIZE;
}

static void ag_init_thread(ag_thread* th) {
	ag_init_queue(&th->in);
	ag_init_queue(&th->out);
	mtx_init(&th->mutex, mtx_plain);
	cnd_init(&th->is_not_empty);
	th->root = NULL;
	th->timer_ms = 0;
	th->timer_proc = 0;
	th->timer_proc_param = 0;
}

bool ag_fn_sys_setMainObject(AgObject* s) {
	ag_thread* th = &ag_main_thread;
	if (!th->in.start)
		ag_init_thread(th);
	ag_release_own(th->root);
	if (s && ag_fn_sys_getParent(s)) {
		th->root = NULL;
		return false;
	}
	th->root = ag_retain_pin(s);
	return true;
}

void ag_resize_queue(ag_queue* q, size_t space_needed) {
	size_t free_space = q->read_pos > q->write_pos
		? q->write_pos - q->read_pos
		: (q->end - q->start) - (q->write_pos - q->read_pos);
	if (free_space < space_needed) {
		uint64_t new_size = (q->end - q->start) * 2 + space_needed;
		uint64_t* new_buf = AG_ALLOC(sizeof(uint64_t) * new_size);
		if (!new_buf)
			exit(-42);
		if (q->read_pos > q->write_pos) {
			size_t r_size = q->end - q->read_pos;
			size_t w_size = q->write_pos - q->start;
			memcpy(new_buf + new_size - r_size, q->read_pos, sizeof(uint64_t) * (r_size));
			memcpy(new_buf, q->start, sizeof(uint64_t) * (w_size));
			AG_FREE(q->start);
			q->read_pos = new_buf + new_size - r_size;
			q->write_pos = new_buf + w_size;
		} else {
			size_t size = q->write_pos - q->read_pos;
			memcpy(new_buf, q->read_pos, sizeof(uint64_t) * size);
			AG_FREE(q->start);
			q->read_pos = new_buf;
			q->write_pos = new_buf + size;
		}
		q->start = new_buf;
		q->end = new_buf + new_size;
	}
}

uint64_t ag_read_queue(ag_queue* q) {
	uint64_t r = *q->read_pos;
	if (++q->read_pos == q->end)
		q->read_pos = q->start;
	return r;
}

void ag_write_queue(ag_queue* q, uint64_t param) {
	*q->write_pos = param;
	if (++q->write_pos == q->end)
		q->write_pos = q->start;
}

uint64_t ag_get_thread_param(ag_thread* th) {  // for trampolines
	return ag_read_queue(&th->in);
}

void ag_unlock_thread_queue(ag_thread* th) { // for trampolines
	mtx_unlock(&th->mutex);
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

void ag_unlock_and_notify_thread(ag_thread* th) {
	mtx_unlock(&th->mutex);
	cnd_broadcast(&th->is_not_empty);
}

bool ag_fn_sys_postTimer(int64_t at, AgWeak* receiver, ag_fn fn) {
	ag_thread* th = ag_lock_thread(receiver);
	if (!th)
		return false;
	th->timer_ms = at;
	th->timer_proc = fn;
	th->timer_proc_param = receiver;
	ag_unlock_and_notify_thread(th);
	return true;
}

// returns mtx-locked thread or NULL if receiver's thread is dead
// receiver goes prelocked
ag_thread* ag_prepare_post_message(AgWeak* receiver, ag_fn fn, ag_trampoline tramp, size_t params_count) {
	ag_thread* th = ag_lock_thread(receiver);
	if (!th)
		return NULL;
	if (!ag_current_thread) {
		mtx_unlock(&th->mutex);
		return NULL; // todo add support for non-ag threads
	}
	// no need to lock out-queue thread b/c it's our thread
	ag_queue* q = &ag_current_thread->out;
	ag_resize_queue(
		q,
		params_count + 4);  // params_count + params + trampoline + entry_point + receiver_weak
	ag_put_thread_param(th, (uint64_t) tramp);
	ag_put_thread_param(th, (uint64_t) receiver); // if weak posted to another thread, it's already mt-marked, no need to mark it here
	ag_put_thread_param(th, (uint64_t) fn);
	ag_put_thread_param(th, (uint64_t) params_count);
	return th;
}

void ag_put_thread_param(ag_thread* th, uint64_t param) {
	if (th)
		ag_write_queue(&ag_current_thread->out, param);
}

void ag_finalize_post_message(ag_thread* th) {
	if (th)
		mtx_unlock(&th->mutex);
}

inline void ag_make_weak_mt(AgWeak* w) {
	if (ag_not_null(w) && (w->wb_ctr_mt & AG_CTR_MT) == 0) {
		w->wb_ctr_mt |= AG_CTR_MT;  //previously this weak belonged only to this thread, so no atomic op here
	}
}

void ag_bound_field_to_thread(void* field, int type, void* ctx);

inline void ag_bound_own_to_thread(AgObject* ptr, ag_thread* th) {
	if (ag_not_null(ptr)) {
		uintptr_t parent = 0;
		if ((ptr->wb_p & AG_F_PARENT) == 0) {
			parent = ptr->wb_p;
			AgWeak* w = (AgWeak*)ptr->wb_p;
			if (w->thread == th)
				return;
			if (w->wb_ctr_mt & AG_CTR_MT) // we can check this bit, but can't rewrite it if it's mt
				w->wb_ctr_mt |= AG_CTR_MT;
			w->thread = th;
		} else {
			parent = ptr->wb_p & ~AG_F_PARENT;
		}
		if (parent == AG_SHARED) {
			if ((ptr->ctr_mt & AG_CTR_MT) == 0) {
				ptr->ctr_mt |= AG_CTR_MT;  //previously it belonged only to this thread, so no atomic op here
				((AgVmt*)(ag_head(ptr)->dispatcher))[-1].visit(ptr, ag_bound_field_to_thread, th);
			}
		} else { // non-shared object strictly belongs to one thread, and can't be MT
			((AgVmt*)(ag_head(ptr)->dispatcher))[-1].visit(ptr, ag_bound_field_to_thread, th);
		}
	}
}

void ag_put_thread_param_weak_ptr(ag_thread* th, AgWeak* param) {
	if (ag_current_thread != th)
		ag_make_weak_mt(param);
	ag_put_thread_param(th, (uint64_t)param);
}

void ag_bound_field_to_thread(void* field, int type, void* ctx) {
	if (type == AG_VISIT_WEAK) {
		ag_make_weak_mt(*(AgWeak**)field);
	} else if (type == AG_VISIT_OWN){
		ag_bound_own_to_thread(*(AgObject**)field, (ag_thread*)ctx);
	} else if (type == AG_VISIT_STRING_BUF) {
		AgStringBuffer* buf = *(AgStringBuffer**)field;
		if ((buf->counter_mt & 1) == 0)
			buf->counter_mt |= 1;
	}
}
void ag_put_thread_param_own_ptr(ag_thread* th, AgObject* param) {
	if (ag_current_thread != th)
		ag_bound_own_to_thread(param, th);
	ag_put_thread_param(th, (uint64_t)param);
}

void ag_init_retain_buffer() {
	if (ag_retain_buffer)
		return;
	ag_retain_buffer = AG_ALLOC(sizeof(intptr_t) * AG_RETAIN_BUFFER_SIZE);
	ag_retain_pos = ag_retain_buffer;
	ag_release_pos = ag_retain_buffer + AG_RETAIN_BUFFER_SIZE;
}

int ag_thread_proc(ag_thread* th) {
	ag_current_thread = th;
	ag_init_retain_buffer();
	struct timespec now;
	mtx_lock(&th->mutex);
	for (;;) {
		if (th->in.read_pos != th->in.write_pos) {
			uint64_t tramp = ag_get_thread_param(th);
			if (!tramp) {
				mtx_unlock(&th->mutex);
				AgObject* r = th->root;
				th->root = NULL;
				ag_release_own(r);
				if (th->timer_ms) {
					ag_release_weak(th->timer_proc_param);
					th->timer_ms = 0;
				}
			} else {
				AgWeak* w_receiver = (AgWeak*)ag_get_thread_param(th);
				ag_fn entry_point = (ag_fn)ag_get_thread_param(th);
				AgObject* receiver = ag_deref_weak(w_receiver);
				((ag_trampoline)tramp)(receiver, entry_point, th); // it unlocks mutex internally
				ag_release_pin(receiver);
				ag_release_weak(w_receiver);
			}
			mtx_lock(&th->mutex);
		} else if (th->timer_ms && timespec_get(&now, TIME_UTC) && timespec_to_ms(&now) <= th->timer_ms) {
			th->timer_ms = 0;
			AgObject* timer_object = ag_deref_weak(th->timer_proc_param);
			if (timer_object) {
				mtx_unlock(&th->mutex);
				th->timer_proc(timer_object);
				ag_release_pin(timer_object);
				mtx_lock(&th->mutex);
			}
		} else if (th->out.read_pos != th->out.write_pos) {
			if (ag_retain_buffer != ag_retain_pos || ag_release_pos != ag_retain_buffer + AG_RETAIN_BUFFER_SIZE)
				ag_flush_retain_release();
			mtx_unlock(&th->mutex);
			ag_queue* out = &th->out;
			while (out->read_pos != out->write_pos) {
				uint64_t tramp = ag_read_queue(out);
				uint64_t recv = ag_read_queue(out);
				uint64_t fn = ag_read_queue(out);
				uint64_t count = ag_read_queue(out);
				ag_thread* out_th = ag_lock_thread((AgWeak*)recv);
				if (!out_th) {
					out_th = th; // send to myself to dispose
					mtx_lock(&th->mutex);
				}
				ag_queue* q = &out_th->in;
				ag_resize_queue(
					q,
					count + 3);  // trampoline + entry_point + receiver_weak + params
				ag_write_queue(q, (uint64_t)tramp);
				ag_write_queue(q, recv);
				ag_write_queue(q, fn);
				for (; count; --count)
					ag_write_queue(q, ag_read_queue(out));
				ag_unlock_and_notify_thread(out_th);
			}
			mtx_lock(&th->mutex);
		} else if (th->root) {
			if (th->timer_ms) {
				struct timespec timeout;
				timeout.tv_sec = th->timer_ms / 1000;
				timeout.tv_nsec = th->timer_ms % 1000 * 1000000;
				cnd_timedwait(&th->is_not_empty, &th->mutex, &timeout);
			} else {
				cnd_wait(&th->is_not_empty, &th->mutex);
			}
		} else {
			break;
		}
	}
	mtx_unlock(&th->mutex);
	ag_flush_retain_release();
	return 0;
}

int ag_handle_main_thread() {
	if (ag_main_thread.root) {
		ag_thread_proc(&ag_main_thread);
	}
	return 0;
}

AgThread* ag_m_sys_Thread_start(AgThread* th, AgObject* root) {
	ag_thread* t = NULL;
	if (!ag_retain_buffer) { // it's first thread creation
		ag_init_retain_buffer();
		mtx_init(&ag_retain_release_mutex, mtx_plain);
		mtx_init(&ag_threads_mutex, mtx_plain);
	}
	mtx_lock(&ag_threads_mutex);
	if (ag_thread_free) {
		t = ag_thread_free;
		ag_thread_free = (ag_thread*)ag_thread_free->timer_proc_param;
	} else {
		if (!ag_alloc_threads_left) {
			ag_alloc_threads_left = 16;
			ag_alloc_thread = AG_ALLOC(sizeof(ag_thread) * ag_alloc_threads_left);
			if (!ag_alloc_thread)
				exit(-42);
		}
		t = ag_alloc_thread++;
		ag_alloc_threads_left--;
		ag_init_thread(t);
	}
	mtx_unlock(&ag_threads_mutex);
	// TODO: make root object marker value for parent ptr.
	t->root = ag_retain_pin(root); // ok to retain on the crating thread before thrd_create
	th->thread = t;
	AgWeak* w = ag_mk_weak(root);
	w->wb_ctr_mt = (w->wb_ctr_mt - AG_CTR_STEP) | AG_CTR_MT;
	w->thread = t;
	thrd_create(&t->thread, ag_thread_proc, t);
	th->head.ctr_mt += AG_CTR_STEP;
	return th;
}

AgWeak* ag_m_sys_Thread_root(AgThread* th) {
	return ag_mk_weak(th->thread->root);
}
void ag_copy_sys_Thread(AgThread* dst, AgThread* src) {
	dst->thread = NULL;
}
void ag_dtor_sys_Thread(AgThread* ptr) {
	if (ptr->thread) {
		ag_thread* th = ptr->thread;
		mtx_lock(&th->mutex);
		ag_resize_queue(&th->in, 1);
		ag_write_queue(&th->in, 0);
		ag_unlock_and_notify_thread(th);
		int unused_result;
		thrd_join(th->thread, &unused_result);
		th->timer_proc_param = (AgWeak*) ag_thread_free;
		ag_thread_free = th;
	}
}
void ag_visit_sys_Thread(
	AgThread* ptr,
	void(*visitor)(void*, int, void*),
	void* ctx)
{}

void ag_init() {
	ag_current_thread = &ag_main_thread;
}
