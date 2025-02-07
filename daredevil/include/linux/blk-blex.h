/* SPDX-License-Identifier: GPL-2.0 */
#ifndef BLK_BLEX_H
#define BLK_BLEX_H

#ifdef CONFIG_DAREDEVIL_IO_STACK
#include <linux/blkdev.h>
#include <linux/sbitmap.h>
#include <linux/blk-mq.h>
#include <linux/bitops.h>

#define blk_blex_pr_info(format, ...) pr_info("%s:%d CPU: %u blk-blex INFO: " format, __FILE__, __LINE__, raw_smp_processor_id(), ##__VA_ARGS__)
#define blk_blex_pr_err(format, ...) pr_err("%s:%d CPU %u blk-blex ERROR: " format, __FILE__, __LINE__, raw_smp_processor_id(), ##__VA_ARGS__)

/* Determine whether blk-blex can be applied to this device.
 * Two criteria are required: 
 * 1. the provided device-queues should be more than 1 to enable multi-queue blk-blex.
 * 2. the blk-blex subset is set.
 */
#define blk_blex_nvme_enabled(set) \
	((set)->nr_hw_queues > 1 && (set)->blex_info)

/* In normal NVMe SSDs, which are not registered as an admin device, 
 * there exists a offset of '1' in the mapping between the NQ ID of 
 * the actual device (i.e., NVMe SSD) and the hardware queues (hctxs) 
 * of the request_queue.
 * This means that hctx->queue_num = 0 corresponding to nvmeq->qid = 1,
 * and so on.
 * This is caused due to that the nvmeq with qid = 0 is used as the admin
 * queue and thus is not exposed to the request_queue.
 * Thus, we define two macros for usage here to simplify the mapping.
 */
#define blk_blex_hctx_to_nqid(hctx) ((hctx)->queue_num + 1)

#define blk_blex_nqid_to_hctx(q, nqid) \
({	\
	struct blk_mq_hw_ctx *__res_hctx = NULL;	\
	__res_hctx = xa_load(&((q)->hctx_table), (nqid) == 0 ? (nqid) : (nqid) - 1);	\
	__res_hctx;	\
})

#define blk_blex_get_enq(qid, set) \
({	\
	struct blk_blex_exposed_nq *__enq = NULL;	\
	__enq = &(set)->enqs[(qid) - 1];	\
	__enq;	\
})

typedef unsigned int blk_blex_prio_t;

#define BLK_BLEX_T_PRIO 0	/* Throughput-oriented priority. */
#define BLK_BLEX_L_PRIO 1	/* Latency-critical priority. */
#define BLK_BLEX_PRIO_LEVEL 2	/* The number of base priority levels. */
#define BLK_BLEX_OUTLIER_PRIO 3	/* Outlier priority. */
#define BLK_BLEX_L_PRIO_MASK 0x1

/** We notice that the highest 8 bits of req->rq_flags are not used. 
 * Thus we use its [26:25] bits to store the IO priority used in Daredevil.
 */
#define BLK_BLEX_RQ_PRIO_BASE_SHIFT 25
#define blk_blex_set_req_prio(req, prio) \
	((req)->rq_flags |= ((__force req_flags_t)((prio) << (BLK_BLEX_RQ_PRIO_BASE_SHIFT))))
#define blk_blex_get_req_prio(req) (((req)->rq_flags >> (BLK_BLEX_RQ_PRIO_BASE_SHIFT)) & 0x3)

#define BLK_BLEX_ACTIVE_TASK_MARK XA_MARK_1

enum blk_blex_nq_type_enum {
	BLEX_NSQ = 0,	/* BLEX_NSQ-type NQ has NSQ only. */
	BLEX_NCQ,		/* BLEX_NCQ-type NQ has both NCQ and NSQ. */
};

/**
 * struct blk_blex_vhw_ctx - The data structure for the virtual NQ
 * that connects a SQ (core) to its corresponding NVMe queue.
 * This data structure preserves some features of the stale blk-mq
 * HQ design but differs in its functionality.
 */
