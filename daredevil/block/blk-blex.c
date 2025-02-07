#ifdef CONFIG_DAREDEVIL_IO_STACK
#include <linux/blk-mq.h>
#include <linux/blk-blex.h>
#include <linux/interrupt.h>
#include <trace/events/block.h>

#include "blk.h"
#include "blk-rq-qos.h"
#include "blk-blex.h"
#include "blk-mq.h"
#include "blk-mq-sched.h"

#define BLEX_SCHED_MERIT_INF 2147483647

int blk_blex_alloc_and_init_vnqs(struct blk_mq_hw_ctx *hctx, struct blk_mq_tag_set *set, int node)
{
	struct blk_blex_vhw_ctx *vnq;
	unsigned int i;
	gfp_t gfp = GFP_NOIO | __GFP_NOWARN | __GFP_NORETRY;
	
	hctx->vnqs = kcalloc_node(nr_cpu_ids, sizeof(struct blk_blex_vhw_ctx),
		       	GFP_KERNEL, node);
	if (unlikely(!hctx->vnqs))
		return -ENOMEM;

	atomic_set(&hctx->nr_vnq_rq, 0);
	for (i = 0; i < nr_cpu_ids; i++){
		vnq = &hctx->vnqs[i];
		vnq->hctx = hctx;
		vnq->numa_node = node;
		spin_lock_init(&vnq->dispatch_lock);
		INIT_LIST_HEAD(&vnq->dispatch_list);
		vnq->fq = blk_alloc_flush_queue(node, set->cmd_size, gfp);
		if (!vnq->fq)
			goto err;
	}
	
	return 0;
err:
	for (i = 0; i < nr_cpu_ids; i++){
		vnq = &hctx->vnqs[i];
		blk_free_flush_queue(vnq->fq);
	}
	return -ENOMEM;
}
EXPORT_SYMBOL_GPL(blk_blex_alloc_and_init_vnqs);

enum {
	BLEX_NO_RW = -1,
	BLEX_RR_RW,
	BLEX_RR_SW,
	BLEX_SR_RW,
	BLEX_SR_SW,
};

static void blk_blex_set_maps(struct blk_blex_nq_regulator *nq_reg, unsigned int cpu, struct blk_blex_exposed_nq *enq);
static void blk_blex_clear_maps(struct blk_blex_nq_regulator *nq_reg, unsigned int cpu, struct blk_blex_exposed_nq *enq);

int blk_blex_alloc_and_init_task_ioc(struct task_struct *task, struct blk_blex_task_ioc **ioc, struct request_queue *q, unsigned short prio, unsigned int cpu)
{
	struct blk_blex_task_ioc *ioc_gen;

	if (!(*ioc)){
		ioc_gen = kzalloc_node(sizeof(struct blk_blex_task_ioc), GFP_KERNEL, cpu_to_node(cpu));
		if (!ioc_gen){
			blk_blex_pr_err("Can not allocate memory for blk-blex I/O context.\n");
			return -ENOMEM;
		}
	}

	ioc_gen->q = q;
	ioc_gen->blex_ioprio = blk_blex_ioprio_simple_map(prio);
	ioc_gen->last_update_ns = ktime_get_ns();
	ioc_gen->parent_task = task;
	ioc_gen->hctx = blk_blex_sched_hctx(ioc_gen, NULL, NULL, ioc_gen->blex_ioprio, BLEX_SCHED_TASK_INIT_CONTEXT);
	ioc_gen->outlier_hctx = NULL;
	if (!ioc_gen->hctx){
		blk_blex_pr_err("IOC INIT: Can not select a default NQ during blk_blex_alloc_and_init_task_ioc().\n");
		kfree(ioc_gen);
		return -1;
	}

	*ioc = ioc_gen;
	return 0;
}

void blk_blex_free_task_ioc(struct blk_blex_task_ioc *ioc)
{
	struct blk_mq_tag_set *tag_set = ioc->q->tag_set;
	struct blk_blex_exposed_nq *enq;
	
	enq = blk_blex_get_enq(blk_blex_hctx_to_nqid(ioc->hctx), tag_set);
	blk_blex_clear_maps(tag_set->nq_reg, ioc->last_working_cpu, enq);
	kfree(ioc);
}

/****** START OF DYNAMIC UPDATE FUNCTION. *******/

/** Start: per-tenant I/O profiling. ***/

/* [static]
 * blk_blex_decide_dominance() - Check the dominance of the two candidates.
 * @candidate0: the value of the first candidate 
 * @candidate1: the value of the second candidate
 * @type: the type of checking dominance, 0 for check the dominance between two, 1 for checking
 * whether the first candidate has dominance.
 * Return 0 indicates dominance of candidate 0 and 1 for candidate 1, -1 on no clear dominance in type 0
 * Return 0 or 1 indicating whether candidate 0 has dominance of candidate 1.
 * The dominance is decided based on whether one candidate exceeds the other by more than a magnitude.
 */
static int blk_blex_decide_dominance(int candidate0, int candidate1, int type)
{
	if (type == 0){
		if (candidate0 && candidate1){
			return (candidate0 + (candidate1 / 2) / candidate1 >= 10) ? 0 : 
				((candidate1 + (candidate0 / 2) / candidate0 >= 10) ? 1 : -1);
		} else {
			return candidate0 ? 0 : (candidate1 ? 1 : -1);
		}
		
	} else 
		return (candidate1 == 0) ? 0 : (candidate0 + (candidate1 / 2) / candidate1 >= 10);
	
}

/** 
 * blk_blex_map_task_ioc_ioprio() - Map the I/O context statistics info stored in @ioc 
 * to the blex-defined I/O priority.
 * @ioc: The blex-define I/O context.
 * @parent_ioprio: The I/O priority of the parent task.
 * 
 * Return: the mapped blex-defined I/O priority. */
static blk_blex_prio_t blk_blex_map_task_ioc_ioprio(struct blk_blex_task_ioc *ioc, unsigned short parent_ioprio)
{
	blk_blex_prio_t blex_ioprio = blk_blex_ioprio_simple_map(parent_ioprio);
	bool sync_dom = blk_blex_decide_dominance(ioc->nr_async_reqs, ioc->nr_sync_reqs, 0);

	if (sync_dom && blex_ioprio == BLK_BLEX_T_PRIO)
		return BLK_BLEX_OUTLIER_PRIO;
	return blex_ioprio;
}

/* Update the blex-defined I/O context statistics, e.g., rand/seq, read/write, sync/async.
 * The actual update of the whole blex-define IO context will be done during the periodical update.
 */
