/*
 * afq scheduler: fsync only
 */
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/time.h>
#include <linux/list.h>
#include <linux/completion.h>
#include <linux/ioprio.h>
#include <linux/slab.h>
#include <linux/iocontext.h>
#include <linux/ioprio.h>
#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/module.h>
#include <linux/hashtable.h>
#include <linux/cause_tags.h>
#include <linux/delay.h>
#include <linux/kthread.h>

#include "split_sched.h"
#include "split_account.h"
#include "io_batch.h"
#include "ioctl_helper.h"


//Locking rules
//queue_lock -> (afq_data->lock)

#define MAX_CAUSE_COUNT 32

#define WRITE_ALLOWANCE 204800

#define TOTAL_SHARE 840 //LGM of 1, 2, 3, 4, 5, 6, 7, 8
#define MAX_VIO_COUNTER_THRESHOLD  LONG_MAX/4
#define ACCT_MAX_SLEEP_TIME (300*HZ)

#define ALLOW_NEW_BATCH 0
#define NO_NEW_BATCH 1
#define KEEP_IN_FLIGHT 2
#define START_IN_FLIGHT_TIME HZ/5

#define AFQ_BATCH_DEAD_INTVAL (HZ/100) //20ms

#define MIN_IN_FLIGHT 2

#define AFQ_SCLICE_IDLE (HZ/50) //20ms
#define AFQ_MAX_SLICE  (HZ/4) //200ms



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
#define safe_div(a, b) __safe_div(a, b, __LINE__)

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


struct afq_data{
	struct request_queue *queue;
	struct acct_hash acct_hash;
	struct list_head active_acct_list;
	struct account *min_acct;
	long global_min_vio_counter;
	long global_min_vio_counter2;
	struct account *last_served_acct;
	spinlock_t lock;
	int keep_wait;
	int batch_dead;
	atomic_t inflight;
	struct io_batch *sim_batch;
	struct timer_list batch_wait_timer;
	struct timer_list batch_dead_timer;
	struct task_struct *sched_thread;
	struct task_struct *min_thread;
	/*
	 * yangsuli added tracing facility
	 */
	int disk_full_count;
	int prio_served_rqs[IOPRIO_BE_NR];
	int caust_count_dist[MAX_CAUSE_COUNT];
	int journal_cct[MAX_CAUSE_COUNT];
};

void update_active_min(struct afq_data* afq_data);

/*
inline int prio_to_stride(int prio){
	switch(prio){
		case 8:
			return 128;
		case 7:
			return 64;
		case 6:
			return 32;
		case 5:
			return 16;
		case 4:
			return 8;
		case 3:
			return 4;
		case 2:
			return 2;
		case 1:
			return 1;
	}
	return -1;
}
*/

inline int prio_to_stride(int prio){
	return prio;
}

/*
 * context: called with queue_lock held
 */
static void add_vio_counter(struct afq_data *afq_data, struct account *acct, int quantity){
	int stride;
	int bkt;
	struct account *cur_acct;
	
	//stride = acct->ioprio;
	stride = prio_to_stride(acct->ioprio);

	if(stride == 0)
		stride = 1;

	acct->vio_counter += TOTAL_SHARE / stride;

	if(acct == afq_data->min_acct || afq_data->min_acct == NULL){
		update_active_min(afq_data);
	}

	if(acct->vio_counter > MAX_VIO_COUNTER_THRESHOLD){
		acct_hash_for_each(&afq_data->acct_hash, bkt, cur_acct, node){
			cur_acct->vio_counter = 0;
			cur_acct->vio_reset++;
		}
		afq_data->global_min_vio_counter = 0;
		afq_data->global_min_vio_counter2 = 0;
	}
}

static void add_estimate_vio(struct afq_data *afq_data, struct account *acct, int quantity){
	acct->estimate += quantity;
	add_vio_counter(afq_data, acct, quantity);
}

static void new_batch_wait_timer_func(unsigned long arg){
	struct afq_data *afq_data = (struct afq_data *)arg;
	unsigned long flags;
	spin_lock_irqsave(&afq_data->lock, flags);
	if(afq_data->keep_wait == NO_NEW_BATCH){
		afq_data->keep_wait = KEEP_IN_FLIGHT;
	}
	spin_unlock_irqrestore(&afq_data->lock, flags);
}

static void batch_dead_timer_func(unsigned long arg){
	struct afq_data *afq_data = (struct afq_data *)arg;
	unsigned long flags;
	spin_lock_irqsave(&afq_data->lock, flags);
	afq_data->batch_dead = 0;
	spin_unlock_irqrestore(&afq_data->lock, flags);
}

/*
 * context: this function will *always* be called while queue_lock held
 * so we don't have to worry about concurrency control inside this function
 * It is called just before we issue the request to the disk
 * So last_end_request time is also updated here...
 */
#define SEQ_THRESHOLD 12
#define SEQ_DIST 1024

inline void afq_accout_pos_seq_update(struct afq_data* afq_data, struct account *acct, struct request *rq){
	int idx = (IOPRIO_BE_NR - acct->ioprio) % IOPRIO_BE_NR;

	/*
	 * already accounted for this rq for this acct
	 */
	if(acct->last_sector == blk_rq_pos(rq)){
		return;
	}

	afq_data->prio_served_rqs[idx]++;

	if(abs(acct->last_sector - blk_rq_pos(rq)) < SEQ_DIST){
		if(acct->seq_counter < SEQ_THRESHOLD){
			acct->seq_counter++;
		}
	}else{
		if(acct->seq_counter > 0){
			acct->seq_counter -= 2;
		}
	}

	acct->last_sector = blk_rq_pos(rq);
	acct->last_end_request = jiffies;
}
					
