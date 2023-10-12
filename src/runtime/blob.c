#include "blob.h"
#include "utils/utf8.h"

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

void ag_dtor_sys_Blob(AgBlob* p) {
	ag_free(p->data);
}
void ag_dtor_sys_Container(AgBlob* p) {
	ag_dtor_sys_Blob(p);
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

void ag_make_blob_fit(AgBlob* b, size_t required_size) {
	required_size = (required_size + sizeof(int64_t) - 1) / sizeof(int64_t);
	if (b->size < required_size)
		ag_m_sys_Container_insertItems(b, b->size, required_size - b->size);
}