static bool __blk_blex_update_task_ioc_stats(struct bio *bio, struct blk_blex_task_ioc *q_ioc)
{
	int old_sync_dom, sync_changed = 0;

	old_sync_dom = blk_blex_decide_dominance(q_ioc->nr_sync_reqs, q_ioc->nr_async_reqs, 1);

	if (blk_blex_is_sync_req(bio->bi_opf))
		q_ioc->nr_sync_reqs++;
	else
		q_ioc->nr_async_reqs++;

	sync_changed = (blk_blex_decide_dominance(q_ioc->nr_sync_reqs, q_ioc->nr_async_reqs, 1) != old_sync_dom);

	return sync_changed;
}

void blk_blex_tenant_ioc_profile(struct task_struct *task, struct bio *bio, struct request_queue *q)
{
	struct io_context *ioc;
	struct blk_blex_task_ioc *q_ioc;
	int err;
	bool profile_update = false;

	if (!task)
		return;

	if (unlikely(task != current)){
		task_lock(task);
		ioc = task->io_context;
		task_unlock(task);
	} else
		ioc = current->io_context;
	
	q_ioc = xa_load(&ioc->blk_blex_iocs, q->id);

	if (!q_ioc){
		err = blk_blex_alloc_and_init_task_ioc(task, &q_ioc, q, ioc->ioprio, raw_smp_processor_id());
		if (err){
			blk_blex_pr_err("TASK INIT: Can not alloc or init blk-blex I/O context from device %s (ID %u) to Task %d.\n",
				q->disk->disk_name, q->id, current->pid);
			return;
		}

		err = xa_insert(&ioc->blk_blex_iocs, q->id, q_ioc, GFP_KERNEL);
		if (err){
			blk_blex_pr_err("Can not allocate xarray index memory.\n");
			kfree(q_ioc);
			return;
		}

	} else if (unlikely(!q_ioc->hctx)){
		q_ioc->hctx = blk_blex_sched_hctx(q_ioc, NULL, NULL, 
			blk_blex_ioprio_simple_map(get_current_ioprio()), BLEX_SCHED_TASK_INIT_CONTEXT);
		q_ioc->last_working_cpu = raw_smp_processor_id();
	}

	/* Update the blex I/O context field of current task if I/O profiling is enabled. */
	if (daredevil_io_profile_on)
		profile_update = __blk_blex_update_task_ioc_stats(bio, q_ioc);

	if (q_ioc->last_working_cpu != raw_smp_processor_id()){
		/* The task has been rescheduled, then we need to clear its working fields accordingly and 
		 * set the working status of current CPU cores. */
		blk_blex_clear_maps(q->tag_set->nq_reg, q_ioc->last_working_cpu, 
				blk_blex_get_enq(blk_blex_hctx_to_nqid(q_ioc->hctx), q->tag_set));
		blk_blex_set_maps(q->tag_set->nq_reg, raw_smp_processor_id(), 
				blk_blex_get_enq(blk_blex_hctx_to_nqid(q_ioc->hctx), q->tag_set));
		q_ioc->last_working_cpu = raw_smp_processor_id();
	}



	if (READ_ONCE(q_ioc->need_update)){
		blk_blex_prio_t new_ioprio;
		struct blk_mq_hw_ctx *new_hctx;

		WRITE_ONCE(q_ioc->need_update, false);

		new_ioprio = blk_blex_map_task_ioc_ioprio(q_ioc, ioc->ioprio);
		new_hctx = blk_blex_sched_hctx(q_ioc, NULL, NULL, new_ioprio, BLEX_SCHED_TASK_UPDATE_CONTEXT);
		
		q_ioc->blex_ioprio = new_ioprio;
		q_ioc->hctx = new_hctx;

		blk_blex_pr_info("IOPRIO CHANGE: Setting task_ioc=%p (to new hctx id=%d, new ioprio=%d) with q=%s.\n",
			q_ioc, new_hctx->queue_num, new_ioprio, q->disk->disk_name);
	}

	/* Detecting changes in synchronous requests versus asynchronous requests. */
	if (profile_update){
		blk_blex_prio_t new_ioprio, old_ioprio;
		struct blk_mq_hw_ctx *new_outlier;

		new_ioprio = blk_blex_map_task_ioc_ioprio(q_ioc, ioc->ioprio);
		old_ioprio = q_ioc->blex_ioprio;

		/* After detecting outlier changes, if the IO prio fits the outlier pattern. */
		if (new_ioprio == BLK_BLEX_OUTLIER_PRIO){
			if (!q_ioc->outlier_hctx){
				/* NULL outlier hctx, schedule one. */
				new_outlier = blk_blex_sched_hctx(q_ioc, NULL, NULL, BLK_BLEX_L_PRIO, BLEX_SCHED_TASK_UPDATE_CONTEXT);
				q_ioc->outlier_hctx = new_outlier;
			}
		} else {
			/* Here, the synchronization pattern change means that it has returned from the outlier to normal. */
			if (old_ioprio == BLK_BLEX_OUTLIER_PRIO)
				q_ioc->outlier_hctx = NULL;
		}

		q_ioc->nr_sync_reqs = q_ioc->nr_async_reqs = 0;
		q_ioc->blex_ioprio = new_ioprio;
	}
}

/** End: per-tenant I/O profile. */

/** Start: dynamic update of per-tenant profile. */
void blk_blex_update_task_ioc(struct blk_blex_task_ioc *ioc)
{
	WARN_ON_ONCE(!ioc->need_update);
	
	if (!ioc->need_update){
		blk_blex_pr_err("TASK UPDDATE: The I/O context of task=%d, q=%s does not need update.\n",
			ioc->parent_task->pid, ioc->q->disk->disk_name);
		return;
	}
}
/** End: dynamic update of per-tenant profile. */

/****** END OF DYNAMIC UPDATE FUNCTION. *******/

/****** START OF NQ SCHEDULING FUNCTIONS USED IN TSCHED AND NQREG ******/

/** [static]
 * blk_blex_sched_rr_next_nsqid() - Get the next NSQ ID from the NQ group with the specified 
 * priority using the round-robin fashion.
 * @nq_reg: The NQ pool nq_reg associated with a specific device
 * @prio: The blk-blex defined priority
 * @type: The type to fetch next using round-robin fashion, either NSQ-style or NCQ-style
 *
 * Return: A non-negative value indicating the index of the next NSQ on success and negative on failure.
 */
