#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/ioctl.h>
#include <linux/list.h>

#include "ioctl_helper.h"

struct disk_info {
	struct request_queue *q;
	kv_set_fn *kv_set;
	struct list_head list;
};

static spinlock_t big_lock;
struct list_head disk_info_list;
static int maj = 0;

static void kv_set_all(int id, char* key, long val) {
	struct disk_info* di;
	spin_lock(&big_lock);
	list_for_each_entry(di, &disk_info_list, list) {
		di->kv_set(di->q, id, key, val);
	}
	spin_unlock(&big_lock);
}

// ioctl code adapted from:
// http://people.ee.ethz.ch/~arkeller/linux/code/ioctl.c
static long device_ioctl(struct file *filep,
						 unsigned int cmd,
						 unsigned long arg) {
	long id;
	char *field;
	long val;
	char *start;
	char *end;
	char msg[100];
	int rv;

	rv = copy_from_user(msg, (char *)arg, 100);

	// parse out "<id> <field> <value>"
	start = msg;
	end = strpbrk(start, " ");
	if (!end) {
		return -1;
	}
	*end = '\0';
	//field 1
	kstrtol(start, 10, &id);
	start = end+1;
	end = strpbrk(start, " ");
	if (!end) {
		return -1;
	}
	*end = '\0';
	//field 2
	field = start;
	start = end+1;
	// field 3
	kstrtol(start, 10, &val);

	if (strcmp(field, "set-account") == 0) {
		printk("set account %ld\n", val);
		atomic_set(&current->account_id, val);
	} else {
		kv_set_all(id, field, val);
	}

	return 100;
}

static struct file_operations fops = {
	.unlocked_ioctl = device_ioctl,
};

int ioctl_helper_register_disk(struct request_queue *q,
							   kv_set_fn kv_set) {
	struct disk_info* di;
	di = kmalloc(sizeof(*di), GFP_KERNEL);
	if (!di)
		return 1;
	di->q = q;
	di->kv_set = kv_set;
	INIT_LIST_HEAD(&di->list);

	spin_lock(&big_lock);
	list_add_tail(&di->list, &disk_info_list);
	spin_unlock(&big_lock);

	return 0;
}

void ioctl_helper_unregister_disk(struct request_queue *q) {
	struct disk_info* di;
	struct disk_info* tmp;
	spin_lock(&big_lock);
	list_for_each_entry_safe(di, tmp, &disk_info_list, list) {
		if(di->q == q) {
			list_del(&di->list);
			kfree(di);
			spin_unlock(&big_lock);
			return;
		}
	}
	spin_unlock(&big_lock);
	BUG_ON(1);
}

int ioctl_helper_init(int _maj) {
	spin_lock_init(&big_lock);
	INIT_LIST_HEAD(&disk_info_list);
	maj = _maj;
	return register_chrdev(maj, "my_device", &fops);
}

void ioctl_helper_cleanup(void) {
	BUG_ON(!list_empty(&disk_info_list));
	unregister_chrdev(IOCTL_MAJOR, "my_device");
}


MODULE_LICENSE("GPL");

//
// Example program to set rates/limits.
//
/*
// adapted from http://people.ee.ethz.ch/~arkeller/linux/code/ioctl_user.c
#include <stdio.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <errno.h>

int main(){
char buf[100];
int fd = -1;
int rv;
mknod("/dev/cdev_example", S_IFCHR|0666, makedev(IOCTL_MAJOR, 0));
printf("open\n");
if ((fd = open("/dev/cdev_example", O_RDWR)) < 0) {
perror("open");
return -1;
}
sprintf(buf, "123 rate 100");
printf("buf = %p\n", buf);
if(ioctl(fd, 1, buf) < 0)
perror("first ioctl");

return 0;
}
*/
