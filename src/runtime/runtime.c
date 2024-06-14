#include <stddef.h> // size_t
#include <stdint.h> // int32_t int64_t
#include <stdio.h> // puts
#include <assert.h>
#include <time.h>  // timespec, timespec_get

#include "ag-threads.h"
#include "utf8.h"
#include "runtime.h"
#include "ag-queue.h"

//#define AG_RT_WITH_TRACE
#ifdef AG_RT_WITH_TRACE
#define AG_TRACE(msg, ...) printf("--%p " msg "\n", ag_current_thread, __VA_ARGS__)
#define AG_TRACE0(msg) printf("--%p " msg "\n", ag_current_thread)
#else
#define AG_TRACE(...)
#define AG_TRACE0(msg)
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

void* ag_alloc(size_t size) {
	size_t* r = (size_t*)AG_ALLOC(size);
	if (!r) {  // todo: add more handling
		exit(-42);
	}
	return r;
}
void ag_free(void* data) {
	if (data) {
		AG_FREE(data);
	}
}

#endif

typedef struct ag_thread_tag {
	ag_queue        in;
	ag_queue        out;
	AgObject*       root;     // 0 if free
	uint64_t        timer_ms;  // todo: replace with pyramid-heap
	ag_fn           timer_proc;
	AgWeak*         timer_proc_param; // next free if free
	pthread_mutex_t mutex;
	pthread_cond_t  is_not_empty;
	pthread_t       thread;
} ag_thread;

// Ag_threads never deallocated.
// We allocate ag_threads by pages, we hold deallocated pages in a list using timer_proc_param field
ag_thread*      ag_alloc_thread = NULL;    // next free ag_thread in page
uint64_t        ag_alloc_threads_left = 0; // number of free ag_threads left in page
ag_thread*      ag_thread_free = NULL;     // head of freed ag_thread chain
pthread_mutex_t ag_threads_mutex;

ag_thread ag_main_thread = { 0 };

#define AG_RETAIN_BUFFER_SIZE 8192
AG_THREAD_LOCAL ag_thread* ag_current_thread = NULL;
AG_THREAD_LOCAL uintptr_t* ag_retain_buffer = NULL;
AG_THREAD_LOCAL uintptr_t* ag_retain_pos;
AG_THREAD_LOCAL uintptr_t* ag_release_pos;
pthread_mutex_t            ag_retain_release_mutex;

void ag_flush_retain_release() {
	AG_TRACE0("flush [");
	pthread_mutex_lock(&ag_retain_release_mutex);
	uintptr_t* i = ag_retain_buffer;
	uintptr_t* term = ag_retain_pos;
	for (; i != term; ++i) {
		AG_TRACE("flush retain item=%p oldCtr=%p", (void*)*i, (void*)((AgObject*)*i)->ctr_mt);
		((AgObject*)*i)->ctr_mt += AG_CTR_STEP;
	}
	i = ag_release_pos;
	term = ag_retain_buffer + AG_RETAIN_BUFFER_SIZE;
	AgObject* root = NULL;
	for (; i != term; ++i) {
		AgObject* obj = (AgObject*)*i;
		AG_TRACE("flush release item=%p oldCtr=%p", obj, (void*)obj->ctr_mt);
		if ((obj->ctr_mt -= AG_CTR_STEP) < AG_CTR_STEP) {
			obj->ctr_mt = (obj->ctr_mt & AG_CTR_WEAK) | ((intptr_t)root);
			root = obj;
		}
	}
	ag_retain_pos = ag_retain_buffer;
	ag_release_pos = ag_retain_buffer + AG_RETAIN_BUFFER_SIZE;
	pthread_mutex_unlock(&ag_retain_release_mutex);
	while (root) {
		AG_TRACE("flush delete item=%p", root);
		AgObject* n = AG_UNTAG_PTR(AgObject, root->ctr_mt);
		if (root->ctr_mt & AG_CTR_WEAK)
			ag_free(root);
		else
			ag_dispose_obj(AG_UNTAG_PTR(AgObject, root));
		root = n;
	}
	AG_TRACE0("flush ]");
}

