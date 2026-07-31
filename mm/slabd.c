// SPDX-License-Identifier: GPL-2.0
/*
 * Built-in port of the Moon kshrink_slabd module.
 *
 * Moves one eligible slab reclaim pass at a time onto a freezable kernel
 * worker, faithfully following the original module's semantics:
 *  - android_vh_shrink_slab_bypass hook with a single pending slot;
 *  - hook-side throttling (~249 jiffies between async passes) and a
 *    com.miui.home foreground exclusion, both from the original
 *    should_shrink_async() decision path;
 *  - worker re-enters shrink_slab() with the same gfp/nid/memcg/priority;
 *  - worker CPU affinity chosen from the lowest-frequency cpufreq policy,
 *    bound to the online CPUs outside that policy (original
 *    set_async_slabd_cpus behaviour);
 *  - PF_KTHREAD/PID self-exclusion so the worker's own shrink_slab()
 *    call runs synchronously.
 *
 * Lifecycle adaptation for built-in: non-root memcgs are pinned with
 * css_tryget_online() across the queue and released after the worker
 * consumes them; the original module leaked this reference.
 */
#include <linux/cgroup.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/freezer.h>
#include <linux/gfp.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kthread.h>
#include <linux/memcontrol.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>
#include <linux/topology.h>
#include <linux/wait.h>

#include <trace/hooks/vmscan.h>

#include "slabd.h"

/* Original throttle: jiffies - prev > 0xf9 (249). */
#define KSHRINK_SLABD_MIN_INTERVAL	249

struct kshrink_slabd_request {
	gfp_t gfp_mask;
	int nid;
	int priority;
	struct mem_cgroup *memcg;
	bool memcg_pinned;
};

static DEFINE_SPINLOCK(kshrink_slabd_lock);
static DECLARE_WAIT_QUEUE_HEAD(kshrink_slabd_wait);
static struct kshrink_slabd_request kshrink_slabd_request;
static struct task_struct *kshrink_slabd_task;
static bool kshrink_slabd_pending;
static bool kshrink_slabd_enabled;
static bool kshrink_slabd_affinity_done;
static unsigned long kshrink_slabd_prev_jiffies;
static unsigned long kshrink_slabd_queued;
static unsigned long kshrink_slabd_completed;

extern unsigned long shrink_slab(gfp_t gfp_mask, int nid,
				 struct mem_cgroup *memcg, int priority);

/*
 * Original set_async_slabd_cpus(): walk possible CPUs, pick the policy
 * with the lowest max frequency, then bind the worker to the online
 * CPUs outside that policy's related set. Runs once.
 */
static void kshrink_slabd_set_affinity(void)
{
	struct cpufreq_policy *policy;
	struct cpufreq_policy *lowest = NULL;
	unsigned int lowest_max = 0;
	cpumask_t allowed;
	int cpu;

	for_each_possible_cpu(cpu) {
		policy = cpufreq_cpu_get(cpu);
		if (!policy)
			continue;
		if (!lowest || policy->max < lowest_max) {
			if (lowest)
				cpufreq_cpu_put(lowest);
			lowest = policy;
			lowest_max = policy->max;
		} else {
			cpufreq_cpu_put(policy);
		}
	}
	if (!lowest)
		return;

	cpumask_andnot(&allowed, cpu_online_mask, lowest->related_cpus);
	cpufreq_cpu_put(lowest);

	if (!cpumask_empty(&allowed) &&
	    !set_cpus_allowed_ptr(current, &allowed))
		kshrink_slabd_affinity_done = true;
}

static int kshrink_slabd(void *unused)
{
	set_freezable();

	while (!kthread_should_stop()) {
		struct kshrink_slabd_request request;
		bool pending;

		wait_event_freezable(kshrink_slabd_wait,
			kthread_should_stop() || READ_ONCE(kshrink_slabd_pending));
		if (kthread_should_stop())
			break;

		spin_lock_irq(&kshrink_slabd_lock);
		pending = kshrink_slabd_pending;
		if (pending) {
			request = kshrink_slabd_request;
			kshrink_slabd_pending = false;
		}
		spin_unlock_irq(&kshrink_slabd_lock);

		if (!pending)
			continue;

		if (!READ_ONCE(kshrink_slabd_affinity_done))
			kshrink_slabd_set_affinity();

		shrink_slab(request.gfp_mask, request.nid, request.memcg,
			    request.priority);
		if (request.memcg_pinned)
			css_put(&request.memcg->css);
		WRITE_ONCE(kshrink_slabd_completed,
			   READ_ONCE(kshrink_slabd_completed) + 1);
	}

	return 0;
}

