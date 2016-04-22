/* 
 * File:   disk_sim.h
 * Author: samer
 *
 * Created on March 13, 2014, 5:09 AM
 */

#ifndef DISK_SIM_H
#define	DISK_SIM_H

#include <linux/blk_types.h>
#include <linux/kernel.h>

#define SEEK_CAP ((size_t)2*1024*1024) // 2MB

typedef struct {
    off_t offset;
    int size;  /* The size in bytes */
} IORequest;

/**
 * The Disk struct
 */
struct disk_sim {
	off_t head_location;
};

/**
 * Initialize the disk simulator
 */
void disk_init(struct disk_sim* disk);

size_t seek_time(off_t o1, off_t o2);

/**
 * Simulate the processing of a single request
 * 
 * @param request The block I/O request
 * @return the estimated disk time for the request 
 */
size_t disk_simulate_request(struct disk_sim* disk, IORequest* request);

void disk_set_head(struct disk_sim* disk, off_t location);

#endif	/* DISK_SIM_H */

