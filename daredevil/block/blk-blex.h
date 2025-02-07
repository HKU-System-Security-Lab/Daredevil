#ifndef INT_BLK_BLEX_H
#define INT_BLK_BLEX_H

#ifdef CONFIG_DAREDEVIL_IO_STACK

#include <linux/blkdev.h>
#include <linux/blk-blex.h>
#include <linux/blk-mq.h>
#include <linux/blk_types.h>

#include "blk-mq.h"

/* Tunable variables and their default values in Daredevil. */

/** Start: enable/disable different levels of Daredevil optimization. */
static int daredevil_blex_opt_level __read_mostly = 2;
module_param(daredevil_blex_opt_level, int, 0644);
MODULE_PARM_DESC(daredevil_blex_opt_level, "Daredevil optimization levels at the block layer: "
	"0 for using static mapping during request init, +1: decoupled strucutured + round-robin scheduling, "
	"+2: +NQ scheduling & request routing. ");

static int daredevil_io_profile_on __read_mostly = 1;
module_param(daredevil_io_profile_on, int, 0644);
MODULE_PARM_DESC(daredevil_io_profile_on, "Daredevil optimization of whether enable I/O profiling.");

/** End: enable/disable different levels of Daredevil optimization.*/

/** Start: tunable parameters used in NQ scheduling. */
static int blex_sched_merit_alpha_numer __read_mostly = 4;
module_param(blex_sched_merit_alpha_numer, int, 0644);
MODULE_PARM_DESC(blex_sched_merit_alpha_numer, "Daredevil numerator for the scheduling merit alpha value.");

static int blex_sched_merit_alpha_denom __read_mostly = 5;
module_param(blex_sched_merit_alpha_denom, int, 0644);
MODULE_PARM_DESC(blex_sched_merit_alpha_denom, "Daredevil denominator for the scheduling merit alpha value.");

static int blex_prio_arr_mru __read_mostly = 4;
module_param(blex_prio_arr_mru, int, 0644);
MODULE_PARM_DESC(blex_prio_arr_mru, "Daredevil MRU value used for NQ scheduling.");

/** End: tunable parameters used in NQ scheduling. */

/* The context enumeration when calling the scheduling algorithm 
 * for selecting the target NQs.
 * There exist four possible states when trying to assign an NQ
 * to a req or a kernel thread (task).
 *  
 * TASK_INIT_CONTEXT: trying to assign a default NQ to a kernel task 
 * during the task initialization.
 * 
 * TASK_UPDATE_CONTEXT: trying to assign a default NQ to a kernel task
 * when the task is being updated.
 *
 * RQ_CONTEXT: trying to select a NQ for a particular request,
 * this is typically called by __blk_mq_alloc_requests().
 * 
 * NO_TAG_CONTEXT: trying to select a NQ when the last selected one has no 
 * more available tags. Typically called under very I/O intensive scenarios 
 * or suggesting something went wrong with our scheduler design.
 */
enum blk_blex_sched_context {
	BLEX_SCHED_TASK_INIT_CONTEXT = 0,
	BLEX_SCHED_TASK_UPDATE_CONTEXT,
	BLEX_SCHED_REQ_CONTEXT,
	BLEX_SCHED_NO_TAG_CONTEXT,
};

#define BLK_BLEX_REQ_HIGH_PRIO_FLAGS (REQ_SYNC | REQ_META | REQ_PRIO)

struct blk_blex_task_ioc {
	/* 16-bit to record I/O patterns, detailed description included in include/linux/blk-blex.h. */
	u16 io_pattern;
	struct {
		/* The fields for I/O pattern determination. */
		unsigned int nr_sync_reqs;				/* The priority of each task is also determined by sync state. */
		unsigned int nr_async_reqs;
	};
	/**
	 * @last_working_cpu: the last cpu on which the parent task wakes up.
	 * The current working cpu can be retrieved by using task_cpu(parent_task).
	 */
	unsigned int last_working_cpu;
	struct request_queue *q;
	struct blk_mq_hw_ctx *hctx;
	struct blk_mq_hw_ctx *outlier_hctx;
	struct task_struct *parent_task;
	u64 last_update_ns;							/* The microsecond that the associated task gets updated last time for this device. */
	bool need_update;
	blk_blex_prio_t blex_ioprio;
};

