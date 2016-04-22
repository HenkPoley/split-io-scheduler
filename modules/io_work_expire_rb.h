#ifndef __IO_WORK_RB_H__
#define __IO_WORK_RB_H__

#include <linux/rbtree.h>
#include "split_sched.h"

void io_work_expire_rb_add(struct rb_root *root, struct io_work *io_work);
void io_work_expire_rb_del(struct rb_root *root, struct io_work *io_work);
struct io_work *io_work_expire_rb_find(struct rb_root *root,
		unsigned long expire);
struct io_work *io_work_expire_rb_first_work(struct rb_root *root);
struct io_work *io_work_expire_rb_last_work(struct rb_root *root);
struct io_work *io_work_expire_rb_prev_work(struct io_work *io_work);
struct io_work *io_work_expire_rb_next_work(struct io_work *io_work);
void dump_io_work_expire_rb_tree(struct rb_root *root);


#endif