static int blk_blex_sched_rr_next_nsqid(struct blk_blex_nq_regulator *nq_reg, blk_blex_prio_t prio, enum blk_blex_nq_type_enum type)
{
	struct blk_blex_nq_group *nq_group = &nq_reg->nq_groups[prio];
	struct blk_blex_exposed_nq *ncq;
	unsigned int sel_ncq_idx, sel_nsq_id, start_ncq_idx;
	unsigned int max_nqid = nq_reg->blex_info->max_nqid, nr_cqs = nq_reg->blex_info->nr_cqs;
	
	start_ncq_idx = blk_blex_cqid_to_index(nq_group->ncq_start, max_nqid, nr_cqs);

	if (type == BLEX_NCQ){
		unsigned int sel_ncq_idx_offset, sel_nsq_id_offset, sel_ncq_qid;
		struct blk_blex_enq_ncq *encq;

		sel_ncq_idx_offset = (unsigned int)atomic_fetch_inc(&nq_group->rr_ncq_offset);
		sel_ncq_idx_offset %= nq_group->nr_ncqs;
		sel_ncq_idx = start_ncq_idx + sel_ncq_idx_offset;
		
		sel_ncq_qid = blk_blex_get_kth_cqid(sel_ncq_idx, max_nqid, nr_cqs);
		
		ncq = &nq_reg->enqs[sel_ncq_qid - 1];
		encq = ncq->encq;

		sel_nsq_id_offset = (unsigned int)atomic_fetch_inc(&encq->rr_nsq_offset);
		sel_nsq_id_offset %= encq->nr_attached_nsqs;
		sel_nsq_id = sel_ncq_qid + sel_nsq_id_offset;

	} else {
		int sel_nsq_id_offset;

		sel_nsq_id_offset = (unsigned int)atomic_fetch_inc(&nq_group->rr_nsq_offset);
		sel_nsq_id_offset %= nq_group->nr_attach_nsqs;
		sel_nsq_id = nq_group->ncq_start + sel_nsq_id_offset;
	}

	return sel_nsq_id;
}

/** [static]
 * blk_blex_sched_rr_next_nq() - Get the next target NQ given a device and priority using round-robin fashion.
 * @q: The pointer to the block device (i.e., request queue) 
 * @prio: The blk-blex defined priority
 */
static inline struct blk_mq_hw_ctx *blk_blex_sched_rr_next_nq(struct request_queue *q, blk_blex_prio_t prio,  enum blk_blex_nq_type_enum type)
{
	int nqid = blk_blex_sched_rr_next_nsqid(q->tag_set->nq_reg, prio, type);
	return blk_blex_nqid_to_hctx(q, nqid);
}

int blk_blex_sched_random_nqid(struct blk_blex_nq_regulator *nq_reg, blk_blex_prio_t prio, enum blk_blex_nq_type_enum type)
{
	struct blk_blex_nq_group *nq_group = &nq_reg->nq_groups[prio];
	int i, nqid = 0;

	if (unlikely(nq_group->nr_attach_nsqs == 1 || nq_group->nr_attach_nsqs == 2))
		nqid = blk_blex_sched_rr_next_nsqid(nq_reg, prio, BLEX_NSQ);
	else {
		get_random_bytes(&i, sizeof(i));
		if (likely(type == BLEX_NSQ))
			nqid = i % (int)nq_group->nr_attach_nsqs + nq_group->ncq_start;
		else {
			unsigned int ncq_start_index, max_nqid, nr_cqs;
			max_nqid = nq_reg->blex_info->max_nqid;
			nr_cqs = nq_reg->blex_info->nr_cqs;
			ncq_start_index = blk_blex_cqid_to_index(nq_group->ncq_start, max_nqid, nr_cqs);
			nqid = blk_blex_get_kth_cqid(i % (int)nq_group->nr_ncqs + ncq_start_index, max_nqid, nr_cqs);
		}
	}
	return nqid;
}
EXPORT_SYMBOL_GPL(blk_blex_sched_random_nqid);

/** [static]
 * blk_blex_clear_maps() - 
 * @cpu: The CPU that was supposedly has tenants accessing the @enq but was checked to be wiped out. 
 * @enq: The target eNQ.
 */
static void blk_blex_clear_maps(struct blk_blex_nq_regulator *nq_reg, unsigned int cpu, struct blk_blex_exposed_nq *enq)
{
	struct blk_blex_nsq_map *nmap;
	void *v;

	nmap = &nq_reg->nsq_maps[cpu];
	v = xa_load(&nmap->xa_nsq_map, enq->nqid);
	if (!v){
		/* In case that nothing was stored inside the nsq_map, i.e., we only clear the CPU mask. */
		cpumask_test_and_clear_cpu(cpu, enq->ensq.claimed_core_cpumask);
		return;
	} else {
		/* Now, the old CPU still has information that certain tenants are using this eNQ. */
		if (xa_to_value(v) > 1){
			/* There are at least 2 tasks running on this CPU, including this task, currently 
			 * claiming usage to this old_enq. Thus, we should not clear any mapping here. 
			 */
			xa_store(&nmap->xa_nsq_map, enq->nqid, xa_mk_value(xa_to_value(v) - 1), GFP_KERNEL);
		} else {
			/* Only one task uses this NSQ, and now we have rescheduled the task to another 
			 * NSQ, this means that 1. for a forthcoming period, the CPU no longer used this 
			 * NSQ, and 2. the NSQ is no longer claimed by this CPU. 
			 */
			
			/* Clear the CPU bit in the old_ensq's cpu mask. */
			cpumask_test_and_clear_cpu(cpu, enq->ensq.claimed_core_cpumask);

			/* Clear the NSQ usage entry in the per-CPU nsq map. */
			atomic_dec(&nmap->nr_in_use);
			xa_erase(&nmap->xa_nsq_map, enq->nqid);
		}
	}
}

static void blk_blex_set_maps(struct blk_blex_nq_regulator *nq_reg, unsigned int cpu, struct blk_blex_exposed_nq *enq)
{
	struct blk_blex_nsq_map *nmap;
	void *v;

	/* Set the CPU map in the eNSQ first. */
	cpumask_set_cpu(cpu, enq->ensq.claimed_core_cpumask);

	/* Then we set the per-CPU NSQ map. */
	nmap = &nq_reg->nsq_maps[cpu];

	v = xa_load(&nmap->xa_nsq_map, enq->nqid);
	if (!v){
		/* This CPU access the NSQ for the first time. */
		atomic_inc(&nmap->nr_in_use);
		xa_store(&nmap->xa_nsq_map, enq->nqid, xa_mk_value(1), GFP_KERNEL);
	} else {
		xa_store(&nmap->xa_nsq_map, enq->nqid, xa_mk_value(xa_to_value(v) + 1), GFP_KERNEL);
	}
}

