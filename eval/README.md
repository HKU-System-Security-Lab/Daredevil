# Evaluation

## Daredevil Configurations
This sub-directory contains scripts used to conduct the evaluation described in our EuroSys'25 paper.
Daredevil uses several run-time configurable parameters to guide its behaviors in providing I/O services.
To customize Daredevil to your working environment and tune its performance, you may consider changing these parameters accordingly.
These parameters can be seen via:

```bash
$ ls /sys/module/blex/parameters

$ ls /sys/module/nvme/parameters
```

* **daredevil_blex_opt_level**: 

    Daredevil optimization levels at the block layer. 
    "+1": blex + round-robin request routing (dare-base).
    "+2": + NQ scheduling & request routing (dare-sched).

* **daredevil_io_profile_on**:

    Enable profiling the I/O pattern of tenants/processes.

* **blex_sched_merit_alpha_numer/blex_sched_merit_alpha_denom**:
    
    The numerator/denominator for the $\alpha$ value used to calculate the merits of tenants/processes.

* **blex_prio_arr_mru**:

    The Most-Recently-Used (MRU) value used in NQ scheduling.

* **daredevil_compl_dispatch_on**:

    Enable SLA-aware I/O service dispatching.

## Comparison Targets

We have compared Daredevil with blk-switch (OSDI'21) and the vanilla Linux kernel.
Because blk-switch was originally implemented on Linux kernel v5.4.43, we have carefully migrated it to the Linux kernel v6.1.53, preserving its core functionalities described in its paper.
The migrated source codes of blk-switch is available at [eval/blk-switch](./blkswitch).

Setting up these targets following the same procedure of setting up Daredevil, as detailed in [this doc](../README.md).

```bash
$ pushd workdir/Daredevil/eval
$ wget https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/snapshot/linux-6.1.53.tar.gz
$ tar xzvf linux-6.1.53.tar.gz

# Prepare the Linux kernel source codes for blk-switch and vanilla
$ mv linux-6.1.53 linux-blkswitch
$ cp -r linux-blkswitch linux-vanilla

# Set up blk-switch
$ cp -rf blkswitch/block blkswitch/include linux-blkswitch
```

## Evaluation

The scripts to run the experiments described in the paper are provided in [scripts](./scripts/).
Some of these scripts rely on third-party applications, such as RocksDB and YCSB, which are added to this repo as submodules.
You need to initialize them before running the scripts.

```bash
pushd workdir/Daredevil/eval/lib
git submodule update --init --recursive
popd
```

After initialization, please build the third-party applications according to the instructions given in their documentation, which is not described in this doc.

With configured comparison targets and compiled third-party applications, you are now able to use the following scripts to run the experiments described in our paper. The purpose and functionality of these scripts are detailed below:

- **scripts/eval_single_ns_incr_T_tenant.sh**:

    The script used to increase the severity of the multi-tenancy issue by continuously increasing the number of T-tenants, using *only one* namespace.
    The results collected from this script corresponding to Figure 6-9.

    Usage:

    ```bash
    $ bash eval_single_ns_incr_T_tenant.sh --sys [your comparison target]
    ```

- **scripts/eval_multi_ns_incr_T_tenant.sh**:

    The script used to evaluate system's performance using *multiple* namespaces (corresponding to Figure 10).

    Usage:

    ```bash
    $ bash eval_multi_ns_incr_T_tenant.sh --sys [your comparison target]
    ```

- **scripts/eval_filebench_mailserver.sh**:

    The script used to run the mailserver workload of filebench (corresponding to Figure 12).

    Usage:

    ```bash
    $ bash eval_filebench_mailserver.sh --sys [your comparison target]
    ```

- **scripts/eval_rocksdb_ycsb.sh**:

    The script used to run YCSB benchmarks on RocksDB (corresponding to Figure 12).

    Usage

    ```bash
    $ bash eval_rocksdb_ycsb.sh --sys [your comparison target]
    ```


- **scripts/eval_collect_bcc_nqreg_latency.py**:

    The script used to probe and collect performance statistics from the eBPF tracing points enabled by CONFIG_BLK_DAREDEVIL_PERF_TP (corresponding to Fig. 13).

    Usage:

    ```bash
    $ pushd workdir/Daredevil/eval
    $ cp scripts/eval_collect_bcc_nqreg_latency.py lib/bcc/tools
    $ pushd lib/bcc/tools
    $ python3 eval_collect_bcc_nqreg_latency.py --disk [NVMe disk name] --duration [duration] --interval [interval to print perf statistics] --result-file [the file to store results]
    ```


- **scripts/eval_change_ionice_freq.sh**:

    The script used to change ionice values of tenants with different frequencies (corresponding to Fig. 14).

    Usage:

    ```bash
    $ bash eval_change_ionice_freq.sh --sys [your comparison target] --tag [your customized tag for this experiment]
    ```


