// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * raid5.c : Multiple Devices driver for Linux
 *	   Copyright (C) 1996, 1997 Ingo Molnar, Miguel de Icaza, Gadi Oxman
 *	   Copyright (C) 1999, 2000 Ingo Molnar
 *	   Copyright (C) 2002, 2003 H. Peter Anvin
 *
 * RAID-4/5/6 management functions.
 * Thanks to Penguin Computing for making the RAID-6 development possible
 * by donating a test server!
 */

/*
 * BITMAP UNPLUGGING:
 *
 * The sequencing for updating the bitmap reliably is a little
 * subtle (and I got it wrong the first time) so it deserves some
 * explanation.
 *
 * We group bitmap updates into batches.  Each batch has a number.
 * We may write out several batches at once, but that isn't very important.
 * conf->seq_write is the number of the last batch successfully written.
 * conf->seq_flush is the number of the last batch that was closed to
 *    new additions.
 * When we discover that we will need to write to any block in a stripe
 * (in add_stripe_bio) we update the in-memory bitmap and record in sh->bm_seq
 * the number of the batch it will be in. This is seq_flush+1.
 * When we are ready to do a write, if that batch hasn't been written yet,
 *   we plug the array and queue the stripe for later.
 * When an unplug happens, we increment bm_flush, thus closing the current
 *   batch.
 * When we notice that bm_flush > bm_write, we write out all pending updates
 * to the bitmap, and advance bm_write to where bm_flush was.
 * This may occasionally write a bit out twice, but is sure never to
 * miss any bits.
 */

#include <linux/blkdev.h>
#include <linux/kthread.h>
#include <linux/raid/pq.h>
#include <linux/async_tx.h>
#include <linux/module.h>
#include <linux/async.h>
#include <linux/seq_file.h>
#include <linux/cpu.h>
#include <linux/slab.h>
#include <linux/ratelimit.h>
#include <linux/nodemask.h>

#include <trace/events/block.h>
#include <linux/list_sort.h>

#include "md.h"
#include "raid5.h"
#include "raid0.h"
#include "md-bitmap.h"
#include "raid5-log.h"

#define UNSUPPORTED_MDDEV_FLAGS	(1L << MD_FAILFAST_SUPPORTED)

#define cpu_to_group(cpu) cpu_to_node(cpu)
#define ANY_GROUP NUMA_NO_NODE

#define RAID5_MAX_REQ_STRIPES 256

static bool devices_handle_discard_safely = false;
module_param(devices_handle_discard_safely, bool, 0644);
MODULE_PARM_DESC(devices_handle_discard_safely,
		 "Set to Y if all devices in each array reliably return zeroes on reads from discarded regions");
static struct workqueue_struct *raid5_wq;

static void raid5_quiesce(struct mddev *mddev, int quiesce);

static inline struct hlist_head *stripe_hash(struct r5conf *conf, sector_t sect)
{
	int hash = (sect >> RAID5_STRIPE_SHIFT(conf)) & HASH_MASK;
	return &conf->stripe_hashtbl[hash];
}

static inline int stripe_hash_locks_hash(struct r5conf *conf, sector_t sect)
{
	return (sect >> RAID5_STRIPE_SHIFT(conf)) & STRIPE_HASH_LOCKS_MASK;
}

static inline void lock_device_hash_lock(struct r5conf *conf, int hash)
	__acquires(&conf->device_lock)
{
	spin_lock_irq(conf->hash_locks + hash);
	spin_lock(&conf->device_lock);
}

static inline void unlock_device_hash_lock(struct r5conf *conf, int hash)
	__releases(&conf->device_lock)
{
	spin_unlock(&conf->device_lock);
	spin_unlock_irq(conf->hash_locks + hash);
}

static inline void lock_all_device_hash_locks_irq(struct r5conf *conf)
	__acquires(&conf->device_lock)
{
	int i;
	spin_lock_irq(conf->hash_locks);
	for (i = 1; i < NR_STRIPE_HASH_LOCKS; i++)
		spin_lock_nest_lock(conf->hash_locks + i, conf->hash_locks);
	spin_lock(&conf->device_lock);
}

static inline void unlock_all_device_hash_locks_irq(struct r5conf *conf)
	__releases(&conf->device_lock)
{
	int i;
	spin_unlock(&conf->device_lock);
	for (i = NR_STRIPE_HASH_LOCKS - 1; i; i--)
		spin_unlock(conf->hash_locks + i);
	spin_unlock_irq(conf->hash_locks);
}

/* Find first data disk in a raid6 stripe */
static inline int raid6_d0(struct stripe_head *sh)
{
	if (sh->ddf_layout)
		/* ddf always start from first device */
		return 0;
	/* md starts just after Q block */
	if (sh->qd_idx == sh->disks - 1)
		return 0;
	else
		return sh->qd_idx + 1;
}
static inline int raid6_next_disk(int disk, int raid_disks)
{
	disk++;
	return (disk < raid_disks) ? disk : 0;
}

/* When walking through the disks in a raid5, starting at raid6_d0,
 * We need to map each disk to a 'slot', where the data disks are slot
 * 0 .. raid_disks-3, the parity disk is raid_disks-2 and the Q disk
 * is raid_disks-1.  This help does that mapping.
 */
static int raid6_idx_to_slot(int idx, struct stripe_head *sh,
			     int *count, int syndrome_disks)
{
	int slot = *count;

	if (sh->ddf_layout)
		(*count)++;
	if (idx == sh->pd_idx)
		return syndrome_disks;
	if (idx == sh->qd_idx)
		return syndrome_disks + 1;
	if (!sh->ddf_layout)
		(*count)++;
	return slot;
}

static void print_raid5_conf(struct r5conf *conf);

static int stripe_operations_active(struct stripe_head *sh)
{
	return sh->check_state || sh->reconstruct_state ||
	       test_bit(STRIPE_BIOFILL_RUN, &sh->state) ||
	       test_bit(STRIPE_COMPUTE_RUN, &sh->state);
}

static bool stripe_is_lowprio(struct stripe_head *sh)
{
	return (test_bit(STRIPE_R5C_FULL_STRIPE, &sh->state) ||
		test_bit(STRIPE_R5C_PARTIAL_STRIPE, &sh->state)) &&
	       !test_bit(STRIPE_R5C_CACHING, &sh->state);
}

static void raid5_wakeup_stripe_thread(struct stripe_head *sh)
	__must_hold(&sh->raid_conf->device_lock)
{
	struct r5conf *conf = sh->raid_conf;
	struct r5worker_group *group;
	int thread_cnt;
	int i, cpu = sh->cpu;

	if (!cpu_online(cpu)) {
		cpu = cpumask_any(cpu_online_mask);
		sh->cpu = cpu;
	}

	if (list_empty(&sh->lru)) {
		struct r5worker_group *group;
		group = conf->worker_groups + cpu_to_group(cpu);
		if (stripe_is_lowprio(sh))
			list_add_tail(&sh->lru, &group->loprio_list);
		else
			list_add_tail(&sh->lru, &group->handle_list);
		group->stripes_cnt++;
		sh->group = group;
	}

	if (conf->worker_cnt_per_group == 0) {
		md_wakeup_thread(conf->mddev->thread);
		return;
	}

	group = conf->worker_groups + cpu_to_group(sh->cpu);

	group->workers[0].working = true;
	/* at least one worker should run to avoid race */
	queue_work_on(sh->cpu, raid5_wq, &group->workers[0].work);

	thread_cnt = group->stripes_cnt / MAX_STRIPE_BATCH - 1;
	/* wakeup more workers */
	for (i = 1; i < conf->worker_cnt_per_group && thread_cnt > 0; i++) {
		if (group->workers[i].working == false) {
			group->workers[i].working = true;
			queue_work_on(sh->cpu, raid5_wq,
				      &group->workers[i].work);
			thread_cnt--;
		}
	}
}

static void do_release_stripe(struct r5conf *conf, struct stripe_head *sh,
			      struct list_head *temp_inactive_list)
	__must_hold(&conf->device_lock)
{
	int i;
	int injournal = 0;	/* number of date pages with R5_InJournal */

	BUG_ON(!list_empty(&sh->lru));
	BUG_ON(atomic_read(&conf->active_stripes)==0);

	if (r5c_is_writeback(conf->log))
		for (i = sh->disks; i--; )
			if (test_bit(R5_InJournal, &sh->dev[i].flags))
				injournal++;
	/*
	 * In the following cases, the stripe cannot be released to cached
	 * lists. Therefore, we make the stripe write out and set
	 * STRIPE_HANDLE:
	 *   1. when quiesce in r5c write back;
	 *   2. when resync is requested fot the stripe.
	 */
	if (test_bit(STRIPE_SYNC_REQUESTED, &sh->state) ||
	    (conf->quiesce && r5c_is_writeback(conf->log) &&
	     !test_bit(STRIPE_HANDLE, &sh->state) && injournal != 0)) {
		if (test_bit(STRIPE_R5C_CACHING, &sh->state))
			r5c_make_stripe_write_out(sh);
		set_bit(STRIPE_HANDLE, &sh->state);
	}

	if (test_bit(STRIPE_HANDLE, &sh->state)) {
		if (test_bit(STRIPE_DELAYED, &sh->state) &&
		    !test_bit(STRIPE_PREREAD_ACTIVE, &sh->state))
			list_add_tail(&sh->lru, &conf->delayed_list);
		else if (test_bit(STRIPE_BIT_DELAY, &sh->state) &&
			   sh->bm_seq - conf->seq_write > 0)
			list_add_tail(&sh->lru, &conf->bitmap_list);
		else {
			clear_bit(STRIPE_DELAYED, &sh->state);
			clear_bit(STRIPE_BIT_DELAY, &sh->state);
			if (conf->worker_cnt_per_group == 0) {
				if (stripe_is_lowprio(sh))
					list_add_tail(&sh->lru,
							&conf->loprio_list);
				else
					list_add_tail(&sh->lru,
							&conf->handle_list);
			} else {
				raid5_wakeup_stripe_thread(sh);
				return;
			}
		}
		md_wakeup_thread(conf->mddev->thread);
	} else {
		BUG_ON(stripe_operations_active(sh));
		if (test_and_clear_bit(STRIPE_PREREAD_ACTIVE, &sh->state))
			if (atomic_dec_return(&conf->preread_active_stripes)
			    < IO_THRESHOLD)
				md_wakeup_thread(conf->mddev->thread);
		atomic_dec(&conf->active_stripes);
		if (!test_bit(STRIPE_EXPANDING, &sh->state)) {
			if (!r5c_is_writeback(conf->log))
				list_add_tail(&sh->lru, temp_inactive_list);
			else {
				WARN_ON(test_bit(R5_InJournal, &sh->dev[sh->pd_idx].flags));
				if (injournal == 0)
					list_add_tail(&sh->lru, temp_inactive_list);
				else if (injournal == conf->raid_disks - conf->max_degraded) {
					/* full stripe */
					if (!test_and_set_bit(STRIPE_R5C_FULL_STRIPE, &sh->state))
						atomic_inc(&conf->r5c_cached_full_stripes);
					if (test_and_clear_bit(STRIPE_R5C_PARTIAL_STRIPE, &sh->state))
						atomic_dec(&conf->r5c_cached_partial_stripes);
					list_add_tail(&sh->lru, &conf->r5c_full_stripe_list);
					r5c_check_cached_full_stripe(conf);
				} else
					/*
					 * STRIPE_R5C_PARTIAL_STRIPE is set in
					 * r5c_try_caching_write(). No need to
					 * set it again.
					 */
					list_add_tail(&sh->lru, &conf->r5c_partial_stripe_list);
			}
		}
	}
}

static void __release_stripe(struct r5conf *conf, struct stripe_head *sh,
			     struct list_head *temp_inactive_list)
	__must_hold(&conf->device_lock)
{
	if (atomic_dec_and_test(&sh->count))
		do_release_stripe(conf, sh, temp_inactive_list);
}

/*
 * @hash could be NR_STRIPE_HASH_LOCKS, then we have a list of inactive_list
 *
 * Be careful: Only one task can add/delete stripes from temp_inactive_list at
 * given time. Adding stripes only takes device lock, while deleting stripes
 * only takes hash lock.
 */
static void release_inactive_stripe_list(struct r5conf *conf,
					 struct list_head *temp_inactive_list,
					 int hash)
{
	int size;
	bool do_wakeup = false;
	unsigned long flags;

	if (hash == NR_STRIPE_HASH_LOCKS) {
		size = NR_STRIPE_HASH_LOCKS;
		hash = NR_STRIPE_HASH_LOCKS - 1;
	} else
		size = 1;
	while (size) {
		struct list_head *list = &temp_inactive_list[size - 1];

		/*
		 * We don't hold any lock here yet, raid5_get_active_stripe() might
		 * remove stripes from the list
		 */
		if (!list_empty_careful(list)) {
			spin_lock_irqsave(conf->hash_locks + hash, flags);
			if (list_empty(conf->inactive_list + hash) &&
			    !list_empty(list))
				atomic_dec(&conf->empty_inactive_list_nr);
			list_splice_tail_init(list, conf->inactive_list + hash);
			do_wakeup = true;
			spin_unlock_irqrestore(conf->hash_locks + hash, flags);
		}
		size--;
		hash--;
	}

	if (do_wakeup) {
		wake_up(&conf->wait_for_stripe);
		if (atomic_read(&conf->active_stripes) == 0)
			wake_up(&conf->wait_for_quiescent);
		if (conf->retry_read_aligned)
			md_wakeup_thread(conf->mddev->thread);
	}
}

static int release_stripe_list(struct r5conf *conf,
			       struct list_head *temp_inactive_list)
	__must_hold(&conf->device_lock)
{
	struct stripe_head *sh, *t;
	int count = 0;
	struct llist_node *head;

	head = llist_del_all(&conf->released_stripes);
	head = llist_reverse_order(head);
	llist_for_each_entry_safe(sh, t, head, release_list) {
		int hash;

		/* sh could be readded after STRIPE_ON_RELEASE_LIST is cleard */
		smp_mb();
		clear_bit(STRIPE_ON_RELEASE_LIST, &sh->state);
		/*
		 * Don't worry the bit is set here, because if the bit is set
		 * again, the count is always > 1. This is true for
		 * STRIPE_ON_UNPLUG_LIST bit too.
		 */
		hash = sh->hash_lock_index;
		__release_stripe(conf, sh, &temp_inactive_list[hash]);
		count++;
	}

	return count;
}

void raid5_release_stripe(struct stripe_head *sh)
{
	struct r5conf *conf = sh->raid_conf;
	unsigned long flags;
	struct list_head list;
	int hash;
	bool wakeup;

	/* Avoid release_list until the last reference.
	 */
	if (atomic_add_unless(&sh->count, -1, 1))
		return;

	if (unlikely(!conf->mddev->thread) ||
		test_and_set_bit(STRIPE_ON_RELEASE_LIST, &sh->state))
		goto slow_path;
	wakeup = llist_add(&sh->release_list, &conf->released_stripes);
	if (wakeup)
		md_wakeup_thread(conf->mddev->thread);
	return;
slow_path:
	/* we are ok here if STRIPE_ON_RELEASE_LIST is set or not */
	if (atomic_dec_and_lock_irqsave(&sh->count, &conf->device_lock, flags)) {
		INIT_LIST_HEAD(&list);
		hash = sh->hash_lock_index;
		do_release_stripe(conf, sh, &list);
		spin_unlock_irqrestore(&conf->device_lock, flags);
		release_inactive_stripe_list(conf, &list, hash);
	}
}

static inline void remove_hash(struct stripe_head *sh)
{
	pr_debug("remove_hash(), stripe %llu\n",
		(unsigned long long)sh->sector);

	hlist_del_init(&sh->hash);
}

static inline void insert_hash(struct r5conf *conf, struct stripe_head *sh)
{
	struct hlist_head *hp = stripe_hash(conf, sh->sector);

	pr_debug("insert_hash(), stripe %llu\n",
		(unsigned long long)sh->sector);

	hlist_add_head(&sh->hash, hp);
}

/* find an idle stripe, make sure it is unhashed, and return it. */
static struct stripe_head *get_free_stripe(struct r5conf *conf, int hash)
{
	struct stripe_head *sh = NULL;
	struct list_head *first;

	if (list_empty(conf->inactive_list + hash))
		goto out;
	first = (conf->inactive_list + hash)->next;
	sh = list_entry(first, struct stripe_head, lru);
	list_del_init(first);
	remove_hash(sh);
	atomic_inc(&conf->active_stripes);
	BUG_ON(hash != sh->hash_lock_index);
	if (list_empty(conf->inactive_list + hash))
		atomic_inc(&conf->empty_inactive_list_nr);
out:
	return sh;
}

#if PAGE_SIZE != DEFAULT_STRIPE_SIZE
static void free_stripe_pages(struct stripe_head *sh)
{
	int i;
	struct page *p;

	/* Have not allocate page pool */
	if (!sh->pages)
		return;

	for (i = 0; i < sh->nr_pages; i++) {
		p = sh->pages[i];
		if (p)
			put_page(p);
		sh->pages[i] = NULL;
	}
}

static int alloc_stripe_pages(struct stripe_head *sh, gfp_t gfp)
{
	int i;
	struct page *p;

	for (i = 0; i < sh->nr_pages; i++) {
		/* The page have allocated. */
		if (sh->pages[i])
			continue;

		p = alloc_page(gfp);
		if (!p) {
			free_stripe_pages(sh);
			return -ENOMEM;
		}
		sh->pages[i] = p;
	}
	return 0;
}

static int
init_stripe_shared_pages(struct stripe_head *sh, struct r5conf *conf, int disks)
{
	int nr_pages, cnt;

	if (sh->pages)
		return 0;

	/* Each of the sh->dev[i] need one conf->stripe_size */
	cnt = PAGE_SIZE / conf->stripe_size;
	nr_pages = (disks + cnt - 1) / cnt;

	sh->pages = kcalloc(nr_pages, sizeof(struct page *), GFP_KERNEL);
	if (!sh->pages)
		return -ENOMEM;
	sh->nr_pages = nr_pages;
	sh->stripes_per_page = cnt;
	return 0;
}
#endif

static void shrink_buffers(struct stripe_head *sh)
{
	int i;
	int num = sh->raid_conf->pool_size;

#if PAGE_SIZE == DEFAULT_STRIPE_SIZE
	for (i = 0; i < num ; i++) {
		struct page *p;

		WARN_ON(sh->dev[i].page != sh->dev[i].orig_page);
		p = sh->dev[i].page;
		if (!p)
			continue;
		sh->dev[i].page = NULL;
		put_page(p);
	}
#else
	for (i = 0; i < num; i++)
		sh->dev[i].page = NULL;
	free_stripe_pages(sh); /* Free pages */
#endif
}

static int grow_buffers(struct stripe_head *sh, gfp_t gfp)
{
	int i;
	int num = sh->raid_conf->pool_size;

#if PAGE_SIZE == DEFAULT_STRIPE_SIZE
	for (i = 0; i < num; i++) {
		struct page *page;

		if (!(page = alloc_page(gfp))) {
			return 1;
		}
		sh->dev[i].page = page;
		sh->dev[i].orig_page = page;
		sh->dev[i].offset = 0;
	}
#else
	if (alloc_stripe_pages(sh, gfp))
		return -ENOMEM;

	for (i = 0; i < num; i++) {
		sh->dev[i].page = raid5_get_dev_page(sh, i);
		sh->dev[i].orig_page = sh->dev[i].page;
		sh->dev[i].offset = raid5_get_page_offset(sh, i);
	}
#endif
	return 0;
}

static void stripe_set_idx(sector_t stripe, struct r5conf *conf, int previous,
			    struct stripe_head *sh);

static void init_stripe(struct stripe_head *sh, sector_t sector, int previous)
{
	struct r5conf *conf = sh->raid_conf;
	int i, seq;

	BUG_ON(atomic_read(&sh->count) != 0);
	BUG_ON(test_bit(STRIPE_HANDLE, &sh->state));
	BUG_ON(stripe_operations_active(sh));
	BUG_ON(sh->batch_head);

	pr_debug("init_stripe called, stripe %llu\n",
		(unsigned long long)sector);
retry:
	seq = read_seqcount_begin(&conf->gen_lock);
	sh->generation = conf->generation - previous;
	sh->disks = previous ? conf->previous_raid_disks : conf->raid_disks;
	sh->sector = sector;
	stripe_set_idx(sector, conf, previous, sh);
	sh->state = 0;

	for (i = sh->disks; i--; ) {
		struct r5dev *dev = &sh->dev[i];

		if (dev->toread || dev->read || dev->towrite || dev->written ||
		    test_bit(R5_LOCKED, &dev->flags)) {
			pr_err("sector=%llx i=%d %p %p %p %p %d\n",
			       (unsigned long long)sh->sector, i, dev->toread,
			       dev->read, dev->towrite, dev->written,
			       test_bit(R5_LOCKED, &dev->flags));
			WARN_ON(1);
		}
		dev->flags = 0;
		dev->sector = raid5_compute_blocknr(sh, i, previous);
	}
	if (read_seqcount_retry(&conf->gen_lock, seq))
		goto retry;
	sh->overwrite_disks = 0;
	insert_hash(conf, sh);
	sh->cpu = smp_processor_id();
	set_bit(STRIPE_BATCH_READY, &sh->state);
}

static struct stripe_head *__find_stripe(struct r5conf *conf, sector_t sector,
					 short generation)
{
	struct stripe_head *sh;

	pr_debug("__find_stripe, sector %llu\n", (unsigned long long)sector);
	hlist_for_each_entry(sh, stripe_hash(conf, sector), hash)
		if (sh->sector == sector && sh->generation == generation)
			return sh;
	pr_debug("__stripe %llu not in cache\n", (unsigned long long)sector);
	return NULL;
}

static struct stripe_head *find_get_stripe(struct r5conf *conf,
		sector_t sector, short generation, int hash)
{
	int inc_empty_inactive_list_flag;
	struct stripe_head *sh;

	sh = __find_stripe(conf, sector, generation);
	if (!sh)
		return NULL;

	if (atomic_inc_not_zero(&sh->count))
		return sh;

	/*
	 * Slow path. The reference count is zero which means the stripe must
	 * be on a list (sh->lru). Must remove the stripe from the list that
	 * references it with the device_lock held.
	 */

	spin_lock(&conf->device_lock);
	if (!atomic_read(&sh->count)) {
		if (!test_bit(STRIPE_HANDLE, &sh->state))
			atomic_inc(&conf->active_stripes);
		BUG_ON(list_empty(&sh->lru) &&
		       !test_bit(STRIPE_EXPANDING, &sh->state));
		inc_empty_inactive_list_flag = 0;
		if (!list_empty(conf->inactive_list + hash))
			inc_empty_inactive_list_flag = 1;
		list_del_init(&sh->lru);
		if (list_empty(conf->inactive_list + hash) &&
		    inc_empty_inactive_list_flag)
			atomic_inc(&conf->empty_inactive_list_nr);
		if (sh->group) {
			sh->group->stripes_cnt--;
			sh->group = NULL;
		}
	}
	atomic_inc(&sh->count);
	spin_unlock(&conf->device_lock);

	return sh;
}

/*
 * Need to check if array has failed when deciding whether to:
 *  - start an array
 *  - remove non-faulty devices
 *  - add a spare
 *  - allow a reshape
 * This determination is simple when no reshape is happening.
 * However if there is a reshape, we need to carefully check
 * both the before and after sections.
 * This is because some failed devices may only affect one
 * of the two sections, and some non-in_sync devices may
 * be insync in the section most affected by failed devices.
 *
 * Most calls to this function hold &conf->device_lock. Calls
 * in raid5_run() do not require the lock as no other threads
 * have been started yet.
 */
int raid5_calc_degraded(struct r5conf *conf)
{
	int degraded, degraded2;
	int i;

	degraded = 0;
	for (i = 0; i < conf->previous_raid_disks; i++) {
		struct md_rdev *rdev = READ_ONCE(conf->disks[i].rdev);

		if (rdev && test_bit(Faulty, &rdev->flags))
			rdev = READ_ONCE(conf->disks[i].replacement);
		if (!rdev || test_bit(Faulty, &rdev->flags))
			degraded++;
		else if (test_bit(In_sync, &rdev->flags))
			;
		else
			/* not in-sync or faulty.
			 * If the reshape increases the number of devices,
			 * this is being recovered by the reshape, so
			 * this 'previous' section is not in_sync.
			 * If the number of devices is being reduced however,
			 * the device can only be part of the array if
			 * we are reverting a reshape, so this section will
			 * be in-sync.
			 */
			if (conf->raid_disks >= conf->previous_raid_disks)
				degraded++;
	}
	if (conf->raid_disks == conf->previous_raid_disks)
		return degraded;
	degraded2 = 0;
	for (i = 0; i < conf->raid_disks; i++) {
		struct md_rdev *rdev = READ_ONCE(conf->disks[i].rdev);

		if (rdev && test_bit(Faulty, &rdev->flags))
			rdev = READ_ONCE(conf->disks[i].replacement);
		if (!rdev || test_bit(Faulty, &rdev->flags))
			degraded2++;
		else if (test_bit(In_sync, &rdev->flags))
			;
		else
			/* not in-sync or faulty.
			 * If reshape increases the number of devices, this
			 * section has already been recovered, else it
			 * almost certainly hasn't.
			 */
			if (conf->raid_disks <= conf->previous_raid_disks)
				degraded2++;
	}
	if (degraded2 > degraded)
		return degraded2;
	return degraded;
}

static bool has_failed(struct r5conf *conf)
{
	int degraded = conf->mddev->degraded;

	if (test_bit(MD_BROKEN, &conf->mddev->flags))
		return true;

	if (conf->mddev->reshape_position != MaxSector)
		degraded = raid5_calc_degraded(conf);

	return degraded > conf->max_degraded;
}

enum stripe_result {
	STRIPE_SUCCESS = 0,
	STRIPE_RETRY,
	STRIPE_SCHEDULE_AND_RETRY,
	STRIPE_FAIL,
	STRIPE_WAIT_RESHAPE,
};

struct stripe_request_ctx {
	/* a reference to the last stripe_head for batching */
	struct stripe_head *batch_last;

	/* first sector in the request */
	sector_t first_sector;

	/* last sector in the request */
	sector_t last_sector;

	/*
	 * bitmap to track stripe sectors that have been added to stripes
	 * add one to account for unaligned requests
	 */
	DECLARE_BITMAP(sectors_to_do, RAID5_MAX_REQ_STRIPES + 1);

	/* the request had REQ_PREFLUSH, cleared after the first stripe_head */
	bool do_flush;
};

/*
 * Block until another thread clears R5_INACTIVE_BLOCKED or
 * there are fewer than 3/4 the maximum number of active stripes
 * and there is an inactive stripe available.
 */
static bool is_inactive_blocked(struct r5conf *conf, int hash)
{
	if (list_empty(conf->inactive_list + hash))
		return false;

	if (!test_bit(R5_INACTIVE_BLOCKED, &conf->cache_state))
		return true;

	return (atomic_read(&conf->active_stripes) <
		(conf->max_nr_stripes * 3 / 4));
}

struct stripe_head *raid5_get_active_stripe(struct r5conf *conf,
		struct stripe_request_ctx *ctx, sector_t sector,
		unsigned int flags)
{
	struct stripe_head *sh;
	int hash = stripe_hash_locks_hash(conf, sector);
	int previous = !!(flags & R5_GAS_PREVIOUS);

	pr_debug("get_stripe, sector %llu\n", (unsigned long long)sector);

	spin_lock_irq(conf->hash_locks + hash);

	for (;;) {
		if (!(flags & R5_GAS_NOQUIESCE) && conf->quiesce) {
			/*
			 * Must release the reference to batch_last before
			 * waiting, on quiesce, otherwise the batch_last will
			 * hold a reference to a stripe and raid5_quiesce()
			 * will deadlock waiting for active_stripes to go to
			 * zero.
			 */
			if (ctx && ctx->batch_last) {
				raid5_release_stripe(ctx->batch_last);
				ctx->batch_last = NULL;
			}

			wait_event_lock_irq(conf->wait_for_quiescent,
					    !conf->quiesce,
					    *(conf->hash_locks + hash));
		}

		sh = find_get_stripe(conf, sector, conf->generation - previous,
				     hash);
		if (sh)
			break;

		if (!test_bit(R5_INACTIVE_BLOCKED, &conf->cache_state)) {
			sh = get_free_stripe(conf, hash);
			if (sh) {
				r5c_check_stripe_cache_usage(conf);
				init_stripe(sh, sector, previous);
				atomic_inc(&sh->count);
				break;
			}

			if (!test_bit(R5_DID_ALLOC, &conf->cache_state))
				set_bit(R5_ALLOC_MORE, &conf->cache_state);
		}

		if (flags & R5_GAS_NOBLOCK)
			break;

		set_bit(R5_INACTIVE_BLOCKED, &conf->cache_state);
		r5l_wake_reclaim(conf->log, 0);

		/* release batch_last before wait to avoid risk of deadlock */
		if (ctx && ctx->batch_last) {
			raid5_release_stripe(ctx->batch_last);
			ctx->batch_last = NULL;
		}

		wait_event_lock_irq(conf->wait_for_stripe,
				    is_inactive_blocked(conf, hash),
				    *(conf->hash_locks + hash));
		clear_bit(R5_INACTIVE_BLOCKED, &conf->cache_state);
	}

	spin_unlock_irq(conf->hash_locks + hash);
	return sh;
}

static bool is_full_stripe_write(struct stripe_head *sh)
{
	BUG_ON(sh->overwrite_disks > (sh->disks - sh->raid_conf->max_degraded));
	return sh->overwrite_disks == (sh->disks - sh->raid_conf->max_degraded);
}

static void lock_two_stripes(struct stripe_head *sh1, struct stripe_head *sh2)
		__acquires(&sh1->stripe_lock)
		__acquires(&sh2->stripe_lock)
{
	if (sh1 > sh2) {
		spin_lock_irq(&sh2->stripe_lock);
		spin_lock_nested(&sh1->stripe_lock, 1);
	} else {
		spin_lock_irq(&sh1->stripe_lock);
		spin_lock_nested(&sh2->stripe_lock, 1);
	}
}

static void unlock_two_stripes(struct stripe_head *sh1, struct stripe_head *sh2)
		__releases(&sh1->stripe_lock)
		__releases(&sh2->stripe_lock)
{
	spin_unlock(&sh1->stripe_lock);
	spin_unlock_irq(&sh2->stripe_lock);
}

/* Only freshly new full stripe normal write stripe can be added to a batch list */
static bool stripe_can_batch(struct stripe_head *sh)
{
	struct r5conf *conf = sh->raid_conf;

	if (raid5_has_log(conf) || raid5_has_ppl(conf))
		return false;
	return test_bit(STRIPE_BATCH_READY, &sh->state) &&
	       is_full_stripe_write(sh);
}

/* we only do back search */
static void stripe_add_to_batch_list(struct r5conf *conf,
		struct stripe_head *sh, struct stripe_head *last_sh)
{
	struct stripe_head *head;
	sector_t head_sector, tmp_sec;
	int hash;
	int dd_idx;

	/* Don't cross chunks, so stripe pd_idx/qd_idx is the same */
	tmp_sec = sh->sector;
	if (!sector_div(tmp_sec, conf->chunk_sectors))
		return;
	head_sector = sh->sector - RAID5_STRIPE_SECTORS(conf);

	if (last_sh && head_sector == last_sh->sector) {
		head = last_sh;
		atomic_inc(&head->count);
	} else {
		hash = stripe_hash_locks_hash(conf, head_sector);
		spin_lock_irq(conf->hash_locks + hash);
		head = find_get_stripe(conf, head_sector, conf->generation,
				       hash);
		spin_unlock_irq(conf->hash_locks + hash);
		if (!head)
			return;
		if (!stripe_can_batch(head))
			goto out;
	}

	lock_two_stripes(head, sh);
	/* clear_batch_ready clear the flag */
	if (!stripe_can_batch(head) || !stripe_can_batch(sh))
		goto unlock_out;

	if (sh->batch_head)
		goto unlock_out;

	dd_idx = 0;
	while (dd_idx == sh->pd_idx || dd_idx == sh->qd_idx)
		dd_idx++;
	if (head->dev[dd_idx].towrite->bi_opf != sh->dev[dd_idx].towrite->bi_opf ||
	    bio_op(head->dev[dd_idx].towrite) != bio_op(sh->dev[dd_idx].towrite))
		goto unlock_out;

	if (head->batch_head) {
		spin_lock(&head->batch_head->batch_lock);
		/* This batch list is already running */
		if (!stripe_can_batch(head)) {
			spin_unlock(&head->batch_head->batch_lock);
			goto unlock_out;
		}
		/*
		 * We must assign batch_head of this stripe within the
		 * batch_lock, otherwise clear_batch_ready of batch head
		 * stripe could clear BATCH_READY bit of this stripe and
		 * this stripe->batch_head doesn't get assigned, which
		 * could confuse clear_batch_ready for this stripe
		 */
		sh->batch_head = head->batch_head;

		/*
		 * at this point, head's BATCH_READY could be cleared, but we
		 * can still add the stripe to batch list
		 */
		list_add(&sh->batch_list, &head->batch_list);
		spin_unlock(&head->batch_head->batch_lock);
	} else {
		head->batch_head = head;
		sh->batch_head = head->batch_head;
		spin_lock(&head->batch_lock);
		list_add_tail(&sh->batch_list, &head->batch_list);
		spin_unlock(&head->batch_lock);
	}

	if (test_and_clear_bit(STRIPE_PREREAD_ACTIVE, &sh->state))
		if (atomic_dec_return(&conf->preread_active_stripes)
		    < IO_THRESHOLD)
			md_wakeup_thread(conf->mddev->thread);

	if (test_and_clear_bit(STRIPE_BIT_DELAY, &sh->state)) {
		int seq = sh->bm_seq;
		if (test_bit(STRIPE_BIT_DELAY, &sh->batch_head->state) &&
		    sh->batch_head->bm_seq > seq)
			seq = sh->batch_head->bm_seq;
		set_bit(STRIPE_BIT_DELAY, &sh->batch_head->state);
		sh->batch_head->bm_seq = seq;
	}

	atomic_inc(&sh->count);
unlock_out:
	unlock_two_stripes(head, sh);
out:
	raid5_release_stripe(head);
}

/* Determine if 'data_offset' or 'new_data_offset' should be used
 * in this stripe_head.
 */
static int use_new_offset(struct r5conf *conf, struct stripe_head *sh)
{
	sector_t progress = conf->reshape_progress;
	/* Need a memory barrier to make sure we see the value
	 * of conf->generation, or ->data_offset that was set before
	 * reshape_progress was updated.
	 */
	smp_rmb();
	if (progress == MaxSector)
		return 0;
	if (sh->generation == conf->generation - 1)
		return 0;
	/* We are in a reshape, and this is a new-generation stripe,
	 * so use new_data_offset.
	 */
	return 1;
}

static void dispatch_bio_list(struct bio_list *tmp)
{
	struct bio *bio;

	while ((bio = bio_list_pop(tmp)))
		submit_bio_noacct(bio);
}

static int cmp_stripe(void *priv, const struct list_head *a,
		      const struct list_head *b)
{
	const struct r5pending_data *da = list_entry(a,
				struct r5pending_data, sibling);
	const struct r5pending_data *db = list_entry(b,
				struct r5pending_data, sibling);
	if (da->sector > db->sector)
		return 1;
	if (da->sector < db->sector)
		return -1;
	return 0;
}

static void dispatch_defer_bios(struct r5conf *conf, int target,
				struct bio_list *list)
{
	struct r5pending_data *data;
	struct list_head *first, *next = NULL;
	int cnt = 0;

	if (conf->pending_data_cnt == 0)
		return;

	list_sort(NULL, &conf->pending_list, cmp_stripe);

	first = conf->pending_list.next;

	/* temporarily move the head */
	if (conf->next_pending_data)
		list_move_tail(&conf->pending_list,
				&conf->next_pending_data->sibling);

	while (!list_empty(&conf->pending_list)) {
		data = list_first_entry(&conf->pending_list,
			struct r5pending_data, sibling);
		if (&data->sibling == first)
			first = data->sibling.next;
		next = data->sibling.next;

		bio_list_merge(list, &data->bios);
		list_move(&data->sibling, &conf->free_list);
		cnt++;
		if (cnt >= target)
			break;
	}
	conf->pending_data_cnt -= cnt;
	BUG_ON(conf->pending_data_cnt < 0 || cnt < target);

	if (next != &conf->pending_list)
		conf->next_pending_data = list_entry(next,
				struct r5pending_data, sibling);
	else
		conf->next_pending_data = NULL;
	/* list isn't empty */
	if (first != &conf->pending_list)
		list_move_tail(&conf->pending_list, first);
}

static void flush_deferred_bios(struct r5conf *conf)
{
	struct bio_list tmp = BIO_EMPTY_LIST;

	if (conf->pending_data_cnt == 0)
		return;

	spin_lock(&conf->pending_bios_lock);
	dispatch_defer_bios(conf, conf->pending_data_cnt, &tmp);
	BUG_ON(conf->pending_data_cnt != 0);
	spin_unlock(&conf->pending_bios_lock);

	dispatch_bio_list(&tmp);
}

static void defer_issue_bios(struct r5conf *conf, sector_t sector,
				struct bio_list *bios)
{
	struct bio_list tmp = BIO_EMPTY_LIST;
	struct r5pending_data *ent;

	spin_lock(&conf->pending_bios_lock);
	ent = list_first_entry(&conf->free_list, struct r5pending_data,
							sibling);
	list_move_tail(&ent->sibling, &conf->pending_list);
	ent->sector = sector;
	bio_list_init(&ent->bios);
	bio_list_merge(&ent->bios, bios);
	conf->pending_data_cnt++;
	if (conf->pending_data_cnt >= PENDING_IO_MAX)
		dispatch_defer_bios(conf, PENDING_IO_ONE_FLUSH, &tmp);

	spin_unlock(&conf->pending_bios_lock);

	dispatch_bio_list(&tmp);
}

static void
raid5_end_read_request(struct bio *bi);
static void
raid5_end_write_request(struct bio *bi);

static void ops_run_io(struct stripe_head *sh, struct stripe_head_state *s)
{
	struct r5conf *conf = sh->raid_conf;
	int i, disks = sh->disks;
	struct stripe_head *head_sh = sh;
	struct bio_list pending_bios = BIO_EMPTY_LIST;
	struct r5dev *dev;
	bool should_defer;

	might_sleep();

	if (log_stripe(sh, s) == 0)
		return;

	should_defer = conf->batch_bio_dispatch && conf->group_cnt;

	for (i = disks; i--; ) {
		enum req_op op;
		blk_opf_t op_flags = 0;
		int replace_only = 0;
		struct bio *bi, *rbi;
		struct md_rdev *rdev, *rrdev = NULL;

		sh = head_sh;
		if (test_and_clear_bit(R5_Wantwrite, &sh->dev[i].flags)) {
			op = REQ_OP_WRITE;
			if (test_and_clear_bit(R5_WantFUA, &sh->dev[i].flags))
				op_flags = REQ_FUA;
			if (test_bit(R5_Discard, &sh->dev[i].flags))
				op = REQ_OP_DISCARD;
		} else if (test_and_clear_bit(R5_Wantread, &sh->dev[i].flags))
			op = REQ_OP_READ;
		else if (test_and_clear_bit(R5_WantReplace,
					    &sh->dev[i].flags)) {
			op = REQ_OP_WRITE;
			replace_only = 1;
		} else
			continue;
		if (test_and_clear_bit(R5_SyncIO, &sh->dev[i].flags))
			op_flags |= REQ_SYNC;

again:
		dev = &sh->dev[i];
		bi = &dev->req;
		rbi = &dev->rreq; /* For writing to replacement */

		rdev = conf->disks[i].rdev;
		rrdev = conf->disks[i].replacement;
		if (op_is_write(op)) {
			if (replace_only)
				rdev = NULL;
			if (rdev == rrdev)
				/* We raced and saw duplicates */
				rrdev = NULL;
		} else {
			if (test_bit(R5_ReadRepl, &head_sh->dev[i].flags) && rrdev)
				rdev = rrdev;
			rrdev = NULL;
		}

		if (rdev && test_bit(Faulty, &rdev->flags))
			rdev = NULL;
		if (rdev)
			atomic_inc(&rdev->nr_pending);
		if (rrdev && test_bit(Faulty, &rrdev->flags))
			rrdev = NULL;
		if (rrdev)
			atomic_inc(&rrdev->nr_pending);

		/* We have already checked bad blocks for reads.  Now
		 * need to check for writes.  We never accept write errors
		 * on the replacement, so we don't to check rrdev.
		 */
		while (op_is_write(op) && rdev &&
		       test_bit(WriteErrorSeen, &rdev->flags)) {
			int bad = rdev_has_badblock(rdev, sh->sector,
						    RAID5_STRIPE_SECTORS(conf));
			if (!bad)
				break;

			if (bad < 0) {
				set_bit(BlockedBadBlocks, &rdev->flags);
				if (!conf->mddev->external &&
				    conf->mddev->sb_flags) {
					/* It is very unlikely, but we might
					 * still need to write out the
					 * bad block log - better give it
					 * a chance*/
					md_check_recovery(conf->mddev);
				}
				/*
				 * Because md_wait_for_blocked_rdev
				 * will dec nr_pending, we must
				 * increment it first.
				 */
				atomic_inc(&rdev->nr_pending);
				md_wait_for_blocked_rdev(rdev, conf->mddev);
			} else {
				/* Acknowledged bad block - skip the write */
				rdev_dec_pending(rdev, conf->mddev);
				rdev = NULL;
			}
		}

		if (rdev) {
			set_bit(STRIPE_IO_STARTED, &sh->state);

			bio_init(bi, rdev->bdev, &dev->vec, 1, op | op_flags);
			bi->bi_end_io = op_is_write(op)
				? raid5_end_write_request
				: raid5_end_read_request;
			bi->bi_private = sh;

			pr_debug("%s: for %llu schedule op %d on disc %d\n",
				__func__, (unsigned long long)sh->sector,
				bi->bi_opf, i);
			atomic_inc(&sh->count);
			if (sh != head_sh)
				atomic_inc(&head_sh->count);
			if (use_new_offset(conf, sh))
				bi->bi_iter.bi_sector = (sh->sector
						 + rdev->new_data_offset);
			else
				bi->bi_iter.bi_sector = (sh->sector
						 + rdev->data_offset);
			if (test_bit(R5_ReadNoMerge, &head_sh->dev[i].flags))
				bi->bi_opf |= REQ_NOMERGE;

			if (test_bit(R5_SkipCopy, &sh->dev[i].flags))
				WARN_ON(test_bit(R5_UPTODATE, &sh->dev[i].flags));

			if (!op_is_write(op) &&
			    test_bit(R5_InJournal, &sh->dev[i].flags))
				/*
				 * issuing read for a page in journal, this
				 * must be preparing for prexor in rmw; read
				 * the data into orig_page
				 */
				sh->dev[i].vec.bv_page = sh->dev[i].orig_page;
			else
				sh->dev[i].vec.bv_page = sh->dev[i].page;
			bi->bi_vcnt = 1;
			bi->bi_io_vec[0].bv_len = RAID5_STRIPE_SIZE(conf);
			bi->bi_io_vec[0].bv_offset = sh->dev[i].offset;
			bi->bi_iter.bi_size = RAID5_STRIPE_SIZE(conf);
			/*
			 * If this is discard request, set bi_vcnt 0. We don't
			 * want to confuse SCSI because SCSI will replace payload
			 */
			if (op == REQ_OP_DISCARD)
				bi->bi_vcnt = 0;
			if (rrdev)
				set_bit(R5_DOUBLE_LOCKED, &sh->dev[i].flags);

			mddev_trace_remap(conf->mddev, bi, sh->dev[i].sector);
			if (should_defer && op_is_write(op))
				bio_list_add(&pending_bios, bi);
			else
				submit_bio_noacct(bi);
		}
		if (rrdev) {
			set_bit(STRIPE_IO_STARTED, &sh->state);

			bio_init(rbi, rrdev->bdev, &dev->rvec, 1, op | op_flags);
			BUG_ON(!op_is_write(op));
			rbi->bi_end_io = raid5_end_write_request;
			rbi->bi_private = sh;

			pr_debug("%s: for %llu schedule op %d on "
				 "replacement disc %d\n",
				__func__, (unsigned long long)sh->sector,
				rbi->bi_opf, i);
			atomic_inc(&sh->count);
			if (sh != head_sh)
				atomic_inc(&head_sh->count);
			if (use_new_offset(conf, sh))
				rbi->bi_iter.bi_sector = (sh->sector
						  + rrdev->new_data_offset);
			else
				rbi->bi_iter.bi_sector = (sh->sector
						  + rrdev->data_offset);
			if (test_bit(R5_SkipCopy, &sh->dev[i].flags))
				WARN_ON(test_bit(R5_UPTODATE, &sh->dev[i].flags));
			sh->dev[i].rvec.bv_page = sh->dev[i].page;
			rbi->bi_vcnt = 1;
			rbi->bi_io_vec[0].bv_len = RAID5_STRIPE_SIZE(conf);
			rbi->bi_io_vec[0].bv_offset = sh->dev[i].offset;
			rbi->bi_iter.bi_size = RAID5_STRIPE_SIZE(conf);
			/*
			 * If this is discard request, set bi_vcnt 0. We don't
			 * want to confuse SCSI because SCSI will replace payload
			 */
			if (op == REQ_OP_DISCARD)
				rbi->bi_vcnt = 0;
			mddev_trace_remap(conf->mddev, rbi, sh->dev[i].sector);
			if (should_defer && op_is_write(op))
				bio_list_add(&pending_bios, rbi);
			else
				submit_bio_noacct(rbi);
		}
		if (!rdev && !rrdev) {
			pr_debug("skip op %d on disc %d for sector %llu\n",
				bi->bi_opf, i, (unsigned long long)sh->sector);
			clear_bit(R5_LOCKED, &sh->dev[i].flags);
			set_bit(STRIPE_HANDLE, &sh->state);
		}

		if (!head_sh->batch_head)
			continue;
		sh = list_first_entry(&sh->batch_list, struct stripe_head,
				      batch_list);
		if (sh != head_sh)
			goto again;
	}

	if (should_defer && !bio_list_empty(&pending_bios))
		defer_issue_bios(conf, head_sh->sector, &pending_bios);
}

static struct dma_async_tx_descriptor *
async_copy_data(int frombio, struct bio *bio, struct page **page,
	unsigned int poff, sector_t sector, struct dma_async_tx_descriptor *tx,
	struct stripe_head *sh, int no_skipcopy)
{
	struct bio_vec bvl;
	struct bvec_iter iter;
	struct page *bio_page;
	int page_offset;
	struct async_submit_ctl submit;
	enum async_tx_flags flags = 0;
	struct r5conf *conf = sh->raid_conf;

	if (bio->bi_iter.bi_sector >= sector)
		page_offset = (signed)(bio->bi_iter.bi_sector - sector) * 512;
	else
		page_offset = (signed)(sector - bio->bi_iter.bi_sector) * -512;

	if (frombio)
		flags |= ASYNC_TX_FENCE;
	init_async_submit(&submit, flags, tx, NULL, NULL, NULL);

	bio_for_each_segment(bvl, bio, iter) {
		int len = bvl.bv_len;
		int clen;
		int b_offset = 0;

		if (page_offset < 0) {
			b_offset = -page_offset;
			page_offset += b_offset;
			len -= b_offset;
		}

		if (len > 0 && page_offset + len > RAID5_STRIPE_SIZE(conf))
			clen = RAID5_STRIPE_SIZE(conf) - page_offset;
		else
			clen = len;

		if (clen > 0) {
			b_offset += bvl.bv_offset;
			bio_page = bvl.bv_page;
			if (frombio) {
				if (conf->skip_copy &&
				    b_offset == 0 && page_offset == 0 &&
				    clen == RAID5_STRIPE_SIZE(conf) &&
				    !no_skipcopy)
					*page = bio_page;
				else
					tx = async_memcpy(*page, bio_page, page_offset + poff,
						  b_offset, clen, &submit);
			} else
				tx = async_memcpy(bio_page, *page, b_offset,
						  page_offset + poff, clen, &submit);
		}
		/* chain the operations */
		submit.depend_tx = tx;

		if (clen < len) /* hit end of page */
			break;
		page_offset +=  len;
	}

	return tx;
}

static void ops_complete_biofill(void *stripe_head_ref)
{
	struct stripe_head *sh = stripe_head_ref;
	int i;
	struct r5conf *conf = sh->raid_conf;

	pr_debug("%s: stripe %llu\n", __func__,
		(unsigned long long)sh->sector);

	/* clear completed biofills */
	for (i = sh->disks; i--; ) {
		struct r5dev *dev = &sh->dev[i];

		/* acknowledge completion of a biofill operation */
		/* and check if we need to reply to a read request,
		 * new R5_Wantfill requests are held off until
		 * !STRIPE_BIOFILL_RUN
		 */
		if (test_and_clear_bit(R5_Wantfill, &dev->flags)) {
			struct bio *rbi, *rbi2;

			BUG_ON(!dev->read);
			rbi = dev->read;
			dev->read = NULL;
			while (rbi && rbi->bi_iter.bi_sector <
				dev->sector + RAID5_STRIPE_SECTORS(conf)) {
				rbi2 = r5_next_bio(conf, rbi, dev->sector);
				bio_endio(rbi);
				rbi = rbi2;
			}
		}
	}
	clear_bit(STRIPE_BIOFILL_RUN, &sh->state);

	set_bit(STRIPE_HANDLE, &sh->state);
	raid5_release_stripe(sh);
}

static void ops_run_biofill(struct stripe_head *sh)
{
	struct dma_async_tx_descriptor *tx = NULL;
	struct async_submit_ctl submit;
	int i;
	struct r5conf *conf = sh->raid_conf;

	BUG_ON(sh->batch_head);
	pr_debug("%s: stripe %llu\n", __func__,
		(unsigned long long)sh->sector);

	for (i = sh->disks; i--; ) {
		struct r5dev *dev = &sh->dev[i];
		if (test_bit(R5_Wantfill, &dev->flags)) {
			struct bio *rbi;
			spin_lock_irq(&sh->stripe_lock);
			dev->read = rbi = dev->toread;
			dev->toread = NULL;
			spin_unlock_irq(&sh->stripe_lock);
			while (rbi && rbi->bi_iter.bi_sector <
				dev->sector + RAID5_STRIPE_SECTORS(conf)) {
				tx = async_copy_data(0, rbi, &dev->page,
						     dev->offset,
						     dev->sector, tx, sh, 0);
				rbi = r5_next_bio(conf, rbi, dev->sector);
			}
		}
	}

	atomic_inc(&sh->count);
	init_async_submit(&submit, ASYNC_TX_ACK, tx, ops_complete_biofill, sh, NULL);
	async_trigger_callback(&submit);
}

static void mark_target_uptodate(struct stripe_head *sh, int target)
{
	struct r5dev *tgt;

	if (target < 0)
		return;

	tgt = &sh->dev[target];
	set_bit(R5_UPTODATE, &tgt->flags);
	BUG_ON(!test_bit(R5_Wantcompute, &tgt->flags));
	clear_bit(R5_Wantcompute, &tgt->flags);
}

static void ops_complete_compute(void *stripe_head_ref)
{
	struct stripe_head *sh = stripe_head_ref;

	pr_debug("%s: stripe %llu\n", __func__,
		(unsigned long long)sh->sector);

	/* mark the computed target(s) as uptodate */
	mark_target_uptodate(sh, sh->ops.target);
	mark_target_uptodate(sh, sh->ops.target2);

	clear_bit(STRIPE_COMPUTE_RUN, &sh->state);
	if (sh->check_state == check_state_compute_run)
		sh->check_state = check_state_compute_result;
	set_bit(STRIPE_HANDLE, &sh->state);
	raid5_release_stripe(sh);
}

/* return a pointer to the address conversion region of the scribble buffer */
static struct page **to_addr_page(struct raid5_percpu *percpu, int i)
{
	return percpu->scribble + i * percpu->scribble_obj_size;
}

/* return a pointer to the address conversion region of the scribble buffer */
static addr_conv_t *to_addr_conv(struct stripe_head *sh,
				 struct raid5_percpu *percpu, int i)
{
	return (void *) (to_addr_page(percpu, i) + sh->disks + 2);
}

/*
 * Return a pointer to record offset address.
 */
static unsigned int *
to_addr_offs(struct stripe_head *sh, struct raid5_percpu *percpu)
{
	return (unsigned int *) (to_addr_conv(sh, percpu, 0) + sh->disks + 2);
}

static struct dma_async_tx_descriptor *
ops_run_compute5(struct stripe_head *sh, struct raid5_percpu *percpu)
{
	int disks = sh->disks;
	struct page **xor_srcs = to_addr_page(percpu, 0);
	unsigned int *off_srcs = to_addr_offs(sh, percpu);
	int target = sh->ops.target;
	struct r5dev *tgt = &sh->dev[target];
	struct page *xor_dest = tgt->page;
	unsigned int off_dest = tgt->offset;
	int count = 0;
	struct dma_async_tx_descriptor *tx;
	struct async_submit_ctl submit;
	int i;

	BUG_ON(sh->batch_head);

	pr_debug("%s: stripe %llu block: %d\n",
		__func__, (unsigned long long)sh->sector, target);
	BUG_ON(!test_bit(R5_Wantcompute, &tgt->flags));

	for (i = disks; i--; ) {
		if (i != target) {
			off_srcs[count] = sh->dev[i].offset;
			xor_srcs[count++] = sh->dev[i].page;
		}
	}

	atomic_inc(&sh->count);

	init_async_submit(&submit, ASYNC_TX_FENCE|ASYNC_TX_XOR_ZERO_DST, NULL,
			  ops_complete_compute, sh, to_addr_conv(sh, percpu, 0));
	if (unlikely(count == 1))
		tx = async_memcpy(xor_dest, xor_srcs[0], off_dest, off_srcs[0],
				RAID5_STRIPE_SIZE(sh->raid_conf), &submit);
	else
		tx = async_xor_offs(xor_dest, off_dest, xor_srcs, off_srcs, count,
				RAID5_STRIPE_SIZE(sh->raid_conf), &submit);

	return tx;
}

/* set_syndrome_sources - populate source buffers for gen_syndrome
 * @srcs - (struct page *) array of size sh->disks
 * @offs - (unsigned int) array of offset for each page
 * @sh - stripe_head to parse
 *
 * Populates srcs in proper layout order for the stripe and returns the
 * 'count' of sources to be used in a call to async_gen_syndrome.  The P
 * destination buffer is recorded in srcs[count] and the Q destination
 * is recorded in srcs[count+1]].
 */
static int set_syndrome_sources(struct page **srcs,
				unsigned int *offs,
				struct stripe_head *sh,
				int srctype)
{
	int disks = sh->disks;
	int syndrome_disks = sh->ddf_layout ? disks : (disks - 2);
	int d0_idx = raid6_d0(sh);
	int count;
	int i;

	for (i = 0; i < disks; i++)
		srcs[i] = NULL;

	count = 0;
	i = d0_idx;
	do {
		int slot = raid6_idx_to_slot(i, sh, &count, syndrome_disks);
		struct r5dev *dev = &sh->dev[i];

		if (i == sh->qd_idx || i == sh->pd_idx ||
		    (srctype == SYNDROME_SRC_ALL) ||
		    (srctype == SYNDROME_SRC_WANT_DRAIN &&
		     (test_bit(R5_Wantdrain, &dev->flags) ||
		      test_bit(R5_InJournal, &dev->flags))) ||
		    (srctype == SYNDROME_SRC_WRITTEN &&
		     (dev->written ||
		      test_bit(R5_InJournal, &dev->flags)))) {
			if (test_bit(R5_InJournal, &dev->flags))
				srcs[slot] = sh->dev[i].orig_page;
			else
				srcs[slot] = sh->dev[i].page;
			/*
			 * For R5_InJournal, PAGE_SIZE must be 4KB and will
			 * not shared page. In that case, dev[i].offset
			 * is 0.
			 */
			offs[slot] = sh->dev[i].offset;
		}
		i = raid6_next_disk(i, disks);
	} while (i != d0_idx);

	return syndrome_disks;
}

static struct dma_async_tx_descriptor *
ops_run_compute6_1(struct stripe_head *sh, struct raid5_percpu *percpu)
{
	int disks = sh->disks;
	struct page **blocks = to_addr_page(percpu, 0);
	unsigned int *offs = to_addr_offs(sh, percpu);
	int target;
	int qd_idx = sh->qd_idx;
	struct dma_async_tx_descriptor *tx;
	struct async_submit_ctl submit;
	struct r5dev *tgt;
	struct page *dest;
	unsigned int dest_off;
	int i;
	int count;

	BUG_ON(sh->batch_head);
	if (sh->ops.target < 0)
		target = sh->ops.target2;
	else if (sh->ops.target2 < 0)
		target = sh->ops.target;
	else
		/* we should only have one valid target */
		BUG();
	BUG_ON(target < 0);
	pr_debug("%s: stripe %llu block: %d\n",
		__func__, (unsigned long long)sh->sector, target);

	tgt = &sh->dev[target];
	BUG_ON(!test_bit(R5_Wantcompute, &tgt->flags));
	dest = tgt->page;
	dest_off = tgt->offset;

	atomic_inc(&sh->count);

	if (target == qd_idx) {
		count = set_syndrome_sources(blocks, offs, sh, SYNDROME_SRC_ALL);
		blocks[count] = NULL; /* regenerating p is not necessary */
		BUG_ON(blocks[count+1] != dest); /* q should already be set */
		init_async_submit(&submit, ASYNC_TX_FENCE, NULL,
				  ops_complete_compute, sh,
				  to_addr_conv(sh, percpu, 0));
		tx = async_gen_syndrome(blocks, offs, count+2,
				RAID5_STRIPE_SIZE(sh->raid_conf), &submit);
	} else {
		/* Compute any data- or p-drive using XOR */
		count = 0;
		for (i = disks; i-- ; ) {
			if (i == target || i == qd_idx)
				continue;
			offs[count] = sh->dev[i].offset;
			blocks[count++] = sh->dev[i].page;
		}

		init_async_submit(&submit, ASYNC_TX_FENCE|ASYNC_TX_XOR_ZERO_DST,
				  NULL, ops_complete_compute, sh,
				  to_addr_conv(sh, percpu, 0));
		tx = async_xor_offs(dest, dest_off, blocks, offs, count,
				RAID5_STRIPE_SIZE(sh->raid_conf), &submit);
	}

	return tx;
}

static struct dma_async_tx_descriptor *
ops_run_compute6_2(struct stripe_head *sh, struct raid5_percpu *percpu)
{
	int i, count, disks = sh->disks;
	int syndrome_disks = sh->ddf_layout ? disks : disks-2;
	int d0_idx = raid6_d0(sh);
	int faila = -1, failb = -1;
	int target = sh->ops.target;
	int target2 = sh->ops.target2;
	struct r5dev *tgt = &sh->dev[target];
	struct r5dev *tgt2 = &sh->dev[target2];
	struct dma_async_tx_descriptor *tx;
	struct page **blocks = to_addr_page(percpu, 0);
	unsigned int *offs = to_addr_offs(sh, percpu);
	struct async_submit_ctl submit;

	BUG_ON(sh->batch_head);
	pr_debug("%s: stripe %llu block1: %d block2: %d\n",
		 __func__, (unsigned long long)sh->sector, target, target2);
	BUG_ON(target < 0 || target2 < 0);
	BUG_ON(!test_bit(R5_Wantcompute, &tgt->flags));
	BUG_ON(!test_bit(R5_Wantcompute, &tgt2->flags));

	/* we need to open-code set_syndrome_sources to handle the
	 * slot number conversion for 'faila' and 'failb'
	 */
	for (i = 0; i < disks ; i++) {
		offs[i] = 0;
		blocks[i] = NULL;
	}
	count = 0;
	i = d0_idx;
	do {
		int slot = raid6_idx_to_slot(i, sh, &count, syndrome_disks);

		offs[slot] = sh->dev[i].offset;
		blocks[slot] = sh->dev[i].page;

		if (i == target)
			faila = slot;
		if (i == target2)
			failb = slot;
		i = raid6_next_disk(i, disks);
	} while (i != d0_idx);

	BUG_ON(faila == failb);
	if (failb < faila)
		swap(faila, failb);
	pr_debug("%s: stripe: %llu faila: %d failb: %d\n",
		 __func__, (unsigned long long)sh->sector, faila, failb);

	atomic_inc(&sh->count);

	if (failb == syndrome_disks+1) {
		/* Q disk is one of the missing disks */
		if (faila == syndrome_disks) {
			/* Missing P+Q, just recompute */
			init_async_submit(&submit, ASYNC_TX_FENCE, NULL,
					  ops_complete_compute, sh,
					  to_addr_conv(sh, percpu, 0));
			return async_gen_syndrome(blocks, offs, syndrome_disks+2,
						  RAID5_STRIPE_SIZE(sh->raid_conf),
						  &submit);
		} else {
			struct page *dest;
			unsigned int dest_off;
			int data_target;
			int qd_idx = sh->qd_idx;

			/* Missing D+Q: recompute D from P, then recompute Q */
			if (target == qd_idx)
				data_target = target2;
			else
				data_target = target;

			count = 0;
			for (i = disks; i-- ; ) {
				if (i == data_target || i == qd_idx)
					continue;
				offs[count] = sh->dev[i].offset;
				blocks[count++] = sh->dev[i].page;
			}
			dest = sh->dev[data_target].page;
			dest_off = sh->dev[data_target].offset;
			init_async_submit(&submit,
					  ASYNC_TX_FENCE|ASYNC_TX_XOR_ZERO_DST,
					  NULL, NULL, NULL,
					  to_addr_conv(sh, percpu, 0));
			tx = async_xor_offs(dest, dest_off, blocks, offs, count,
				       RAID5_STRIPE_SIZE(sh->raid_conf),
				       &submit);

			count = set_syndrome_sources(blocks, offs, sh, SYNDROME_SRC_ALL);
			init_async_submit(&submit, ASYNC_TX_FENCE, tx,
					  ops_complete_compute, sh,
					  to_addr_conv(sh, percpu, 0));
			return async_gen_syndrome(blocks, offs, count+2,
						  RAID5_STRIPE_SIZE(sh->raid_conf),
						  &submit);
		}
	} else {
		init_async_submit(&submit, ASYNC_TX_FENCE, NULL,
				  ops_complete_compute, sh,
				  to_addr_conv(sh, percpu, 0));
		if (failb == syndrome_disks) {
			/* We're missing D+P. */
			return async_raid6_datap_recov(syndrome_disks+2,
						RAID5_STRIPE_SIZE(sh->raid_conf),
						faila,
						blocks, offs, &submit);
		} else {
			/* We're missing D+D. */
			return async_raid6_2data_recov(syndrome_disks+2,
						RAID5_STRIPE_SIZE(sh->raid_conf),
						faila, failb,
						blocks, offs, &submit);
		}
	}
}

static void ops_complete_prexor(void *stripe_head_ref)
{
	struct stripe_head *sh = stripe_head_ref;

	pr_debug("%s: stripe %llu\n", __func__,
		(unsigned long long)sh->sector);

	if (r5c_is_writeback(sh->raid_conf->log))
		/*
		 * raid5-cache write back uses orig_page during prexor.
		 * After prexor, it is time to free orig_page
		 */
		r5c_release_extra_page(sh);
}

static struct dma_async_tx_descriptor *
ops_run_prexor5(struct stripe_head *sh, struct raid5_percpu *percpu,
		struct dma_async_tx_descriptor *tx)
{
	int disks = sh->disks;
	struct page **xor_srcs = to_addr_page(percpu, 0);
	unsigned int *off_srcs = to_addr_offs(sh, percpu);
	int count = 0, pd_idx = sh->pd_idx, i;
	struct async_submit_ctl submit;

	/* existing parity data subtracted */
	unsigned int off_dest = off_srcs[count] = sh->dev[pd_idx].offset;
	struct page *xor_dest = xor_srcs[count++] = sh->dev[pd_idx].page;

	BUG_ON(sh->batch_head);
	pr_debug("%s: stripe %llu\n", __func__,
		(unsigned long long)sh->sector);

	for (i = disks; i--; ) {
		struct r5dev *dev = &sh->dev[i];
		/* Only process blocks that are known to be uptodate */
		if (test_bit(R5_InJournal, &dev->flags)) {
			/*
			 * For this case, PAGE_SIZE must be equal to 4KB and
			 * page offset is zero.
			 */
			off_srcs[count] = dev->offset;
			xor_srcs[count++] = dev->orig_page;
		} else if (test_bit(R5_Wantdrain, &dev->flags)) {
			off_srcs[count] = dev->offset;
			xor_srcs[count++] = dev->page;
		}
	}

	init_async_submit(&submit, ASYNC_TX_FENCE|ASYNC_TX_XOR_DROP_DST, tx,
			  ops_complete_prexor, sh, to_addr_conv(sh, percpu, 0));
	tx = async_xor_offs(xor_dest, off_dest, xor_srcs, off_srcs, count,
			RAID5_STRIPE_SIZE(sh->raid_conf), &submit);

	return tx;
}

static struct dma_async_tx_descriptor *
ops_run_prexor6(struct stripe_head *sh, struct raid5_percpu *percpu,
		struct dma_async_tx_descriptor *tx)
{
	struct page **blocks = to_addr_page(percpu, 0);
	unsigned int *offs = to_addr_offs(sh, percpu);
	int count;
	struct async_submit_ctl submit;

	pr_debug("%s: stripe %llu\n", __func__,
		(unsigned long long)sh->sector);

	count = set_syndrome_sources(blocks, offs, sh, SYNDROME_SRC_WANT_DRAIN);

	init_async_submit(&submit, ASYNC_TX_FENCE|ASYNC_TX_PQ_XOR_DST, tx,
			  ops_complete_prexor, sh, to_addr_conv(sh, percpu, 0));
	tx = async_gen_syndrome(blocks, offs, count+2,
			RAID5_STRIPE_SIZE(sh->raid_conf), &submit);

	return tx;
}

static struct dma_async_tx_descriptor *
ops_run_biodrain(struct stripe_head *sh, struct dma_async_tx_descriptor *tx)
{
	struct r5conf *conf = sh->raid_conf;
	int disks = sh->disks;
	int i;
	struct stripe_head *head_sh = sh;

	pr_debug("%s: stripe %llu\n", __func__,
		(unsigned long long)sh->sector);

	for (i = disks; i--; ) {
		struct r5dev *dev;
		struct bio *chosen;

		sh = head_sh;
		if (test_and_clear_bit(R5_Wantdrain, &head_sh->dev[i].flags)) {
			struct bio *wbi;

again:
			dev = &sh->dev[i];
			/*
			 * clear R5_InJournal, so when rewriting a page in
			 * journal, it is not skipped by r5l_log_stripe()
			 */
			clear_bit(R5_InJournal, &dev->flags);
			spin_lock_irq(&sh->stripe_lock);
			chosen = dev->towrite;
			dev->towrite = NULL;
			sh->overwrite_disks = 0;
			BUG_ON(dev->written);
			wbi = dev->written = chosen;
			spin_unlock_irq(&sh->stripe_lock);
			WARN_ON(dev->page != dev->orig_page);

			while (wbi && wbi->bi_iter.bi_sector <
				dev->sector + RAID5_STRIPE_SECTORS(conf)) {
				if (wbi->bi_opf & REQ_FUA)
					set_bit(R5_WantFUA, &dev->flags);
				if (wbi->bi_opf & REQ_SYNC)
					set_bit(R5_SyncIO, &dev->flags);
				if (bio_op(wbi) == REQ_OP_DISCARD)
					set_bit(R5_Discard, &dev->flags);
				else {
					tx = async_copy_data(1, wbi, &dev->page,
							     dev->offset,
							     dev->sector, tx, sh,
							     r5c_is_writeback(conf->log));
					if (dev->page != dev->orig_page &&
					    !r5c_is_writeback(conf->log)) {
						set_bit(R5_SkipCopy, &dev->flags);
						clear_bit(R5_UPTODATE, &dev->flags);
						clear_bit(R5_OVERWRITE, &dev->flags);
					}
				}
				wbi = r5_next_bio(conf, wbi, dev->sector);
			}

			if (head_sh->batch_head) {
				sh = list_first_entry(&sh->batch_list,
						      struct stripe_head,
						      batch_list);
				if (sh == head_sh)
					continue;
				goto again;
			}
		}
	}

	return tx;
}

static void ops_complete_reconstruct(void *stripe_head_ref)
{
	struct stripe_head *sh = stripe_head_ref;
	int disks = sh->disks;
	int pd_idx = sh->pd_idx;
	int qd_idx = sh->qd_idx;
	int i;
	bool fua = false, sync = false, discard = false;

	pr_debug("%s: stripe %llu\n", __func__,
		(unsigned long long)sh->sector);

	for (i = disks; i--; ) {
		fua |= test_bit(R5_WantFUA, &sh->dev[i].flags);
		sync |= test_bit(R5_SyncIO, &sh->dev[i].flags);
		discard |= test_bit(R5_Discard, &sh->dev[i].flags);
	}

	for (i = disks; i--; ) {
		struct r5dev *dev = &sh->dev[i];

		if (dev->written || i == pd_idx || i == qd_idx) {
			if (!discard && !test_bit(R5_SkipCopy, &dev->flags)) {
				set_bit(R5_UPTODATE, &dev->flags);
				if (test_bit(STRIPE_EXPAND_READY, &sh->state))
					set_bit(R5_Expanded, &dev->flags);
			}
			if (fua)
				set_bit(R5_WantFUA, &dev->flags);
			if (sync)
				set_bit(R5_SyncIO, &dev->flags);
		}
	}

	if (sh->reconstruct_state == reconstruct_state_drain_run)
		sh->reconstruct_state = reconstruct_state_drain_result;
	else if (sh->reconstruct_state == reconstruct_state_prexor_drain_run)
		sh->reconstruct_state = reconstruct_state_prexor_drain_result;
	else {
		BUG_ON(sh->reconstruct_state != reconstruct_state_run);
		sh->reconstruct_state = reconstruct_state_result;
	}

	set_bit(STRIPE_HANDLE, &sh->state);
	raid5_release_stripe(sh);
}

static void
ops_run_reconstruct5(struct stripe_head *sh, struct raid5_percpu *percpu,
		     struct dma_async_tx_descriptor *tx)
{
	int disks = sh->disks;
	struct page **xor_srcs;
	unsigned int *off_srcs;
	struct async_submit_ctl submit;
	int count, pd_idx = sh->pd_idx, i;
	struct page *xor_dest;
	unsigned int off_dest;
	int prexor = 0;
	unsigned long flags;
	int j = 0;
	struct stripe_head *head_sh = sh;
	int last_stripe;

	pr_debug("%s: stripe %llu\n", __func__,
		(unsigned long long)sh->sector);

	for (i = 0; i < sh->disks; i++) {
		if (pd_idx == i)
			continue;
		if (!test_bit(R5_Discard, &sh->dev[i].flags))
			break;
	}
	if (i >= sh->disks) {
		atomic_inc(&sh->count);
		set_bit(R5_Discard, &sh->dev[pd_idx].flags);
		ops_complete_reconstruct(sh);
		return;
	}
again:
	count = 0;
	xor_srcs = to_addr_page(percpu, j);
	off_srcs = to_addr_offs(sh, percpu);
	/* check if prexor is active which means only process blocks
	 * that are part of a read-modify-write (written)
	 */
	if (head_sh->reconstruct_state == reconstruct_state_prexor_drain_run) {
		prexor = 1;
		off_dest = off_srcs[count] = sh->dev[pd_idx].offset;
		xor_dest = xor_srcs[count++] = sh->dev[pd_idx].page;
		for (i = disks; i--; ) {
			struct r5dev *dev = &sh->dev[i];
			if (head_sh->dev[i].written ||
			    test_bit(R5_InJournal, &head_sh->dev[i].flags)) {
				off_srcs[count] = dev->offset;
				xor_srcs[count++] = dev->page;
			}
		}
	} else {
		xor_dest = sh->dev[pd_idx].page;
		off_dest = sh->dev[pd_idx].offset;
		for (i = disks; i--; ) {
			struct r5dev *dev = &sh->dev[i];
			if (i != pd_idx) {
				off_srcs[count] = dev->offset;
				xor_srcs[count++] = dev->page;
			}
		}
	}

	/* 1/ if we prexor'd then the dest is reused as a source
	 * 2/ if we did not prexor then we are redoing the parity
	 * set ASYNC_TX_XOR_DROP_DST and ASYNC_TX_XOR_ZERO_DST
	 * for the synchronous xor case
	 */
	last_stripe = !head_sh->batch_head ||
		list_first_entry(&sh->batch_list,
				 struct stripe_head, batch_list) == head_sh;
	if (last_stripe) {
		flags = ASYNC_TX_ACK |
			(prexor ? ASYNC_TX_XOR_DROP_DST : ASYNC_TX_XOR_ZERO_DST);

		atomic_inc(&head_sh->count);
		init_async_submit(&submit, flags, tx, ops_complete_reconstruct, head_sh,
				  to_addr_conv(sh, percpu, j));
	} else {
		flags = prexor ? ASYNC_TX_XOR_DROP_DST : ASYNC_TX_XOR_ZERO_DST;
		init_async_submit(&submit, flags, tx, NULL, NULL,
				  to_addr_conv(sh, percpu, j));
	}

	if (unlikely(count == 1))
		tx = async_memcpy(xor_dest, xor_srcs[0], off_dest, off_srcs[0],
				RAID5_STRIPE_SIZE(sh->raid_conf), &submit);
	else
		tx = async_xor_offs(xor_dest, off_dest, xor_srcs, off_srcs, count,
				RAID5_STRIPE_SIZE(sh->raid_conf), &submit);
	if (!last_stripe) {
		j++;
		sh = list_first_entry(&sh->batch_list, struct stripe_head,
				      batch_list);
		goto again;
	}
}

static void
ops_run_reconstruct6(struct stripe_head *sh, struct raid5_percpu *percpu,
		     struct dma_async_tx_descriptor *tx)
{
	struct async_submit_ctl submit;
	struct page **blocks;
	unsigned int *offs;
	int count, i, j = 0;
	struct stripe_head *head_sh = sh;
	int last_stripe;
	int synflags;
	unsigned long txflags;

	pr_debug("%s: stripe %llu\n", __func__, (unsigned long long)sh->sector);

	for (i = 0; i < sh->disks; i++) {
		if (sh->pd_idx == i || sh->qd_idx == i)
			continue;
		if (!test_bit(R5_Discard, &sh->dev[i].flags))
			break;
	}
	if (i >= sh->disks) {
		atomic_inc(&sh->count);
		set_bit(R5_Discard, &sh->dev[sh->pd_idx].flags);
		set_bit(R5_Discard, &sh->dev[sh->qd_idx].flags);
		ops_complete_reconstruct(sh);
		return;
	}

again:
	blocks = to_addr_page(percpu, j);
	offs = to_addr_offs(sh, percpu);

	if (sh->reconstruct_state == reconstruct_state_prexor_drain_run) {
		synflags = SYNDROME_SRC_WRITTEN;
		txflags = ASYNC_TX_ACK | ASYNC_TX_PQ_XOR_DST;
	} else {
		synflags = SYNDROME_SRC_ALL;
		txflags = ASYNC_TX_ACK;
	}

	count = set_syndrome_sources(blocks, offs, sh, synflags);
	last_stripe = !head_sh->batch_head ||
		list_first_entry(&sh->batch_list,
				 struct stripe_head, batch_list) == head_sh;

	if (last_stripe) {
		atomic_inc(&head_sh->count);
		init_async_submit(&submit, txflags, tx, ops_complete_reconstruct,
				  head_sh, to_addr_conv(sh, percpu, j));
	} else
		init_async_submit(&submit, 0, tx, NULL, NULL,
				  to_addr_conv(sh, percpu, j));
	tx = async_gen_syndrome(blocks, offs, count+2,
			RAID5_STRIPE_SIZE(sh->raid_conf),  &submit);
	if (!last_stripe) {
		j++;
		sh = list_first_entry(&sh->batch_list, struct stripe_head,
				      batch_list);
		goto again;
	}
}

static void ops_complete_check(void *stripe_head_ref)
{
	struct stripe_head *sh = stripe_head_ref;

	pr_debug("%s: stripe %llu\n", __func__,
		(unsigned long long)sh->sector);

	sh->check_state = check_state_check_result;
	set_bit(STRIPE_HANDLE, &sh->state);
	raid5_release_stripe(sh);
}

static void ops_run_check_p(struct stripe_head *sh, struct raid5_percpu *percpu)
{
	int disks = sh->disks;
	int pd_idx = sh->pd_idx;
	int qd_idx = sh->qd_idx;
	struct page *xor_dest;
	unsigned int off_dest;
	struct page **xor_srcs = to_addr_page(percpu, 0);
	unsigned int *off_srcs = to_addr_offs(sh, percpu);
	struct dma_async_tx_descriptor *tx;
	struct async_submit_ctl submit;
	int count;
	int i;

	pr_debug("%s: stripe %llu\n", __func__,
		(unsigned long long)sh->sector);

	BUG_ON(sh->batch_head);
	count = 0;
	xor_dest = sh->dev[pd_idx].page;
	off_dest = sh->dev[pd_idx].offset;
	off_srcs[count] = off_dest;
	xor_srcs[count++] = xor_dest;
	for (i = disks; i--; ) {
		if (i == pd_idx || i == qd_idx)
			continue;
		off_srcs[count] = sh->dev[i].offset;
		xor_srcs[count++] = sh->dev[i].page;
	}

	init_async_submit(&submit, 0, NULL, NULL, NULL,
			  to_addr_conv(sh, percpu, 0));
	tx = async_xor_val_offs(xor_dest, off_dest, xor_srcs, off_srcs, count,
			   RAID5_STRIPE_SIZE(sh->raid_conf),
			   &sh->ops.zero_sum_result, &submit);

	atomic_inc(&sh->count);
	init_async_submit(&submit, ASYNC_TX_ACK, tx, ops_complete_check, sh, NULL);
	tx = async_trigger_callback(&submit);
}

static void ops_run_check_pq(struct stripe_head *sh, struct raid5_percpu *percpu, int checkp)
{
	struct page **srcs = to_addr_page(percpu, 0);
	unsigned int *offs = to_addr_offs(sh, percpu);
	struct async_submit_ctl submit;
	int count;

	pr_debug("%s: stripe %llu checkp: %d\n", __func__,
		(unsigned long long)sh->sector, checkp);

	BUG_ON(sh->batch_head);
	count = set_syndrome_sources(srcs, offs, sh, SYNDROME_SRC_ALL);
	if (!checkp)
		srcs[count] = NULL;

	atomic_inc(&sh->count);
	init_async_submit(&submit, ASYNC_TX_ACK, NULL, ops_complete_check,
			  sh, to_addr_conv(sh, percpu, 0));
	async_syndrome_val(srcs, offs, count+2,
			   RAID5_STRIPE_SIZE(sh->raid_conf),
			   &sh->ops.zero_sum_result, percpu->spare_page, 0, &submit);
}

static void raid_run_ops(struct stripe_head *sh, unsigned long ops_request)
{
	int overlap_clear = 0, i, disks = sh->disks;
	struct dma_async_tx_descriptor *tx = NULL;
	struct r5conf *conf = sh->raid_conf;
	int level = conf->level;
	struct raid5_percpu *percpu;

	local_lock(&conf->percpu->lock);
	percpu = this_cpu_ptr(conf->percpu);
	if (test_bit(STRIPE_OP_BIOFILL, &ops_request)) {
		ops_run_biofill(sh);
		overlap_clear++;
	}

	if (test_bit(STRIPE_OP_COMPUTE_BLK, &ops_request)) {
		if (level < 6)
			tx = ops_run_compute5(sh, percpu);
		else {
			if (sh->ops.target2 < 0 || sh->ops.target < 0)
				tx = ops_run_compute6_1(sh, percpu);
			else
				tx = ops_run_compute6_2(sh, percpu);
		}
		/* terminate the chain if reconstruct is not set to be run */
		if (tx && !test_bit(STRIPE_OP_RECONSTRUCT, &ops_request))
			async_tx_ack(tx);
	}

	if (test_bit(STRIPE_OP_PREXOR, &ops_request)) {
		if (level < 6)
			tx = ops_run_prexor5(sh, percpu, tx);
		else
			tx = ops_run_prexor6(sh, percpu, tx);
	}

	if (test_bit(STRIPE_OP_PARTIAL_PARITY, &ops_request))
		tx = ops_run_partial_parity(sh, percpu, tx);

	if (test_bit(STRIPE_OP_BIODRAIN, &ops_request)) {
		tx = ops_run_biodrain(sh, tx);
		overlap_clear++;
	}

	if (test_bit(STRIPE_OP_RECONSTRUCT, &ops_request)) {
		if (level < 6)
			ops_run_reconstruct5(sh, percpu, tx);
		else
			ops_run_reconstruct6(sh, percpu, tx);
	}

	if (test_bit(STRIPE_OP_CHECK, &ops_request)) {
		if (sh->check_state == check_state_run)
			ops_run_check_p(sh, percpu);
		else if (sh->check_state == check_state_run_q)
			ops_run_check_pq(sh, percpu, 0);
		else if (sh->check_state == check_state_run_pq)
			ops_run_check_pq(sh, percpu, 1);
		else
			BUG();
	}

	if (overlap_clear && !sh->batch_head) {
		for (i = disks; i--; ) {
			struct r5dev *dev = &sh->dev[i];
			if (test_and_clear_bit(R5_Overlap, &dev->flags))
				wake_up_bit(&dev->flags, R5_Overlap);
		}
	}
	local_unlock(&conf->percpu->lock);
}

static void free_stripe(struct kmem_cache *sc, struct stripe_head *sh)
{
#if PAGE_SIZE != DEFAULT_STRIPE_SIZE
	kfree(sh->pages);
#endif
	if (sh->ppl_page)
		__free_page(sh->ppl_page);
	kmem_cache_free(sc, sh);
}

static struct stripe_head *alloc_stripe(struct kmem_cache *sc, gfp_t gfp,
	int disks, struct r5conf *conf)
{
	struct stripe_head *sh;

	sh = kmem_cache_zalloc(sc, gfp);
	if (sh) {
		spin_lock_init(&sh->stripe_lock);
		spin_lock_init(&sh->batch_lock);
		INIT_LIST_HEAD(&sh->batch_list);
		INIT_LIST_HEAD(&sh->lru);
		INIT_LIST_HEAD(&sh->r5c);
		INIT_LIST_HEAD(&sh->log_list);
		atomic_set(&sh->count, 1);
		sh->raid_conf = conf;
		sh->log_start = MaxSector;

		if (raid5_has_ppl(conf)) {
			sh->ppl_page = alloc_page(gfp);
			if (!sh->ppl_page) {
				free_stripe(sc, sh);
				return NULL;
			}
		}
#if PAGE_SIZE != DEFAULT_STRIPE_SIZE
		if (init_stripe_shared_pages(sh, conf, disks)) {
			free_stripe(sc, sh);
			return NULL;
		}
#endif
	}
	return sh;
}
static int grow_one_stripe(struct r5conf *conf, gfp_t gfp)
{
	struct stripe_head *sh;

	sh = alloc_stripe(conf->slab_cache, gfp, conf->pool_size, conf);
	if (!sh)
		return 0;

	if (grow_buffers(sh, gfp)) {
		shrink_buffers(sh);
		free_stripe(conf->slab_cache, sh);
		return 0;
	}
	sh->hash_lock_index =
		conf->max_nr_stripes % NR_STRIPE_HASH_LOCKS;
	/* we just created an active stripe so... */
	atomic_inc(&conf->active_stripes);

	raid5_release_stripe(sh);
	WRITE_ONCE(conf->max_nr_stripes, conf->max_nr_stripes + 1);
	return 1;
}

static int grow_stripes(struct r5conf *conf, int num)
{
	struct kmem_cache *sc;
	size_t namelen = sizeof(conf->cache_name[0]);
	int devs = max(conf->raid_disks, conf->previous_raid_disks);

	if (mddev_is_dm(conf->mddev))
		snprintf(conf->cache_name[0], namelen,
			"raid%d-%p", conf->level, conf->mddev);
	else
		snprintf(conf->cache_name[0], namelen,
			"raid%d-%s", conf->level, mdname(conf->mddev));
	snprintf(conf->cache_name[1], namelen, "%.27s-alt", conf->cache_name[0]);

	conf->active_name = 0;
	sc = kmem_cache_create(conf->cache_name[conf->active_name],
			       struct_size_t(struct stripe_head, dev, devs),
			       0, 0, NULL);
	if (!sc)
		return 1;
	conf->slab_cache = sc;
	conf->pool_size = devs;
	while (num--)
		if (!grow_one_stripe(conf, GFP_KERNEL))
			return 1;

	return 0;
}

/**
 * scribble_alloc - allocate percpu scribble buffer for required size
 *		    of the scribble region
 * @percpu: from for_each_present_cpu() of the caller
 * @num: total number of disks in the array
 * @cnt: scribble objs count for required size of the scribble region
 *
 * The scribble buffer size must be enough to contain:
 * 1/ a struct page pointer for each device in the array +2
 * 2/ room to convert each entry in (1) to its corresponding dma
 *    (dma_map_page()) or page (page_address()) address.
 *
 * Note: the +2 is for the destination buffers of the ddf/raid6 case where we
 * calculate over all devices (not just the data blocks), using zeros in place
 * of the P and Q blocks.
 */
static int scribble_alloc(struct raid5_percpu *percpu,
			  int num, int cnt)
{
	size_t obj_size =
		sizeof(struct page *) * (num + 2) +
		sizeof(addr_conv_t) * (num + 2) +
		sizeof(unsigned int) * (num + 2);
	void *scribble;

	/*
	 * If here is in raid array suspend context, it is in memalloc noio
	 * context as well, there is no potential recursive memory reclaim
	 * I/Os with the GFP_KERNEL flag.
	 */
	scribble = kvmalloc_array(cnt, obj_size, GFP_KERNEL);
	if (!scribble)
		return -ENOMEM;

	kvfree(percpu->scribble);

	percpu->scribble = scribble;
	percpu->scribble_obj_size = obj_size;
	return 0;
}

static int resize_chunks(struct r5conf *conf, int new_disks, int new_sectors)
{
	unsigned long cpu;
	int err = 0;

	/* Never shrink. */
	if (conf->scribble_disks >= new_disks &&
	    conf->scribble_sectors >= new_sectors)
		return 0;

	raid5_quiesce(conf->mddev, true);
	cpus_read_lock();

	for_each_present_cpu(cpu) {
		struct raid5_percpu *percpu;

		percpu = per_cpu_ptr(conf->percpu, cpu);
		err = scribble_alloc(percpu, new_disks,
				     new_sectors / RAID5_STRIPE_SECTORS(conf));
		if (err)
			break;
	}

	cpus_read_unlock();
	raid5_quiesce(conf->mddev, false);

	if (!err) {
		conf->scribble_disks = new_disks;
		conf->scribble_sectors = new_sectors;
	}
	return err;
}

static int resize_stripes(struct r5conf *conf, int newsize)
{
	/* Make all the stripes able to hold 'newsize' devices.
	 * New slots in each stripe get 'page' set to a new page.
	 *
	 * This happens in stages:
	 * 1/ create a new kmem_cache and allocate the required number of
	 *    stripe_heads.
	 * 2/ gather all the old stripe_heads and transfer the pages across
	 *    to the new stripe_heads.  This will have the side effect of
	 *    freezing the array as once all stripe_heads have been collected,
	 *    no IO will be possible.  Old stripe heads are freed once their
	 *    pages have been transferred over, and the old kmem_cache is
	 *    freed when all stripes are done.
	 * 3/ reallocate conf->disks to be suitable bigger.  If this fails,
	 *    we simple return a failure status - no need to clean anything up.
	 * 4/ allocate new pages for the new slots in the new stripe_heads.
	 *    If this fails, we don't bother trying the shrink the
	 *    stripe_heads down again, we just leave them as they are.
	 *    As each stripe_head is processed the new one is released into
	 *    active service.
	 *
	 * Once step2 is started, we cannot afford to wait for a write,
	 * so we use GFP_NOIO allocations.
	 */
	struct stripe_head *osh, *nsh;
	LIST_HEAD(newstripes);
	struct disk_info *ndisks;
	int err = 0;
	struct kmem_cache *sc;
	int i;
	int hash, cnt;

	md_allow_write(conf->mddev);

	/* Step 1 */
	sc = kmem_cache_create(conf->cache_name[1-conf->active_name],
			       struct_size_t(struct stripe_head, dev, newsize),
			       0, 0, NULL);
	if (!sc)
		return -ENOMEM;

	/* Need to ensure auto-resizing doesn't interfere */
	mutex_lock(&conf->cache_size_mutex);

	for (i = conf->max_nr_stripes; i; i--) {
		nsh = alloc_stripe(sc, GFP_KERNEL, newsize, conf);
		if (!nsh)
			break;

		list_add(&nsh->lru, &newstripes);
	}
	if (i) {
		/* didn't get enough, give up */
		while (!list_empty(&newstripes)) {
			nsh = list_entry(newstripes.next, struct stripe_head, lru);
			list_del(&nsh->lru);
			free_stripe(sc, nsh);
		}
		kmem_cache_destroy(sc);
		mutex_unlock(&conf->cache_size_mutex);
		return -ENOMEM;
	}
	/* Step 2 - Must use GFP_NOIO now.
	 * OK, we have enough stripes, start collecting inactive
	 * stripes and copying them over
	 */
	hash = 0;
	cnt = 0;
	list_for_each_entry(nsh, &newstripes, lru) {
		lock_device_hash_lock(conf, hash);
		wait_event_cmd(conf->wait_for_stripe,
				    !list_empty(conf->inactive_list + hash),
				    unlock_device_hash_lock(conf, hash),
				    lock_device_hash_lock(conf, hash));
		osh = get_free_stripe(conf, hash);
		unlock_device_hash_lock(conf, hash);

#if PAGE_SIZE != DEFAULT_STRIPE_SIZE
	for (i = 0; i < osh->nr_pages; i++) {
		nsh->pages[i] = osh->pages[i];
		osh->pages[i] = NULL;
	}
#endif
		for(i=0; i<conf->pool_size; i++) {
			nsh->dev[i].page = osh->dev[i].page;
			nsh->dev[i].orig_page = osh->dev[i].page;
			nsh->dev[i].offset = osh->dev[i].offset;
		}
		nsh->hash_lock_index = hash;
		free_stripe(conf->slab_cache, osh);
		cnt++;
		if (cnt >= conf->max_nr_stripes / NR_STRIPE_HASH_LOCKS +
		    !!((conf->max_nr_stripes % NR_STRIPE_HASH_LOCKS) > hash)) {
			hash++;
			cnt = 0;
		}
	}
	kmem_cache_destroy(conf->slab_cache);

	/* Step 3.
	 * At this point, we are holding all the stripes so the array
	 * is completely stalled, so now is a good time to resize
	 * conf->disks and the scribble region
	 */
	ndisks = kcalloc(newsize, sizeof(struct disk_info), GFP_NOIO);
	if (ndisks) {
		for (i = 0; i < conf->pool_size; i++)
			ndisks[i] = conf->disks[i];

		for (i = conf->pool_size; i < newsize; i++) {
			ndisks[i].extra_page = alloc_page(GFP_NOIO);
			if (!ndisks[i].extra_page)
				err = -ENOMEM;
		}

		if (err) {
			for (i = conf->pool_size; i < newsize; i++)
				if (ndisks[i].extra_page)
					put_page(ndisks[i].extra_page);
			kfree(ndisks);
		} else {
			kfree(conf->disks);
			conf->disks = ndisks;
		}
	} else
		err = -ENOMEM;

	conf->slab_cache = sc;
	conf->active_name = 1-conf->active_name;

	/* Step 4, return new stripes to service */
	while(!list_empty(&newstripes)) {
		nsh = list_entry(newstripes.next, struct stripe_head, lru);
		list_del_init(&nsh->lru);

#if PAGE_SIZE != DEFAULT_STRIPE_SIZE
		for (i = 0; i < nsh->nr_pages; i++) {
			if (nsh->pages[i])
				continue;
			nsh->pages[i] = alloc_page(GFP_NOIO);
			if (!nsh->pages[i])
				err = -ENOMEM;
		}

		for (i = conf->raid_disks; i < newsize; i++) {
			if (nsh->dev[i].page)
				continue;
			nsh->dev[i].page = raid5_get_dev_page(nsh, i);
			nsh->dev[i].orig_page = nsh->dev[i].page;
			nsh->dev[i].offset = raid5_get_page_offset(nsh, i);
		}
#else
		for (i=conf->raid_disks; i < newsize; i++)
			if (nsh->dev[i].page == NULL) {
				struct page *p = alloc_page(GFP_NOIO);
				nsh->dev[i].page = p;
				nsh->dev[i].orig_page = p;
				nsh->dev[i].offset = 0;
				if (!p)
					err = -ENOMEM;
			}
#endif
		raid5_release_stripe(nsh);
	}
	/* critical section pass, GFP_NOIO no longer needed */

	if (!err)
		conf->pool_size = newsize;
	mutex_unlock(&conf->cache_size_mutex);

	return err;
}

static int drop_one_stripe(struct r5conf *conf)
{
	struct stripe_head *sh;
	int hash = (conf->max_nr_stripes - 1) & STRIPE_HASH_LOCKS_MASK;

	spin_lock_irq(conf->hash_locks + hash);
	sh = get_free_stripe(conf, hash);
	spin_unlock_irq(conf->hash_locks + hash);
	if (!sh)
		return 0;
	BUG_ON(atomic_read(&sh->count));
	shrink_buffers(sh);
	free_stripe(conf->slab_cache, sh);
	atomic_dec(&conf->active_stripes);
	WRITE_ONCE(conf->max_nr_stripes, conf->max_nr_stripes - 1);
	return 1;
}

static void shrink_stripes(struct r5conf *conf)
{
	while (conf->max_nr_stripes &&
	       drop_one_stripe(conf))
		;

	kmem_cache_destroy(conf->slab_cache);
	conf->slab_cache = NULL;
}

static void raid5_end_read_request(struct bio * bi)
{
	struct stripe_head *sh = bi->bi_private;
	struct r5conf *conf = sh->raid_conf;
	int disks = sh->disks, i;
	struct md_rdev *rdev = NULL;
	sector_t s;

	for (i=0 ; i<disks; i++)
		if (bi == &sh->dev[i].req)
			break;

	pr_debug("end_read_request %llu/%d, count: %d, error %d.\n",
		(unsigned long long)sh->sector, i, atomic_read(&sh->count),
		bi->bi_status);
	if (i == disks) {
		BUG();
		return;
	}
	if (test_bit(R5_ReadRepl, &sh->dev[i].flags))
		/* If replacement finished while this request was outstanding,
		 * 'replacement' might be NULL already.
		 * In that case it moved down to 'rdev'.
		 * rdev is not removed until all requests are finished.
		 */
		rdev = conf->disks[i].replacement;
	if (!rdev)
		rdev = conf->disks[i].rdev;

	if (use_new_offset(conf, sh))
		s = sh->sector + rdev->new_data_offset;
	else
		s = sh->sector + rdev->data_offset;
	if (!bi->bi_status) {
		set_bit(R5_UPTODATE, &sh->dev[i].flags);
		if (test_bit(R5_ReadError, &sh->dev[i].flags)) {
			/* Note that this cannot happen on a
			 * replacement device.  We just fail those on
			 * any error
			 */
			pr_info_ratelimited(
				"md/raid:%s: read error corrected (%lu sectors at %llu on %pg)\n",
				mdname(conf->mddev), RAID5_STRIPE_SECTORS(conf),
				(unsigned long long)s,
				rdev->bdev);
			atomic_add(RAID5_STRIPE_SECTORS(conf), &rdev->corrected_errors);
			clear_bit(R5_ReadError, &sh->dev[i].flags);
			clear_bit(R5_ReWrite, &sh->dev[i].flags);
		} else if (test_bit(R5_ReadNoMerge, &sh->dev[i].flags))
			clear_bit(R5_ReadNoMerge, &sh->dev[i].flags);

		if (test_bit(R5_InJournal, &sh->dev[i].flags))
			/*
			 * end read for a page in journal, this
			 * must be preparing for prexor in rmw
			 */
			set_bit(R5_OrigPageUPTDODATE, &sh->dev[i].flags);

		if (atomic_read(&rdev->read_errors))
			atomic_set(&rdev->read_errors, 0);
	} else {
		int retry = 0;
		int set_bad = 0;

		clear_bit(R5_UPTODATE, &sh->dev[i].flags);
		if (!(bi->bi_status == BLK_STS_PROTECTION))
			atomic_inc(&rdev->read_errors);
		if (test_bit(R5_ReadRepl, &sh->dev[i].flags))
			pr_warn_ratelimited(
				"md/raid:%s: read error on replacement device (sector %llu on %pg).\n",
				mdname(conf->mddev),
				(unsigned long long)s,
				rdev->bdev);
		else if (conf->mddev->degraded >= conf->max_degraded) {
			set_bad = 1;
			pr_warn_ratelimited(
				"md/raid:%s: read error not correctable (sector %llu on %pg).\n",
				mdname(conf->mddev),
				(unsigned long long)s,
				rdev->bdev);
		} else if (test_bit(R5_ReWrite, &sh->dev[i].flags)) {
			/* Oh, no!!! */
			set_bad = 1;
			pr_warn_ratelimited(
				"md/raid:%s: read error NOT corrected!! (sector %llu on %pg).\n",
				mdname(conf->mddev),
				(unsigned long long)s,
				rdev->bdev);
		} else if (atomic_read(&rdev->read_errors)
			 > conf->max_nr_stripes) {
			if (!test_bit(Faulty, &rdev->flags)) {
				pr_warn("md/raid:%s: %d read_errors > %d stripes\n",
				    mdname(conf->mddev),
				    atomic_read(&rdev->read_errors),
				    conf->max_nr_stripes);
				pr_warn("md/raid:%s: Too many read errors, failing device %pg.\n",
				    mdname(conf->mddev), rdev->bdev);
			}
		} else
			retry = 1;
		if (set_bad && test_bit(In_sync, &rdev->flags)
		    && !test_bit(R5_ReadNoMerge, &sh->dev[i].flags))
			retry = 1;
		if (retry)
			if (sh->qd_idx >= 0 && sh->pd_idx == i)
				set_bit(R5_ReadError, &sh->dev[i].flags);
			else if (test_bit(R5_ReadNoMerge, &sh->dev[i].flags)) {
				set_bit(R5_ReadError, &sh->dev[i].flags);
				clear_bit(R5_ReadNoMerge, &sh->dev[i].flags);
			} else
				set_bit(R5_ReadNoMerge, &sh->dev[i].flags);
		else {
			clear_bit(R5_ReadError, &sh->dev[i].flags);
			clear_bit(R5_ReWrite, &sh->dev[i].flags);
			if (!(set_bad
			      && test_bit(In_sync, &rdev->flags)
			      && rdev_set_badblocks(
				      rdev, sh->sector, RAID5_STRIPE_SECTORS(conf), 0)))
				md_error(conf->mddev, rdev);
		}
	}
	rdev_dec_pending(rdev, conf->mddev);
	bio_uninit(bi);
	clear_bit(R5_LOCKED, &sh->dev[i].flags);
	set_bit(STRIPE_HANDLE, &sh->state);
	raid5_release_stripe(sh);
}

static void raid5_end_write_request(struct bio *bi)
{
	struct stripe_head *sh = bi->bi_private;
	struct r5conf *conf = sh->raid_conf;
	int disks = sh->disks, i;
	struct md_rdev *rdev;
	int replacement = 0;

	for (i = 0 ; i < disks; i++) {
		if (bi == &sh->dev[i].req) {
			rdev = conf->disks[i].rdev;
			break;
		}
		if (bi == &sh->dev[i].rreq) {
			rdev = conf->disks[i].replacement;
			if (rdev)
				replacement = 1;
			else
				/* rdev was removed and 'replacement'
				 * replaced it.  rdev is not removed
				 * until all requests are finished.
				 */
				rdev = conf->disks[i].rdev;
			break;
		}
	}
	pr_debug("end_write_request %llu/%d, count %d, error: %d.\n",
		(unsigned long long)sh->sector, i, atomic_read(&sh->count),
		bi->bi_status);
	if (i == disks) {
		BUG();
		return;
	}

	if (replacement) {
		if (bi->bi_status)
			md_error(conf->mddev, rdev);
		else if (rdev_has_badblock(rdev, sh->sector,
					   RAID5_STRIPE_SECTORS(conf)))
			set_bit(R5_MadeGoodRepl, &sh->dev[i].flags);
	} else {
		if (bi->bi_status) {
			set_bit(WriteErrorSeen, &rdev->flags);
			set_bit(R5_WriteError, &sh->dev[i].flags);
			if (!test_and_set_bit(WantReplacement, &rdev->flags))
				set_bit(MD_RECOVERY_NEEDED,
					&rdev->mddev->recovery);
		} else if (rdev_has_badblock(rdev, sh->sector,
					     RAID5_STRIPE_SECTORS(conf))) {
			set_bit(R5_MadeGood, &sh->dev[i].flags);
			if (test_bit(R5_ReadError, &sh->dev[i].flags))
				/* That was a successful write so make
				 * sure it looks like we already did
				 * a re-write.
				 */
				set_bit(R5_ReWrite, &sh->dev[i].flags);
		}
	}
	rdev_dec_pending(rdev, conf->mddev);

	if (sh->batch_head && bi->bi_status && !replacement)
		set_bit(STRIPE_BATCH_ERR, &sh->batch_head->state);

	bio_uninit(bi);
	if (!test_and_clear_bit(R5_DOUBLE_LOCKED, &sh->dev[i].flags))
		clear_bit(R5_LOCKED, &sh->dev[i].flags);
	set_bit(STRIPE_HANDLE, &sh->state);

	if (sh->batch_head && sh != sh->batch_head)
		raid5_release_stripe(sh->batch_head);
	raid5_release_stripe(sh);
}

static void raid5_error(struct mddev *mddev, struct md_rdev *rdev)
{
	struct r5conf *conf = mddev->private;
	unsigned long flags;
	pr_debug("raid456: error called\n");

	pr_crit("md/raid:%s: Disk failure on %pg, disabling device.\n",
		mdname(mddev), rdev->bdev);

	spin_lock_irqsave(&conf->device_lock, flags);
	set_bit(Faulty, &rdev->flags);
	clear_bit(In_sync, &rdev->flags);
	mddev->degraded = raid5_calc_degraded(conf);

	if (has_failed(conf)) {
		set_bit(MD_BROKEN, &conf->mddev->flags);
		conf->recovery_disabled = mddev->recovery_disabled;

		pr_crit("md/raid:%s: Cannot continue operation (%d/%d failed).\n",
			mdname(mddev), mddev->degraded, conf->raid_disks);
	} else {
		pr_crit("md/raid:%s: Operation continuing on %d devices.\n",
			mdname(mddev), conf->raid_disks - mddev->degraded);
	}

	spin_unlock_irqrestore(&conf->device_lock, flags);
	set_bit(MD_RECOVERY_INTR, &mddev->recovery);

	set_bit(Blocked, &rdev->flags);
	set_mask_bits(&mddev->sb_flags, 0,
		      BIT(MD_SB_CHANGE_DEVS) | BIT(MD_SB_CHANGE_PENDING));
	r5c_update_on_rdev_error(mddev, rdev);
}

/*
 * Input: a 'big' sector number,
 * Output: index of the data and parity disk, and the sector # in them.
 */
sector_t raid5_compute_sector(struct r5conf *conf, sector_t r_sector,
			      int previous, int *dd_idx,
			      struct stripe_head *sh)
{
	sector_t stripe, stripe2;
	sector_t chunk_number;
	unsigned int chunk_offset;
	int pd_idx, qd_idx;
	int ddf_layout = 0;
	sector_t new_sector;
	int algorithm = previous ? conf->prev_algo
				 : conf->algorithm;
	int sectors_per_chunk = previous ? conf->prev_chunk_sectors
					 : conf->chunk_sectors;
	int raid_disks = previous ? conf->previous_raid_disks
				  : conf->raid_disks;
	int data_disks = raid_disks - conf->max_degraded;

	/* First compute the information on this sector */

	/*
	 * Compute the chunk number and the sector offset inside the chunk
	 */
	chunk_offset = sector_div(r_sector, sectors_per_chunk);
	chunk_number = r_sector;

	/*
	 * Compute the stripe number
	 */
	stripe = chunk_number;
	*dd_idx = sector_div(stripe, data_disks);
	stripe2 = stripe;
	/*
	 * Select the parity disk based on the user selected algorithm.
	 */
	pd_idx = qd_idx = -1;
	switch(conf->level) {
	case 4:
		pd_idx = data_disks;
		break;
	case 5:
		switch (algorithm) {
		case ALGORITHM_LEFT_ASYMMETRIC:
			pd_idx = data_disks - sector_div(stripe2, raid_disks);
			if (*dd_idx >= pd_idx)
				(*dd_idx)++;
			break;
		case ALGORITHM_RIGHT_ASYMMETRIC:
			pd_idx = sector_div(stripe2, raid_disks);
			if (*dd_idx >= pd_idx)
				(*dd_idx)++;
			break;
		case ALGORITHM_LEFT_SYMMETRIC:
			pd_idx = data_disks - sector_div(stripe2, raid_disks);
			*dd_idx = (pd_idx + 1 + *dd_idx) % raid_disks;
			break;
		case ALGORITHM_RIGHT_SYMMETRIC:
			pd_idx = sector_div(stripe2, raid_disks);
			*dd_idx = (pd_idx + 1 + *dd_idx) % raid_disks;
			break;
		case ALGORITHM_PARITY_0:
			pd_idx = 0;
			(*dd_idx)++;
			break;
		case ALGORITHM_PARITY_N:
			pd_idx = data_disks;
			break;
		default:
			BUG();
		}
		break;
	case 6:

		switch (algorithm) {
		case ALGORITHM_LEFT_ASYMMETRIC:
			pd_idx = raid_disks - 1 - sector_div(stripe2, raid_disks);
			qd_idx = pd_idx + 1;
			if (pd_idx == raid_disks-1) {
				(*dd_idx)++;	/* Q D D D P */
				qd_idx = 0;
			} else if (*dd_idx >= pd_idx)
				(*dd_idx) += 2; /* D D P Q D */
			break;
		case ALGORITHM_RIGHT_ASYMMETRIC:
			pd_idx = sector_div(stripe2, raid_disks);
			qd_idx = pd_idx + 1;
			if (pd_idx == raid_disks-1) {
				(*dd_idx)++;	/* Q D D D P */
				qd_idx = 0;
			} else if (*dd_idx >= pd_idx)
				(*dd_idx) += 2; /* D D P Q D */
			break;
		case ALGORITHM_LEFT_SYMMETRIC:
			pd_idx = raid_disks - 1 - sector_div(stripe2, raid_disks);
			qd_idx = (pd_idx + 1) % raid_disks;
			*dd_idx = (pd_idx + 2 + *dd_idx) % raid_disks;
			break;
		case ALGORITHM_RIGHT_SYMMETRIC:
			pd_idx = sector_div(stripe2, raid_disks);
			qd_idx = (pd_idx + 1) % raid_disks;
			*dd_idx = (pd_idx + 2 + *dd_idx) % raid_disks;
			break;

		case ALGORITHM_PARITY_0:
			pd_idx = 0;
			qd_idx = 1;
			(*dd_idx) += 2;
			break;
		case ALGORITHM_PARITY_N:
			pd_idx = data_disks;
			qd_idx = data_disks + 1;
			break;

		case ALGORITHM_ROTATING_ZERO_RESTART:
			/* Exactly the same as RIGHT_ASYMMETRIC, but or
			 * of blocks for computing Q is different.
			 */
			pd_idx = sector_div(stripe2, raid_disks);
			qd_idx = pd_idx + 1;
			if (pd_idx == raid_disks-1) {
				(*dd_idx)++;	/* Q D D D P */
				qd_idx = 0;
			} else if (*dd_idx >= pd_idx)
				(*dd_idx) += 2; /* D D P Q D */
			ddf_layout = 1;
			break;

		case ALGORITHM_ROTATING_N_RESTART:
			/* Same a left_asymmetric, by first stripe is
			 * D D D P Q  rather than
			 * Q D D D P
			 */
			stripe2 += 1;
			pd_idx = raid_disks - 1 - sector_div(stripe2, raid_disks);
			qd_idx = pd_idx + 1;
			if (pd_idx == raid_disks-1) {
				(*dd_idx)++;	/* Q D D D P */
				qd_idx = 0;
			} else if (*dd_idx >= pd_idx)
				(*dd_idx) += 2; /* D D P Q D */
			ddf_layout = 1;
			break;

		case ALGORITHM_ROTATING_N_CONTINUE:
			/* Same as left_symmetric but Q is before P */
			pd_idx = raid_disks - 1 - sector_div(stripe2, raid_disks);
			qd_idx = (pd_idx + raid_disks - 1) % raid_disks;
			*dd_idx = (pd_idx + 1 + *dd_idx) % raid_disks;
			ddf_layout = 1;
			break;

		case ALGORITHM_LEFT_ASYMMETRIC_6:
			/* RAID5 left_asymmetric, with Q on last device */
			pd_idx = data_disks - sector_div(stripe2, raid_disks-1);
			if (*dd_idx >= pd_idx)
				(*dd_idx)++;
			qd_idx = raid_disks - 1;
			break;

		case ALGORITHM_RIGHT_ASYMMETRIC_6:
			pd_idx = sector_div(stripe2, raid_disks-1);
			if (*dd_idx >= pd_idx)
				(*dd_idx)++;
			qd_idx = raid_disks - 1;
			break;

		case ALGORITHM_LEFT_SYMMETRIC_6:
			pd_idx = data_disks - sector_div(stripe2, raid_disks-1);
			*dd_idx = (pd_idx + 1 + *dd_idx) % (raid_disks-1);
			qd_idx = raid_disks - 1;
			break;

		case ALGORITHM_RIGHT_SYMMETRIC_6:
			pd_idx = sector_div(stripe2, raid_disks-1);
			*dd_idx = (pd_idx + 1 + *dd_idx) % (raid_disks-1);
			qd_idx = raid_disks - 1;
			break;

		case ALGORITHM_PARITY_0_6:
			pd_idx = 0;
			(*dd_idx)++;
			qd_idx = raid_disks - 1;
			break;

		default:
			BUG();
		}
		break;
	}

	if (sh) {
		sh->pd_idx = pd_idx;
		sh->qd_idx = qd_idx;
		sh->ddf_layout = ddf_layout;
	}
	/*
	 * Finally, compute the new sector number
	 */
	new_sector = (sector_t)stripe * sectors_per_chunk + chunk_offset;
	return new_sector;
}

sector_t raid5_compute_blocknr(struct stripe_head *sh, int i, int previous)
{
	struct r5conf *conf = sh->raid_conf;
	int raid_disks = sh->disks;
	int data_disks = raid_disks - conf->max_degraded;
	sector_t new_sector = sh->sector, check;
	int sectors_per_chunk = previous ? conf->prev_chunk_sectors
					 : conf->chunk_sectors;
	int algorithm = previous ? conf->prev_algo
				 : conf->algorithm;
	sector_t stripe;
	int chunk_offset;
	sector_t chunk_number;
	int dummy1, dd_idx = i;
	sector_t r_sector;
	struct stripe_head sh2;

	chunk_offset = sector_div(new_sector, sectors_per_chunk);
	stripe = new_sector;

	if (i == sh->pd_idx)
		return 0;
	switch(conf->level) {
	case 4: break;
	case 5:
		switch (algorithm) {
		case ALGORITHM_LEFT_ASYMMETRIC:
		case ALGORITHM_RIGHT_ASYMMETRIC:
			if (i > sh->pd_idx)
				i--;
			break;
		case ALGORITHM_LEFT_SYMMETRIC:
		case ALGORITHM_RIGHT_SYMMETRIC:
			if (i < sh->pd_idx)
				i += raid_disks;
			i -= (sh->pd_idx + 1);
			break;
		case ALGORITHM_PARITY_0:
			i -= 1;
			break;
		case ALGORITHM_PARITY_N:
			break;
		default:
			BUG();
		}
		break;
	case 6:
		if (i == sh->qd_idx)
			return 0; /* It is the Q disk */
		switch (algorithm) {
		case ALGORITHM_LEFT_ASYMMETRIC:
		case ALGORITHM_RIGHT_ASYMMETRIC:
		case ALGORITHM_ROTATING_ZERO_RESTART:
		case ALGORITHM_ROTATING_N_RESTART:
			if (sh->pd_idx == raid_disks-1)
				i--;	/* Q D D D P */
			else if (i > sh->pd_idx)
				i -= 2; /* D D P Q D */
			break;
		case ALGORITHM_LEFT_SYMMETRIC:
		case ALGORITHM_RIGHT_SYMMETRIC:
			if (sh->pd_idx == raid_disks-1)
				i--; /* Q D D D P */
			else {
				/* D D P Q D */
				if (i < sh->pd_idx)
					i += raid_disks;
				i -= (sh->pd_idx + 2);
			}
			break;
		case ALGORITHM_PARITY_0:
			i -= 2;
			break;
		case ALGORITHM_PARITY_N:
			break;
		case ALGORITHM_ROTATING_N_CONTINUE:
			/* Like left_symmetric, but P is before Q */
			if (sh->pd_idx == 0)
				i--;	/* P D D D Q */
			else {
				/* D D Q P D */
				if (i < sh->pd_idx)
					i += raid_disks;
				i -= (sh->pd_idx + 1);
			}
			break;
		case ALGORITHM_LEFT_ASYMMETRIC_6:
		case ALGORITHM_RIGHT_ASYMMETRIC_6:
			if (i > sh->pd_idx)
				i--;
			break;
		case ALGORITHM_LEFT_SYMMETRIC_6:
		case ALGORITHM_RIGHT_SYMMETRIC_6:
			if (i < sh->pd_idx)
				i += data_disks + 1;
			i -= (sh->pd_idx + 1);
			break;
		case ALGORITHM_PARITY_0_6:
			i -= 1;
			break;
		default:
			BUG();
		}
		break;
	}

	chunk_number = stripe * data_disks + i;
	r_sector = chunk_number * sectors_per_chunk + chunk_offset;

	check = raid5_compute_sector(conf, r_sector,
				     previous, &dummy1, &sh2);
	if (check != sh->sector || dummy1 != dd_idx || sh2.pd_idx != sh->pd_idx
		|| sh2.qd_idx != sh->qd_idx) {
		pr_warn("md/raid:%s: compute_blocknr: map not correct\n",
			mdname(conf->mddev));
		return 0;
	}
	return r_sector;
}

/*
 * There are cases where we want handle_stripe_dirtying() and
 * schedule_reconstruction() to delay towrite to some dev of a stripe.
 *
 * This function checks whether we want to delay the towrite. Specifically,
 * we delay the towrite when:
 *
 *   1. degraded stripe has a non-overwrite to the missing dev, AND this
 *      stripe has data in journal (for other devices).
 *
 *      In this case, when reading data for the non-overwrite dev, it is
 *      necessary to handle complex rmw of write back cache (prexor with
 *      orig_page, and xor with page). To keep read path simple, we would
 *      like to flush data in journal to RAID disks first, so complex rmw
 *      is handled in the write patch (handle_stripe_dirtying).
 *
 *   2. when journal space is critical (R5C_LOG_CRITICAL=1)
 *
 *      It is important to be able to flush all stripes in raid5-cache.
 *      Therefore, we need reserve some space on the journal device for
 *      these flushes. If flush operation includes pending writes to the
 *      stripe, we need to reserve (conf->raid_disk + 1) pages per stripe
 *      for the flush out. If we exclude these pending writes from flush
 *      operation, we only need (conf->max_degraded + 1) pages per stripe.
 *      Therefore, excluding pending writes in these cases enables more
 *      efficient use of the journal device.
 *
 *      Note: To make sure the stripe makes progress, we only delay
 *      towrite for stripes with data already in journal (injournal > 0).
 *      When LOG_CRITICAL, stripes with injournal == 0 will be sent to
 *      no_space_stripes list.
 *
 *   3. during journal failure
 *      In journal failure, we try to flush all cached data to raid disks
 *      based on data in stripe cache. The array is read-only to upper
 *      layers, so we would skip all pending writes.
 *
 */
static inline bool delay_towrite(struct r5conf *conf,
				 struct r5dev *dev,
				 struct stripe_head_state *s)
{
	/* case 1 above */
	if (!test_bit(R5_OVERWRITE, &dev->flags) &&
	    !test_bit(R5_Insync, &dev->flags) && s->injournal)
		return true;
	/* case 2 above */
	if (test_bit(R5C_LOG_CRITICAL, &conf->cache_state) &&
	    s->injournal > 0)
		return true;
	/* case 3 above */
	if (s->log_failed && s->injournal)
		return true;
	return false;
}

static void
schedule_reconstruction(struct stripe_head *sh, struct stripe_head_state *s,
			 int rcw, int expand)
{
	int i, pd_idx = sh->pd_idx, qd_idx = sh->qd_idx, disks = sh->disks;
	struct r5conf *conf = sh->raid_conf;
	int level = conf->level;

	if (rcw) {
		/*
		 * In some cases, handle_stripe_dirtying initially decided to
		 * run rmw and allocates extra page for prexor. However, rcw is
		 * cheaper later on. We need to free the extra page now,
		 * because we won't be able to do that in ops_complete_prexor().
		 */
		r5c_release_extra_page(sh);

		for (i = disks; i--; ) {
			struct r5dev *dev = &sh->dev[i];

			if (dev->towrite && !delay_towrite(conf, dev, s)) {
				set_bit(R5_LOCKED, &dev->flags);
				set_bit(R5_Wantdrain, &dev->flags);
				if (!expand)
					clear_bit(R5_UPTODATE, &dev->flags);
				s->locked++;
			} else if (test_bit(R5_InJournal, &dev->flags)) {
				set_bit(R5_LOCKED, &dev->flags);
				s->locked++;
			}
		}
		/* if we are not expanding this is a proper write request, and
		 * there will be bios with new data to be drained into the
		 * stripe cache
		 */
		if (!expand) {
			if (!s->locked)
				/* False alarm, nothing to do */
				return;
			sh->reconstruct_state = reconstruct_state_drain_run;
			set_bit(STRIPE_OP_BIODRAIN, &s->ops_request);
		} else
			sh->reconstruct_state = reconstruct_state_run;

		set_bit(STRIPE_OP_RECONSTRUCT, &s->ops_request);

		if (s->locked + conf->max_degraded == disks)
			if (!test_and_set_bit(STRIPE_FULL_WRITE, &sh->state))
				atomic_inc(&conf->pending_full_writes);
	} else {
		BUG_ON(!(test_bit(R5_UPTODATE, &sh->dev[pd_idx].flags) ||
			test_bit(R5_Wantcompute, &sh->dev[pd_idx].flags)));
		BUG_ON(level == 6 &&
			(!(test_bit(R5_UPTODATE, &sh->dev[qd_idx].flags) ||
			   test_bit(R5_Wantcompute, &sh->dev[qd_idx].flags))));

		for (i = disks; i--; ) {
			struct r5dev *dev = &sh->dev[i];
			if (i == pd_idx || i == qd_idx)
				continue;

			if (dev->towrite &&
			    (test_bit(R5_UPTODATE, &dev->flags) ||
			     test_bit(R5_Wantcompute, &dev->flags))) {
				set_bit(R5_Wantdrain, &dev->flags);
				set_bit(R5_LOCKED, &dev->flags);
				clear_bit(R5_UPTODATE, &dev->flags);
				s->locked++;
			} else if (test_bit(R5_InJournal, &dev->flags)) {
				set_bit(R5_LOCKED, &dev->flags);
				s->locked++;
			}
		}
		if (!s->locked)
			/* False alarm - nothing to do */
			return;
		sh->reconstruct_state = reconstruct_state_prexor_drain_run;
		set_bit(STRIPE_OP_PREXOR, &s->ops_request);
		set_bit(STRIPE_OP_BIODRAIN, &s->ops_request);
		set_bit(STRIPE_OP_RECONSTRUCT, &s->ops_request);
	}

	/* keep the parity disk(s) locked while asynchronous operations
	 * are in flight
	 */
	set_bit(R5_LOCKED, &sh->dev[pd_idx].flags);
	clear_bit(R5_UPTODATE, &sh->dev[pd_idx].flags);
	s->locked++;

	if (level == 6) {
		int qd_idx = sh->qd_idx;
		struct r5dev *dev = &sh->dev[qd_idx];

		set_bit(R5_LOCKED, &dev->flags);
		clear_bit(R5_UPTODATE, &dev->flags);
		s->locked++;
	}

	if (raid5_has_ppl(sh->raid_conf) && sh->ppl_page &&
	    test_bit(STRIPE_OP_BIODRAIN, &s->ops_request) &&
	    !test_bit(STRIPE_FULL_WRITE, &sh->state) &&
	    test_bit(R5_Insync, &sh->dev[pd_idx].flags))
		set_bit(STRIPE_OP_PARTIAL_PARITY, &s->ops_request);

	pr_debug("%s: stripe %llu locked: %d ops_request: %lx\n",
		__func__, (unsigned long long)sh->sector,
		s->locked, s->ops_request);
}

static bool stripe_bio_overlaps(struct stripe_head *sh, struct bio *bi,
				int dd_idx, int forwrite)
{
	struct r5conf *conf = sh->raid_conf;
	struct bio **bip;

	pr_debug("checking bi b#%llu to stripe s#%llu\n",
		 bi->bi_iter.bi_sector, sh->sector);

	/* Don't allow new IO added to stripes in batch list */
	if (sh->batch_head)
		return true;

	if (forwrite)
		bip = &sh->dev[dd_idx].towrite;
	else
		bip = &sh->dev[dd_idx].toread;

	while (*bip && (*bip)->bi_iter.bi_sector < bi->bi_iter.bi_sector) {
		if (bio_end_sector(*bip) > bi->bi_iter.bi_sector)
			return true;
		bip = &(*bip)->bi_next;
	}

	if (*bip && (*bip)->bi_iter.bi_sector < bio_end_sector(bi))
		return true;

	if (forwrite && raid5_has_ppl(conf)) {
		/*
		 * With PPL only writes to consecutive data chunks within a
		 * stripe are allowed because for a single stripe_head we can
		 * only have one PPL entry at a time, which describes one data
		 * range. Not really an overlap, but R5_Overlap can be
		 * used to handle this.
		 */
		sector_t sector;
		sector_t first = 0;
		sector_t last = 0;
		int count = 0;
		int i;

		for (i = 0; i < sh->disks; i++) {
			if (i != sh->pd_idx &&
			    (i == dd_idx || sh->dev[i].towrite)) {
				sector = sh->dev[i].sector;
				if (count == 0 || sector < first)
					first = sector;
				if (sector > last)
					last = sector;
				count++;
			}
		}

		if (first + conf->chunk_sectors * (count - 1) != last)
			return true;
	}

	return false;
}

static void __add_stripe_bio(struct stripe_head *sh, struct bio *bi,
			     int dd_idx, int forwrite, int previous)
{
	struct r5conf *conf = sh->raid_conf;
	struct bio **bip;
	int firstwrite = 0;

	if (forwrite) {
		bip = &sh->dev[dd_idx].towrite;
		if (!*bip)
			firstwrite = 1;
	} else {
		bip = &sh->dev[dd_idx].toread;
	}

	while (*bip && (*bip)->bi_iter.bi_sector < bi->bi_iter.bi_sector)
		bip = &(*bip)->bi_next;

	if (!forwrite || previous)
		clear_bit(STRIPE_BATCH_READY, &sh->state);

	BUG_ON(*bip && bi->bi_next && (*bip) != bi->bi_next);
	if (*bip)
		bi->bi_next = *bip;
	*bip = bi;
	bio_inc_remaining(bi);
	md_write_inc(conf->mddev, bi);

	if (forwrite) {
		/* check if page is covered */
		sector_t sector = sh->dev[dd_idx].sector;
		for (bi=sh->dev[dd_idx].towrite;
		     sector < sh->dev[dd_idx].sector + RAID5_STRIPE_SECTORS(conf) &&
			     bi && bi->bi_iter.bi_sector <= sector;
		     bi = r5_next_bio(conf, bi, sh->dev[dd_idx].sector)) {
			if (bio_end_sector(bi) >= sector)
				sector = bio_end_sector(bi);
		}
		if (sector >= sh->dev[dd_idx].sector + RAID5_STRIPE_SECTORS(conf))
			if (!test_and_set_bit(R5_OVERWRITE, &sh->dev[dd_idx].flags))
				sh->overwrite_disks++;
	}

	pr_debug("added bi b#%llu to stripe s#%llu, disk %d, logical %llu\n",
		 (*bip)->bi_iter.bi_sector, sh->sector, dd_idx,
		 sh->dev[dd_idx].sector);

	if (conf->mddev->bitmap && firstwrite && !sh->batch_head) {
		sh->bm_seq = conf->seq_flush+1;
		set_bit(STRIPE_BIT_DELAY, &sh->state);
	}
}

/*
 * Each stripe/dev can have one or more bios attached.
 * toread/towrite point to the first in a chain.
 * The bi_next chain must be in order.
 */
static bool add_stripe_bio(struct stripe_head *sh, struct bio *bi,
			   int dd_idx, int forwrite, int previous)
{
	spin_lock_irq(&sh->stripe_lock);

	if (stripe_bio_overlaps(sh, bi, dd_idx, forwrite)) {
		set_bit(R5_Overlap, &sh->dev[dd_idx].flags);
		spin_unlock_irq(&sh->stripe_lock);
		return false;
	}

	__add_stripe_bio(sh, bi, dd_idx, forwrite, previous);
	spin_unlock_irq(&sh->stripe_lock);
	return true;
}

static void end_reshape(struct r5conf *conf);

static void stripe_set_idx(sector_t stripe, struct r5conf *conf, int previous,
			    struct stripe_head *sh)
{
	int sectors_per_chunk =
		previous ? conf->prev_chunk_sectors : conf->chunk_sectors;
	int dd_idx;
	int chunk_offset = sector_div(stripe, sectors_per_chunk);
	int disks = previous ? conf->previous_raid_disks : conf->raid_disks;

	raid5_compute_sector(conf,
			     stripe * (disks - conf->max_degraded)
			     *sectors_per_chunk + chunk_offset,
			     previous,
			     &dd_idx, sh);
}

static void
handle_failed_stripe(struct r5conf *conf, struct stripe_head *sh,
		     struct stripe_head_state *s, int disks)
{
	int i;
	BUG_ON(sh->batch_head);
	for (i = disks; i--; ) {
		struct bio *bi;

		if (test_bit(R5_ReadError, &sh->dev[i].flags)) {
			struct md_rdev *rdev = conf->disks[i].rdev;

			if (rdev && test_bit(In_sync, &rdev->flags) &&
			    !test_bit(Faulty, &rdev->flags))
				atomic_inc(&rdev->nr_pending);
			else
				rdev = NULL;
			if (rdev) {
				if (!rdev_set_badblocks(
					    rdev,
					    sh->sector,
					    RAID5_STRIPE_SECTORS(conf), 0))
					md_error(conf->mddev, rdev);
				rdev_dec_pending(rdev, conf->mddev);
			}
		}
		spin_lock_irq(&sh->stripe_lock);
		/* fail all writes first */
		bi = sh->dev[i].towrite;
		sh->dev[i].towrite = NULL;
		sh->overwrite_disks = 0;
		spin_unlock_irq(&sh->stripe_lock);

		log_stripe_write_finished(sh);

		if (test_and_clear_bit(R5_Overlap, &sh->dev[i].flags))
			wake_up_bit(&sh->dev[i].flags, R5_Overlap);

		while (bi && bi->bi_iter.bi_sector <
			sh->dev[i].sector + RAID5_STRIPE_SECTORS(conf)) {
			struct bio *nextbi = r5_next_bio(conf, bi, sh->dev[i].sector);

			md_write_end(conf->mddev);
			bio_io_error(bi);
			bi = nextbi;
		}
		/* and fail all 'written' */
		bi = sh->dev[i].written;
		sh->dev[i].written = NULL;
		if (test_and_clear_bit(R5_SkipCopy, &sh->dev[i].flags)) {
			WARN_ON(test_bit(R5_UPTODATE, &sh->dev[i].flags));
			sh->dev[i].page = sh->dev[i].orig_page;
		}

		while (bi && bi->bi_iter.bi_sector <
		       sh->dev[i].sector + RAID5_STRIPE_SECTORS(conf)) {
			struct bio *bi2 = r5_next_bio(conf, bi, sh->dev[i].sector);

			md_write_end(conf->mddev);
			bio_io_error(bi);
			bi = bi2;
		}

		/* fail any reads if this device is non-operational and
		 * the data has not reached the cache yet.
		 */
		if (!test_bit(R5_Wantfill, &sh->dev[i].flags) &&
		    s->failed > conf->max_degraded &&
		    (!test_bit(R5_Insync, &sh->dev[i].flags) ||
		      test_bit(R5_ReadError, &sh->dev[i].flags))) {
			spin_lock_irq(&sh->stripe_lock);
			bi = sh->dev[i].toread;
			sh->dev[i].toread = NULL;
			spin_unlock_irq(&sh->stripe_lock);
			if (test_and_clear_bit(R5_Overlap, &sh->dev[i].flags))
				wake_up_bit(&sh->dev[i].flags, R5_Overlap);
			if (bi)
				s->to_read--;
			while (bi && bi->bi_iter.bi_sector <
			       sh->dev[i].sector + RAID5_STRIPE_SECTORS(conf)) {
				struct bio *nextbi =
					r5_next_bio(conf, bi, sh->dev[i].sector);

				bio_io_error(bi);
				bi = nextbi;
			}
		}
		/* If we were in the middle of a write the parity block might
		 * still be locked - so just clear all R5_LOCKED flags
		 */
		clear_bit(R5_LOCKED, &sh->dev[i].flags);
	}
	s->to_write = 0;
	s->written = 0;

	if (test_and_clear_bit(STRIPE_FULL_WRITE, &sh->state))
		if (atomic_dec_and_test(&conf->pending_full_writes))
			md_wakeup_thread(conf->mddev->thread);
}

static void
handle_failed_sync(struct r5conf *conf, struct stripe_head *sh,
		   struct stripe_head_state *s)
{
	int abort = 0;
	int i;

	BUG_ON(sh->batch_head);
	clear_bit(STRIPE_SYNCING, &sh->state);
	if (test_and_clear_bit(R5_Overlap, &sh->dev[sh->pd_idx].flags))
		wake_up_bit(&sh->dev[sh->pd_idx].flags, R5_Overlap);
	s->syncing = 0;
	s->replacing = 0;
	/* There is nothing more to do for sync/check/repair.
	 * Don't even need to abort as that is handled elsewhere
	 * if needed, and not always wanted e.g. if there is a known
	 * bad block here.
	 * For recover/replace we need to record a bad block on all
	 * non-sync devices, or abort the recovery
	 */
	if (test_bit(MD_RECOVERY_RECOVER, &conf->mddev->recovery)) {
		/* During recovery devices cannot be removed, so
		 * locking and refcounting of rdevs is not needed
		 */
		for (i = 0; i < conf->raid_disks; i++) {
			struct md_rdev *rdev = conf->disks[i].rdev;

			if (rdev
			    && !test_bit(Faulty, &rdev->flags)
			    && !test_bit(In_sync, &rdev->flags)
			    && !rdev_set_badblocks(rdev, sh->sector,
						   RAID5_STRIPE_SECTORS(conf), 0))
				abort = 1;
			rdev = conf->disks[i].replacement;

			if (rdev
			    && !test_bit(Faulty, &rdev->flags)
			    && !test_bit(In_sync, &rdev->flags)
			    && !rdev_set_badblocks(rdev, sh->sector,
						   RAID5_STRIPE_SECTORS(conf), 0))
				abort = 1;
		}
		if (abort)
			conf->recovery_disabled =
				conf->mddev->recovery_disabled;
	}
	md_done_sync(conf->mddev, RAID5_STRIPE_SECTORS(conf), !abort);
}

static int want_replace(struct stripe_head *sh, int disk_idx)
{
	struct md_rdev *rdev;
	int rv = 0;

	rdev = sh->raid_conf->disks[disk_idx].replacement;
	if (rdev
	    && !test_bit(Faulty, &rdev->flags)
	    && !test_bit(In_sync, &rdev->flags)
	    && (rdev->recovery_offset <= sh->sector
		|| rdev->mddev->recovery_cp <= sh->sector))
		rv = 1;
	return rv;
}

static int need_this_block(struct stripe_head *sh, struct stripe_head_state *s,
			   int disk_idx, int disks)
{
	struct r5dev *dev = &sh->dev[disk_idx];
	struct r5dev *fdev[2] = { &sh->dev[s->failed_num[0]],
				  &sh->dev[s->failed_num[1]] };
	int i;
	bool force_rcw = (sh->raid_conf->rmw_level == PARITY_DISABLE_RMW);


	if (test_bit(R5_LOCKED, &dev->flags) ||
	    test_bit(R5_UPTODATE, &dev->flags))
		/* No point reading this as we already have it or have
		 * decided to get it.
		 */
		return 0;

	if (dev->toread ||
	    (dev->towrite && !test_bit(R5_OVERWRITE, &dev->flags)))
		/* We need this block to directly satisfy a request */
		return 1;

	if (s->syncing || s->expanding ||
	    (s->replacing && want_replace(sh, disk_idx)))
		/* When syncing, or expanding we read everything.
		 * When replacing, we need the replaced block.
		 */
		return 1;

	if ((s->failed >= 1 && fdev[0]->toread) ||
	    (s->failed >= 2 && fdev[1]->toread))
		/* If we want to read from a failed device, then
		 * we need to actually read every other device.
		 */
		return 1;

	/* Sometimes neither read-modify-write nor reconstruct-write
	 * cycles can work.  In those cases we read every block we
	 * can.  Then the parity-update is certain to have enough to
	 * work with.
	 * This can only be a problem when we need to write something,
	 * and some device has failed.  If either of those tests
	 * fail we need look no further.
	 */
	if (!s->failed || !s->to_write)
		return 0;

	if (test_bit(R5_Insync, &dev->flags) &&
	    !test_bit(STRIPE_PREREAD_ACTIVE, &sh->state))
		/* Pre-reads at not permitted until after short delay
		 * to gather multiple requests.  However if this
		 * device is no Insync, the block could only be computed
		 * and there is no need to delay that.
		 */
		return 0;

	for (i = 0; i < s->failed && i < 2; i++) {
		if (fdev[i]->towrite &&
		    !test_bit(R5_UPTODATE, &fdev[i]->flags) &&
		    !test_bit(R5_OVERWRITE, &fdev[i]->flags))
			/* If we have a partial write to a failed
			 * device, then we will need to reconstruct
			 * the content of that device, so all other
			 * devices must be read.
			 */
			return 1;

		if (s->failed >= 2 &&
		    (fdev[i]->towrite ||
		     s->failed_num[i] == sh->pd_idx ||
		     s->failed_num[i] == sh->qd_idx) &&
		    !test_bit(R5_UPTODATE, &fdev[i]->flags))
			/* In max degraded raid6, If the failed disk is P, Q,
			 * or we want to read the failed disk, we need to do
			 * reconstruct-write.
			 */
			force_rcw = true;
	}

	/* If we are forced to do a reconstruct-write, because parity
	 * cannot be trusted and we are currently recovering it, there
	 * is extra need to be careful.
	 * If one of the devices that we would need to read, because
	 * it is not being overwritten (and maybe not written at all)
	 * is missing/faulty, then we need to read everything we can.
	 */
	if (!force_rcw &&
	    sh->sector < sh->raid_conf->mddev->recovery_cp)
		/* reconstruct-write isn't being forced */
		return 0;
	for (i = 0; i < s->failed && i < 2; i++) {
		if (s->failed_num[i] != sh->pd_idx &&
		    s->failed_num[i] != sh->qd_idx &&
		    !test_bit(R5_UPTODATE, &fdev[i]->flags) &&
		    !test_bit(R5_OVERWRITE, &fdev[i]->flags))
			return 1;
	}

	return 0;
}

/* fetch_block - checks the given member device to see if its data needs
 * to be read or computed to satisfy a request.
 *
 * Returns 1 when no more member devices need to be checked, otherwise returns
 * 0 to tell the loop in handle_stripe_fill to continue
 */
static int fetch_block(struct stripe_head *sh, struct stripe_head_state *s,
		       int disk_idx, int disks)
{
	struct r5dev *dev = &sh->dev[disk_idx];

	/* is the data in this block needed, and can we get it? */
	if (need_this_block(sh, s, disk_idx, disks)) {
		/* we would like to get this block, possibly by computing it,
		 * otherwise read it if the backing disk is insync
		 */
		BUG_ON(test_bit(R5_Wantcompute, &dev->flags));
		BUG_ON(test_bit(R5_Wantread, &dev->flags));
		BUG_ON(sh->batch_head);

		/*
		 * In the raid6 case if the only non-uptodate disk is P
		 * then we already trusted P to compute the other failed
		 * drives. It is safe to compute rather than re-read P.
		 * In other cases we only compute blocks from failed
		 * devices, otherwise check/repair might fail to detect
		 * a real inconsistency.
		 */

		if ((s->uptodate == disks - 1) &&
		    ((sh->qd_idx >= 0 && sh->pd_idx == disk_idx) ||
		    (s->failed && (disk_idx == s->failed_num[0] ||
				   disk_idx == s->failed_num[1])))) {
			/* have disk failed, and we're requested to fetch it;
			 * do compute it
			 */
			pr_debug("Computing stripe %llu block %d\n",
			       (unsigned long long)sh->sector, disk_idx);
			set_bit(STRIPE_COMPUTE_RUN, &sh->state);
			set_bit(STRIPE_OP_COMPUTE_BLK, &s->ops_request);
			set_bit(R5_Wantcompute, &dev->flags);
			sh->ops.target = disk_idx;
			sh->ops.target2 = -1; /* no 2nd target */
			s->req_compute = 1;
			/* Careful: from this point on 'uptodate' is in the eye
			 * of raid_run_ops which services 'compute' operations
			 * before writes. R5_Wantcompute flags a block that will
			 * be R5_UPTODATE by the time it is needed for a
			 * subsequent operation.
			 */
			s->uptodate++;
			return 1;
		} else if (s->uptodate == disks-2 && s->failed >= 2) {
			/* Computing 2-failure is *very* expensive; only
			 * do it if failed >= 2
			 */
			int other;
			for (other = disks; other--; ) {
				if (other == disk_idx)
					continue;
				if (!test_bit(R5_UPTODATE,
				      &sh->dev[other].flags))
					break;
			}
			BUG_ON(other < 0);
			pr_debug("Computing stripe %llu blocks %d,%d\n",
			       (unsigned long long)sh->sector,
			       disk_idx, other);
			set_bit(STRIPE_COMPUTE_RUN, &sh->state);
			set_bit(STRIPE_OP_COMPUTE_BLK, &s->ops_request);
			set_bit(R5_Wantcompute, &sh->dev[disk_idx].flags);
			set_bit(R5_Wantcompute, &sh->dev[other].flags);
			sh->ops.target = disk_idx;
			sh->ops.target2 = other;
			s->uptodate += 2;
			s->req_compute = 1;
			return 1;
		} else if (test_bit(R5_Insync, &dev->flags)) {
			set_bit(R5_LOCKED, &dev->flags);
			set_bit(R5_Wantread, &dev->flags);
			s->locked++;
			pr_debug("Reading block %d (sync=%d)\n",
				disk_idx, s->syncing);
		}
	}

	return 0;
}

/*
 * handle_stripe_fill - read or compute data to satisfy pending requests.
 */
static void handle_stripe_fill(struct stripe_head *sh,
			       struct stripe_head_state *s,
			       int disks)
{
	int i;

	/* look for blocks to read/compute, skip this if a compute
	 * is already in flight, or if the stripe contents are in the
	 * midst of changing due to a write
	 */
	if (!test_bit(STRIPE_COMPUTE_RUN, &sh->state) && !sh->check_state &&
	    !sh->reconstruct_state) {

		/*
		 * For degraded stripe with data in journal, do not handle
		 * read requests yet, instead, flush the stripe to raid
		 * disks first, this avoids handling complex rmw of write
		 * back cache (prexor with orig_page, and then xor with
		 * page) in the read path
		 */
		if (s->to_read && s->injournal && s->failed) {
			if (test_bit(STRIPE_R5C_CACHING, &sh->state))
				r5c_make_stripe_write_out(sh);
			goto out;
		}

		for (i = disks; i--; )
			if (fetch_block(sh, s, i, disks))
				break;
	}
out:
	set_bit(STRIPE_HANDLE, &sh->state);
}

static void break_stripe_batch_list(struct stripe_head *head_sh,
				    unsigned long handle_flags);
/* handle_stripe_clean_event
 * any written block on an uptodate or failed drive can be returned.
 * Note that if we 'wrote' to a failed drive, it will be UPTODATE, but
 * never LOCKED, so we don't need to test 'failed' directly.
 */
static void handle_stripe_clean_event(struct r5conf *conf,
	struct stripe_head *sh, int disks)
{
	int i;
	struct r5dev *dev;
	int discard_pending = 0;
	struct stripe_head *head_sh = sh;
	bool do_endio = false;

	for (i = disks; i--; )
		if (sh->dev[i].written) {
			dev = &sh->dev[i];
			if (!test_bit(R5_LOCKED, &dev->flags) &&
			    (test_bit(R5_UPTODATE, &dev->flags) ||
			     test_bit(R5_Discard, &dev->flags) ||
			     test_bit(R5_SkipCopy, &dev->flags))) {
				/* We can return any write requests */
				struct bio *wbi, *wbi2;
				pr_debug("Return write for disc %d\n", i);
				if (test_and_clear_bit(R5_Discard, &dev->flags))
					clear_bit(R5_UPTODATE, &dev->flags);
				if (test_and_clear_bit(R5_SkipCopy, &dev->flags)) {
					WARN_ON(test_bit(R5_UPTODATE, &dev->flags));
				}
				do_endio = true;

returnbi:
				dev->page = dev->orig_page;
				wbi = dev->written;
				dev->written = NULL;
				while (wbi && wbi->bi_iter.bi_sector <
					dev->sector + RAID5_STRIPE_SECTORS(conf)) {
					wbi2 = r5_next_bio(conf, wbi, dev->sector);
					md_write_end(conf->mddev);
					bio_endio(wbi);
					wbi = wbi2;
				}

				if (head_sh->batch_head) {
					sh = list_first_entry(&sh->batch_list,
							      struct stripe_head,
							      batch_list);
					if (sh != head_sh) {
						dev = &sh->dev[i];
						goto returnbi;
					}
				}
				sh = head_sh;
				dev = &sh->dev[i];
			} else if (test_bit(R5_Discard, &dev->flags))
				discard_pending = 1;
		}

	log_stripe_write_finished(sh);

	if (!discard_pending &&
	    test_bit(R5_Discard, &sh->dev[sh->pd_idx].flags)) {
		int hash;
		clear_bit(R5_Discard, &sh->dev[sh->pd_idx].flags);
		clear_bit(R5_UPTODATE, &sh->dev[sh->pd_idx].flags);
		if (sh->qd_idx >= 0) {
			clear_bit(R5_Discard, &sh->dev[sh->qd_idx].flags);
			clear_bit(R5_UPTODATE, &sh->dev[sh->qd_idx].flags);
		}
		/* now that discard is done we can proceed with any sync */
		clear_bit(STRIPE_DISCARD, &sh->state);
		/*
		 * SCSI discard will change some bio fields and the stripe has
		 * no updated data, so remove it from hash list and the stripe
		 * will be reinitialized
		 */
unhash:
		hash = sh->hash_lock_index;
		spin_lock_irq(conf->hash_locks + hash);
		remove_hash(sh);
		spin_unlock_irq(conf->hash_locks + hash);
		if (head_sh->batch_head) {
			sh = list_first_entry(&sh->batch_list,
					      struct stripe_head, batch_list);
			if (sh != head_sh)
					goto unhash;
		}
		sh = head_sh;

		if (test_bit(STRIPE_SYNC_REQUESTED, &sh->state))
			set_bit(STRIPE_HANDLE, &sh->state);

	}

	if (test_and_clear_bit(STRIPE_FULL_WRITE, &sh->state))
		if (atomic_dec_and_test(&conf->pending_full_writes))
			md_wakeup_thread(conf->mddev->thread);

	if (head_sh->batch_head && do_endio)
		break_stripe_batch_list(head_sh, STRIPE_EXPAND_SYNC_FLAGS);
}

/*
 * For RMW in write back cache, we need extra page in prexor to store the
 * old data. This page is stored in dev->orig_page.
 *
 * This function checks whether we have data for prexor. The exact logic
 * is:
 *       R5_UPTODATE && (!R5_InJournal || R5_OrigPageUPTDODATE)
 */
static inline bool uptodate_for_rmw(struct r5dev *dev)
{
	return (test_bit(R5_UPTODATE, &dev->flags)) &&
		(!test_bit(R5_InJournal, &dev->flags) ||
		 test_bit(R5_OrigPageUPTDODATE, &dev->flags));
}

static int handle_stripe_dirtying(struct r5conf *conf,
				  struct stripe_head *sh,
				  struct stripe_head_state *s,
				  int disks)
{
	int rmw = 0, rcw = 0, i;
	sector_t recovery_cp = conf->mddev->recovery_cp;

	/* Check whether resync is now happening or should start.
	 * If yes, then the array is dirty (after unclean shutdown or
	 * initial creation), so parity in some stripes might be inconsistent.
	 * In this case, we need to always do reconstruct-write, to ensure
	 * that in case of drive failure or read-error correction, we
	 * generate correct data from the parity.
	 */
	if (conf->rmw_level == PARITY_DISABLE_RMW ||
	    (recovery_cp < MaxSector && sh->sector >= recovery_cp &&
	     s->failed == 0)) {
		/* Calculate the real rcw later - for now make it
		 * look like rcw is cheaper
		 */
		rcw = 1; rmw = 2;
		pr_debug("force RCW rmw_level=%u, recovery_cp=%llu sh->sector=%llu\n",
			 conf->rmw_level, (unsigned long long)recovery_cp,
			 (unsigned long long)sh->sector);
	} else for (i = disks; i--; ) {
		/* would I have to read this buffer for read_modify_write */
		struct r5dev *dev = &sh->dev[i];
		if (((dev->towrite && !delay_towrite(conf, dev, s)) ||
		     i == sh->pd_idx || i == sh->qd_idx ||
		     test_bit(R5_InJournal, &dev->flags)) &&
		    !test_bit(R5_LOCKED, &dev->flags) &&
		    !(uptodate_for_rmw(dev) ||
		      test_bit(R5_Wantcompute, &dev->flags))) {
			if (test_bit(R5_Insync, &dev->flags))
				rmw++;
			else
				rmw += 2*disks;  /* cannot read it */
		}
		/* Would I have to read this buffer for reconstruct_write */
		if (!test_bit(R5_OVERWRITE, &dev->flags) &&
		    i != sh->pd_idx && i != sh->qd_idx &&
		    !test_bit(R5_LOCKED, &dev->flags) &&
		    !(test_bit(R5_UPTODATE, &dev->flags) ||
		      test_bit(R5_Wantcompute, &dev->flags))) {
			if (test_bit(R5_Insync, &dev->flags))
				rcw++;
			else
				rcw += 2*disks;
		}
	}

	pr_debug("for sector %llu state 0x%lx, rmw=%d rcw=%d\n",
		 (unsigned long long)sh->sector, sh->state, rmw, rcw);
	set_bit(STRIPE_HANDLE, &sh->state);
	if ((rmw < rcw || (rmw == rcw && conf->rmw_level == PARITY_PREFER_RMW)) && rmw > 0) {
		/* prefer read-modify-write, but need to get some data */
		mddev_add_trace_msg(conf->mddev, "raid5 rmw %llu %d",
				sh->sector, rmw);

		for (i = disks; i--; ) {
			struct r5dev *dev = &sh->dev[i];
			if (test_bit(R5_InJournal, &dev->flags) &&
			    dev->page == dev->orig_page &&
			    !test_bit(R5_LOCKED, &sh->dev[sh->pd_idx].flags)) {
				/* alloc page for prexor */
				struct page *p = alloc_page(GFP_NOIO);

				if (p) {
					dev->orig_page = p;
					continue;
				}

				/*
				 * alloc_page() failed, try use
				 * disk_info->extra_page
				 */
				if (!test_and_set_bit(R5C_EXTRA_PAGE_IN_USE,
						      &conf->cache_state)) {
					r5c_use_extra_page(sh);
					break;
				}

				/* extra_page in use, add to delayed_list */
				set_bit(STRIPE_DELAYED, &sh->state);
				s->waiting_extra_page = 1;
				return -EAGAIN;
			}
		}

		for (i = disks; i--; ) {
			struct r5dev *dev = &sh->dev[i];
			if (((dev->towrite && !delay_towrite(conf, dev, s)) ||
			     i == sh->pd_idx || i == sh->qd_idx ||
			     test_bit(R5_InJournal, &dev->flags)) &&
			    !test_bit(R5_LOCKED, &dev->flags) &&
			    !(uptodate_for_rmw(dev) ||
			      test_bit(R5_Wantcompute, &dev->flags)) &&
			    test_bit(R5_Insync, &dev->flags)) {
				if (test_bit(STRIPE_PREREAD_ACTIVE,
					     &sh->state)) {
					pr_debug("Read_old block %d for r-m-w\n",
						 i);
					set_bit(R5_LOCKED, &dev->flags);
					set_bit(R5_Wantread, &dev->flags);
					s->locked++;
				} else
					set_bit(STRIPE_DELAYED, &sh->state);
			}
		}
	}
	if ((rcw < rmw || (rcw == rmw && conf->rmw_level != PARITY_PREFER_RMW)) && rcw > 0) {
		/* want reconstruct write, but need to get some data */
		int qread =0;
		rcw = 0;
		for (i = disks; i--; ) {
			struct r5dev *dev = &sh->dev[i];
			if (!test_bit(R5_OVERWRITE, &dev->flags) &&
			    i != sh->pd_idx && i != sh->qd_idx &&
			    !test_bit(R5_LOCKED, &dev->flags) &&
			    !(test_bit(R5_UPTODATE, &dev->flags) ||
			      test_bit(R5_Wantcompute, &dev->flags))) {
				rcw++;
				if (test_bit(R5_Insync, &dev->flags) &&
				    test_bit(STRIPE_PREREAD_ACTIVE,
					     &sh->state)) {
					pr_debug("Read_old block "
						"%d for Reconstruct\n", i);
					set_bit(R5_LOCKED, &dev->flags);
					set_bit(R5_Wantread, &dev->flags);
					s->locked++;
					qread++;
				} else
					set_bit(STRIPE_DELAYED, &sh->state);
			}
		}
		if (rcw && !mddev_is_dm(conf->mddev))
			blk_add_trace_msg(conf->mddev->gendisk->queue,
				"raid5 rcw %llu %d %d %d",
				(unsigned long long)sh->sector, rcw, qread,
				test_bit(STRIPE_DELAYED, &sh->state));
	}

	if (rcw > disks && rmw > disks &&
	    !test_bit(STRIPE_PREREAD_ACTIVE, &sh->state))
		set_bit(STRIPE_DELAYED, &sh->state);

	/* now if nothing is locked, and if we have enough data,
	 * we can start a write request
	 */
	/* since handle_stripe can be called at any time we need to handle the
	 * case where a compute block operation has been submitted and then a
	 * subsequent call wants to start a write request.  raid_run_ops only
	 * handles the case where compute block and reconstruct are requested
	 * simultaneously.  If this is not the case then new writes need to be
	 * held off until the compute completes.
	 */
	if ((s->req_compute || !test_bit(STRIPE_COMPUTE_RUN, &sh->state)) &&
	    (s->locked == 0 && (rcw == 0 || rmw == 0) &&
	     !test_bit(STRIPE_BIT_DELAY, &sh->state)))
		schedule_reconstruction(sh, s, rcw == 0, 0);
	return 0;
}

static void handle_parity_checks5(struct r5conf *conf, struct stripe_head *sh,
				struct stripe_head_state *s, int disks)
{
	struct r5dev *dev = NULL;

	BUG_ON(sh->batch_head);
	set_bit(STRIPE_HANDLE, &sh->state);

	switch (sh->check_state) {
	case check_state_idle:
		/* start a new check operation if there are no failures */
		if (s->failed == 0) {
			BUG_ON(s->uptodate != disks);
			sh->check_state = check_state_run;
			set_bit(STRIPE_OP_CHECK, &s->ops_request);
			clear_bit(R5_UPTODATE, &sh->dev[sh->pd_idx].flags);
			s->uptodate--;
			break;
		}
		dev = &sh->dev[s->failed_num[0]];
		fallthrough;
	case check_state_compute_result:
		sh->check_state = check_state_idle;
		if (!dev)
			dev = &sh->dev[sh->pd_idx];

		/* check that a write has not made the stripe insync */
		if (test_bit(STRIPE_INSYNC, &sh->state))
			break;

		/* either failed parity check, or recovery is happening */
		BUG_ON(!test_bit(R5_UPTODATE, &dev->flags));
		BUG_ON(s->uptodate != disks);

		set_bit(R5_LOCKED, &dev->flags);
		s->locked++;
		set_bit(R5_Wantwrite, &dev->flags);

		set_bit(STRIPE_INSYNC, &sh->state);
		break;
	case check_state_run:
		break; /* we will be called again upon completion */
	case check_state_check_result:
		sh->check_state = check_state_idle;

		/* if a failure occurred during the check operation, leave
		 * STRIPE_INSYNC not set and let the stripe be handled again
		 */
		if (s->failed)
			break;

		/* handle a successful check operation, if parity is correct
		 * we are done.  Otherwise update the mismatch count and repair
		 * parity if !MD_RECOVERY_CHECK
		 */
		if ((sh->ops.zero_sum_result & SUM_CHECK_P_RESULT) == 0)
			/* parity is correct (on disc,
			 * not in buffer any more)
			 */
			set_bit(STRIPE_INSYNC, &sh->state);
		else {
			atomic64_add(RAID5_STRIPE_SECTORS(conf), &conf->mddev->resync_mismatches);
			if (test_bit(MD_RECOVERY_CHECK, &conf->mddev->recovery)) {
				/* don't try to repair!! */
				set_bit(STRIPE_INSYNC, &sh->state);
				pr_warn_ratelimited("%s: mismatch sector in range "
						    "%llu-%llu\n", mdname(conf->mddev),
						    (unsigned long long) sh->sector,
						    (unsigned long long) sh->sector +
						    RAID5_STRIPE_SECTORS(conf));
			} else {
				sh->check_state = check_state_compute_run;
				set_bit(STRIPE_COMPUTE_RUN, &sh->state);
				set_bit(STRIPE_OP_COMPUTE_BLK, &s->ops_request);
				set_bit(R5_Wantcompute,
					&sh->dev[sh->pd_idx].flags);
				sh->ops.target = sh->pd_idx;
				sh->ops.target2 = -1;
				s->uptodate++;
			}
		}
		break;
	case check_state_compute_run:
		break;
	default:
		pr_err("%s: unknown check_state: %d sector: %llu\n",
		       __func__, sh->check_state,
		       (unsigned long long) sh->sector);
		BUG();
	}
}

static void handle_parity_checks6(struct r5conf *conf, struct stripe_head *sh,
				  struct stripe_head_state *s,
				  int disks)
{
	int pd_idx = sh->pd_idx;
	int qd_idx = sh->qd_idx;
	struct r5dev *dev;

	BUG_ON(sh->batch_head);
	set_bit(STRIPE_HANDLE, &sh->state);

	BUG_ON(s->failed > 2);

	/* Want to check and possibly repair P and Q.
	 * However there could be one 'failed' device, in which
	 * case we can only check one of them, possibly using the
	 * other to generate missing data
	 */

	switch (sh->check_state) {
	case check_state_idle:
		/* start a new check operation if there are < 2 failures */
		if (s->failed == s->q_failed) {
			/* The only possible failed device holds Q, so it
			 * makes sense to check P (If anything else were failed,
			 * we would have used P to recreate it).
			 */
			sh->check_state = check_state_run;
		}
		if (!s->q_failed && s->failed < 2) {
			/* Q is not failed, and we didn't use it to generate
			 * anything, so it makes sense to check it
			 */
			if (sh->check_state == check_state_run)
				sh->check_state = check_state_run_pq;
			else
				sh->check_state = check_state_run_q;
		}

		/* discard potentially stale zero_sum_result */
		sh->ops.zero_sum_result = 0;

		if (sh->check_state == check_state_run) {
			/* async_xor_zero_sum destroys the contents of P */
			clear_bit(R5_UPTODATE, &sh->dev[pd_idx].flags);
			s->uptodate--;
		}
		if (sh->check_state >= check_state_run &&
		    sh->check_state <= check_state_run_pq) {
			/* async_syndrome_zero_sum preserves P and Q, so
			 * no need to mark them !uptodate here
			 */
			set_bit(STRIPE_OP_CHECK, &s->ops_request);
			break;
		}

		/* we have 2-disk failure */
		BUG_ON(s->failed != 2);
		fallthrough;
	case check_state_compute_result:
		sh->check_state = check_state_idle;

		/* check that a write has not made the stripe insync */
		if (test_bit(STRIPE_INSYNC, &sh->state))
			break;

		/* now write out any block on a failed drive,
		 * or P or Q if they were recomputed
		 */
		dev = NULL;
		if (s->failed == 2) {
			dev = &sh->dev[s->failed_num[1]];
			s->locked++;
			set_bit(R5_LOCKED, &dev->flags);
			set_bit(R5_Wantwrite, &dev->flags);
		}
		if (s->failed >= 1) {
			dev = &sh->dev[s->failed_num[0]];
			s->locked++;
			set_bit(R5_LOCKED, &dev->flags);
			set_bit(R5_Wantwrite, &dev->flags);
		}
		if (sh->ops.zero_sum_result & SUM_CHECK_P_RESULT) {
			dev = &sh->dev[pd_idx];
			s->locked++;
			set_bit(R5_LOCKED, &dev->flags);
			set_bit(R5_Wantwrite, &dev->flags);
		}
		if (sh->ops.zero_sum_result & SUM_CHECK_Q_RESULT) {
			dev = &sh->dev[qd_idx];
			s->locked++;
			set_bit(R5_LOCKED, &dev->flags);
			set_bit(R5_Wantwrite, &dev->flags);
		}
		if (WARN_ONCE(dev && !test_bit(R5_UPTODATE, &dev->flags),
			      "%s: disk%td not up to date\n",
			      mdname(conf->mddev),
			      dev - (struct r5dev *) &sh->dev)) {
			clear_bit(R5_LOCKED, &dev->flags);
			clear_bit(R5_Wantwrite, &dev->flags);
			s->locked--;
		}

		set_bit(STRIPE_INSYNC, &sh->state);
		break;
	case check_state_run:
	case check_state_run_q:
	case check_state_run_pq:
		break; /* we will be called again upon completion */
	case check_state_check_result:
		sh->check_state = check_state_idle;

		/* handle a successful check operation, if parity is correct
		 * we are done.  Otherwise update the mismatch count and repair
		 * parity if !MD_RECOVERY_CHECK
		 */
		if (sh->ops.zero_sum_result == 0) {
			/* both parities are correct */
			if (!s->failed)
				set_bit(STRIPE_INSYNC, &sh->state);
			else {
				/* in contrast to the raid5 case we can validate
				 * parity, but still have a failure to write
				 * back
				 */
				sh->check_state = check_state_compute_result;
				/* Returning at this point means that we may go
				 * off and bring p and/or q uptodate again so
				 * we make sure to check zero_sum_result again
				 * to verify if p or q need writeback
				 */
			}
		} else {
			atomic64_add(RAID5_STRIPE_SECTORS(conf), &conf->mddev->resync_mismatches);
			if (test_bit(MD_RECOVERY_CHECK, &conf->mddev->recovery)) {
				/* don't try to repair!! */
				set_bit(STRIPE_INSYNC, &sh->state);
				pr_warn_ratelimited("%s: mismatch sector in range "
						    "%llu-%llu\n", mdname(conf->mddev),
						    (unsigned long long) sh->sector,
						    (unsigned long long) sh->sector +
						    RAID5_STRIPE_SECTORS(conf));
			} else {
				int *target = &sh->ops.target;

				sh->ops.target = -1;
				sh->ops.target2 = -1;
				sh->check_state = check_state_compute_run;
				set_bit(STRIPE_COMPUTE_RUN, &sh->state);
				set_bit(STRIPE_OP_COMPUTE_BLK, &s->ops_request);
				if (sh->ops.zero_sum_result & SUM_CHECK_P_RESULT) {
					set_bit(R5_Wantcompute,
						&sh->dev[pd_idx].flags);
					*target = pd_idx;
					target = &sh->ops.target2;
					s->uptodate++;
				}
				if (sh->ops.zero_sum_result & SUM_CHECK_Q_RESULT) {
					set_bit(R5_Wantcompute,
						&sh->dev[qd_idx].flags);
					*target = qd_idx;
					s->uptodate++;
				}
			}
		}
		break;
	case check_state_compute_run:
		break;
	default:
		pr_warn("%s: unknown check_state: %d sector: %llu\n",
			__func__, sh->check_state,
			(unsigned long long) sh->sector);
		BUG();
	}
}

static void handle_stripe_expansion(struct r5conf *conf, struct stripe_head *sh)
{
	int i;

	/* We have read all the blocks in this stripe and now we need to
	 * copy some of them into a target stripe for expand.
	 */
	struct dma_async_tx_descriptor *tx = NULL;
	BUG_ON(sh->batch_head);
	clear_bit(STRIPE_EXPAND_SOURCE, &sh->state);
	for (i = 0; i < sh->disks; i++)
		if (i != sh->pd_idx && i != sh->qd_idx) {
			int dd_idx, j;
			struct stripe_head *sh2;
			struct async_submit_ctl submit;

			sector_t bn = raid5_compute_blocknr(sh, i, 1);
			sector_t s = raid5_compute_sector(conf, bn, 0,
							  &dd_idx, NULL);
			sh2 = raid5_get_active_stripe(conf, NULL, s,
				R5_GAS_NOBLOCK | R5_GAS_NOQUIESCE);
			if (sh2 == NULL)
				/* so far only the early blocks of this stripe
				 * have been requested.  When later blocks
				 * get requested, we will try again
				 */
				continue;
			if (!test_bit(STRIPE_EXPANDING, &sh2->state) ||
			   test_bit(R5_Expanded, &sh2->dev[dd_idx].flags)) {
				/* must have already done this block */
				raid5_release_stripe(sh2);
				continue;
			}

			/* place all the copies on one channel */
			init_async_submit(&submit, 0, tx, NULL, NULL, NULL);
			tx = async_memcpy(sh2->dev[dd_idx].page,
					  sh->dev[i].page, sh2->dev[dd_idx].offset,
					  sh->dev[i].offset, RAID5_STRIPE_SIZE(conf),
					  &submit);

			set_bit(R5_Expanded, &sh2->dev[dd_idx].flags);
			set_bit(R5_UPTODATE, &sh2->dev[dd_idx].flags);
			for (j = 0; j < conf->raid_disks; j++)
				if (j != sh2->pd_idx &&
				    j != sh2->qd_idx &&
				    !test_bit(R5_Expanded, &sh2->dev[j].flags))
					break;
			if (j == conf->raid_disks) {
				set_bit(STRIPE_EXPAND_READY, &sh2->state);
				set_bit(STRIPE_HANDLE, &sh2->state);
			}
			raid5_release_stripe(sh2);

		}
	/* done submitting copies, wait for them to complete */
	async_tx_quiesce(&tx);
}

/*
 * handle_stripe - do things to a stripe.
 *
 * We lock the stripe by setting STRIPE_ACTIVE and then examine the
 * state of various bits to see what needs to be done.
 * Possible results:
 *    return some read requests which now have data
 *    return some write requests which are safely on storage
 *    schedule a read on some buffers
 *    schedule a write of some buffers
 *    return confirmation of parity correctness
 *
 */

static void analyse_stripe(struct stripe_head *sh, struct stripe_head_state *s)
{
	struct r5conf *conf = sh->raid_conf;
	int disks = sh->disks;
	struct r5dev *dev;
	int i;
	int do_recovery = 0;

	memset(s, 0, sizeof(*s));

	s->expanding = test_bit(STRIPE_EXPAND_SOURCE, &sh->state) && !sh->batch_head;
	s->expanded = test_bit(STRIPE_EXPAND_READY, &sh->state) && !sh->batch_head;
	s->failed_num[0] = -1;
	s->failed_num[1] = -1;
	s->log_failed = r5l_log_disk_error(conf);

	/* Now to look around and see what can be done */
	for (i=disks; i--; ) {
		struct md_rdev *rdev;
		int is_bad = 0;

		dev = &sh->dev[i];

		pr_debug("check %d: state 0x%lx read %p write %p written %p\n",
			 i, dev->flags,
			 dev->toread, dev->towrite, dev->written);
		/* maybe we can reply to a read
		 *
		 * new wantfill requests are only permitted while
		 * ops_complete_biofill is guaranteed to be inactive
		 */
		if (test_bit(R5_UPTODATE, &dev->flags) && dev->toread &&
		    !test_bit(STRIPE_BIOFILL_RUN, &sh->state))
			set_bit(R5_Wantfill, &dev->flags);

		/* now count some things */
		if (test_bit(R5_LOCKED, &dev->flags))
			s->locked++;
		if (test_bit(R5_UPTODATE, &dev->flags))
			s->uptodate++;
		if (test_bit(R5_Wantcompute, &dev->flags)) {
			s->compute++;
			BUG_ON(s->compute > 2);
		}

		if (test_bit(R5_Wantfill, &dev->flags))
			s->to_fill++;
		else if (dev->toread)
			s->to_read++;
		if (dev->towrite) {
			s->to_write++;
			if (!test_bit(R5_OVERWRITE, &dev->flags))
				s->non_overwrite++;
		}
		if (dev->written)
			s->written++;
		/* Prefer to use the replacement for reads, but only
		 * if it is recovered enough and has no bad blocks.
		 */
		rdev = conf->disks[i].replacement;
		if (rdev && !test_bit(Faulty, &rdev->flags) &&
		    rdev->recovery_offset >= sh->sector + RAID5_STRIPE_SECTORS(conf) &&
		    !rdev_has_badblock(rdev, sh->sector,
				       RAID5_STRIPE_SECTORS(conf)))
			set_bit(R5_ReadRepl, &dev->flags);
		else {
			if (rdev && !test_bit(Faulty, &rdev->flags))
				set_bit(R5_NeedReplace, &dev->flags);
			else
				clear_bit(R5_NeedReplace, &dev->flags);
			rdev = conf->disks[i].rdev;
			clear_bit(R5_ReadRepl, &dev->flags);
		}
		if (rdev && test_bit(Faulty, &rdev->flags))
			rdev = NULL;
		if (rdev) {
			is_bad = rdev_has_badblock(rdev, sh->sector,
						   RAID5_STRIPE_SECTORS(conf));
			if (s->blocked_rdev == NULL) {
				if (is_bad < 0)
					set_bit(BlockedBadBlocks, &rdev->flags);
				if (rdev_blocked(rdev)) {
					s->blocked_rdev = rdev;
					atomic_inc(&rdev->nr_pending);
				}
			}
		}
		clear_bit(R5_Insync, &dev->flags);
		if (!rdev)
			/* Not in-sync */;
		else if (is_bad) {
			/* also not in-sync */
			if (!test_bit(WriteErrorSeen, &rdev->flags) &&
			    test_bit(R5_UPTODATE, &dev->flags)) {
				/* treat as in-sync, but with a read error
				 * which we can now try to correct
				 */
				set_bit(R5_Insync, &dev->flags);
				set_bit(R5_ReadError, &dev->flags);
			}
		} else if (test_bit(In_sync, &rdev->flags))
			set_bit(R5_Insync, &dev->flags);
		else if (sh->sector + RAID5_STRIPE_SECTORS(conf) <= rdev->recovery_offset)
			/* in sync if before recovery_offset */
			set_bit(R5_Insync, &dev->flags);
		else if (test_bit(R5_UPTODATE, &dev->flags) &&
			 test_bit(R5_Expanded, &dev->flags))
			/* If we've reshaped into here, we assume it is Insync.
			 * We will shortly update recovery_offset to make
			 * it official.
			 */
			set_bit(R5_Insync, &dev->flags);

		if (test_bit(R5_WriteError, &dev->flags)) {
			/* This flag does not apply to '.replacement'
			 * only to .rdev, so make sure to check that*/
			struct md_rdev *rdev2 = conf->disks[i].rdev;

			if (rdev2 == rdev)
				clear_bit(R5_Insync, &dev->flags);
			if (rdev2 && !test_bit(Faulty, &rdev2->flags)) {
				s->handle_bad_blocks = 1;
				atomic_inc(&rdev2->nr_pending);
			} else
				clear_bit(R5_WriteError, &dev->flags);
		}
		if (test_bit(R5_MadeGood, &dev->flags)) {
			/* This flag does not apply to '.replacement'
			 * only to .rdev, so make sure to check that*/
			struct md_rdev *rdev2 = conf->disks[i].rdev;

			if (rdev2 && !test_bit(Faulty, &rdev2->flags)) {
				s->handle_bad_blocks = 1;
				atomic_inc(&rdev2->nr_pending);
			} else
				clear_bit(R5_MadeGood, &dev->flags);
		}
		if (test_bit(R5_MadeGoodRepl, &dev->flags)) {
			struct md_rdev *rdev2 = conf->disks[i].replacement;

			if (rdev2 && !test_bit(Faulty, &rdev2->flags)) {
				s->handle_bad_blocks = 1;
				atomic_inc(&rdev2->nr_pending);
			} else
				clear_bit(R5_MadeGoodRepl, &dev->flags);
		}
		if (!test_bit(R5_Insync, &dev->flags)) {
			/* The ReadError flag will just be confusing now */
			clear_bit(R5_ReadError, &dev->flags);
			clear_bit(R5_ReWrite, &dev->flags);
		}
		if (test_bit(R5_ReadError, &dev->flags))
			clear_bit(R5_Insync, &dev->flags);
		if (!test_bit(R5_Insync, &dev->flags)) {
			if (s->failed < 2)
				s->failed_num[s->failed] = i;
			s->failed++;
			if (rdev && !test_bit(Faulty, &rdev->flags))
				do_recovery = 1;
			else if (!rdev) {
				rdev = conf->disks[i].replacement;
				if (rdev && !test_bit(Faulty, &rdev->flags))
					do_recovery = 1;
			}
		}

		if (test_bit(R5_InJournal, &dev->flags))
			s->injournal++;
		if (test_bit(R5_InJournal, &dev->flags) && dev->written)
			s->just_cached++;
	}
	if (test_bit(STRIPE_SYNCING, &sh->state)) {
		/* If there is a failed device being replaced,
		 *     we must be recovering.
		 * else if we are after recovery_cp, we must be syncing
		 * else if MD_RECOVERY_REQUESTED is set, we also are syncing.
		 * else we can only be replacing
		 * sync and recovery both need to read all devices, and so
		 * use the same flag.
		 */
		if (do_recovery ||
		    sh->sector >= conf->mddev->recovery_cp ||
		    test_bit(MD_RECOVERY_REQUESTED, &(conf->mddev->recovery)))
			s->syncing = 1;
		else
			s->replacing = 1;
	}
}

/*
 * Return '1' if this is a member of batch, or '0' if it is a lone stripe or
 * a head which can now be handled.
 */
static int clear_batch_ready(struct stripe_head *sh)
{
	struct stripe_head *tmp;
	if (!test_and_clear_bit(STRIPE_BATCH_READY, &sh->state))
		return (sh->batch_head && sh->batch_head != sh);
	spin_lock(&sh->stripe_lock);
	if (!sh->batch_head) {
		spin_unlock(&sh->stripe_lock);
		return 0;
	}

	/*
	 * this stripe could be added to a batch list before we check
	 * BATCH_READY, skips it
	 */
	if (sh->batch_head != sh) {
		spin_unlock(&sh->stripe_lock);
		return 1;
	}
	spin_lock(&sh->batch_lock);
	list_for_each_entry(tmp, &sh->batch_list, batch_list)
		clear_bit(STRIPE_BATCH_READY, &tmp->state);
	spin_unlock(&sh->batch_lock);
	spin_unlock(&sh->stripe_lock);

	/*
	 * BATCH_READY is cleared, no new stripes can be added.
	 * batch_list can be accessed without lock
	 */
	return 0;
}

static void break_stripe_batch_list(struct stripe_head *head_sh,
				    unsigned long handle_flags)
{
	struct stripe_head *sh, *next;
	int i;

	list_for_each_entry_safe(sh, next, &head_sh->batch_list, batch_list) {

		list_del_init(&sh->batch_list);

		WARN_ONCE(sh->state & ((1 << STRIPE_ACTIVE) |
					  (1 << STRIPE_SYNCING) |
					  (1 << STRIPE_REPLACED) |
					  (1 << STRIPE_DELAYED) |
					  (1 << STRIPE_BIT_DELAY) |
					  (1 << STRIPE_FULL_WRITE) |
					  (1 << STRIPE_BIOFILL_RUN) |
					  (1 << STRIPE_COMPUTE_RUN)  |
					  (1 << STRIPE_DISCARD) |
					  (1 << STRIPE_BATCH_READY) |
					  (1 << STRIPE_BATCH_ERR)),
			"stripe state: %lx\n", sh->state);
		WARN_ONCE(head_sh->state & ((1 << STRIPE_DISCARD) |
					      (1 << STRIPE_REPLACED)),
			"head stripe state: %lx\n", head_sh->state);

		set_mask_bits(&sh->state, ~(STRIPE_EXPAND_SYNC_FLAGS |
					    (1 << STRIPE_PREREAD_ACTIVE) |
					    (1 << STRIPE_ON_UNPLUG_LIST)),
			      head_sh->state & (1 << STRIPE_INSYNC));

		sh->check_state = head_sh->check_state;
		sh->reconstruct_state = head_sh->reconstruct_state;
		spin_lock_irq(&sh->stripe_lock);
		sh->batch_head = NULL;
		spin_unlock_irq(&sh->stripe_lock);
		for (i = 0; i < sh->disks; i++) {
			if (test_and_clear_bit(R5_Overlap, &sh->dev[i].flags))
				wake_up_bit(&sh->dev[i].flags, R5_Overlap);
			sh->dev[i].flags = head_sh->dev[i].flags &
				(~((1 << R5_WriteError) | (1 << R5_Overlap)));
		}
		if (handle_flags == 0 ||
		    sh->state & handle_flags)
			set_bit(STRIPE_HANDLE, &sh->state);
		raid5_release_stripe(sh);
	}
	spin_lock_irq(&head_sh->stripe_lock);
	head_sh->batch_head = NULL;
	spin_unlock_irq(&head_sh->stripe_lock);
	for (i = 0; i < head_sh->disks; i++)
		if (test_and_clear_bit(R5_Overlap, &head_sh->dev[i].flags))
			wake_up_bit(&head_sh->dev[i].flags, R5_Overlap);
	if (head_sh->state & handle_flags)
		set_bit(STRIPE_HANDLE, &head_sh->state);
}

static void handle_stripe(struct stripe_head *sh)
{
	struct stripe_head_state s;
	struct r5conf *conf = sh->raid_conf;
	int i;
	int prexor;
	int disks = sh->disks;
	struct r5dev *pdev, *qdev;

	clear_bit(STRIPE_HANDLE, &sh->state);

	/*
	 * handle_stripe should not continue handle the batched stripe, only
	 * the head of batch list or lone stripe can continue. Otherwise we
	 * could see break_stripe_batch_list warns about the STRIPE_ACTIVE
	 * is set for the batched stripe.
	 */
	if (clear_batch_ready(sh))
		return;

	if (test_and_set_bit_lock(STRIPE_ACTIVE, &sh->state)) {
		/* already being handled, ensure it gets handled
		 * again when current action finishes */
		set_bit(STRIPE_HANDLE, &sh->state);
		return;
	}

	if (test_and_clear_bit(STRIPE_BATCH_ERR, &sh->state))
		break_stripe_batch_list(sh, 0);

	if (test_bit(STRIPE_SYNC_REQUESTED, &sh->state) && !sh->batch_head) {
		spin_lock(&sh->stripe_lock);
		/*
		 * Cannot process 'sync' concurrently with 'discard'.
		 * Flush data in r5cache before 'sync'.
		 */
		if (!test_bit(STRIPE_R5C_PARTIAL_STRIPE, &sh->state) &&
		    !test_bit(STRIPE_R5C_FULL_STRIPE, &sh->state) &&
		    !test_bit(STRIPE_DISCARD, &sh->state) &&
		    test_and_clear_bit(STRIPE_SYNC_REQUESTED, &sh->state)) {
			set_bit(STRIPE_SYNCING, &sh->state);
			clear_bit(STRIPE_INSYNC, &sh->state);
			clear_bit(STRIPE_REPLACED, &sh->state);
		}
		spin_unlock(&sh->stripe_lock);
	}
	clear_bit(STRIPE_DELAYED, &sh->state);

	pr_debug("handling stripe %llu, state=%#lx cnt=%d, "
		"pd_idx=%d, qd_idx=%d\n, check:%d, reconstruct:%d\n",
	       (unsigned long long)sh->sector, sh->state,
	       atomic_read(&sh->count), sh->pd_idx, sh->qd_idx,
	       sh->check_state, sh->reconstruct_state);

	analyse_stripe(sh, &s);

	if (test_bit(STRIPE_LOG_TRAPPED, &sh->state))
		goto finish;

	if (s.handle_bad_blocks ||
	    test_bit(MD_SB_CHANGE_PENDING, &conf->mddev->sb_flags)) {
		set_bit(STRIPE_HANDLE, &sh->state);
		goto finish;
	}

	if (unlikely(s.blocked_rdev)) {
		if (s.syncing || s.expanding || s.expanded ||
		    s.replacing || s.to_write || s.written) {
			set_bit(STRIPE_HANDLE, &sh->state);
			goto finish;
		}
		/* There is nothing for the blocked_rdev to block */
		rdev_dec_pending(s.blocked_rdev, conf->mddev);
		s.blocked_rdev = NULL;
	}

	if (s.to_fill && !test_bit(STRIPE_BIOFILL_RUN, &sh->state)) {
		set_bit(STRIPE_OP_BIOFILL, &s.ops_request);
		set_bit(STRIPE_BIOFILL_RUN, &sh->state);
	}

	pr_debug("locked=%d uptodate=%d to_read=%d"
	       " to_write=%d failed=%d failed_num=%d,%d\n",
	       s.locked, s.uptodate, s.to_read, s.to_write, s.failed,
	       s.failed_num[0], s.failed_num[1]);
	/*
	 * check if the array has lost more than max_degraded devices and,
	 * if so, some requests might need to be failed.
	 *
	 * When journal device failed (log_failed), we will only process
	 * the stripe if there is data need write to raid disks
	 */
	if (s.failed > conf->max_degraded ||
	    (s.log_failed && s.injournal == 0)) {
		sh->check_state = 0;
		sh->reconstruct_state = 0;
		break_stripe_batch_list(sh, 0);
		if (s.to_read+s.to_write+s.written)
			handle_failed_stripe(conf, sh, &s, disks);
		if (s.syncing + s.replacing)
			handle_failed_sync(conf, sh, &s);
	}

	/* Now we check to see if any write operations have recently
	 * completed
	 */
	prexor = 0;
	if (sh->reconstruct_state == reconstruct_state_prexor_drain_result)
		prexor = 1;
	if (sh->reconstruct_state == reconstruct_state_drain_result ||
	    sh->reconstruct_state == reconstruct_state_prexor_drain_result) {
		sh->reconstruct_state = reconstruct_state_idle;

		/* All the 'written' buffers and the parity block are ready to
		 * be written back to disk
		 */
		BUG_ON(!test_bit(R5_UPTODATE, &sh->dev[sh->pd_idx].flags) &&
		       !test_bit(R5_Discard, &sh->dev[sh->pd_idx].flags));
		BUG_ON(sh->qd_idx >= 0 &&
		       !test_bit(R5_UPTODATE, &sh->dev[sh->qd_idx].flags) &&
		       !test_bit(R5_Discard, &sh->dev[sh->qd_idx].flags));
		for (i = disks; i--; ) {
			struct r5dev *dev = &sh->dev[i];
			if (test_bit(R5_LOCKED, &dev->flags) &&
				(i == sh->pd_idx || i == sh->qd_idx ||
				 dev->written || test_bit(R5_InJournal,
							  &dev->flags))) {
				pr_debug("Writing block %d\n", i);
				set_bit(R5_Wantwrite, &dev->flags);
				if (prexor)
					continue;
				if (s.failed > 1)
					continue;
				if (!test_bit(R5_Insync, &dev->flags) ||
				    ((i == sh->pd_idx || i == sh->qd_idx)  &&
				     s.failed == 0))
					set_bit(STRIPE_INSYNC, &sh->state);
			}
		}
		if (test_and_clear_bit(STRIPE_PREREAD_ACTIVE, &sh->state))
			s.dec_preread_active = 1;
	}

	/*
	 * might be able to return some write requests if the parity blocks
	 * are safe, or on a failed drive
	 */
	pdev = &sh->dev[sh->pd_idx];
	s.p_failed = (s.failed >= 1 && s.failed_num[0] == sh->pd_idx)
		|| (s.failed >= 2 && s.failed_num[1] == sh->pd_idx);
	qdev = &sh->dev[sh->qd_idx];
	s.q_failed = (s.failed >= 1 && s.failed_num[0] == sh->qd_idx)
		|| (s.failed >= 2 && s.failed_num[1] == sh->qd_idx)
		|| conf->level < 6;

	if (s.written &&
	    (s.p_failed || ((test_bit(R5_Insync, &pdev->flags)
			     && !test_bit(R5_LOCKED, &pdev->flags)
			     && (test_bit(R5_UPTODATE, &pdev->flags) ||
				 test_bit(R5_Discard, &pdev->flags))))) &&
	    (s.q_failed || ((test_bit(R5_Insync, &qdev->flags)
			     && !test_bit(R5_LOCKED, &qdev->flags)
			     && (test_bit(R5_UPTODATE, &qdev->flags) ||
				 test_bit(R5_Discard, &qdev->flags))))))
		handle_stripe_clean_event(conf, sh, disks);

	if (s.just_cached)
		r5c_handle_cached_data_endio(conf, sh, disks);
	log_stripe_write_finished(sh);

	/* Now we might consider reading some blocks, either to check/generate
	 * parity, or to satisfy requests
	 * or to load a block that is being partially written.
	 */
	if (s.to_read || s.non_overwrite
	    || (s.to_write && s.failed)
	    || (s.syncing && (s.uptodate + s.compute < disks))
	    || s.replacing
	    || s.expanding)
		handle_stripe_fill(sh, &s, disks);

	/*
	 * When the stripe finishes full journal write cycle (write to journal
	 * and raid disk), this is the clean up procedure so it is ready for
	 * next operation.
	 */
	r5c_finish_stripe_write_out(conf, sh, &s);

	/*
	 * Now to consider new write requests, cache write back and what else,
	 * if anything should be read.  We do not handle new writes when:
	 * 1/ A 'write' operation (copy+xor) is already in flight.
	 * 2/ A 'check' operation is in flight, as it may clobber the parity
	 *    block.
	 * 3/ A r5c cache log write is in flight.
	 */

	if (!sh->reconstruct_state && !sh->check_state && !sh->log_io) {
		if (!r5c_is_writeback(conf->log)) {
			if (s.to_write)
				handle_stripe_dirtying(conf, sh, &s, disks);
		} else { /* write back cache */
			int ret = 0;

			/* First, try handle writes in caching phase */
			if (s.to_write)
				ret = r5c_try_caching_write(conf, sh, &s,
							    disks);
			/*
			 * If caching phase failed: ret == -EAGAIN
			 *    OR
			 * stripe under reclaim: !caching && injournal
			 *
			 * fall back to handle_stripe_dirtying()
			 */
			if (ret == -EAGAIN ||
			    /* stripe under reclaim: !caching && injournal */
			    (!test_bit(STRIPE_R5C_CACHING, &sh->state) &&
			     s.injournal > 0)) {
				ret = handle_stripe_dirtying(conf, sh, &s,
							     disks);
				if (ret == -EAGAIN)
					goto finish;
			}
		}
	}

	/* maybe we need to check and possibly fix the parity for this stripe
	 * Any reads will already have been scheduled, so we just see if enough
	 * data is available.  The parity check is held off while parity
	 * dependent operations are in flight.
	 */
	if (sh->check_state ||
	    (s.syncing && s.locked == 0 &&
	     !test_bit(STRIPE_COMPUTE_RUN, &sh->state) &&
	     !test_bit(STRIPE_INSYNC, &sh->state))) {
		if (conf->level == 6)
			handle_parity_checks6(conf, sh, &s, disks);
		else
			handle_parity_checks5(conf, sh, &s, disks);
	}

	if ((s.replacing || s.syncing) && s.locked == 0
	    && !test_bit(STRIPE_COMPUTE_RUN, &sh->state)
	    && !test_bit(STRIPE_REPLACED, &sh->state)) {
		/* Write out to replacement devices where possible */
		for (i = 0; i < conf->raid_disks; i++)
			if (test_bit(R5_NeedReplace, &sh->dev[i].flags)) {
				WARN_ON(!test_bit(R5_UPTODATE, &sh->dev[i].flags));
				set_bit(R5_WantReplace, &sh->dev[i].flags);
				set_bit(R5_LOCKED, &sh->dev[i].flags);
				s.locked++;
			}
		if (s.replacing)
			set_bit(STRIPE_INSYNC, &sh->state);
		set_bit(STRIPE_REPLACED, &sh->state);
	}
	if ((s.syncing || s.replacing) && s.locked == 0 &&
	    !test_bit(STRIPE_COMPUTE_RUN, &sh->state) &&
	    test_bit(STRIPE_INSYNC, &sh->state)) {
		md_done_sync(conf->mddev, RAID5_STRIPE_SECTORS(conf), 1);
		clear_bit(STRIPE_SYNCING, &sh->state);
		if (test_and_clear_bit(R5_Overlap, &sh->dev[sh->pd_idx].flags))
			wake_up_bit(&sh->dev[sh->pd_idx].flags, R5_Overlap);
	}

	/* If the failed drives are just a ReadError, then we might need
	 * to progress the repair/check process
	 */
	if (s.failed <= conf->max_degraded && !conf->mddev->ro)
		for (i = 0; i < s.failed; i++) {
			struct r5dev *dev = &sh->dev[s.failed_num[i]];
			if (test_bit(R5_ReadError, &dev->flags)
			    && !test_bit(R5_LOCKED, &dev->flags)
			    && test_bit(R5_UPTODATE, &dev->flags)
				) {
				if (!test_bit(R5_ReWrite, &dev->flags)) {
					set_bit(R5_Wantwrite, &dev->flags);
					set_bit(R5_ReWrite, &dev->flags);
				} else
					/* let's read it back */
					set_bit(R5_Wantread, &dev->flags);
				set_bit(R5_LOCKED, &dev->flags);
				s.locked++;
			}
		}

	/* Finish reconstruct operations initiated by the expansion process */
	if (sh->reconstruct_state == reconstruct_state_result) {
		struct stripe_head *sh_src
			= raid5_get_active_stripe(conf, NULL, sh->sector,
					R5_GAS_PREVIOUS | R5_GAS_NOBLOCK |
					R5_GAS_NOQUIESCE);
		if (sh_src && test_bit(STRIPE_EXPAND_SOURCE, &sh_src->state)) {
			/* sh cannot be written until sh_src has been read.
			 * so arrange for sh to be delayed a little
			 */
			set_bit(STRIPE_DELAYED, &sh->state);
			set_bit(STRIPE_HANDLE, &sh->state);
			if (!test_and_set_bit(STRIPE_PREREAD_ACTIVE,
					      &sh_src->state))
				atomic_inc(&conf->preread_active_stripes);
			raid5_release_stripe(sh_src);
			goto finish;
		}
		if (sh_src)
			raid5_release_stripe(sh_src);

		sh->reconstruct_state = reconstruct_state_idle;
		clear_bit(STRIPE_EXPANDING, &sh->state);
		for (i = conf->raid_disks; i--; ) {
			set_bit(R5_Wantwrite, &sh->dev[i].flags);
			set_bit(R5_LOCKED, &sh->dev[i].flags);
			s.locked++;
		}
	}

	if (s.expanded && test_bit(STRIPE_EXPANDING, &sh->state) &&
	    !sh->reconstruct_state) {
		/* Need to write out all blocks after computing parity */
		sh->disks = conf->raid_disks;
		stripe_set_idx(sh->sector, conf, 0, sh);
		schedule_reconstruction(sh, &s, 1, 1);
	} else if (s.expanded && !sh->reconstruct_state && s.locked == 0) {
		clear_bit(STRIPE_EXPAND_READY, &sh->state);
		atomic_dec(&conf->reshape_stripes);
		wake_up(&conf->wait_for_reshape);
		md_done_sync(conf->mddev, RAID5_STRIPE_SECTORS(conf), 1);
	}

	if (s.expanding && s.locked == 0 &&
	    !test_bit(STRIPE_COMPUTE_RUN, &sh->state))
		handle_stripe_expansion(conf, sh);

finish:
	/* wait for this device to become unblocked */
	if (unlikely(s.blocked_rdev)) {
		if (conf->mddev->external)
			md_wait_for_blocked_rdev(s.blocked_rdev,
						 conf->mddev);
		else
			/* Internal metadata will immediately
			 * be written by raid5d, so we don't
			 * need to wait here.
			 */
			rdev_dec_pending(s.blocked_rdev,
					 conf->mddev);
	}

	if (s.handle_bad_blocks)
		for (i = disks; i--; ) {
			struct md_rdev *rdev;
			struct r5dev *dev = &sh->dev[i];
			if (test_and_clear_bit(R5_WriteError, &dev->flags)) {
				/* We own a safe reference to the rdev */
				rdev = conf->disks[i].rdev;
				if (!rdev_set_badblocks(rdev, sh->sector,
							RAID5_STRIPE_SECTORS(conf), 0))
					md_error(conf->mddev, rdev);
				rdev_dec_pending(rdev, conf->mddev);
			}
			if (test_and_clear_bit(R5_MadeGood, &dev->flags)) {
				rdev = conf->disks[i].rdev;
				rdev_clear_badblocks(rdev, sh->sector,
						     RAID5_STRIPE_SECTORS(conf), 0);
				rdev_dec_pending(rdev, conf->mddev);
			}
			if (test_and_clear_bit(R5_MadeGoodRepl, &dev->flags)) {
				rdev = conf->disks[i].replacement;
				if (!rdev)
					/* rdev have been moved down */
					rdev = conf->disks[i].rdev;
				rdev_clear_badblocks(rdev, sh->sector,
						     RAID5_STRIPE_SECTORS(conf), 0);
				rdev_dec_pending(rdev, conf->mddev);
			}
		}

	if (s.ops_request)
		raid_run_ops(sh, s.ops_request);

	ops_run_io(sh, &s);

	if (s.dec_preread_active) {
		/* We delay this until after ops_run_io so that if make_request
		 * is waiting on a flush, it won't continue until the writes
		 * have actually been submitted.
		 */
		atomic_dec(&conf->preread_active_stripes);
		if (atomic_read(&conf->preread_active_stripes) <
		    IO_THRESHOLD)
			md_wakeup_thread(conf->mddev->thread);
	}

	clear_bit_unlock(STRIPE_ACTIVE, &sh->state);
}

static void raid5_activate_delayed(struct r5conf *conf)
	__must_hold(&conf->device_lock)
{
	if (atomic_read(&conf->preread_active_stripes) < IO_THRESHOLD) {
		while (!list_empty(&conf->delayed_list)) {
			struct list_head *l = conf->delayed_list.next;
			struct stripe_head *sh;
			sh = list_entry(l, struct stripe_head, lru);
			list_del_init(l);
			clear_bit(STRIPE_DELAYED, &sh->state);
			if (!test_and_set_bit(STRIPE_PREREAD_ACTIVE, &sh->state))
				atomic_inc(&conf->preread_active_stripes);
			list_add_tail(&sh->lru, &conf->hold_list);
			raid5_wakeup_stripe_thread(sh);
		}
	}
}

static void activate_bit_delay(struct r5conf *conf,
		struct list_head *temp_inactive_list)
	__must_hold(&conf->device_lock)
{
	struct list_head head;
	list_add(&head, &conf->bitmap_list);
	list_del_init(&conf->bitmap_list);
	while (!list_empty(&head)) {
		struct stripe_head *sh = list_entry(head.next, struct stripe_head, lru);
		int hash;
		list_del_init(&sh->lru);
		atomic_inc(&sh->count);
		hash = sh->hash_lock_index;
		__release_stripe(conf, sh, &temp_inactive_list[hash]);
	}
}

static int in_chunk_boundary(struct mddev *mddev, struct bio *bio)
{
	struct r5conf *conf = mddev->private;
	sector_t sector = bio->bi_iter.bi_sector;
	unsigned int chunk_sectors;
	unsigned int bio_sectors = bio_sectors(bio);

	chunk_sectors = min(conf->chunk_sectors, conf->prev_chunk_sectors);
	return  chunk_sectors >=
		((sector & (chunk_sectors - 1)) + bio_sectors);
}

/*
 *  add bio to the retry LIFO  ( in O(1) ... we are in interrupt )
 *  later sampled by raid5d.
 */
static void add_bio_to_retry(struct bio *bi,struct r5conf *conf)
{
	unsigned long flags;

	spin_lock_irqsave(&conf->device_lock, flags);

	bi->bi_next = conf->retry_read_aligned_list;
	conf->retry_read_aligned_list = bi;

	spin_unlock_irqrestore(&conf->device_lock, flags);
	md_wakeup_thread(conf->mddev->thread);
}

static struct bio *remove_bio_from_retry(struct r5conf *conf,
					 unsigned int *offset)
{
	struct bio *bi;

	bi = conf->retry_read_aligned;
	if (bi) {
		*offset = conf->retry_read_offset;
		conf->retry_read_aligned = NULL;
		return bi;
	}
	bi = conf->retry_read_aligned_list;
	if(bi) {
		conf->retry_read_aligned_list = bi->bi_next;
		bi->bi_next = NULL;
		*offset = 0;
	}

	return bi;
}

/*
 *  The "raid5_align_endio" should check if the read succeeded and if it
 *  did, call bio_endio on the original bio (having bio_put the new bio
 *  first).
 *  If the read failed..
 */
static void raid5_align_endio(struct bio *bi)
{
	struct bio *raid_bi = bi->bi_private;
	struct md_rdev *rdev = (void *)raid_bi->bi_next;
	struct mddev *mddev = rdev->mddev;
	struct r5conf *conf = mddev->private;
	blk_status_t error = bi->bi_status;

	bio_put(bi);
	raid_bi->bi_next = NULL;
	rdev_dec_pending(rdev, conf->mddev);

	if (!error) {
		bio_endio(raid_bi);
		if (atomic_dec_and_test(&conf->active_aligned_reads))
			wake_up(&conf->wait_for_quiescent);
		return;
	}

	pr_debug("raid5_align_endio : io error...handing IO for a retry\n");

	add_bio_to_retry(raid_bi, conf);
}

static int raid5_read_one_chunk(struct mddev *mddev, struct bio *raid_bio)
{
	struct r5conf *conf = mddev->private;
	struct bio *align_bio;
	struct md_rdev *rdev;
	sector_t sector, end_sector;
	int dd_idx;
	bool did_inc;

	if (!in_chunk_boundary(mddev, raid_bio)) {
		pr_debug("%s: non aligned\n", __func__);
		return 0;
	}

	sector = raid5_compute_sector(conf, raid_bio->bi_iter.bi_sector, 0,
				      &dd_idx, NULL);
	end_sector = sector + bio_sectors(raid_bio);

	if (r5c_big_stripe_cached(conf, sector))
		return 0;

	rdev = conf->disks[dd_idx].replacement;
	if (!rdev || test_bit(Faulty, &rdev->flags) ||
	    rdev->recovery_offset < end_sector) {
		rdev = conf->disks[dd_idx].rdev;
		if (!rdev)
			return 0;
		if (test_bit(Faulty, &rdev->flags) ||
		    !(test_bit(In_sync, &rdev->flags) ||
		      rdev->recovery_offset >= end_sector))
			return 0;
	}

	atomic_inc(&rdev->nr_pending);

	if (rdev_has_badblock(rdev, sector, bio_sectors(raid_bio))) {
		rdev_dec_pending(rdev, mddev);
		return 0;
	}

	md_account_bio(mddev, &raid_bio);
	raid_bio->bi_next = (void *)rdev;

	align_bio = bio_alloc_clone(rdev->bdev, raid_bio, GFP_NOIO,
				    &mddev->bio_set);
	align_bio->bi_end_io = raid5_align_endio;
	align_bio->bi_private = raid_bio;
	align_bio->bi_iter.bi_sector = sector;

	/* No reshape active, so we can trust rdev->data_offset */
	align_bio->bi_iter.bi_sector += rdev->data_offset;

	did_inc = false;
	if (conf->quiesce == 0) {
		atomic_inc(&conf->active_aligned_reads);
		did_inc = true;
	}
	/* need a memory barrier to detect the race with raid5_quiesce() */
	if (!did_inc || smp_load_acquire(&conf->quiesce) != 0) {
		/* quiesce is in progress, so we need to undo io activation and wait
		 * for it to finish
		 */
		if (did_inc && atomic_dec_and_test(&conf->active_aligned_reads))
			wake_up(&conf->wait_for_quiescent);
		spin_lock_irq(&conf->device_lock);
		wait_event_lock_irq(conf->wait_for_quiescent, conf->quiesce == 0,
				    conf->device_lock);
		atomic_inc(&conf->active_aligned_reads);
		spin_unlock_irq(&conf->device_lock);
	}

	mddev_trace_remap(mddev, align_bio, raid_bio->bi_iter.bi_sector);
	submit_bio_noacct(align_bio);
	return 1;
}

static struct bio *chunk_aligned_read(struct mddev *mddev, struct bio *raid_bio)
{
	struct bio *split;
	sector_t sector = raid_bio->bi_iter.bi_sector;
	unsigned chunk_sects = mddev->chunk_sectors;
	unsigned sectors = chunk_sects - (sector & (chunk_sects-1));

	if (sectors < bio_sectors(raid_bio)) {
		struct r5conf *conf = mddev->private;
		split = bio_split(raid_bio, sectors, GFP_NOIO, &conf->bio_split);
		bio_chain(split, raid_bio);
		submit_bio_noacct(raid_bio);
		raid_bio = split;
	}

	if (!raid5_read_one_chunk(mddev, raid_bio))
		return raid_bio;

	return NULL;
}

/* __get_priority_stripe - get the next stripe to process
 *
 * Full stripe writes are allowed to pass preread active stripes up until
 * the bypass_threshold is exceeded.  In general the bypass_count
 * increments when the handle_list is handled before the hold_list; however, it
 * will not be incremented when STRIPE_IO_STARTED is sampled set signifying a
 * stripe with in flight i/o.  The bypass_count will be reset when the
 * head of the hold_list has changed, i.e. the head was promoted to the
 * handle_list.
 */
static struct stripe_head *__get_priority_stripe(struct r5conf *conf, int group)
	__must_hold(&conf->device_lock)
{
	struct stripe_head *sh, *tmp;
	struct list_head *handle_list = NULL;
	struct r5worker_group *wg;
	bool second_try = !r5c_is_writeback(conf->log) &&
		!r5l_log_disk_error(conf);
	bool try_loprio = test_bit(R5C_LOG_TIGHT, &conf->cache_state) ||
		r5l_log_disk_error(conf);

again:
	wg = NULL;
	sh = NULL;
	if (conf->worker_cnt_per_group == 0) {
		handle_list = try_loprio ? &conf->loprio_list :
					&conf->handle_list;
	} else if (group != ANY_GROUP) {
		handle_list = try_loprio ? &conf->worker_groups[group].loprio_list :
				&conf->worker_groups[group].handle_list;
		wg = &conf->worker_groups[group];
	} else {
		int i;
		for (i = 0; i < conf->group_cnt; i++) {
			handle_list = try_loprio ? &conf->worker_groups[i].loprio_list :
				&conf->worker_groups[i].handle_list;
			wg = &conf->worker_groups[i];
			if (!list_empty(handle_list))
				break;
		}
	}

	pr_debug("%s: handle: %s hold: %s full_writes: %d bypass_count: %d\n",
		  __func__,
		  list_empty(handle_list) ? "empty" : "busy",
		  list_empty(&conf->hold_list) ? "empty" : "busy",
		  atomic_read(&conf->pending_full_writes), conf->bypass_count);

	if (!list_empty(handle_list)) {
		sh = list_entry(handle_list->next, typeof(*sh), lru);

		if (list_empty(&conf->hold_list))
			conf->bypass_count = 0;
		else if (!test_bit(STRIPE_IO_STARTED, &sh->state)) {
			if (conf->hold_list.next == conf->last_hold)
				conf->bypass_count++;
			else {
				conf->last_hold = conf->hold_list.next;
				conf->bypass_count -= conf->bypass_threshold;
				if (conf->bypass_count < 0)
					conf->bypass_count = 0;
			}
		}
	} else if (!list_empty(&conf->hold_list) &&
		   ((conf->bypass_threshold &&
		     conf->bypass_count > conf->bypass_threshold) ||
		    atomic_read(&conf->pending_full_writes) == 0)) {

		list_for_each_entry(tmp, &conf->hold_list,  lru) {
			if (conf->worker_cnt_per_group == 0 ||
			    group == ANY_GROUP ||
			    !cpu_online(tmp->cpu) ||
			    cpu_to_group(tmp->cpu) == group) {
				sh = tmp;
				break;
			}
		}

		if (sh) {
			conf->bypass_count -= conf->bypass_threshold;
			if (conf->bypass_count < 0)
				conf->bypass_count = 0;
		}
		wg = NULL;
	}

	if (!sh) {
		if (second_try)
			return NULL;
		second_try = true;
		try_loprio = !try_loprio;
		goto again;
	}

	if (wg) {
		wg->stripes_cnt--;
		sh->group = NULL;
	}
	list_del_init(&sh->lru);
	BUG_ON(atomic_inc_return(&sh->count) != 1);
	return sh;
}

struct raid5_plug_cb {
	struct blk_plug_cb	cb;
	struct list_head	list;
	struct list_head	temp_inactive_list[NR_STRIPE_HASH_LOCKS];
};

static void raid5_unplug(struct blk_plug_cb *blk_cb, bool from_schedule)
{
	struct raid5_plug_cb *cb = container_of(
		blk_cb, struct raid5_plug_cb, cb);
	struct stripe_head *sh;
	struct mddev *mddev = cb->cb.data;
	struct r5conf *conf = mddev->private;
	int cnt = 0;
	int hash;

	if (cb->list.next && !list_empty(&cb->list)) {
		spin_lock_irq(&conf->device_lock);
		while (!list_empty(&cb->list)) {
			sh = list_first_entry(&cb->list, struct stripe_head, lru);
			list_del_init(&sh->lru);
			/*
			 * avoid race release_stripe_plug() sees
			 * STRIPE_ON_UNPLUG_LIST clear but the stripe
			 * is still in our list
			 */
			smp_mb__before_atomic();
			clear_bit(STRIPE_ON_UNPLUG_LIST, &sh->state);
			/*
			 * STRIPE_ON_RELEASE_LIST could be set here. In that
			 * case, the count is always > 1 here
			 */
			hash = sh->hash_lock_index;
			__release_stripe(conf, sh, &cb->temp_inactive_list[hash]);
			cnt++;
		}
		spin_unlock_irq(&conf->device_lock);
	}
	release_inactive_stripe_list(conf, cb->temp_inactive_list,
				     NR_STRIPE_HASH_LOCKS);
	if (!mddev_is_dm(mddev))
		trace_block_unplug(mddev->gendisk->queue, cnt, !from_schedule);
	kfree(cb);
}

static void release_stripe_plug(struct mddev *mddev,
				struct stripe_head *sh)
{
	struct blk_plug_cb *blk_cb = blk_check_plugged(
		raid5_unplug, mddev,
		sizeof(struct raid5_plug_cb));
	struct raid5_plug_cb *cb;

	if (!blk_cb) {
		raid5_release_stripe(sh);
		return;
	}

	cb = container_of(blk_cb, struct raid5_plug_cb, cb);

	if (cb->list.next == NULL) {
		int i;
		INIT_LIST_HEAD(&cb->list);
		for (i = 0; i < NR_STRIPE_HASH_LOCKS; i++)
			INIT_LIST_HEAD(cb->temp_inactive_list + i);
	}

	if (!test_and_set_bit(STRIPE_ON_UNPLUG_LIST, &sh->state))
		list_add_tail(&sh->lru, &cb->list);
	else
		raid5_release_stripe(sh);
}

static void make_discard_request(struct mddev *mddev, struct bio *bi)
{
	struct r5conf *conf = mddev->private;
	sector_t logical_sector, last_sector;
	struct stripe_head *sh;
	int stripe_sectors;

	/* We need to handle this when io_uring supports discard/trim */
	if (WARN_ON_ONCE(bi->bi_opf & REQ_NOWAIT))
		return;

	if (mddev->reshape_position != MaxSector)
		/* Skip discard while reshape is happening */
		return;

	logical_sector = bi->bi_iter.bi_sector & ~((sector_t)RAID5_STRIPE_SECTORS(conf)-1);
	last_sector = bio_end_sector(bi);

	bi->bi_next = NULL;

	stripe_sectors = conf->chunk_sectors *
		(conf->raid_disks - conf->max_degraded);
	logical_sector = DIV_ROUND_UP_SECTOR_T(logical_sector,
					       stripe_sectors);
	sector_div(last_sector, stripe_sectors);

	logical_sector *= conf->chunk_sectors;
	last_sector *= conf->chunk_sectors;

	for (; logical_sector < last_sector;
	     logical_sector += RAID5_STRIPE_SECTORS(conf)) {
		DEFINE_WAIT(w);
		int d;
	again:
		sh = raid5_get_active_stripe(conf, NULL, logical_sector, 0);
		set_bit(R5_Overlap, &sh->dev[sh->pd_idx].flags);
		if (test_bit(STRIPE_SYNCING, &sh->state)) {
			raid5_release_stripe(sh);
			wait_on_bit(&sh->dev[sh->pd_idx].flags, R5_Overlap,
				    TASK_UNINTERRUPTIBLE);
			goto again;
		}
		clear_bit(R5_Overlap, &sh->dev[sh->pd_idx].flags);
		spin_lock_irq(&sh->stripe_lock);
		for (d = 0; d < conf->raid_disks; d++) {
			if (d == sh->pd_idx || d == sh->qd_idx)
				continue;
			if (sh->dev[d].towrite || sh->dev[d].toread) {
				set_bit(R5_Overlap, &sh->dev[d].flags);
				spin_unlock_irq(&sh->stripe_lock);
				raid5_release_stripe(sh);
				wait_on_bit(&sh->dev[d].flags, R5_Overlap,
					    TASK_UNINTERRUPTIBLE);
				goto again;
			}
		}
		set_bit(STRIPE_DISCARD, &sh->state);
		sh->overwrite_disks = 0;
		for (d = 0; d < conf->raid_disks; d++) {
			if (d == sh->pd_idx || d == sh->qd_idx)
				continue;
			sh->dev[d].towrite = bi;
			set_bit(R5_OVERWRITE, &sh->dev[d].flags);
			bio_inc_remaining(bi);
			md_write_inc(mddev, bi);
			sh->overwrite_disks++;
		}
		spin_unlock_irq(&sh->stripe_lock);
		if (conf->mddev->bitmap) {
			sh->bm_seq = conf->seq_flush + 1;
			set_bit(STRIPE_BIT_DELAY, &sh->state);
		}

		set_bit(STRIPE_HANDLE, &sh->state);
		clear_bit(STRIPE_DELAYED, &sh->state);
		if (!test_and_set_bit(STRIPE_PREREAD_ACTIVE, &sh->state))
			atomic_inc(&conf->preread_active_stripes);
		release_stripe_plug(mddev, sh);
	}

	bio_endio(bi);
}

static bool ahead_of_reshape(struct mddev *mddev, sector_t sector,
			     sector_t reshape_sector)
{
	return mddev->reshape_backwards ? sector < reshape_sector :
					  sector >= reshape_sector;
}

static bool range_ahead_of_reshape(struct mddev *mddev, sector_t min,
				   sector_t max, sector_t reshape_sector)
{
	return mddev->reshape_backwards ? max < reshape_sector :
					  min >= reshape_sector;
}

static bool stripe_ahead_of_reshape(struct mddev *mddev, struct r5conf *conf,
				    struct stripe_head *sh)
{
	sector_t max_sector = 0, min_sector = MaxSector;
	bool ret = false;
	int dd_idx;

	for (dd_idx = 0; dd_idx < sh->disks; dd_idx++) {
		if (dd_idx == sh->pd_idx || dd_idx == sh->qd_idx)
			continue;

		min_sector = min(min_sector, sh->dev[dd_idx].sector);
		max_sector = max(max_sector, sh->dev[dd_idx].sector);
	}

	spin_lock_irq(&conf->device_lock);

	if (!range_ahead_of_reshape(mddev, min_sector, max_sector,
				     conf->reshape_progress))
		/* mismatch, need to try again */
		ret = true;

	spin_unlock_irq(&conf->device_lock);

	return ret;
}

static int add_all_stripe_bios(struct r5conf *conf,
		struct stripe_request_ctx *ctx, struct stripe_head *sh,
		struct bio *bi, int forwrite, int previous)
{
	int dd_idx;

	spin_lock_irq(&sh->stripe_lock);

	for (dd_idx = 0; dd_idx < sh->disks; dd_idx++) {
		struct r5dev *dev = &sh->dev[dd_idx];

		if (dd_idx == sh->pd_idx || dd_idx == sh->qd_idx)
			continue;

		if (dev->sector < ctx->first_sector ||
		    dev->sector >= ctx->last_sector)
			continue;

		if (stripe_bio_overlaps(sh, bi, dd_idx, forwrite)) {
			set_bit(R5_Overlap, &dev->flags);
			spin_unlock_irq(&sh->stripe_lock);
			raid5_release_stripe(sh);
			/* release batch_last before wait to avoid risk of deadlock */
			if (ctx->batch_last) {
				raid5_release_stripe(ctx->batch_last);
				ctx->batch_last = NULL;
			}
			md_wakeup_thread(conf->mddev->thread);
			wait_on_bit(&dev->flags, R5_Overlap, TASK_UNINTERRUPTIBLE);
			return 0;
		}
	}

	for (dd_idx = 0; dd_idx < sh->disks; dd_idx++) {
		struct r5dev *dev = &sh->dev[dd_idx];

		if (dd_idx == sh->pd_idx || dd_idx == sh->qd_idx)
			continue;

		if (dev->sector < ctx->first_sector ||
		    dev->sector >= ctx->last_sector)
			continue;

		__add_stripe_bio(sh, bi, dd_idx, forwrite, previous);
		clear_bit((dev->sector - ctx->first_sector) >>
			  RAID5_STRIPE_SHIFT(conf), ctx->sectors_to_do);
	}

	spin_unlock_irq(&sh->stripe_lock);
	return 1;
}

enum reshape_loc {
	LOC_NO_RESHAPE,
	LOC_AHEAD_OF_RESHAPE,
	LOC_INSIDE_RESHAPE,
	LOC_BEHIND_RESHAPE,
};

static enum reshape_loc get_reshape_loc(struct mddev *mddev,
		struct r5conf *conf, sector_t logical_sector)
{
	sector_t reshape_progress, reshape_safe;

	if (likely(conf->reshape_progress == MaxSector))
		return LOC_NO_RESHAPE;
	/*
	 * Spinlock is needed as reshape_progress may be
	 * 64bit on a 32bit platform, and so it might be
	 * possible to see a half-updated value
	 * Of course reshape_progress could change after
	 * the lock is dropped, so once we get a reference
	 * to the stripe that we think it is, we will have
	 * to check again.
	 */
	spin_lock_irq(&conf->device_lock);
	reshape_progress = conf->reshape_progress;
	reshape_safe = conf->reshape_safe;
	spin_unlock_irq(&conf->device_lock);
	if (reshape_progress == MaxSector)
		return LOC_NO_RESHAPE;
	if (ahead_of_reshape(mddev, logical_sector, reshape_progress))
		return LOC_AHEAD_OF_RESHAPE;
	if (ahead_of_reshape(mddev, logical_sector, reshape_safe))
		return LOC_INSIDE_RESHAPE;
	return LOC_BEHIND_RESHAPE;
}

static void raid5_bitmap_sector(struct mddev *mddev, sector_t *offset,
				unsigned long *sectors)
{
	struct r5conf *conf = mddev->private;
	sector_t start = *offset;
	sector_t end = start + *sectors;
	sector_t prev_start = start;
	sector_t prev_end = end;
	int sectors_per_chunk;
	enum reshape_loc loc;
	int dd_idx;

	sectors_per_chunk = conf->chunk_sectors *
		(conf->raid_disks - conf->max_degraded);
	start = round_down(start, sectors_per_chunk);
	end = round_up(end, sectors_per_chunk);

	start = raid5_compute_sector(conf, start, 0, &dd_idx, NULL);
	end = raid5_compute_sector(conf, end, 0, &dd_idx, NULL);

	/*
	 * For LOC_INSIDE_RESHAPE, this IO will wait for reshape to make
	 * progress, hence it's the same as LOC_BEHIND_RESHAPE.
	 */
	loc = get_reshape_loc(mddev, conf, prev_start);
	if (likely(loc != LOC_AHEAD_OF_RESHAPE)) {
		*offset = start;
		*sectors = end - start;
		return;
	}

	sectors_per_chunk = conf->prev_chunk_sectors *
		(conf->previous_raid_disks - conf->max_degraded);
	prev_start = round_down(prev_start, sectors_per_chunk);
	prev_end = round_down(prev_end, sectors_per_chunk);

	prev_start = raid5_compute_sector(conf, prev_start, 1, &dd_idx, NULL);
	prev_end = raid5_compute_sector(conf, prev_end, 1, &dd_idx, NULL);

	/*
	 * for LOC_AHEAD_OF_RESHAPE, reshape can make progress before this IO
	 * is handled in make_stripe_request(), we can't know this here hence
	 * we set bits for both.
	 */
	*offset = min(start, prev_start);
	*sectors = max(end, prev_end) - *offset;
}

static enum stripe_result make_stripe_request(struct mddev *mddev,
		struct r5conf *conf, struct stripe_request_ctx *ctx,
		sector_t logical_sector, struct bio *bi)
{
	const int rw = bio_data_dir(bi);
	enum stripe_result ret;
	struct stripe_head *sh;
	enum reshape_loc loc;
	sector_t new_sector;
	int previous = 0, flags = 0;
	int seq, dd_idx;

	seq = read_seqcount_begin(&conf->gen_lock);
	loc = get_reshape_loc(mddev, conf, logical_sector);
	if (loc == LOC_INSIDE_RESHAPE) {
		ret = STRIPE_SCHEDULE_AND_RETRY;
		goto out;
	}
	if (loc == LOC_AHEAD_OF_RESHAPE)
		previous = 1;

	new_sector = raid5_compute_sector(conf, logical_sector, previous,
					  &dd_idx, NULL);
	pr_debug("raid456: %s, sector %llu logical %llu\n", __func__,
		 new_sector, logical_sector);

	if (previous)
		flags |= R5_GAS_PREVIOUS;
	if (bi->bi_opf & REQ_RAHEAD)
		flags |= R5_GAS_NOBLOCK;
	sh = raid5_get_active_stripe(conf, ctx, new_sector, flags);
	if (unlikely(!sh)) {
		/* cannot get stripe, just give-up */
		bi->bi_status = BLK_STS_IOERR;
		return STRIPE_FAIL;
	}

	if (unlikely(previous) &&
	    stripe_ahead_of_reshape(mddev, conf, sh)) {
		/*
		 * Expansion moved on while waiting for a stripe.
		 * Expansion could still move past after this
		 * test, but as we are holding a reference to
		 * 'sh', we know that if that happens,
		 *  STRIPE_EXPANDING will get set and the expansion
		 * won't proceed until we finish with the stripe.
		 */
		ret = STRIPE_SCHEDULE_AND_RETRY;
		goto out_release;
	}

	if (read_seqcount_retry(&conf->gen_lock, seq)) {
		/* Might have got the wrong stripe_head by accident */
		ret = STRIPE_RETRY;
		goto out_release;
	}

	if (test_bit(STRIPE_EXPANDING, &sh->state)) {
		md_wakeup_thread(mddev->thread);
		ret = STRIPE_SCHEDULE_AND_RETRY;
		goto out_release;
	}

	if (!add_all_stripe_bios(conf, ctx, sh, bi, rw, previous)) {
		ret = STRIPE_RETRY;
		goto out;
	}

	if (stripe_can_batch(sh)) {
		stripe_add_to_batch_list(conf, sh, ctx->batch_last);
		if (ctx->batch_last)
			raid5_release_stripe(ctx->batch_last);
		atomic_inc(&sh->count);
		ctx->batch_last = sh;
	}

	if (ctx->do_flush) {
		set_bit(STRIPE_R5C_PREFLUSH, &sh->state);
		/* we only need flush for one stripe */
		ctx->do_flush = false;
	}

	set_bit(STRIPE_HANDLE, &sh->state);
	clear_bit(STRIPE_DELAYED, &sh->state);
	if ((!sh->batch_head || sh == sh->batch_head) &&
	    (bi->bi_opf & REQ_SYNC) &&
	    !test_and_set_bit(STRIPE_PREREAD_ACTIVE, &sh->state))
		atomic_inc(&conf->preread_active_stripes);

	release_stripe_plug(mddev, sh);
	return STRIPE_SUCCESS;

out_release:
	raid5_release_stripe(sh);
out:
	if (ret == STRIPE_SCHEDULE_AND_RETRY && reshape_interrupted(mddev)) {
		bi->bi_status = BLK_STS_RESOURCE;
		ret = STRIPE_WAIT_RESHAPE;
		pr_err_ratelimited("dm-raid456: io across reshape position while reshape can't make progress");
	}
	return ret;
}

/*
 * If the bio covers multiple data disks, find sector within the bio that has
 * the lowest chunk offset in the first chunk.
 */
static sector_t raid5_bio_lowest_chunk_sector(struct r5conf *conf,
					      struct bio *bi)
{
	int sectors_per_chunk = conf->chunk_sectors;
	int raid_disks = conf->raid_disks;
	int dd_idx;
	struct stripe_head sh;
	unsigned int chunk_offset;
	sector_t r_sector = bi->bi_iter.bi_sector & ~((sector_t)RAID5_STRIPE_SECTORS(conf)-1);
	sector_t sector;

	/* We pass in fake stripe_head to get back parity disk numbers */
	sector = raid5_compute_sector(conf, r_sector, 0, &dd_idx, &sh);
	chunk_offset = sector_div(sector, sectors_per_chunk);
	if (sectors_per_chunk - chunk_offset >= bio_sectors(bi))
		return r_sector;
	/*
	 * Bio crosses to the next data disk. Check whether it's in the same
	 * chunk.
	 */
	dd_idx++;
	while (dd_idx == sh.pd_idx || dd_idx == sh.qd_idx)
		dd_idx++;
	if (dd_idx >= raid_disks)
		return r_sector;
	return r_sector + sectors_per_chunk - chunk_offset;
}

static bool raid5_make_request(struct mddev *mddev, struct bio * bi)
{
	DEFINE_WAIT_FUNC(wait, woken_wake_function);
	bool on_wq;
	struct r5conf *conf = mddev->private;
	sector_t logical_sector;
	struct stripe_request_ctx ctx = {};
	const int rw = bio_data_dir(bi);
	enum stripe_result res;
	int s, stripe_cnt;

	if (unlikely(bi->bi_opf & REQ_PREFLUSH)) {
		int ret = log_handle_flush_request(conf, bi);

		if (ret == 0)
			return true;
		if (ret == -ENODEV) {
			if (md_flush_request(mddev, bi))
				return true;
		}
		/* ret == -EAGAIN, fallback */
		/*
		 * if r5l_handle_flush_request() didn't clear REQ_PREFLUSH,
		 * we need to flush journal device
		 */
		ctx.do_flush = bi->bi_opf & REQ_PREFLUSH;
	}

	md_write_start(mddev, bi);
	/*
	 * If array is degraded, better not do chunk aligned read because
	 * later we might have to read it again in order to reconstruct
	 * data on failed drives.
	 */
	if (rw == READ && mddev->degraded == 0 &&
	    mddev->reshape_position == MaxSector) {
		bi = chunk_aligned_read(mddev, bi);
		if (!bi)
			return true;
	}

	if (unlikely(bio_op(bi) == REQ_OP_DISCARD)) {
		make_discard_request(mddev, bi);
		md_write_end(mddev);
		return true;
	}

	logical_sector = bi->bi_iter.bi_sector & ~((sector_t)RAID5_STRIPE_SECTORS(conf)-1);
	ctx.first_sector = logical_sector;
	ctx.last_sector = bio_end_sector(bi);
	bi->bi_next = NULL;

	stripe_cnt = DIV_ROUND_UP_SECTOR_T(ctx.last_sector - logical_sector,
					   RAID5_STRIPE_SECTORS(conf));
	bitmap_set(ctx.sectors_to_do, 0, stripe_cnt);

	pr_debug("raid456: %s, logical %llu to %llu\n", __func__,
		 bi->bi_iter.bi_sector, ctx.last_sector);

	/* Bail out if conflicts with reshape and REQ_NOWAIT is set */
	if ((bi->bi_opf & REQ_NOWAIT) &&
	    get_reshape_loc(mddev, conf, logical_sector) == LOC_INSIDE_RESHAPE) {
		bio_wouldblock_error(bi);
		if (rw == WRITE)
			md_write_end(mddev);
		return true;
	}
	md_account_bio(mddev, &bi);

	/*
	 * Lets start with the stripe with the lowest chunk offset in the first
	 * chunk. That has the best chances of creating IOs adjacent to
	 * previous IOs in case of sequential IO and thus creates the most
	 * sequential IO pattern. We don't bother with the optimization when
	 * reshaping as the performance benefit is not worth the complexity.
	 */
	if (likely(conf->reshape_progress == MaxSector)) {
		logical_sector = raid5_bio_lowest_chunk_sector(conf, bi);
		on_wq = false;
	} else {
		add_wait_queue(&conf->wait_for_reshape, &wait);
		on_wq = true;
	}
	s = (logical_sector - ctx.first_sector) >> RAID5_STRIPE_SHIFT(conf);

	while (1) {
		res = make_stripe_request(mddev, conf, &ctx, logical_sector,
					  bi);
		if (res == STRIPE_FAIL || res == STRIPE_WAIT_RESHAPE)
			break;

		if (res == STRIPE_RETRY)
			continue;

		if (res == STRIPE_SCHEDULE_AND_RETRY) {
			WARN_ON_ONCE(!on_wq);
			/*
			 * Must release the reference to batch_last before
			 * scheduling and waiting for work to be done,
			 * otherwise the batch_last stripe head could prevent
			 * raid5_activate_delayed() from making progress
			 * and thus deadlocking.
			 */
			if (ctx.batch_last) {
				raid5_release_stripe(ctx.batch_last);
				ctx.batch_last = NULL;
			}

			wait_woken(&wait, TASK_UNINTERRUPTIBLE,
				   MAX_SCHEDULE_TIMEOUT);
			continue;
		}

		s = find_next_bit_wrap(ctx.sectors_to_do, stripe_cnt, s);
		if (s == stripe_cnt)
			break;

		logical_sector = ctx.first_sector +
			(s << RAID5_STRIPE_SHIFT(conf));
	}
	if (unlikely(on_wq))
		remove_wait_queue(&conf->wait_for_reshape, &wait);

	if (ctx.batch_last)
		raid5_release_stripe(ctx.batch_last);

	if (rw == WRITE)
		md_write_end(mddev);
	if (res == STRIPE_WAIT_RESHAPE) {
		md_free_cloned_bio(bi);
		return false;
	}

	bio_endio(bi);
	return true;
}

static sector_t raid5_size(struct mddev *mddev, sector_t sectors, int raid_disks);

static sector_t reshape_request(struct mddev *mddev, sector_t sector_nr, int *skipped)
{
	/* reshaping is quite different to recovery/resync so it is
	 * handled quite separately ... here.
	 *
	 * On each call to sync_request, we gather one chunk worth of
	 * destination stripes and flag them as expanding.
	 * Then we find all the source stripes and request reads.
	 * As the reads complete, handle_stripe will copy the data
	 * into the destination stripe and release that stripe.
	 */
	struct r5conf *conf = mddev->private;
	struct stripe_head *sh;
	struct md_rdev *rdev;
	sector_t first_sector, last_sector;
	int raid_disks = conf->previous_raid_disks;
	int data_disks = raid_disks - conf->max_degraded;
	int new_data_disks = conf->raid_disks - conf->max_degraded;
	int i;
	int dd_idx;
	sector_t writepos, readpos, safepos;
	sector_t stripe_addr;
	int reshape_sectors;
	struct list_head stripes;
	sector_t retn;

	if (sector_nr == 0) {
		/* If restarting in the middle, skip the initial sectors */
		if (mddev->reshape_backwards &&
		    conf->reshape_progress < raid5_size(mddev, 0, 0)) {
			sector_nr = raid5_size(mddev, 0, 0)
				- conf->reshape_progress;
		} else if (mddev->reshape_backwards &&
			   conf->reshape_progress == MaxSector) {
			/* shouldn't happen, but just in case, finish up.*/
			sector_nr = MaxSector;
		} else if (!mddev->reshape_backwards &&
			   conf->reshape_progress > 0)
			sector_nr = conf->reshape_progress;
		sector_div(sector_nr, new_data_disks);
		if (sector_nr) {
			mddev->curr_resync_completed = sector_nr;
			sysfs_notify_dirent_safe(mddev->sysfs_completed);
			*skipped = 1;
			retn = sector_nr;
			goto finish;
		}
	}

	/* We need to process a full chunk at a time.
	 * If old and new chunk sizes differ, we need to process the
	 * largest of these
	 */

	reshape_sectors = max(conf->chunk_sectors, conf->prev_chunk_sectors);

	/* We update the metadata at least every 10 seconds, or when
	 * the data about to be copied would over-write the source of
	 * the data at the front of the range.  i.e. one new_stripe
	 * along from reshape_progress new_maps to after where
	 * reshape_safe old_maps to
	 */
	writepos = conf->reshape_progress;
	sector_div(writepos, new_data_disks);
	readpos = conf->reshape_progress;
	sector_div(readpos, data_disks);
	safepos = conf->reshape_safe;
	sector_div(safepos, data_disks);
	if (mddev->reshape_backwards) {
		if (WARN_ON(writepos < reshape_sectors))
			return MaxSector;

		writepos -= reshape_sectors;
		readpos += reshape_sectors;
		safepos += reshape_sectors;
	} else {
		writepos += reshape_sectors;
		/* readpos and safepos are worst-case calculations.
		 * A negative number is overly pessimistic, and causes
		 * obvious problems for unsigned storage.  So clip to 0.
		 */
		readpos -= min_t(sector_t, reshape_sectors, readpos);
		safepos -= min_t(sector_t, reshape_sectors, safepos);
	}

	/* Having calculated the 'writepos' possibly use it
	 * to set 'stripe_addr' which is where we will write to.
	 */
	if (mddev->reshape_backwards) {
		if (WARN_ON(conf->reshape_progress == 0))
			return MaxSector;

		stripe_addr = writepos;
		if (WARN_ON((mddev->dev_sectors &
		    ~((sector_t)reshape_sectors - 1)) -
		    reshape_sectors - stripe_addr != sector_nr))
			return MaxSector;
	} else {
		if (WARN_ON(writepos != sector_nr + reshape_sectors))
			return MaxSector;

		stripe_addr = sector_nr;
	}

	/* 'writepos' is the most advanced device address we might write.
	 * 'readpos' is the least advanced device address we might read.
	 * 'safepos' is the least address recorded in the metadata as having
	 *     been reshaped.
	 * If there is a min_offset_diff, these are adjusted either by
	 * increasing the safepos/readpos if diff is negative, or
	 * increasing writepos if diff is positive.
	 * If 'readpos' is then behind 'writepos', there is no way that we can
	 * ensure safety in the face of a crash - that must be done by userspace
	 * making a backup of the data.  So in that case there is no particular
	 * rush to update metadata.
	 * Otherwise if 'safepos' is behind 'writepos', then we really need to
	 * update the metadata to advance 'safepos' to match 'readpos' so that
	 * we can be safe in the event of a crash.
	 * So we insist on updating metadata if safepos is behind writepos and
	 * readpos is beyond writepos.
	 * In any case, update the metadata every 10 seconds.
	 * Maybe that number should be configurable, but I'm not sure it is
	 * worth it.... maybe it could be a multiple of safemode_delay???
	 */
	if (conf->min_offset_diff < 0) {
		safepos += -conf->min_offset_diff;
		readpos += -conf->min_offset_diff;
	} else
		writepos += conf->min_offset_diff;

	if ((mddev->reshape_backwards
	     ? (safepos > writepos && readpos < writepos)
	     : (safepos < writepos && readpos > writepos)) ||
	    time_after(jiffies, conf->reshape_checkpoint + 10*HZ)) {
		/* Cannot proceed until we've updated the superblock... */
		wait_event(conf->wait_for_reshape,
			   atomic_read(&conf->reshape_stripes)==0
			   || test_bit(MD_RECOVERY_INTR, &mddev->recovery));
		if (atomic_read(&conf->reshape_stripes) != 0)
			return 0;
		mddev->reshape_position = conf->reshape_progress;
		mddev->curr_resync_completed = sector_nr;
		if (!mddev->reshape_backwards)
			/* Can update recovery_offset */
			rdev_for_each(rdev, mddev)
				if (rdev->raid_disk >= 0 &&
				    !test_bit(Journal, &rdev->flags) &&
				    !test_bit(In_sync, &rdev->flags) &&
				    rdev->recovery_offset < sector_nr)
					rdev->recovery_offset = sector_nr;

		conf->reshape_checkpoint = jiffies;
		set_bit(MD_SB_CHANGE_DEVS, &mddev->sb_flags);
		md_wakeup_thread(mddev->thread);
		wait_event(mddev->sb_wait, mddev->sb_flags == 0 ||
			   test_bit(MD_RECOVERY_INTR, &mddev->recovery));
		if (test_bit(MD_RECOVERY_INTR, &mddev->recovery))
			return 0;
		spin_lock_irq(&conf->device_lock);
		conf->reshape_safe = mddev->reshape_position;
		spin_unlock_irq(&conf->device_lock);
		wake_up(&conf->wait_for_reshape);
		sysfs_notify_dirent_safe(mddev->sysfs_completed);
	}

	INIT_LIST_HEAD(&stripes);
	for (i = 0; i < reshape_sectors; i += RAID5_STRIPE_SECTORS(conf)) {
		int j;
		int skipped_disk = 0;
		sh = raid5_get_active_stripe(conf, NULL, stripe_addr+i,
					     R5_GAS_NOQUIESCE);
		set_bit(STRIPE_EXPANDING, &sh->state);
		atomic_inc(&conf->reshape_stripes);
		/* If any of this stripe is beyond the end of the old
		 * array, then we need to zero those blocks
		 */
		for (j=sh->disks; j--;) {
			sector_t s;
			if (j == sh->pd_idx)
				continue;
			if (conf->level == 6 &&
			    j == sh->qd_idx)
				continue;
			s = raid5_compute_blocknr(sh, j, 0);
			if (s < raid5_size(mddev, 0, 0)) {
				skipped_disk = 1;
				continue;
			}
			memset(page_address(sh->dev[j].page), 0, RAID5_STRIPE_SIZE(conf));
			set_bit(R5_Expanded, &sh->dev[j].flags);
			set_bit(R5_UPTODATE, &sh->dev[j].flags);
		}
		if (!skipped_disk) {
			set_bit(STRIPE_EXPAND_READY, &sh->state);
			set_bit(STRIPE_HANDLE, &sh->state);
		}
		list_add(&sh->lru, &stripes);
	}
	spin_lock_irq(&conf->device_lock);
	if (mddev->reshape_backwards)
		conf->reshape_progress -= reshape_sectors * new_data_disks;
	else
		conf->reshape_progress += reshape_sectors * new_data_disks;
	spin_unlock_irq(&conf->device_lock);
	/* Ok, those stripe are ready. We can start scheduling
	 * reads on the source stripes.
	 * The source stripes are determined by mapping the first and last
	 * block on the destination stripes.
	 */
	first_sector =
		raid5_compute_sector(conf, stripe_addr*(new_data_disks),
				     1, &dd_idx, NULL);
	last_sector =
		raid5_compute_sector(conf, ((stripe_addr+reshape_sectors)
					    * new_data_disks - 1),
				     1, &dd_idx, NULL);
	if (last_sector >= mddev->dev_sectors)
		last_sector = mddev->dev_sectors - 1;
	while (first_sector <= last_sector) {
		sh = raid5_get_active_stripe(conf, NULL, first_sector,
				R5_GAS_PREVIOUS | R5_GAS_NOQUIESCE);
		set_bit(STRIPE_EXPAND_SOURCE, &sh->state);
		set_bit(STRIPE_HANDLE, &sh->state);
		raid5_release_stripe(sh);
		first_sector += RAID5_STRIPE_SECTORS(conf);
	}
	/* Now that the sources are clearly marked, we can release
	 * the destination stripes
	 */
	while (!list_empty(&stripes)) {
		sh = list_entry(stripes.next, struct stripe_head, lru);
		list_del_init(&sh->lru);
		raid5_release_stripe(sh);
	}
	/* If this takes us to the resync_max point where we have to pause,
	 * then we need to write out the superblock.
	 */
	sector_nr += reshape_sectors;
	retn = reshape_sectors;
finish:
	if (mddev->curr_resync_completed > mddev->resync_max ||
	    (sector_nr - mddev->curr_resync_completed) * 2
	    >= mddev->resync_max - mddev->curr_resync_completed) {
		/* Cannot proceed until we've updated the superblock... */
		wait_event(conf->wait_for_reshape,
			   atomic_read(&conf->reshape_stripes) == 0
			   || test_bit(MD_RECOVERY_INTR, &mddev->recovery));
		if (atomic_read(&conf->reshape_stripes) != 0)
			goto ret;
		mddev->reshape_position = conf->reshape_progress;
		mddev->curr_resync_completed = sector_nr;
		if (!mddev->reshape_backwards)
			/* Can update recovery_offset */
			rdev_for_each(rdev, mddev)
				if (rdev->raid_disk >= 0 &&
				    !test_bit(Journal, &rdev->flags) &&
				    !test_bit(In_sync, &rdev->flags) &&
				    rdev->recovery_offset < sector_nr)
					rdev->recovery_offset = sector_nr;
		conf->reshape_checkpoint = jiffies;
		set_bit(MD_SB_CHANGE_DEVS, &mddev->sb_flags);
		md_wakeup_thread(mddev->thread);
		wait_event(mddev->sb_wait,
			   !test_bit(MD_SB_CHANGE_DEVS, &mddev->sb_flags)
			   || test_bit(MD_RECOVERY_INTR, &mddev->recovery));
		if (test_bit(MD_RECOVERY_INTR, &mddev->recovery))
			goto ret;
		spin_lock_irq(&conf->device_lock);
		conf->reshape_safe = mddev->reshape_position;
		spin_unlock_irq(&conf->device_lock);
		wake_up(&conf->wait_for_reshape);
		sysfs_notify_dirent_safe(mddev->sysfs_completed);
	}
ret:
	return retn;
}

static inline sector_t raid5_sync_request(struct mddev *mddev, sector_t sector_nr,
					  sector_t max_sector, int *skipped)
{
	struct r5conf *conf = mddev->private;
	struct stripe_head *sh;
	sector_t sync_blocks;
	bool still_degraded = false;
	int i;

	if (sector_nr >= max_sector) {
		/* just being told to finish up .. nothing much to do */

		if (test_bit(MD_RECOVERY_RESHAPE, &mddev->recovery)) {
			end_reshape(conf);
			return 0;
		}

		if (mddev->curr_resync < max_sector) /* aborted */
			mddev->bitmap_ops->end_sync(mddev, mddev->curr_resync,
						    &sync_blocks);
		else /* completed sync */
			conf->fullsync = 0;
		mddev->bitmap_ops->close_sync(mddev);

		return 0;
	}

	/* Allow raid5_quiesce to complete */
	wait_event(conf->wait_for_reshape, conf->quiesce != 2);

	if (test_bit(MD_RECOVERY_RESHAPE, &mddev->recovery))
		return reshape_request(mddev, sector_nr, skipped);

	/* No need to check resync_max as we never do more than one
	 * stripe, and as resync_max will always be on a chunk boundary,
	 * if the check in md_do_sync didn't fire, there is no chance
	 * of overstepping resync_max here
	 */

	/* if there is too many failed drives and we are trying
	 * to resync, then assert that we are finished, because there is
	 * nothing we can do.
	 */
	if (mddev->degraded >= conf->max_degraded &&
	    test_bit(MD_RECOVERY_SYNC, &mddev->recovery)) {
		sector_t rv = mddev->dev_sectors - sector_nr;
		*skipped = 1;
		return rv;
	}
	if (!test_bit(MD_RECOVERY_REQUESTED, &mddev->recovery) &&
	    !conf->fullsync &&
	    !mddev->bitmap_ops->start_sync(mddev, sector_nr, &sync_blocks,
					   true) &&
	    sync_blocks >= RAID5_STRIPE_SECTORS(conf)) {
		/* we can skip this block, and probably more */
		do_div(sync_blocks, RAID5_STRIPE_SECTORS(conf));
		*skipped = 1;
		/* keep things rounded to whole stripes */
		return sync_blocks * RAID5_STRIPE_SECTORS(conf);
	}

	mddev->bitmap_ops->cond_end_sync(mddev, sector_nr, false);

	sh = raid5_get_active_stripe(conf, NULL, sector_nr,
				     R5_GAS_NOBLOCK);
	if (sh == NULL) {
		sh = raid5_get_active_stripe(conf, NULL, sector_nr, 0);
		/* make sure we don't swamp the stripe cache if someone else
		 * is trying to get access
		 */
		schedule_timeout_uninterruptible(1);
	}
	/* Need to check if array will still be degraded after recovery/resync
	 * Note in case of > 1 drive failures it's possible we're rebuilding
	 * one drive while leaving another faulty drive in array.
	 */
	for (i = 0; i < conf->raid_disks; i++) {
		struct md_rdev *rdev = conf->disks[i].rdev;

		if (rdev == NULL || test_bit(Faulty, &rdev->flags))
			still_degraded = true;
	}

	mddev->bitmap_ops->start_sync(mddev, sector_nr, &sync_blocks,
				      still_degraded);

	set_bit(STRIPE_SYNC_REQUESTED, &sh->state);
	set_bit(STRIPE_HANDLE, &sh->state);

	raid5_release_stripe(sh);

	return RAID5_STRIPE_SECTORS(conf);
}

static int  retry_aligned_read(struct r5conf *conf, struct bio *raid_bio,
			       unsigned int offset)
{
	/* We may not be able to submit a whole bio at once as there
	 * may not be enough stripe_heads available.
	 * We cannot pre-allocate enough stripe_heads as we may need
	 * more than exist in the cache (if we allow ever large chunks).
	 * So we do one stripe head at a time and record in
	 * ->bi_hw_segments how many have been done.
	 *
	 * We *know* that this entire raid_bio is in one chunk, so
	 * it will be only one 'dd_idx' and only need one call to raid5_compute_sector.
	 */
	struct stripe_head *sh;
	int dd_idx;
	sector_t sector, logical_sector, last_sector;
	int scnt = 0;
	int handled = 0;

	logical_sector = raid_bio->bi_iter.bi_sector &
		~((sector_t)RAID5_STRIPE_SECTORS(conf)-1);
	sector = raid5_compute_sector(conf, logical_sector,
				      0, &dd_idx, NULL);
	last_sector = bio_end_sector(raid_bio);

	for (; logical_sector < last_sector;
	     logical_sector += RAID5_STRIPE_SECTORS(conf),
		     sector += RAID5_STRIPE_SECTORS(conf),
		     scnt++) {

		if (scnt < offset)
			/* already done this stripe */
			continue;

		sh = raid5_get_active_stripe(conf, NULL, sector,
				R5_GAS_NOBLOCK | R5_GAS_NOQUIESCE);
		if (!sh) {
			/* failed to get a stripe - must wait */
			conf->retry_read_aligned = raid_bio;
			conf->retry_read_offset = scnt;
			return handled;
		}

		if (!add_stripe_bio(sh, raid_bio, dd_idx, 0, 0)) {
			raid5_release_stripe(sh);
			conf->retry_read_aligned = raid_bio;
			conf->retry_read_offset = scnt;
			return handled;
		}

		set_bit(R5_ReadNoMerge, &sh->dev[dd_idx].flags);
		handle_stripe(sh);
		raid5_release_stripe(sh);
		handled++;
	}

	bio_endio(raid_bio);

	if (atomic_dec_and_test(&conf->active_aligned_reads))
		wake_up(&conf->wait_for_quiescent);
	return handled;
}

static int handle_active_stripes(struct r5conf *conf, int group,
				 struct r5worker *worker,
				 struct list_head *temp_inactive_list)
		__must_hold(&conf->device_lock)
{
	struct stripe_head *batch[MAX_STRIPE_BATCH], *sh;
	int i, batch_size = 0, hash;
	bool release_inactive = false;

	while (batch_size < MAX_STRIPE_BATCH &&
			(sh = __get_priority_stripe(conf, group)) != NULL)
		batch[batch_size++] = sh;

	if (batch_size == 0) {
		for (i = 0; i < NR_STRIPE_HASH_LOCKS; i++)
			if (!list_empty(temp_inactive_list + i))
				break;
		if (i == NR_STRIPE_HASH_LOCKS) {
			spin_unlock_irq(&conf->device_lock);
			log_flush_stripe_to_raid(conf);
			spin_lock_irq(&conf->device_lock);
			return batch_size;
		}
		release_inactive = true;
	}
	spin_unlock_irq(&conf->device_lock);

	release_inactive_stripe_list(conf, temp_inactive_list,
				     NR_STRIPE_HASH_LOCKS);

	r5l_flush_stripe_to_raid(conf->log);
	if (release_inactive) {
		spin_lock_irq(&conf->device_lock);
		return 0;
	}

	for (i = 0; i < batch_size; i++)
		handle_stripe(batch[i]);
	log_write_stripe_run(conf);

	cond_resched();

	spin_lock_irq(&conf->device_lock);
	for (i = 0; i < batch_size; i++) {
		hash = batch[i]->hash_lock_index;
		__release_stripe(conf, batch[i], &temp_inactive_list[hash]);
	}
	return batch_size;
}

static void raid5_do_work(struct work_struct *work)
{
	struct r5worker *worker = container_of(work, struct r5worker, work);
	struct r5worker_group *group = worker->group;
	struct r5conf *conf = group->conf;
	struct mddev *mddev = conf->mddev;
	int group_id = group - conf->worker_groups;
	int handled;
	struct blk_plug plug;

	pr_debug("+++ raid5worker active\n");

	blk_start_plug(&plug);
	handled = 0;
	spin_lock_irq(&conf->device_lock);
	while (1) {
		int batch_size, released;

		released = release_stripe_list(conf, worker->temp_inactive_list);

		batch_size = handle_active_stripes(conf, group_id, worker,
						   worker->temp_inactive_list);
		worker->working = false;
		if (!batch_size && !released)
			break;
		handled += batch_size;
		wait_event_lock_irq(mddev->sb_wait,
			!test_bit(MD_SB_CHANGE_PENDING, &mddev->sb_flags),
			conf->device_lock);
	}
	pr_debug("%d stripes handled\n", handled);

	spin_unlock_irq(&conf->device_lock);

	flush_deferred_bios(conf);

	r5l_flush_stripe_to_raid(conf->log);

	async_tx_issue_pending_all();
	blk_finish_plug(&plug);

	pr_debug("--- raid5worker inactive\n");
}

/*
 * This is our raid5 kernel thread.
 *
 * We scan the hash table for stripes which can be handled now.
 * During the scan, completed stripes are saved for us by the interrupt
 * handler, so that they will not have to wait for our next wakeup.
 */
static void raid5d(struct md_thread *thread)
{
	struct mddev *mddev = thread->mddev;
	struct r5conf *conf = mddev->private;
	int handled;
	struct blk_plug plug;

	pr_debug("+++ raid5d active\n");

	md_check_recovery(mddev);

	blk_start_plug(&plug);
	handled = 0;
	spin_lock_irq(&conf->device_lock);
	while (1) {
		struct bio *bio;
		int batch_size, released;
		unsigned int offset;

		if (test_bit(MD_SB_CHANGE_PENDING, &mddev->sb_flags))
			break;

		released = release_stripe_list(conf, conf->temp_inactive_list);
		if (released)
			clear_bit(R5_DID_ALLOC, &conf->cache_state);

		if (
		    !list_empty(&conf->bitmap_list)) {
			/* Now is a good time to flush some bitmap updates */
			conf->seq_flush++;
			spin_unlock_irq(&conf->device_lock);
			mddev->bitmap_ops->unplug(mddev, true);
			spin_lock_irq(&conf->device_lock);
			conf->seq_write = conf->seq_flush;
			activate_bit_delay(conf, conf->temp_inactive_list);
		}
		raid5_activate_delayed(conf);

		while ((bio = remove_bio_from_retry(conf, &offset))) {
			int ok;
			spin_unlock_irq(&conf->device_lock);
			ok = retry_aligned_read(conf, bio, offset);
			spin_lock_irq(&conf->device_lock);
			if (!ok)
				break;
			handled++;
		}

		batch_size = handle_active_stripes(conf, ANY_GROUP, NULL,
						   conf->temp_inactive_list);
		if (!batch_size && !released)
			break;
		handled += batch_size;

		if (mddev->sb_flags & ~(1 << MD_SB_CHANGE_PENDING)) {
			spin_unlock_irq(&conf->device_lock);
			md_check_recovery(mddev);
			spin_lock_irq(&conf->device_lock);
		}
	}
	pr_debug("%d stripes handled\n", handled);

	spin_unlock_irq(&conf->device_lock);
	if (test_and_clear_bit(R5_ALLOC_MORE, &conf->cache_state) &&
	    mutex_trylock(&conf->cache_size_mutex)) {
		grow_one_stripe(conf, __GFP_NOWARN);
		/* Set flag even if allocation failed.  This helps
		 * slow down allocation requests when mem is short
		 */
		set_bit(R5_DID_ALLOC, &conf->cache_state);
		mutex_unlock(&conf->cache_size_mutex);
	}

	flush_deferred_bios(conf);

	r5l_flush_stripe_to_raid(conf->log);

	async_tx_issue_pending_all();
	blk_finish_plug(&plug);

	pr_debug("--- raid5d inactive\n");
}

static ssize_t
raid5_show_stripe_cache_size(struct mddev *mddev, char *page)
{
	struct r5conf *conf;
	int ret = 0;
	spin_lock(&mddev->lock);
	conf = mddev->private;
	if (conf)
		ret = sprintf(page, "%d\n", conf->min_nr_stripes);
	spin_unlock(&mddev->lock);
	return ret;
}

int
raid5_set_cache_size(struct mddev *mddev, int size)
{
	int result = 0;
	struct r5conf *conf = mddev->private;

	if (size <= 16 || size > 32768)
		return -EINVAL;

	WRITE_ONCE(conf->min_nr_stripes, size);
	mutex_lock(&conf->cache_size_mutex);
	while (size < conf->max_nr_stripes &&
	       drop_one_stripe(conf))
		;
	mutex_unlock(&conf->cache_size_mutex);

	md_allow_write(mddev);

	mutex_lock(&conf->cache_size_mutex);
	while (size > conf->max_nr_stripes)
		if (!grow_one_stripe(conf, GFP_KERNEL)) {
			WRITE_ONCE(conf->min_nr_stripes, conf->max_nr_stripes);
			result = -ENOMEM;
			break;
		}
	mutex_unlock(&conf->cache_size_mutex);

	return result;
}
EXPORT_SYMBOL(raid5_set_cache_size);

static ssize_t
raid5_store_stripe_cache_size(struct mddev *mddev, const char *page, size_t len)
{
	struct r5conf *conf;
	unsigned long new;
	int err;

	if (len >= PAGE_SIZE)
		return -EINVAL;
	if (kstrtoul(page, 10, &new))
		return -EINVAL;
	err = mddev_lock(mddev);
	if (err)
		return err;
	conf = mddev->private;
	if (!conf)
		err = -ENODEV;
	else
		err = raid5_set_cache_size(mddev, new);
	mddev_unlock(mddev);

	return err ?: len;
}

static struct md_sysfs_entry
raid5_stripecache_size = __ATTR(stripe_cache_size, S_IRUGO | S_IWUSR,
				raid5_show_stripe_cache_size,
				raid5_store_stripe_cache_size);

static ssize_t
raid5_show_rmw_level(struct mddev  *mddev, char *page)
{
	struct r5conf *conf = mddev->private;
	if (conf)
		return sprintf(page, "%d\n", conf->rmw_level);
	else
		return 0;
}

static ssize_t
raid5_store_rmw_level(struct mddev  *mddev, const char *page, size_t len)
{
	struct r5conf *conf = mddev->private;
	unsigned long new;

	if (!conf)
		return -ENODEV;

	if (len >= PAGE_SIZE)
		return -EINVAL;

	if (kstrtoul(page, 10, &new))
		return -EINVAL;

	if (new != PARITY_DISABLE_RMW && !raid6_call.xor_syndrome)
		return -EINVAL;

	if (new != PARITY_DISABLE_RMW &&
	    new != PARITY_ENABLE_RMW &&
	    new != PARITY_PREFER_RMW)
		return -EINVAL;

	conf->rmw_level = new;
	return len;
}

static struct md_sysfs_entry
raid5_rmw_level = __ATTR(rmw_level, S_IRUGO | S_IWUSR,
			 raid5_show_rmw_level,
			 raid5_store_rmw_level);

static ssize_t
raid5_show_stripe_size(struct mddev  *mddev, char *page)
{
	struct r5conf *conf;
	int ret = 0;

	spin_lock(&mddev->lock);
	conf = mddev->private;
	if (conf)
		ret = sprintf(page, "%lu\n", RAID5_STRIPE_SIZE(conf));
	spin_unlock(&mddev->lock);
	return ret;
}

#if PAGE_SIZE != DEFAULT_STRIPE_SIZE
static ssize_t
raid5_store_stripe_size(struct mddev  *mddev, const char *page, size_t len)
{
	struct r5conf *conf;
	unsigned long new;
	int err;
	int size;

	if (len >= PAGE_SIZE)
		return -EINVAL;
	if (kstrtoul(page, 10, &new))
		return -EINVAL;

	/*
	 * The value should not be bigger than PAGE_SIZE. It requires to
	 * be multiple of DEFAULT_STRIPE_SIZE and the value should be power
	 * of two.
	 */
	if (new % DEFAULT_STRIPE_SIZE != 0 ||
			new > PAGE_SIZE || new == 0 ||
			new != roundup_pow_of_two(new))
		return -EINVAL;

	err = mddev_suspend_and_lock(mddev);
	if (err)
		return err;

	conf = mddev->private;
	if (!conf) {
		err = -ENODEV;
		goto out_unlock;
	}

	if (new == conf->stripe_size)
		goto out_unlock;

	pr_debug("md/raid: change stripe_size from %lu to %lu\n",
			conf->stripe_size, new);

	if (test_bit(MD_RECOVERY_RUNNING, &mddev->recovery) ||
	    mddev->reshape_position != MaxSector || mddev->sysfs_active) {
		err = -EBUSY;
		goto out_unlock;
	}

	mutex_lock(&conf->cache_size_mutex);
	size = conf->max_nr_stripes;

	shrink_stripes(conf);

	conf->stripe_size = new;
	conf->stripe_shift = ilog2(new) - 9;
	conf->stripe_sectors = new >> 9;
	if (grow_stripes(conf, size)) {
		pr_warn("md/raid:%s: couldn't allocate buffers\n",
				mdname(mddev));
		err = -ENOMEM;
	}
	mutex_unlock(&conf->cache_size_mutex);

out_unlock:
	mddev_unlock_and_resume(mddev);
	return err ?: len;
}

static struct md_sysfs_entry
raid5_stripe_size = __ATTR(stripe_size, 0644,
			 raid5_show_stripe_size,
			 raid5_store_stripe_size);
#else
static struct md_sysfs_entry
raid5_stripe_size = __ATTR(stripe_size, 0444,
			 raid5_show_stripe_size,
			 NULL);
#endif

static ssize_t
raid5_show_preread_threshold(struct mddev *mddev, char *page)
{
	struct r5conf *conf;
	int ret = 0;
	spin_lock(&mddev->lock);
	conf = mddev->private;
	if (conf)
		ret = sprintf(page, "%d\n", conf->bypass_threshold);
	spin_unlock(&mddev->lock);
	return ret;
}

static ssize_t
raid5_store_preread_threshold(struct mddev *mddev, const char *page, size_t len)
{
	struct r5conf *conf;
	unsigned long new;
	int err;

	if (len >= PAGE_SIZE)
		return -EINVAL;
	if (kstrtoul(page, 10, &new))
		return -EINVAL;

	err = mddev_lock(mddev);
	if (err)
		return err;
	conf = mddev->private;
	if (!conf)
		err = -ENODEV;
	else if (new > conf->min_nr_stripes)
		err = -EINVAL;
	else
		conf->bypass_threshold = new;
	mddev_unlock(mddev);
	return err ?: len;
}

static struct md_sysfs_entry
raid5_preread_bypass_threshold = __ATTR(preread_bypass_threshold,
					S_IRUGO | S_IWUSR,
					raid5_show_preread_threshold,
					raid5_store_preread_threshold);

static ssize_t
raid5_show_skip_copy(struct mddev *mddev, char *page)
{
	struct r5conf *conf;
	int ret = 0;
	spin_lock(&mddev->lock);
	conf = mddev->private;
	if (conf)
		ret = sprintf(page, "%d\n", conf->skip_copy);
	spin_unlock(&mddev->lock);
	return ret;
}

static ssize_t
raid5_store_skip_copy(struct mddev *mddev, const char *page, size_t len)
{
	struct r5conf *conf;
	unsigned long new;
	int err;

	if (len >= PAGE_SIZE)
		return -EINVAL;
	if (kstrtoul(page, 10, &new))
		return -EINVAL;
	new = !!new;

	err = mddev_suspend_and_lock(mddev);
	if (err)
		return err;
	conf = mddev->private;
	if (!conf)
		err = -ENODEV;
	else if (new != conf->skip_copy) {
		struct request_queue *q = mddev->gendisk->queue;
		struct queue_limits lim = queue_limits_start_update(q);

		conf->skip_copy = new;
		if (new)
			lim.features |= BLK_FEAT_STABLE_WRITES;
		else
			lim.features &= ~BLK_FEAT_STABLE_WRITES;
		err = queue_limits_commit_update(q, &lim);
	}
	mddev_unlock_and_resume(mddev);
	return err ?: len;
}

static struct md_sysfs_entry
raid5_skip_copy = __ATTR(skip_copy, S_IRUGO | S_IWUSR,
					raid5_show_skip_copy,
					raid5_store_skip_copy);

static ssize_t
stripe_cache_active_show(struct mddev *mddev, char *page)
{
	struct r5conf *conf = mddev->private;
	if (conf)
		return sprintf(page, "%d\n", atomic_read(&conf->active_stripes));
	else
		return 0;
}

static struct md_sysfs_entry
raid5_stripecache_active = __ATTR_RO(stripe_cache_active);

static ssize_t
raid5_show_group_thread_cnt(struct mddev *mddev, char *page)
{
	struct r5conf *conf;
	int ret = 0;
	spin_lock(&mddev->lock);
	conf = mddev->private;
	if (conf)
		ret = sprintf(page, "%d\n", conf->worker_cnt_per_group);
	spin_unlock(&mddev->lock);
	return ret;
}

static int alloc_thread_groups(struct r5conf *conf, int cnt,
			       int *group_cnt,
			       struct r5worker_group **worker_groups);
static ssize_t
raid5_store_group_thread_cnt(struct mddev *mddev, const char *page, size_t len)
{
	struct r5conf *conf;
	unsigned int new;
	int err;
	struct r5worker_group *new_groups, *old_groups;
	int group_cnt;

	if (len >= PAGE_SIZE)
		return -EINVAL;
	if (kstrtouint(page, 10, &new))
		return -EINVAL;
	/* 8192 should be big enough */
	if (new > 8192)
		return -EINVAL;

	err = mddev_suspend_and_lock(mddev);
	if (err)
		return err;
	raid5_quiesce(mddev, true);

	conf = mddev->private;
	if (!conf)
		err = -ENODEV;
	else if (new != conf->worker_cnt_per_group) {
		old_groups = conf->worker_groups;
		if (old_groups)
			flush_workqueue(raid5_wq);

		err = alloc_thread_groups(conf, new, &group_cnt, &new_groups);
		if (!err) {
			spin_lock_irq(&conf->device_lock);
			conf->group_cnt = group_cnt;
			conf->worker_cnt_per_group = new;
			conf->worker_groups = new_groups;
			spin_unlock_irq(&conf->device_lock);

			if (old_groups)
				kfree(old_groups[0].workers);
			kfree(old_groups);
		}
	}

	raid5_quiesce(mddev, false);
	mddev_unlock_and_resume(mddev);

	return err ?: len;
}

static struct md_sysfs_entry
raid5_group_thread_cnt = __ATTR(group_thread_cnt, S_IRUGO | S_IWUSR,
				raid5_show_group_thread_cnt,
				raid5_store_group_thread_cnt);

static struct attribute *raid5_attrs[] =  {
	&raid5_stripecache_size.attr,
	&raid5_stripecache_active.attr,
	&raid5_preread_bypass_threshold.attr,
	&raid5_group_thread_cnt.attr,
	&raid5_skip_copy.attr,
	&raid5_rmw_level.attr,
	&raid5_stripe_size.attr,
	&r5c_journal_mode.attr,
	&ppl_write_hint.attr,
	NULL,
};
static const struct attribute_group raid5_attrs_group = {
	.name = NULL,
	.attrs = raid5_attrs,
};

static int alloc_thread_groups(struct r5conf *conf, int cnt, int *group_cnt,
			       struct r5worker_group **worker_groups)
{
	int i, j, k;
	ssize_t size;
	struct r5worker *workers;

	if (cnt == 0) {
		*group_cnt = 0;
		*worker_groups = NULL;
		return 0;
	}
	*group_cnt = num_possible_nodes();
	size = sizeof(struct r5worker) * cnt;
	workers = kcalloc(size, *group_cnt, GFP_NOIO);
	*worker_groups = kcalloc(*group_cnt, sizeof(struct r5worker_group),
				 GFP_NOIO);
	if (!*worker_groups || !workers) {
		kfree(workers);
		kfree(*worker_groups);
		return -ENOMEM;
	}

	for (i = 0; i < *group_cnt; i++) {
		struct r5worker_group *group;

		group = &(*worker_groups)[i];
		INIT_LIST_HEAD(&group->handle_list);
		INIT_LIST_HEAD(&group->loprio_list);
		group->conf = conf;
		group->workers = workers + i * cnt;

		for (j = 0; j < cnt; j++) {
			struct r5worker *worker = group->workers + j;
			worker->group = group;
			INIT_WORK(&worker->work, raid5_do_work);

			for (k = 0; k < NR_STRIPE_HASH_LOCKS; k++)
				INIT_LIST_HEAD(worker->temp_inactive_list + k);
		}
	}

	return 0;
}

static void free_thread_groups(struct r5conf *conf)
{
	if (conf->worker_groups)
		kfree(conf->worker_groups[0].workers);
	kfree(conf->worker_groups);
	conf->worker_groups = NULL;
}

static sector_t
raid5_size(struct mddev *mddev, sector_t sectors, int raid_disks)
{
	struct r5conf *conf = mddev->private;

	if (!sectors)
		sectors = mddev->dev_sectors;
	if (!raid_disks)
		/* size is defined by the smallest of previous and new size */
		raid_disks = min(conf->raid_disks, conf->previous_raid_disks);

	sectors &= ~((sector_t)conf->chunk_sectors - 1);
	sectors &= ~((sector_t)conf->prev_chunk_sectors - 1);
	return sectors * (raid_disks - conf->max_degraded);
}

static void free_scratch_buffer(struct r5conf *conf, struct raid5_percpu *percpu)
{
	safe_put_page(percpu->spare_page);
	percpu->spare_page = NULL;
	kvfree(percpu->scribble);
	percpu->scribble = NULL;
}

static int alloc_scratch_buffer(struct r5conf *conf, struct raid5_percpu *percpu)
{
	if (conf->level == 6 && !percpu->spare_page) {
		percpu->spare_page = alloc_page(GFP_KERNEL);
		if (!percpu->spare_page)
			return -ENOMEM;
	}

	if (scribble_alloc(percpu,
			   max(conf->raid_disks,
			       conf->previous_raid_disks),
			   max(conf->chunk_sectors,
			       conf->prev_chunk_sectors)
			   / RAID5_STRIPE_SECTORS(conf))) {
		free_scratch_buffer(conf, percpu);
		return -ENOMEM;
	}

	local_lock_init(&percpu->lock);
	return 0;
}

static int raid456_cpu_dead(unsigned int cpu, struct hlist_node *node)
{
	struct r5conf *conf = hlist_entry_safe(node, struct r5conf, node);

	free_scratch_buffer(conf, per_cpu_ptr(conf->percpu, cpu));
	return 0;
}

static void raid5_free_percpu(struct r5conf *conf)
{
	if (!conf->percpu)
		return;

	cpuhp_state_remove_instance(CPUHP_MD_RAID5_PREPARE, &conf->node);
	free_percpu(conf->percpu);
}

static void free_conf(struct r5conf *conf)
{
	int i;

	log_exit(conf);

	shrinker_free(conf->shrinker);
	free_thread_groups(conf);
	shrink_stripes(conf);
	raid5_free_percpu(conf);
	for (i = 0; i < conf->pool_size; i++)
		if (conf->disks[i].extra_page)
			put_page(conf->disks[i].extra_page);
	kfree(conf->disks);
	bioset_exit(&conf->bio_split);
	kfree(conf->stripe_hashtbl);
	kfree(conf->pending_data);
	kfree(conf);
}

static int raid456_cpu_up_prepare(unsigned int cpu, struct hlist_node *node)
{
	struct r5conf *conf = hlist_entry_safe(node, struct r5conf, node);
	struct raid5_percpu *percpu = per_cpu_ptr(conf->percpu, cpu);

	if (alloc_scratch_buffer(conf, percpu)) {
		pr_warn("%s: failed memory allocation for cpu%u\n",
			__func__, cpu);
		return -ENOMEM;
	}
	return 0;
}

static int raid5_alloc_percpu(struct r5conf *conf)
{
	int err = 0;

	conf->percpu = alloc_percpu(struct raid5_percpu);
	if (!conf->percpu)
		return -ENOMEM;

	err = cpuhp_state_add_instance(CPUHP_MD_RAID5_PREPARE, &conf->node);
	if (!err) {
		conf->scribble_disks = max(conf->raid_disks,
			conf->previous_raid_disks);
		conf->scribble_sectors = max(conf->chunk_sectors,
			conf->prev_chunk_sectors);
	}
	return err;
}

static unsigned long raid5_cache_scan(struct shrinker *shrink,
				      struct shrink_control *sc)
{
	struct r5conf *conf = shrink->private_data;
	unsigned long ret = SHRINK_STOP;

	if (mutex_trylock(&conf->cache_size_mutex)) {
		ret= 0;
		while (ret < sc->nr_to_scan &&
		       conf->max_nr_stripes > conf->min_nr_stripes) {
			if (drop_one_stripe(conf) == 0) {
				ret = SHRINK_STOP;
				break;
			}
			ret++;
		}
		mutex_unlock(&conf->cache_size_mutex);
	}
	return ret;
}

static unsigned long raid5_cache_count(struct shrinker *shrink,
				       struct shrink_control *sc)
{
	struct r5conf *conf = shrink->private_data;
	int max_stripes = READ_ONCE(conf->max_nr_stripes);
	int min_stripes = READ_ONCE(conf->min_nr_stripes);

	if (max_stripes < min_stripes)
		/* unlikely, but not impossible */
		return 0;
	return max_stripes - min_stripes;
}

static struct r5conf *setup_conf(struct mddev *mddev)
{
	struct r5conf *conf;
	int raid_disk, memory, max_disks;
	struct md_rdev *rdev;
	struct disk_info *disk;
	char pers_name[6];
	int i;
	int group_cnt;
	struct r5worker_group *new_group;
	int ret = -ENOMEM;

	if (mddev->new_level != 5
	    && mddev->new_level != 4
	    && mddev->new_level != 6) {
		pr_warn("md/raid:%s: raid level not set to 4/5/6 (%d)\n",
			mdname(mddev), mddev->new_level);
		return ERR_PTR(-EIO);
	}
	if ((mddev->new_level == 5
	     && !algorithm_valid_raid5(mddev->new_layout)) ||
	    (mddev->new_level == 6
	     && !algorithm_valid_raid6(mddev->new_layout))) {
		pr_warn("md/raid:%s: layout %d not supported\n",
			mdname(mddev), mddev->new_layout);
		return ERR_PTR(-EIO);
	}
	if (mddev->new_level == 6 && mddev->raid_disks < 4) {
		pr_warn("md/raid:%s: not enough configured devices (%d, minimum 4)\n",
			mdname(mddev), mddev->raid_disks);
		return ERR_PTR(-EINVAL);
	}

	if (!mddev->new_chunk_sectors ||
	    (mddev->new_chunk_sectors << 9) % PAGE_SIZE ||
	    !is_power_of_2(mddev->new_chunk_sectors)) {
		pr_warn("md/raid:%s: invalid chunk size %d\n",
			mdname(mddev), mddev->new_chunk_sectors << 9);
		return ERR_PTR(-EINVAL);
	}

	conf = kzalloc(sizeof(struct r5conf), GFP_KERNEL);
	if (conf == NULL)
		goto abort;

#if PAGE_SIZE != DEFAULT_STRIPE_SIZE
	conf->stripe_size = DEFAULT_STRIPE_SIZE;
	conf->stripe_shift = ilog2(DEFAULT_STRIPE_SIZE) - 9;
	conf->stripe_sectors = DEFAULT_STRIPE_SIZE >> 9;
#endif
	INIT_LIST_HEAD(&conf->free_list);
	INIT_LIST_HEAD(&conf->pending_list);
	conf->pending_data = kcalloc(PENDING_IO_MAX,
				     sizeof(struct r5pending_data),
				     GFP_KERNEL);
	if (!conf->pending_data)
		goto abort;
	for (i = 0; i < PENDING_IO_MAX; i++)
		list_add(&conf->pending_data[i].sibling, &conf->free_list);
	/* Don't enable multi-threading by default*/
	if (!alloc_thread_groups(conf, 0, &group_cnt, &new_group)) {
		conf->group_cnt = group_cnt;
		conf->worker_cnt_per_group = 0;
		conf->worker_groups = new_group;
	} else
		goto abort;
	spin_lock_init(&conf->device_lock);
	seqcount_spinlock_init(&conf->gen_lock, &conf->device_lock);
	mutex_init(&conf->cache_size_mutex);

	init_waitqueue_head(&conf->wait_for_quiescent);
	init_waitqueue_head(&conf->wait_for_stripe);
	init_waitqueue_head(&conf->wait_for_reshape);
	INIT_LIST_HEAD(&conf->handle_list);
	INIT_LIST_HEAD(&conf->loprio_list);
	INIT_LIST_HEAD(&conf->hold_list);
	INIT_LIST_HEAD(&conf->delayed_list);
	INIT_LIST_HEAD(&conf->bitmap_list);
	init_llist_head(&conf->released_stripes);
	atomic_set(&conf->active_stripes, 0);
	atomic_set(&conf->preread_active_stripes, 0);
	atomic_set(&conf->active_aligned_reads, 0);
	spin_lock_init(&conf->pending_bios_lock);
	conf->batch_bio_dispatch = true;
	rdev_for_each(rdev, mddev) {
		if (test_bit(Journal, &rdev->flags))
			continue;
		if (bdev_nonrot(rdev->bdev)) {
			conf->batch_bio_dispatch = false;
			break;
		}
	}

	conf->bypass_threshold = BYPASS_THRESHOLD;
	conf->recovery_disabled = mddev->recovery_disabled - 1;

	conf->raid_disks = mddev->raid_disks;
	if (mddev->reshape_position == MaxSector)
		conf->previous_raid_disks = mddev->raid_disks;
	else
		conf->previous_raid_disks = mddev->raid_disks - mddev->delta_disks;
	max_disks = max(conf->raid_disks, conf->previous_raid_disks);

	conf->disks = kcalloc(max_disks, sizeof(struct disk_info),
			      GFP_KERNEL);

	if (!conf->disks)
		goto abort;

	for (i = 0; i < max_disks; i++) {
		conf->disks[i].extra_page = alloc_page(GFP_KERNEL);
		if (!conf->disks[i].extra_page)
			goto abort;
	}

	ret = bioset_init(&conf->bio_split, BIO_POOL_SIZE, 0, 0);
	if (ret)
		goto abort;
	conf->mddev = mddev;

	ret = -ENOMEM;
	conf->stripe_hashtbl = kzalloc(PAGE_SIZE, GFP_KERNEL);
	if (!conf->stripe_hashtbl)
		goto abort;

	/* We init hash_locks[0] separately to that it can be used
	 * as the reference lock in the spin_lock_nest_lock() call
	 * in lock_all_device_hash_locks_irq in order to convince
	 * lockdep that we know what we are doing.
	 */
	spin_lock_init(conf->hash_locks);
	for (i = 1; i < NR_STRIPE_HASH_LOCKS; i++)
		spin_lock_init(conf->hash_locks + i);

	for (i = 0; i < NR_STRIPE_HASH_LOCKS; i++)
		INIT_LIST_HEAD(conf->inactive_list + i);

	for (i = 0; i < NR_STRIPE_HASH_LOCKS; i++)
		INIT_LIST_HEAD(conf->temp_inactive_list + i);

	atomic_set(&conf->r5c_cached_full_stripes, 0);
	INIT_LIST_HEAD(&conf->r5c_full_stripe_list);
	atomic_set(&conf->r5c_cached_partial_stripes, 0);
	INIT_LIST_HEAD(&conf->r5c_partial_stripe_list);
	atomic_set(&conf->r5c_flushing_full_stripes, 0);
	atomic_set(&conf->r5c_flushing_partial_stripes, 0);

	conf->level = mddev->new_level;
	conf->chunk_sectors = mddev->new_chunk_sectors;
	ret = raid5_alloc_percpu(conf);
	if (ret)
		goto abort;

	pr_debug("raid456: run(%s) called.\n", mdname(mddev));

	ret = -EIO;
	rdev_for_each(rdev, mddev) {
		raid_disk = rdev->raid_disk;
		if (raid_disk >= max_disks
		    || raid_disk < 0 || test_bit(Journal, &rdev->flags))
			continue;
		disk = conf->disks + raid_disk;

		if (test_bit(Replacement, &rdev->flags)) {
			if (disk->replacement)
				goto abort;
			disk->replacement = rdev;
		} else {
			if (disk->rdev)
				goto abort;
			disk->rdev = rdev;
		}

		if (test_bit(In_sync, &rdev->flags)) {
			pr_info("md/raid:%s: device %pg operational as raid disk %d\n",
				mdname(mddev), rdev->bdev, raid_disk);
		} else if (rdev->saved_raid_disk != raid_disk)
			/* Cannot rely on bitmap to complete recovery */
			conf->fullsync = 1;
	}

	conf->level = mddev->new_level;
	if (conf->level == 6) {
		conf->max_degraded = 2;
		if (raid6_call.xor_syndrome)
			conf->rmw_level = PARITY_ENABLE_RMW;
		else
			conf->rmw_level = PARITY_DISABLE_RMW;
	} else {
		conf->max_degraded = 1;
		conf->rmw_level = PARITY_ENABLE_RMW;
	}
	conf->algorithm = mddev->new_layout;
	conf->reshape_progress = mddev->reshape_position;
	if (conf->reshape_progress != MaxSector) {
		conf->prev_chunk_sectors = mddev->chunk_sectors;
		conf->prev_algo = mddev->layout;
	} else {
		conf->prev_chunk_sectors = conf->chunk_sectors;
		conf->prev_algo = conf->algorithm;
	}

	conf->min_nr_stripes = NR_STRIPES;
	if (mddev->reshape_position != MaxSector) {
		int stripes = max_t(int,
			((mddev->chunk_sectors << 9) / RAID5_STRIPE_SIZE(conf)) * 4,
			((mddev->new_chunk_sectors << 9) / RAID5_STRIPE_SIZE(conf)) * 4);
		conf->min_nr_stripes = max(NR_STRIPES, stripes);
		if (conf->min_nr_stripes != NR_STRIPES)
			pr_info("md/raid:%s: force stripe size %d for reshape\n",
				mdname(mddev), conf->min_nr_stripes);
	}
	memory = conf->min_nr_stripes * (sizeof(struct stripe_head) +
		 max_disks * ((sizeof(struct bio) + PAGE_SIZE))) / 1024;
	atomic_set(&conf->empty_inactive_list_nr, NR_STRIPE_HASH_LOCKS);
	if (grow_stripes(conf, conf->min_nr_stripes)) {
		pr_warn("md/raid:%s: couldn't allocate %dkB for buffers\n",
			mdname(mddev), memory);
		ret = -ENOMEM;
		goto abort;
	} else
		pr_debug("md/raid:%s: allocated %dkB\n", mdname(mddev), memory);
	/*
	 * Losing a stripe head costs more than the time to refill it,
	 * it reduces the queue depth and so can hurt throughput.
	 * So set it rather large, scaled by number of devices.
	 */
	conf->shrinker = shrinker_alloc(0, "md-raid5:%s", mdname(mddev));
	if (!conf->shrinker) {
		ret = -ENOMEM;
		pr_warn("md/raid:%s: couldn't allocate shrinker.\n",
			mdname(mddev));
		goto abort;
	}

	conf->shrinker->seeks = DEFAULT_SEEKS * conf->raid_disks * 4;
	conf->shrinker->scan_objects = raid5_cache_scan;
	conf->shrinker->count_objects = raid5_cache_count;
	conf->shrinker->batch = 128;
	conf->shrinker->private_data = conf;

	shrinker_register(conf->shrinker);

	sprintf(pers_name, "raid%d", mddev->new_level);
	rcu_assign_pointer(conf->thread,
			   md_register_thread(raid5d, mddev, pers_name));
	if (!conf->thread) {
		pr_warn("md/raid:%s: couldn't allocate thread.\n",
			mdname(mddev));
		ret = -ENOMEM;
		goto abort;
	}

	return conf;

 abort:
	if (conf)
		free_conf(conf);
	return ERR_PTR(ret);
}

static int only_parity(int raid_disk, int algo, int raid_disks, int max_degraded)
{
	switch (algo) {
	case ALGORITHM_PARITY_0:
		if (raid_disk < max_degraded)
			return 1;
		break;
	case ALGORITHM_PARITY_N:
		if (raid_disk >= raid_disks - max_degraded)
			return 1;
		break;
	case ALGORITHM_PARITY_0_6:
		if (raid_disk == 0 ||
		    raid_disk == raid_disks - 1)
			return 1;
		break;
	case ALGORITHM_LEFT_ASYMMETRIC_6:
	case ALGORITHM_RIGHT_ASYMMETRIC_6:
	case ALGORITHM_LEFT_SYMMETRIC_6:
	case ALGORITHM_RIGHT_SYMMETRIC_6:
		if (raid_disk == raid_disks - 1)
			return 1;
	}
	return 0;
}

static int raid5_set_limits(struct mddev *mddev)
{
	struct r5conf *conf = mddev->private;
	struct queue_limits lim;
	int data_disks, stripe;
	struct md_rdev *rdev;

	/*
	 * The read-ahead size must cover two whole stripes, which is
	 * 2 * (datadisks) * chunksize where 'n' is the number of raid devices.
	 */
	data_disks = conf->previous_raid_disks - conf->max_degraded;

	/*
	 * We can only discard a whole stripe. It doesn't make sense to
	 * discard data disk but write parity disk
	 */
	stripe = roundup_pow_of_two(data_disks * (mddev->chunk_sectors << 9));

	md_init_stacking_limits(&lim);
	lim.io_min = mddev->chunk_sectors << 9;
	lim.io_opt = lim.io_min * (conf->raid_disks - conf->max_degraded);
	lim.features |= BLK_FEAT_RAID_PARTIAL_STRIPES_EXPENSIVE;
	lim.discard_granularity = stripe;
	lim.max_write_zeroes_sectors = 0;
	mddev_stack_rdev_limits(mddev, &lim, 0);
	rdev_for_each(rdev, mddev)
		queue_limits_stack_bdev(&lim, rdev->bdev, rdev->new_data_offset,
				mddev->gendisk->disk_name);

	/*
	 * Zeroing is required for discard, otherwise data could be lost.
	 *
	 * Consider a scenario: discard a stripe (the stripe could be
	 * inconsistent if discard_zeroes_data is 0); write one disk of the
	 * stripe (the stripe could be inconsistent again depending on which
	 * disks are used to calculate parity); the disk is broken; The stripe
	 * data of this disk is lost.
	 *
	 * We only allow DISCARD if the sysadmin has confirmed that only safe
	 * devices are in use by setting a module parameter.  A better idea
	 * might be to turn DISCARD into WRITE_ZEROES requests, as that is
	 * required to be safe.
	 */
	if (!devices_handle_discard_safely ||
	    lim.max_discard_sectors < (stripe >> 9) ||
	    lim.discard_granularity < stripe)
		lim.max_hw_discard_sectors = 0;

	/*
	 * Requests require having a bitmap for each stripe.
	 * Limit the max sectors based on this.
	 */
	lim.max_hw_sectors = RAID5_MAX_REQ_STRIPES << RAID5_STRIPE_SHIFT(conf);

	/* No restrictions on the number of segments in the request */
	lim.max_segments = USHRT_MAX;

	return queue_limits_set(mddev->gendisk->queue, &lim);
}

static int raid5_run(struct mddev *mddev)
{
	struct r5conf *conf;
	int dirty_parity_disks = 0;
	struct md_rdev *rdev;
	struct md_rdev *journal_dev = NULL;
	sector_t reshape_offset = 0;
	int i;
	long long min_offset_diff = 0;
	int first = 1;
	int ret = -EIO;

	if (mddev->recovery_cp != MaxSector)
		pr_notice("md/raid:%s: not clean -- starting background reconstruction\n",
			  mdname(mddev));

	rdev_for_each(rdev, mddev) {
		long long diff;

		if (test_bit(Journal, &rdev->flags)) {
			journal_dev = rdev;
			continue;
		}
		if (rdev->raid_disk < 0)
			continue;
		diff = (rdev->new_data_offset - rdev->data_offset);
		if (first) {
			min_offset_diff = diff;
			first = 0;
		} else if (mddev->reshape_backwards &&
			 diff < min_offset_diff)
			min_offset_diff = diff;
		else if (!mddev->reshape_backwards &&
			 diff > min_offset_diff)
			min_offset_diff = diff;
	}

	if ((test_bit(MD_HAS_JOURNAL, &mddev->flags) || journal_dev) &&
	    (mddev->bitmap_info.offset || mddev->bitmap_info.file)) {
		pr_notice("md/raid:%s: array cannot have both journal and bitmap\n",
			  mdname(mddev));
		return -EINVAL;
	}

	if (mddev->reshape_position != MaxSector) {
		/* Check that we can continue the reshape.
		 * Difficulties arise if the stripe we would write to
		 * next is at or after the stripe we would read from next.
		 * For a reshape that changes the number of devices, this
		 * is only possible for a very short time, and mdadm makes
		 * sure that time appears to have past before assembling
		 * the array.  So we fail if that time hasn't passed.
		 * For a reshape that keeps the number of devices the same
		 * mdadm must be monitoring the reshape can keeping the
		 * critical areas read-only and backed up.  It will start
		 * the array in read-only mode, so we check for that.
		 */
		sector_t here_new, here_old;
		int old_disks;
		int max_degraded = (mddev->level == 6 ? 2 : 1);
		int chunk_sectors;
		int new_data_disks;

		if (journal_dev) {
			pr_warn("md/raid:%s: don't support reshape with journal - aborting.\n",
				mdname(mddev));
			return -EINVAL;
		}

		if (mddev->new_level != mddev->level) {
			pr_warn("md/raid:%s: unsupported reshape required - aborting.\n",
				mdname(mddev));
			return -EINVAL;
		}
		old_disks = mddev->raid_disks - mddev->delta_disks;
		/* reshape_position must be on a new-stripe boundary, and one
		 * further up in new geometry must map after here in old
		 * geometry.
		 * If the chunk sizes are different, then as we perform reshape
		 * in units of the largest of the two, reshape_position needs
		 * be a multiple of the largest chunk size times new data disks.
		 */
		here_new = mddev->reshape_position;
		chunk_sectors = max(mddev->chunk_sectors, mddev->new_chunk_sectors);
		new_data_disks = mddev->raid_disks - max_degraded;
		if (sector_div(here_new, chunk_sectors * new_data_disks)) {
			pr_warn("md/raid:%s: reshape_position not on a stripe boundary\n",
				mdname(mddev));
			return -EINVAL;
		}
		reshape_offset = here_new * chunk_sectors;
		/* here_new is the stripe we will write to */
		here_old = mddev->reshape_position;
		sector_div(here_old, chunk_sectors * (old_disks-max_degraded));
		/* here_old is the first stripe that we might need to read
		 * from */
		if (mddev->delta_disks == 0) {
			/* We cannot be sure it is safe to start an in-place
			 * reshape.  It is only safe if user-space is monitoring
			 * and taking constant backups.
			 * mdadm always starts a situation like this in
			 * readonly mode so it can take control before
			 * allowing any writes.  So just check for that.
			 */
			if (abs(min_offset_diff) >= mddev->chunk_sectors &&
			    abs(min_offset_diff) >= mddev->new_chunk_sectors)
				/* not really in-place - so OK */;
			else if (mddev->ro == 0) {
				pr_warn("md/raid:%s: in-place reshape must be started in read-only mode - aborting\n",
					mdname(mddev));
				return -EINVAL;
			}
		} else if (mddev->reshape_backwards
		    ? (here_new * chunk_sectors + min_offset_diff <=
		       here_old * chunk_sectors)
		    : (here_new * chunk_sectors >=
		       here_old * chunk_sectors + (-min_offset_diff))) {
			/* Reading from the same stripe as writing to - bad */
			pr_warn("md/raid:%s: reshape_position too early for auto-recovery - aborting.\n",
				mdname(mddev));
			return -EINVAL;
		}
		pr_debug("md/raid:%s: reshape will continue\n", mdname(mddev));
		/* OK, we should be able to continue; */
	} else {
		BUG_ON(mddev->level != mddev->new_level);
		BUG_ON(mddev->layout != mddev->new_layout);
		BUG_ON(mddev->chunk_sectors != mddev->new_chunk_sectors);
		BUG_ON(mddev->delta_disks != 0);
	}

	if (test_bit(MD_HAS_JOURNAL, &mddev->flags) &&
	    test_bit(MD_HAS_PPL, &mddev->flags)) {
		pr_warn("md/raid:%s: using journal device and PPL not allowed - disabling PPL\n",
			mdname(mddev));
		clear_bit(MD_HAS_PPL, &mddev->flags);
		clear_bit(MD_HAS_MULTIPLE_PPLS, &mddev->flags);
	}

	if (mddev->private == NULL)
		conf = setup_conf(mddev);
	else
		conf = mddev->private;

	if (IS_ERR(conf))
		return PTR_ERR(conf);

	if (test_bit(MD_HAS_JOURNAL, &mddev->flags)) {
		if (!journal_dev) {
			pr_warn("md/raid:%s: journal disk is missing, force array readonly\n",
				mdname(mddev));
			mddev->ro = 1;
			set_disk_ro(mddev->gendisk, 1);
		} else if (mddev->recovery_cp == MaxSector)
			set_bit(MD_JOURNAL_CLEAN, &mddev->flags);
	}

	conf->min_offset_diff = min_offset_diff;
	rcu_assign_pointer(mddev->thread, conf->thread);
	rcu_assign_pointer(conf->thread, NULL);
	mddev->private = conf;

	for (i = 0; i < conf->raid_disks && conf->previous_raid_disks;
	     i++) {
		rdev = conf->disks[i].rdev;
		if (!rdev)
			continue;
		if (conf->disks[i].replacement &&
		    conf->reshape_progress != MaxSector) {
			/* replacements and reshape simply do not mix. */
			pr_warn("md: cannot handle concurrent replacement and reshape.\n");
			goto abort;
		}
		if (test_bit(In_sync, &rdev->flags))
			continue;
		/* This disc is not fully in-sync.  However if it
		 * just stored parity (beyond the recovery_offset),
		 * when we don't need to be concerned about the
		 * array being dirty.
		 * When reshape goes 'backwards', we never have
		 * partially completed devices, so we only need
		 * to worry about reshape going forwards.
		 */
		/* Hack because v0.91 doesn't store recovery_offset properly. */
		if (mddev->major_version == 0 &&
		    mddev->minor_version > 90)
			rdev->recovery_offset = reshape_offset;

		if (rdev->recovery_offset < reshape_offset) {
			/* We need to check old and new layout */
			if (!only_parity(rdev->raid_disk,
					 conf->algorithm,
					 conf->raid_disks,
					 conf->max_degraded))
				continue;
		}
		if (!only_parity(rdev->raid_disk,
				 conf->prev_algo,
				 conf->previous_raid_disks,
				 conf->max_degraded))
			continue;
		dirty_parity_disks++;
	}

	/*
	 * 0 for a fully functional array, 1 or 2 for a degraded array.
	 */
	mddev->degraded = raid5_calc_degraded(conf);

	if (has_failed(conf)) {
		pr_crit("md/raid:%s: not enough operational devices (%d/%d failed)\n",
			mdname(mddev), mddev->degraded, conf->raid_disks);
		goto abort;
	}

	/* device size must be a multiple of chunk size */
	mddev->dev_sectors &= ~((sector_t)mddev->chunk_sectors - 1);
	mddev->resync_max_sectors = mddev->dev_sectors;

	if (mddev->degraded > dirty_parity_disks &&
	    mddev->recovery_cp != MaxSector) {
		if (test_bit(MD_HAS_PPL, &mddev->flags))
			pr_crit("md/raid:%s: starting dirty degraded array with PPL.\n",
				mdname(mddev));
		else if (mddev->ok_start_degraded)
			pr_crit("md/raid:%s: starting dirty degraded array - data corruption possible.\n",
				mdname(mddev));
		else {
			pr_crit("md/raid:%s: cannot start dirty degraded array.\n",
				mdname(mddev));
			goto abort;
		}
	}

	pr_info("md/raid:%s: raid level %d active with %d out of %d devices, algorithm %d\n",
		mdname(mddev), conf->level,
		mddev->raid_disks-mddev->degraded, mddev->raid_disks,
		mddev->new_layout);

	print_raid5_conf(conf);

	if (conf->reshape_progress != MaxSector) {
		conf->reshape_safe = conf->reshape_progress;
		atomic_set(&conf->reshape_stripes, 0);
		clear_bit(MD_RECOVERY_SYNC, &mddev->recovery);
		clear_bit(MD_RECOVERY_CHECK, &mddev->recovery);
		set_bit(MD_RECOVERY_RESHAPE, &mddev->recovery);
		set_bit(MD_RECOVERY_NEEDED, &mddev->recovery);
	}

	/* Ok, everything is just fine now */
	if (mddev->to_remove == &raid5_attrs_group)
		mddev->to_remove = NULL;
	else if (mddev->kobj.sd &&
	    sysfs_create_group(&mddev->kobj, &raid5_attrs_group))
		pr_warn("raid5: failed to create sysfs attributes for %s\n",
			mdname(mddev));
	md_set_array_sectors(mddev, raid5_size(mddev, 0, 0));

	if (!mddev_is_dm(mddev)) {
		ret = raid5_set_limits(mddev);
		if (ret)
			goto abort;
	}

	if (log_init(conf, journal_dev, raid5_has_ppl(conf)))
		goto abort;

	return 0;
abort:
	md_unregister_thread(mddev, &mddev->thread);
	print_raid5_conf(conf);
	free_conf(conf);
	mddev->private = NULL;
	pr_warn("md/raid:%s: failed to run raid set.\n", mdname(mddev));
	return ret;
}

static void raid5_free(struct mddev *mddev, void *priv)
{
	struct r5conf *conf = priv;

	free_conf(conf);
	mddev->to_remove = &raid5_attrs_group;
}

static void raid5_status(struct seq_file *seq, struct mddev *mddev)
{
	struct r5conf *conf = mddev->private;
	int i;

	lockdep_assert_held(&mddev->lock);

	seq_printf(seq, " level %d, %dk chunk, algorithm %d", mddev->level,
		conf->chunk_sectors / 2, mddev->layout);
	seq_printf (seq, " [%d/%d] [", conf->raid_disks, conf->raid_disks - mddev->degraded);
	for (i = 0; i < conf->raid_disks; i++) {
		struct md_rdev *rdev = READ_ONCE(conf->disks[i].rdev);

		seq_printf (seq, "%s", rdev && test_bit(In_sync, &rdev->flags) ? "U" : "_");
	}
	seq_printf (seq, "]");
}

static void print_raid5_conf(struct r5conf *conf)
{
	struct md_rdev *rdev;
	int i;

	pr_debug("RAID conf printout:\n");
	if (!conf) {
		pr_debug("(conf==NULL)\n");
		return;
	}
	pr_debug(" --- level:%d rd:%d wd:%d\n", conf->level,
	       conf->raid_disks,
	       conf->raid_disks - conf->mddev->degraded);

	for (i = 0; i < conf->raid_disks; i++) {
		rdev = conf->disks[i].rdev;
		if (rdev)
			pr_debug(" disk %d, o:%d, dev:%pg\n",
			       i, !test_bit(Faulty, &rdev->flags),
			       rdev->bdev);
	}
}

static int raid5_spare_active(struct mddev *mddev)
{
	int i;
	struct r5conf *conf = mddev->private;
	struct md_rdev *rdev, *replacement;
	int count = 0;
	unsigned long flags;

	for (i = 0; i < conf->raid_disks; i++) {
		rdev = conf->disks[i].rdev;
		replacement = conf->disks[i].replacement;
		if (replacement
		    && replacement->recovery_offset == MaxSector
		    && !test_bit(Faulty, &replacement->flags)
		    && !test_and_set_bit(In_sync, &replacement->flags)) {
			/* Replacement has just become active. */
			if (!rdev
			    || !test_and_clear_bit(In_sync, &rdev->flags))
				count++;
			if (rdev) {
				/* Replaced device not technically faulty,
				 * but we need to be sure it gets removed
				 * and never re-added.
				 */
				set_bit(Faulty, &rdev->flags);
				sysfs_notify_dirent_safe(
					rdev->sysfs_state);
			}
			sysfs_notify_dirent_safe(replacement->sysfs_state);
		} else if (rdev
		    && rdev->recovery_offset == MaxSector
		    && !test_bit(Faulty, &rdev->flags)
		    && !test_and_set_bit(In_sync, &rdev->flags)) {
			count++;
			sysfs_notify_dirent_safe(rdev->sysfs_state);
		}
	}
	spin_lock_irqsave(&conf->device_lock, flags);
	mddev->degraded = raid5_calc_degraded(conf);
	spin_unlock_irqrestore(&conf->device_lock, flags);
	print_raid5_conf(conf);
	return count;
}

static int raid5_remove_disk(struct mddev *mddev, struct md_rdev *rdev)
{
	struct r5conf *conf = mddev->private;
	int err = 0;
	int number = rdev->raid_disk;
	struct md_rdev **rdevp;
	struct disk_info *p;
	struct md_rdev *tmp;

	print_raid5_conf(conf);
	if (test_bit(Journal, &rdev->flags) && conf->log) {
		/*
		 * we can't wait pending write here, as this is called in
		 * raid5d, wait will deadlock.
		 * neilb: there is no locking about new writes here,
		 * so this cannot be safe.
		 */
		if (atomic_read(&conf->active_stripes) ||
		    atomic_read(&conf->r5c_cached_full_stripes) ||
		    atomic_read(&conf->r5c_cached_partial_stripes)) {
			return -EBUSY;
		}
		log_exit(conf);
		return 0;
	}
	if (unlikely(number >= conf->pool_size))
		return 0;
	p = conf->disks + number;
	if (rdev == p->rdev)
		rdevp = &p->rdev;
	else if (rdev == p->replacement)
		rdevp = &p->replacement;
	else
		return 0;

	if (number >= conf->raid_disks &&
	    conf->reshape_progress == MaxSector)
		clear_bit(In_sync, &rdev->flags);

	if (test_bit(In_sync, &rdev->flags) ||
	    atomic_read(&rdev->nr_pending)) {
		err = -EBUSY;
		goto abort;
	}
	/* Only remove non-faulty devices if recovery
	 * isn't possible.
	 */
	if (!test_bit(Faulty, &rdev->flags) &&
	    mddev->recovery_disabled != conf->recovery_disabled &&
	    !has_failed(conf) &&
	    (!p->replacement || p->replacement == rdev) &&
	    number < conf->raid_disks) {
		err = -EBUSY;
		goto abort;
	}
	WRITE_ONCE(*rdevp, NULL);
	if (!err) {
		err = log_modify(conf, rdev, false);
		if (err)
			goto abort;
	}

	tmp = p->replacement;
	if (tmp) {
		/* We must have just cleared 'rdev' */
		WRITE_ONCE(p->rdev, tmp);
		clear_bit(Replacement, &tmp->flags);
		WRITE_ONCE(p->replacement, NULL);

		if (!err)
			err = log_modify(conf, tmp, true);
	}

	clear_bit(WantReplacement, &rdev->flags);
abort:

	print_raid5_conf(conf);
	return err;
}

static int raid5_add_disk(struct mddev *mddev, struct md_rdev *rdev)
{
	struct r5conf *conf = mddev->private;
	int ret, err = -EEXIST;
	int disk;
	struct disk_info *p;
	struct md_rdev *tmp;
	int first = 0;
	int last = conf->raid_disks - 1;

	if (test_bit(Journal, &rdev->flags)) {
		if (conf->log)
			return -EBUSY;

		rdev->raid_disk = 0;
		/*
		 * The array is in readonly mode if journal is missing, so no
		 * write requests running. We should be safe
		 */
		ret = log_init(conf, rdev, false);
		if (ret)
			return ret;

		ret = r5l_start(conf->log);
		if (ret)
			return ret;

		return 0;
	}
	if (mddev->recovery_disabled == conf->recovery_disabled)
		return -EBUSY;

	if (rdev->saved_raid_disk < 0 && has_failed(conf))
		/* no point adding a device */
		return -EINVAL;

	if (rdev->raid_disk >= 0)
		first = last = rdev->raid_disk;

	/*
	 * find the disk ... but prefer rdev->saved_raid_disk
	 * if possible.
	 */
	if (rdev->saved_raid_disk >= first &&
	    rdev->saved_raid_disk <= last &&
	    conf->disks[rdev->saved_raid_disk].rdev == NULL)
		first = rdev->saved_raid_disk;

	for (disk = first; disk <= last; disk++) {
		p = conf->disks + disk;
		if (p->rdev == NULL) {
			clear_bit(In_sync, &rdev->flags);
			rdev->raid_disk = disk;
			if (rdev->saved_raid_disk != disk)
				conf->fullsync = 1;
			WRITE_ONCE(p->rdev, rdev);

			err = log_modify(conf, rdev, true);

			goto out;
		}
	}
	for (disk = first; disk <= last; disk++) {
		p = conf->disks + disk;
		tmp = p->rdev;
		if (test_bit(WantReplacement, &tmp->flags) &&
		    mddev->reshape_position == MaxSector &&
		    p->replacement == NULL) {
			clear_bit(In_sync, &rdev->flags);
			set_bit(Replacement, &rdev->flags);
			rdev->raid_disk = disk;
			err = 0;
			conf->fullsync = 1;
			WRITE_ONCE(p->replacement, rdev);
			break;
		}
	}
out:
	print_raid5_conf(conf);
	return err;
}

static int raid5_resize(struct mddev *mddev, sector_t sectors)
{
	/* no resync is happening, and there is enough space
	 * on all devices, so we can resize.
	 * We need to make sure resync covers any new space.
	 * If the array is shrinking we should possibly wait until
	 * any io in the removed space completes, but it hardly seems
	 * worth it.
	 */
	sector_t newsize;
	struct r5conf *conf = mddev->private;
	int ret;

	if (raid5_has_log(conf) || raid5_has_ppl(conf))
		return -EINVAL;
	sectors &= ~((sector_t)conf->chunk_sectors - 1);
	newsize = raid5_size(mddev, sectors, mddev->raid_disks);
	if (mddev->external_size &&
	    mddev->array_sectors > newsize)
		return -EINVAL;

	ret = mddev->bitmap_ops->resize(mddev, sectors, 0, false);
	if (ret)
		return ret;

	md_set_array_sectors(mddev, newsize);
	if (sectors > mddev->dev_sectors &&
	    mddev->recovery_cp > mddev->dev_sectors) {
		mddev->recovery_cp = mddev->dev_sectors;
		set_bit(MD_RECOVERY_NEEDED, &mddev->recovery);
	}
	mddev->dev_sectors = sectors;
	mddev->resync_max_sectors = sectors;
	return 0;
}

static int check_stripe_cache(struct mddev *mddev)
{
	/* Can only proceed if there are plenty of stripe_heads.
	 * We need a minimum of one full stripe,, and for sensible progress
	 * it is best to have about 4 times that.
	 * If we require 4 times, then the default 256 4K stripe_heads will
	 * allow for chunk sizes up to 256K, which is probably OK.
	 * If the chunk size is greater, user-space should request more
	 * stripe_heads first.
	 */
	struct r5conf *conf = mddev->private;
	if (((mddev->chunk_sectors << 9) / RAID5_STRIPE_SIZE(conf)) * 4
	    > conf->min_nr_stripes ||
	    ((mddev->new_chunk_sectors << 9) / RAID5_STRIPE_SIZE(conf)) * 4
	    > conf->min_nr_stripes) {
		pr_warn("md/raid:%s: reshape: not enough stripes.  Needed %lu\n",
			mdname(mddev),
			((max(mddev->chunk_sectors, mddev->new_chunk_sectors) << 9)
			 / RAID5_STRIPE_SIZE(conf))*4);
		return 0;
	}
	return 1;
}

static int check_reshape(struct mddev *mddev)
{
	struct r5conf *conf = mddev->private;

	if (raid5_has_log(conf) || raid5_has_ppl(conf))
		return -EINVAL;
	if (mddev->delta_disks == 0 &&
	    mddev->new_layout == mddev->layout &&
	    mddev->new_chunk_sectors == mddev->chunk_sectors)
		return 0; /* nothing to do */
	if (has_failed(conf))
		return -EINVAL;
	if (mddev->delta_disks < 0 && mddev->reshape_position == MaxSector) {
		/* We might be able to shrink, but the devices must
		 * be made bigger first.
		 * For raid6, 4 is the minimum size.
		 * Otherwise 2 is the minimum
		 */
		int min = 2;
		if (mddev->level == 6)
			min = 4;
		if (mddev->raid_disks + mddev->delta_disks < min)
			return -EINVAL;
	}

	if (!check_stripe_cache(mddev))
		return -ENOSPC;

	if (mddev->new_chunk_sectors > mddev->chunk_sectors ||
	    mddev->delta_disks > 0)
		if (resize_chunks(conf,
				  conf->previous_raid_disks
				  + max(0, mddev->delta_disks),
				  max(mddev->new_chunk_sectors,
				      mddev->chunk_sectors)
			    ) < 0)
			return -ENOMEM;

	if (conf->previous_raid_disks + mddev->delta_disks <= conf->pool_size)
		return 0; /* never bother to shrink */
	return resize_stripes(conf, (conf->previous_raid_disks
				     + mddev->delta_disks));
}

static int raid5_start_reshape(struct mddev *mddev)
{
	struct r5conf *conf = mddev->private;
	struct md_rdev *rdev;
	int spares = 0;
	int i;
	unsigned long flags;

	if (test_bit(MD_RECOVERY_RUNNING, &mddev->recovery))
		return -EBUSY;

	if (!check_stripe_cache(mddev))
		return -ENOSPC;

	if (has_failed(conf))
		return -EINVAL;

	/* raid5 can't handle concurrent reshape and recovery */
	if (mddev->recovery_cp < MaxSector)
		return -EBUSY;
	for (i = 0; i < conf->raid_disks; i++)
		if (conf->disks[i].replacement)
			return -EBUSY;

	rdev_for_each(rdev, mddev) {
		if (!test_bit(In_sync, &rdev->flags)
		    && !test_bit(Faulty, &rdev->flags))
			spares++;
	}

	if (spares - mddev->degraded < mddev->delta_disks - conf->max_degraded)
		/* Not enough devices even to make a degraded array
		 * of that size
		 */
		return -EINVAL;

	/* Refuse to reduce size of the array.  Any reductions in
	 * array size must be through explicit setting of array_size
	 * attribute.
	 */
	if (raid5_size(mddev, 0, conf->raid_disks + mddev->delta_disks)
	    < mddev->array_sectors) {
		pr_warn("md/raid:%s: array size must be reduced before number of disks\n",
			mdname(mddev));
		return -EINVAL;
	}

	atomic_set(&conf->reshape_stripes, 0);
	spin_lock_irq(&conf->device_lock);
	write_seqcount_begin(&conf->gen_lock);
	conf->previous_raid_disks = conf->raid_disks;
	conf->raid_disks += mddev->delta_disks;
	conf->prev_chunk_sectors = conf->chunk_sectors;
	conf->chunk_sectors = mddev->new_chunk_sectors;
	conf->prev_algo = conf->algorithm;
	conf->algorithm = mddev->new_layout;
	conf->generation++;
	/* Code that selects data_offset needs to see the generation update
	 * if reshape_progress has been set - so a memory barrier needed.
	 */
	smp_mb();
	if (mddev->reshape_backwards)
		conf->reshape_progress = raid5_size(mddev, 0, 0);
	else
		conf->reshape_progress = 0;
	conf->reshape_safe = conf->reshape_progress;
	write_seqcount_end(&conf->gen_lock);
	spin_unlock_irq(&conf->device_lock);

	/* Now make sure any requests that proceeded on the assumption
	 * the reshape wasn't running - like Discard or Read - have
	 * completed.
	 */
	raid5_quiesce(mddev, true);
	raid5_quiesce(mddev, false);

	/* Add some new drives, as many as will fit.
	 * We know there are enough to make the newly sized array work.
	 * Don't add devices if we are reducing the number of
	 * devices in the array.  This is because it is not possible
	 * to correctly record the "partially reconstructed" state of
	 * such devices during the reshape and confusion could result.
	 */
	if (mddev->delta_disks >= 0) {
		rdev_for_each(rdev, mddev)
			if (rdev->raid_disk < 0 &&
			    !test_bit(Faulty, &rdev->flags)) {
				if (raid5_add_disk(mddev, rdev) == 0) {
					if (rdev->raid_disk
					    >= conf->previous_raid_disks)
						set_bit(In_sync, &rdev->flags);
					else
						rdev->recovery_offset = 0;

					/* Failure here is OK */
					sysfs_link_rdev(mddev, rdev);
				}
			} else if (rdev->raid_disk >= conf->previous_raid_disks
				   && !test_bit(Faulty, &rdev->flags)) {
				/* This is a spare that was manually added */
				set_bit(In_sync, &rdev->flags);
			}

		/* When a reshape changes the number of devices,
		 * ->degraded is measured against the larger of the
		 * pre and post number of devices.
		 */
		spin_lock_irqsave(&conf->device_lock, flags);
		mddev->degraded = raid5_calc_degraded(conf);
		spin_unlock_irqrestore(&conf->device_lock, flags);
	}
	mddev->raid_disks = conf->raid_disks;
	mddev->reshape_position = conf->reshape_progress;
	set_bit(MD_SB_CHANGE_DEVS, &mddev->sb_flags);

	clear_bit(MD_RECOVERY_SYNC, &mddev->recovery);
	clear_bit(MD_RECOVERY_CHECK, &mddev->recovery);
	clear_bit(MD_RECOVERY_DONE, &mddev->recovery);
	set_bit(MD_RECOVERY_RESHAPE, &mddev->recovery);
	set_bit(MD_RECOVERY_NEEDED, &mddev->recovery);
	conf->reshape_checkpoint = jiffies;
	md_new_event();
	return 0;
}

/* This is called from the reshape thread and should make any
 * changes needed in 'conf'
 */
static void end_reshape(struct r5conf *conf)
{

	if (!test_bit(MD_RECOVERY_INTR, &conf->mddev->recovery)) {
		struct md_rdev *rdev;

		spin_lock_irq(&conf->device_lock);
		conf->previous_raid_disks = conf->raid_disks;
		md_finish_reshape(conf->mddev);
		smp_wmb();
		conf->reshape_progress = MaxSector;
		conf->mddev->reshape_position = MaxSector;
		rdev_for_each(rdev, conf->mddev)
			if (rdev->raid_disk >= 0 &&
			    !test_bit(Journal, &rdev->flags) &&
			    !test_bit(In_sync, &rdev->flags))
				rdev->recovery_offset = MaxSector;
		spin_unlock_irq(&conf->device_lock);
		wake_up(&conf->wait_for_reshape);

		mddev_update_io_opt(conf->mddev,
			conf->raid_disks - conf->max_degraded);
	}
}

/* This is called from the raid5d thread with mddev_lock held.
 * It makes config changes to the device.
 */
static void raid5_finish_reshape(struct mddev *mddev)
{
	struct r5conf *conf = mddev->private;
	struct md_rdev *rdev;

	if (!test_bit(MD_RECOVERY_INTR, &mddev->recovery)) {

		if (mddev->delta_disks <= 0) {
			int d;
			spin_lock_irq(&conf->device_lock);
			mddev->degraded = raid5_calc_degraded(conf);
			spin_unlock_irq(&conf->device_lock);
			for (d = conf->raid_disks ;
			     d < conf->raid_disks - mddev->delta_disks;
			     d++) {
				rdev = conf->disks[d].rdev;
				if (rdev)
					clear_bit(In_sync, &rdev->flags);
				rdev = conf->disks[d].replacement;
				if (rdev)
					clear_bit(In_sync, &rdev->flags);
			}
		}
		mddev->layout = conf->algorithm;
		mddev->chunk_sectors = conf->chunk_sectors;
		mddev->reshape_position = MaxSector;
		mddev->delta_disks = 0;
		mddev->reshape_backwards = 0;
	}
}

static void raid5_quiesce(struct mddev *mddev, int quiesce)
{
	struct r5conf *conf = mddev->private;

	if (quiesce) {
		/* stop all writes */
		lock_all_device_hash_locks_irq(conf);
		/* '2' tells resync/reshape to pause so that all
		 * active stripes can drain
		 */
		r5c_flush_cache(conf, INT_MAX);
		/* need a memory barrier to make sure read_one_chunk() sees
		 * quiesce started and reverts to slow (locked) path.
		 */
		smp_store_release(&conf->quiesce, 2);
		wait_event_cmd(conf->wait_for_quiescent,
				    atomic_read(&conf->active_stripes) == 0 &&
				    atomic_read(&conf->active_aligned_reads) == 0,
				    unlock_all_device_hash_locks_irq(conf),
				    lock_all_device_hash_locks_irq(conf));
		conf->quiesce = 1;
		unlock_all_device_hash_locks_irq(conf);
		/* allow reshape to continue */
		wake_up(&conf->wait_for_reshape);
	} else {
		/* re-enable writes */
		lock_all_device_hash_locks_irq(conf);
		conf->quiesce = 0;
		wake_up(&conf->wait_for_quiescent);
		wake_up(&conf->wait_for_reshape);
		unlock_all_device_hash_locks_irq(conf);
	}
	log_quiesce(conf, quiesce);
}

static void *raid45_takeover_raid0(struct mddev *mddev, int level)
{
	struct r0conf *raid0_conf = mddev->private;
	sector_t sectors;

	/* for raid0 takeover only one zone is supported */
	if (raid0_conf->nr_strip_zones > 1) {
		pr_warn("md/raid:%s: cannot takeover raid0 with more than one zone.\n",
			mdname(mddev));
		return ERR_PTR(-EINVAL);
	}

	sectors = raid0_conf->strip_zone[0].zone_end;
	sector_div(sectors, raid0_conf->strip_zone[0].nb_dev);
	mddev->dev_sectors = sectors;
	mddev->new_level = level;
	mddev->new_layout = ALGORITHM_PARITY_N;
	mddev->new_chunk_sectors = mddev->chunk_sectors;
	mddev->raid_disks += 1;
	mddev->delta_disks = 1;
	/* make sure it will be not marked as dirty */
	mddev->recovery_cp = MaxSector;

	return setup_conf(mddev);
}

static void *raid5_takeover_raid1(struct mddev *mddev)
{
	int chunksect;
	void *ret;

	if (mddev->raid_disks != 2 ||
	    mddev->degraded > 1)
		return ERR_PTR(-EINVAL);

	/* Should check if there are write-behind devices? */

	chunksect = 64*2; /* 64K by default */

	/* The array must be an exact multiple of chunksize */
	while (chunksect && (mddev->array_sectors & (chunksect-1)))
		chunksect >>= 1;

	if ((chunksect<<9) < RAID5_STRIPE_SIZE((struct r5conf *)mddev->private))
		/* array size does not allow a suitable chunk size */
		return ERR_PTR(-EINVAL);

	mddev->new_level = 5;
	mddev->new_layout = ALGORITHM_LEFT_SYMMETRIC;
	mddev->new_chunk_sectors = chunksect;

	ret = setup_conf(mddev);
	if (!IS_ERR(ret))
		mddev_clear_unsupported_flags(mddev,
			UNSUPPORTED_MDDEV_FLAGS);
	return ret;
}

static void *raid5_takeover_raid6(struct mddev *mddev)
{
	int new_layout;

	switch (mddev->layout) {
	case ALGORITHM_LEFT_ASYMMETRIC_6:
		new_layout = ALGORITHM_LEFT_ASYMMETRIC;
		break;
	case ALGORITHM_RIGHT_ASYMMETRIC_6:
		new_layout = ALGORITHM_RIGHT_ASYMMETRIC;
		break;
	case ALGORITHM_LEFT_SYMMETRIC_6:
		new_layout = ALGORITHM_LEFT_SYMMETRIC;
		break;
	case ALGORITHM_RIGHT_SYMMETRIC_6:
		new_layout = ALGORITHM_RIGHT_SYMMETRIC;
		break;
	case ALGORITHM_PARITY_0_6:
		new_layout = ALGORITHM_PARITY_0;
		break;
	case ALGORITHM_PARITY_N:
		new_layout = ALGORITHM_PARITY_N;
		break;
	default:
		return ERR_PTR(-EINVAL);
	}
	mddev->new_level = 5;
	mddev->new_layout = new_layout;
	mddev->delta_disks = -1;
	mddev->raid_disks -= 1;
	return setup_conf(mddev);
}

static int raid5_check_reshape(struct mddev *mddev)
{
	/* For a 2-drive array, the layout and chunk size can be changed
	 * immediately as not restriping is needed.
	 * For larger arrays we record the new value - after validation
	 * to be used by a reshape pass.
	 */
	struct r5conf *conf = mddev->private;
	int new_chunk = mddev->new_chunk_sectors;

	if (mddev->new_layout >= 0 && !algorithm_valid_raid5(mddev->new_layout))
		return -EINVAL;
	if (new_chunk > 0) {
		if (!is_power_of_2(new_chunk))
			return -EINVAL;
		if (new_chunk < (PAGE_SIZE>>9))
			return -EINVAL;
		if (mddev->array_sectors & (new_chunk-1))
			/* not factor of array size */
			return -EINVAL;
	}

	/* They look valid */

	if (mddev->raid_disks == 2) {
		/* can make the change immediately */
		if (mddev->new_layout >= 0) {
			conf->algorithm = mddev->new_layout;
			mddev->layout = mddev->new_layout;
		}
		if (new_chunk > 0) {
			conf->chunk_sectors = new_chunk ;
			mddev->chunk_sectors = new_chunk;
		}
		set_bit(MD_SB_CHANGE_DEVS, &mddev->sb_flags);
		md_wakeup_thread(mddev->thread);
	}
	return check_reshape(mddev);
}

static int raid6_check_reshape(struct mddev *mddev)
{
	int new_chunk = mddev->new_chunk_sectors;

	if (mddev->new_layout >= 0 && !algorithm_valid_raid6(mddev->new_layout))
		return -EINVAL;
	if (new_chunk > 0) {
		if (!is_power_of_2(new_chunk))
			return -EINVAL;
		if (new_chunk < (PAGE_SIZE >> 9))
			return -EINVAL;
		if (mddev->array_sectors & (new_chunk-1))
			/* not factor of array size */
			return -EINVAL;
	}

	/* They look valid */
	return check_reshape(mddev);
}

static void *raid5_takeover(struct mddev *mddev)
{
	/* raid5 can take over:
	 *  raid0 - if there is only one strip zone - make it a raid4 layout
	 *  raid1 - if there are two drives.  We need to know the chunk size
	 *  raid4 - trivial - just use a raid4 layout.
	 *  raid6 - Providing it is a *_6 layout
	 */
	if (mddev->level == 0)
		return raid45_takeover_raid0(mddev, 5);
	if (mddev->level == 1)
		return raid5_takeover_raid1(mddev);
	if (mddev->level == 4) {
		mddev->new_layout = ALGORITHM_PARITY_N;
		mddev->new_level = 5;
		return setup_conf(mddev);
	}
	if (mddev->level == 6)
		return raid5_takeover_raid6(mddev);

	return ERR_PTR(-EINVAL);
}

static void *raid4_takeover(struct mddev *mddev)
{
	/* raid4 can take over:
	 *  raid0 - if there is only one strip zone
	 *  raid5 - if layout is right
	 */
	if (mddev->level == 0)
		return raid45_takeover_raid0(mddev, 4);
	if (mddev->level == 5 &&
	    mddev->layout == ALGORITHM_PARITY_N) {
		mddev->new_layout = 0;
		mddev->new_level = 4;
		return setup_conf(mddev);
	}
	return ERR_PTR(-EINVAL);
}

static struct md_personality raid5_personality;

static void *raid6_takeover(struct mddev *mddev)
{
	/* Currently can only take over a raid5.  We map the
	 * personality to an equivalent raid6 personality
	 * with the Q block at the end.
	 */
	int new_layout;

	if (mddev->pers != &raid5_personality)
		return ERR_PTR(-EINVAL);
	if (mddev->degraded > 1)
		return ERR_PTR(-EINVAL);
	if (mddev->raid_disks > 253)
		return ERR_PTR(-EINVAL);
	if (mddev->raid_disks < 3)
		return ERR_PTR(-EINVAL);

	switch (mddev->layout) {
	case ALGORITHM_LEFT_ASYMMETRIC:
		new_layout = ALGORITHM_LEFT_ASYMMETRIC_6;
		break;
	case ALGORITHM_RIGHT_ASYMMETRIC:
		new_layout = ALGORITHM_RIGHT_ASYMMETRIC_6;
		break;
	case ALGORITHM_LEFT_SYMMETRIC:
		new_layout = ALGORITHM_LEFT_SYMMETRIC_6;
		break;
	case ALGORITHM_RIGHT_SYMMETRIC:
		new_layout = ALGORITHM_RIGHT_SYMMETRIC_6;
		break;
	case ALGORITHM_PARITY_0:
		new_layout = ALGORITHM_PARITY_0_6;
		break;
	case ALGORITHM_PARITY_N:
		new_layout = ALGORITHM_PARITY_N;
		break;
	default:
		return ERR_PTR(-EINVAL);
	}
	mddev->new_level = 6;
	mddev->new_layout = new_layout;
	mddev->delta_disks = 1;
	mddev->raid_disks += 1;
	return setup_conf(mddev);
}

static int raid5_change_consistency_policy(struct mddev *mddev, const char *buf)
{
	struct r5conf *conf;
	int err;

	err = mddev_suspend_and_lock(mddev);
	if (err)
		return err;
	conf = mddev->private;
	if (!conf) {
		mddev_unlock_and_resume(mddev);
		return -ENODEV;
	}

	if (strncmp(buf, "ppl", 3) == 0) {
		/* ppl only works with RAID 5 */
		if (!raid5_has_ppl(conf) && conf->level == 5) {
			err = log_init(conf, NULL, true);
			if (!err) {
				err = resize_stripes(conf, conf->pool_size);
				if (err)
					log_exit(conf);
			}
		} else
			err = -EINVAL;
	} else if (strncmp(buf, "resync", 6) == 0) {
		if (raid5_has_ppl(conf)) {
			log_exit(conf);
			err = resize_stripes(conf, conf->pool_size);
		} else if (test_bit(MD_HAS_JOURNAL, &conf->mddev->flags) &&
			   r5l_log_disk_error(conf)) {
			bool journal_dev_exists = false;
			struct md_rdev *rdev;

			rdev_for_each(rdev, mddev)
				if (test_bit(Journal, &rdev->flags)) {
					journal_dev_exists = true;
					break;
				}

			if (!journal_dev_exists)
				clear_bit(MD_HAS_JOURNAL, &mddev->flags);
			else  /* need remove journal device first */
				err = -EBUSY;
		} else
			err = -EINVAL;
	} else {
		err = -EINVAL;
	}

	if (!err)
		md_update_sb(mddev, 1);

	mddev_unlock_and_resume(mddev);

	return err;
}

static int raid5_start(struct mddev *mddev)
{
	struct r5conf *conf = mddev->private;

	return r5l_start(conf->log);
}

/*
 * This is only used for dm-raid456, caller already frozen sync_thread, hence
 * if rehsape is still in progress, io that is waiting for reshape can never be
 * done now, hence wake up and handle those IO.
 */
static void raid5_prepare_suspend(struct mddev *mddev)
{
	struct r5conf *conf = mddev->private;

	wake_up(&conf->wait_for_reshape);
}

static struct md_personality raid6_personality =
{
	.head = {
		.type	= MD_PERSONALITY,
		.id	= ID_RAID6,
		.name	= "raid6",
		.owner	= THIS_MODULE,
	},

	.make_request	= raid5_make_request,
	.run		= raid5_run,
	.start		= raid5_start,
	.free		= raid5_free,
	.status		= raid5_status,
	.error_handler	= raid5_error,
	.hot_add_disk	= raid5_add_disk,
	.hot_remove_disk= raid5_remove_disk,
	.spare_active	= raid5_spare_active,
	.sync_request	= raid5_sync_request,
	.resize		= raid5_resize,
	.size		= raid5_size,
	.check_reshape	= raid6_check_reshape,
	.start_reshape  = raid5_start_reshape,
	.finish_reshape = raid5_finish_reshape,
	.quiesce	= raid5_quiesce,
	.takeover	= raid6_takeover,
	.change_consistency_policy = raid5_change_consistency_policy,
	.prepare_suspend = raid5_prepare_suspend,
	.bitmap_sector	= raid5_bitmap_sector,
};
static struct md_personality raid5_personality =
{
	.head = {
		.type	= MD_PERSONALITY,
		.id	= ID_RAID5,
		.name	= "raid5",
		.owner	= THIS_MODULE,
	},

	.make_request	= raid5_make_request,
	.run		= raid5_run,
	.start		= raid5_start,
	.free		= raid5_free,
	.status		= raid5_status,
	.error_handler	= raid5_error,
	.hot_add_disk	= raid5_add_disk,
	.hot_remove_disk= raid5_remove_disk,
	.spare_active	= raid5_spare_active,
	.sync_request	= raid5_sync_request,
	.resize		= raid5_resize,
	.size		= raid5_size,
	.check_reshape	= raid5_check_reshape,
	.start_reshape  = raid5_start_reshape,
	.finish_reshape = raid5_finish_reshape,
	.quiesce	= raid5_quiesce,
	.takeover	= raid5_takeover,
	.change_consistency_policy = raid5_change_consistency_policy,
	.prepare_suspend = raid5_prepare_suspend,
	.bitmap_sector	= raid5_bitmap_sector,
};

static struct md_personality raid4_personality =
{
	.head = {
		.type	= MD_PERSONALITY,
		.id	= ID_RAID4,
		.name	= "raid4",
		.owner	= THIS_MODULE,
	},

	.make_request	= raid5_make_request,
	.run		= raid5_run,
	.start		= raid5_start,
	.free		= raid5_free,
	.status		= raid5_status,
	.error_handler	= raid5_error,
	.hot_add_disk	= raid5_add_disk,
	.hot_remove_disk= raid5_remove_disk,
	.spare_active	= raid5_spare_active,
	.sync_request	= raid5_sync_request,
	.resize		= raid5_resize,
	.size		= raid5_size,
	.check_reshape	= raid5_check_reshape,
	.start_reshape  = raid5_start_reshape,
	.finish_reshape = raid5_finish_reshape,
	.quiesce	= raid5_quiesce,
	.takeover	= raid4_takeover,
	.change_consistency_policy = raid5_change_consistency_policy,
	.prepare_suspend = raid5_prepare_suspend,
	.bitmap_sector	= raid5_bitmap_sector,
};

static int __init raid5_init(void)
{
	int ret;

	raid5_wq = alloc_workqueue("raid5wq",
		WQ_UNBOUND|WQ_MEM_RECLAIM|WQ_SYSFS, 0);
	if (!raid5_wq)
		return -ENOMEM;

	ret = cpuhp_setup_state_multi(CPUHP_MD_RAID5_PREPARE,
				      "md/raid5:prepare",
				      raid456_cpu_up_prepare,
				      raid456_cpu_dead);
	if (ret)
		goto err_destroy_wq;

	ret = register_md_submodule(&raid6_personality.head);
	if (ret)
		goto err_cpuhp_remove;

	ret = register_md_submodule(&raid5_personality.head);
	if (ret)
		goto err_unregister_raid6;

	ret = register_md_submodule(&raid4_personality.head);
	if (ret)
		goto err_unregister_raid5;

	return 0;

err_unregister_raid5:
	unregister_md_submodule(&raid5_personality.head);
err_unregister_raid6:
	unregister_md_submodule(&raid6_personality.head);
err_cpuhp_remove:
	cpuhp_remove_multi_state(CPUHP_MD_RAID5_PREPARE);
err_destroy_wq:
	destroy_workqueue(raid5_wq);
	return ret;
}

static void __exit raid5_exit(void)
{
	unregister_md_submodule(&raid6_personality.head);
	unregister_md_submodule(&raid5_personality.head);
	unregister_md_submodule(&raid4_personality.head);
	cpuhp_remove_multi_state(CPUHP_MD_RAID5_PREPARE);
	destroy_workqueue(raid5_wq);
}

module_init(raid5_init);
module_exit(raid5_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("RAID4/5/6 (striping with parity) personality for MD");
MODULE_ALIAS("md-personality-4"); /* RAID5 */
MODULE_ALIAS("md-raid5");
MODULE_ALIAS("md-raid4");
MODULE_ALIAS("md-level-5");
MODULE_ALIAS("md-level-4");
MODULE_ALIAS("md-personality-8"); /* RAID6 */
MODULE_ALIAS("md-raid6");
MODULE_ALIAS("md-level-6");

/* This used to be two separate modules, they were: */
MODULE_ALIAS("raid5");
MODULE_ALIAS("raid6");
