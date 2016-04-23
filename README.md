# split-io-scheduler
Back/forward ported patches based on https://research.cs.wisc.edu/adsl/Software/split/

# Build instructions

Apply the patches for your kernel version:

```
cd your-kernel-tree
for i in <this path>/patches/<kernel version>/*.patch*; do patch -p1 < $i; done
```

or:

```
quilt import <this path>/patches/<kernel version>/*patch*
quilt push -a
```

Copy config_yangsuli (salvaged from release tarball) to .config (note the dot). This config does not boot a default Ubuntu 12.04.0 (precise) install, try the config_ubuntu* instead.
`make menuconfig`, and then make necessary changes in configuration. For example it is required to compile BTRFS into the kernel (=y), build will fail as module. This is already set in the ubuntu config

```
make -j4 (based on how many cores you have)
##make modules -j4
#sudo make modules_install
#sudo make install
fake_root make-kpkg -j4 --initrd --append-to-version=-split-level-io --revision=1 kernel_image kernel_headers
sudo dpkg -i ../*-split-level-io*1*.deb
sudo reboot
```

# Modules
The modules/ directory contains the kernel module sources of the actual schedulers. The makefile depends on that you run the split-level-io kernel.

```
cd modules
make
make insert (inserts just afq as example)
```

After initial compiling, if you updated some code in the kernel:

make -j4
##make modules -j4
#sudo make modules_install
#sudo make install
fakeroot make-kpkg -j4 --initrd --append-to-version=-split-level-io --revision=2 kernel_image kernel_headers
sudo dpkg -i ../*-split-level-io*2*.deb
sudo reboot

If you want to install updated header files (e.g., because you added a syscall)
sudo make headers_install INSTALL_HDR_PATH=/usr/include

# Notes about the code
* include/linux/hashtable.h is backported from Linux 3.7 (or later)
* The variable expire_rb_node is used in the modules, not in the kernel
* xfs_vnodeops.c.patch just contains a comment about dirtying data
* variable i_private1 probably needs a better comment
