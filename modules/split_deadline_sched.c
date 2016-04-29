/*
 *  Deadline i/o scheduler.
 *
 *  Copyright (C) 2002 Jens Axboe <axboe@kernel.dk>
 */
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
#include <linux/time.h>
#include <linux/delay.h>
#include <linux/random.h>
#include <linux/kthread.h>
#include <linux/writeback.h>
#include <linux/backing-dev.h>

#include "split_account.h"
#include "split_sched.h"
#include "ioctl_helper.h"

/*
 * See Documentation/block/deadline-iosched.txt
 */
static const int read_expire = HZ / 2;  /* max time before a read is submitted. */
static const int write_expire = 5 * HZ; /* ditto for writes, these limits are SOFT! */
static const int writes_starved = 2;    /* max times reads can starve a write */
static const int fifo_batch = 16;       /* # of sequential requests treated as one
				     by the above parameters. For throughput. */

static const int WRITE_ALLOW_THRES = 1000;
static const int DIRTY_PAGE_THRES = 100;
static const int FSYNC_WB_PAGES = 100;
static const int WRITE_WB_PAGES = 100;

struct deadline_data {
	/*
	 * run time data
	 */
	struct request_queue *queue;
	struct acct_hash acct_hash;
	char magic;
	struct task_struct *sched_thread;
	int last_accepted[2];
	int last_issued[2];

	/*
	 * requests (deadline_rq s) are present on both sort_list and fifo_list
	 */
	struct rb_root sort_list[2];	
	struct list_head fifo_list[2];

	/*
	 * next in sort order. read, write or both are NULL
	 */
	struct request *next_rq[2];
	unsigned int batching;		/* number of sequential requests made */
	sector_t last_sector;		/* head position */ unsigned int starved;		/* times reads have starved writes */ 
	/*
	 * settings that change how the i/o scheduler behaves
	 */
	int fifo_expire[2];
	int fifo_batch;
	int writes_starved;
	int front_merges;
};

inline int get_time_diff(struct timeval *end, struct timeval *start){
	return (end->tv_sec - start->tv_sec) * 1000 + (end->tv_usec - start->tv_usec) / 1000;
}

static inline long inode_get_dirty_pages(struct inode *inode){
	return (inode->i_private1) >> 8;
}

static inline void check_dirty_number(struct deadline_data *dd, struct inode *inode){
	if((inode->i_private1 & 0xff) == (dd->magic & 0xff)){//our magic number
		return;
	}
	inode->i_private1 = dd->magic;
}


static inline void 
inode_inc_dirty_pages(struct deadline_data *dd, struct inode *inode, int num){
	check_dirty_number(dd, inode);
	//Don't set dirty_pages to negative
	if(num < 0 && inode_get_dirty_pages(inode) + num < 0)
		return;
	inode->i_private1 += (num << 8);
}

static inline void 
inode_dec_dirty_pages(struct deadline_data *dd, struct inode *inode, int num){
	inode_inc_dirty_pages(dd, inode, -num);
}


struct inode_write_back_work {
	struct inode *inode;
	int nr_to_write;
	struct work_struct work;
};


static int __filemap_fdatawrite_pages(struct address_space *mapping, int nr_to_write, int sync_mode){
	int ret;
	struct writeback_control wbc = {
		.sync_mode = sync_mode,
		.nr_to_write = nr_to_write,
		.range_start = 0,
		.range_end = LLONG_MAX,
	};

	wbc.nr_to_write = nr_to_write / 8; //because ext4 will just multiply our number by 8...

	if(!mapping_cap_writeback_dirty(mapping)){
		return 0;
	}

	ret = do_writepages(mapping, &wbc);
	printk("__filemap_fdatawrite_pages written %ld pages\n", nr_to_write - wbc.nr_to_write);
	return ret;
}

void do_inode_wb_work(struct work_struct *work){
	struct inode_write_back_work *inode_write_back_work = container_of(work, struct inode_write_back_work, work);
	struct inode *inode = inode_write_back_work->inode;
	int nr_to_write = inode_write_back_work->nr_to_write;
	if(unlikely(!inode))
		goto exit;
	__filemap_fdatawrite_pages(inode->i_mapping, nr_to_write, WB_SYNC_NONE);
exit:
	kfree(inode_write_back_work);
}

