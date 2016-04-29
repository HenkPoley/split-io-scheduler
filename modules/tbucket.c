#undef __KERNEL__
#define __KERNEL__
#undef MODULE
#define MODULE

#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/cause_tags.h>
#include <linux/fs.h>
#include <asm/uaccess.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/sched.h>
#include <linux/time.h>
#include <linux/list.h>
#include <linux/kthread.h>
#include <linux/export.h>
#include <linux/completion.h>
#include <linux/random.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/iocontext.h>
#include <linux/ioprio.h>
#include <linux/hashtable.h>
#include <linux/hrtimer.h>

#include "ioctl_helper.h"
#include "io_batch.h"
#include "queue.h"

#define KB (1024)
#define PG_SIZE (4*KB)
#define MB (1024*KB)
#define DEFAULT_ACCOUNT (0)
#define DISPATCH_CAP (0)
#define BATCH_SIZE (64)
#define BATCH_BYTE_CAP (32*MB)
#define INSANE_CAP (1000)
#define ANTICIPATION_US (10000) // don't set this to >1s!
#define RUN_SIZE_LIMIT (10*MB)

#define RQ_CAUSES(rq) (rq)->elv.priv[0]
#define RQ_CAUSES_CAST(rq) ((struct cause_list *)(rq)->elv.priv[0])
#define RQ_QUEUE(rq) (rq)->elv.priv[1]
#define RQ_QUEUE_CAST(rq) ((struct queue *)(rq)->elv.priv[1])

struct tbucket_data {
	struct request_queue *q;
	DECLARE_HASHTABLE(accounts_hash, 10); // 1000 buckets
	struct list_head accounts;
	struct list_head queues;
	int queues_count;
	struct timer_list dispatch_nudger;
	int dispatched;
	struct io_batch *batch;
	long run_serve_size;

	struct {
		struct queue *last_queue;
		long queue_runs;
		size_t disk_writes;
		size_t disk_reads;
		size_t disk_norm;
		long io_batches;
		long io_batch_add_seq;
		long io_batch_add_tot;

		long dispatch_declined_no_work;
		long dispatch_declined_throttle;

		struct timespec start_sleep;
		struct timespec sleep_time;
	} stats;
};

struct account {
	// general
	int id;
	struct hlist_node node;
	struct list_head accounts; // other accounts
	struct queue rqueue;   // read requests
	struct queue wqueue;   // write requests
	// specific
	long rate;
	long limit;
	long credit;       // income    (fluctuates, but usually monotonic)
	long estimate;     // memory    (fluctuates)
	long used;         // disk time (monotonic)
	long used_raw;     // disk byte (monotonic)
	long lost;         // income lost due to limit (monotonic)
	long sane;         // how often are causes reasonable?
	long insane[INSANE_CAP]; // how often are causes unreasonable?

	struct timespec last_update;
	struct list_head queue; // request queue
	struct io_batch *batch; // never NULL
	// batch_estimate describes how much we have charged for
	// this batch prior the the batch being complete.  When
	// we actually charge for the batch, we need to refund this.
	size_t batch_estimate;
};

static long count(struct account *account) {
	return account->credit - (account->estimate + account->used);
}

static struct account *queue_to_account(struct queue *queue) {
	if (queue->rw)
		return container_of(queue, struct account, wqueue);
	else
		return container_of(queue, struct account, rqueue);
}

static void trace_queue(char* event, struct request *rq) {
	struct account *account;
	struct timespec now;

	return;

	account = queue_to_account(RQ_QUEUE_CAST(rq));
	getnstimeofday(&now);

	printk(KERN_INFO "VIZ_TRACE %ld.%09ld %s"
		   " req=%p"
		   " account=%d"
		   " rw=%s"
		   " bytes=%d"
		   "\n",
		   now.tv_sec, now.tv_nsec, event,
		   rq,
		   account->id,
		   rq_data_dir(rq) == READ ? "r" : "w",
		   blk_rq_bytes(rq));
}

