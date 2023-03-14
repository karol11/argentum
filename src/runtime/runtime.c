#include <stddef.h> // size_t
#include <stdint.h> // int32_t
#include <stdio.h> // puts

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

#include "utils/utf8.h"
#include "runtime/runtime.h"

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
// Tags in copy operation
//
#define AG_TG_WEAK_BLOCK ((uintptr_t) 0)
#define AG_TG_OBJECT     ((uintptr_t) 1)
#define AG_TG_WEAK       ((uintptr_t) 2)

#define AG_PTR_TAG(PTR)            (((uintptr_t)PTR) & 3)
#define AG_TAG_PTR(TYPE, PTR, TAG) ((TYPE*)(((uintptr_t)(PTR)) | TAG))
#define AG_UNTAG_PTR(TYPE, PTR)    ((TYPE*)(((uintptr_t)(PTR)) & ~3))

inline AgHead* ag_head(AgObject* obj) { return ((AgHead*)obj) - 1; }
bool           ag_leak_detector_ok();

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

void ag_release(AgObject* obj) {
	if (!obj || (size_t)obj < 256)
		return;
	if ((ag_head(obj)->counter & AG_CTR_WEAKLESS) != 0) {
		if ((ag_head(obj)->counter -= AG_CTR_STEP) >= AG_CTR_STEP)
			return;
	} else {
		AgWeak* wb = (AgWeak*)(ag_head(obj)->counter);
		if ((wb->org_counter -= AG_CTR_STEP) >= AG_CTR_STEP)
			return;
		wb->target = 0;
		ag_head(obj)->counter = 0;
		ag_release_weak(wb);
	}
	((AgVmt*)(ag_head(obj)->dispatcher))[-1].dispose(obj);
	ag_free((AgHead*)(obj) - 1);
}

AgObject* ag_retain(AgObject* obj) {
	if (obj && (size_t)obj >= 256) {
		if ((ag_head(obj)->counter & AG_CTR_WEAKLESS) != 0) {
			ag_head(obj)->counter += AG_CTR_STEP;
		} else {
			((AgWeak*)(ag_head(obj)->counter))->org_counter += AG_CTR_STEP;
		}
	}
	return obj;
}

AgObject* ag_allocate_obj(size_t size) {
	AgHead* r = (AgHead*) ag_alloc(size + sizeof(AgHead));
	if (!r) {  // todo: add more handling
		exit(-42);
	}
	ag_zero_mem(r, size);
	r->counter = AG_CTR_STEP | AG_CTR_WEAKLESS;
	return r + 1;
}

AgObject* ag_copy(AgObject* src) {
	AgObject* dst = ag_copy_object_field(src);
	AgObject* c = 0;
	AgWeak* wb = 0;
	for (AgObject* i = ag_copy_head; i;) {
		switch (AG_PTR_TAG(i)) {
		case AG_TG_OBJECT:
			if (c)
				ag_head(c)->counter = AG_CTR_STEP | AG_CTR_WEAKLESS;
			c = AG_UNTAG_PTR(AgObject, i);
			i = (AgObject*)(ag_head(c)->counter);
			break;
		case AG_TG_WEAK_BLOCK:
			wb = AG_UNTAG_PTR(AgWeak, i);
			i = wb->target;
			wb->target = c;
			c = 0;
			break;
		case AG_TG_WEAK: {
			AgWeak** w = AG_UNTAG_PTR(AgWeak*, i);
			i = (AgObject*)*w;
			*w = wb;
			wb->wb_counter++;
			break; }
		}
	}
	if (c)
		ag_head(c)->counter = AG_CTR_STEP | AG_CTR_WEAKLESS;
	ag_copy_head = 0;
	return dst;
}

