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
##make modules -j`nproc`
#sudo make modules_install
#sudo make install
fake_root make-kpkg -j`nproc` --initrd --append-to-version=-split-level-io --revision=1 kernel_image kernel_headers
sudo dpkg -i ../*-split-level-io*1*.deb
sudo reboot
```

## Modules
The modules/ directory contains the kernel module sources of the actual schedulers. The Makefile depends on that you run the split-level-io kernel. 

```
cd modules
make
make insert (inserts just afq as example)
```

After initial compilation, if you updated some code in the kernel:

```
make -j`nproc`
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

* include/linux/hashtable.h is backported from Linux 3.7 (or later)
* The variable expire_rb_node is used in the modules, not in the kernel, gives build warning
* xfs_vnodeops.c.patch just contains a comment about dirtying data
* variable i_private1 probably needs a better comment
* Fixed in 3.4 patch: printf misuse of %d for a size_t complaint during build (should be %zu)
* Lost track of one of the 'here dirty' *comments* in fs/xfs/xfs_inode.c in Linux v3.5. Ah well..

## TODO

* Change modules/Makefile so you can also build the modules before booting the kernel.
* Merge patches together into sensible patchsets, instead of per file
* Fix kernel compilation linking failure with BTRFS / Ext4 / XFS as modules.
* Test if kernels also boot O:-)
* Do regression testing with SibylFS if the extra locking does not introduce bugs: https://sibylfs.github.io/
* Check that the kernel thread changes do not introduce problems, with KernelThreadSanitizer (Linux 4.x): https://github.com/google/ktsan
* Maybe cleanup all the commented printk() in block/cfq-iosched.c & fs/btrfs/file.c
* Why is cfq_latency = 0 in block/cfq-iosched.c (instead of vanilla default 1)
* Maybe remove large "SAMER" comment in fs/btrfs/extent_io.c
* Maybe remove the request_sanity_check() in fs/btrfs/extent_io.c. Thought it was used in modules/tbucket.c, but it's not.
* BUG? It feels to me like the sched_uniq++ construct in block/elevator.c will not work properly in the Linux v3.5 patch. I suspect it was supposed to give every new loaded I/O scheduler (elevator) a new unique ID. From what I can see it always either becomes 1000, 1001, or 1. Because it doesn't track an atomic incrementing counter elsewhere. It just writes on newly created elevator queues. But maybe those inherit pretty much everything from the previous one (?)
* Since SPLIT_NODEP is not defined, maybe remove the #ifndef SPLIT_NODEP / #endif checks.

## Order of back/forward porting

I 'might not' have put changes I've done in later patches back into the older patches.

* 3.2.51 - original, from tarball, excluded some cruft. Boots.
* 3.2 - also has trailing whitespace fixes. Compiles. Not tested.
* 3.3 - elevator->elevator_type to elevator->type, and a fixup around q->sched_uniq in elevator initialization/switching, some whitespace fixes. Compiles, 'section mismatch' warnings in some modules are not caused by this patch. Not tested.
* 3.4 - Ext4 (jbd2) now also has it's own function to free a transaction, moved the causes list admin call there. Should check if other filesystems also did this change, and I properly added the causes tracking there too. Compiles. 'section mismatch' warning is still not caused by our code. Not tested.
* 3.5 - lost the insertion point of a *comment* in fs/xfs/xfs_inode.c. Seems that there is nothing that 'dirties' anything after that point anymore. Not tested.
* 3.6 - Seemed straightforward. Not tested.
* 3.7 - Seemed straightforward. Not tested.