static struct account *new_account(int account_id) {
	struct account *account;
	account = kmalloc(sizeof(*account), GFP_ATOMIC);
	if (!account)
		goto fail;
	memset(account, 0, sizeof(*account));
	account->batch = iobatch_new(BATCH_SIZE, BATCH_BYTE_CAP);
	if (!account->batch)
		goto fail;
	account->id = account_id;
	getnstimeofday(&account->last_update);
	INIT_HLIST_NODE(&account->node);
	INIT_LIST_HEAD(&account->accounts);
	queue_init(&account->rqueue, 0);
	queue_init(&account->wqueue, 1);
	return account;

 fail:
	if (account && account->batch)
		iobatch_free(account->batch);
	kfree(account);
	return NULL;
}

static void add_account(struct tbucket_data* td,
						struct account *account) {
	hash_add(td->accounts_hash, &account->node, account->id);
	list_add_tail(&account->accounts, &td->accounts);
	list_add_tail(&account->rqueue.queues, &td->queues);
	list_add_tail(&account->wqueue.queues, &td->queues);
	td->queues_count += 2;
}

// elv lock must be held!
//
// return account, regardless of rate
static struct account *get_account(struct tbucket_data* td,
								   int account_id) {
	struct account *account;

	// search existing accounts
	hash_for_each_possible(td->accounts_hash, account,
						   node, account_id) {
		if (account->id == account_id)
			return account;
	}

	// not found!
	account = new_account(account_id);
	if (account) {
		add_account(td, account);
		return account;
	}

	// use default account
	hash_for_each_possible(td->accounts_hash, account,
						   node, DEFAULT_ACCOUNT) {
		if (account->id == DEFAULT_ACCOUNT)
			return account;
	}

	// we should never reach here, as we create the default
	// account during queue init
	BUG_ON("cannot even find default account!");
	return NULL;
}

static long long __safe_div(long long a, long long b, unsigned int line) {
	if (b == 0) {
		printk(KERN_INFO "divide %lld/%lld at tbucket.c:%u", a, b, line);
		if (a == 0)
			return 0;
		else
			return a;
	}
	return a / b;
}

static void normalize_timespec(struct timespec *ts) {
	while (ts->tv_nsec < 0) {
		ts->tv_nsec += NSEC_PER_SEC;
		ts->tv_sec -= 1;
	}
	while (ts->tv_nsec >= NSEC_PER_SEC) {
		ts->tv_nsec -= NSEC_PER_SEC;
		ts->tv_sec += 1;
	}
}

#define safe_div(a, b) __safe_div(a, b, __LINE__)

static unsigned long estimate_wait_usecs(struct account *a) {
	return max((unsigned long)safe_div(-count(a) * 1000000, a->rate), 100ul);
}

void inode_set_pos(struct inode *inode, long pos) {
	inode->i_private1 = pos;
}

long inode_get_pos(struct inode *inode) {
	return inode->i_private1;
}

void causes_set_cost(struct cause_list* causes, long cost) {
	causes->private = cost;
}

long causes_get_cost(struct cause_list* causes) {
	return causes->private;
}

static void print_account(struct account *account) {
	int i;
	printk(KERN_INFO "account {id=%d, rate=%ld, limit=%ld, count=%ld,\n"
		   "credit=%ld, lost=%ld, estimate=%ld,\n"
		   "used=%ld, used_raw=%ld, sane=%ld}\n",
		   account->id, account->rate, account->limit, count(account),
		   account->credit, account->lost, account->estimate,
		   account->used, account->used_raw, account->sane);
	for (i = 0; i<INSANE_CAP; i++) {
		if (account->insane[i])
			printk(KERN_INFO "  Cause errors from line %d: %ld\n", i, account->insane[i]);

	}
}

