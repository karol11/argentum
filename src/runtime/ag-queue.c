#include "ag-queue.h"
#include "runtime.h"

void ag_init_queue(ag_queue* q) {
	q->read_pos = q->write_pos = q->start = AG_ALLOC(sizeof(int64_t) * AG_THREAD_QUEUE_SIZE);
	q->end = q->start + AG_THREAD_QUEUE_SIZE;
}

void ag_resize_queue(ag_queue* q, uint64_t space_needed) {
	uint64_t free_space = q->read_pos > q->write_pos
		? q->write_pos - q->read_pos
		: (q->end - q->start) - (q->write_pos - q->read_pos);
	if (free_space < space_needed) {
		uint64_t new_size = (q->end - q->start) * 2 + space_needed;
		uint64_t* new_buf = AG_ALLOC(sizeof(uint64_t) * new_size);
		if (!new_buf)
			exit(-42);
		if (q->read_pos > q->write_pos) {
			uint64_t r_size = q->end - q->read_pos;
			uint64_t w_size = q->write_pos - q->start;
			memcpy(new_buf + new_size - r_size, q->read_pos, sizeof(uint64_t) * (r_size));
			memcpy(new_buf, q->start, sizeof(uint64_t) * (w_size));
			AG_FREE(q->start);
			q->read_pos = new_buf + new_size - r_size;
			q->write_pos = new_buf + w_size;
		} else {
			uint64_t size = q->write_pos - q->read_pos;
			memcpy(new_buf, q->read_pos, sizeof(uint64_t) * size);
			AG_FREE(q->start);
			q->read_pos = new_buf;
			q->write_pos = new_buf + size;
		}
		q->start = new_buf;
		q->end = new_buf + new_size;
	}
}
