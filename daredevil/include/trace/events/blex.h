#undef TRACE_SYSTEM
#define TRACE_SYSTEM blex

#if !defined(_TRACE_BLEX_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_BLEX_H

#include <linux/tracepoint.h>

#ifdef CONFIG_BLK_DAREDEVIL_PERF_TP /* BLK_DAREDEVIL_PERF_TP ENABLED*/
#include <linux/blk-mq.h>
#include <linux/blk_types.h>
#include <linux/blkdev.h>

#ifdef CONFIG_DAREDEVIL_IO_STACK
#include <linux/blk-blex.h>
#endif

/* Tracepoints in the nqreg component.
 * Used in the drivers/nvme/ module.
 */

/* Tracepoint class used for single request operations, e.g., nvme_queue_rq(). */
DECLARE_EVENT_CLASS(blex_nqreg_single_rq_tp,
    
    TP_PROTO(struct request *req, int nqid, int cpu, unsigned long time),
    
    TP_ARGS(req, nqid, cpu, time),
    
    TP_STRUCT__entry(
        __field(    struct request *,   req)
        __array(char,   disk_name,  DISK_NAME_LEN)
        __field(    int,    tag)
        __field(    int,    nqid)
        __field(    unsigned int,   blex_ioprio)
        __field(    unsigned int,   cpu)
        __field(    unsigned long,  time)
    ),

    TP_fast_assign(
        __entry->req = req;
        blex_trace_cpy(__entry->disk_name, req, DISK_NAME_LEN);
        __entry->tag = req->tag;
        __entry->blex_ioprio = ((req->rq_flags >> 25) & 0x3); // Mimic the blk_blex_get_req_prio() in include/linux/blk-blex.h
        __entry->cpu = cpu;
        __entry->nqid = nqid;
        __entry->time = time;
    ),

    TP_printk("req=%p: time(ns)=%lu, disk=%s, tag=%d, ioprio=%u, cpu=%u, nqid=%d",
        __entry->req, __entry->time, __entry->disk_name, __entry->tag,
        __entry->blex_ioprio, __entry->cpu, __entry->nqid)
);

/* Trying to grab the nvmeq->spin_lock in queue_rq(). */
DEFINE_EVENT(blex_nqreg_single_rq_tp,
    blex_nvme_try_hold_lock_single_rq,
    TP_PROTO(struct request *req, int nqid, int cpu, unsigned long time),
    TP_ARGS(req, nqid, cpu, time));

/* Holding the nvmeq->spin_lock in queue_rq(). */
DEFINE_EVENT(blex_nqreg_single_rq_tp,
    blex_nvme_lock_acquired_single_rq,
    TP_PROTO(struct request *req, int nqid, int cpu, unsigned long time),
    TP_ARGS(req, nqid, cpu, time));

/* The completion IRQ fires off. */
DEFINE_EVENT(blex_nqreg_single_rq_tp,
    blex_nvme_compl_irq_single_rq,
    TP_PROTO(struct request *req, int nqid, int cpu, unsigned long time),
    TP_ARGS(req, nqid, cpu, time));

/* The completion handled by block layer. */
DEFINE_EVENT(blex_nqreg_single_rq_tp,
    blex_compl_done_single_rq,
    TP_PROTO(struct request *req, int nqid, int cpu, unsigned long time),
    TP_ARGS(req, nqid, cpu, time));


/* Tracepoint class used for batched request operations, e.g., nvme_submit_cmds() */
DECLARE_EVENT_CLASS(blex_nqreg_batched_rqs_tp,
    
    TP_PROTO(struct request **rqlist, int nqid, int cpu, unsigned long time),
    
    TP_ARGS(rqlist, nqid, cpu, time),
    
    TP_STRUCT__entry(
        __field(    struct request *, rqlist_head)
        __array(char,   disk_name,  DISK_NAME_LEN)
        __field(    int,    nqid)
        __field(    unsigned int,   cpu)
        __field(    unsigned long,  time)
    ),

    TP_fast_assign(
        __entry->rqlist_head = rq_list_peek(rqlist);
        blex_trace_cpy(__entry->disk_name, __entry->rqlist_head, DISK_NAME_LEN);
        __entry->nqid = nqid;
        __entry->cpu = cpu;
        __entry->time = time;
    ),

    TP_printk("rqlist_head=%p: disk=%s, time(ns)=%lu, cpu=%u, nqid=%d",
        __entry->rqlist_head, __entry->disk_name, __entry->time, __entry->cpu, __entry->nqid)
);

/* Trying to grab the nvmeq->spin_lock in submit_cmds(). */
DEFINE_EVENT(blex_nqreg_batched_rqs_tp,
    blex_nvme_try_hold_lock_batched_rqs,
    TP_PROTO(struct request **rqlist, int nqid, int cpu, unsigned long time),
    TP_ARGS(rqlist, nqid, cpu, time));

/* Holding the nvmeq->spin_lock in queue_rq(). */
DEFINE_EVENT(blex_nqreg_batched_rqs_tp,
    blex_nvme_lock_acquired_batched_rqs,
    TP_PROTO(struct request **rqlist, int nqid, int cpu, unsigned long time),
    TP_ARGS(rqlist, nqid, cpu, time));

#endif /* BLK_DAREDEVIL_PERF_TP */

/* __TRACE_BLEX_H */
#endif

#include <trace/define_trace.h>
