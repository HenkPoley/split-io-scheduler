#include "io_batch.h"

#include <linux/cause_tags.h>

struct io_batch *iobatch_new(int capacity, size_t bytes_cap) {
    struct io_batch *batch = kmalloc(sizeof(*batch), GFP_ATOMIC);
	IORequest *reqs = kmalloc(sizeof(*reqs)*capacity, GFP_ATOMIC);
	if (!batch || !reqs) {
		kfree(batch);
		kfree(reqs);
		return NULL;
	}
	memset(batch, 0, sizeof(*batch));
	memset(reqs, 0, sizeof(*reqs));
	disk_init(&batch->disk);
	batch->reqs = reqs;
	batch->reqs_size = 0;
	batch->reqs_capacity = capacity;
	batch->cost = -1;
	batch->bytes = 0;
	batch->bytes_cap = bytes_cap;
	return batch;
}

int iobatch_full(struct io_batch* batch) {
	BUG_ON(batch->reqs_size > batch->reqs_capacity);
	return (batch->reqs_size >= batch->reqs_capacity ||
			batch->bytes >= batch->bytes_cap);
}

int iobatch_add_request(struct io_batch* batch,
						off_t offset, int size) {
	IORequest *prev;
	int rv = 0;
	if (iobatch_full(batch)) {
		WARN_ON("trying to add to full!");
		return rv;
	}
	prev = &batch->reqs[batch->reqs_size - 1];

	if (batch->reqs_size > 0 && offset == (prev->offset + prev->size)) {
		prev->size += size;
		rv = 1;
	} else {
		batch->reqs[batch->reqs_size].offset = offset;
		batch->reqs[batch->reqs_size].size = size;
		batch->reqs_size++;
	}
	batch->cost = -1;
	batch->bytes += size;
	return rv;
}

/**
 * Reset an io request, and delete all its causes.
 * 
 * @param request the IO request
 */
void iobatch_clear(struct io_batch* batch) {
	IORequest *last;
	if (batch->reqs_size > 0) {
		last = &batch->reqs[batch->reqs_size-1];
		batch->reqs_size = 0;
		batch->cost = -1;
		batch->bytes = 0;
	}
}

void iobatch_free(struct io_batch* batch) {
	kfree(batch->reqs);
	kfree(batch);
}

/**
 * Compare to requests based on the sector number.
 * 
 * @param a The first block request (IORequest)
 * @param b The second block request (IORequest)
 * @return >0 first larger than second, =0 equal, <0 smaller than second
 */
int request_cmp(const void *a, const void *b) {
	off_t off1 = ((IORequest*)a)->offset;
    off_t off2 = ((IORequest*)b)->offset;
	if (off1 < off2)
		return -1;
	else if (off1 > off2)
		return 1;
	return 0;
}

/**
 * Sort the struct io_batch based on the sector number
 * 
 * @param batch The struct io_batch to be sorted
 */
static void iobatch_scan_sort(struct io_batch* batch) {
    sort(batch->reqs, batch->reqs_size,
		 sizeof(batch->reqs[0]), request_cmp, NULL);
}

size_t iobatch_get_cost(struct io_batch* batch) {
    int i = 0;
    size_t cost = SEEK_CAP;

	// check result cache
	if (batch->cost != -1)
		return batch->cost;

	if (batch->reqs_size == 0) {
		batch->cost = 0;
		return 0;
	}

	iobatch_scan_sort(batch);
    disk_set_head(&batch->disk, batch->reqs[0].offset);
    for (i = 0; i < batch->reqs_size; i++)
		cost += disk_simulate_request(&batch->disk, &batch->reqs[i]);
	batch->cost = cost;
    return cost;
}
