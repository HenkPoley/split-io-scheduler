#ifndef __DEADLINE_RB_H__
#define __DEADLINE_RB_H__

#include <linux/elevator.h>
#include <linux/rbtree.h>

void expire_rb_add(struct rb_root *root, struct request *rq);
void expire_rb_del(struct rb_root *root, struct request *rq);
struct request *expire_rb_find(struct rb_root *root, unsigned long expire);
struct request *expire_rb_first_request(struct rb_root *root);
struct request *expire_rb_last_request(struct rb_root *root);
struct request *expire_rb_prev_request(struct request *rq);
struct request *expire_rb_next_request(struct request *rq);
void dump_expire_rb_tree(struct rb_root *root);

#endif
