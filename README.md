# Daredevil: Rescue Your Flash Storage from Inflexible Kernel Storage Stack

## Introduction

Daredevil is a general kernel storage stack to enable flexible multi-tenancy control within the Linux kernel.
It enables unconstrained I/O paths between CPU cores and NVMe I/O queues (NQs) by decoupling the original static structure of blk-mq and performing scheduling accordingly.

We have implemented and integrated Daredevil into the kernel storage stack based on the Linux kernel v6.1.53. 
This means that the only changes are made to the Linux kernel and thus no specific changes are required for user-space applications.
You can effortlessly run your applications in an environment with Daredevil as the bottom level storage stack.

We have tested Daredevil in our lab server and have successfully deployed it for multiple months.
However, please note that manipulating with the Linux kernel is dangerous and **Daredevil is a research-oriented prototype**, which means that it is not mature enough to handle all unexpected cases in production.
Therefore, we suggest that you **take extra caution** when actually trying to use or deploy Daredevil in your own environments.

> Note: Daredevil only affects local NVMe SSDs (i.e., attached to the host via the PCIe bus). You can rest assured for your storage devices of other types, such as SATA SSDs or remote NVMe SSDs (e.g., attached via NVMe-oF). 

This repository delivers the source codes of Daredevil, along with the evaluation scripts used to conduct the experiments as described in our EuroSys'25 paper.
The layout of the source code tree is described below:

```bash
.                                                                                                                                                   
├── daredevil # Daredevil source codes
│   ├── block/
│   ├── drivers/
│   └── include/
├── eval                              
│   ├── blkswitch/  # Source codes of migrated blkswitch
│   ├── common.sh
│   ├── eval_filebench_test_cont.sh
│   ├── eval_fio_cont_test_base.sh
│   ├── eval_fio_per_host_test_base.sh
│   ├── eval_rocksdb_yscb_test_base.sh
│   ├── lib # Third-party applications required to run evaluation scripts
│   │   ├── bcc/
│   │   ├── filebench/
│   │   ├── rocksdb/
│   │   └── YCSB/
│   ├── manage_nvme_ns.sh             
│   ├── README.md                     
│   ├── reset_nvme_ns.sh              
│   └── scripts/    # Scripts to evaluate Daredevil and other comparison targets 
└── README.md
```

Please consider citing our paper from EuroSys'25 if you use Daredevil.

```latex
@inproceedings{eurosys25-daredevil,
author = {Li, Junzhe and Shu, Ran and Lin, Jiayi and Zhang, Qingyu and Yang, Ziyue and Zhang, Jie and Xiong, Yongqiang and Qian, Chenxiong},
title = {Daredevil: Rescue Your Flash Storage from Inflexible Kernel Storage Stack},
year = {2025},
isbn = {9798400711961},
publisher = {Association for Computing Machinery},
address = {New York, NY, USA},
url = {https://doi.org/10.1145/3689031.3717482},
doi = {10.1145/3689031.3717482},
booktitle = {Proceedings of the Twentieth European Conference on Computer Systems},
pages = {991–1008},
numpages = {18},
keywords = {Linux kernel, Solid-state drives, Storage systems},
location = {Rotterdam, Netherlands},
series = {EuroSys '25}
}
```

> **NOTE**: we are currently restructuring the Daredevil source code to make it more compact for future releases. Please contact jzzzli@connect.hku.hk for any questions.

## System Setup

### System Configurations

As described in our paper, our prototype has been tested in our lab server with the configurations shown below:

- Ubuntu 22.04 LTS
- 3.2TB Samsung PM1735 NVMe SSD and 2TB Samsung 980Pro NVMe SSD

That said, Daredevil does not require particular software or hardware configurations for deployment. 
You can use Daredevil at your will for environments using the Linux kernel with NVMe SSDs equipped.

### Preparing the Kernel Image

#### Step 1: Prepare the package dependency needed to compiler the Linux kernel

```bash
$ sudo apt-get install libncurses-dev gawk flex bison openssl libssl-dev dkms libelf-dev libudev-dev libpci-dev libiberty-dev autoconf
```


#### Step 2: Obtain the source codes of the Linux kernel version that Daredevil is built upon and the Daredevil source codes.
> Note: for the rest of this doc, we assume that your working directory is named as 'workdir'

```bash
$ cd workdir

# Fetch the Daredevil source codes
$ git clone https://github.com/HKU-System-Security-Lab/Daredevil

# Dive into the repository of Daredevil
$ pushd Daredevil

# Fetch the Linux kernel source codes
$ wget https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/snapshot/linux-6.1.53.tar.gz
$ tar xzvf linux-6.1.53.tar.gz
$ mv linux-6.1.53 linux-daredevil

# Update the Linux kernel to use Daredevil
$ cp -rf daredevil/block daredevil/include daredevil/drivers linux-daredevil

```

> Note: we assume that you are working under workdir/Daredevil hereafter.

