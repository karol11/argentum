#include <stddef.h> // size_t
#include <stdint.h> // int32_t
#include <stdio.h> // puts
#include <assert.h>

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
// AG_TG_NOWEAK_DST - this is object, its wb_p points to its parent, its counter points to next-in-queue object
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

inline void ag_set_parent_nn(AgObject * obj, AgObject* parent) {
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
		++ag_head(obj)->counter;
	}
	return true;
}

void ag_set_parent(AgObject* obj, AgObject* parent) {
	if (ag_not_null(obj)) {
		ag_set_parent_nn(obj, parent);
	}
}

void ag_release(AgObject * obj) {
	if (ag_not_null(obj)) {
		if (--ag_head(obj)->counter == 0)
			ag_dispose_obj(obj);
	}
}
inline AgObject* ag_retain(AgObject* obj) {
	if (ag_not_null(obj))
		++ag_head(obj)->counter;
	return obj;
}
inline void ag_release_weak(AgWeak* w) {
	if (ag_not_null(w)) {
		if (--w->wb_counter == 0)
			ag_free(w);
	}
}
inline AgWeak* ag_retain_weak(AgWeak* w) {
	if (ag_not_null(w)) {
		++ag_head(w)->counter;
	}
	return w;
}
inline void ag_release_own(AgObject* obj) {
	if (ag_not_null(obj)) {
		if (--ag_head(obj)->counter == 0)
			ag_dispose_obj(obj);
		else
			ag_set_parent_nn(obj, AG_IN_STACK);
	}
}
inline void ag_retain_own(AgObject* obj, AgObject* parent) {
	if (ag_not_null(obj)) {
		++ag_head(obj)->counter;
		ag_set_parent_nn(obj, parent);
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
	r->counter = 1;
	r->wb_p = AG_IN_STACK | AG_F_PARENT;
	return r;
}

AgObject* ag_copy(AgObject* src) {
	AgObject* dst = ag_copy_object_field(src, 0);
	for (AgObject* obj = ag_copy_head; obj;) {
		if (AG_PTR_TAG(obj) == AG_TG_NOWEAK_DST) {
			AgObject* dst = AG_UNTAG_PTR(AgObject, obj);
			obj = (AgObject*) dst->counter;
			dst->counter = 1;
		} else {
			assert(AG_PTR_TAG(obj) == AG_TG_OBJECT);
			obj = AG_UNTAG_PTR(AgObject, obj);
			assert(AG_PTR_TAG(obj->wb_p) == AG_TG_WEAK_BLOCK);
			AgWeak* wb = (AgWeak*) obj->wb_p;
			void* next = wb->target;
			wb->target = obj;
			while (AG_PTR_TAG(next) == AG_TG_WEAK) {
				void** w = AG_UNTAG_PTR(void*, next);
				wb->wb_counter++;
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
	if (((ag_head(src)->wb_p & AG_F_PARENT)
			? ag_head(src)->wb_p == (AG_SHARED | AG_F_PARENT)
			: ((AgWeak*)ag_head(src)->wb_p)->org_pointer_to_parent) == AG_SHARED)
		return ag_retain(src);
	AgVmt* vmt = ((AgVmt*)(ag_head(src)->dispatcher)) - 1;
	AgObject* dh = (AgObject*) ag_alloc(vmt->instance_alloc_size + AG_HEAD_SIZE);
	if (!dh) { exit(-42); }
	ag_memcpy(dh, ag_head(src), vmt->instance_alloc_size + AG_HEAD_SIZE);
	dh->counter = 1;
	dh->wb_p = (uintptr_t) AG_TAG_PTR(AgObject, parent, AG_F_PARENT);  //NO_WEAK also makes it AG_TG_NOWEAK_DST
	vmt->copy_ref_fields((AgObject*)(dh + AG_HEAD_SIZE), src);
	if ((ag_head(src)->wb_p & AG_F_PARENT) == 0) { // has weak block
		AgWeak* wb = (AgWeak*) ag_head(src)->wb_p;
		if (wb->target == src) { // no weak copied yet
			dh->counter = (uintptr_t) ag_copy_head;  // AG_TG_NOWEAK_DST uses counter as link
			ag_copy_head = AG_TAG_PTR(AgObject, src, AG_TG_OBJECT);
		} else {
			AgWeak* dst_wb = (AgWeak*) ag_alloc(sizeof(AgWeak));
			dst_wb->org_pointer_to_parent = (uintptr_t) parent;
			dh->wb_p = (uintptr_t) dst_wb;  // also clears NO_WEAK
			void* i = ((AgWeak*)(ag_head(src)->wb_p))->target;
			uintptr_t dst_wb_locks = 1;
			while (AG_PTR_TAG(i) == AG_TG_WEAK) {
				AgWeak** w = AG_UNTAG_PTR(AgWeak*, i);
				i = *w;
				*w = dst_wb;
				dst_wb_locks++;
			}
			dst_wb->wb_counter = dst_wb_locks;
			dst_wb->target = (AgObject*) i;
		}
		wb->target = AG_TAG_PTR(AgObject, dh + AG_HEAD_SIZE, AG_TG_OBJECT);
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
	} else if (!src->target) {
		src->wb_counter++;
		*dst = src;
	} else {
		switch (AG_PTR_TAG(src->target)) {
		case AG_TG_WEAK_BLOCK: // tagWB == 0, so it is an uncopied object
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
				cwb->org_pointer_to_parent = ag_head(copy)->wb_p & ~AG_F_PARENT;
				cwb->wb_counter = 1;
				cwb->target = (AgObject*)(ag_head(copy)->counter);
				ag_head(copy)->wb_p = (uintptr_t) cwb;
			} else
				cwb = AG_UNTAG_PTR(AgWeak, ag_head(copy)->wb_p);
			cwb->wb_counter++;
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
		w->wb_counter = 2; // one from obj and one from `mk_weak` result
		ag_head(obj)->wb_p = (uintptr_t) w;
		return w;
	}
	AgWeak* w = (AgWeak*)(ag_head(obj)->wb_p);
	w->wb_counter++;
	return w;
}

AgObject* ag_deref_weak(AgWeak* w) {
	if (!w || (size_t)w < 256 || !w->target) {
		return 0;
	}
	++w->target->counter;
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

int32_t ag_fn_sys_String_getCh(AgString* s) {
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

int64_t ag_fn_sys_Container_size(AgBlob* b) {
	return b->size;
}

void ag_fn_sys_Container_insert(AgBlob* b, uint64_t index, uint64_t count) {
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

void ag_fn_sys_Blob_delete(AgBlob* b, uint64_t index, uint64_t count) {
	if (!count || index > b->size || index + count > b->size)
		return;
	int64_t* new_data = (int64_t*) ag_alloc(sizeof(int64_t) * (b->size - count));
	ag_memcpy(new_data, b->data, sizeof(int64_t) * index);
	ag_memcpy(new_data + index, b->data + index + count, sizeof(int64_t) * (b->size - index));
	ag_free(b->data);
	b->data = new_data;
	b->size -= count;
}

void ag_fn_sys_Array_delete(AgBlob* b, uint64_t index, uint64_t count) {
	if (!count || index > b->size || index + count > b->size)
		return;
	AgObject** data = ((AgObject**)(b->data)) + index;
	for (uint64_t i = count; i != 0; i--, data++) {
		ag_release_own(*data);
	}
	ag_fn_sys_Blob_delete(b, index, count);
}

void ag_fn_sys_WeakArray_delete(AgBlob* b, uint64_t index, uint64_t count) {
	if (!count || index > b->size || index + count > b->size)
		return;
	AgWeak** data = ((AgWeak**)(b->data)) + index;
	for (uint64_t i = count; i != 0; i--, data++) {
		ag_release_weak(*data);
		*data = 0;
	}
	ag_fn_sys_Blob_delete(b, index, count);
}

bool ag_fn_sys_Container_move(AgBlob* blob, uint64_t a, uint64_t b, uint64_t c) {
	if (a >= b || b >= c || c > blob->size)
		return false;
	uint64_t* temp = (uint64_t*) ag_alloc(sizeof(uint64_t) * (b - a));
	ag_memmove(temp, blob->data + a, sizeof(uint64_t) * (b - a));
	ag_memmove(blob->data + a, blob->data + b, sizeof(uint64_t) * (c - b));
	ag_memmove(blob->data + a + (c - b), temp, sizeof(uint64_t) * (b - a));
	ag_free(temp);
	return true;
}

int64_t ag_fn_sys_Blob_getAt(AgBlob* b, uint64_t index) {
	return index < b->size ? b->data[index] : 0;
}

void ag_fn_sys_Blob_setAt(AgBlob* b, uint64_t index, int64_t val) {
	if (index < b->size)
		b->data[index] = val;
}
int64_t ag_fn_sys_Blob_getByteAt(AgBlob* b, uint64_t index) {
	return index / sizeof(int64_t) < b->size
		? ((uint8_t*)(b->data))[index]
		: 0;
}

void ag_fn_sys_Blob_setByteAt(AgBlob* b, uint64_t index, int64_t val) {
	if (index / sizeof(int64_t) < b->size)
		((uint8_t*)(b->data))[index] = (uint8_t)val;
}

int64_t ag_fn_sys_Blob_get16At(AgBlob* b, uint64_t index) {
	return index / sizeof(int64_t) * sizeof(int16_t) < b->size
		? ((uint16_t*)(b->data))[index]
		: 0;
}

void ag_fn_sys_Blob_set16At(AgBlob* b, uint64_t index, int64_t val) {
	if (index / sizeof(int64_t) * sizeof(int16_t) < b->size)
		((uint16_t*)(b->data))[index] = (uint16_t)val;
}

int64_t ag_fn_sys_Blob_get32At(AgBlob* b, uint64_t index) {
	return index / sizeof(int64_t) * sizeof(int32_t) < b->size
		? ((uint32_t*)(b->data))[index]
		: 0;
}

void ag_fn_sys_Blob_set32At(AgBlob* b, uint64_t index, int64_t val) {
	if (index / sizeof(int64_t) * sizeof(int32_t) < b->size)
		((uint32_t*)(b->data))[index] = (uint32_t)val;
}

bool ag_fn_sys_Blob_copy(AgBlob* dst, uint64_t dst_index, AgBlob* src, uint64_t src_index, uint64_t bytes) {
	if ((src_index + bytes) / sizeof(int64_t) >= src->size || (dst_index + bytes) / sizeof(int64_t) >= dst->size)
		return false;
	ag_memmove(((uint8_t*)(dst->data)) + dst_index, ((uint8_t*)(src->data)) + src_index, bytes);
	return true;
}

AgObject* ag_fn_sys_Array_getAt(AgBlob* b, uint64_t index) {
	return index < b->size
		? ag_retain(((AgObject*)(b->data)[index]))
		: 0;
}

AgWeak* ag_fn_sys_WeakArray_getAt(AgBlob* b, uint64_t index) {
	return index < b->size
		? ag_retain_weak((AgWeak*)(b->data[index]))
		: 0;
}

void ag_fn_sys_Array_setAt(AgBlob* b, uint64_t index, AgObject* val) {
	if (index < b->size) {
		AgObject** dst = ((AgObject**)(b->data)) + index;
		ag_retain_own(val, &b->head);
		ag_release_own(*dst);
		*dst = val;
	}
}

bool ag_fn_sys_Array_spliceAt(AgBlob* b, uint64_t index, AgObject* val) {
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

void ag_fn_sys_WeakArray_setAt(AgBlob* b, uint64_t index, AgWeak* val) {
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

bool ag_fn_sys_String_fromBlob(AgString* s, AgBlob* b, int at, int count) {
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

int64_t ag_fn_sys_Blob_putCh(AgBlob* b, int at, int codepoint) {
	char* cursor = ((char*)(b->data)) + at;
	if (at + 5 > b->size * sizeof(uint64_t))
		return 0;
	put_utf8(codepoint, &cursor, ag_put_fn);
	return cursor - (char*)(b->data);
}

AgObject* ag_fn_sys_getParent(AgObject* obj) {  // obj not null, result is nullable
	return ag_retain((AgObject*)(obj->wb_p & AG_F_PARENT
		? obj->wb_p & ~AG_F_PARENT
		: ((AgWeak*)obj->wb_p)->org_pointer_to_parent));
}

void ag_fn_terminate(int result) {
	exit(result);
}

void ag_fn_sys_log(AgString* s) {
	puts(s->ptr);
}