int issue_async_inode_wb(struct inode *inode, int nr_to_write){
	struct inode_write_back_work *inode_write_back_work = NULL;

	if(!inode)
		return -EINVAL;

	inode_write_back_work =	kmalloc(sizeof(struct inode_write_back_work), GFP_ATOMIC);
	if(unlikely(!inode_write_back_work)){
		return -ENOMEM;
	}

	inode_write_back_work->inode = inode;
	inode_write_back_work->nr_to_write = nr_to_write;
	INIT_WORK(&inode_write_back_work->work, do_inode_wb_work);
	schedule_work(&inode_write_back_work->work);

	return 0;

}


static void deadline_move_request(struct deadline_data *, struct request *);

static inline struct rb_root *
deadline_rb_root(struct deadline_data *dd, struct request *rq)
{
	return &dd->sort_list[rq_data_dir(rq)];
}

/*
 * get the request after `rq' in sector-sorted order
 */
static inline struct request *
deadline_latter_request(struct request *rq)
{
	struct rb_node *node = rb_next(&rq->rb_node);

	if (node)
		return rb_entry_rq(node);

	return NULL;
}

static void
deadline_add_rq_rb(struct deadline_data *dd, struct request *rq)
{
	struct rb_root *root = deadline_rb_root(dd, rq);

	elv_rb_add(root, rq);
}

static inline void
deadline_del_rq_rb(struct deadline_data *dd, struct request *rq)
{
	const int data_dir = rq_data_dir(rq);

	if (dd->next_rq[data_dir] == rq)
		dd->next_rq[data_dir] = deadline_latter_request(rq);

	elv_rb_del(deadline_rb_root(dd, rq), rq);
}

/*
 * add rq to rbtree and fifo
 */
static void
deadline_add_request(struct request_queue *q, struct request *rq)
{
	struct deadline_data *dd = q->elevator->elevator_data;
	const int data_dir = rq_data_dir(rq);
	dd->last_accepted[data_dir]++;

	deadline_add_rq_rb(dd, rq);

	/*
	 * set expire time and add to fifo list
	 */
	((rq)->csd.llist.next = (void *) (jiffies + dd->fifo_expire[data_dir]));

	list_add_tail(&rq->queuelist, &dd->fifo_list[data_dir]);
}

/*
 * remove rq from rbtree and fifo.
 */
static void deadline_remove_request(struct request_queue *q, struct request *rq)
{
	struct deadline_data *dd = q->elevator->elevator_data;

	rq_fifo_clear(rq);
	deadline_del_rq_rb(dd, rq);
}

static int
deadline_merge(struct request_queue *q, struct request **req, struct bio *bio)
{
	struct deadline_data *dd = q->elevator->elevator_data;
	struct request *__rq;
	int ret;

	/*
	 * check for front merge
	 */
	if (dd->front_merges) {
		sector_t sector = bio->bi_iter.bi_sector + bio_sectors(bio);

		__rq = elv_rb_find(&dd->sort_list[bio_data_dir(bio)], sector);
		if (__rq) {
			BUG_ON(sector != blk_rq_pos(__rq));

			if (elv_rq_merge_ok(__rq, bio)) {
				ret = ELEVATOR_FRONT_MERGE;
				goto out;
			}
		}
	}

	return ELEVATOR_NO_MERGE;
out:
	*req = __rq;
	return ret;
}

static void deadline_merged_request(struct request_queue *q,
				    struct request *req, int type)
{
	struct deadline_data *dd = q->elevator->elevator_data;

	/*
	 * if the merge was a front merge, we need to reposition request
	 */
	if (type == ELEVATOR_FRONT_MERGE) {
		elv_rb_del(deadline_rb_root(dd, req), req);
		deadline_add_rq_rb(dd, req);
	}
}

static void
deadline_merged_requests(struct request_queue *q, struct request *req,
			 struct request *next)
{
	/*
	 * if next expires before rq, assign its expire time to rq
	 * and move into next position (next will be deleted) in fifo
	 */
	if (!list_empty(&req->queuelist) && !list_empty(&next->queuelist)) {
		if (
			time_before(((unsigned long) (next)->csd.llist.next), 
				((unsigned long) (req)->csd.llist.next))) {
			list_move(&req->queuelist, &next->queuelist);
			((req)->csd.llist.next = (void *) (((unsigned long) (next)->csd.llist.next)));
		}
	}

	/*
	 * kill knowledge of next, this one is a goner
	 */
	deadline_remove_request(q, next);
}