struct blk_blex_vhw_ctx {
	/** @parent_cpu: the cpu that owns this virtual NQ. */ 
	int parent_cpu;

	/**
	 * @dispatch_list: The list containing the requests that can 
	 * not be enqueueing to either vNQ or the NQ due to limited 
	 * resources of the NVMe device.
	 */
	struct {
		spinlock_t dispatch_lock;
		struct list_head dispatch_list;
	};
	
	struct {
		spinlock_t ctx_lock;
		struct list_head ctx_list;
	};
	/*
	 * @hctx: The blk-mq HQ context associated with the NVMe queue.
	 * This HQ is preserved for
	 * 1. backward compatibility and easy development
	 * 2. HQ already has many needed fields that can be shared 
	 * among all vNQs and we treat HQ merely as an representative 
	 * of the underlying NQ,
	 */
	struct blk_mq_hw_ctx *hctx;
	
	/** @fq: vNQ attached flush queue. */
	struct blk_flush_queue *fq;
	/** @numa_node: NUMA node the storage adapter has been connected to. */
	unsigned int		numa_node;
};

static inline struct blk_blex_vhw_ctx *blk_blex_get_vnq_from_hctx(struct blk_mq_hw_ctx *hctx,
								unsigned int cpu)
{
	if (!hctx->vnqs)
		return NULL;
	return &hctx->vnqs[cpu];
}

/** 
 * struct blk_blex_dev_info - The per-device information specially designed 
 * the Daredevil storage stack. This is designed over blk-mq of current Linux 
 * kernel, such that it aims to  be compatible with many features or data 
 * structures of blk-mq.
 */ 
struct blk_blex_dev_info {
	/** blk-blex owned fields */
	const struct blk_mq_ops 	*blex_ops;
	struct blk_mq_tag_set		*tag_set;

	/** 
	 * @nr_abitration_burst: the arbitration burst used by the NVMe controller. 
	 * This value determines how many commands will be fetch from one NQ each 
	 * time by the controller, typically **4** (i.e. 2 ^ 2).
	 * nr_arbitration = -1 indicates that there is no limit.
	 * other non-negative value determines the burst as usual.
	 */
	int nr_arbitration;

	/** With extra nvme queues enabled, the exposed nr_hw_queues only means the number of CQs in
	* the device. The actual number of I/O queues, i.e., SQs, may be larger.
	*/
	/** @nr_cqs: the number of I/O completion queues (CQs) created by the device.*/
	unsigned int nr_cqs;

	/** @max_nqid: the number of total I/O queues, including both NSQs and NCQs. */
	unsigned int max_nqid;
};

struct blk_blex_nq_group;
struct blk_blex_exposed_nq;
struct blk_blex_nq_regulator;

struct blk_blex_nq_priority_array {
		/** @bin_heap_lock: The lock to protect the binary heap, i.e., priority queue. 	*/
		spinlock_t bin_heap_lock;

		/** @mru_count: The most recently used count of the heap top. */
		atomic_t mru;
		
		/** @bin_heap: The array-implemented binary heap for the priority queue. */
		int *bin_heap;

		/** @top_nq: The top item of the binary heap. */
		struct blk_blex_exposed_nq *top_nq;

		/**
		 * @nq_count: The number of NQs used in this priority array, also the length
		 * of the @bin_heap.*/
		int nq_count;

		/** @parent_data: The parent that uses this priority array, either NQGroup or NCQ. */
		void *parent_data;
};

struct blk_blex_nq_group {
	/** @group_lock: The lock to protect the whole group when updating. */
	spinlock_t group_lock;

	/** @nq_reg: The NQ nq_reg this NQ group belongs to. */
	struct blk_blex_nq_regulator *nq_reg;

	/** @prio: The priority of requests served by this NQ group. */
	blk_blex_prio_t prio;

	/** @nr_attach_nsqs: The number of NSQs attached to this NQ group. */
	int nr_attach_nsqs;

	struct blk_blex_nq_priority_array prio_arr;

	struct {
		/**
		 * @rr_nsq_offset: Accessing the attached NSQs in a round robin fashion.
		 * This is a zero-based value, and has the range within [0, nr_attach_nsq - 1].
		 * And the actual NSQ ID is calculated as ncq_start + rr_nsq_offset.
		 */
		atomic_t rr_nsq_offset;