/** [static]
 * blk_blex_sched_update_mappings() - Update the CPU mask mappings stored in the eNSQ, i.e.,
 * the usage of CPUs in this eNSQ, and the NSQ mapping stored in per-CPU, i.e., the usage of NSQs 
 * for this CPU.
 * Only called under TASK_INIT or TASK_UPDATE contexts and is called on a per-CPU basis.
 * Return: the CPU core ID set by this function.
 */
static int blk_blex_sched_update_mappings(struct blk_blex_nq_regulator *nq_reg, struct blk_blex_exposed_nq *sched_enq, 
		struct blk_blex_exposed_nq *old_enq, int context)
{
	int cpu = get_cpu();

	blk_blex_set_maps(nq_reg, cpu, sched_enq);

	/* Only under the TASK_UPDATE context do we need to update the information of its 
	 * original scheduled eNQ (old_enq). */
	if (context == BLEX_SCHED_TASK_UPDATE_CONTEXT)
		blk_blex_clear_maps(nq_reg, cpu, old_enq);

	put_cpu();

	return cpu;
}

/** [static]
 * blk_blex_sched_task_init() - Schedule an initial NQ for a task during its initialization.
 * In this step, since no information about the task is given except for the priority,
 * we use round-robin style to scatter the tasks across NQs.
 * @q: The request queue representing the device.
 * @prio: The priority of the task to be initialized
 *
 * Return an struct hctx pointer mapped to the selected NSQ.
 */
static struct blk_mq_hw_ctx *blk_blex_sched_task_init(struct blk_blex_task_ioc *task_ioc, unsigned int task_cpu)
{
	struct blk_mq_hw_ctx *select_hctx = NULL;
	struct request_queue *q = task_ioc->q;
	struct blk_blex_nq_regulator *nq_reg = q->tag_set->nq_reg;
	struct blk_blex_exposed_nq *sel_enq;
	blk_blex_prio_t prio = task_ioc->blex_ioprio;
	unsigned int sel_nsq_id;

	/* In both blex_opt_level == 0 or == 1 case, i.e., static mapping and baseline scheduling,
	 * task_init is needed for potential further optimization level changes.
	 */
	if (daredevil_blex_opt_level < 2)
		sel_nsq_id = blk_blex_sched_rr_next_nsqid(nq_reg, prio, BLEX_NSQ);
	else {	
		sel_enq = blk_blex_recommend_nsq(nq_reg, prio, true, 0, 0);
		sel_nsq_id = sel_enq->nqid;

		task_ioc->last_working_cpu = blk_blex_sched_update_mappings(nq_reg, sel_enq, NULL, BLEX_SCHED_TASK_INIT_CONTEXT);
	}

	select_hctx = blk_blex_nqid_to_hctx(q, sel_nsq_id);
	return select_hctx;
}

/** [static]
 * blk_blex_sched_nq_allright() - Check whether an NQ ID, given its supposed serving priority, still
 * remains suitable to the group of NQs that serve the same priority.
 * @nq_reg: The NQ pool nq_reg.
 * @prio: The supposed serving priority.
 * @nqid: The ID of the given NQ.
 *
 * Return true if belonging to the same group and false otherwise.
 */
static inline bool blk_blex_sched_nq_allright(struct blk_blex_nq_regulator *nq_reg, blk_blex_prio_t prio, unsigned int nqid, int context)
{
	struct blk_blex_nq_group *nq_group = &nq_reg->nq_groups[prio];
	struct blk_blex_exposed_nq *enq = &nq_reg->enqs[nqid - 1];
	bool ret = false;

	/* Baseline optimization only checks whether they match. */
	ret = (enq->prio == prio);
	if (daredevil_blex_opt_level == 2 && context == BLEX_SCHED_TASK_UPDATE_CONTEXT)
			ret  = (ret && enq == blk_blex_peek_recommend(nq_group));
	
	return ret;
}

/** [static]
 * blk_blex_sched_hctx_task_update() - Schedule an NQ for the given task (kthread) during task update. 
 * @task: The kthread to be updated.
 * @q: The device request queue.
 */
static struct blk_mq_hw_ctx *blk_blex_sched_task_update(blk_blex_prio_t ioprio, struct blk_blex_task_ioc *task_ioc)
{
	struct request_queue *q = task_ioc->q;
	struct task_struct *task = task_ioc->parent_task;
	struct blk_blex_nq_regulator *nq_reg = q->tag_set->nq_reg;
	struct blk_mq_hw_ctx *sched_hctx = NULL, *old_hctx = task_ioc->hctx;
	// blk_blex_prio_t ioprio = task_ioc->blex_ioprio;
	struct blk_blex_exposed_nq *sched_enq;
	bool is_baseline = (daredevil_blex_opt_level == 1);	/* Whether baseline optimization. */

	sched_hctx = old_hctx; /* Use the original target NQ by default. */

	if (!blk_blex_sched_nq_allright(nq_reg, ioprio, blk_blex_hctx_to_nqid(old_hctx), BLEX_SCHED_TASK_UPDATE_CONTEXT)){
		if (is_baseline){
			/* Baseline optimization uses simple round-robin. */
			int nqid = blk_blex_sched_rr_next_nsqid(nq_reg, ioprio, BLEX_NSQ);
			sched_hctx = blk_blex_nqid_to_hctx(q, nqid);
		} else {
			sched_enq = blk_blex_recommend_nsq(nq_reg, ioprio, false, blex_prio_arr_mru, 1);
			sched_hctx = blk_blex_nqid_to_hctx(q, sched_enq->nqid);
		}
	}

	if (is_baseline)
		return sched_hctx;

	/* Update the CPU map in the selected eNQ and nsq map in the nq_reg*/
	if (sched_hctx && sched_hctx != old_hctx){
		struct blk_blex_exposed_nq *old_enq = blk_blex_get_enq(blk_blex_hctx_to_nqid(old_hctx), q->tag_set);
		int cpu = task_cpu(task);

		if (task_ioc->last_working_cpu != cpu){
			blk_blex_clear_maps(nq_reg, task_ioc->last_working_cpu, old_enq);
		}

		task_ioc->last_working_cpu = blk_blex_sched_update_mappings(nq_reg, sched_enq, old_enq, BLEX_SCHED_TASK_UPDATE_CONTEXT);
	}
	return sched_hctx;
}

/** [static]
 * */