/*
 * Original should_shrink_async(): async only when enabled, the caller
 * is not the worker itself, the current task is not a launcher-thread
 * (com.miui.home) holding the UI, at least 249 jiffies passed since the
 * last accepted async pass, and the single slot is free.
 */
static bool kshrink_slabd_should_async(void)
{
	unsigned long now = jiffies;
	unsigned long prev;

	if (!READ_ONCE(kshrink_slabd_enabled))
		return false;
	if (current == READ_ONCE(kshrink_slabd_task))
		return false;
	if (current->flags & PF_KTHREAD)
		return false;

	/* ko: skip async while a com.miui.home thread is the caller. */
	if (!strcmp(current->comm, "com.miui.home"))
		return false;

	prev = READ_ONCE(kshrink_slabd_prev_jiffies);
	WRITE_ONCE(kshrink_slabd_prev_jiffies, now);
	if (now - prev <= KSHRINK_SLABD_MIN_INTERVAL)
		return false;

	return true;
}

bool kshrink_slabd_queue(gfp_t gfp_mask, int nid,
				struct mem_cgroup *memcg, int priority)
{
	unsigned long flags;
	bool queued = false;

	if (!READ_ONCE(kshrink_slabd_task))
		return false;

	spin_lock_irqsave(&kshrink_slabd_lock, flags);
	if (!kshrink_slabd_pending) {
		struct mem_cgroup *pinned = memcg;
		bool memcg_pinned = false;

		/* ko did not pin; built-in must keep the memcg alive. */
		if (pinned && !mem_cgroup_is_root(pinned)) {
			if (!css_tryget_online(&pinned->css))
				pinned = NULL;
			else
				memcg_pinned = true;
		}

		if (pinned || !memcg) {
			kshrink_slabd_request.gfp_mask = gfp_mask;
			kshrink_slabd_request.nid = nid;
			kshrink_slabd_request.priority = priority;
			kshrink_slabd_request.memcg = pinned;
			kshrink_slabd_request.memcg_pinned = memcg_pinned;
			kshrink_slabd_pending = true;
			kshrink_slabd_queued++;
			queued = true;
		}
	}
	spin_unlock_irqrestore(&kshrink_slabd_lock, flags);

	if (queued)
		wake_up_interruptible(&kshrink_slabd_wait);
	return queued;
}

static void kshrink_slabd_bypass(void *data, gfp_t gfp_mask, int nid,
				 struct mem_cgroup *memcg, int priority,
				 bool *bypass)
{
	if (*bypass)
		return;
	if (!kshrink_slabd_should_async())
		return;
	if (kshrink_slabd_queue(gfp_mask, nid, memcg, priority))
		*bypass = true;
}

static int kshrink_slabd_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "enable = %d\nqueued = %lu\ncompleted = %lu\npending = %u\ninterval_jiffies = %u\n",
		   READ_ONCE(kshrink_slabd_enabled) ? 1 : 0,
		   READ_ONCE(kshrink_slabd_queued),
		   READ_ONCE(kshrink_slabd_completed),
		   READ_ONCE(kshrink_slabd_pending),
		   KSHRINK_SLABD_MIN_INTERVAL);
	return 0;
}

static int __init kshrink_slabd_init(void)
{
	int ret;

	kshrink_slabd_prev_jiffies = jiffies;
	kshrink_slabd_task = kthread_run(kshrink_slabd, NULL,
					 "kshrink_slabd");
	if (IS_ERR(kshrink_slabd_task))
		return PTR_ERR(kshrink_slabd_task);

	ret = register_trace_android_vh_shrink_slab_bypass(
		kshrink_slabd_bypass, NULL);
	if (ret) {
		kthread_stop(kshrink_slabd_task);
		kshrink_slabd_task = NULL;
		return ret;
	}

	proc_create_single("kshrink_slabd", 0444, NULL,
			   kshrink_slabd_proc_show);
	WRITE_ONCE(kshrink_slabd_enabled, true);
	return 0;
}
module_init(kshrink_slabd_init);