		/** @rr_cur_nq: The index of the default NCQ in round-robin fashion. */
		atomic_t rr_ncq_offset;
	};

	/** @nr_nqs: The number of NCQs belonging to this group. */
	unsigned nr_ncqs;

	/** @nq_st: The start index of this NCQ group. */
	unsigned ncq_start;
};

struct blk_blex_enq_ncq {
	/** @cpu: The CPU this NCQ is registered on. */
	int cpu;

	struct blk_blex_exposed_nq *enq;

	unsigned int cqid;

	/** @nr_attached_nsqs: The number of NSQs attached to this eNQ. */
	unsigned int nr_attached_nsqs;

	/**
	 * @rr_nsq_offset: Accessing the attached NSQs in a round robin fashion. 
	 * This is a zero-based value and is only useful when nqid == cqid.
	 */
	atomic_t rr_nsq_offset;
	
	/** @ncq_sched_merit: The schedule merit of the NCQ for its NCQGroup. */
	int ncq_sched_merit;

	struct {
		/** @nr_in_flight_rqs: The number of in-flight requests being handled by the SSDs. */
		unsigned int nr_in_flight_rqs;

		/** @nr_complete_rqs: The number of completed requests. */
		unsigned int nr_complete_rqs;

		/** @nr_irqs: The number of interrupts issued in this NCQ. */
		unsigned int nr_irqs;
	};
	
	struct blk_blex_nq_priority_array prio_arr;
};

struct blk_blex_enq_nsq {
	struct blk_blex_exposed_nq *enq;
	
	/** @sqid: The identifier of this NSQ. */
	unsigned int sqid;

	/** @nsq_sched_merit: The schedule merit of this NSQ for its NCQ. */
	int nsq_sched_merit;
	
	/** Fields recording statistics of this NSQ, usually updated when holding nvmeq->sq_lock. */
	struct {
		/** @nr_in_lock_ns: The microseconds used in lock-hold session. */
		atomic_t nr_in_lock_ns;

		/** @nr_submit_rqs: The number of total submitted requests. */
		atomic_t nr_submit_rqs;

		/**
		 * @nr_in_queue_rqs: The number of I/O reqs enqueued to the device-level 
		 * NSQs but the door bell has not been ringed, this value can only be
		 * changed when holding the nvmeq->sq_lock.
		 */
		unsigned int nr_in_queue_rqs;
	};

	/** Fields used to record CPU contention. */
	/** @nr_contending_cpus: The number of CPUs trying to contend for this NQ. */
	atomic_t nr_contending_cpus;

	/** @claimed_core_cpumask: The bitmap of the CPUs that may contend for this NSQ. */
	cpumask_var_t claimed_core_cpumask;

};

struct blk_blex_exposed_nq {
	/** @group: The NQ group this NQ belongs to. */
	struct blk_blex_nq_group *group;

	/** @prio: The priority of this eNQ. */
	blk_blex_prio_t prio;

	/** @qdepth: The queue depth of this NQ. */
	unsigned int qdepth;

	/** @nqid: The ID of the associated NQ. */
	unsigned int nqid;

	/** @cqid: The ID of its associated NCQ. */
	unsigned int cqid;

	/** @ensq: The associated eNSQ, existent for each eNQ. */
	struct blk_blex_enq_nsq ensq;
	
	/** @encq: The associated eNCQ, if any. */
	struct blk_blex_enq_ncq *encq;

	/** @driver_data: The attached NQ, dereferenced as struct nvmeq in NVMe driver. */
	void *driver_data;
};

struct blk_blex_nsq_map {
	/* @cpu: The CPU core this nsq map is associated with. */
	int cpu;
	
	/** @nr_in_use: The number of NSQs in use by the CPU. */
	atomic_t nr_in_use;

	/** 
	 * @xa_nsq_map: The xarray to store per-cpu mapping of nsq_id -> nr_tasks. 
	 * It records
	 * 1. the usage of nsqs in use by this cpu, recorded in the index
	 * 2. the number of tasks using this NSQ, recorded in the value.
	 */
	struct xarray xa_nsq_map;
};