static void print_accounts(struct tbucket_data* td) {
	struct account *cur;
	printk(KERN_INFO "accounts:");
 	list_for_each_entry(cur, &td->accounts, accounts) {
		print_account(cur);
	}
	printk(KERN_INFO "queue_runs: %ld", td->stats.queue_runs);
	printk(KERN_INFO "Avg Run Len: %lld", 
		   safe_div(td->stats.disk_reads+td->stats.disk_writes,
					max(td->stats.queue_runs,1l)));
	printk(KERN_INFO "disk_norm: %ld", td->stats.disk_norm);
	printk(KERN_INFO "io_batches: %ld", td->stats.io_batches);
	printk(KERN_INFO "io_batch_add_seq: %ld",
		   td->stats.io_batch_add_seq);
	printk(KERN_INFO "io_batch_add_tot: %ld",
		   td->stats.io_batch_add_tot);
	printk(KERN_INFO "Avg Batch Size: %lld", 
		   safe_div(td->stats.disk_reads+td->stats.disk_writes,
					max(td->stats.io_batches,1l)));
	printk(KERN_INFO "disk_reads: %ld", td->stats.disk_reads);
	printk(KERN_INFO "disk_writes: %ld", td->stats.disk_writes);
	printk(KERN_INFO "Disk I/O: %ld", 
		   td->stats.disk_reads+td->stats.disk_writes);
	printk(KERN_INFO "dispatch_declined_throttle: %ld", 
		   td->stats.dispatch_declined_throttle);
	printk(KERN_INFO "dispatch_declined_no_work: %ld", 
		   td->stats.dispatch_declined_no_work);
	printk(KERN_INFO "sleep_time: %ld.%06ld", 
		   td->stats.sleep_time.tv_sec,
		   td->stats.sleep_time.tv_nsec);
}

// elv lock must be held!
static void free_accounts(struct tbucket_data* td) {
	struct account *cur, *tmp;

	print_accounts(td);

 	list_for_each_entry_safe(cur, tmp, &td->accounts, accounts) {
		list_del(&cur->accounts);
		hash_del(&cur->node);
		iobatch_free(cur->batch);
		kfree(cur);
	}
}

static struct cause_list *bio_uniq_causes(struct request *rq, struct bio *bio) {
	if (bio->cll && bio->cll->uniq_causes)
		return bio->cll->uniq_causes;
	return RQ_CAUSES(rq);
}

// returns an account id
static int get_one_cause(struct request *rq) {
	struct cause_list* causes;
	struct io_cause *cause;
	struct bio *bio;
	int rv = DEFAULT_ACCOUNT;

	bio = rq->bio;
	while (bio != NULL) {
		causes = bio_uniq_causes(rq, bio);
		if (causes) {
			list_for_each_entry(cause, &causes->items, list) {
				if (rv != DEFAULT_ACCOUNT && rv != cause->account_id)
					return DEFAULT_ACCOUNT; // multiple causes
				rv = cause->account_id;
			}
		}
		bio = bio->bi_next;
	}

	return rv;
}

static void saturate_counts(struct account* a) {
	long diff = count(a) - a->limit;
	if (diff > 0) {
		a->credit -= diff;
		a->lost += diff;
	}
	WARN_ON(count(a) > a->limit);
}

static void refresh_count(struct account* a) {
	// snippets borrowed from SCS
	struct timespec now;
	struct timespec update;
	unsigned long usecs;
	long inc;
	getnstimeofday(&now);
	update = timespec_sub(now, a->last_update);
	usecs = (update.tv_sec * USEC_PER_SEC +
			 update.tv_nsec / NSEC_PER_USEC);
	inc = (usecs * a->rate) / 1000000;
	BUG_ON(inc < 0);
	if (inc > 0) {
		a->credit += inc;
		saturate_counts(a);
		a->last_update = now;
	}
}

static void tbucket_merge_request(struct request_queue *q,
								  struct request *rq,
								  struct request *next) {
	queue_del(RQ_QUEUE(rq), rq);

	// causes are per bio, and we're merging requests, so
	// causes should automatically get moved with no extra
	// effort here.
}