static int blk_blex_sched_get_non_default_nqid(struct blk_blex_task_ioc *task_ioc, struct blk_blex_nq_regulator *nq_reg)
{
	int cpu = task_ioc->last_working_cpu, nr_tot_cpus = nr_cpu_ids;
	int max_nqid = nq_reg->blex_info->max_nqid, L_prio_st_nqid = nq_reg->nq_groups[BLK_BLEX_L_PRIO].ncq_start;
	int nr_L_prio_nqs, nqid;

	nr_L_prio_nqs = max_nqid - L_prio_st_nqid + 1;
	if (cpu <= nr_tot_cpus / 2)
		nqid = cpu % nr_L_prio_nqs + 1;
	else
		nqid = (nr_tot_cpus - cpu - 1) % nr_L_prio_nqs + 1;

	return nqid;
}

/** [static]
 * blk_blex_sched_req_alloc() - Schedule an NQ for the request during request allocation, i.e., 
 * the scheduled NQ is to be assigned to data->hctx in request allocation calls.
 * @task: The kthread that issues this request and asks for allocation.
 * @q: The device request queue.
 * @ctx: The additional context information provided.
 * @cmd_flags: The command flags for this request, in which bottom bits are req_op and top bits are req flags.
 */ 

static struct blk_mq_hw_ctx *blk_blex_sched_req_alloc(struct blk_blex_task_ioc *task_ioc, struct bio *bio, blk_opf_t cmd_flags)
{
	struct request_queue *q = task_ioc->q;
	struct blk_blex_nq_regulator *nq_reg = q->tag_set->nq_reg;
	struct blk_mq_hw_ctx *sched_hctx = NULL;
	blk_blex_prio_t bio_ioprio;
	bool use_default = true;
	
	bio_ioprio = blk_blex_ioprio_simple_map(bio->bi_ioprio);

	/* Only high-priority reqs from T-tenants does not use default one. */
	if (!(task_ioc->blex_ioprio & BLK_BLEX_L_PRIO_MASK) && 
		(bio_ioprio & BLK_BLEX_L_PRIO_MASK || blk_blex_is_high_prio(cmd_flags)))
		use_default = false;
	
	if (use_default)
		sched_hctx = task_ioc->hctx;
	else {
		if (task_ioc->blex_ioprio == BLK_BLEX_OUTLIER_PRIO){
			/* Under outlier context, use the outlier NQ. */
			sched_hctx = task_ioc->outlier_hctx;
		} else {
			/* These requests are promoted to L_PRIO_L but not within the outlier context. */
			int nqid = blk_blex_sched_get_non_default_nqid(task_ioc, nq_reg);
			sched_hctx = xa_load(&q->hctx_table, nqid);
		}
	}

	return sched_hctx;
}

/** [static]
 * blk_blex_sched_no_tag() - Schedule an NQ for the request during tag retrieval, i.e., 
 * the originally scheduled NQ has run out of available tags, typically used within blk_mq_get_tag().
 * @task: The kthread that issues this request and asks for the tag.
 * @q: The device request queue.
 * @old_hctx: The original NQ scheduled, which is out of tags.
 * @cmd_flags: The command flags for this request, in which bottom bits are req_op and top bits are req flags.
 */
static struct blk_mq_hw_ctx *blk_blex_sched_no_tag(struct blk_blex_task_ioc *task_ioc, struct blk_mq_hw_ctx *old_hctx, blk_opf_t cmd_flags)
{
	struct request_queue *q = task_ioc->q;
	struct task_struct *task = task_ioc->parent_task;
	struct blk_blex_nq_regulator *nq_reg = q->tag_set->nq_reg;
	struct blk_mq_hw_ctx *sched_hctx = NULL;
	blk_blex_prio_t ioprio;
	struct blk_blex_exposed_nq *old_enq = &nq_reg->enqs[blk_blex_hctx_to_nqid(old_hctx) - 1], *sched_enq;
	bool is_baseline = (daredevil_blex_opt_level == 1);

	if (current != task){
		blk_blex_pr_err("NQ SCHED REQ ALLOC: blk_blex_sched_req_alloc() called in the wrong context. "
			"current=%p (pid=%u) and task=%p (pid=%u).\n", current, current->pid, task, task->pid);
		return NULL;
	}

	/* The old scheduled NQ is the default NQ of the task,
		 * then task's default NQ can not be used anymore, requires update. */
	if (old_hctx == task_ioc->hctx)
		task_ioc->need_update = true;

	/* When reaching to this code, it means that the originall scheduled NSQ does not have enough tags
	 * for request submission. Since these tags are associated with the corresponding NCQs, then we need to 
	 * first update the underlying NQ management accordingly.
	 */
	ioprio = old_enq->prio;

	if (is_baseline){
		sched_hctx = blk_blex_sched_rr_next_nq(q, ioprio, BLEX_NCQ);
	} else {
		blk_blex_update_priority_array(&old_enq->group->prio_arr, nq_reg, 
				blex_prio_arr_mru, BLEX_SCHED_MERIT_INF, BLEX_NCQ);
		sched_enq = blk_blex_recommend_nsq(nq_reg, ioprio, false, 0, 1);
		sched_hctx = blk_blex_nqid_to_hctx(q, sched_enq->nqid);
	}

	/* Try at maximum twice, and if nothing works out, then use the priority-promotion one. */
	if (sbitmap_weight(&sched_hctx->tags->bitmap_tags.sb) == sched_hctx->nq_depth){
		sched_hctx = blk_blex_sched_rr_next_nq(q, ioprio, BLEX_NCQ);
		if (sbitmap_weight(&sched_hctx->tags->bitmap_tags.sb) == sched_hctx->nq_depth){
			int sched_nsqid = blk_blex_sched_get_non_default_nqid(task_ioc, nq_reg);
			sched_hctx = blk_blex_nqid_to_hctx(q, sched_nsqid);
		}
	}

	return sched_hctx;
}
struct blk_mq_hw_ctx *blk_blex_sched_hctx(struct blk_blex_task_ioc *task_ioc, struct blk_mq_hw_ctx *old_hctx,
			struct bio *bio, unsigned int info, int context)
{
	struct blk_mq_hw_ctx *sched_hctx = NULL;
	struct task_struct *task = task_ioc->parent_task;
	struct request_queue *q = task_ioc->q;

	switch (context)
	{
		/* The NQ scheduler is called when initializing a task that recently issues I/Os. */
		case BLEX_SCHED_TASK_INIT_CONTEXT:	
			sched_hctx = blk_blex_sched_task_init(task_ioc, task_cpu(task));
			break;
		/* The NQ scheduler is called when the task is updating itself of its priority and target I/O. */
		case BLEX_SCHED_TASK_UPDATE_CONTEXT:
			sched_hctx = blk_blex_sched_task_update((blk_blex_prio_t)info, task_ioc);
			break;
		/* The NQ scheduler is called when an I/O request is initialized and can not use its issuing task's default NQ. */
		case BLEX_SCHED_REQ_CONTEXT:
			sched_hctx = blk_blex_sched_req_alloc(task_ioc, bio, (blk_opf_t)info);
			break;
		/* The NQ scheduler is called when the originally assigned NQ for an request has run out of tags. */
		case BLEX_SCHED_NO_TAG_CONTEXT:
			sched_hctx = blk_blex_sched_no_tag(task_ioc, old_hctx, (blk_opf_t)info);
			break;
		default:
			blk_blex_pr_err("SCHED ERROR: blk_blex_sched_hctx() called with unsupported context: %d. "
				"for device: %s and task pid=%u.\n", context, q->disk->disk_name, task->pid);
	}		
	return sched_hctx;
}