static inline void
inode_dec_dirty_for_submitted_rq(struct deadline_data *dd, struct request *rq){
	struct bio *bio = rq->bio;
	struct cause_list *causes;
	struct inode *inode;
	int i;

	if(rq_data_dir(rq) == READ)
		return;

	while(bio != NULL){
		if(bio->cll){
			for(i = 0; i < bio->cll->item_count; i++){
				causes = bio->cll->items[i];
				inode = causes->inode;
				if (inode) {
					inode_dec_dirty_pages(dd, inode, 1);
				}
				causes->inode = NULL;
				causes->callback_q = NULL;
			}
		}
		bio = bio->bi_next;
	}
}

/*
 * move request from sort list to dispatch queue.
 */
static inline void
deadline_move_to_dispatch(struct deadline_data *dd, struct request *rq)
{
	struct request_queue *q = rq->q;

	deadline_remove_request(q, rq);
	inode_dec_dirty_for_submitted_rq(dd, rq);
	dd->last_issued[rq_data_dir(rq)]++;
	elv_dispatch_add_tail(q, rq);
}

/*
 * move an entry to dispatch queue
 */
static void
deadline_move_request(struct deadline_data *dd, struct request *rq)
{
	const int data_dir = rq_data_dir(rq);

	dd->next_rq[READ] = NULL;
	dd->next_rq[WRITE] = NULL;
	dd->next_rq[data_dir] = deadline_latter_request(rq);

	dd->last_sector = rq_end_sector(rq);

	/*
	 * take it off the sort and fifo list, move
	 * to dispatch queue
	 */
	deadline_move_to_dispatch(dd, rq);
}

/*
 * deadline_check_fifo returns 0 if there are no expired requests on the fifo,
 * 1 otherwise. Requires !list_empty(&dd->fifo_list[data_dir])
 */
static inline int deadline_check_fifo(struct deadline_data *dd, int ddir)
{
	struct request *rq = rq_entry_fifo(dd->fifo_list[ddir].next);

	/*
	 * rq is expired!
	 */
	if (time_after(jiffies, ((unsigned long) (rq)->csd.llist.next)))
		return 1;

	return 0;
}

/*
ssize_t deadline_write_entry(struct request_queue *rq, struct file *file, size_t count, loff_t *pos, void** opaque, int sched_uniq){
	struct wrdesc *wrdesc = NULL;
	struct deadline_data *dd = rq->elevator->elevator_data;
	struct account *acct = NULL;
	unsigned long flags;
	int need_throttle = 0;
	struct inode *inode;
	int num_dirty_pages = 0;
	int ret;
	struct timeval wb_start, wb_end;
	int last_issued = 0;

	wrdesc = get_wrdesc(file, count, pos);
	if(IS_ERR(wrdesc)){
		return PTR_ERR(wrdesc);
	}

	if(wrdesc == NULL){
		printk(KERN_ERR "wrdesc is NULL\n");
		return -ENOMEM;
	}

	spin_lock_irqsave(rq->queue_lock, flags);

	if(sched_uniq != rq->sched_uniq){
		spin_unlock_irqrestore(rq->queue_lock, flags);
		put_wrdesc(wrdesc);
		*opaque = NULL;
		return -1;
	}

	acct = get_account(&dd->acct_hash, get_account_id(current));
	if(acct){
		wrdesc->acct = acct;
		if(acct->fsync_expire == 200){
			need_throttle = 1;
		}
	}
	spin_unlock_irqrestore(rq->queue_lock, flags);

	inode = file->f_mapping->host;
	num_dirty_pages = inode_get_dirty_pages(inode);

	do_gettimeofday(&wb_start);
	while(num_dirty_pages >= WRITE_ALLOW_THRES){
		blkdev_issue_flush(inode->i_sb->s_bdev, GFP_KERNEL, NULL);
		msleep(10);
		if(num_dirty_pages == inode_get_dirty_pages(inode)){
			ret = issue_async_inode_wb(inode, WRITE_WB_PAGES);
			if (ret != 0){
				printk("issue_async_inode_wb failed with return code %d\n", ret);
			}
			last_issued = 1;
		}else{
			last_issued = 0;
		}
		if(last_issued && num_dirty_pages == inode_get_dirty_pages(inode)){
			inode_dec_dirty_pages(dd, inode, WRITE_WB_PAGES);
		}
		num_dirty_pages = inode_get_dirty_pages(inode);
	}
	do_gettimeofday(&wb_end);
	if(get_time_diff(&wb_end, &wb_start) >= 10){
		printk("write of file %s waiting writeback took %d ms\n", file->f_dentry->d_name.name, get_time_diff(&wb_end, &wb_start));
	}

	do_gettimeofday(&wrdesc->w_time);

	*opaque = wrdesc;

	return 0;
}
*/