static void do_disk_accounting(struct tbucket_data *td,
							   struct cause_list* causes,
							   off_t offset) {
	int size = causes->size;
	size_t combined_cost, summed_cost;    // cumulative costs
	size_t basic_charge, adjusted_charge; // individual costs
	struct account *account;
	struct io_cause *cause;
	int ret;
	int full = 0;
	int causes_cost = causes_get_cost(causes);

	// estimate cost if we didn't earlier in dirty hook
	if (causes_cost <= 0){
		WARN_ON(1);
	}

	// update global batch and account batches
	ret = iobatch_add_request(td->batch, offset, size);
	full |= iobatch_full(td->batch);
	if (ret)
		td->stats.io_batch_add_seq++;
	td->stats.io_batch_add_tot++;

	list_for_each_entry(cause, &causes->items, list) {
		account = get_account(td, cause->account_id);
		iobatch_add_request(account->batch, offset, size);
		full |= iobatch_full(account->batch);
		// amount we have already charged for req being added to batch
		account->batch_estimate += safe_div(causes_cost, causes->item_count);
	}

	if (full) {
		combined_cost = iobatch_get_cost(td->batch);
		iobatch_clear(td->batch);
		td->stats.disk_norm += combined_cost;
		td->stats.io_batches++;
		
		summed_cost = 0;
		list_for_each_entry(account, &td->accounts, accounts) {
			summed_cost += iobatch_get_cost(account->batch);
		}

		list_for_each_entry(account, &td->accounts, accounts) {
			basic_charge = iobatch_get_cost(account->batch);
			adjusted_charge = safe_div(combined_cost * basic_charge, summed_cost);

			// charge based on disk model, refund estimate
			account->used += adjusted_charge;
			account->used_raw += account->batch_estimate;
			account->estimate -= account->batch_estimate;
			saturate_counts(account);
			account->batch_estimate = 0;
			iobatch_clear(account->batch);
		}
	}
}

static void account_for_causes(struct tbucket_data *td,
							   struct cause_list* causes,
							   off_t offset, size_t size) {
	int amt;
	struct io_cause *cause;
	struct account *account;

	if (causes->callback_q) {
		// we will have already accounted for this in
		// tbucket_causes_dirty().  Disable free() callback,
		// as we already have the I/O at the disk level,
		// so we don't want to issue a refund when causes
		// is freed.
		causes->callback_q = NULL;
	} else if (causes->item_count > 0) {
		causes_set_cost(causes, size);
		amt = safe_div(size, causes->item_count);
		list_for_each_entry(cause, &causes->items, list) {
			account = get_account(td, cause->account_id);
			account->estimate += amt;
			saturate_counts(account);
		}
	}else{
		WARN_ON(1);
	}
	// at this point, we have charged for causes by assuming
	// sequentiality.  We have done so either in
	// (a) the dirty hook
	// OR
	// (b) in this function
	//
	// disk accounting may revise this charge
	do_disk_accounting(td, causes, offset);

	// TODO: why do we pass size to this function,
	// but not to do_disk_accounting? 
}

static void account_for_bio(struct tbucket_data *td,
							struct request *rq,
							struct bio *bio) {
	struct cause_list* causes;
	off_t offset = bio->bi_iter.bi_sector * (off_t)512;
	int i;

	if (bio->cll && bio->cll->uniq_causes->item_count) {
		for (i=0; i < bio->cll->item_count; i++) {
			causes = bio->cll->items[i];
			account_for_causes(td, causes, offset, causes->size);
			offset += causes->size;
		}
	} else if (RQ_CAUSES(rq)) {
		account_for_causes(td, RQ_CAUSES_CAST(rq), offset, bio->bi_iter.bi_size);
	} else {
		WARN_ON("nobody charged!");
	}

	// stats
	if (rq_data_dir(rq) == WRITE)
		td->stats.disk_writes += bio->bi_iter.bi_size;
	else
		td->stats.disk_reads += bio->bi_iter.bi_size;
}

static void account_for_req(struct tbucket_data *td,
							struct request *rq) {
	struct bio *bio = rq->bio;
	WARN_ON(bio == NULL);
	while (bio != NULL) {
		account_for_bio(td, rq, bio);
		bio = bio->bi_next;
	}
}

