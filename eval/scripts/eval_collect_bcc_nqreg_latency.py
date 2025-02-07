#!/usr/bin/env python3

# @lint-avoid-python-3-compatibility-imports
#
# eval_bcc_nqreg_latency    Quantifying the I/O latency experienced within nqreg.
#
# USAGE: eval_bcc_nqreg_latency.py 
#
# Copyright (c) 2015 Brendan Gregg.
# Licensed under the Apache License, Version 2.0 (the "License")
#
# 20-Sep-2015   Brendan Gregg   Created this.
# 31-Mar-2022   Rocky Xing      Added disk filter support.
# 01-Aug-2023   Jerome Marchand Added support for block tracepoints

from __future__ import print_function
from bcc import BPF
from time import sleep, strftime
import argparse
import ctypes as ct
import os
import sys

MAX_NR_CPU = 64
MAX_NR_NQ = 64

# define BPF program
bpf_text = """
#include <uapi/linux/ptrace.h>
#include <linux/blk-mq.h>
#include <linux/blkdev.h>

#ifdef CONFIG_DAREDEVIL_IO_STACK
#include <linux/blk-blex.h>
#endif

#define TYPE_LOCK   0
#define TYPE_IRQ    1

#define AVE_TYPE_CPU_LOCKING    0
#define AVE_TYPE_CPU_IRQ    1
#define AVE_TYPE_NQ_LOCKING    2
#define AVE_TYPE_NQ_IRQ   3

#define MAX_NR_NQ   128
#define MAX_NR_CPU  128

/* A string compare function from https://github.com/iovisor/bcc/issues/4561 */
static __always_inline int customized_strncmp(const char *cs, const char *ct, int size)
{
    int len = 0;
    unsigned char c1, c2;
    for (len = 0;len < (size & 0xff); len++) {
        c1 = *cs++;
        c2 = *ct++;
        if (c1 != c2) return c1 < c2 ? -1 : 1;
        if (!c1) break;
    }
    return 0;
}

/* The following fields come from the format received from /sys/kernel/tracing/events/blex/xxx/format. */
struct single_rq_tp_args {
    u64 __unused__;
    struct request *req;
    char disk_name[32];
    int tag;
    int nqid;
    unsigned int blex_ioprio;
    unsigned int cpu;
    unsigned long time;
};

struct batch_rq_tp_args {
    u64 __unused__;
    struct request *rqlist_head;
    char disk_name[32];
    int nqid;
    unsigned int cpu;
    unsigned long time;
};

/** The type used for collecting average performance statistics. */
typedef struct average_val {
    u64 total;  /* Total time, in ns, spent. */
    u64 count;  /* Total number of times spent. */
} ave_val_t;

/** The array used for collecting average performance statistics. 
 * Three types in total: 0 --> lock, 1 --> IRQ handling.
 */
BPF_ARRAY(tot_ave_arr, ave_val_t, 2);

/** The array used for collecting average locking & IRQ statistics per CPU. */
BPF_ARRAY(cpu_ave_locking_arr, ave_val_t, MAX_NR_CPU);
BPF_ARRAY(cpu_ave_irq_arr, ave_val_t, MAX_NR_CPU);

/** The array used for collecting average locking & IRQ statistics per NQ. */
BPF_ARRAY(nq_ave_locking_arr, ave_val_t, MAX_NR_NQ);
BPF_ARRAY(nq_ave_irq_arr, ave_val_t, MAX_NR_NQ);

static void __trace_update_total_average_array(u32 type_index, u64 delta)
{
    ave_val_t *ave_val = tot_ave_arr.lookup(&type_index);
    if (ave_val){
        lock_xadd(&ave_val->total, delta);
        lock_xadd(&ave_val->count, 1);
    }
}

static void __trace_update_sub_average_array(int sub_ave_arr_type, unsigned int cpu, int nqid, u64 delta)
{
    ave_val_t *ave_val;

    switch (sub_ave_arr_type)
    {
        case AVE_TYPE_CPU_LOCKING:
            ave_val = cpu_ave_locking_arr.lookup(&cpu);
            break;
        case AVE_TYPE_CPU_IRQ:
            ave_val = cpu_ave_irq_arr.lookup(&cpu);
            break;
        case AVE_TYPE_NQ_LOCKING:
            ave_val = nq_ave_locking_arr.lookup(&nqid);
            break;
        case AVE_TYPE_NQ_IRQ:
            ave_val = nq_ave_irq_arr.lookup(&nqid);
            break;
        default:
            break;
    }
    if (ave_val){
        lock_xadd(&ave_val->total, delta);
        lock_xadd(&ave_val->count, 1);
    }
}

/* BPF structures and types for locking latency. */

struct lock_key {
    char disk_name[32];
    int req_tag;   /* Used for both single and batched */
    int nqid;   /* The contended NQ */
    unsigned int cpu;    /* The CPU involved. */
};

typedef struct cpu_key {
    u64 cpu;
    u64 slot;
} cpu_key_t;

typedef struct nq_key {
    u64 nqid;
    u64 slot;
} nq_key_t;

BPF_HASH(lock_map, struct lock_key);

BPF_HISTOGRAM(lock_time_hist);  /* The histogram to store lock contention time. */
BPF_HISTOGRAM(lock_cpu_hist, cpu_key_t); /* The histogram to store lock contention time within each cpu. */
BPF_HISTOGRAM(lock_nq_hist, nq_key_t);  /* The histogram to store lock contention time within each NQ. */

/* BPF structures and types for IRQ handling latency. */

struct irq_key {
    char disk_name[32];
    int req_tag;
    int nqid;
};

BPF_HASH(irq_map, struct irq_key);

BPF_HISTOGRAM(irq_time_hist);  /* The histogram to store IRQ handling time. */
BPF_HISTOGRAM(irq_nq_hist, nq_key_t); /* The histogram to store IRQ handling time within each NQ. */

/* Functions */
static void __trace_update_lock_histogram(unsigned int cpu, int nqid, u64 delta)
{
    cpu_key_t ckey = {
        .cpu = cpu,
        .slot = bpf_log2l(delta)
    };

    nq_key_t nkey = {
        .nqid = nqid,
        .slot = bpf_log2l(delta)
    };

    lock_time_hist.atomic_increment(bpf_log2l(delta)); // update locking histogram
    lock_cpu_hist.atomic_increment(ckey);   // update per-cpu locking time histogram
    lock_nq_hist.atomic_increment(nkey);    // update per-nq locking time histogram
}

static void __trace_update_irq_histogram(int nqid, u64 delta)
{
    nq_key_t nkey = {
        .nqid = (u64)nqid,
        .slot = bpf_log2l(delta)
    };
    irq_time_hist.atomic_increment(bpf_log2l(delta));
    irq_nq_hist.atomic_increment(nkey);
}

/* Tracing functions */

static int __trace_lock_acquired(struct lock_key key)
{
    u64 *tsp, delta;

    DISK_FILTER

    tsp = lock_map.lookup(&key);

    if (tsp == NULL)
        return 0;   // missed issue

    delta = bpf_ktime_get_ns() - *tsp;

    __trace_update_lock_histogram(key.cpu, key.nqid, delta);
    __trace_update_total_average_array(TYPE_LOCK, delta);
    __trace_update_sub_average_array(AVE_TYPE_CPU_LOCKING, key.cpu, 0, delta);
    __trace_update_sub_average_array(AVE_TYPE_NQ_LOCKING, 0, key.nqid, delta);
    lock_map.delete(&key);

    return 0;
}

static int __trace_try_lock(struct lock_key key)
{
    DISK_FILTER

    u64 ts = bpf_ktime_get_ns();
    lock_map.update(&key, &ts);

    return 0;
}

/* ebpf probes attached to single-request tps. */
int trace_single_rq_try_lock_tp(struct single_rq_tp_args *args)
{
    struct lock_key key = {
        .req_tag = args->tag,
        .nqid = args->nqid,
        .cpu = args->cpu
    };
    int ret;

    ret = bpf_probe_read_kernel_str(key.disk_name, 32, args->disk_name);
    if (ret < 0)
        return ret;

    return __trace_try_lock(key);
}

int trace_single_rq_lock_acquire_tp(struct single_rq_tp_args *args)
{
    struct lock_key key = {
        .req_tag = args->tag,
        .nqid = args->nqid,
        .cpu = args->cpu
    };
    int ret;

    ret = bpf_probe_read_kernel_str(key.disk_name, 32, args->disk_name);
    if (ret < 0)
        return ret;

    return __trace_lock_acquired(key);
}

static int __trace_rq_compl(struct irq_key key)
{
    DISK_FILTER
    u64 ts = bpf_ktime_get_ns();
    irq_map.update(&key, &ts); 
    return 0;
}

int trace_single_rq_compl_irq_tp(struct single_rq_tp_args *args)
{
    struct irq_key key = {
        .req_tag = args->tag,
        .nqid = args->nqid
    };
    int ret = bpf_probe_read_kernel_str(key.disk_name, 32, args->disk_name);
    if (ret < 0)
        return ret;
    return __trace_rq_compl(key);
}

static int __trace_rq_compl_done(struct irq_key key)
{
    u64 *tsp, delta;
    DISK_FILTER

    tsp = irq_map.lookup(&key);
    if (tsp == 0)
        return 0;

    delta = bpf_ktime_get_ns() - *tsp;
    __trace_update_irq_histogram(key.nqid, delta);
    __trace_update_total_average_array(TYPE_IRQ, delta);
    __trace_update_sub_average_array(AVE_TYPE_CPU_IRQ, bpf_get_smp_processor_id(), 0, delta);
    __trace_update_sub_average_array(AVE_TYPE_NQ_IRQ, 0, key.nqid, delta);
    return 0;
}
int trace_single_rq_compl_done_tp(struct single_rq_tp_args *args)
{
    
    struct irq_key key = {
        .req_tag = args->tag,
        .nqid = args->nqid
    };
    int ret;

    ret = bpf_probe_read_kernel_str(key.disk_name, 32, args->disk_name);
    if (ret < 0)
        return ret;

    return __trace_rq_compl_done(key);
}

/* ebpf probes attached to batch-request tps. */
int trace_batch_rq_try_lock_tp(struct batch_rq_tp_args *args)
{
    struct lock_key key = {
        .req_tag = 2048,
        .nqid = args->nqid,
        .cpu = args->cpu
    };
    int ret;

    ret = bpf_probe_read_kernel_str(key.disk_name, 32, args->disk_name);
    if (ret < 0)
        return ret;
    

    return __trace_try_lock(key);
}

int trace_batch_rq_lock_acquire_tp(struct batch_rq_tp_args *args)
{
     struct lock_key key = {
        .req_tag = 2048,
        .nqid = args->nqid,
        .cpu = args->cpu
    };
    int ret;

    ret = bpf_probe_read_kernel_str(key.disk_name, 32, args->disk_name);
    if (ret < 0)
        return ret;

    return __trace_lock_acquired(key);
}
"""    