/*
void deadline_write_return(struct request_queue *q, void *opaque, ssize_t rv, int sched_uniq){
	struct wrdesc *wrdesc = opaque;
	struct timeval now;
	int ret;
	struct inode *inode = wrdesc->w_file->f_mapping->host;
	ret = issue_async_inode_wb(inode, WRITE_WB_PAGES);

	do_gettimeofday(&now);

	if(get_time_diff(&now, &wrdesc->w_time) >= 10){
		printk("write on file %s took %d ms (fsync_expire %d)\n", wrdesc->w_file->f_dentry->d_name.name, get_time_diff(&now, &wrdesc->w_time), wrdesc->acct==NULL?-1:wrdesc->acct->fsync_expire);
	}
	put_wrdesc(wrdesc);
	blkdev_issue_flush(inode->i_sb->s_bdev, GFP_KERNEL, NULL);
}
*/


int deadline_fsync_entry(struct request_queue *rq, struct file* file, int datasync, void **opaque, int sched_uniq){
	struct fsync_desc *fsync_desc = NULL;
	struct deadline_data *dd = rq->elevator->elevator_data;
	struct inode *inode;
	int num_dirty_pages = 0;
	int ret;
	struct account *acct = NULL;
	unsigned long flags;
	struct timeval wb_start, wb_end;
	int need_wb = 0;
	int issued_too_much = 0;

	fsync_desc = get_fsync_desc(file, datasync);
	if(IS_ERR(fsync_desc))
		return PTR_ERR(fsync_desc);

	if(fsync_desc == NULL){
		printk(KERN_ERR "fsync_desc is NULL\n");
		return -ENOMEM;
	}

	spin_lock_irqsave(rq->queue_lock, flags);

	if(sched_uniq != rq->sched_uniq){
		spin_unlock_irqrestore(rq->queue_lock, flags);
		put_fsync_desc(fsync_desc);
		*opaque = NULL;
		return -1;
	}

	acct = get_account(&dd->acct_hash, get_account_id(current));
	if(acct){
		fsync_desc->acct = acct;
		if(acct->fsync_expire == 200){
			need_wb = 1;
			if(dd->last_issued[READ] + dd->last_issued[WRITE] >= 1000){
				issued_too_much = 1;
			}else{
				issued_too_much = 0;
			}
		}
	}
	spin_unlock_irqrestore(rq->queue_lock, flags);

	inode = file->f_mapping->host;
	num_dirty_pages = inode_get_dirty_pages(inode);
	if(num_dirty_pages >= 200)
		printk("fsync on file %s called with dirty pages %d\n", file->f_path.dentry->d_name.name, num_dirty_pages);

	if(need_wb){
	do_gettimeofday(&wb_start);
	while(num_dirty_pages >= DIRTY_PAGE_THRES){
		blkdev_issue_flush(inode->i_sb->s_bdev, GFP_KERNEL, NULL);
		//if(!issued_too_much){
		ret = issue_async_inode_wb(inode, FSYNC_WB_PAGES);
		if (ret != 0){
			printk("issue_async_inode_wb failed with return code %d\n", ret);
		}
		//}
		msleep(10);
		//if(!issued_too_much && num_dirty_pages == inode_get_dirty_pages(inode)){
		if(num_dirty_pages == inode_get_dirty_pages(inode)){
			//for whatever reason, dirty page count is not going down...
			//FIXME: we need to debug this...
			inode_dec_dirty_pages(dd, inode, FSYNC_WB_PAGES);
		}
		num_dirty_pages = inode_get_dirty_pages(inode);
		//printk("after calling issue_async_inode_wb, now num_dirty_pages is %d\n", num_dirty_pages);
	}
	do_gettimeofday(&wb_end);
	printk("issuing writeback took %d ms\n", get_time_diff(&wb_end, &wb_start));
	}


	do_gettimeofday(&fsync_desc->f_time);

	*opaque = fsync_desc;

	return 0;
}