static void dispatch_nudge(unsigned long arg) {
	struct request_queue *q = (struct request_queue *)arg;
	spin_lock_irq(q->queue_lock);
	__blk_run_queue(q);
	spin_unlock_irq(q->queue_lock);
}

static void dispatch_sort(struct request_queue *q,
						  struct queue *queue,
						  struct request *rq) {
	struct tbucket_data *td = q->elevator->elevator_data;

	trace_queue("dispatch_sort", rq);

	account_for_req(td, rq);
	td->run_serve_size += blk_rq_bytes(rq);
	elv_dispatch_sort(q, rq);
	td->dispatched += 1;

	// stats
	if (queue != td->stats.last_queue) {
		td->stats.queue_runs++;
		td->stats.last_queue = queue;
	}
	if (td->stats.start_sleep.tv_sec != 0) {
		struct timespec now;
		getnstimeofday(&now);
		td->stats.sleep_time.tv_sec +=
			(now.tv_sec - td->stats.start_sleep.tv_sec);
		td->stats.sleep_time.tv_nsec +=
			(now.tv_nsec - td->stats.start_sleep.tv_nsec);
		normalize_timespec(&td->stats.sleep_time);
		td->stats.start_sleep.tv_sec = 0;
	}
}

static int tbucket_dispatch_force(struct request_queue *q) {
	struct tbucket_data *td = q->elevator->elevator_data;
	struct queue *queue;
	struct request *rq;
	int rv = 0;
 	list_for_each_entry(queue, &td->queues, queues) {
		while ((rq = queue_pop(queue)) != NULL) {
			dispatch_sort(q, queue, rq);
			rv++;
		}
	}
	return rv;
}

static int tbucket_dispatch(struct request_queue *q, int force) {
	int i;
	struct tbucket_data *td = q->elevator->elevator_data;
	struct request *rq;
	struct account *account;
	struct queue *queue;
	unsigned long usecs_to_wait = 0;
	unsigned long min_usecs_to_wait = 0;
	unsigned long jiffy_wait = 0;
	unsigned long pop_elapsed_us = 0;
	int have_switched = 0;

	if (force)
		return tbucket_dispatch_force(q);

	if (DISPATCH_CAP > 0 && td->dispatched >= DISPATCH_CAP)
		return 0;

	for (i=0; i<td->queues_count; i++) {
		queue = container_of(td->queues.next, struct queue, queues);
		account = queue_to_account(queue);
		refresh_count(account);
		if(td->run_serve_size >= RUN_SIZE_LIMIT)
			goto switch_queue;
		if (queue->rw || account->rate <= 0 || count(account) > 0) {
			rq = queue_pop(queue);
			if (rq) {
				dispatch_sort(q, queue, rq);
				return 1;
			} else if(have_switched == 0){
				// anticipation.  we want to run this queue, but it is empty
				pop_elapsed_us = queue_pop_elapsed_us(queue);
				if (pop_elapsed_us < ANTICIPATION_US) {
					min_usecs_to_wait = ANTICIPATION_US - pop_elapsed_us;
					//					printk(KERN_INFO "anticipate for %ld", min_usecs_to_wait);
					break;
				} else {
					//					printk(KERN_INFO "don't anticipate, switch away from %d, %d",
					//						   account->id, queue->rw);
				}
			}
		}

		// how long before this will be ready?
		if (queue->count > 0) {
			usecs_to_wait = estimate_wait_usecs(account);
			if (min_usecs_to_wait == 0 || usecs_to_wait < min_usecs_to_wait)
				min_usecs_to_wait = usecs_to_wait;
		}
switch_queue:

		// move queue to end of list of queues
		have_switched = 1;
		td->run_serve_size = 0;
		list_del_init(&queue->queues);
		list_add_tail(&queue->queues, &td->queues);
	}

	if (min_usecs_to_wait > 0) {
		jiffy_wait = jiffies;
		jiffy_wait += usecs_to_jiffies(max(min_usecs_to_wait, 100ul));
		del_timer(&td->dispatch_nudger);
		mod_timer(&td->dispatch_nudger, jiffy_wait);
		td->stats.dispatch_declined_throttle++;
	} else {
		td->stats.dispatch_declined_no_work++;
	}
	td->stats.last_queue = NULL;
	if (td->stats.start_sleep.tv_sec == 0)
		getnstimeofday(&td->stats.start_sleep);

	return 0;
}

