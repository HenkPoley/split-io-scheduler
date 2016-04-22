/* 
 * File:   io_batch.h
 * Author: samer
 *
 * Created on March 13, 2014, 5:11 AM
 */

#ifndef IO_BATCH_H
#define	IO_BATCH_H

#include <linux/bio.h>
#include <linux/blk_types.h>
#include <linux/slab.h>
#include <linux/sort.h>

#include "disk_sim.h"

struct io_batch {
	struct disk_sim disk;
    IORequest *reqs;
    int reqs_size;
    int reqs_capacity;
	size_t bytes; // sum of bytes accross reqs
	size_t cost;  // amount of seq I/O these reqs are equivalent to
	size_t bytes_cap;
};

struct io_batch* iobatch_new(int capacity, size_t bytes_cap);

int iobatch_full(struct io_batch* batch);

/**
 * Add an IO request to the batch
 * 
 * @param batch the batch
 * @param request the io request
 */
int iobatch_add_request(struct io_batch* batch,
						off_t offset, int size);

/**
 * Reset the batch internal structures, and free old causes.
 * 
 * @param batch the batch
 */
void iobatch_clear(struct io_batch* batch);

void iobatch_free(struct io_batch* batch);

size_t iobatch_get_cost(struct io_batch* batch);

#endif	/* IO_REQUEST_BATCH_H */