/***** END OF NQ SCHEDULING FUNCTION USED IN TSCHED AND NQREG *********/


/****** START OF NQREG-RELATED FUNCTION. ******/

/** Start: initialization and decomposition of nqreg, including NQGroups, eNQs. */
static int blk_blex_nq_prio_arr_init(struct blk_blex_nq_priority_array *prio_arr, struct blk_blex_nq_regulator *nqreg,
	void *parent, enum blk_blex_nq_type_enum type)
{
	struct blk_blex_nq_group *group = NULL;
	struct blk_blex_enq_ncq *encq = NULL;
	int max_heap_sz, init_nq_count, id;
	
	if (type == BLEX_NCQ){
		group = (struct blk_blex_nq_group *)parent;
		max_heap_sz = nr_cpu_ids;
		init_nq_count = group->nr_ncqs;
	} else {
		encq = (struct blk_blex_enq_ncq *)parent;
		max_heap_sz = init_nq_count = encq->nr_attached_nsqs;
	}

	spin_lock_init(&prio_arr->bin_heap_lock);
	atomic_set(&prio_arr->mru, blex_prio_arr_mru);
	
	prio_arr->parent_data = parent;
	prio_arr->nq_count = init_nq_count;

	/* Initialize the main binary heap part. */
	prio_arr->bin_heap = kcalloc_node(max_heap_sz, sizeof(int),
			GFP_KERNEL, nqreg->tag_set->numa_node);
	if (unlikely(!prio_arr->bin_heap)){
		blk_blex_pr_err("PRIO ARR INIT: failed to allocate memory for prio_arr->bin_heap.\n");
		return -ENOMEM;
	}
	
	if (type == BLEX_NCQ){
		unsigned int start_ncq_idx, max_nqid, nr_cqs;

		max_nqid = nqreg->blex_info->max_nqid;
		nr_cqs = nqreg->blex_info->nr_cqs;
		start_ncq_idx = blk_blex_cqid_to_index(group->ncq_start, max_nqid, nr_cqs);
		for (id = 0; id < init_nq_count; id++){
			prio_arr->bin_heap[id] = blk_blex_get_kth_cqid(start_ncq_idx + id, max_nqid, nr_cqs);
		}
	} else {
		for (id = 0; id < init_nq_count; id++){
			prio_arr->bin_heap[id] = encq->cqid + id;
		}
	}
	prio_arr->top_nq = &nqreg->enqs[prio_arr->bin_heap[0] - 1];
	
	return 0;
}

/** 
 * blk_blex_nq_group_init() - Initialize the given NQ group based on its priority,
 * and associate the NCQs with its corresponding NQ group.
 */
static int blk_blex_nq_group_init(struct blk_blex_nq_group *nq_group, blk_blex_prio_t prio,
			struct blk_blex_nq_regulator *nq_reg)
{
	int ret = 0;
	unsigned int max_nqid, nr_cqs, nr_init_per_group_cqs, nr_attach_nsqs = 0, i, cqid, ncq_start_idx;

	max_nqid = nq_reg->blex_info->max_nqid;
	nr_cqs = nq_reg->blex_info->nr_cqs;
	nr_init_per_group_cqs = nr_cqs / BLK_BLEX_PRIO_LEVEL;

	/* Calculate initial NSQ and NCQ info for this group. */
	ncq_start_idx = prio * nr_init_per_group_cqs;
	nq_group->nq_reg = nq_reg;
	nq_group->prio = prio;
	nq_group->ncq_start = blk_blex_get_kth_cqid(ncq_start_idx, max_nqid, nr_cqs);
	nq_group->nr_ncqs = (prio + 1 == BLK_BLEX_PRIO_LEVEL) ? nr_cqs - ncq_start_idx : nr_init_per_group_cqs;
	
	for (i = 0; i < nq_group->nr_ncqs; i++){
		cqid = blk_blex_get_kth_cqid(ncq_start_idx + i, max_nqid, nr_cqs);
		nq_reg->enqs[cqid - 1].group = nq_group;
		nr_attach_nsqs += blk_blex_get_cq_nr_nsqs(cqid, max_nqid, nr_cqs);
	}
	nq_group->nr_attach_nsqs = nr_attach_nsqs;
	
	/* Initialize round-robin fields for this NQGroup. */	
	atomic_set(&nq_group->rr_ncq_offset, 0);
	atomic_set(&nq_group->rr_nsq_offset, 0);

	/* Initialize NCQ heap and group lock and latency records. */
	ret = blk_blex_nq_prio_arr_init(&nq_group->prio_arr, nq_reg, nq_group, BLEX_NCQ);
	if (ret)
		return ret;

	spin_lock_init(&nq_group->group_lock);

	return ret;
}

/** 
 * blk_blex_enq_init() - Initialize the per-NQ information given its nqid and the 
 * attached NQ nq_reg.
 */
static int blk_blex_enq_init(struct blk_blex_exposed_nq *enq, unsigned int nqid, 
					struct blk_blex_nq_regulator *nq_reg)
{
	struct blk_blex_exposed_nq *cq_info;
	struct blk_blex_enq_nsq *ensq;
	unsigned int max_nqid = nq_reg->blex_info->max_nqid, nr_cqs = nq_reg->blex_info->nr_cqs;
	bool has_cq = false;

	/* Preparation of some fields within this eNQ. */
	enq->nqid = nqid;
	enq->cqid = blk_blex_get_cqid(nqid, max_nqid, nr_cqs);
	
	has_cq = (enq->nqid == enq->cqid);
	cq_info = &nq_reg->enqs[enq->cqid - 1];

	/* The group of this eNQ, regardless of whether it contains eNSQ and eNCQ, is 
	 * consistent with the group that its NCQ belongs to.
	 */
	enq->group = cq_info->group;
	enq->prio = enq->group->prio;
	
	/* Initialize eNSQ first. */
	ensq = &enq->ensq;
	ensq->enq = enq;
	ensq->sqid = nqid;