void deadline_fsync_return(struct request_queue *q, void *opaque, int rv, int sched_uniq){
	struct fsync_desc *fsync_desc = opaque;
	struct timeval now;

	do_gettimeofday(&now);
	
	if(get_time_diff(&now, &fsync_desc->f_time) >= 10 || (fsync_desc->acct && fsync_desc->acct->fsync_expire == 200)){
	printk("fsync on file %s took %d ms (fsync_expire %d)\n", fsync_desc->f_file->f_path.dentry->d_name.name, get_time_diff(&now, &fsync_desc->f_time), fsync_desc->acct==NULL?-1:fsync_desc->acct->fsync_expire);
	}
	put_fsync_desc(fsync_desc);
}
	


void deadline_causes_dirty(struct request_queue *q, struct cause_list *causes, struct task_struct *new, struct inode *inode, long pos){
	struct deadline_data *dd = q->elevator->elevator_data;

	if(!list_empty(&causes->items)){
		WARN_ON(causes->item_count > 1 && causes->inode == NULL);
	}

	if(causes->inode == NULL){
		//inode hasn't been set, means page corresponding to this cause_list hasn't been dirtied eariler
		causes->inode = inode;
		inode_inc_dirty_pages(dd, inode, 1);
	}else if(causes->inode != inode){
		printk("causes->inode not NULL %p new inode %p\n", causes->inode, inode);
	}

}

void deadline_causes_free(struct request_queue *q, struct cause_list *causes){
	struct deadline_data *dd = q->elevator->elevator_data;
	struct inode *inode = causes->inode;
	if(inode){
		inode_dec_dirty_pages(dd, inode, 1);
	}
}

/*
 * deadline_dispatch_requests selects the best request according to
 * read/write expire, fifo_batch, etc
 */
static int deadline_dispatch_requests(struct request_queue *q, int force)
{
	struct deadline_data *dd = q->elevator->elevator_data;
	const int reads = !list_empty(&dd->fifo_list[READ]);
	const int writes = !list_empty(&dd->fifo_list[WRITE]);
	struct request *rq;
	int data_dir;

	/*
	 * batches are currently reads XOR writes
	 */
	if (dd->next_rq[WRITE])
		rq = dd->next_rq[WRITE];
	else
		rq = dd->next_rq[READ];

	if (rq && dd->batching < dd->fifo_batch)
		/* we have a next request are still entitled to batch */
		goto dispatch_request;

	/*
	 * at this point we are not running a batch. select the appropriate
	 * data direction (read / write)
	 */

	if (reads) {
		BUG_ON(RB_EMPTY_ROOT(&dd->sort_list[READ]));

		if (writes && (dd->starved++ >= dd->writes_starved))
			goto dispatch_writes;

		data_dir = READ;

		goto dispatch_find_request;
	}

	/*
	 * there are either no reads or writes have been starved
	 */

	if (writes) {
dispatch_writes:
		BUG_ON(RB_EMPTY_ROOT(&dd->sort_list[WRITE]));

		dd->starved = 0;

		data_dir = WRITE;

		goto dispatch_find_request;
	}

	return 0;

dispatch_find_request:
	/*
	 * we are not running a batch, find best request for selected data_dir
	 */
	if (deadline_check_fifo(dd, data_dir) || !dd->next_rq[data_dir]) {
		/*
		 * A deadline has expired, the last request was in the other
		 * direction, or we have run out of higher-sectored requests.
		 * Start again from the request with the earliest expiry time.
		 */
		rq = rq_entry_fifo(dd->fifo_list[data_dir].next);
	} else {
		/*
		 * The last req was the same dir and we have a next request in
		 * sort order. No expired requests so continue on from here.
		 */
		rq = dd->next_rq[data_dir];
	}

	dd->batching = 0;

dispatch_request:
	/*
	 * rq is the selected appropriate request.
	 */
	dd->batching++;
	deadline_move_request(dd, rq);

	return 1;
}

