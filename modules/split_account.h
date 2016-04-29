#include <linux/list.h>
#include <linux/hashtable.h>
#include "io_batch.h"

#define KB (1024)
#define MB (1024*KB)
#define BATCH_SIZE (64)
#define BATCH_BYTE_CAP (32*MB)

struct account {
	//general account stuff
	int account_id;
	struct hlist_node node;
	struct list_head list;

	//stats
	int num_read_req;
	int num_write_req;
	int num_write_call;
	int num_write_passed;
	int num_write_blocked;
	int num_fsync_call;
	int set_vio_to_global_min;

	//scheduler specific
	int ioprio;
	struct list_head io_work_list_head;
	long vio_counter;
	long estimate;
	// batch_estimate describes how much we have charged for
	// this batch prior the the batch being complete.  When
	// we actually charge for the batch, we need to refund this.
	size_t batch_estimate;
	int vio_reset;
	int seq_counter;
	int last_sector;

	unsigned long last_end_request;
	unsigned long slice_begin;
	unsigned long slice_end;

	int read_expire;
	int write_expire;
	int fsync_expire;
	int dirty_pages;

	struct io_batch *sim_batch;
};

struct acct_hash {
	DECLARE_HASHTABLE(accounts, 10);
};

void init_acct_hash(struct acct_hash *acct_hash);
struct account *get_account(struct acct_hash *acct_hash, int account_id);
int get_account_id(struct task_struct* tsk);
void print_accounts_stat(struct acct_hash* acct_hash);
void free_accounts(struct acct_hash* acct_hash);

#define acct_hash_for_each(name, bkt, obj, member)	\
	hash_for_each((name)->accounts, bkt, obj, member)