/* typedef int (elevator_set_req_fn) (struct request_queue *, struct request *,
 *				   struct bio *, gfp_t);
 * TODO: That the struct bio *bio is passed in here now, probably means we need
 *   to do something with it.. */
static int tbucket_set_request(struct request_queue *q, struct request *rq,
					struct bio *bio, gfp_t gfp_mask) {
	RQ_CAUSES(rq) = new_cause_list();
	return 0;
}

static void tbucket_put_request(struct request *rq) {
	put_cause_list_safe((struct cause_list *)RQ_CAUSES(rq));
}

static int request_sanity_check(struct request *rq) {
	struct cause_list *causes;
	struct bio *bio = rq->bio;
	int warn = 0;
#define WARN_ON2(x) WARN_ON(x); if(x) warn = __LINE__
	WARN_ON2(bio == NULL);
	while (bio != NULL) {
		// (cll set) iff (req is a write)
		WARN_ON2((bio->cll != NULL) && (rq_data_dir(rq) == READ));
		//WARN_ON2((bio->cll == NULL) && (rq_data_dir(rq) == WRITE));
		if (bio->cll && rq_data_dir(rq) == WRITE) {
			int cl_count = 0;
			int cl_bytes = 0;
			int i;
			for (i=0; i < bio->cll->item_count; i++) {
				causes = bio->cll->items[i];
				if (causes->type != SPLIT_ZERO) {
					WARN_ON2(causes->size != PG_SIZE);
					// sometimes transactions are commited without any I/O
					// for processes, so journal cause_list's may be empty
					if (causes->type != SPLIT_JOURNAL)
						WARN_ON2(causes->item_count == 0);
				}
				cl_count++;
				cl_bytes += causes->size;
			}
			WARN_ON2(bio->cll->size != bio->bi_iter.bi_size);
			if(bio->cll->size != bio->bi_iter.bi_size) {
				printk(KERN_INFO "mismatch %ld != %d (%d)",
					   bio->cll->size, bio->bi_iter.bi_size,
					   bio->cll->item_count);
			}
			WARN_ON2(cl_count != bio->cll->item_count);
			WARN_ON2(cl_bytes != bio->cll->size);
		}
		bio = bio->bi_next;
	}
	return warn;
}

static void tbucket_add_request(struct request_queue *q,
								struct request *rq) {
	struct tbucket_data *td = q->elevator->elevator_data;
	struct account *account;
	int warn = request_sanity_check(rq);

	__cause_list_add(RQ_CAUSES_CAST(rq),
					 atomic_read(&current->account_id));
	account = get_account(td, get_one_cause(rq));
	if (warn == 0)
		account->sane++;
	else {
		account->insane[warn >= INSANE_CAP ? 0 : warn]++;
	}
	RQ_QUEUE(rq) = rq_data_dir(rq) ? &account->wqueue : &account->rqueue;
	queue_push(RQ_QUEUE(rq), rq);

	trace_queue("add_request", rq);
}

static struct request *
tbucket_former_request(struct request_queue *q, struct request *rq) {
	return NULL;
}

static struct request *
tbucket_latter_request(struct request_queue *q, struct request *rq) {
	return NULL;
}

static void tbucket_completed_request(struct request_queue *q,
									  struct request *rq) {
	struct tbucket_data *td = q->elevator->elevator_data;
	td->dispatched -= 1;
}