	/* Set submission statistics for calculating merits. */
	atomic_set(&ensq->nr_in_lock_ns, 0);
	atomic_set(&ensq->nr_submit_rqs, 0);

	/* Set contention-related fields. */
	atomic_set(&ensq->nr_contending_cpus, 0);
	if (!zalloc_cpumask_var_node(&ensq->claimed_core_cpumask, 
		GFP_NOIO | __GFP_NOWARN | __GFP_NORETRY, nq_reg->tag_set->numa_node))
		goto ensq_alloc_err;

	/* Initialize eNCQ, if any. */	
	if (has_cq){
		/* This NQ info is attached only to an NSQ. */
		struct blk_blex_enq_ncq *encq;
		
		encq = kzalloc_node(sizeof(struct blk_blex_enq_ncq), GFP_KERNEL, nq_reg->tag_set->numa_node);
		if (unlikely(!encq)){
			blk_blex_pr_err("eNQ INIT: can not allocate enq->encq for enq=%d of device %s.\n", nqid, nq_reg->name);
			goto encq_alloc_err;
		}
		enq->encq = encq;

		encq->enq = enq;
		encq->cqid = enq->cqid;
		encq->nr_attached_nsqs = blk_blex_get_cq_nr_nsqs(encq->cqid, max_nqid, nr_cqs);
		atomic_set(&encq->rr_nsq_offset, 0);
		if (encq->nr_attached_nsqs > 1 && blk_blex_nq_prio_arr_init(&encq->prio_arr, nq_reg, encq, BLEX_NSQ))
			goto prio_arr_init_err;
	}

	return 0;

prio_arr_init_err:
	kfree(enq->encq);
encq_alloc_err:
	free_cpumask_var(ensq->claimed_core_cpumask);
ensq_alloc_err:
	return -ENOMEM;
}

int blk_blex_nq_regulator_alloc_and_init(struct blk_blex_nq_regulator *nq_reg, int numa_node)
{
	struct blk_mq_tag_set *set = nq_reg->tag_set;
	unsigned int max_nqid, nqid, prio, cpu;
	int ret = 0, nmap_cnt = 0;

	if (!nq_reg->tag_set || !nq_reg->driver_data)
		return -1;
	
	nq_reg->blex_info = set->blex_info;
	max_nqid = set->blex_info->max_nqid;

	spin_lock_init(&nq_reg->lock);
	xa_init(&nq_reg->online_hosts);
	atomic_set(&nq_reg->nr_updating_cpus, 0);
	
	/* Initialize the per-CPU NSQ map. */
	nq_reg->nsq_maps = kcalloc_node(nr_cpu_ids, sizeof(struct blk_blex_nsq_map), GFP_KERNEL, numa_node);
	if (unlikely(!nq_reg->nsq_maps)){
		blk_blex_pr_err("NQREG INIT: Can not allocate per-cpu NSQ xarray-based map for nq_reg %s.\n", nq_reg->name);
		goto nsq_map_err;
	}

	for_each_possible_cpu(cpu){
		struct blk_blex_nsq_map *nmap = &nq_reg->nsq_maps[cpu];
		nmap->cpu = cpu;
		atomic_set(&nmap->nr_in_use, 0);
		xa_init(&nmap->xa_nsq_map);
		nmap_cnt++;
	}

	/* Allocate and the per-NQ information */
	nq_reg->enqs = kcalloc_node(max_nqid, sizeof(struct blk_blex_exposed_nq), 
			GFP_KERNEL, numa_node);
	if (unlikely(!nq_reg->enqs)){
		blk_blex_pr_err("NQREG INIT: Can not allocate nq_reg->enqs for set %p.\n", set);
		goto enq_alloc_err;
	}

	/* Allocated and set up the NQ groups and attach NCQs to their corresponding NQ group. */
	for (prio = 0; prio < BLK_BLEX_PRIO_LEVEL; prio++){
		ret = blk_blex_nq_group_init(&nq_reg->nq_groups[prio], prio, nq_reg);
		if (ret){
			blk_blex_pr_err("NQREG INIT: Can not initialize NQ group %d of nq_reg %p.\n", prio, nq_reg);
			goto group_init_err;
		}
	}

	/* Initialize the per-NQ information for each NQ. */
	for (nqid = 1; nqid <= max_nqid; nqid++){
		ret = blk_blex_enq_init(&nq_reg->enqs[nqid - 1], nqid, nq_reg);
		
		if (unlikely(ret)){
			struct blk_blex_exposed_nq *enq;
			for (int i = 1; i < nqid; i++){
				enq = &nq_reg->enqs[i - 1];
				free_cpumask_var(enq->ensq.claimed_core_cpumask);
				if (enq->encq){
					if (enq->encq->prio_arr.bin_heap)
						kfree(enq->encq->prio_arr.bin_heap);
					kfree(enq->encq);
				}
			}
			goto group_init_err;
		}
	}
	return 0;

group_init_err:
	for (prio = 0; prio < BLK_BLEX_PRIO_LEVEL; prio++){
		if (nq_reg->nq_groups[prio].prio_arr.bin_heap)
			kfree(nq_reg->nq_groups[prio].prio_arr.bin_heap);
	}
	kfree(nq_reg->enqs);
enq_alloc_err:
	kfree(nq_reg->nsq_maps);
nsq_map_err:
	return -ENOMEM;
}
EXPORT_SYMBOL_GPL(blk_blex_nq_regulator_alloc_and_init);

void blk_blex_nq_regulator_free(struct blk_blex_nq_regulator *nq_reg)
{
	unsigned int i;
	struct blk_blex_exposed_nq *enq;

	if (unlikely(!nq_reg))
		return;
	
	kfree(nq_reg->name);
	kfree(nq_reg->nsq_maps);

	for (i = 0; i < nq_reg->blex_info->max_nqid; i++){
		enq = &nq_reg->enqs[i];
		free_cpumask_var(enq->ensq.claimed_core_cpumask);
		if (enq->encq){
			if (enq->encq->prio_arr.bin_heap)
				kfree(enq->encq->prio_arr.bin_heap);
			kfree(enq->encq);
		}
	}
	kfree(nq_reg->enqs);

	for (i = 0; i < BLK_BLEX_PRIO_LEVEL; i++){
		if (nq_reg->nq_groups[i].prio_arr.bin_heap)
			kfree(nq_reg->nq_groups[i].prio_arr.bin_heap);
	}
	
	kfree(nq_reg);
}
EXPORT_SYMBOL_GPL(blk_blex_nq_regulator_free);

/** End: initialization and decomposition of nqreg, including NQGroups, eNQs.*/