# Preliminary preparation for the BPF program
parser = argparse.ArgumentParser(description="The tool used for collecting I/O statistics via eBPF program.")
parser.add_argument("--disk", type=str, required=True, help="The specific disk name to collect performance data..")
parser.add_argument("--duration", type=int, help="The running duration of the eBPF program, in seconds.")
parser.add_argument("--interval", type=int, help="The interval period for printing out the information, in seconds.")
parser.add_argument("--result-file", type=str, help="The target file to store the results.")
args = parser.parse_args()

# Add the disk filtering
target_disk = args.disk
disk_filter_str = f"""
    if (customized_strncmp(key.disk_name, "{target_disk}", 32) != 0){{
        bpf_trace_printk("Comparing '%s' against given disk name '{target_disk}' fails.\\n", key.disk_name);
        return 0;
    }}
"""

# Tick and print interval
TICK = 1
INTERVAL = 999999999
if args.interval:
    INTERVAL = args.interval

bpf_text = bpf_text.replace('DISK_FILTER', disk_filter_str)

# The main program for loading the ebpf program
b  = BPF(text=bpf_text)

# single-request tracepoints
b.attach_tracepoint(tp="blex:blex_nvme_try_hold_lock_single_rq", fn_name="trace_single_rq_try_lock_tp")
b.attach_tracepoint(tp="blex:blex_nvme_lock_acquired_single_rq", fn_name="trace_single_rq_lock_acquire_tp")
b.attach_tracepoint(tp="blex:blex_nvme_compl_irq_single_rq", fn_name="trace_single_rq_compl_irq_tp")
b.attach_tracepoint(tp="blex:blex_compl_done_single_rq", fn_name="trace_single_rq_compl_done_tp")

