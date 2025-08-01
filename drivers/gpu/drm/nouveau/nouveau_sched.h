/* SPDX-License-Identifier: MIT */

#ifndef NOUVEAU_SCHED_H
#define NOUVEAU_SCHED_H

#include <linux/types.h>

#include <drm/drm_gpuvm.h>
#include <drm/gpu_scheduler.h>

#include "nouveau_drv.h"

#define to_nouveau_job(sched_job)		\
		container_of((sched_job), struct nouveau_job, base)

struct nouveau_job_ops;

enum nouveau_job_state {
	NOUVEAU_JOB_UNINITIALIZED = 0,
	NOUVEAU_JOB_INITIALIZED,
	NOUVEAU_JOB_SUBMIT_SUCCESS,
	NOUVEAU_JOB_SUBMIT_FAILED,
	NOUVEAU_JOB_RUN_SUCCESS,
	NOUVEAU_JOB_RUN_FAILED,
};

struct nouveau_job_args {
	struct drm_file *file_priv;
	struct nouveau_sched *sched;
	u32 credits;

	enum dma_resv_usage resv_usage;
	bool sync;

	struct {
		struct drm_nouveau_sync *s;
		u32 count;
	} in_sync;

	struct {
		struct drm_nouveau_sync *s;
		u32 count;
	} out_sync;

	const struct nouveau_job_ops *ops;
};

struct nouveau_job {
	struct drm_sched_job base;

	enum nouveau_job_state state;

	struct nouveau_sched *sched;
	struct list_head entry;

	struct drm_file *file_priv;
	struct nouveau_cli *cli;

	enum dma_resv_usage resv_usage;
	struct dma_fence *done_fence;

	bool sync;

	struct {
		struct drm_nouveau_sync *data;
		u32 count;
	} in_sync;

	struct {
		struct drm_nouveau_sync *data;
		struct drm_syncobj **objs;
		struct dma_fence_chain **chains;
		u32 count;
	} out_sync;

	const struct nouveau_job_ops {
		/* If .submit() returns without any error, it is guaranteed that
		 * armed_submit() is called.
		 */
		int (*submit)(struct nouveau_job *, struct drm_gpuvm_exec *);
		void (*armed_submit)(struct nouveau_job *, struct drm_gpuvm_exec *);
		struct dma_fence *(*run)(struct nouveau_job *);
		void (*free)(struct nouveau_job *);
		enum drm_gpu_sched_stat (*timeout)(struct nouveau_job *);
	} *ops;
};

int nouveau_job_ucopy_syncs(struct nouveau_job_args *args,
			    u32 inc, u64 ins,
			    u32 outc, u64 outs);

int nouveau_job_init(struct nouveau_job *job,
		     struct nouveau_job_args *args);
void nouveau_job_fini(struct nouveau_job *job);
int nouveau_job_submit(struct nouveau_job *job);
void nouveau_job_done(struct nouveau_job *job);
void nouveau_job_free(struct nouveau_job *job);

struct nouveau_sched {
	struct drm_gpu_scheduler base;
	struct drm_sched_entity entity;
	struct workqueue_struct *wq;
	struct mutex mutex;

	struct {
		struct list_head head;
		spinlock_t lock;
	} job_list;
};

int nouveau_sched_create(struct nouveau_sched **psched, struct nouveau_drm *drm,
			 struct workqueue_struct *wq, u32 credit_limit);
void nouveau_sched_destroy(struct nouveau_sched **psched);

#endif
