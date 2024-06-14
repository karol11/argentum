#include "blob.h"
#include "utf8.h"

int64_t ag_m_sys_Blob_capacity(AgBlob* b) {
	return b->bytes_count;
}

void ag_m_sys_Blob_insert(AgBlob* b, uint64_t index, uint64_t count) {
	if (!count || index > b->bytes_count)
		return;
	int8_t* new_data = (int8_t*) ag_alloc(b->bytes_count + count);
	ag_memcpy(new_data, b->bytes, index);
	ag_zero_mem(new_data + index, count);
	ag_memcpy(new_data + index + count, b->bytes + index, b->bytes_count - index);
	ag_free(b->bytes);
	b->bytes = new_data;
	b->bytes_count += count;
}

void ag_m_sys_Blob_delete(AgBlob* b, uint64_t index, uint64_t count) {
	if (!count || index > b->bytes_count || index + count > b->bytes_count)
		return;
	int8_t* new_data = (int8_t*) ag_alloc(b->bytes_count - count);
	ag_memcpy(new_data, b->bytes, index);
	ag_memcpy(new_data + index, b->bytes + index + count, b->bytes_count - index - count);
	ag_free(b->bytes);
	b->bytes = new_data;
	b->bytes_count -= count;
}

int64_t ag_m_sys_Blob_get8At(AgBlob* b, uint64_t index) {
	return index < b->bytes_count
		? b->bytes[index]
		: 0;
}

void ag_m_sys_Blob_set8At(AgBlob* b, uint64_t index, int64_t val) {
	if (index < b->bytes_count)
		b->bytes[index] = (uint8_t)val;
}

int64_t ag_m_sys_Blob_get16At(AgBlob* b, uint64_t index) {
	return index < b->bytes_count >> 1
		? ((uint16_t*)b->bytes)[index]
		: 0;
}

void ag_m_sys_Blob_set16At(AgBlob* b, uint64_t index, int64_t val) {
	if (index < b->bytes_count >> 1)
		((uint16_t*)b->bytes)[index] = (uint16_t)val;
}

int64_t ag_m_sys_Blob_get32At(AgBlob* b, uint64_t index) {
	return index < b->bytes_count >> 2
		? ((uint32_t*)b->bytes)[index]
		: 0;
}

void ag_m_sys_Blob_set32At(AgBlob* b, uint64_t index, int64_t val) {
	if (index < b->bytes_count >> 2)
		((uint32_t*)b->bytes)[index] = (uint32_t)val;
}

int64_t ag_m_sys_Blob_get64At(AgBlob* b, uint64_t index) {
	return index < b->bytes_count >> 3
		? ((uint64_t*)b->bytes)[index]
		: 0;
}

void ag_m_sys_Blob_set64At(AgBlob* b, uint64_t index, int64_t val) {
	if (index < b->bytes_count >> 3)
		((uint64_t*)b->bytes)[index] = val;
}

bool ag_m_sys_Blob_copy(AgBlob* dst, uint64_t dst_index, AgBlob* src, uint64_t src_index, uint64_t count) {
	if (src_index + count >= src->bytes_count || dst_index + count >= dst->bytes_count)
		return false;
	ag_memmove(dst->bytes + dst_index, src->bytes + src_index, count);
	return true;
}

void ag_copy_sys_Blob(AgBlob* d, AgBlob* s) {
	d->bytes_count = s->bytes_count;
	d->bytes = ag_alloc(d->bytes_count);
	ag_memcpy(d->bytes, s->bytes, d->bytes_count);
}

void ag_visit_sys_Blob(
	void* ptr,
	void(*visitor)(void*, int, void*),
	void* ctx)
{}

void ag_dtor_sys_Blob(AgBlob* p) {
	ag_free(p->bytes);
}

static int ag_put_fn(void* ctx, int b) {
	char** c = (char**)ctx;
	**c = b;
	(*c)++;
	return 1;
}

int64_t ag_m_sys_Blob_putChAt(AgBlob* b, int at, int codepoint) {
	if (at + 5 > b->bytes_count)
		return 0;
	int8_t* cursor = (int8_t*)b->bytes + at;
	put_utf8(codepoint, &cursor, ag_put_fn);
	return cursor - b->bytes;
}

AgString* ag_m_sys_Blob_mkStr(AgBlob* b, int at, int count) {
	if (count < 0 || at + count > b->bytes_count)
		count = 0;
	return ag_make_str(b->bytes + at, count);
}

void ag_make_blob_fit(AgBlob* b, size_t size) {
	if (b->bytes_count < size)
		ag_m_sys_Blob_insert(b, b->bytes_count, size - b->bytes_count);
}
