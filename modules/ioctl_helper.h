#ifndef __IOCTL_HELPER_
#define __IOCTL_HELPER_

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/ioctl.h>
#include <linux/blkdev.h>

#define IOCTL_MAJOR 437


typedef void (kv_set_fn) (struct request_queue *q,
						  int id, char* key, long val);

int ioctl_helper_register_disk(struct request_queue *q,
							   kv_set_fn kv_set);
void ioctl_helper_unregister_disk(struct request_queue *q);
int ioctl_helper_init(int maj);
void ioctl_helper_cleanup(void);

#endif