static inline bool blk_blex_is_high_prio(blk_opf_t cmd_flags)
{
	return (cmd_flags & BLK_BLEX_REQ_HIGH_PRIO_FLAGS) ? !(cmd_flags & REQ_IDLE) : false;
}

static inline bool blk_blex_is_sync_req(blk_opf_t cmd_flags)
{
	return (cmd_flags & REQ_SYNC) && !(cmd_flags & REQ_IDLE);
}
/**
 * blk_blex_alloc_and_init_task_ioc() - Allocate and initialize the blk-blex I/O context.
 * @task: The pointer to the parent task of this I/O context.
 * @ioc: The pointer to the allocated blk-blex I/O context struct.
 * @q: The request queue this I/O context is associated with.
 * @prio: The priority of this task.
 * @cpu: The cpu this task is attached to.
 *
 * Return: 0 on success and -ENOMEM if allocation fails and -1 for other error.
 */
int blk_blex_alloc_and_init_task_ioc(struct task_struct *task, struct blk_blex_task_ioc **ioc, 
		struct request_queue *q, unsigned short prio, unsigned int cpu);

void blk_blex_free_task_ioc(struct blk_blex_task_ioc *ioc);

/* The default request queue flags for blk-blex specifies no bio merging and data collection */
#define BLK_BLEX_QUEUE_FLAGS ((1UL << QUEUE_FLAG_NOMERGES) | \
				(1UL << QUEUE_FLAG_STATS))

static inline void blk_blex_mark_allowed_cpu(struct blk_mq_hw_ctx *hctx, unsigned int cpu)
{
	cpumask_test_and_set_cpu(cpu, hctx->run_cpumask);
}

static inline void blk_blex_unmark_allowed_cpu(struct blk_mq_hw_ctx *hctx, unsigned int cpu)
{
	cpumask_test_and_clear_cpu(cpu, hctx->run_cpumask);
}

/** 
 * blk_blex_stage_request() - temporary stage the request to the block layer in case
 * that the underlying NQ is full for now.
*/
static inline void blk_blex_stage_request(struct request *rq, struct blk_mq_hw_ctx *hctx, bool at_head, bool bypass)
{
	struct blk_blex_vhw_ctx *vnq = &hctx->vnqs[rq->mq_ctx->cpu];
	spinlock_t *lock = bypass ? &vnq->dispatch_lock : &vnq->ctx_lock;
	struct list_head *head = bypass ? &vnq->dispatch_list : &vnq->ctx_list;

	blk_blex_mark_allowed_cpu(hctx, rq->mq_ctx->cpu);
	spin_lock(lock);
	if (at_head)
		list_add(&rq->queuelist, head);
	else
		list_add_tail(&rq->queuelist, head);
	spin_unlock(lock);
}

/** 
 * blk_blex_ioprio_simple_map() - Simply mapping the I/O priority of the conventional 
 * Linux kernel usage to blex-defined I/O priority.
 */
static inline blk_blex_prio_t blk_blex_ioprio_simple_map(unsigned short prio)
{
	if (IOPRIO_PRIO_CLASS(prio) == IOPRIO_CLASS_RT)
		return BLK_BLEX_L_PRIO;
	else
		return BLK_BLEX_T_PRIO;
}

void blk_blex_tenant_ioc_profile(struct task_struct *task, struct bio *bio, struct request_queue *q);

/** 
 * blk_blex_sched_hctx() - Perform SQ to NQ scheduling under normal scenarios. 
 * This function should consider many aspects when performing scheduling, e.g., lock contention,
 * interrupt handling, etc.
 *
 * @task: The kernel thread that tries to issue the I/O request. 
 * @q: The target request queue (i.e., device).
 * @info: The additional info provided to the scheduler. In TASK_*_CONTEXT, info indicates the blex priority, 
 * while in other contexts where the task already has I/O contexts associated, info indicates command flags.
 * @appendix: The pointer used to provide some additional information needed in the form of pointer.
 * @old_hctx: The old NQ selected before for the target request/task.
 * @context: The calling context to perform scheduling.
 *
 * Return the chosen NQ on success and NULL on failure.
 */
struct blk_mq_hw_ctx *blk_blex_sched_hctx(struct blk_blex_task_ioc *task_ioc, struct blk_mq_hw_ctx *old_hctx,
			struct bio *bio, unsigned int info, int context);

#endif

#endif