struct blk_blex_nq_regulator {
	/** @name: The name of the underlying SSD, typically the same as dev_name, e.g., nvme0. */
	char *name;
	
	/** @lock: Protect the whole NQ nq_reg when updating. */
	spinlock_t lock;

	/** @update_worker: The worker thread function for updating NQ management.*/
	struct delayed_work update_worker;

	/** @tag_set: The tag set associated with the underlying device.*/	
	struct blk_mq_tag_set *tag_set;

	/** @blex_info: The pointer to the blex-specific device info associated with the device. */
	struct blk_blex_dev_info *blex_info;

	/** 
	 * @driver_data: The underlying NVMe device, will be dereferenced as
	 * struct nvme_dev* in NVMe driver.
	 */
	void *driver_data;

	struct {
		/** 
		 * @nsq_maps: The per-cpu mapping of nsq_id -> nr_tasks. It records
		 * 1. the usage of nsqs in use by this cpu, recorded in the index
		 * 2. the number of tasks using this NSQ, recorded in the value.
		 */ 
		struct blk_blex_nsq_map *nsq_maps ____cacheline_aligned_in_smp;

		/** @nr_nmaps: The number of nmaps used. */
		int nr_nmaps;
	};
	
	/**
	 * @enqs: The array of size max_nqid containing exposed NQs.
	 */
	struct blk_blex_exposed_nq *enqs;

	atomic_t nr_updating_cpus;			/* The number of cpus that is updating the nq_reg fields. */
	u64 last_work_ns;					/* The kernel ns of the last time that the nq_reg works. */

	struct blk_blex_nq_group nq_groups[BLK_BLEX_PRIO_LEVEL];

	/** @online_hosts: The xarray of request queues sharing this NVMe SSD. */
	struct xarray online_hosts;
};

/**
 * blk_blex_alloc_and_init_vhw_ctxs() - Allocate and init the per-SQ vNQs for a NQ 
 * (i.e., HQ here).
 * @hctx: The NQ that the vNQs are attached to.
 * @set: The blk-mq tag set that records the information of the NVMe device.
 * @node: The NUMA node to allocate the vNQs.
 *
 * Return: negative number upon failure and 0 on success.
 */
int blk_blex_alloc_and_init_vnqs(struct blk_mq_hw_ctx *hctx, struct blk_mq_tag_set *set, int node);

/**
 * blk_blex_min_no_extra_sq_cqid() - Get the minimum NCQ ID that does not have extra
 * NSQs due to asymmetric between NSQs and NCQs.
 * @max_nqid: The number of NQs.
 * @nr_cqs: The number of NCQs.
 */
static inline unsigned int blk_blex_min_no_extra_sq_cqid(unsigned int max_nqid, unsigned int nr_cqs)
{
	unsigned int nr_has_extra_nsq, nr_ave_nsqs;

	nr_ave_nsqs = max_nqid / nr_cqs;	/* Number of average NSQs per NCQ. */
	nr_has_extra_nsq = max_nqid % nr_cqs; /* Number of NCQs that have one extra NSQ than average. */
	return nr_has_extra_nsq * (nr_ave_nsqs + 1) + 1;
}

/**
 * blk_blex_cqid_to_sqid() - Get the k-th NSQ attached to this NCQ given the maximum 
 * number of NQs and NCQs, this is a zero-based counter, i.e., k starts with 0.
 *
 * @cqid: The ID of NCQ
 * @k: The k-th NSQ attached to this NCQ.
 * @max_nqid: The maximum of NQs.
 * @nq_cqs: The number of NCQs.
 */
static inline unsigned int blk_blex_cqid_to_kth_sqid(unsigned int cqid, unsigned int k,
			unsigned int max_nqid, unsigned int nr_cqs)
{
	unsigned int nr_attached_nsqs;

	nr_attached_nsqs = cqid >= blk_blex_min_no_extra_sq_cqid(max_nqid, nr_cqs) ? 
		max_nqid / nr_cqs : max_nqid / nr_cqs + 1;
	k %= nr_attached_nsqs;
	return cqid + k;
}

