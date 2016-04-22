#ifndef QUEUE_H
#define	QUEUE_H

#include <linux/list.h>
#include <linux/time.h>

struct queue {
	struct list_head reqs;   // requests
	struct list_head queues; // other queues
	int count; // number of requests
	int rw;    // 0:read, 1:write
	struct timespec lastpop;
};

static inline void queue_init(struct queue *q, int rw) {
	memset(q, 0, sizeof(*q));
	INIT_LIST_HEAD(&q->reqs);
	INIT_LIST_HEAD(&q->queues);
	q->rw = rw;
}

static inline void queue_push(struct queue *q, struct request *rq) {
	list_add_tail(&rq->queuelist, &q->reqs);
	q->count++;
}

static inline struct request *queue_pop(struct queue *q) {
	struct request *rq;
	if (list_empty(&q->reqs))
		return NULL;
	rq = list_entry(q->reqs.next, struct request, queuelist);
	list_del_init(&rq->queuelist);
	q->count--;
	getnstimeofday(&q->lastpop);
	return rq;
}

// return elapsed time, max of 1s
static inline unsigned long queue_pop_elapsed_us(struct queue *q) {
	struct timespec ts;
	unsigned long elapsed;
	getnstimeofday(&ts);
	WARN_ON(q->lastpop.tv_sec > ts.tv_sec);
	if (ts.tv_sec > q->lastpop.tv_sec + 2)
		return 1000000;
	elapsed = (ts.tv_sec - q->lastpop.tv_sec) * 1000000;
	elapsed += ts.tv_nsec / 1000;
	elapsed -= q->lastpop.tv_nsec / 1000;
	return elapsed;
}

static inline void queue_del(struct queue *q, struct request *rq) {
	list_del_init(&rq->queuelist);
	q->count--;
}

#endif
