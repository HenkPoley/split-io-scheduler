#include <linux/rbtree.h>
#include "split_sched.h"
#include "io_work_expire_rb.h"


void io_work_expire_rb_add(struct rb_root *root, struct io_work *io_work){
	struct rb_node **p = &root->rb_node;
	struct rb_node *parent = NULL;
	struct io_work *__io_work;

	while (*p) {
		parent = *p;
		__io_work = rb_entry(parent, struct io_work, rb_node);

		if (io_work->expire < __io_work->expire)
			p = &(*p)->rb_left;
		else if (io_work->expire >= __io_work->expire)
			p = &(*p)->rb_right;
	}

	rb_link_node(&io_work->rb_node, parent, p);
	rb_insert_color(&io_work->rb_node, root);
}

void io_work_expire_rb_del(struct rb_root *root, struct io_work *io_work)
{
	BUG_ON(RB_EMPTY_NODE(&io_work->rb_node));
	rb_erase(&io_work->rb_node, root);
	RB_CLEAR_NODE(&io_work->rb_node);
}

struct io_work *io_work_expire_rb_find(struct rb_root *root,
		unsigned long expire)
{
	struct rb_node *n = root->rb_node;
	struct io_work *io_work;

	while(n){
		io_work = rb_entry(n, struct io_work, rb_node);
		if(expire < io_work->expire)
			n = n->rb_left;
		else if (expire > io_work->expire)
			n = n->rb_right;
		else
			return io_work;
	}

	return NULL;
}

struct io_work *io_work_expire_rb_first_work(struct rb_root *root){
	struct rb_node *rbfirst = rb_first(root);

	if(rbfirst)
		return rb_entry(rbfirst, struct io_work, rb_node);

	return NULL;
}

struct io_work *io_work_expire_rb_last_work(struct rb_root *root){
	struct rb_node *rblast = rb_last(root);

	if(rblast)
		return rb_entry(rblast, struct io_work, rb_node);

	return NULL;
}

struct io_work *io_work_expire_rb_prev_work(struct io_work *io_work){
	struct rb_node *rbprev = rb_prev(&io_work->rb_node);

	if(rbprev)
		return rb_entry(rbprev, struct io_work, rb_node);

	return NULL;
}

struct io_work *io_work_expire_rb_next_work(struct io_work *io_work){
	struct rb_node *rbnext = rb_next(&io_work->rb_node);

	if(rbnext)
		return rb_entry(rbnext, struct io_work, rb_node);

	return NULL;
}

void dump_io_work_expire_rb_tree(struct rb_root *root){
	struct io_work *io_work = io_work_expire_rb_first_work(root);

	if(!io_work){
		printk("io_work_expire_rb_tree empty\n");
	} 

	printk("\nio_work_expire_rb_tree dump begins\n");

	while(io_work != NULL){
		printk("io_work %p expire %lu\n", io_work, io_work->expire);
		io_work = io_work_expire_rb_next_work(io_work);
	}


	printk("io_work_expire_rb_tree dump begins\n\n");
}

