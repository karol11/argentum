#ifndef AG_QUEUE_H_
#define AG_QUEUE_H_

#include <stdint.h>

#define AG_THREAD_QUEUE_SIZE 8192

typedef struct ag_queue {
	int64_t* start;
	int64_t* end;
	int64_t* read_pos;
	int64_t* write_pos;
} ag_queue;

void ag_init_queue(ag_queue* q);
void ag_resize_queue(ag_queue* q, uint64_t space_needed);

static inline uint64_t ag_read_queue(ag_queue* q) {
	uint64_t r = *q->read_pos;
	if (++q->read_pos == q->end)
		q->read_pos = q->start;
	return r;
}

static inline void ag_write_queue(ag_queue* q, uint64_t param) {
	*q->write_pos = param;
	if (++q->write_pos == q->end)
		q->write_pos = q->start;
}

#endif // AG_QUEUE_H_
