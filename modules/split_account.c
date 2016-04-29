#include <linux/slab.h>
#include <linux/list.h>
#include <linux/hashtable.h>
#include <linux/sched.h>
#include "split_account.h"

void init_acct_hash(struct acct_hash *acct_hash){
	hash_init(acct_hash->accounts);
}

static struct account *new_account(int account_id){
	struct account *account = kmalloc(sizeof(struct account), GFP_ATOMIC);
	if (account) {
		memset(account, 0, sizeof(struct account));
		account->account_id = account_id;
		INIT_HLIST_NODE(&account->node);
		INIT_LIST_HEAD(&account->list);
		account->sim_batch = iobatch_new(BATCH_SIZE, BATCH_BYTE_CAP);
		if(!account->sim_batch)
			goto fail;
		account->ioprio = 0;
		account->vio_counter = 0;
		account->estimate = 0;
		account->batch_estimate = 0;
		account->vio_reset = 0;
		account->num_read_req = 0;
		account->num_write_req = 0;
		account->num_write_call = 0;
		account->num_write_passed = 0;
		account->num_write_blocked = 0;
		account->num_fsync_call = 0;
		account->set_vio_to_global_min = 0;
		account->seq_counter = 0;
		account->last_sector = 0;
		account->last_end_request = jiffies;
		account->slice_begin = 0;
		account->slice_end = 0;
		account->read_expire = HZ / 2;
		account->write_expire = 5 * HZ;
		account->fsync_expire = 5 * HZ;
		account->dirty_pages = 0;
		INIT_LIST_HEAD(&account->io_work_list_head);
	}

	return account;

fail:
	kfree(account);
	return NULL;
}
//queue lock must be held!
struct account *get_account(struct acct_hash *acct_hash, int account_id) {
	struct account *account = NULL;

	/*
	if (account_id == 0)
		return NULL;
	*/ 

	hash_for_each_possible(acct_hash->accounts, account,
			node, account_id) {
		if (account->account_id == account_id)
			goto found;
	}

	//not found!
	account = new_account(account_id);
	if (account){
		hash_add(acct_hash->accounts, &account->node, account_id);
	}

found:
	return account;
}

int get_account_id(struct task_struct* tsk) {
	return atomic_read(&tsk->account_id);
}

void print_accounts_stat(struct acct_hash* acct_hash){
	int bkt;
	struct hlist_node *cur;
	struct account *account;
	hash_for_each_safe(acct_hash->accounts, bkt, cur, account, node){
		printk("account_id %d prio %d vio_counter %ld "
			"num_read_req %d num_write_req %d "
			"num_write_call %d num_write_passed %d num_write_blocked %d "
			"num_fsync_call %d "
			"vio_reset %d\n",
			account->account_id, account->ioprio, account->vio_counter, 
			account->num_read_req, account->num_write_req,
			account->num_write_call, account->num_write_passed, account->num_write_blocked,
			account->num_fsync_call,
			account->vio_reset);
	}
}

void free_accounts(struct acct_hash* acct_hash){
	int bkt;
	struct hlist_node *cur;
	struct account *account;
	hash_for_each_safe(acct_hash->accounts, bkt, cur, account, node){
		hash_del(&account->node);
		kfree(account);
	}

}