/**
 * blk_blex_get_cq_nr_nsqs() - Get the number of NSQs attached to a NCQ.
 */
static inline unsigned int blk_blex_get_cq_nr_nsqs(unsigned int cqid, 
			unsigned int max_nqid, unsigned int nr_cqs)
{
	if (cqid < blk_blex_min_no_extra_sq_cqid(max_nqid, nr_cqs))
		return max_nqid / nr_cqs + 1;
	else
		return max_nqid / nr_cqs;
}

/**
 * blk_blex_cqid_to_sqid() - Get the k-th NCQ given the maximum number of NQs and 
 * NCQs. k is a **zero-based counter**, i.e., k starts with 0.
 * @k: The index of k to the NCQ, starts with 0 and is suggested not to exceed 
 * nr_cqs - 1, however, in any case k >= nr_cqs, it will be modulo down.
 * @max_nqid: The number of NQs.
 * @nr_cqs: The number of NCQs.
 */
static inline unsigned int blk_blex_get_kth_cqid(unsigned int k, 
			unsigned int max_nqid, unsigned int nr_cqs)
{
	unsigned int nr_has_extra_nsq, nr_ave_nsqs;
	
	k %= nr_cqs;
	nr_ave_nsqs = max_nqid / nr_cqs;	/* Number of average NSQs per NCQ. */
	nr_has_extra_nsq = max_nqid % nr_cqs; /* Number of NCQs that have one extra NSQ than average. */

	if (k < nr_has_extra_nsq)
		return k * (nr_ave_nsqs + 1) + 1;
	else
		return nr_has_extra_nsq + k * nr_ave_nsqs + 1;
}

/**
 * blk_blex_cqid_to_index() - Convert a given NCQ ID to its index within the whole NCQs.
 * We can get the kth NCQ's ID via blk_blex_get_kth_cqid() and this function is to get 
 * the 'k' for the NCQ with ID=cqid.
 * NOTE: the returned index is a zero-based value.
 * @cqid: the NCQ id.
 * @max_nqid: The number of NQs.
 * @nr_cqs: The number of NCQs.
 */
static inline unsigned int blk_blex_cqid_to_index(unsigned int cqid,
			unsigned int max_nqid, unsigned int nr_cqs)
{
	unsigned int min_cqid_without_extra_nsq = blk_blex_min_no_extra_sq_cqid(max_nqid, nr_cqs);
	unsigned int nr_ave_nsqs = max_nqid / nr_cqs;
	
	if (cqid <= min_cqid_without_extra_nsq)
		return cqid / (nr_ave_nsqs + 1);
	else
		return ((cqid - min_cqid_without_extra_nsq) / nr_ave_nsqs) + 
				(min_cqid_without_extra_nsq / (nr_ave_nsqs + 1));
}

/**
 * blk_blex_get_cqid() - Determine the corresponding NVMe completion queue 
 * given the qid, maximum of NQs, and number of completion queues.
 * The qid is a 1-based value, i.e., it starts counting from 1 as qid=0 is
 * used for admin queue.
 * @qid: ID of the NQ we want to determine the NCQ ID, itself could be NCQ or NSQ.
 * @max_nqid: The number of NVMe queues
 * @nr_cqs: The number of NCQs.
 */
static inline unsigned int blk_blex_get_cqid(unsigned int qid, 
			unsigned int max_nqid, unsigned int nr_cqs)
{
	unsigned int min_cqid_without_extra_nsq, nr_ave_nsqs;

	nr_ave_nsqs = max_nqid / nr_cqs;
	min_cqid_without_extra_nsq = blk_blex_min_no_extra_sq_cqid(max_nqid, nr_cqs);

	if (qid < min_cqid_without_extra_nsq)
		return ((qid - 1) / (nr_ave_nsqs + 1)) * (nr_ave_nsqs + 1) + 1;
	else
		return min_cqid_without_extra_nsq + ((qid - min_cqid_without_extra_nsq) / nr_ave_nsqs) * nr_ave_nsqs;
}