static void deadline_kv_set_callback(struct request_queue *q,
		int id, char *key, long val){
	struct deadline_data *dd = q->elevator->elevator_data;
	struct account *account;
	unsigned long flags;

	spin_lock_irqsave(q->queue_lock, flags);
	account = get_account(&dd->acct_hash, id);
	if(account){
		printk("key: %s\n", key);
		if(strcmp(key, "read_expire") == 0){
			printk("set account %d read_expire %ld\n", id, val);
			account->read_expire = val;
		}
		if(strcmp(key, "fsync_expire") == 0){
			if(val == 300)
				val = 5;
			if(val == 6000){
				val = 200;
			}
			printk("set account %d fsync_expire %ld\n", id, val);
			account->fsync_expire = val;
		}
	}
	spin_unlock_irqrestore(q->queue_lock, flags);
}
static const int sched_sleep = 100;
int loop = 0;
static int sched_thread_func(void *arg){
	unsigned long flags;
	struct deadline_data *dd = arg;

	while(!kthread_should_stop()){
		spin_lock_irqsave(dd->queue->queue_lock, flags);
		if(dd->last_accepted[READ] == 0 &&
			dd->last_accepted[WRITE] == 0 &&
			dd->last_issued[READ] == 0 &&
			dd->last_issued[WRITE] == 0){
			goto reset_stat;
		}

		printk("for last %d ms:\n", sched_sleep);
		printk("last accepted reads %d writes %d\n", dd->last_accepted[READ], dd->last_accepted[WRITE]);
		printk("last issued reads %d writes %d\n", dd->last_issued[READ], dd->last_issued[WRITE]);

reset_stat:
		dd->last_accepted[READ] = 0;
		dd->last_accepted[WRITE] = 0;
		dd->last_issued[READ] = 0;
		dd->last_issued[WRITE] = 0;

		spin_unlock_irqrestore(dd->queue->queue_lock, flags);
		msleep(sched_sleep);
	}

	return 0;
}


int start_sched_thread(struct deadline_data *dd){
	printk("start_sched_thread called\n");

	dd->sched_thread = kthread_create(sched_thread_func, dd, "deadline_sched_thread");

	if(!IS_ERR(dd->sched_thread)){
		wake_up_process(dd->sched_thread);
	}else{
		return -ENOMEM;
	}

	return 0;
}

void stop_sched_thread(struct deadline_data *dd){
	printk("stop sched_thread called\n");
	kthread_stop(dd->sched_thread);
}

/* typedef int (elevator_init_fn) (struct request_queue *,
 * 				struct elevator_type *e);
 * See also: block/cfq-iosched.c */
/*
 * initialize elevator private data (deadline_data).
 */
static int deadline_init_queue(struct request_queue *q, struct elevator_type *e)
{
	struct elevator_queue *eq;
	struct deadline_data *dd;
	int ret;

	eq = elevator_alloc(q, e);
	if (!eq)
		return -ENOMEM;

	dd = kmalloc_node(sizeof(*dd), GFP_KERNEL | __GFP_ZERO, q->node);
	if (!dd)
		return -ENOMEM;
	dd->queue = q;
	
	eq->elevator_data = dd;

	init_acct_hash(&dd->acct_hash);
	get_random_bytes(&dd->magic, sizeof(dd->magic));
	dd->magic &= 0xff;

	INIT_LIST_HEAD(&dd->fifo_list[READ]);
	INIT_LIST_HEAD(&dd->fifo_list[WRITE]);
	dd->sort_list[READ] = RB_ROOT;
	dd->sort_list[WRITE] = RB_ROOT;
	dd->fifo_expire[READ] = read_expire;
	dd->fifo_expire[WRITE] = write_expire;
	dd->writes_starved = writes_starved;
	dd->front_merges = 1;
	dd->fifo_batch = fifo_batch;

	ret = start_sched_thread(dd);
	if(ret != 0){
		goto fail;
	}
	
	spin_lock_irq(q->queue_lock);
	q->elevator = eq;
	spin_unlock_irq(q->queue_lock);

	ioctl_helper_register_disk(q, deadline_kv_set_callback);

	return 0;

fail:
	kfree(dd);
	return -ENOMEM;
}

static void deadline_exit_queue(struct elevator_queue *e)
{
	struct deadline_data *dd = e->elevator_data;

	BUG_ON(!list_empty(&dd->fifo_list[READ]));
	BUG_ON(!list_empty(&dd->fifo_list[WRITE]));

	stop_sched_thread(dd);
	free_accounts(&dd->acct_hash);
	ioctl_helper_unregister_disk(dd->queue);

	kfree(dd);
}
/*
 * sysfs parts below
 */

static ssize_t
deadline_var_show(int var, char *page)
{
	return sprintf(page, "%d\n", var);
}

static ssize_t
deadline_var_store(int *var, const char *page, size_t count)
{
	char *p = (char *) page;

	*var = simple_strtol(p, &p, 10);
	return count;
}

