#include "disk_sim.h"

static size_t offset_diff(off_t o1, off_t o2) {
	return (o1>o2) ? (o1-o2) : (o2-o1);
}

void disk_init(struct disk_sim* disk) {
    disk->head_location = 0;
}

size_t seek_time(off_t o1, off_t o2) {
	return min(offset_diff(o1, o2), (size_t)SEEK_CAP);
}

/**
 * Estimate the head positioning time
 * 
 * @param request the I/O request
 * @return the positioning time
 */
static size_t disk_position_time(struct disk_sim* disk,
								 IORequest* request) {
	return min(seek_time(disk->head_location, request->offset),
			   (size_t)SEEK_CAP);
}

size_t disk_simulate_request(struct disk_sim* disk,
							 IORequest* request) {
	size_t cost;
	cost = (request->size + disk_position_time(disk, request));
    disk->head_location = request->offset + request->size;
    return cost;
}

void disk_set_head(struct disk_sim* disk, off_t location) {
    disk->head_location = location;
}
