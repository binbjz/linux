/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2016-2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2013 Red Hat
 * Author: Rob Clark <robdclark@gmail.com>
 */

#ifndef __MSM_KMS_H__
#define __MSM_KMS_H__

#include <linux/clk.h>
#include <linux/regulator/consumer.h>

#include "msm_drv.h"

#ifdef CONFIG_DRM_MSM_KMS

#define MAX_PLANE	4

/* As there are different display controller blocks depending on the
 * snapdragon version, the kms support is split out and the appropriate
 * implementation is loaded at runtime.  The kms module is responsible
 * for constructing the appropriate planes/crtcs/encoders/connectors.
 */
struct msm_kms_funcs {
	/* hw initialization: */
	int (*hw_init)(struct msm_kms *kms);
	/* irq handling: */
	void (*irq_preinstall)(struct msm_kms *kms);
	int (*irq_postinstall)(struct msm_kms *kms);
	void (*irq_uninstall)(struct msm_kms *kms);
	irqreturn_t (*irq)(struct msm_kms *kms);
	int (*enable_vblank)(struct msm_kms *kms, struct drm_crtc *crtc);
	void (*disable_vblank)(struct msm_kms *kms, struct drm_crtc *crtc);

	/*
	 * Atomic commit handling:
	 *
	 * Note that in the case of async commits, the funcs which take
	 * a crtc_mask (ie. ->flush_commit(), and ->complete_commit())
	 * might not be evenly balanced with ->prepare_commit(), however
	 * each crtc that effected by a ->prepare_commit() (potentially
	 * multiple times) will eventually (at end of vsync period) be
	 * flushed and completed.
	 *
	 * This has some implications about tracking of cleanup state,
	 * for example SMP blocks to release after commit completes.  Ie.
	 * cleanup state should be also duplicated in the various
	 * duplicate_state() methods, as the current cleanup state at
	 * ->complete_commit() time may have accumulated cleanup work
	 * from multiple commits.
	 */

	/**
	 * Enable/disable power/clks needed for hw access done in other
	 * commit related methods.
	 *
	 * If mdp4 is migrated to runpm, we could probably drop these
	 * and use runpm directly.
	 */
	void (*enable_commit)(struct msm_kms *kms);
	void (*disable_commit)(struct msm_kms *kms);

	/**
	 * @check_mode_changed:
	 *
	 * Verify if the commit requires a full modeset on one of CRTCs.
	 */
	int (*check_mode_changed)(struct msm_kms *kms, struct drm_atomic_state *state);

	/**
	 * Prepare for atomic commit.  This is called after any previous
	 * (async or otherwise) commit has completed.
	 */
	void (*prepare_commit)(struct msm_kms *kms, struct drm_atomic_state *state);

	/**
	 * Flush an atomic commit.  This is called after the hardware
	 * updates have already been pushed down to effected planes/
	 * crtcs/encoders/connectors.
	 */
	void (*flush_commit)(struct msm_kms *kms, unsigned crtc_mask);

	/**
	 * Wait for any in-progress flush to complete on the specified
	 * crtcs.  This should not block if there is no in-progress
	 * commit (ie. don't just wait for a vblank), as it will also
	 * be called before ->prepare_commit() to ensure any potential
	 * "async" commit has completed.
	 */
	void (*wait_flush)(struct msm_kms *kms, unsigned crtc_mask);

	/**
	 * Clean up after commit is completed.  This is called after
	 * ->wait_flush(), to give the backend a chance to do any
	 * post-commit cleanup.
	 */
	void (*complete_commit)(struct msm_kms *kms, unsigned crtc_mask);

	/*
	 * Format handling:
	 */

	/* misc: */
	long (*round_pixclk)(struct msm_kms *kms, unsigned long rate,
			struct drm_encoder *encoder);
	/* cleanup: */
	void (*destroy)(struct msm_kms *kms);