/** Start: heap-based priority array for NQ scheduling. */
static int blk_blex_calc_sched_merit(enum blk_blex_nq_type_enum type, struct blk_blex_exposed_nq *enq)
{
	int merit_last, merit_tmp, merit;
	/** TODO: update this calculation metric. */
	if (type == BLEX_NCQ){
		struct blk_blex_enq_ncq *encq = enq->encq;
		unsigned int complete_rqs, in_flight_rqs, irqs;

		complete_rqs = encq->nr_complete_rqs;
		in_flight_rqs = encq->nr_in_flight_rqs;
		irqs = encq->nr_irqs;

		merit_last = encq->ncq_sched_merit;
		merit_tmp = (in_flight_rqs * irqs) / enq->qdepth + complete_rqs;
	} else{
		/* type == BLEX_NSQ */
		struct blk_blex_enq_nsq *ensq = &enq->ensq;
		unsigned int in_lock_ns, submit_rqs, nr_claimed_cores;

		in_lock_ns = atomic_read(&ensq->nr_in_lock_ns);
		submit_rqs = atomic_read(&ensq->nr_submit_rqs);
		nr_claimed_cores = cpumask_weight(ensq->claimed_core_cpumask) + 1;

		merit_last = ensq->nsq_sched_merit;
		merit_tmp = submit_rqs ? (in_lock_ns * nr_claimed_cores) / submit_rqs : 0;
	}

	merit = merit_last - (merit_last * blex_sched_merit_alpha_numer / blex_sched_merit_alpha_denom) + 
		merit_tmp * blex_sched_merit_alpha_numer / blex_sched_merit_alpha_denom;

	return merit;
}

void blk_blex_update_priority_array(struct blk_blex_nq_priority_array *prio_arr, 
	struct blk_blex_nq_regulator *nq_reg, int mru, int force_merit, enum blk_blex_nq_type_enum type)
{
	int new_mru;

	new_mru = atomic_sub_return(mru, &prio_arr->mru);

	if (new_mru <= 0){
		struct blk_blex_exposed_nq *enqs = nq_reg->enqs, *old_top_nq;
		struct blk_blex_enq_ncq *encq;
		struct blk_blex_enq_nsq *ensq;
		int nq_count = prio_arr->nq_count, *bin_heap = prio_arr->bin_heap;
		int merit, used_merit, i = 0;
		bool top_changed = false;

		if (!spin_trylock(&prio_arr->bin_heap_lock)){
			/* It can not acquire the lock because other cores are already in the 
			 * critical section, meaning that the priority array is being update. 
			 * Thus, there is no need to update the priority array, we can simply
			 * just return.
			 */
			return;
		}
		
		/* Now we are the lock-holder responsible for updating the priority heap. */
		
		old_top_nq = rcu_dereference_protected(prio_arr->top_nq, lockdep_is_held(&prio_arr->bin_heap_lock));
		merit = blk_blex_calc_sched_merit(type, old_top_nq);
		used_merit = (force_merit == BLEX_SCHED_MERIT_INF) ? BLEX_SCHED_MERIT_INF : merit + force_merit;

		/* Min-heap based priority array updating. */
		while (i * 2 + 1 < nq_count){
			int left_son = i * 2 + 1, right_son = i * 2 + 2;
			int left_son_nqid = bin_heap[left_son], right_son_nqid = bin_heap[right_son];
			int left_merit, right_merit, compared_merit, tmp_son;

			left_merit = blk_blex_calc_sched_merit(type, &enqs[left_son_nqid - 1]);
			right_merit = right_son < nq_count ? blk_blex_calc_sched_merit(type, &enqs[right_son_nqid - 1]) : left_merit;

			compared_merit = left_merit;

			if (right_son < nq_count && right_merit < left_merit){
				left_son = right_son;
				compared_merit = right_merit;
			}
			
			if (compared_merit > used_merit)
				break;
			
			tmp_son = bin_heap[i];
			bin_heap[i] = bin_heap[left_son];
			bin_heap[left_son] = tmp_son;

			i = left_son;
		}

		/* The top item has been updated. */
		if (bin_heap[0] != old_top_nq->nqid){
			top_changed = true;
			rcu_assign_pointer(prio_arr->top_nq, &enqs[bin_heap[0] - 1]);
		}
		
		spin_unlock(&prio_arr->bin_heap_lock);
		synchronize_rcu();
		
		/* Resuming the mru value and clear the fields of the original top item. */
		atomic_xchg(&prio_arr->mru, blex_prio_arr_mru);

		/* If the old top item remains on top, then we do not need to update its fields.*/
		if (!top_changed)
			return;

		if (type == BLEX_NCQ){
			encq = old_top_nq->encq;
			encq->ncq_sched_merit = merit;
			encq->nr_in_flight_rqs = encq->nr_complete_rqs = encq->nr_irqs = 0;
		}
		else{
			ensq = &old_top_nq->ensq;
			ensq->nsq_sched_merit = merit;
			atomic_xchg(&ensq->nr_in_lock_ns, 0);
			atomic_xchg(&ensq->nr_submit_rqs, 0);
		}
	}
}

struct blk_blex_exposed_nq *blk_blex_recommend_nsq(struct blk_blex_nq_regulator *nq_reg, 
		blk_blex_prio_t prio, bool round_robin, int ncq_mru, int nsq_mru)
{
	struct blk_blex_nq_group *nq_group = &nq_reg->nq_groups[prio];
	struct blk_blex_exposed_nq *ncq, *nsq;
	int force_merit = 0;

	if (round_robin){
		/* Recommending NSQs in a round-robin fashion sets the nsq_mru and ncq_mru to the maximum. */
		nsq_mru = ncq_mru = blex_prio_arr_mru;
		force_merit = BLEX_SCHED_MERIT_INF;
	}

	/* Using sorting based recommendation. And since this function is only used to 
	 * recommend an NSQ for an task in tsched, so when recommending an NSQ, 
	 * we may perform two-step scheduling.
	 */
	ncq = blk_blex_peek_priority_array(&nq_group->prio_arr);
	blk_blex_update_priority_array(&nq_group->prio_arr, nq_reg, ncq_mru, force_merit, BLEX_NCQ);

	if (ncq->encq->nr_attached_nsqs == 1)
		nsq = ncq;
	else {
		nsq = blk_blex_peek_priority_array(&ncq->encq->prio_arr);
		blk_blex_update_priority_array(&ncq->encq->prio_arr, nq_reg, nsq_mru, force_merit, BLEX_NSQ);
	}

	return nsq;
}

/** End: heap-based priority array for NQ scheduling. */

/****** END OF NQREG-RELATED FUNCTION. ******/
#endif