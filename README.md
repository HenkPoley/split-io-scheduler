# split-io-scheduler
Back/forward ported patches based on https://research.cs.wisc.edu/adsl/Software/split/

## Build instructions

Fetch the kernel source you want:

```
wget https://cdn.kernel.org/pub/linux/kernel/v3.x/linux-<kernel version>.tar.xz
unp linux-<kernel version>.tar.xz
cd linux-<kernel version>
```

Apply the patches for your kernel version:

```
for i in <this path>/patches/<kernel version>/*.patch*; do patch -p1 < $i; done
```

or using quilt:

```
cp -r <this path>/patches/<kernel version>/ patches/
quilt push -a
```

Then fix individual patches with `quilt push -f`, edit the changes in yourself, `quilt refresh`. Continue with `quilt push -a`.

Copy config_yangsuli, salvaged from release tarball, to .config (note the dot). This config does not boot a default Ubuntu 12.04.0 (precise) install, try the config_ubuntu* instead.
`make menuconfig`, and then make necessary changes in configuration. For example it is required to compile BTRFS into the kernel (=y), build will fail as module. This is already set in the ubuntu config.

```
make -j`nproc`
##make bzImage -j`nproc`
##make modules -j`nproc`
#sudo make modules_install
#sudo make install
fakeroot make-kpkg -j`nproc` --initrd --append-to-version=-split-level-io --revision=1 kernel_image kernel_headers
sudo dpkg -i ../*-split-level-io*1*.deb
sudo reboot
```

## Modules
The modules/ directory contains the kernel module sources of the actual schedulers. The Makefile depends on that you run the split-level-io kernel. 

```
cd modules
make
#make insert (inserts just afq as example)
sudo insmod ./tb-iosched.ko
sudo insmod ./acct-afq-iosched.ko
cat /sys/block/sdb/queue/scheduler (should show extra 'tb' & 'afq' items)
echo afq | sudo tee /sys/block/sdb/queue/scheduler
```

After initial compilation, if you updated some code in the kernel:

```
make -j`nproc`
##make bzImage -j`nproc`
##make modules -j`nproc`
#sudo make modules_install
#sudo make install
fakeroot make-kpkg -j`nproc` --initrd --append-to-version=-split-level-io --revision=2 kernel_image kernel_headers
sudo dpkg -i ../*-split-level-io*2*.deb
sudo reboot
```

If you want to install updated header files (e.g., because you added a syscall)
```
sudo make headers_install INSTALL_HDR_PATH=/usr/include
```

## Notes about the code

* include/linux/hashtable.h is backported from Linux 3.7 (code unchanged)
* The variable expire_rb_node is used in the modules, not in the kernel, gives build warning
* xfs_vnodeops.c.patch just contains a comment about dirtying data
* variable i_private1 probably needs a better comment
* Fixed in 3.4 patch: printf misuse of %d for a size_t complaint during build (should be %zu)
* Lost track of one of the 'here dirty' *comments* in fs/xfs/xfs_inode.c in Linux v3.5. Ah well..
* Removed large-ish "SAMER" comment in fs/btrfs/extent_io.c in Linux v3.8 patchset.
* Commented out request_sanity_check() in fs/btrfs/extent_io.c in kernel 3.14, as it was no longer compatible
* The variable `current` is a pointer to the current process.
* BUG_ON() checks if something is zero/null, else it panicks the kernel


## TODO