	/* snapshot: */
	void (*snapshot)(struct msm_disp_state *disp_state, struct msm_kms *kms);

#ifdef CONFIG_DEBUG_FS
	/* debugfs: */
	int (*debugfs_init)(struct msm_kms *kms, struct drm_minor *minor);
#endif
};

struct msm_kms;

/*
 * A per-crtc timer for pending async atomic flushes.  Scheduled to expire
 * shortly before vblank to flush pending async updates.
 */
struct msm_pending_timer {
	struct msm_hrtimer_work work;
	struct kthread_worker *worker;
	struct msm_kms *kms;
	unsigned crtc_idx;
};

/* Commit/Event thread specific structure */
struct msm_drm_thread {
	struct drm_device *dev;
	struct kthread_worker *worker;
};

struct msm_kms {
	const struct msm_kms_funcs *funcs;
	struct drm_device *dev;

	struct hdmi *hdmi;

	struct msm_dsi *dsi[MSM_DSI_CONTROLLER_COUNT];

	struct msm_dp *dp[MSM_DP_CONTROLLER_COUNT];

	/* irq number to be passed on to msm_irq_install */
	int irq;
	bool irq_requested;

	/* rate limit the snapshot capture to once per attach */
	atomic_t fault_snapshot_capture;

	/* mapper-id used to request GEM buffer mapped for scanout: */
	struct drm_gpuvm *vm;

	/* disp snapshot support */
	struct kthread_worker *dump_worker;
	struct kthread_work dump_work;
	struct mutex dump_mutex;

	/*
	 * For async commit, where ->flush_commit() and later happens
	 * from the crtc's pending_timer close to end of the frame:
	 */
	struct mutex commit_lock[MAX_CRTCS];
	unsigned pending_crtc_mask;
	struct msm_pending_timer pending_timers[MAX_CRTCS];

	struct workqueue_struct *wq;
	struct msm_drm_thread event_thread[MAX_CRTCS];
};

static inline int msm_kms_init(struct msm_kms *kms,
		const struct msm_kms_funcs *funcs)
{
	unsigned i, ret;

	for (i = 0; i < ARRAY_SIZE(kms->commit_lock); i++)
		mutex_init(&kms->commit_lock[i]);

	kms->funcs = funcs;

	kms->wq = alloc_ordered_workqueue("msm", 0);
	if (!kms->wq)
		return -ENOMEM;

	for (i = 0; i < ARRAY_SIZE(kms->pending_timers); i++) {
		ret = msm_atomic_init_pending_timer(&kms->pending_timers[i], kms, i);
		if (ret) {
			return ret;
		}
	}

	return 0;
}

static inline void msm_kms_destroy(struct msm_kms *kms)
{
	unsigned i;

	for (i = 0; i < ARRAY_SIZE(kms->pending_timers); i++)
		msm_atomic_destroy_pending_timer(&kms->pending_timers[i]);

	destroy_workqueue(kms->wq);
}

#define for_each_crtc_mask(dev, crtc, crtc_mask) \
	drm_for_each_crtc(crtc, dev) \
		for_each_if (drm_crtc_mask(crtc) & (crtc_mask))

#define for_each_crtc_mask_reverse(dev, crtc, crtc_mask) \
	drm_for_each_crtc_reverse(crtc, dev) \
		for_each_if (drm_crtc_mask(crtc) & (crtc_mask))

int msm_drm_kms_init(struct device *dev, const struct drm_driver *drv);
void msm_drm_kms_post_init(struct device *dev);
void msm_drm_kms_unregister(struct device *dev);
void msm_drm_kms_uninit(struct device *dev);

#else /* ! CONFIG_DRM_MSM_KMS */

static inline int msm_drm_kms_init(struct device *dev, const struct drm_driver *drv)
{
	return -ENODEV;
}

static inline void msm_drm_kms_post_init(struct device *dev)
{
}

static inline void msm_drm_kms_unregister(struct device *dev)
{
}

static inline void msm_drm_kms_uninit(struct device *dev)
{
}

#endif

#endif /* __MSM_KMS_H__ */