# batched-requests tracepoints
b.attach_tracepoint(tp="blex:blex_nvme_try_hold_lock_batched_rqs", fn_name="trace_batch_rq_try_lock_tp")
b.attach_tracepoint(tp="blex:blex_nvme_lock_acquired_batched_rqs", fn_name="trace_batch_rq_lock_acquire_tp")

# Arrays
tot_ave_array = b.get_table("tot_ave_arr") # The array containing the average latency statistics of all requests in locking [0] and IRQ [1].

## CPU arrays
cpu_ave_locking_array = b.get_table("cpu_ave_locking_arr") # The array container average latency in locking across all CPUs, indexed by [cpu_id]
cpu_ave_irq_array = b.get_table("cpu_ave_irq_arr") # The array container average latency in IRQ across all CPUs, indexed by [cpu_id]

## NQ arrays
nq_ave_locking_array = b.get_table("nq_ave_locking_arr") # The array container average latency in locking across all NQs, indexed by [nqid]
nq_ave_irq_array = b.get_table("nq_ave_irq_arr") # The array container average latency in IRQ across all NQs, indexed by [nqid]

# HISTOGRAMs
## lock histograms
lock_time_hist = b.get_table("lock_time_hist")
lock_cpu_hist = b.get_table("lock_cpu_hist")
lock_nq_hist = b.get_table("lock_nq_hist")