* Test whether these kernels still boot O:-)
* Check if everything in 3.2.51 is still represented in the latest patchset, and in a sane place.
* Figure out if new block dirtying have been introduced in later kernels that need 'causes' tagged.
* Figure out some benchmarks to test if it still does anything.
* Change modules/Makefile so you can also build the modules before booting the kernel.
* Merge patches together into sensible patchsets, instead of per file
* Fix kernel compilation linking failure with BTRFS / Ext4 / XFS as modules.
* Do regression testing with SibylFS if the extra locking does not introduce bugs: https://sibylfs.github.io/
* Check that the kernel thread changes do not introduce problems, with KernelThreadSanitizer (Linux 4.x): https://github.com/google/ktsan
* Clean out comments, such as all the commented printk() in block/cfq-iosched.c & fs/btrfs/file.c
* Why is cfq_latency = 0 in block/cfq-iosched.c (instead of vanilla default 1) ?
* BUG? It feels to me like the sched_uniq++ construct in block/elevator.c will not work properly in the Linux v3.5 patch. I suspect it was supposed to give every new loaded I/O scheduler (elevator) a new unique ID. From what I can see it always either becomes 1000, 1001, or 1. Because it doesn't track an atomic incrementing counter elsewhere. It just writes on newly created elevator queues. But maybe those inherit pretty much everything from the previous one (?)
* Since SPLIT_NODEP is not defined, maybe remove the #ifndef SPLIT_NODEP / #endif checks. Check the paper what this is supposed to do.
* Clean out SPLIT_DEBUG from fs/jbd2/commit.c
* What is a "cause proxy" ?
* Code documentation / story

## Order of back/forward porting

I 'might not' have put changes I've done in later patches back into the older patches.
Be sure to note that the "Does it compile?" test does not mean it actually works.

* 3.2.51 - original, from tarball, excluded some cruft. Boots.
* 3.2 - Backport. Also has trailing whitespace fixes. Compiles. Not tested.
* 3.3 - elevator->elevator_type to elevator->type, and a fixup around q->sched_uniq in elevator initialization/switching, some whitespace fixes. Compiles, 'section mismatch' warnings in some modules are not caused by this patch. Not tested.
* 3.4 - Ext4 (jbd2) now also has it's own function to free a transaction, moved the causes list admin call there. Should check if other filesystems also did this change, and I properly added the causes tracking there too. Compiles. 'section mismatch' warning is still not caused by our code. Not tested.
* 3.5 - lost the insertion point of a *comment* in fs/xfs/xfs_inode.c. Seems that there is nothing that 'dirties' anything after that point anymore. Not tested.
* 3.6 - Seemed straightforward. Compiles. Not tested.
* 3.7 - Seemed straightforward. Compiles. Not tested.
* 3.8 - Seemed straightforward. Compiles. Not tested.
* 3.9 - Dropped a commented printk() in block/cfq-iosched.c. Compiles. Not tested.
* 3.10 - Seemed straightforward. Compiles. Not tested.
* 3.11 - Had to do some careful parsing of fs/jbd2/commit.c, but that patch was straightforward in the end. Changes to fs/ext4/inode.c are more complicated. Might well have made mistakes here. In some places they renamed 'bh' to 'descriptor', those mistakes would probably be picked up by the compiler. Had to fix thinkos in my code in both fs/jbd2/commit.c & fs/ext4/inode.c already. Split journal in jbd2_journal_commit_transaction() was removed in this kernel, so I commented the causes tracking for SPLIT_JOURNAL_META. Compiles. Not tested.
* 3.12 - fs/xfs/xfs_vnodeops.c is gone, since it's merely a comment I deleted the patch from the patchset. Otherwise straightforward. Compiles. Not tested.
* 3.13 - Seemed straightforward. Compiles. Not tested.
* 3.14 - Removed print_page_info() from fs/btrfs/extent_io.c, which broke compilation. That function was only referenced from the previously removed "SAMER" comment. Commented out request_sanity_check() too. Seemed otherwise straightforward.  Compiles. Not tested.
* 3.15 - Seemed straightforward. Compiles. Not tested.
* 3.16 - fs/bio.c moved to block/bio.c, otherwise straightforward. Compiles. Not tested.
* 3.17 - kernel/hrtimer.c moved to kernel/time/hrtimer.c. Otherwise straightforward. Compiles. Not tested.
* 3.18 - Seemed straightforward. Small change around a kprint() in fs/jbd2/checkpoint.c. Compiles. Not tested.
* 3.19 - Applied in one go, just refreshed patches for good measure. Compiles. Not tested.
* 4.0 - Seems like somebody did some copy-pasting in fs/xfs/xfs_inode.c, pasted yangsuli *comment* in both places. Otherwise straightforward. Compiles. Not tested.
* 4.1 - Seemed straightforward. Compiles. Not tested.
* 4.2 - Seemed straightforward. Had a problem with INIT_TASK() in init/init_task.c. Solved by putting causes and account_id in task_struct of include/linux/sched.h, where it belongs. Code got there because they moved a few lines where the patch relied on. Compiles. Not tested.
* 4.3 - Seemed straightforward. Compiles. Not tested.
* 4.4 - Seemed straightforward. Compiles. Not tested.
* 4.5 - Seemed straightforward. Compiles. Kernel boots. Modules fail to build.
* Ran scripts/checkpatch.pl, cleaned up remarks
* Adapted tbucket.c for Linux v4.5. (struct queue)->elevator_private becomes (struct queue)->elv.priv, bio->bi_size become bio->bi_iter.bi_size, account_for_causes() no longer needs a temp variable. an elevator_init_fn() needs to allocate an elevator and store its variables in there, and not return a pointer but an integer success/error value. tb-iosched.ko now insmods and "does something". But if you switch to it, after a while the kernel hangs, during a "make clean; make bzImage -j4".
* Same fixes applied to acct_afq_sched.c, split_account.{c,h}, split_sched.{c,h}. The module acct-afq-iosched.ko now inserts. And when you switch a block device to it, a kernel compile does not hang the system (very long..) as happened with tbucket.

