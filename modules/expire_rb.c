#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/compiler.h>
#include <linux/rbtree.h>
#include "expire_rb.h"

void expire_rb_add(struct rb_root *root, struct request *rq)
{
	struct rb_node **p = &root->rb_node;
	struct rb_node *parent = NULL;
	struct request *__rq;

	while (*p) {
		parent = *p;
		__rq = rb_entry(parent, struct request, expire_rb_node);
		if (((unsigned long) (rq)->csd.llist.next) < ((unsigned long) (__rq)->csd.llist.next))
			p = &(*p)->rb_left;

		else if (((unsigned long) (rq)->csd.llist.next) >= ((unsigned long) (__rq)->csd.llist.next))
			p = &(*p)->rb_right;
	}

	rb_link_node(&rq->expire_rb_node, parent, p);
	rb_insert_color(&rq->expire_rb_node, root);
}

void expire_rb_del(struct rb_root *root, struct request *rq)
{
	BUG_ON(RB_EMPTY_NODE(&rq->expire_rb_node));
	rb_erase(&rq->expire_rb_node, root);
	RB_CLEAR_NODE(&rq->expire_rb_node);
}

struct request *expire_rb_find(struct rb_root *root, unsigned long expire)
{
	struct rb_node *n = root->rb_node;
	struct request *rq;

	while(n){
		rq = rb_entry(n, struct request, expire_rb_node);
		if(expire < ((unsigned long) (rq)->csd.llist.next))
			n = n->rb_left;

		else if (expire > ((unsigned long) (rq)->csd.llist.next))
			n = n->rb_right;
		else
			return rq;
	}

	return NULL;
}

struct request *expire_rb_first_request(struct rb_root *root)
{
	struct rb_node *rbfirst = rb_first(root);

	if(rbfirst)
		return rb_entry(rbfirst, struct request, expire_rb_node);

	return NULL;
}


struct request *expire_rb_last_request(struct rb_root *root)
{
	struct rb_node *rblast = rb_last(root);

	if(rblast)
		return rb_entry(rblast, struct request, expire_rb_node);

	return NULL;
}


struct request *expire_rb_prev_request(struct request *rq)
{
	struct rb_node *rbprev = rb_prev(&rq->expire_rb_node);

	if(rbprev)
		return rb_entry(rbprev, struct request, expire_rb_node);

	return NULL;
}

struct request *expire_rb_next_request(struct request *rq)
{
	struct rb_node *rbnext = rb_next(&rq->expire_rb_node);

	if(rbnext)
		return rb_entry(rbnext, struct request, expire_rb_node);

	return NULL;
}

void dump_expire_rb_tree(struct rb_root *root){
	struct request *rq = expire_rb_first_request(root);

	if(!rq){
		printk("\nempty expire rb tree\n");
	}

	printk("expire rb tree dump begins\n");

	while(rq != NULL){
		printk("rq %p expire time %lu\n", rq, ((unsigned long) (rq)->csd.llist.next));
		rq = expire_rb_next_request(rq);
	}

	printk("expire rb tree dump ends\n\n");
}