## irq histograms
irq_time_hist = b.get_table("irq_time_hist")
irq_nq_hist = b.get_table("irq_nq_hist")

def print_average_array_info(table, index):
    total = table[index].total
    count = table[index].count
    if count > 0:
        print("avg = %ld ns, total: %ld ns, count: %ld\n" %
              (total / count, total, count))
    else:
        print("avg = nil ns, total: nil ns, count: nil\n")

def print_stats():
    count_down = int(args.duration)
    do_exit = False
    interval = INTERVAL
    report_index = 0
    while True:
        try:
            sleep(TICK)
        except KeyboardInterrupt:
            do_exit = True
        
        count_down -= 1
        
        if count_down == 0:
            do_exit = True
        
        interval -= 1

        if interval == 0:
            interval = INTERVAL
            report_index += 1
        else:
            if not do_exit:
                continue

        print(f"\n************************START OF STATS REPORT {report_index}*************************\n**************************************************")
        print("%-8s\n" % strftime("%H:%M:%S"), end="")

        print(f"---------Total Average Timing Stats---------------\n")
        print(f"Total average latency in **LOCKING**:")
        print_average_array_info(tot_ave_array, 0)
        print(f"Total average latency in **IRQ**:")
        print_average_array_info(tot_ave_array, 1)
        print(f"==================================================\n")

        print(f"---------Per-CPU Average Timing Stats-----------------")
        for cpu in range(MAX_NR_CPU):
            print(f"\nCPU ID: {cpu} stats:\n")
            print(f"LOCKING:")
            print_average_array_info(cpu_ave_locking_array, cpu)
            print(f"IRQ:")
            print_average_array_info(cpu_ave_irq_array, cpu)
        print(f"==================================================\n")

        print(f"---------Per-NQ Average Timing Stats-----------------")
        for nqid in range(1, MAX_NR_NQ + 1):
            print(f"\nNQID: {nqid} stats:\n")
            print(f"LOCKING:")
            print_average_array_info(nq_ave_locking_array, nqid)
            print(f"IRQ:")
            print_average_array_info(nq_ave_irq_array, nqid)
        print(f"==================================================\n")

        print(f"------------------**LOCKING** HISTOGRAMS------------------------")
        print(f"\n**Total** in-lock latency histogram:\n")
        lock_time_hist.print_json_hist("nsecs", "overall")
        lock_time_hist.print_log2_hist("nsecs", "overall")

        print(f"\n**Per-CPU** in-lock latency histogram:\n")
        lock_cpu_hist.print_json_hist("nsecs", "CPU ID")
        lock_cpu_hist.print_log2_hist("nsecs", "CPU ID")

        print(f"\n**Per-NQ** in-lock latency histogram:\n")
        lock_nq_hist.print_json_hist("nsecs", "nqid")
        lock_nq_hist.print_log2_hist("nsecs", "nqid")
        print(f"==================================================\n")
        
        print(f"------------------**IRQ** HISTOGRAMS------------------------")
        print(f"\n**Total** in-IRQ latency histogram:\n")
        irq_time_hist.print_json_hist("nsecs", "overall")
        irq_time_hist.print_log2_hist("nsecs", "overall")

        print(f"\n**Per-NQ** in-IRQ latency histogram:\n")
        irq_nq_hist.print_json_hist("nsecs", "nqid")
        irq_nq_hist.print_log2_hist("nsecs", "nqid")
        print(f"==================================================\n")
        print(f"*************************END OF STATS REPORT {report_index}*************************\n**************************************************\n")

        if do_exit:
            exit()
    
if args.result_file:
    with open(args.result_file, 'w') as fd:
        sys.stdout = fd
        print_stats()
    sys.stdout = sys.__stdout__
else:
    print_stats()
    
    
    