## Missing code verification

Check if 3.2.51 and 4.5 have similar code in similar places.

* bio.c - check
* blk-core.c - check
* blk-lib.c - check
* blk_types.h - check
* blk_dev.h - the include module.h was (re)moved, and unneeded. check
* buffer.c - check
* buffer_head.h - check
* cause_tags.c - check
* cause_tags.h - check
* cfq-iosched.c - cleaned out commented code. This patch is not strictly necessary. It add a little message when you unload cfq that tells you how often it's queues were used. check
* checkpoint.c - The call to jbd2_free_transaction() is now in the vanilla kernel, so no need to patch it in ourselves. I patched the cause tracking in there. check.
* commit.c - Ought to figure out if I need to do anything with the dropped 'SPLIT_JOURNAL_META' cause, due to the reworked code that doesn't juggle multiple journal blocks. Since the regular 'SPLIT_JOURNAL' cause is still there I suspect it's fine. jbd2_free_transaction() is now in the vanilla kernel. A lot SPLIT_DEBUG segments, which mostly activate if you have a disk on /dev/sdf. Maybe remove those?
* elevator.c - elv_dispatch_add_head() is unused in the kernel and modules, so I commented it out. check.
* elevator.h - Removed the spurious elv_dispatch_add_head(). check.
* ext4-inode.c - check
* ext4_jbd2.c - check
* extent_io.c - check
* file.c - check
* filemap.c - check
* fs-ionode.c - Removed spurious SPLIT_NODEP. check
* fsync.c - A lot SPLIT_DEBUG segments. check
* hashtable.h - check
* hrtime.c - check
* init_task.h - check
* jbd2.h - jbd2_free_transaction() is now in vanilla kernel. check
* journal-head.h - check
* journal.c - check
* kthread.c - check
* Makefile - check
* mm_types.h - check
* namei.c - check
* page-io.c - check
* page-writeback.c - check
* read_write.c - check
* revoke.c - check
* sched.h - check
* super.c - check
* sync.c - check
* transaction.c - removed SPLIT_NODEP. Removed commented code. check
* xfs_aops.c - check. Removed patch.
* xfs_inode.c - just comments. check. Removed patch
* xfs_vnodeops.c - removed patch. just comments.