static struct cause_list *bio_uniq_causes(struct bio *bio){
	if(bio->cll && bio->cll->uniq_causes &&
			bio->cll->uniq_causes->item_count)
		return bio->cll->uniq_causes;
	return NULL;
}



static void do_disk_accounting(struct afq_data *afq_data,
							   struct cause_list* causes,
							   off_t offset) {
	int size = causes->size;
	size_t combined_cost, summed_cost;    // cumulative costs
	size_t basic_charge, adjusted_charge; // individual costs
	struct account *account;
	struct io_cause *cause;
	int ret;
	int full = 0;
	int bkt;
	int causes_cost = causes_get_cost(causes);

	if (causes_cost <= 0)
		WARN_ON(1);

	// update global sim_batch and account batches
	ret = iobatch_add_request(afq_data->sim_batch, offset, size);
	full |= iobatch_full(afq_data->sim_batch);

	list_for_each_entry(cause, &causes->items, list){
		account = get_account(&afq_data->acct_hash, cause->account_id);
		iobatch_add_request(account->sim_batch, offset, size);
		full |= iobatch_full(account->sim_batch);
		// amount we have already charged for req being added to batch
		account->batch_estimate += safe_div(causes_cost, causes->item_count);
	}

	if (full){
		afq_data->disk_full_count++;
		combined_cost = iobatch_get_cost(afq_data->sim_batch);
		iobatch_clear(afq_data->sim_batch);

		summed_cost = 0;
		acct_hash_for_each(&afq_data->acct_hash, bkt, account, node){
			summed_cost += iobatch_get_cost(account->sim_batch);
		}
		
		acct_hash_for_each(&afq_data->acct_hash, bkt, account, node){
			basic_charge = iobatch_get_cost(account->sim_batch);
			adjusted_charge = safe_div(combined_cost * basic_charge, summed_cost);
			if(adjusted_charge){
				add_vio_counter(afq_data, account, adjusted_charge);
				//printk("for account prio %d add adjusted_charge %d now vio_counter %ld\n", account->ioprio, (int)adjusted_charge, account->vio_counter);
			}
			//refund estimate
			add_estimate_vio(afq_data, account, -account->batch_estimate);
			account->batch_estimate = 0;

			iobatch_clear(account->sim_batch);
		}
	}
}