static void tbucket_kv_set(struct request_queue *q,
						   int id, char* key, long val) {
	struct tbucket_data *td = q->elevator->elevator_data;
	struct account *account;

	printk(KERN_INFO "set,%d,%s,%ld", id, key, val);

	spin_lock_irq(q->queue_lock);
	if (strcmp(key, "debug") == 0) {
		print_accounts(td);
	} else {
		if (id) {
			account = get_account(td, id);
			if (account->id == id) {
				if (strcmp(key, "rate") == 0) {
					account->rate = val;
					saturate_counts(account);
				} else if (strcmp(key, "limit") == 0) {
					account->limit = val;
					saturate_counts(account);
				} else if (strcmp(key, "count") == 0) {
					account->credit += val - count(account);
					saturate_counts(account);
				}
			}
		}
	}
	spin_unlock_irq(q->queue_lock);
}

/* typedef int (elevator_init_fn) (struct request_queue *,
 * 				struct elevator_type *e);
 * See also: block/cfq-iosched.c */
static int tbucket_init_queue(struct request_queue *q, struct elevator_type *e)
{
	struct elevator_queue *eq;
	struct tbucket_data *td = NULL;
	struct account *default_account = NULL;
	struct io_batch *batch = NULL;

	// allocs
	eq = elevator_alloc(q, e);
	if (!eq)
		return -ENOMEM;

	td = kmalloc_node(sizeof(*td), GFP_KERNEL, q->node);
	default_account = new_account(DEFAULT_ACCOUNT);
	batch = iobatch_new(BATCH_SIZE, BATCH_BYTE_CAP);

	// error handling
	if (!td || !default_account || !batch) {
		kfree(td);
		kfree(default_account);
		kfree(batch);
		kobject_put(&eq->kobj);
		return -ENOMEM;
	}

	// init td
	memset(td, 0, sizeof(*td));
	td->q = q;
	hash_init(td->accounts_hash);
	INIT_LIST_HEAD(&td->accounts);
	INIT_LIST_HEAD(&td->queues);
	td->batch = batch;

	eq->elevator_data = td;

	add_account(td, default_account);

	spin_lock_irq(q->queue_lock);
	q->elevator = eq;
	spin_unlock_irq(q->queue_lock);

	setup_timer(&td->dispatch_nudger, dispatch_nudge, (unsigned long) q);
	ioctl_helper_register_disk(q, tbucket_kv_set);

	return 0;
}

static void tbucket_exit_queue(struct elevator_queue *e) {
	struct tbucket_data *td = e->elevator_data;

	ioctl_helper_unregister_disk(td->q);
	free_accounts(td);

	del_timer(&td->dispatch_nudger);
	iobatch_free(td->batch);
	kfree(td);
	e->elevator_data = NULL;
}

// if we're short on tokens, wait until we're likely to have more
static int maybe_sleep(struct request_queue *q, int sched_uniq) {
	struct tbucket_data *td;
	unsigned long usecs_to_wait;
	struct timespec ts;
	int sleepy = 0;
	struct account *account;
	int account_id;

	getnstimeofday(&ts);

	spin_lock_irq(q->queue_lock);
	if (q->sched_uniq != sched_uniq) {
		// scheduler changed!
		spin_unlock_irq(q->queue_lock);
		return 0;
	}

	td = q->elevator->elevator_data;
	if (td) {
		account_id = atomic_read(&current->account_id);
		account = get_account(td, account_id);
		refresh_count(account);
		if (count(account) < 0 && account->rate > 0) {
			sleepy = 1;
			usecs_to_wait = estimate_wait_usecs(account);
			usecs_to_wait = min(usecs_to_wait, 5*1000000ul); // 5s cap
			timespec_add_ns(&ts, (u64)usecs_to_wait*NSEC_PER_USEC);
		}
	}
	spin_unlock_irq(q->queue_lock);

	if (sleepy)
		hrtimer_nanosleep(&ts, NULL, HRTIMER_MODE_ABS, CLOCK_REALTIME);

	return sleepy;
}

ssize_t tbucket_write_entry (struct request_queue *q,
							 struct file* filp,
							 size_t count,
							 loff_t *pos,
							 void** opaque,
							 int sched_uniq) {
	while (maybe_sleep(q, sched_uniq))
		;

	return 0;
}

