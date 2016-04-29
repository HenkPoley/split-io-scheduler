#ifndef __SPLIT_SCHED_H__
#define __SPLIT_SCHED_H__
#include <linux/fs.h>
#include <linux/completion.h>
#include <linux/ioprio.h>
#include <linux/rbtree.h>
#include <linux/cause_tags.h>

#define RQ_CAUSES(rq) (rq)->elv.priv[1]
#define RQ_CAUSES_CAST(rq) ((struct cause_list *)(rq)->elv.priv[1])

/*
 * supported I/O related syscall and block level requests for scheduling
 */
enum SPLIT_SCHED_IO_TYPE{
	SCHED_SYS_WRITE,
	SCHED_SYS_FSYNC,
	SCHED_SYS_MKDIR,
	SCHED_READ_REQ,
	SCHED_WRITE_REQ,
};

struct io_work {
	enum SPLIT_SCHED_IO_TYPE type;
	struct list_head list;
	struct rb_node rb_node;
	unsigned long expire;
};

struct io_batch_desc {
	struct list_head work_selected;
	int one_loop_work;
	spinlock_t lock;
};

struct wrdesc {
	struct account *acct;
	struct file *w_file;
	size_t w_count;
	loff_t w_pos;
	struct io_work io_work;
	struct completion w_completion;
	struct timeval w_time;
	struct io_batch_desc *owner_batch;
	int w_ret;
};

struct fsync_desc {
	struct account *acct;
	struct split_io_context* sic;
	struct file* f_file;
	int f_datasync;
	struct io_work io_work;
	struct completion f_completion;
	struct timeval f_time; //time when user called fsync
	struct io_batch_desc* owner_batch;
	int f_ret;
	int f_dirty_pages;
};

struct mkdir_desc {
	struct account *acct;
	struct inode *dir;
	struct dentry *dentry;
	int mode;
	int ret;
	struct io_work io_work;
	struct completion completion;
	struct timeval time;
	struct io_batch_desc* owner_batch;
};

struct req_desc {
	struct account *acct;
	struct io_work io_work;
	struct request *rq;
	struct request_queue *q;
	struct io_batch_desc *owner_batch;
};

static inline struct fsync_desc *FSYNC_IO(struct io_work *io_work){
	return container_of(io_work, struct fsync_desc, io_work);
}

static inline struct wrdesc *WR_IO(struct io_work *io_work){
	return container_of(io_work, struct wrdesc, io_work);
}

static inline struct mkdir_desc *MKDIR_IO(struct io_work *io_work){
	return container_of(io_work, struct mkdir_desc, io_work);
}

static inline struct req_desc *REQ_IO(struct io_work *io_work){
	return container_of(io_work, struct req_desc, io_work);
}

struct fsync_desc* get_fsync_desc(struct file* file, int datasync);
void put_fsync_desc(struct fsync_desc* fsync_desc);

struct wrdesc *get_wrdesc(struct file *file, size_t count, loff_t *pos);
void put_wrdesc(struct wrdesc *wrdesc);

struct mkdir_desc *get_mkdir_desc(struct inode *dir, struct dentry *dentry, int mode);
void put_mkdir_desc(struct mkdir_desc *mkdir_desc);

struct req_desc *get_req_desc(struct request_queue *q, struct request *rq, gfp_t gfp_mask);
void put_req_desc(struct req_desc* req_desc);

int split_set_request(struct request_queue *q, struct request *rq,
				struct bio *bio, gfp_t gfp_mask);
void split_put_request(struct request *rq);
#endif