#### Step 3: Configure and compile the kernel

Daredevil adds three new options to the Linux kernel:

1. CONFIG_DAREDEVIL_IO_STACK

    This option enables the Daredevil storage stack to function, i.e., the decoupled blex block layer, request routing, and NQ scheduling.

2. CONFIG_DAREDEVIL_IO_STACK_EXTRAQ

    This option depends on CONFIG_DAREDEVIL_IO_STACK and enables more queues than originally available in blk-mq (the Linux kernel originally caps the number of NQs to no more than the CPU core count). 

3. CONFIG_BLK_DAREDEVIL_PERF_TP

    This option adds eBPF tracing points to the critical function calls to enable user-space performance probing.

To properly integrate the Daredevil, we need to configure the Linux kernel accordingly, using the following commands.

```bash
# Use the config file from your working environment for the kernel
$ cp /boot/config-$(uname -r) linux-daredevil/.config 

# Configure the Linux kernel to enable Daredevil
$ pushd linux-daredevil/

$ ./scripts/config -e \
    \   # Enable configurations required to enable Daredevil
    -e CONFIG_DAREDEVIL_IO_STACK \
    -e CONFIG_DAREDEVIL_IO_STACK_EXTRAQ \
    -e CONFIG_BLK_DAREDEVIL_PERF_TP    \
    \   # Enable auxiliary configurations 
    -e CONFIG_BLOCK    \
    -e CONFIG_PCI_MSI  \
    -e CONFIG_BLK_DEV_NVME \
    \   # Name the local version to distinguish it from your install kernels
    --set-str CONFIG_LOCALVERSION "-daredevil"

$ make olddefconfig # Adapt the kernel configurations to the working environment

# Compile the Linux kernel
$ make -j $(nproc)
$ file arch/x86_64/boot/bzImage # Assuming that your target architecture is x86_64
```

### Boot the Daredevil-enable Linux Kernel

There are two ways to experiment with the Daredevil-enable Linux kernel: booting up the modified kernel on your bare-metal machine, or using QEMU simulator to boot up the kernel in a virtual machine.
The former method measures the actual performance of Daredevil as it directly operates on your bare metal mechine but comes at the cost of incurring potential instability to your working environment.
The latter method, on the other hand, allows you to conveniently test and experiment with the functionality of Daredevil, without the need to worry about crashing your machine, but fails to reflect the real-world performance of Daredevil.

In this regard, we suggest that you take extra consideration when choosing between the aforementioned methods.

#### Option 1: Booting on your bare-metal machine

This method is simple to employ. Just install your compiled kernel and reboot your machine.

```
$ sudo make modules_install
$ sudo make headers_install
$ sudo make install
$ ls /boot  # You will find multiple files matching "*-6.1.53-daredevil*"
$ sudo systemctl reboot # Reboot to the recently install kernel
```

#### Option 2: Booting on your QEMU virtual machine with KVM support

To enable KVM support, run the following command in addition to the configuration process described in step 3 of [Preparing the Kernel Image](#preparing-the-kernel-image).

```bash
# Assuming that you are working under workdir/Daredevil/linux-daredevil
$ make kvm_guest.config
```

Then, set up your QEMU virtual machine. We choose not to elaborate on this step as there are multiple online instructions and tutorials for your reference.
For instance, you may refer to [this link](https://rs3lab.github.io/TCLocks/docs/artifact.html#how-to-create-the-disk-image) for creating your Ubuntu disk image and [this link](https://github.com/google/syzkaller/blob/master/docs/linux/setup.md#vm-setup) for the most basic setup of your QEMU VM.

Use the following command to boot Daredevil on your QEMU VM (customize the variables marked with "$[]" to your working environment):

```bash
$ qemu-system-x86_64 \
        -drive file=$[your-Ubuntu-disk-image],format=qcow2 \
        -enable-kvm \
        -m 16G \
        -smp cores=32   \
        -cpu host       \
        -kernel workdir/Daredevil/linux-daredevil/arch/x86_64/boot/bzImage \
        -append "root=$[the boot partition of your Ubuntu disk image] console=ttyS0 nokaslr " \
        -nographic  \
        \
        -drive file=sda.img,if=none,format=qcow2 \
        \    # Add NVMe SSD simulation to the QEMU VM
        -drive file=nvme.img,if=none,id=nvm1 \
        -device nvme,serial=deadbeef1,drive=nvm1,logical_block_size=512,max_ioqpairs=128 \
        \   # (optional) The following commands adds NVMe namspace simulation
        -drive file=nvme-ns-1.img,if=none,id=nvme-ns-1  \
        -device nvme-ns,drive=nvme-ns-1 \
        -drive file=nvme-ns-2.img,if=none,id=nvme-ns-2  \
        -device nvme-ns,drive=nvme-ns-2
```

## Run

Please refer to [this doc](./eval/README.md) for detailed instructions.