int tbucket_fsync_entry (struct request_queue *q,
						 struct file* filp,
						 int datasync,
						 void** opaque,
						 int sched_uniq) {
	return 0;
}
										 
void tbucket_write_return (struct request_queue *q,
						   void *opaque,
						   ssize_t rv,
						   int sched_uniq) {

}

void tbucket_fsync_return (struct request_queue *q,
						   void *opaque,
						   int rv,
						   int sched_uniq) {

}

void tbucket_causes_dirty (struct request_queue *q,
						   struct cause_list* causes,
						   struct task_struct* new,
						   struct inode *inode,
						   long pos) {
	struct tbucket_data *td = q->elevator->elevator_data;
	int new_account_id = atomic_read(&new->account_id);
	struct io_cause *cause;
	struct account *account;
	int charge;
	long tot_cost = causes_get_cost(causes);

	// estimate cost using crude model of randomness
	if (tot_cost <= 0) {
		tot_cost = seek_time(pos, inode_get_pos(inode)) + causes->size;
		causes_set_cost(causes, tot_cost);
	}
	inode_set_pos(inode, pos+causes->size);

	list_for_each_entry(cause, &causes->items, list) {
		account = get_account(td, cause->account_id);
		charge = safe_div(tot_cost, causes->item_count);
		if (account->id != new_account_id)
			charge -= safe_div(tot_cost, (causes->item_count - 1));
		account->estimate += charge;
		saturate_counts(account);
	}
}

void tbucket_causes_free (struct request_queue *q,
						  struct cause_list* causes) {
	struct tbucket_data *td = q->elevator->elevator_data;
	struct io_cause *cause;
	struct account *account;
	int refund;

	list_for_each_entry(cause, &causes->items, list) {
		account = get_account(td, cause->account_id);
		refund = safe_div(causes->size, causes->item_count);
		account->estimate -= refund;
		saturate_counts(account);
	}
}

int tbucket_create_entry (struct request_queue* q,
						   struct inode *dir,
						   struct dentry *dentry, 
						   int mode,
						   void **opaque,
						   int sched_uniq) {
	while (maybe_sleep(q, sched_uniq))
		;

	return 0;
}

void tbucket_create_return (struct request_queue *rq,
							void *opaque,
							int rv,
							int sched_uniq) {

}

static struct elevator_type elevator_tbucket = {
	.ops = {
		.elevator_merge_req_fn      = tbucket_merge_request,
		.elevator_dispatch_fn       = tbucket_dispatch,
		.elevator_set_req_fn        = tbucket_set_request,
		.elevator_put_req_fn        = tbucket_put_request,
		.elevator_add_req_fn        = tbucket_add_request,
		.elevator_former_req_fn     = tbucket_former_request,
		.elevator_latter_req_fn     = tbucket_latter_request,
		.elevator_completed_req_fn  = tbucket_completed_request,
		.elevator_init_fn           = tbucket_init_queue,
		.elevator_exit_fn           = tbucket_exit_queue,
		.elevator_write_entry_fn    = tbucket_write_entry,
		.elevator_fsync_entry_fn    = tbucket_fsync_entry,
		.elevator_create_entry_fn   = tbucket_create_entry,
		.elevator_write_return_fn   = tbucket_write_return,
		.elevator_fsync_return_fn   = tbucket_fsync_return,
		.elevator_create_return_fn  = tbucket_create_return,
		.elevator_causes_dirty_fn   = tbucket_causes_dirty,
		.elevator_causes_free_fn    = tbucket_causes_free
	},
	.elevator_name = "tb",
	.elevator_owner = THIS_MODULE,
};

static int __init tbucket_init(void) {
	int rv;
	elv_register(&elevator_tbucket);
	rv = ioctl_helper_init(IOCTL_MAJOR);
	BUG_ON(rv);
	cause_list_debug();
	return 0;
}

static void __exit tbucket_exit(void) {
	ioctl_helper_cleanup();
	elv_unregister(&elevator_tbucket);
	cause_list_debug();
}

module_init(tbucket_init);
module_exit(tbucket_exit);

MODULE_LICENSE("GPL");
