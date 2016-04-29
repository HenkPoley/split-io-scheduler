#include <linux/slab.h>
#include "split_sched.h"
#include <linux/jiffies.h>
#include <linux/time.h>
#include <linux/list.h>
#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/module.h>

#include "split_sched.h"

struct fsync_desc *get_fsync_desc(struct file* file, int datasync){
	struct io_work *io_work;
	struct fsync_desc *fsync_desc = (struct fsync_desc*)kmalloc(sizeof(struct fsync_desc), GFP_KERNEL);
	if(fsync_desc == NULL){
		return ERR_PTR(-ENOMEM);
	}

	do_gettimeofday(&fsync_desc->f_time);
	fsync_desc->f_file = file;
	fsync_desc->f_datasync = datasync;
	fsync_desc->f_ret = 0;
	fsync_desc->owner_batch = NULL;
	fsync_desc->acct = NULL;
	fsync_desc->f_dirty_pages = 0;

	io_work = &fsync_desc->io_work;
	io_work->type = SCHED_SYS_FSYNC;
	io_work->expire = 0;
	INIT_LIST_HEAD(&io_work->list);
	RB_CLEAR_NODE(&io_work->rb_node);

	init_completion(&fsync_desc->f_completion);

	return fsync_desc;

}

void put_fsync_desc(struct fsync_desc* fsync_desc){
	kfree(fsync_desc);
}

struct mkdir_desc *get_mkdir_desc(struct inode *dir, struct dentry *dentry, int mode){
	struct io_work *io_work;
	struct mkdir_desc *mkdir_desc = kmalloc(sizeof(struct mkdir_desc), GFP_KERNEL);

	if(mkdir_desc == NULL){
		return ERR_PTR(-ENOMEM);
	}

	do_gettimeofday(&mkdir_desc->time);
	mkdir_desc->dir = dir;
	mkdir_desc->dentry = dentry;
	mkdir_desc->mode = mode;
	mkdir_desc->ret = 0;
	mkdir_desc->owner_batch = NULL;
	mkdir_desc->acct = NULL;

	io_work = &mkdir_desc->io_work;
	io_work->type = SCHED_SYS_MKDIR;
	io_work->expire = 0;
	INIT_LIST_HEAD(&io_work->list);
	RB_CLEAR_NODE(&io_work->rb_node);

	init_completion(&mkdir_desc->completion);

	return mkdir_desc;
}


void put_mkdir_desc(struct mkdir_desc *mkdir_desc){
	kfree(mkdir_desc);
}
	

struct wrdesc *get_wrdesc(struct file *file, size_t count, loff_t *pos){
	struct io_work *io_work;
	struct wrdesc *wrdesc = kmalloc(sizeof(struct wrdesc), GFP_KERNEL);

	if(wrdesc == NULL){
		return ERR_PTR(-ENOMEM);
	}

	do_gettimeofday(&wrdesc->w_time);
	wrdesc->w_file = file;
	wrdesc->w_count = count;
	wrdesc->w_pos = *pos;
	wrdesc->w_ret = 0;
	wrdesc->owner_batch = NULL;
	wrdesc->acct = NULL;


	io_work = &wrdesc->io_work;
	io_work->type = SCHED_SYS_WRITE;
	io_work->expire = 0;
	INIT_LIST_HEAD(&io_work->list);
	RB_CLEAR_NODE(&io_work->rb_node);

	init_completion(&wrdesc->w_completion);

	return wrdesc;
}

void put_wrdesc(struct wrdesc *wrdesc){
	kfree(wrdesc);
}

//In priciple, I could just have used struct request
//the queuelist field to link it to list
//and the private[] fields to store my data
//But for consistency I am still allocating a new structure here
//yangsuli 2/27/2014
struct req_desc *get_req_desc(struct request_queue *q, struct request *rq, gfp_t gfp_mask){
	struct io_work *io_work;
	struct req_desc *req_desc = kmalloc(sizeof(struct req_desc), gfp_mask);
	if(req_desc == NULL)
		return ERR_PTR(-ENOMEM);


	req_desc->rq = rq;
	req_desc->q = q;
	req_desc->owner_batch = NULL;
	req_desc->acct = NULL;

	io_work = &req_desc->io_work;
	if(rq->cmd_flags & REQ_WRITE){
		io_work->type = SCHED_WRITE_REQ;
	}else{
		io_work->type = SCHED_READ_REQ;
	}
	io_work->expire = 0;
	INIT_LIST_HEAD(&io_work->list);
	RB_CLEAR_NODE(&io_work->rb_node);

	return req_desc;
}

void put_req_desc(struct req_desc* req_desc){
	kfree(req_desc);
}

/* typedef int (elevator_set_req_fn) (struct request_queue *, struct request *,
 *				   struct bio *, gfp_t);
 * TODO: That the struct bio *bio is passed in here now, probably means we need
 *   to do something with it.. */
int split_set_request(struct request_queue *q, struct request *rq,
				struct bio *bio, gfp_t gfp_mask)
{
	struct req_desc *req_desc = NULL;

	req_desc = get_req_desc(q, rq, gfp_mask);
	if(IS_ERR(req_desc)){
		printk("get req_desc failed with error %lu!\n", PTR_ERR(req_desc));
		return 1;
	}
	rq->elv.priv[0] = req_desc;

	RQ_CAUSES(rq) = new_cause_list();

	return 0;
}


void split_put_request(struct request *rq){
	struct req_desc *req_desc = rq->elv.priv[0];
	put_req_desc(req_desc);

	rq->elv.priv[0] = NULL;
	put_cause_list_safe((struct cause_list *)RQ_CAUSES(rq));
}