AgObject* ag_copy_object_field(AgObject* src) {
	if (!src || (size_t)src < 256)
		return src;
	if ((ag_head(src)->counter & AG_CTR_WEAKLESS
			? ag_head(src)->counter
			: ((AgWeak*)ag_head(src)->counter)->org_counter)
		& AG_CTR_FROZEN) {
		return ag_retain(src);
	}
	AgVmt* vmt = ((AgVmt*)(ag_head(src)->dispatcher)) - 1;
	AgHead* dh = (AgHead*) ag_alloc(vmt->instance_alloc_size + sizeof(AgHead));
	if (!dh) { exit(-42); }
	ag_memcpy(dh, ag_head(src), vmt->instance_alloc_size + sizeof(AgHead));
	dh->counter = AG_CTR_STEP | AG_CTR_WEAKLESS;
	vmt->copy_ref_fields((AgObject*)(dh + 1), src);
	if ((ag_head(src)->counter & AG_CTR_WEAKLESS) == 0) { // has weak block
		AgWeak* wb = (AgWeak*)(ag_head(src)->counter);
		if (wb->target == src) { // no weak copied yet
			wb->target = AG_TAG_PTR(AgObject, dh + 1, AG_TG_OBJECT);
			dh->counter = (uintptr_t) ag_copy_head;
			ag_copy_head = AG_TAG_PTR(AgObject, src, AG_TG_OBJECT);
		} else {
			AgWeak* dst_wb = (AgWeak*) ag_alloc(sizeof(AgWeak));
			dh->counter = (uintptr_t) dst_wb;
			dst_wb->org_counter = AG_CTR_STEP | AG_CTR_WEAKLESS;
			void* i = ((AgWeak*)(ag_head(src)->counter))->target;
			uintptr_t dst_wb_locks = 1;
			while (AG_PTR_TAG(i) == AG_TG_WEAK) {
				AgWeak** w = AG_UNTAG_PTR(AgWeak*, i);
				i = *w;
				*w = dst_wb;
				dst_wb_locks++;
			}
			dst_wb->wb_counter = dst_wb_locks;
			dst_wb->target = (AgObject*) i;
			wb->target = AG_TAG_PTR(AgObject, dh + 1, AG_TG_OBJECT);
		}
	}
	while (ag_copy_fixers_count) {  // TODO retain objects in copy_fixers vector.
		AgCopyFixer* f = ag_copy_fixers + --ag_copy_fixers_count;
		f->fixer(f->data);
	}
	return (AgObject*)(dh + 1);
}

void ag_fn_sys_make_shared(AgObject* obj) {  // TODO: implement hierarchy freeze
	if ((ag_head(obj)->counter & AG_CTR_WEAKLESS) != 0) {
		ag_head(obj)->counter |= AG_CTR_FROZEN;
	} else {
		AgWeak* wb = (AgWeak*)(ag_head(obj)->counter);
		wb->org_counter |= AG_CTR_FROZEN;
	}
}

AgWeak* ag_retain_weak(AgWeak* w) {
	if (w && (size_t)w >= 256)
		++w->wb_counter;
	return w;
}

void ag_release_weak(AgWeak* w) {
	if (!w || (size_t)w < 256)
		return;
	if (--w->wb_counter != 0)
		return;
	ag_free(w);
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
			AgWeak* cwb = (AgWeak*)(ag_head(copy)->counter);
			if (!cwb || AG_PTR_TAG(cwb) == AG_TG_OBJECT) // has no wb yet
			{
				cwb = (AgWeak*) ag_alloc(sizeof(AgWeak));
				cwb->org_counter = AG_CTR_STEP;
				cwb->wb_counter = AG_CTR_STEP;
				cwb->target = (AgObject*)(ag_head(copy)->counter);
				ag_head(copy)->counter = (uintptr_t) AG_TAG_PTR(void, cwb, AG_TG_WEAK_BLOCK);
			} else
				cwb = AG_UNTAG_PTR(AgWeak, cwb);
			cwb->wb_counter++;
			*dst = cwb;
			break; }
		}
	}
}

AgWeak* ag_mk_weak(AgObject* obj) { // obj can't be null
	if (ag_head(obj)->counter & AG_CTR_WEAKLESS) {
		AgWeak* w = (AgWeak*) ag_alloc(sizeof(AgWeak));
		w->org_counter = ag_head(obj)->counter;
		w->target = obj;
		w->wb_counter = 2; // one from obj and one from `mk_weak` result
		ag_head(obj)->counter = (uintptr_t) w;
		return w;
	}
	AgWeak* w = (AgWeak*)(ag_head(obj)->counter);
	w->wb_counter++;
	return w;
}

AgObject* ag_deref_weak(AgWeak* w) {
	if (!w || (size_t)w < 256 || !w->target) {
		return 0;
	}
	w->org_counter += AG_CTR_STEP;
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
		ag_release(*data);
		*data = 0;
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
		ag_retain(val);
		ag_release(*dst);
		*dst = val;
	}
}

void ag_fn_sys_WeakArray_setAt(AgBlob* b, uint64_t index, AgWeak* val) {
	if (index < b->size) {
		AgWeak** dst = ((AgWeak**)(b->data)) + index;
		val = ag_retain_weak(val);
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
		*to = ag_copy_object_field(*from);
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
		ag_release(*ptr);
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

void ag_fn_terminate(int result) {
	exit(result);
}

void ag_fn_sys_log(AgString* s) {
	puts(s->ptr);
}