#define SHOW_FUNCTION(__FUNC, __VAR, __CONV)				\
static ssize_t __FUNC(struct elevator_queue *e, char *page)		\
{									\
	struct deadline_data *dd = e->elevator_data;			\
	int __data = __VAR;						\
	if (__CONV)							\
		__data = jiffies_to_msecs(__data);			\
	return deadline_var_show(__data, (page));			\
}
SHOW_FUNCTION(deadline_read_expire_show, dd->fifo_expire[READ], 1);
SHOW_FUNCTION(deadline_write_expire_show, dd->fifo_expire[WRITE], 1);
SHOW_FUNCTION(deadline_writes_starved_show, dd->writes_starved, 0);
SHOW_FUNCTION(deadline_front_merges_show, dd->front_merges, 0);
SHOW_FUNCTION(deadline_fifo_batch_show, dd->fifo_batch, 0);
#undef SHOW_FUNCTION

#define STORE_FUNCTION(__FUNC, __PTR, MIN, MAX, __CONV)			\
static ssize_t __FUNC(struct elevator_queue *e, const char *page, size_t count)	\
{									\
	struct deadline_data *dd = e->elevator_data;			\
	int __data;							\
	int ret = deadline_var_store(&__data, (page), count);		\
	if (__data < (MIN))						\
		__data = (MIN);						\
	else if (__data > (MAX))					\
		__data = (MAX);						\
	if (__CONV)							\
		*(__PTR) = msecs_to_jiffies(__data);			\
	else								\
		*(__PTR) = __data;					\
	return ret;							\
}
STORE_FUNCTION(deadline_read_expire_store, &dd->fifo_expire[READ], 0, INT_MAX, 1);
STORE_FUNCTION(deadline_write_expire_store, &dd->fifo_expire[WRITE], 0, INT_MAX, 1);
STORE_FUNCTION(deadline_writes_starved_store, &dd->writes_starved, INT_MIN, INT_MAX, 0);
STORE_FUNCTION(deadline_front_merges_store, &dd->front_merges, 0, 1, 0);
STORE_FUNCTION(deadline_fifo_batch_store, &dd->fifo_batch, 0, INT_MAX, 0);
#undef STORE_FUNCTION

#define DD_ATTR(name) \
	__ATTR(name, S_IRUGO|S_IWUSR, deadline_##name##_show, \
				      deadline_##name##_store)

static struct elv_fs_entry deadline_attrs[] = {
	DD_ATTR(read_expire),
	DD_ATTR(write_expire),
	DD_ATTR(writes_starved),
	DD_ATTR(front_merges),
	DD_ATTR(fifo_batch),
	__ATTR_NULL
};

static struct elevator_type iosched_deadline = {
	.ops = {
		.elevator_merge_fn = 		deadline_merge,
		.elevator_merged_fn =		deadline_merged_request,
		.elevator_merge_req_fn =	deadline_merged_requests,
		.elevator_dispatch_fn =		deadline_dispatch_requests,
		.elevator_add_req_fn =		deadline_add_request,
		.elevator_former_req_fn =	elv_rb_former_request,
		.elevator_latter_req_fn =	elv_rb_latter_request,
		//.elevator_write_entry_fn = 	deadline_write_entry,
		//.elevator_write_return_fn = 	deadline_write_return,
		.elevator_fsync_entry_fn = 	deadline_fsync_entry,
		.elevator_fsync_return_fn = 	deadline_fsync_return,
		.elevator_causes_dirty_fn = 	deadline_causes_dirty,
		.elevator_causes_free_fn =	deadline_causes_free,
		.elevator_init_fn =		deadline_init_queue,
		.elevator_exit_fn =		deadline_exit_queue,
	},

	.elevator_attrs = deadline_attrs,
	.elevator_name = "new_sysdeadline",
	.elevator_owner = THIS_MODULE,
};

static int __init deadline_init(void)
{
	int rv;
	rv = ioctl_helper_init(IOCTL_MAJOR);
	WARN_ON(rv);
	elv_register(&iosched_deadline);

	return 0;
}

static void __exit deadline_exit(void)
{
	ioctl_helper_cleanup();
	elv_unregister(&iosched_deadline);
}

module_init(deadline_init);
module_exit(deadline_exit);

MODULE_AUTHOR("Jens Axboe");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("deadline IO scheduler");