/**
 * blk_blex_get_tag_idx() - Get the corresponding tag index given the hctx_idx,
 * i.e., nqid = hctx_idx + 1, and the tag set.
 */
static inline int blk_blex_get_tag_idx(int hctx_idx, struct blk_mq_tag_set *set)
{
	unsigned int max_nqid = set->blex_info->max_nqid, nr_cqs = set->blex_info->nr_cqs;
	unsigned int cqid;
	cqid = blk_blex_get_cqid(hctx_idx + 1, max_nqid, nr_cqs);
	return blk_blex_cqid_to_index(cqid, max_nqid, nr_cqs);
}

static inline bool blk_blex_has_cq(unsigned int nqid, struct blk_mq_tag_set *set)
{
	return nqid == blk_blex_get_cqid(nqid, set->blex_info->max_nqid, set->blex_info->nr_cqs);
}

/**
 * blk_blex_nq_regulator_alloc_and_init() - Allocate and initialize the certain 
 * needed fields of the blk_blex NQ pool nq_reg.
 */
int blk_blex_nq_regulator_alloc_and_init(struct blk_blex_nq_regulator *nq_reg, int numa_node);

/** 
 * blk_blex_nq_regulator_free() - Free an in-use NQ pool nq_reg.
 */
void blk_blex_nq_regulator_free(struct blk_blex_nq_regulator *nq_reg);

/**
 * blk_blex_sched_random_nqid() - Get a random NQ ID from the NQ group of a certain priority.
 * @q: The pointer to the block device (i.e., request queue) 
 * @prio: The blk-blex defined priority
 * @type: The NQ type to fetch next using round-robin fashion, either NSQ or NCQ.
 */
int blk_blex_sched_random_nqid(struct blk_blex_nq_regulator *nq_reg, blk_blex_prio_t prio, enum blk_blex_nq_type_enum type);

/****** START OF NQREG-RELATED FUNCTION. ******/

/** Peek into the priority array and get the top NQ. */
#define blk_blex_peek_priority_array(prio_arr) \
({	\
	struct blk_blex_exposed_nq *__heap_top;	\
	rcu_read_lock(); 	\
	__heap_top = rcu_dereference((prio_arr)->top_nq);	\
	rcu_read_unlock();	\
	__heap_top; \
})

static inline struct blk_blex_exposed_nq *blk_blex_peek_recommend(struct blk_blex_nq_group *group)
{
	struct blk_blex_exposed_nq *ncq, *nsq;
	ncq = blk_blex_peek_priority_array(&group->prio_arr);
	if (ncq->encq->nr_attached_nsqs > 1)
		nsq = blk_blex_peek_priority_array(&ncq->encq->prio_arr);
	else
		nsq = ncq;
	return nsq;
}

/**
 * blk_blex_update_priority_array() - Update the priority array after being the top item
 * has been accessed.
 */
void blk_blex_update_priority_array(struct blk_blex_nq_priority_array *prio_arr,
	struct blk_blex_nq_regulator *nq_reg, int mru, int force_merit, enum blk_blex_nq_type_enum type);

/**
 * blk_blex_recommend_nsq() - Recommend an NSQ given the priority of the requests to be served.
 * @nq_reg: The NQ regulator of the underlying SSD.
 * @prio: The blex-defined priority for the recommended NSQ.
 * @round_robin: Whether to recommend NSQ in a simple round-robin style.
 * @ncq_mru: The number of MRU to decrement for NCQ prio_arr.
 * @nsq_mru: How many MRU to decrement for NSQ prio_arr.
 *
 * Return an pointer to the eNQ of the recommended NSQ on success and NULL on failure.
 */
struct blk_blex_exposed_nq *blk_blex_recommend_nsq(struct blk_blex_nq_regulator *nq_reg, 
	blk_blex_prio_t prio, bool round_robin, int ncq_mru, int nsq_mru);

/****** END OF NQREG-RELATED FUNCTION. ******/

/** COMPLETION PATH HANDLING. */
void blk_blex_complete_remote_call(void *info);
#endif
#endif

