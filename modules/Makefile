obj-m := tb-iosched.o acct-afq-iosched.o 

acct-afq-iosched-objs := acct_afq_sched.o split_sched.o split_account.o ioctl_helper.o io_batch.o disk_sim.o
split-deadline-iosched-objs := split_deadline_sched.o split_account.o split_sched.o ioctl_helper.o expire_rb.o io_work_expire_rb.o io_batch.o disk_sim.o
tb-iosched-objs := tbucket.o ioctl_helper.o io_batch.o disk_sim.o


KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

insert:
	sudo insmod ./acct-afq-iosched.ko 

remove:
	sudo rmmod ./acct-afq-iosched.ko