static void account_for_causes(struct afq_data *afq_data, struct cause_list *causes, off_t offset, size_t size){
	int amt;
	struct io_cause *cause;
	struct account *account;
	
	if(causes->callback_q){
		// we will have already accounted for this in
		// tbucket_causes_dirty().  Disable free() callback,
		// as we already have the I/O at the disk level,
		// so we don't want to issue a refund when causes
		// is freed.
		causes->callback_q = NULL;
	}else if(causes->item_count > 0){
		causes_set_cost(causes, size);
		amt = safe_div(size, causes->item_count);
		list_for_each_entry(cause, &causes->items, list){
			account = get_account(&afq_data->acct_hash, cause->account_id);
			add_estimate_vio(afq_data, account, amt);
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
	do_disk_accounting(afq_data, causes, offset);
}




static void account_for_bio(struct afq_data *afq_data, struct request *rq, struct bio *bio){
	struct cause_list *causes;
	struct io_cause *cause;
	struct account *account;
	off_t offset = bio->bi_iter.bi_sector * (off_t)512;
	int i;

	if(bio->cll && bio->cll->uniq_causes->item_count){
		for(i=0; i < bio->cll->item_count; i++){
			causes = bio->cll->items[i];
			account_for_causes(afq_data, causes, offset, causes->size);
			offset += causes->size;
			list_for_each_entry(cause, &causes->items, list){
				account = get_account(&afq_data->acct_hash, cause->account_id);
				afq_accout_pos_seq_update(afq_data, account, rq);
			}
		}
	}else if (RQ_CAUSES(rq)){
		causes = RQ_CAUSES_CAST(rq);
		account_for_causes(afq_data, RQ_CAUSES_CAST(rq), offset, bio->bi_iter.bi_size);
		list_for_each_entry(cause, &causes->items, list){
			account = get_account(&afq_data->acct_hash, cause->account_id);
			afq_accout_pos_seq_update(afq_data, account, rq);
		}
	}else{
		WARN_ON("nobody charged!");
	}
}


void afq_account_request2(struct afq_data *afq_data, struct req_desc *req_desc){
	struct request *rq = req_desc->rq;
	struct bio *bio = rq->bio;
	
	WARN_ON(bio == NULL);
	while(bio != NULL){
		account_for_bio(afq_data, rq, bio);
		bio = bio->bi_next;
	}

}

void afq_account_request(struct afq_data *afq_data, struct req_desc *req_desc){
	struct request *rq = req_desc->rq;
	struct io_cause *cause;
	struct cause_list *causes;
	struct account *acct;
	int account_id;

	if(rq->cmd_flags & REQ_WRITE){
		struct bio *bio = rq->bio;
		WARN_ON(bio == NULL);
		
		while(bio != NULL){
			causes = bio_uniq_causes(bio);
			if(causes){
				list_for_each_entry(cause, &causes->items, list){
					account_id = cause->account_id;
					acct = get_account(&afq_data->acct_hash, account_id);
					//printk("account %d prio %d vio_counter %d one write bio\n", acct->account_id, acct->ioprio, acct->vio_counter);
					add_vio_counter(afq_data, acct, 1);
					afq_accout_pos_seq_update(afq_data, acct, rq);
				
				}
			}else{
					account_id = get_account_id(current);
					acct = get_account(&afq_data->acct_hash, account_id);
					//printk("account %d prio %d vio_counter %d one direct write bio\n", acct->account_id, acct->ioprio, acct->vio_counter);
					add_vio_counter(afq_data, acct, 1);
					afq_accout_pos_seq_update(afq_data, acct, rq);
			}
			bio = bio->bi_next;
		}
	}else{
		acct = req_desc->acct;
		//printk("account %d prio %d vio_counter %d one read bio\n", acct->account_id, acct->ioprio, acct->vio_counter);
		add_vio_counter(afq_data, acct, 1);
		afq_accout_pos_seq_update(afq_data, acct, rq);
	}
}

static inline int acct_no_pending_io(struct account *acct){
	return list_empty(&acct->io_work_list_head);
}

/*
 * context: called with queue lock held
 */
static void add_io_work_to_acct(struct afq_data *afq_data, struct io_work *io_work, struct account *acct){
	//printk("add_io_work_to_acct id %d\n", acct->account_id);
	if(acct_no_pending_io(acct)){
		list_add_tail(&acct->list, &afq_data->active_acct_list);
		if(time_before(acct->last_end_request + ACCT_MAX_SLEEP_TIME, jiffies)){
			//acct->vio_counter = afq_data->global_min_vio_counter;
			acct->vio_counter = afq_data->global_min_vio_counter2;
		}
	}

	list_add_tail(&io_work->list, &acct->io_work_list_head);

}

//context: called with queue_lock held
static void dec_check_cleanup_batch(struct afq_data *afq_data, struct io_batch_desc *batch_desc){
	unsigned long flags;
	int batch_complete = 0;

	batch_desc->one_loop_work--;
	if(batch_desc->one_loop_work == 0){
		batch_complete = 1;
	}

	if(batch_complete){
		if(afq_data){
			spin_lock_irqsave(&afq_data->lock, flags);
			afq_data->keep_wait = ALLOW_NEW_BATCH;
			spin_unlock_irqrestore(&afq_data->lock, flags);

		//	__blk_run_queue(afq_data->queue);
		}

		kfree(batch_desc);
	}
}

static void cause_list_size_stat2(struct afq_data* afq_data, struct request *rq){
	if(rq->cmd_flags & REQ_WRITE){
		struct bio *bio = rq->bio;
		struct cause_list *causes;
		int i;
		while(bio != NULL){
			if(bio->cll){
				for(i = 0; i < bio->cll->item_count; i++){
					causes = bio->cll->items[i];
					if(causes && causes->type == SPLIT_CHECKPOINT){
						int count = causes->item_count;
						if(count >= MAX_CAUSE_COUNT){
							printk("cause count %d larger than MAX_CAUSE_COUNT!\n", count);
							continue;
						}
						afq_data->caust_count_dist[count]++;
					}
					if(causes && causes->type == SPLIT_JOURNAL_META){
						int count = causes->item_count;
						if(count >= MAX_CAUSE_COUNT){
							printk("cause count %d larger than MAX_CAUSE_COUNT!\n", count);
							continue;
						}
						afq_data->journal_cct[count]++;
					}
				}
			}
			bio = bio->bi_next;
		}
	}
}


static void basic_sched_add_request(struct request_queue *q, struct request *rq){
	struct req_desc *req_desc = rq->elv.priv[0];
	struct afq_data *afq_data = q->elevator->elevator_data;

	cause_list_size_stat2(afq_data, rq);

	__cause_list_add(RQ_CAUSES_CAST(rq),
					 get_account_id(current));

	if(rq->cmd_flags & REQ_WRITE){
		atomic_inc(&afq_data->inflight);
		//printk("incread inflight in basic_sched_add_request, now %d\n", atomic_read(&afq_data->inflight));
		//do accounting before dispatching instead of upon completion
		afq_account_request2(afq_data, req_desc);
		elv_dispatch_sort(q, rq);
		return;
	}

	req_desc->acct = get_account(&afq_data->acct_hash, get_account_id(current));
	if(req_desc->acct && req_desc->acct->account_id){
		add_io_work_to_acct(afq_data, &req_desc->io_work, req_desc->acct);
	}else{
		elv_dispatch_sort(q, rq);
	}

}

static void basic_sched_merged_request(struct request_queue *q, struct request *rq, struct request *next){
	BUG_ON(1);
}

static void basic_sched_complte_request(struct request_queue *q, struct request *rq){
	struct afq_data *afq_data = q->elevator->elevator_data;
	struct req_desc *req_desc = rq->elv.priv[0];

	atomic_dec(&afq_data->inflight);
	//printk("decrease inflight in basic_sched_complte_request, now %d\n", atomic_read(&afq_data->inflight));

	if(req_desc->owner_batch){
		dec_check_cleanup_batch(afq_data, req_desc->owner_batch);
	}
}
//FIXME:
//global_min_vio_counter needs more work...

/*
 * each time choose one account
 * and submit all pending io_work in that account
 * might not work well if every account has too little io (too small batch size)
 * and we have a lot of contexts (too much latency)

 * return value:
 * return 1 if we have chosen some work
 * return 0 if no work chosen
 * return -1 in case of error
 *
 * context: called while queue_lock held
 */
#define MAX_BATCH_SIZE 100
#define MIN_BATCH_SIZE 5
int basic_sched_select_io_work(struct request_queue *q, void** work_selected){
	struct io_work *io_work = NULL;
	struct io_work *io_next = NULL;
	struct account *acct;
	struct account *min_acct = NULL;
	long min_vio_counter = MAX_VIO_COUNTER_THRESHOLD * 2;
	struct io_batch_desc *io_batch_desc = NULL;
	struct afq_data *afq_data = q->elevator->elevator_data;
	int idle_for_last_acct = 0;
	int batch_size = 0;
	int more_io_work = 0;
	int pool_size = 0;

	if(list_empty(&afq_data->active_acct_list)){
		*work_selected = NULL;
		return 0;
	}

	if(afq_data->last_served_acct
		&& time_before(jiffies, afq_data->last_served_acct->last_end_request + AFQ_SCLICE_IDLE)
		&& time_before(jiffies, afq_data->last_served_acct->slice_end)
		&& afq_data->last_served_acct->seq_counter >= SEQ_THRESHOLD){
		idle_for_last_acct = 1;
	}

	if(idle_for_last_acct){
		if(acct_no_pending_io(afq_data->last_served_acct)){
			*work_selected = NULL;
			return 0;
		}
	}

	io_batch_desc = (struct io_batch_desc*)kmalloc(sizeof(struct io_batch_desc), GFP_ATOMIC);
	if(unlikely(io_batch_desc == NULL)){
		printk(KERN_ERR "unable to alloc io_batch_desc!\n");
		*work_selected = NULL;
		return 0;
	}

	io_batch_desc->one_loop_work = 0;
	INIT_LIST_HEAD(&io_batch_desc->work_selected);
		
	if(idle_for_last_acct){
		pool_size = 0;
		min_acct = afq_data->last_served_acct;
		//printk("continue last acct io...\n");
	}else{
		//printk("restart from min acct...\n");
add_io_work:
		pool_size = 0;
		list_for_each_entry(acct, &afq_data->active_acct_list, list){
			pool_size++;
			if(acct->vio_counter < min_vio_counter){
				min_vio_counter = acct->vio_counter;
				min_acct = acct;
			}
		}

		if(unlikely(min_acct == NULL)){
			if(more_io_work){
				goto choose_work_complete;
			}else{
				kfree(io_batch_desc);
				printk(KERN_ERR "choose_io_work returned -1 because min_acct is NULL\n");
				return -1;
			}
		}

		//Since we changed the account we are serving
		//properly set up the fields
		afq_data->last_served_acct = min_acct;
		afq_data->global_min_vio_counter = min_vio_counter;
		//FIXME:
		//DO this?
		min_acct->slice_begin = jiffies;
		min_acct->slice_end = jiffies + AFQ_MAX_SLICE;
	}

	if(acct_no_pending_io(min_acct)){
		kfree(io_batch_desc);
		printk(KERN_ERR "account %d has no pending io!\n", min_acct->account_id);
		return -1;
	}

	//printk("from %d pool choose %s account_id %d prio %d vio_counter %ld\n", pool_size, idle_for_last_acct?"old":"new", min_acct->account_id, min_acct->ioprio, min_acct->vio_counter);
	//printk("idle_for_last_acct %d global_min_vio_counter2 %d\n", idle_for_last_acct, afq_data->global_min_vio_counter2);

	list_for_each_entry_safe(io_work, io_next, &min_acct->io_work_list_head, list){
		list_move_tail(&io_work->list, &io_batch_desc->work_selected);
		batch_size++;
		if(batch_size >= MAX_BATCH_SIZE)
			break;
	}

	if(acct_no_pending_io(min_acct)){
		//printk("del account_id %d from active_account list\n", min_acct->account_id);
		list_del_init(&min_acct->list);
	}

	if(batch_size < MIN_BATCH_SIZE && min_acct != NULL && min_acct->seq_counter < SEQ_THRESHOLD){
		more_io_work = 1;
		min_vio_counter = MAX_VIO_COUNTER_THRESHOLD * 2;
		min_acct = NULL;
		goto add_io_work;
	}

choose_work_complete:
	*work_selected = io_batch_desc;
	return 1;
}

void basic_sched_submit_io_work(struct afq_data *afq_data, void *work_desc){
	struct list_head *io_pos, *io_next;
	struct fsync_desc *to_serve_fsync_desc;
	struct wrdesc *to_serve_wrdesc;
	struct req_desc *to_serve_req_desc;
	struct mkdir_desc *to_serve_mkdir_desc;
	struct io_batch_desc *io_batch_desc = (struct io_batch_desc*)work_desc;
	struct io_work *io_work;
	unsigned long flags;
	int fsync_num = 0;
	int read_req_num = 0;
	int write_num = 0;
	int mkdir_num = 0;

	list_for_each_safe(io_pos, io_next, &io_batch_desc->work_selected){
		io_work = list_entry(io_pos, struct io_work, list);
		switch(io_work->type){
			case SCHED_SYS_FSYNC:
				{
					to_serve_fsync_desc = FSYNC_IO(io_work);
					list_del_init(&io_work->list);
					to_serve_fsync_desc->owner_batch = io_batch_desc;
					io_batch_desc->one_loop_work++;
					fsync_num++;
					complete(&to_serve_fsync_desc->f_completion);
				}
				break;
			case SCHED_SYS_WRITE:
				{
					to_serve_wrdesc = WR_IO(io_work);
					list_del_init(&io_work->list);
					to_serve_wrdesc->owner_batch = io_batch_desc;
					io_batch_desc->one_loop_work++;
					write_num++;
					complete(&to_serve_wrdesc->w_completion);
				}
				break;
			case SCHED_SYS_MKDIR:
				{
					to_serve_mkdir_desc = MKDIR_IO(io_work);
					list_del_init(&io_work->list);
					to_serve_mkdir_desc->owner_batch = io_batch_desc;
					io_batch_desc->one_loop_work++;
					mkdir_num++;
					complete(&to_serve_mkdir_desc->completion);
				}
				break;
			case SCHED_READ_REQ:
				{
					to_serve_req_desc = REQ_IO(io_work);
					to_serve_req_desc->owner_batch = io_batch_desc;
					atomic_inc(&afq_data->inflight);
					//printk("incread inflight in basic_sched_submit_io_work, now %d\n", atomic_read(&afq_data->inflight));
					afq_account_request2(afq_data, to_serve_req_desc);
					//afq_account_request(afq_data, to_serve_req_desc);
					list_del_init(&io_work->list);
					io_batch_desc->one_loop_work++;
					read_req_num++;
					elv_dispatch_sort(to_serve_req_desc->q, to_serve_req_desc->rq);
				}
				break;
			default:
				printk(KERN_ERR "unrecoginzed io_work type %d to submit!!!!\n", io_work->type);
				list_del_init(&io_work->list);
				break;
		}
	}
	//printk("one batch submitted with %d work: %d fsync, %d write %d mkdir %d read_req\n", io_batch_desc->one_loop_work, fsync_num, write_num, mkdir_num, read_req_num);

	spin_lock_irqsave(&afq_data->lock, flags);
	afq_data->keep_wait = NO_NEW_BATCH;
	if(0 && write_num > 0){
	//if(write_num > 0){
		afq_data->batch_dead = 1;
	}
	spin_unlock_irqrestore(&afq_data->lock, flags);
	if(timer_pending(&afq_data->batch_wait_timer)){
		del_timer(&afq_data->batch_wait_timer);
	}
	afq_data->batch_wait_timer.data = (unsigned long)afq_data;
	afq_data->batch_wait_timer.function = new_batch_wait_timer_func;
	afq_data->batch_wait_timer.expires = jiffies + START_IN_FLIGHT_TIME;
	add_timer(&afq_data->batch_wait_timer);

	if(timer_pending(&afq_data->batch_dead_timer)){
		del_timer(&afq_data->batch_dead_timer);
	}
	afq_data->batch_dead_timer.data = (unsigned long)afq_data;
	afq_data->batch_dead_timer.function = batch_dead_timer_func;
	afq_data->batch_dead_timer.expires = jiffies + AFQ_BATCH_DEAD_INTVAL;
	add_timer(&afq_data->batch_dead_timer);
}


int basic_sched_want_dispatch(struct afq_data *afq_data){
	int keep_wait;
	int batch_dead;
	unsigned long flags;

	spin_lock_irqsave(&afq_data->lock, flags);
	keep_wait = afq_data->keep_wait;
	batch_dead = afq_data->batch_dead;
	spin_unlock_irqrestore(&afq_data->lock, flags);

	if(batch_dead)
		return 0;

	if(keep_wait == ALLOW_NEW_BATCH){
		return 1;
	}
	/*
	 * Note: keep_wait might have been changed when we are checkiing inflight
	 * But it is OK in this case because at worst we risk not sending requests to disk
	 * it will be sent later anyway and this should happen rarely
	 * yangsuli 5/30/2014
	 */
	//printk("now in basic_sched_want_dispatch, inflight is %d\n", atomic_read(&afq_data->inflight));
	if ((keep_wait == KEEP_IN_FLIGHT)
		&& (atomic_read(&afq_data->inflight) < MIN_IN_FLIGHT)){
		return 2;
	}
	
	return 0;
}

int basic_sched_dispatch(struct request_queue *q, int force){
	struct afq_data *afq_data = q->elevator->elevator_data;
	int has_work = 0;
	void *work_selected = NULL;
	int ret = 0;

	ret = basic_sched_want_dispatch(afq_data);
	if(!ret)
		return 0;

	has_work = basic_sched_select_io_work(q, &work_selected);

	if(has_work == -1){
		printk(KERN_ERR "select_io_work failed!\n");
		return 0;
	}

	if(has_work == 0){
		return 0;
	}

	basic_sched_submit_io_work(afq_data, work_selected);

	return 1;
}


//FIXME:
//For better performance, use afq_data->lock instead queue_lock to avoid lock contention
ssize_t basic_sched_write_entry(struct request_queue *rq, struct file *file, size_t count, loff_t *pos, void** opaque, int sched_uniq){
	struct wrdesc *wrdesc = NULL;
	struct afq_data *afq_data = rq->elevator->elevator_data;
	struct account *acct = NULL;
	unsigned long flags;


	
	//spin_lock_irqsave(rq->queue_lock, flags);
	spin_lock_irqsave(&afq_data->lock, flags);

	
	
	if(sched_uniq != rq->sched_uniq){
		printk(KERN_ERR "wrong sched_uniq for write_entry!\n");
		*opaque = NULL;
		//spin_unlock_irqrestore(rq->queue_lock, flags);
		return -1;
	}

	acct = get_account(&afq_data->acct_hash, get_account_id(current));
	if(acct && acct->account_id && acct->vio_counter < afq_data->global_min_vio_counter){
		afq_data->global_min_vio_counter = acct->vio_counter;
	}
	//spin_unlock_irqrestore(rq->queue_lock, flags);
	spin_unlock_irqrestore(&afq_data->lock, flags);


	if(!acct || !acct->account_id){
		*opaque = NULL;
		return 0;
	}

	acct->num_write_call++;
	if(unlikely(file->f_flags & O_DIRECT)){
		goto sched_write;
	}

	if(acct->vio_counter - afq_data->global_min_vio_counter2 <= WRITE_ALLOWANCE){
		acct->num_write_passed++;
		//printk("account %d write passed because vio_counter %d and global_min %d\n", acct->account_id, acct->vio_counter, afq_data->global_min_vio_counter2);
		*opaque = NULL;
		return 0;
	
	}
	//printk("account %d with prio %d write blocked because vio_counter %d and global_min %d\n", acct->account_id, acct->ioprio, acct->vio_counter, afq_data->global_min_vio_counter2);

sched_write:
	acct->num_write_blocked++;
	wrdesc = get_wrdesc(file, count, pos);
	if(IS_ERR(wrdesc)){
		return PTR_ERR(wrdesc);
	}

	if(wrdesc == NULL){
		printk(KERN_ERR "wrdesc is NULL\n");
		return -ENOMEM;
	}

	wrdesc->acct = acct;
	spin_lock_irqsave(rq->queue_lock, flags);
	add_io_work_to_acct(afq_data, &wrdesc->io_work, acct);
	__blk_run_queue(afq_data->queue);
	spin_unlock_irqrestore(rq->queue_lock, flags);

	*opaque = wrdesc;
	wait_for_completion(&wrdesc->w_completion);
	return 0;

}

void basic_sched_write_return(struct request_queue *q, void *opaque, ssize_t rv, int sched_uniq){
	struct wrdesc *wrdesc = opaque;
	struct afq_data *afq_data = NULL;
	unsigned long flags;

	if(!wrdesc)
		return;

	spin_lock_irqsave(q->queue_lock, flags);
	if(q->sched_uniq == sched_uniq){
		afq_data = q->elevator->elevator_data;
	}
	dec_check_cleanup_batch(afq_data, wrdesc->owner_batch);
	spin_unlock_irqrestore(q->queue_lock, flags);
	put_wrdesc(wrdesc);
}


int basic_sched_mkdir_entry(struct request_queue *rq, struct inode *dir, struct dentry *dentry, int mode, void** opaque, int sched_uniq){
	struct mkdir_desc *mkdir_desc = NULL;
	struct afq_data *afq_data = rq->elevator->elevator_data;
	struct account *acct = NULL;
	unsigned long flags;

	spin_lock_irqsave(rq->queue_lock, flags);

	if(sched_uniq != rq->sched_uniq){
		*opaque = NULL;
		spin_unlock_irqrestore(rq->queue_lock, flags);
		return -1;
	}

	acct = get_account(&afq_data->acct_hash, get_account_id(current));
	if(acct && acct->account_id && acct->vio_counter < afq_data->global_min_vio_counter){
		afq_data->global_min_vio_counter = acct->vio_counter;
	}
	spin_unlock_irqrestore(rq->queue_lock, flags);

	if(!acct || !acct->account_id){
		*opaque = NULL;
		return 0;
	}

	if(acct->vio_counter - afq_data->global_min_vio_counter2 <= WRITE_ALLOWANCE){
		*opaque = NULL;
		return 0;
	}

	mkdir_desc = get_mkdir_desc(dir, dentry, mode);
	if(IS_ERR(mkdir_desc)){
		return PTR_ERR(mkdir_desc);
	}

	if(mkdir_desc == NULL){
		printk(KERN_ERR "mkdir_desc is NULL\n");
		return -ENOMEM;
	}

	mkdir_desc->acct = acct;
	spin_lock_irqsave(rq->queue_lock, flags);
	add_io_work_to_acct(afq_data, &mkdir_desc->io_work, acct);
	__blk_run_queue(afq_data->queue);
	spin_unlock_irqrestore(rq->queue_lock, flags);

	*opaque = mkdir_desc;
	wait_for_completion(&mkdir_desc->completion);

	return 0;
}


void basic_sched_mkdir_return(struct request_queue *q, void *opaque, int rv, int sched_uniq){
	struct mkdir_desc *mkdir_desc = opaque;
	struct afq_data *afq_data = NULL;
	unsigned long flags;

	if(!mkdir_desc)
		return;

	spin_lock_irqsave(q->queue_lock, flags);
	if(q->sched_uniq == sched_uniq){
		afq_data = q->elevator->elevator_data;
	}
	dec_check_cleanup_batch(afq_data, mkdir_desc->owner_batch);
	spin_unlock_irqrestore(q->queue_lock, flags);
	put_mkdir_desc(mkdir_desc);
}









int basic_sched_fsync_entry(struct request_queue *rq, struct file* file, int datasync, void **opaque, int sched_uniq){
	struct fsync_desc *fsync_desc =  NULL;
	struct afq_data *afq_data = rq->elevator->elevator_data;
	struct account *acct = NULL;
	unsigned long flags;

	fsync_desc = get_fsync_desc(file, datasync);
	if(IS_ERR(fsync_desc)){
		return PTR_ERR(fsync_desc);
	}

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

	acct = get_account(&afq_data->acct_hash, get_account_id(current));
	if (acct && acct->account_id) {
		fsync_desc->acct = acct;
		//printk("add fsync io_work to account...\n");
		add_io_work_to_acct(afq_data, &fsync_desc->io_work, acct);
		WARN_ON(list_empty(&afq_data->active_acct_list));
		__blk_run_queue(afq_data->queue);
	}
	spin_unlock_irqrestore(rq->queue_lock, flags);

	if(!acct || !acct->account_id){
		put_fsync_desc(fsync_desc);
		*opaque = NULL;
		return 0;
	}

	*opaque = fsync_desc;
	wait_for_completion(&fsync_desc->f_completion);

	return 0;
}


void basic_sched_fsync_return(struct request_queue *q, void *opaque, int rv, int sched_uniq){
	struct fsync_desc *fsync_desc = opaque;
	struct afq_data *afq_data = NULL;
	unsigned long flags;

	if (opaque == NULL)
		return;


	spin_lock_irqsave(q->queue_lock, flags);
	if(q->sched_uniq == sched_uniq){
		afq_data = q->elevator->elevator_data;
	}
	dec_check_cleanup_batch(afq_data, fsync_desc->owner_batch);
	spin_unlock_irqrestore(q->queue_lock, flags);
	put_fsync_desc(fsync_desc);
}


void basic_sched_causes_dirty(struct request_queue *q, struct cause_list* causes,
		struct task_struct *new, struct inode *inode, long pos){
	struct afq_data *afq_data = q->elevator->elevator_data;
	int new_account_id = get_account_id(new);
	struct io_cause *cause;
	struct account *account;
	int charge;
	long tot_cost = causes_get_cost(causes);


	if(tot_cost <= 0){
		tot_cost = seek_time(pos, inode_get_pos(inode)) + causes->size;
		causes_set_cost(causes, tot_cost);
	}
	inode_set_pos(inode, pos+causes->size);

	list_for_each_entry(cause, &causes->items, list){
		account = get_account(&afq_data->acct_hash, cause->account_id);
		charge = safe_div(tot_cost, causes->item_count);
		if(account->account_id != new_account_id)
			charge -= safe_div(tot_cost, (causes->item_count - 1));
		add_estimate_vio(afq_data, account, charge);
	}
}


void basic_sched_causes_free(struct request_queue *q, struct cause_list *causes){
	struct afq_data *afq_data = q->elevator->elevator_data;
	struct io_cause *cause;
	struct account *account;
	int refund;

	list_for_each_entry(cause, &causes->items, list){
		account = get_account(&afq_data->acct_hash, cause->account_id);
		refund = safe_div(causes->size, causes->item_count);
		add_vio_counter(afq_data, account, -refund);
	}
}
       	

static const int sched_sleep = AFQ_SCLICE_IDLE * 2;

int sched_thread_func(void *arg){
	unsigned long flags;
	struct afq_data *afq_data = arg;
	printk("sched_thread_func calling split_schedule_ops.split_schedule_init\n");

	while(!kthread_should_stop()){
		spin_lock_irqsave(afq_data->queue->queue_lock, flags);
		__blk_run_queue(afq_data->queue);
		spin_unlock_irqrestore(afq_data->queue->queue_lock, flags);
		msleep(sched_sleep);
	}

	return 0;
}

void update_active_min(struct afq_data* afq_data){
	int bkt;
	struct account *cur_acct;
	long min_vio_counter = MAX_VIO_COUNTER_THRESHOLD;
	struct account *min_acct = NULL;

	acct_hash_for_each(&afq_data->acct_hash, bkt, cur_acct, node){
		if(cur_acct->account_id && time_before(jiffies, cur_acct->last_end_request + ACCT_MAX_SLEEP_TIME/15) && cur_acct->vio_counter < min_vio_counter){
			min_vio_counter = cur_acct->vio_counter;
			min_acct = cur_acct;
		}
	}
	if(min_vio_counter != MAX_VIO_COUNTER_THRESHOLD){
		//printk("set global_min_vio_counter2 to %d\n", min_vio_counter);
		afq_data->global_min_vio_counter2 = min_vio_counter;
		afq_data->min_acct = min_acct;
	}
}

int min_thread_func(void *arg){
	unsigned long flags;
	struct afq_data *afq_data = arg;

	printk("min_thread_func calling\n");

	while(!kthread_should_stop()){
		spin_lock_irqsave(afq_data->queue->queue_lock, flags);
		update_active_min(afq_data);
		spin_unlock_irqrestore(afq_data->queue->queue_lock, flags);
		msleep(HZ);
	}

	return 0;
}



int start_sched_thread(struct afq_data *afq_data){
	afq_data->sched_thread = kthread_create(sched_thread_func,afq_data, "split_sched_thread");
	if(!IS_ERR(afq_data->sched_thread)){
		wake_up_process(afq_data->sched_thread);
	}else{
		return -ENOMEM;
	}

	return 0;
}

int start_min_thread(struct afq_data *afq_data){
	afq_data->min_thread = kthread_create(min_thread_func, afq_data, "split_min_thread");
	if(!IS_ERR(afq_data->min_thread)){
		wake_up_process(afq_data->min_thread);
	}else{
		return -ENOMEM;
	}

	return 0;
}

static void basic_kv_set_callback(struct request_queue *q,
		int id, char *key, long val){
	struct afq_data *afq_data = q->elevator->elevator_data;
	struct account *account;
	unsigned long flags;

	printk("basic_kv_set_callback called with id %d key %s val %ld\n",id, key, val);

	spin_lock_irqsave(q->queue_lock, flags);
	account = get_account(&afq_data->acct_hash, id);
	if(account){
		if (strcmp(key, "ioprio") == 0) {
			if(val != account->ioprio){
				account->ioprio = val;
				//account->vio_counter = 0; //Reset vio counter
				printk("set account %d prio to %d vio_counter %ld\n", account->account_id, account->ioprio, account->vio_counter);
			}
		}
		if (strcmp(key, "vio_counter") == 0) {
			if(val != account->vio_counter){
				account->vio_counter = val;
				printk("set account %d vio_counter to %ld\n", account->account_id, account->vio_counter);
			}
		}
	}
	spin_unlock_irqrestore(q->queue_lock, flags);
}

void stop_sched_thread(struct afq_data *afq_data){
	printk("stop_sched_thread called\n");
	kthread_stop(afq_data->sched_thread);
}

void stop_min_thread(struct afq_data *afq_data){
	printk("stop_min_thread called\n");
	kthread_stop(afq_data->min_thread);
}

/* typedef int (elevator_init_fn) (struct request_queue *,
 * 				struct elevator_type *e);
 * See also: block/cfq-iosched.c */
static int basic_sched_init_data(struct request_queue *q, struct elevator_type *e)
{
	struct elevator_queue *eq;
	struct afq_data *afq_data;
	int ret = 0;
	int i;
	struct io_batch *sim_batch = NULL;
	
	eq = elevator_alloc(q, e);
	if (!eq)
		return -ENOMEM;

	afq_data = kmalloc_node(sizeof(struct afq_data), GFP_KERNEL, q->node);
	if(!afq_data){
		kobject_put(&eq->kobj);
		return -ENOMEM;
	}
	
	eq->elevator_data = afq_data;

	afq_data->queue = q;
	init_acct_hash(&afq_data->acct_hash);
	INIT_LIST_HEAD(&afq_data->active_acct_list);
	afq_data->global_min_vio_counter = MAX_VIO_COUNTER_THRESHOLD;
	afq_data->global_min_vio_counter2 = MAX_VIO_COUNTER_THRESHOLD;
	afq_data->min_acct = NULL;
	afq_data->last_served_acct = NULL;
	spin_lock_init(&afq_data->lock);
	afq_data->keep_wait = ALLOW_NEW_BATCH;
	afq_data->batch_dead = 0;
	afq_data->inflight = ((atomic_t) { (0) });
	init_timer(&afq_data->batch_wait_timer);
	init_timer(&afq_data->batch_dead_timer);

	sim_batch = iobatch_new(BATCH_SIZE, BATCH_BYTE_CAP);
	if(!sim_batch)
		goto fail;

	afq_data->sim_batch = sim_batch;

	afq_data->disk_full_count = 0;

	for ( i = 0; i < IOPRIO_BE_NR; i++){
		afq_data->prio_served_rqs[i] = 0;
	}
	for(i = 0; i < MAX_CAUSE_COUNT; i++){
		afq_data->caust_count_dist[i] = 0;
		afq_data->journal_cct[i] = 0;
	}
	
	spin_lock_irq(q->queue_lock);
	q->elevator = eq;
	spin_unlock_irq(q->queue_lock);

	ret = start_sched_thread(afq_data);
	if(ret != 0){
		goto fail1;
	}

	ret = start_min_thread(afq_data);
	if(ret != 0){
		goto fail2;
	}

	ioctl_helper_register_disk(q, basic_kv_set_callback);
	return 0;

fail2:
	stop_sched_thread(afq_data);
fail1:
	kfree(sim_batch);
fail:
	kfree(afq_data);
	kobject_put(&eq->kobj);
	return -ENOMEM; /* TODO: fix return int */
}

static void basic_sched_exit_data(struct elevator_queue *e){
	struct afq_data *afq_data = e->elevator_data;
	int i;


	printk("afq disk_full_count %d\n", afq_data->disk_full_count);

	for ( i = 0; i < IOPRIO_BE_NR; i++){
		printk("afq prio: %d req_num: %d\n", i, afq_data->prio_served_rqs[i]);
	}

	print_accounts_stat(&afq_data->acct_hash);

	/*
	for(i = 0; i < MAX_CAUSE_COUNT; i++){
		printk("count: %d num_meta_blks: %d\n", i, afq_data->caust_count_dist[i]);
	}

	for(i = 0; i < MAX_CAUSE_COUNT; i++){
		printk("journal: %d num_meta_blks: %d\n", i, afq_data->journal_cct[i]);
	}*/

	if(timer_pending(&afq_data->batch_dead_timer)){
		del_timer(&afq_data->batch_dead_timer);
	}
	if(timer_pending(&afq_data->batch_wait_timer)){
		del_timer(&afq_data->batch_dead_timer);
	}

	stop_sched_thread(afq_data);
	stop_min_thread(afq_data);
	ioctl_helper_unregister_disk(afq_data->queue);
	iobatch_free(afq_data->sim_batch);
	free_accounts(&afq_data->acct_hash);
	kfree(afq_data);
}


static struct elevator_type basic_sched_elv_type = {
	.ops = {
	.elevator_dispatch_fn = basic_sched_dispatch,
	.elevator_add_req_fn = basic_sched_add_request,
	.elevator_completed_req_fn = basic_sched_complte_request,
	.elevator_merge_req_fn = basic_sched_merged_request,
	.elevator_set_req_fn = split_set_request,
	.elevator_put_req_fn = split_put_request,
	.elevator_write_entry_fn = basic_sched_write_entry,
	.elevator_write_return_fn = basic_sched_write_return,
	//.elevator_write_return_fn = NULL,
	.elevator_mkdir_entry_fn = basic_sched_mkdir_entry,
	.elevator_mkdir_return_fn = basic_sched_mkdir_return,
	.elevator_fsync_entry_fn = basic_sched_fsync_entry,
	.elevator_fsync_return_fn = basic_sched_fsync_return,
	.elevator_causes_dirty_fn = basic_sched_causes_dirty,
	.elevator_causes_free_fn = basic_sched_causes_free,
	.elevator_init_fn = basic_sched_init_data,
	.elevator_exit_fn = basic_sched_exit_data,
	},
	.elevator_name = "afq",
	.elevator_owner = THIS_MODULE,
};


static int __init basic_sched_init(void){
	int rv;
	rv = ioctl_helper_init(IOCTL_MAJOR);
	WARN_ON(rv);
	elv_register(&basic_sched_elv_type);
	
	return 0;
}

static void __exit basic_sched_exit(void){
	ioctl_helper_cleanup();
	elv_unregister(&basic_sched_elv_type);
}

module_init(basic_sched_init);
module_exit(basic_sched_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Basic IO Scheduler");