static inline void ag_set_parent_nn(AgObject* obj, AgObject* parent) {
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

void ag_release_pin_nn(AgObject* obj) {
	assert((ag_head(obj)->ctr_mt & AG_CTR_MT) == 0);  // pin cannot be shared and as such mt
	AG_TRACE("release pin obj=%p oldCtr=%p", obj, (void*)obj->ctr_mt);
	if ((ag_head(obj)->ctr_mt -= AG_CTR_STEP) < AG_CTR_STEP)
		ag_dispose_obj(obj);
}
void ag_release_pin(AgObject * obj) {
	if (ag_not_null(obj))
		ag_release_pin_nn(obj);
}
void ag_retain_pin_nn(AgObject* obj) {
	AG_TRACE("retain pin obj=%p oldCtr=%p", obj, (void*)obj->ctr_mt);
	assert((ag_head(obj)->ctr_mt & AG_CTR_MT) == 0);  // pin cannot be shared and as such mt
	ag_head(obj)->ctr_mt += AG_CTR_STEP;
}
void ag_retain_pin(AgObject* obj) {
	if (ag_not_null(obj))
		ag_retain_pin_nn(obj);
}
static inline void ag_reg_mt_release(uintptr_t p) {
	if (--ag_release_pos == ag_retain_pos)
		ag_flush_retain_release();
	*ag_release_pos = p;
}
static inline void ag_reg_mt_retain(uintptr_t p) {
	*ag_retain_pos = p;
	if (++ag_retain_pos == ag_release_pos)
		ag_flush_retain_release();
}
void ag_release_weak(AgWeak* w) {
	if (!ag_not_null(w))
		return;
	if (w->wb_ctr_mt & AG_CTR_MT) {
		AG_TRACE("release weak (dealyed) w=%p ctr~~%p", w, (void*)w->wb_ctr_mt);
		ag_reg_mt_release((uintptr_t)w);
	} else if ((w->wb_ctr_mt -= AG_CTR_STEP) < AG_CTR_STEP) {
		AG_TRACE("release weak (immediate) w=%p oldCtr=%p", w, (void*)w->wb_ctr_mt);
		ag_free(w);
	}
}
static inline AgWeak* ag_retain_weak_nn(AgWeak* w) {
	if (w->wb_ctr_mt & AG_CTR_MT) {
		AG_TRACE("retain weak (dealyed) w=%p ctr~~%p", w, (void*)w->wb_ctr_mt);
		ag_reg_mt_retain((uintptr_t)w);
	} else {
		AG_TRACE("retain weak (immediate) w=%p oldCtr=%p", w, (void*)w->wb_ctr_mt);
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
// - `dispose_own` that handles mt
void ag_release_own_nn(AgObject* obj) {
	if (ag_head(obj)->ctr_mt & AG_CTR_MT) { // when in field it can point to a frozen shared mt.
		AG_TRACE("release own (dealyed) ptr=%p ctr~~%p", obj, (void*)obj->ctr_mt);
		ag_reg_mt_release((uintptr_t)obj);
	} else {
		AG_TRACE("release own (immediate) ptr=%p oldCtr=%p", obj, (void*)(obj->ctr_mt + AG_CTR_STEP));
		if ((ag_head(obj)->ctr_mt -= AG_CTR_STEP) < AG_CTR_STEP)
			ag_dispose_obj(obj);
		else
			ag_set_parent_nn(obj, AG_IN_STACK);
	}
}
void ag_release_own(AgObject* obj) {
	if (ag_not_null(obj))
		ag_release_own_nn(obj);
}
void ag_retain_own_nn(AgObject* obj, AgObject* parent) {
	if (ag_head(obj)->ctr_mt & AG_CTR_MT) {  // when in field it can point to a frozen shared mt
		AG_TRACE("retain own (dealyed) ptr=%p ctr~~%p", obj, (void*)obj->ctr_mt);
		ag_reg_mt_retain((uintptr_t)obj);
	} else {
		AG_TRACE("release own (immediate) ptr=%p oldCtr=%p", obj, (void*)obj->ctr_mt);
		ag_head(obj)->ctr_mt += AG_CTR_STEP;
		ag_set_parent_nn(obj, parent);
	}
}
void ag_retain_own(AgObject* obj, AgObject* parent) {
	if (ag_not_null(obj))
		ag_retain_own_nn(obj, parent);
}
void ag_release_shared_nn(AgObject* obj) {
	// Check for statis lifetime.
	// Only shared ptrs can reference string literals and named consts with static lifetimes, so check only for shared
	if ((((AgObject*)obj)->ctr_mt & ~(AG_CTR_STEP - 1)) == 0)
		return;
	if (ag_head(obj)->ctr_mt & AG_CTR_MT)
		ag_reg_mt_release((uintptr_t)obj);
	else if ((ag_head(obj)->ctr_mt -= AG_CTR_STEP) < AG_CTR_STEP)
		ag_dispose_obj(obj);
}
void ag_release_shared(AgObject* obj) {
	if (ag_not_null(obj))
		ag_release_shared_nn(obj);
}
void ag_retain_shared_nn(AgObject* obj) {
	// Check for statis lifetime. 
	// Only shared ptrs can reference string literals and named consts with static lifetimes, so check only for shared
	if ((((AgObject*)obj)->ctr_mt & ~(AG_CTR_STEP - 1)) == 0)
		return;
	if (ag_head(obj)->ctr_mt & AG_CTR_MT)
		ag_reg_mt_retain((uintptr_t)obj);
	else
		ag_head(obj)->ctr_mt += AG_CTR_STEP;
}
void ag_retain_shared(AgObject* obj) {
	if (ag_not_null(obj))
		ag_retain_shared_nn(obj);
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
	AG_TRACE("obj dispoze obj=%p", obj);
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
	ag_zero_mem(r, size);
	r->ctr_mt = AG_CTR_STEP;
	r->wb_p = AG_IN_STACK | AG_F_PARENT;
	AG_TRACE("obj alloc size=%p, obj=%p", (void*)size, r);
	return r;
}
AgObject* ag_freeze(AgObject* src) {
	AG_TRACE("obj freeze src=%p", src);
	if (src->ctr_mt & AG_CTR_SHARED) {
		ag_retain_shared_nn(src);
		return src;
	}
	ag_copy_freeze = true;
	AgObject* r = ag_copy(src); // todo make optimized version
	ag_copy_freeze = false;
	return r;
}
AgObject* ag_copy(AgObject* src) {
	AG_TRACE("obj copy src=%p", src);
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
	if (!src || (size_t)src < 256)
		return src;
	AgVmt* vmt = ((AgVmt*)(ag_head(src)->dispatcher)) - 1;
	AgObject* dh = (AgObject*) ag_alloc(vmt->instance_alloc_size + AG_HEAD_SIZE);
	ag_memcpy(dh, ag_head(src), vmt->instance_alloc_size + AG_HEAD_SIZE);
	dh->ctr_mt = ag_copy_freeze
		? AG_CTR_STEP | AG_CTR_SHARED
		: AG_CTR_STEP;
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

AgWeak* ag_mk_weak(AgObject* obj) { // obj can't be null TODO: support mk_weak for shared
	AG_TRACE("mk_weak[ for=%p", obj);
	if (ag_head(obj)->wb_p & AG_F_PARENT) {
		AgWeak* w = (AgWeak*) ag_alloc(sizeof(AgWeak));
		w->org_pointer_to_parent = ag_head(obj)->wb_p & ~AG_F_PARENT;
		w->target = obj;
		w->wb_ctr_mt = AG_CTR_WEAK | (AG_CTR_STEP * 2); // one from obj and one from `mk_weak` result
		w->thread = ag_current_thread;
		ag_head(obj)->wb_p = (uintptr_t) w;
		AG_TRACE("mk_weak ret new] for=%p, w=%p, ctr=%p", obj, w, (void*)w->wb_ctr_mt);
		return w;
	}
	AgWeak* w = (AgWeak*)(ag_head(obj)->wb_p);
	ag_retain_weak_nn(w);
	AG_TRACE("mk_weak ret old] for=%p, w=%p, ctr=%p", obj, w, (void*)w->wb_ctr_mt);
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

int32_t ag_m_sys_Cursor_getCh(AgCursor* s) {
	return s->pos && *s->pos
		? get_utf8(&s->pos)
		: 0;
}
int32_t ag_m_sys_Cursor_peekCh(AgCursor* s) {
	const char* pos = s->pos;
	return pos && *pos
		? get_utf8(&pos)
		: 0;
}

void ag_m_sys_Cursor_set(AgCursor* th, AgString* s) {
	ag_retain_shared_nn(&s->head);
	ag_release_shared(&th->str->head);
	th->str = s;
	th->pos = s->chars;
}

AgObject* ag_fn_sys_getParent(AgObject* obj) {  // obj not null, result is nullable
	if (obj->ctr_mt & AG_CTR_SHARED)
		return 0;
	uintptr_t r = obj->wb_p & AG_F_PARENT
		? obj->wb_p & ~AG_F_PARENT
		: ((AgWeak*)obj->wb_p)->org_pointer_to_parent;
	ag_retain_pin((AgObject*)(r));
	return (AgObject*)(r);
}

bool ag_fn_sys_weakExists(AgWeak* w) {
	return ag_not_null(w) && w->target != NULL;
}

void ag_fn_sys_terminate(int result) {
	exit(result);
}

void ag_fn_sys_log(AgString* s) {
	fputs(s->chars, stdout);
}
uint64_t ag_fn_sys_nowMs() {
	struct timespec now;
	return timespec_get(&now, TIME_UTC) ? timespec_to_ms(&now) : 0;
}

int64_t ag_fn_sys_hash(AgObject* obj) {  // Shared
	if (!obj)
		return 0;
	assert(obj->ctr_mt & AG_CTR_SHARED);
	int64_t* dst = (obj->wb_p & AG_F_PARENT) != 0
		? &obj->wb_p
		: &((AgWeak*)obj->wb_p)->org_pointer_to_parent;
	if ((obj->ctr_mt & AG_CTR_HASH) == 0) {
		obj->ctr_mt |= AG_CTR_HASH;
		*dst = ((AgVmt*)(ag_head(obj)->dispatcher))[-1].get_hash(obj) | 1;
	}
	return *dst >> 1;
}

bool ag_eq_mut(AgObject* a, AgObject* b) {
	if (a == b) return true;
	if (!a || !b) return false;
	if (a->dispatcher != b->dispatcher) return false;
	return ((AgVmt*)(ag_head(a)->dispatcher))[-1].equals_to(a, b);
}
bool ag_eq_shared(AgObject* a, AgObject* b) {
	if (a == b) return true;
	if (!a || !b) return false;
	if (a->dispatcher != b->dispatcher) return false;
	if ((a->ctr_mt & AG_CTR_HASH) && (b->ctr_mt & AG_CTR_HASH)) {
		int64_t ah = (a->wb_p & AG_F_PARENT) != 0
			? a->wb_p
			: ((AgWeak*)a->wb_p)->org_pointer_to_parent;
		int64_t bh = (b->wb_p & AG_F_PARENT) != 0
			? b->wb_p
			: ((AgWeak*)b->wb_p)->org_pointer_to_parent;
		if (ah != bh) return false;
	}
	return ((AgVmt*)(ag_head(a)->dispatcher))[-1].equals_to(a, b);
}

int64_t ag_m_sys_Object_getHash(AgObject* obj) {
	int64_t r = (int64_t)obj;
	return r ^ (r >> 14) ^ (r << 16);
}

bool ag_m_sys_Object_equals(AgObject* a, AgObject* b) {
	return false;  // by default equality is only by references
}

int64_t ag_m_sys_String_getHash(AgObject* obj) {
	return ag_getStringHash(((AgString*)obj)->chars);
}
bool ag_m_sys_String_equals(AgObject* a, AgObject* b) {
	return strcmp(((AgString*)a)->chars, ((AgString*)b)->chars) == 0;
}

static void ag_init_thread(ag_thread* th) {
	ag_init_queue(&th->in);
	ag_init_queue(&th->out);
	pthread_mutex_init(&th->mutex, NULL);
	pthread_cond_init(&th->is_not_empty, NULL);
	th->root = NULL;
	th->timer_ms = 0;
	th->timer_proc = 0;
	th->timer_proc_param = 0;
}

bool ag_fn_sys_setMainObject(AgObject* s) {
	AG_TRACE("set main object[ %p", s);
	ag_thread* th = &ag_main_thread;
	if (!th->in.start)
		ag_init_thread(th);
	ag_release_own(th->root);
	if (s && ag_fn_sys_getParent(s)) {
		th->root = NULL;
		return false;
	}
	ag_retain_pin(s);
	th->root = s;
	AG_TRACE0("set main object]");
	return true;
}

uint64_t ag_get_thread_param(ag_thread* th) {  // for trampolines
	return ag_read_queue(&th->in);
}

void ag_unlock_thread_queue(ag_thread* th) { // for trampolines
	pthread_mutex_unlock(&th->mutex);
}

static ag_thread* ag_lock_thread(AgWeak* receiver) {
	ag_thread* th = (ag_thread*)receiver->thread;
	if (!th)
		return NULL;
	pthread_mutex_lock(&th->mutex);
	if (receiver->thread != th) { // thread had died or object moved while we were locking it
		pthread_mutex_unlock(&th->mutex);
		return NULL;
	}
	return th;
}

void ag_unlock_and_notify_thread(ag_thread* th) {
	pthread_mutex_unlock(&th->mutex);
	pthread_cond_broadcast(&th->is_not_empty);
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

// returns non-null mtx-locked thread
// receiver must be prelocked
ag_thread* ag_prepare_post_from_ag(AgWeak* receiver, ag_fn fn, ag_trampoline tramp, size_t params_count) {
	ag_thread* th = (ag_thread*)receiver->thread;
	ag_thread* me = ag_current_thread;
	assert(th);
	assert(me); // must be called on ag-thread
	// no need to lock out-queue thread b/c it's our thread
	ag_queue* q = &me->out;
	ag_resize_queue(
		q,
		params_count + 4);  // params_count + params + trampoline + entry_point + receiver_weak
	ag_write_queue(q, (uint64_t)tramp);
	ag_write_queue(q, (uint64_t)receiver); // if weak posted to another thread, it's already mt-marked, no need to mark it here
	ag_write_queue(q, (uint64_t)fn);
	ag_write_queue(q, (uint64_t)params_count);
	return th;
}

void ag_post_param_from_ag(uint64_t param) {
	ag_write_queue(&ag_current_thread->out, param);
}

static inline void ag_make_weak_mt(AgWeak* w) {
	if (ag_not_null(w) && (w->wb_ctr_mt & AG_CTR_MT) == 0) {
		w->wb_ctr_mt |= AG_CTR_MT;  //previously this weak belonged only to this thread, so no atomic op here
	}
}

void ag_bound_field_to_thread(void* field, int type, void* ctx);

void ag_bound_own_to_thread(AgObject* ptr, ag_thread* th) {
	if (ag_not_null(ptr)) {
		if ((ptr->wb_p & AG_F_PARENT) == 0) {
			AgWeak* w = (AgWeak*)ptr->wb_p;
			if (w->thread == th)
				return;
			if (w->wb_ctr_mt & AG_CTR_MT) // we can check this bit, but can't rewrite it if it's mt
				w->wb_ctr_mt |= AG_CTR_MT;
			w->thread = th;
		}
		if (ptr->ctr_mt & AG_CTR_SHARED) {
			if ((ptr->ctr_mt & AG_CTR_MT) == 0) {
				ptr->ctr_mt |= AG_CTR_MT;  //previously it belonged only to this thread, so no atomic op here
				((AgVmt*)(ag_head(ptr)->dispatcher))[-1].visit(ptr, ag_bound_field_to_thread, th);
			}
		} else { // non-shared object strictly belongs to one thread, and can't be MT
			((AgVmt*)(ag_head(ptr)->dispatcher))[-1].visit(ptr, ag_bound_field_to_thread, th);
		}
	}
}

void ag_post_weak_param_from_ag(AgWeak* param) {
	ag_make_weak_mt(param);
	ag_write_queue(&ag_current_thread->out, (uint64_t)param);
}

void ag_bound_field_to_thread(void* field, int type, void* ctx) {
	if (type == AG_VISIT_WEAK) {
		ag_make_weak_mt(*(AgWeak**)field);
	} else if (type == AG_VISIT_OWN){
		ag_bound_own_to_thread(*(AgObject**)field, (ag_thread*)ctx);
	}
}
void ag_post_own_param_from_ag(ag_thread* th, AgObject* param) {
	if (ag_current_thread != th)
		ag_bound_own_to_thread(param, th);
	ag_write_queue(&ag_current_thread->out, (uint64_t)param);
}

void ag_init_this_thread() {
	if (ag_retain_buffer)
		return;
	AG_TRACE0("init retain buffer");
	ag_retain_buffer = AG_ALLOC(sizeof(intptr_t) * AG_RETAIN_BUFFER_SIZE);
	ag_retain_pos = ag_retain_buffer;
	ag_release_pos = ag_retain_buffer + AG_RETAIN_BUFFER_SIZE;
	if (ag_current_thread == &ag_main_thread) {
		pthread_mutex_init(&ag_retain_release_mutex, NULL);
		pthread_mutex_init(&ag_threads_mutex, NULL);
	}
}

void ag_maybe_flush_retain_release() {
	if (ag_retain_buffer != ag_retain_pos || ag_release_pos != ag_retain_buffer + AG_RETAIN_BUFFER_SIZE)
		ag_flush_retain_release();
}

void* ag_thread_proc(ag_thread* th) {
	ag_current_thread = th;
	AG_TRACE0("thread_proc[");
	ag_init_this_thread();
	struct timespec now;
	pthread_mutex_lock(&th->mutex);
	for (;;) {
		if (th->in.read_pos != th->in.write_pos) {
			AG_TRACE0("thread_proc handle incoming[");
			uint64_t tramp = ag_get_thread_param(th);
			if (!tramp) {
				pthread_mutex_unlock(&th->mutex);
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
			AG_TRACE0("thread_proc handle incoming]");
			pthread_mutex_lock(&th->mutex);
		} else if (th->timer_ms && timespec_get(&now, TIME_UTC) && timespec_to_ms(&now) >= th->timer_ms) {
			th->timer_ms = 0;
			AgObject* timer_object = ag_deref_weak(th->timer_proc_param);
			if (timer_object) {
				pthread_mutex_unlock(&th->mutex);
				th->timer_proc(timer_object);
				ag_release_pin(timer_object);
				pthread_mutex_lock(&th->mutex);
			}
		} else if (th->out.read_pos != th->out.write_pos) {
			AG_TRACE0("thread_proc handle outgoing[");
			ag_maybe_flush_retain_release();
			pthread_mutex_unlock(&th->mutex);
			ag_queue* out = &th->out;
			while (out->read_pos != out->write_pos) {
				uint64_t tramp = ag_read_queue(out);
				uint64_t recv = ag_read_queue(out);
				uint64_t fn = ag_read_queue(out);
				uint64_t count = ag_read_queue(out);
				ag_thread* out_th = ag_lock_thread((AgWeak*)recv);
				if (!out_th) {
					out_th = th; // send to myself to dispose
					pthread_mutex_lock(&th->mutex);
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
			AG_TRACE0("thread_proc handle outgoing]");
			pthread_mutex_lock(&th->mutex);
		} else if (th->root) {
			AG_TRACE0("thread_proc sleep[");
			if (th->timer_ms) {
				struct timespec timeout;
				timeout.tv_sec = th->timer_ms / 1000;
				timeout.tv_nsec = th->timer_ms % 1000 * 1000000;
				pthread_cond_timedwait(&th->is_not_empty, &th->mutex, &timeout);
			} else {
				pthread_cond_wait(&th->is_not_empty, &th->mutex);
			}
			AG_TRACE0("thread_proc sleep]");
		} else {
			AG_TRACE0("thread_proc quitting");
			break;
		}
	}
	pthread_mutex_unlock(&th->mutex);
	ag_maybe_flush_retain_release();
	AG_TRACE0("thread_proc]");
	return NULL;
}

int ag_handle_main_thread() {
	AG_TRACE0("handle main thread[");
	if (ag_main_thread.root) {
		ag_thread_proc(&ag_main_thread);
	}
	AG_TRACE0("handle main thread]");
	return 0;
}

void ag_m_sys_Thread_start(AgThread* th, AgObject* root) {
	AG_TRACE("thread start [root AgThread=%p root=%p", th, root);
	ag_thread* t = NULL;
	ag_init_this_thread();
	pthread_mutex_lock(&ag_threads_mutex);
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
	pthread_mutex_unlock(&ag_threads_mutex);
	// TODO: make root object marker value for parent ptr.
	ag_retain_pin(root); // ok to retain on the creating thread before `pthread_create`
	t->root = root;
	th->thread = t;
	AgWeak* w = ag_mk_weak(root);
	w->wb_ctr_mt = (w->wb_ctr_mt - AG_CTR_STEP) | AG_CTR_MT;
	w->thread = t;
	pthread_create(&t->thread, NULL, (ag_thread_start_t) ag_thread_proc, t);
	AG_TRACE("thread start ] spawned t=%p", t);
}

AgWeak* ag_m_sys_Thread_root(AgThread* th) {
	AG_TRACE("thread get root AgThread=%p ret=%p", th, th->thread->root);
	return ag_mk_weak(th->thread->root);
}
void ag_copy_sys_Thread(AgThread* dst, AgThread* src) {
	AG_TRACE0("thread obj copy");
	dst->thread = NULL;
}
void ag_dtor_sys_Thread(AgThread* ptr) {
	AG_TRACE("thread dtor AgThread=%p th= %p", ptr, ptr->thread);
	if (ptr->thread) {
		ag_thread* th = ptr->thread;
		pthread_mutex_lock(&th->mutex);
		ag_resize_queue(&th->in, 1);
		ag_write_queue(&th->in, 0);
		ag_unlock_and_notify_thread(th);
		void* unused_result;
		pthread_join(th->thread, &unused_result);
		pthread_mutex_lock(&ag_threads_mutex);
		th->timer_proc_param = (AgWeak*) ag_thread_free;
		ag_thread_free = th;
		pthread_mutex_unlock(&ag_threads_mutex);
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

// Used by FFI, not by Ag
// Receiver must be locked.
// Returns thread or NULL if receiver is dead.
ag_thread* ag_prepare_post(AgWeak* recv, void* tramp, void* entry_point, int64_t params_count) {
	ag_thread* th = ag_lock_thread(recv);
	if (!th) {
		ag_release_weak(recv);
		return NULL;
	}
	ag_queue* q = &th->in;
	ag_resize_queue(
		q,
		params_count + 3);  // trampoline + entry_point + receiver_weak + params
	ag_write_queue(q, (uint64_t)tramp);
	ag_write_queue(q, (uint64_t)recv);
	ag_write_queue(q, (uint64_t)entry_point);
	return th;
}

void ag_finalize_post(ag_thread* th) {
	if (th)
		ag_unlock_and_notify_thread(th);
}
void ag_post_param(ag_thread* th, uint64_t param) {
	if (th)
		ag_write_queue(&th->in, param);
}
void ag_post_own_param(ag_thread* th, AgObject* param) {
	if (th) {
		if (ag_current_thread != th)
			ag_bound_own_to_thread(param, th);
		ag_write_queue(&th->in, (uint64_t)param);
	} else {
		ag_release_own(param);
	}
}
void ag_post_weak_param(ag_thread* th, AgWeak* param) {
	if (th) {
		ag_make_weak_mt(param);
		ag_write_queue(&th->in, (uint64_t)param);
	} else {
		ag_release_weak(param);
	}
}
void ag_detach_own(AgObject* obj) {
	assert(ag_fn_sys_getParent(obj) == NULL);
	ag_retain_pin(obj);
	ag_bound_own_to_thread(obj, NULL);
}
void ag_detach_weak(AgWeak* w) {
	ag_retain_weak(w);
	ag_make_weak_mt(w);
}

AgString* ag_make_str(const char* start, size_t size) {
	AgString* s = (AgString*)ag_allocate_obj(sizeof(AgString) + size);
	ag_memcpy(s->chars, start, size);
	s->chars[size] = 0;
	s->head.dispatcher = ag_disp_sys_String;
	s->head.ctr_mt |= AG_CTR_SHARED | AG_CTR_HASH;
	s->head.wb_p = ag_getStringHash(s->chars) | 1;
	// TODO: combine hash calc and copy
	return s;
}
