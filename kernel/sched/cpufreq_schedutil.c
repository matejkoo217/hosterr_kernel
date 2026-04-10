// SPDX-License-Identifier: GPL-2.0
/*
 * CPUFreq governor based on scheduler-provided CPU utilization data.
 *
 * Copyright (C) 2016, Intel Corporation
 * Author: Rafael J. Wysocki <rafael.j.wysocki@intel.com>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include "sched.h"

#include <linux/sched/cpufreq.h>
#include <trace/events/power.h>
#include <trace/hooks/sched.h>

#include <linux/cpu.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/ktime.h>
#include <linux/cpumask.h>
#include <linux/suspend.h>
#include <linux/topology.h>
#include <linux/backlight.h>
#ifndef SCHED_CPUFREQ_IOWAIT
#define SCHED_CPUFREQ_IOWAIT	0x1
#endif
#define IOWAIT_BOOST_MAX		SCHED_CAPACITY_SCALE
#define IOWAIT_BOOST_DECAY_NS	   (8ULL * NSEC_PER_MSEC)
#define HOSTERR_DEFAULT_RATE_LIMIT_US   1000U

struct hosterr_cache {
	unsigned int max_mode;
	unsigned int sleep;
	unsigned int down_damping;
};
static unsigned int hosterr_max_mode	  = 0;
static unsigned int hosterr_sleep		 = 0;
static unsigned int hosterr_down_damping  = 4;
static unsigned int hosterr_frame_delay_ns  = 0;
static unsigned int hosterr_frame_budget_ns = 8333333;
static unsigned long hosterr_frame_boost	  = 0;
static unsigned long hosterr_frame_boost_decay = 3;
static unsigned long hosterr_frame_boost_max   = SCHED_CAPACITY_SCALE / 2;
static u64 hosterr_last_pressure_update = 0;
#define IOWAIT_BOOST_MIN	(SCHED_CAPACITY_SCALE / 8)
module_param_named(max_mode,		hosterr_max_mode,		uint,  0644);
module_param_named(sleep,		   hosterr_sleep,		   uint,  0644);
module_param_named(down_damping,	hosterr_down_damping,	uint,  0644);
module_param_named(frame_delay_ns,  hosterr_frame_delay_ns,  uint,  0644);
module_param_named(frame_budget_ns, hosterr_frame_budget_ns, uint,  0644);

static struct backlight_device *hosterr_bd = NULL;
static bool hosterr_max_mode_saved = false;
static struct hosterr_cache hosterr_cached;
static DEFINE_SPINLOCK(hosterr_cache_lock);
static struct delayed_work hosterr_background_work;
static bool hosterr_timer_running = false;
static bool hosterr_works_init = false;

struct sugov_tunables {
	struct gov_attr_set	attr_set;
	unsigned int		rate_limit_us;
};

struct sugov_policy {
	struct cpufreq_policy	*policy;

	struct sugov_tunables	*tunables;
	struct list_head	tunables_hook;

	raw_spinlock_t		update_lock;
	u64			last_freq_update_time;
	s64			freq_update_delay_ns;
	unsigned int		next_freq;
	unsigned long	   last_util;
	unsigned int		cached_raw_freq;

	/* Hosterr tracking variables */
	int		 last_dir;
	u64		 osc_window_start;
	unsigned int		osc_change_count;
	bool			is_oscillating;
	unsigned int		target_freq_smoothed;

	/* The next fields are only needed if fast switch cannot be used: */
	struct			irq_work irq_work;
	struct			kthread_work work;
	struct			mutex work_lock;
	struct			kthread_worker worker;
	struct task_struct	*thread;
	bool			work_in_progress;

	bool			limits_changed;
	bool			need_freq_update;
};

struct sugov_cpu {
	struct update_util_data	update_util;
	struct sugov_policy	*sg_policy;
	unsigned int		cpu;

	bool			iowait_boost_pending;
	unsigned int		iowait_boost;
	u64			last_update;

	unsigned long		util;
	unsigned long		bw_dl;
	unsigned long		max;

	/* The field below is for single-CPU policies only: */
#ifdef CONFIG_NO_HZ_COMMON
	unsigned long		saved_idle_calls;
#endif
};

static DEFINE_PER_CPU(struct sugov_cpu, sugov_cpu);

/************************ Hosterr Internals ***********************/
static void hosterr_update_cache(void)
{
	unsigned long flags;
	bool changed = false;
	spin_lock_irqsave(&hosterr_cache_lock, flags);
	if (hosterr_cached.max_mode != hosterr_max_mode) {
		hosterr_cached.max_mode = hosterr_max_mode;
		changed = true;
	}
	if (hosterr_cached.sleep != hosterr_sleep) {
		hosterr_cached.sleep = hosterr_sleep;
		changed = true;
	}
	if (hosterr_cached.down_damping != hosterr_down_damping) {
		hosterr_cached.down_damping = hosterr_down_damping;
		changed = true;
	}
	spin_unlock_irqrestore(&hosterr_cache_lock, flags);
	if (changed)
		pr_info_ratelimited("[HOSTERR] parameters updated\n");
}

static void hosterr_frame_pressure_update(void)
{
	u64 now = ktime_get_ns();
	u64 budget_ns = READ_ONCE(hosterr_frame_budget_ns);
	u64 frame_delay = READ_ONCE(hosterr_frame_delay_ns);
	u64 gate_ns;
	unsigned long boost, step, decay;
	if (!budget_ns)
		budget_ns = 16666666ULL;
	gate_ns = budget_ns >> 1;
	if (!gate_ns)
		gate_ns = NSEC_PER_MSEC;
	if ((now - READ_ONCE(hosterr_last_pressure_update)) < gate_ns)
		return;
	WRITE_ONCE(hosterr_last_pressure_update, now);
	boost = READ_ONCE(hosterr_frame_boost);
	if (frame_delay > budget_ns) {
		step = SCHED_CAPACITY_SCALE / 8;
		if (frame_delay > (budget_ns << 1))
			step = SCHED_CAPACITY_SCALE / 4;
		boost = min(boost + step, hosterr_frame_boost_max);
	} else if (boost) {
		decay = READ_ONCE(hosterr_frame_boost_decay);
		if (!decay)
			decay = 1;
		step = max(1UL, boost / decay);
		boost = (step >= boost) ? 0UL : boost - step;
	}
	WRITE_ONCE(hosterr_frame_boost, boost);
}

static unsigned long hosterr_bend_utilization(unsigned long util,
					      unsigned long max)
{
	if (READ_ONCE(hosterr_frame_boost) > (SCHED_CAPACITY_SCALE / 16))
		return util;
	if (!util || !max)
		return 0;
	if (util > max)
		util = max;
	if (util > (max * 7 / 8))
		return util;
	return util - (util >> 3);
}

static unsigned long hosterr_predict_util(struct sugov_policy *sg_policy,
				      unsigned long util,
				      unsigned long max)
{
	unsigned long prev = sg_policy->last_util;
	long delta = (long)util - (long)prev;
	unsigned long predicted;
	if (delta > 0) {
		predicted = util + (delta >> 1);
	} else if (delta < 0) {
		predicted = prev - ((-delta) >> 1);
	} else {
		predicted = util;
	}

	if (predicted > max)
		predicted = max;
	sg_policy->last_util = util;
	return predicted;
}

static unsigned long hosterr_dynamic_curve(struct cpufreq_policy *policy,
				       unsigned long util,
				       unsigned long max)
{
	unsigned long window = policy->max - policy->min;
	unsigned long scale;
	if (!window || window < (policy->cpuinfo.max_freq >> 2))
		return util;
	scale = (policy->max << 10) / window;
	if (scale > (4UL << 10))
		scale = (4UL << 10);
	util += (util * scale) >> 12;
	return min(util, max);
}

static void hosterr_background_handler(struct work_struct *work)
{
	unsigned long flags;
	int brightness = -1;
	bool just_slept = false;
	bool just_woke = false;
	unsigned int check_interval = 1000;
	hosterr_update_cache();
	if (!hosterr_bd)
		hosterr_bd = backlight_device_get_by_name("panel0-backlight");
	if (hosterr_bd) {
		brightness = hosterr_bd->props.brightness;
		spin_lock_irqsave(&hosterr_cache_lock, flags);
		if (brightness == 0 && hosterr_cached.sleep == 0) {
			hosterr_sleep = 1;
			hosterr_cached.sleep = 1;
			just_slept = true;
			if (hosterr_cached.max_mode == 1) {
				hosterr_max_mode_saved = true;
				hosterr_max_mode = 0;
				hosterr_cached.max_mode = 0;
			}
		} else if (brightness > 0 && hosterr_cached.sleep == 1) {
			hosterr_sleep = 0;
			hosterr_cached.sleep = 0;
			just_woke = true;
			if (hosterr_max_mode_saved) {
				hosterr_max_mode = 1;
				hosterr_cached.max_mode = 1;
				hosterr_max_mode_saved = false;
			}
		}
		spin_unlock_irqrestore(&hosterr_cache_lock, flags);
	}
	if (hosterr_timer_running)
		schedule_delayed_work(&hosterr_background_work,
				      msecs_to_jiffies(check_interval));
}

static int hosterr_pm_callback(struct notifier_block *nb, unsigned long action,
				   void *ptr)
{
	unsigned long flags;
	bool do_sleep = false;
	bool do_wake = false;
	switch (action) {
	case PM_SUSPEND_PREPARE:
		hosterr_timer_running = false;
		cancel_delayed_work_sync(&hosterr_background_work);
		spin_lock_irqsave(&hosterr_cache_lock, flags);
		if (hosterr_cached.sleep == 0)
			do_sleep = true;
		hosterr_sleep = 1;
		hosterr_cached.sleep = 1;
		if (hosterr_cached.max_mode == 1) {
			hosterr_max_mode_saved = true;
			hosterr_max_mode = 0;
			hosterr_cached.max_mode = 0;
		} else {
			hosterr_max_mode_saved = false;
		}
		spin_unlock_irqrestore(&hosterr_cache_lock, flags);
		hosterr_update_cache();
		break;
	case PM_POST_SUSPEND:
		hosterr_timer_running = true;
		spin_lock_irqsave(&hosterr_cache_lock, flags);
		if (hosterr_max_mode_saved) {
			hosterr_max_mode = 1;
			hosterr_cached.max_mode = 1;
			hosterr_max_mode_saved = false;
		}
		if (hosterr_cached.sleep == 1)
			do_wake = true;
		WRITE_ONCE(hosterr_sleep, 0);
		hosterr_cached.sleep = 0;
		spin_unlock_irqrestore(&hosterr_cache_lock, flags);
		schedule_delayed_work(&hosterr_background_work, msecs_to_jiffies(500));
		break;
	}

	return NOTIFY_OK;
}
static struct notifier_block hosterr_pm_nb = {
	.notifier_call = hosterr_pm_callback,
};

/************************ Governor internals ***********************/

/**
 * sugov_iowait_boost() - Updates the IO boost status of a CPU.
 * @sg_cpu: the sugov data for the CPU to boost
 * @time: the update time from the caller
 * @flags: SCHED_CPUFREQ_IOWAIT if the task is waking up after an IO wait
 *
 * Replaces tick-based logic with high-precision time decay.
 */
static void sugov_iowait_boost(struct sugov_cpu *sg_cpu, u64 time,
				   unsigned int flags)
{
	if (sg_cpu->iowait_boost_pending) {
		sg_cpu->iowait_boost_pending = false;
	} else if (sg_cpu->iowait_boost) {
		if ((time - sg_cpu->last_update) > IOWAIT_BOOST_DECAY_NS) {
			sg_cpu->iowait_boost >>= 1;
			if (sg_cpu->iowait_boost < IOWAIT_BOOST_MIN)
				sg_cpu->iowait_boost = 0;
		}
	}
	if (flags & SCHED_CPUFREQ_IOWAIT) {
		if (!sg_cpu->iowait_boost) {
			sg_cpu->iowait_boost = IOWAIT_BOOST_MIN;
		} else {
			sg_cpu->iowait_boost <<= 1;
			if (sg_cpu->iowait_boost > IOWAIT_BOOST_MAX)
				sg_cpu->iowait_boost = IOWAIT_BOOST_MAX;
		}
		sg_cpu->iowait_boost_pending = true;
	}
}
/**
 * sugov_iowait_apply() - Apply the IO boost to a CPU.
 * @sg_cpu: the sugov data for the cpu to boost
 * @time: the update time from the caller
 * @util: The current CPU utilization
 * @max: The maximum capacity of the CPU
 *
 * Applies the calculated IO boost, subject to time-based decay.
 */
static unsigned long sugov_iowait_apply(struct sugov_cpu *sg_cpu, u64 time,
					unsigned long util, unsigned long max)
{
	unsigned long boost = sg_cpu->iowait_boost;
	if (!boost)
		return util;
	if (!sg_cpu->iowait_boost_pending) {
		if ((time - sg_cpu->last_update) > IOWAIT_BOOST_DECAY_NS) {
			boost >>= 1;
			if (boost < IOWAIT_BOOST_MIN)
				boost = 0;
			sg_cpu->iowait_boost = boost;
			if (!boost)
				return util;
		}
	}
	if (boost > max)
		boost = max;
	return (util < boost) ? boost : min(util + boost, max);
}

static bool sugov_should_update_freq(struct sugov_policy *sg_policy, u64 time)
{
	s64 delta_ns;
	s64 rate_limit_ns;
	/*
	 * Since cpufreq_update_util() is called with rq->lock held for
	 * the @target_cpu, our per-CPU data is fully serialized.
	 *
	 * However, drivers cannot in general deal with cross-CPU
	 * requests, so while get_next_freq() will work, our
	 * sugov_update_commit() call may not for the fast switching platforms.
	 *
	 * Hence stop here for remote requests if they aren't supported
	 * by the hardware, as calculating the frequency is pointless if
	 * we cannot in fact act on it.
	 *
	 * This is needed on the slow switching platforms too to prevent CPUs
	 * going offline from leaving stale IRQ work items behind.
	 */
	if (!cpufreq_this_cpu_can_update(sg_policy->policy))
		return false;

	if (unlikely(READ_ONCE(sg_policy->limits_changed))) {
		WRITE_ONCE(sg_policy->limits_changed, false);
		sg_policy->need_freq_update = true;

		/*
		 * The above limits_changed update must occur before the reads
		 * of policy limits in cpufreq_driver_resolve_freq() or a policy
		 * limits update might be missed, so use a memory barrier to
		 * ensure it.
		 *
		 * This pairs with the write memory barrier in sugov_limits().
		 */
		smp_mb();

		return true;
	}

	rate_limit_ns = READ_ONCE(sg_policy->freq_update_delay_ns);
	if (rate_limit_ns <= 0)
		rate_limit_ns = HOSTERR_DEFAULT_RATE_LIMIT_US * NSEC_PER_USEC;
	if (sg_policy->is_oscillating)
		rate_limit_ns *= 3;

	delta_ns = time - sg_policy->last_freq_update_time;
	return delta_ns >= rate_limit_ns;
}

static bool sugov_update_next_freq(struct sugov_policy *sg_policy, u64 time,
				   unsigned int next_freq)
{
	bool should_update = true;

	if (sg_policy->need_freq_update) {
		sg_policy->need_freq_update = false;
		/*
		 * The policy limits have changed, but if the return value of
		 * cpufreq_driver_resolve_freq() after applying the new limits
		 * is still equal to the previously selected frequency, the
		 * driver callback need not be invoked unless the driver
		 * specifically wants that to happen on every update of the
		 * policy limits.
		 */
		if (sg_policy->next_freq == next_freq &&
		    !cpufreq_driver_test_flags(CPUFREQ_NEED_UPDATE_LIMITS))
			return false;
	} else if (sg_policy->next_freq == next_freq) {
		return false;
	}
	if (time - sg_policy->osc_window_start < (5ULL * NSEC_PER_SEC)) {
		int current_dir = (next_freq > sg_policy->next_freq) ? 1 : -1;
		if (sg_policy->last_dir != 0 && current_dir != sg_policy->last_dir)
			sg_policy->osc_change_count++;
		sg_policy->last_dir = current_dir;
		if (sg_policy->osc_change_count > 3)
			sg_policy->is_oscillating = true;
	} else {
		sg_policy->osc_window_start = time;
		sg_policy->osc_change_count = 0;
		sg_policy->is_oscillating = false;
		sg_policy->last_dir = 0;
	}

	trace_android_rvh_set_sugov_update(sg_policy, next_freq, &should_update);
	if (!should_update)
		return false;

	sg_policy->next_freq = next_freq;
	sg_policy->last_freq_update_time = time;

	return true;
}

static void sugov_deferred_update(struct sugov_policy *sg_policy)
{
	if (!sg_policy->work_in_progress) {
		sg_policy->work_in_progress = true;
		irq_work_queue(&sg_policy->irq_work);
	}
}

/**
 * get_next_freq - Compute a new frequency for a given cpufreq policy.
 * @sg_policy: schedutil policy object to compute the new frequency for.
 * @util: Current CPU utilization.
 * @max: CPU capacity.
 *
 * Modified to integrate Hosterr logic:
 * Frame pressure adaptation, sleep/max overrides, predictive utilization
 * curves, and adaptive frequency smoothing filters.
 */
static unsigned int get_next_freq(struct sugov_policy *sg_policy,
				  unsigned long util, unsigned long max)
{
	struct cpufreq_policy *policy = sg_policy->policy;
	unsigned int freq;
	unsigned int prev_raw = sg_policy->cached_raw_freq;
	unsigned int damping;
	bool is_sleep = (READ_ONCE(hosterr_sleep) == 1);
	bool is_max   = (READ_ONCE(hosterr_max_mode) == 1);
	unsigned long frame_boost;
	unsigned int smooth_weight_prev;
	unsigned int smooth_weight_new;
	u64 frame_delay_ns;
	u64 frame_budget_ns;
	if (!policy)
		return 0;
	frame_delay_ns  = READ_ONCE(hosterr_frame_delay_ns);
	frame_budget_ns = READ_ONCE(hosterr_frame_budget_ns);
	if (!frame_budget_ns)
		frame_budget_ns = 16666666ULL;

	hosterr_frame_pressure_update();

	if (util > (max * 3 / 4))
		util = max;

	frame_boost = READ_ONCE(hosterr_frame_boost);
	if (frame_boost) {
		if (frame_boost >= (max - util))
			util = max;
		else
			util += frame_boost;
	}

	if (frame_delay_ns > frame_budget_ns) {
		unsigned long extra = max >> 3;
		if (frame_delay_ns > (frame_budget_ns << 1))
			extra = max >> 2;
		if (util + extra >= max)
			util = max;
		else
			util += extra;
	}

	if (unlikely(is_sleep)) {
		freq = policy->cpuinfo.min_freq;
		goto resolve_freq;
	}

	if (unlikely(is_max)) {
		freq = policy->cpuinfo.max_freq;
		goto resolve_freq;
	}
	util = hosterr_predict_util(sg_policy, util, max);
	util = hosterr_bend_utilization(util, max);
	util = hosterr_dynamic_curve(policy, util, max);
	util = map_util_perf(util);
	freq = map_util_freq(util, policy->cpuinfo.max_freq, max);

	if (max < (SCHED_CAPACITY_SCALE * 3 / 4)) {
		smooth_weight_prev = 7;
		smooth_weight_new  = 3;
	} else {
		smooth_weight_prev = 3;
		smooth_weight_new  = 7;
	}

	if (prev_raw != 0 && (sg_policy->is_oscillating || freq < prev_raw)) {
		freq = ((prev_raw * smooth_weight_prev) + (freq * smooth_weight_new)) / 10;
	}

	damping = READ_ONCE(hosterr_down_damping);
	if (damping > 1 && prev_raw != 0 && freq < prev_raw)
		freq = ((prev_raw * (damping - 1)) + freq) / damping;

resolve_freq:
	if (policy->min > policy->cpuinfo.min_freq)
		policy->min = policy->cpuinfo.min_freq;

	if (freq == sg_policy->cached_raw_freq && !sg_policy->need_freq_update)
		return sg_policy->next_freq;

	sg_policy->cached_raw_freq = freq;
	return cpufreq_driver_resolve_freq(policy, freq);
}

static void sugov_get_util(struct sugov_cpu *sg_cpu)
{
	struct rq *rq = cpu_rq(sg_cpu->cpu);
	unsigned long max = arch_scale_cpu_capacity(sg_cpu->cpu);
	unsigned long util_cfs = cpu_util_cfs(rq);
	unsigned long util;

	sg_cpu->max = max;
	sg_cpu->bw_dl = cpu_bw_dl(rq);
	util = effective_cpu_util(sg_cpu->cpu, util_cfs, max, FREQUENCY_UTIL, NULL);
	
	if (util > util_cfs)
		util = util_cfs;
		
	sg_cpu->util = util;
}

#ifdef CONFIG_NO_HZ_COMMON
static bool sugov_cpu_is_busy(struct sugov_cpu *sg_cpu)
{
	unsigned long idle_calls = tick_nohz_get_idle_calls_cpu(sg_cpu->cpu);
	bool ret = idle_calls == sg_cpu->saved_idle_calls;

	sg_cpu->saved_idle_calls = idle_calls;
	return ret;
}
#else
static inline bool sugov_cpu_is_busy(struct sugov_cpu *sg_cpu) { return false; }
#endif /* CONFIG_NO_HZ_COMMON */

/*
 * Make sugov_should_update_freq() ignore the rate limit when DL
 * has increased the utilization.
 */
static inline void ignore_dl_rate_limit(struct sugov_cpu *sg_cpu)
{
	if (cpu_bw_dl(cpu_rq(sg_cpu->cpu)) > sg_cpu->bw_dl)
		WRITE_ONCE(sg_cpu->sg_policy->limits_changed, true);
}

static inline bool sugov_update_single_common(struct sugov_cpu *sg_cpu,
					      u64 time, unsigned int flags)
{
	sugov_iowait_boost(sg_cpu, time, flags);
	sg_cpu->last_update = time;

	ignore_dl_rate_limit(sg_cpu);

	if (!sugov_should_update_freq(sg_cpu->sg_policy, time))
		return false;

	sugov_get_util(sg_cpu);
	sg_cpu->util = sugov_iowait_apply(sg_cpu, time, sg_cpu->util, sg_cpu->max);

	return true;
}

static void sugov_update_single_freq(struct update_util_data *hook, u64 time,
				     unsigned int flags)
{
	struct sugov_cpu *sg_cpu = container_of(hook, struct sugov_cpu, update_util);
	struct sugov_policy *sg_policy = sg_cpu->sg_policy;
	unsigned int cached_freq = sg_policy->cached_raw_freq;
	unsigned int next_f;

	if (!sugov_update_single_common(sg_cpu, time, flags))
		return;

	next_f = get_next_freq(sg_policy, sg_cpu->util, sg_cpu->max);
	/*
	 * Do not reduce the frequency if the CPU has not been idle
	 * recently, as the reduction is likely to be premature then.
	 */
	if (sugov_cpu_is_busy(sg_cpu) && next_f < sg_policy->next_freq) {
		next_f = sg_policy->next_freq;

		/* Restore cached freq as next_freq has changed */
		sg_policy->cached_raw_freq = cached_freq;
	}

	if (!sugov_update_next_freq(sg_policy, time, next_f))
		return;

	/*
	 * This code runs under rq->lock for the target CPU, so it won't run
	 * concurrently on two different CPUs for the same target and it is not
	 * necessary to acquire the lock in the fast switch case.
	 */
	if (sg_policy->policy->fast_switch_enabled) {
		cpufreq_driver_fast_switch(sg_policy->policy, next_f);
	} else {
		raw_spin_lock(&sg_policy->update_lock);
		sugov_deferred_update(sg_policy);
		raw_spin_unlock(&sg_policy->update_lock);
	}
}

static void sugov_update_single_perf(struct update_util_data *hook, u64 time,
				     unsigned int flags)
{
	struct sugov_cpu *sg_cpu = container_of(hook, struct sugov_cpu, update_util);
	unsigned long prev_util = sg_cpu->util;

	/*
	 * Fall back to the "frequency" path if frequency invariance is not
	 * supported, because the direct mapping between the utilization and
	 * the performance levels depends on the frequency invariance.
	 */
	if (!arch_scale_freq_invariant()) {
		sugov_update_single_freq(hook, time, flags);
		return;
	}

	if (!sugov_update_single_common(sg_cpu, time, flags))
		return;

	/*
	 * Do not reduce the target performance level if the CPU has not been
	 * idle recently, as the reduction is likely to be premature then.
	 */
	if (sugov_cpu_is_busy(sg_cpu) && sg_cpu->util < prev_util)
		sg_cpu->util = prev_util;

	cpufreq_driver_adjust_perf(sg_cpu->cpu, map_util_perf(sg_cpu->bw_dl),
				   map_util_perf(sg_cpu->util), sg_cpu->max);

	sg_cpu->sg_policy->last_freq_update_time = time;
}

static unsigned int sugov_next_freq_shared(struct sugov_cpu *sg_cpu, u64 time)
{
	struct sugov_policy *sg_policy = sg_cpu->sg_policy;
	struct cpufreq_policy *policy = sg_policy->policy;
	unsigned long util = 0, max = 1;
	unsigned int j;

	for_each_cpu(j, policy->cpus) {
		struct sugov_cpu *j_sg_cpu = &per_cpu(sugov_cpu, j);
		unsigned long j_util, j_max, noise_floor;

		sugov_get_util(j_sg_cpu);
		j_sg_cpu->util = sugov_iowait_apply(j_sg_cpu, time, j_sg_cpu->util, j_sg_cpu->max);
		j_util = j_sg_cpu->util;
		j_max = j_sg_cpu->max;
		if (!j_max)
			continue;
		noise_floor = j_max >> 7;
		if (j_util < noise_floor)
			j_util = 0;

		if (j_util * max > j_max * util) {
			util = j_util;
			max = j_max;
		}
	}

	return get_next_freq(sg_policy, util, max);
}

static void
sugov_update_shared(struct update_util_data *hook, u64 time, unsigned int flags)
{
	struct sugov_cpu *sg_cpu = container_of(hook, struct sugov_cpu, update_util);
	struct sugov_policy *sg_policy = sg_cpu->sg_policy;
	unsigned int next_f;

	raw_spin_lock(&sg_policy->update_lock);

	sugov_iowait_boost(sg_cpu, time, flags);
	sg_cpu->last_update = time;

	ignore_dl_rate_limit(sg_cpu);

	if (sugov_should_update_freq(sg_policy, time)) {
		next_f = sugov_next_freq_shared(sg_cpu, time);

		if (!sugov_update_next_freq(sg_policy, time, next_f))
			goto unlock;

		if (sg_policy->policy->fast_switch_enabled)
			cpufreq_driver_fast_switch(sg_policy->policy, next_f);
		else
			sugov_deferred_update(sg_policy);
	}
unlock:
	raw_spin_unlock(&sg_policy->update_lock);
}

static void sugov_work(struct kthread_work *work)
{
	struct sugov_policy *sg_policy = container_of(work, struct sugov_policy, work);
	unsigned int freq;
	unsigned long flags;

	/*
	 * Hold sg_policy->update_lock shortly to handle the case where:
	 * in case sg_policy->next_freq is read here, and then updated by
	 * sugov_deferred_update() just before work_in_progress is set to false
	 * here, we may miss queueing the new update.
	 *
	 * Note: If a work was queued after the update_lock is released,
	 * sugov_work() will just be called again by kthread_work code; and the
	 * request will be proceed before the sugov thread sleeps.
	 */
	raw_spin_lock_irqsave(&sg_policy->update_lock, flags);
	freq = sg_policy->next_freq;
	sg_policy->work_in_progress = false;
	raw_spin_unlock_irqrestore(&sg_policy->update_lock, flags);

	mutex_lock(&sg_policy->work_lock);
	__cpufreq_driver_target(sg_policy->policy, freq, CPUFREQ_RELATION_L);
	mutex_unlock(&sg_policy->work_lock);
}

static void sugov_irq_work(struct irq_work *irq_work)
{
	struct sugov_policy *sg_policy;

	sg_policy = container_of(irq_work, struct sugov_policy, irq_work);

	kthread_queue_work(&sg_policy->worker, &sg_policy->work);
}

/************************** sysfs interface ************************/

static struct sugov_tunables *global_tunables;
static DEFINE_MUTEX(global_tunables_lock);

static inline struct sugov_tunables *to_sugov_tunables(struct gov_attr_set *attr_set)
{
	return container_of(attr_set, struct sugov_tunables, attr_set);
}

static ssize_t rate_limit_us_show(struct gov_attr_set *attr_set, char *buf)
{
	struct sugov_tunables *tunables = to_sugov_tunables(attr_set);

	return sprintf(buf, "%u\n", tunables->rate_limit_us);
}

static ssize_t
rate_limit_us_store(struct gov_attr_set *attr_set, const char *buf, size_t count)
{
	struct sugov_tunables *tunables = to_sugov_tunables(attr_set);
	struct sugov_policy *sg_policy;
	unsigned int rate_limit_us;

	if (kstrtouint(buf, 10, &rate_limit_us))
		return -EINVAL;

	tunables->rate_limit_us = rate_limit_us;

	list_for_each_entry(sg_policy, &attr_set->policy_list, tunables_hook)
		sg_policy->freq_update_delay_ns = rate_limit_us * NSEC_PER_USEC;

	return count;
}

static struct governor_attr rate_limit_us = __ATTR_RW(rate_limit_us);

static struct attribute *sugov_attrs[] = {
	&rate_limit_us.attr,
	NULL
};
ATTRIBUTE_GROUPS(sugov);

static void sugov_tunables_free(struct kobject *kobj)
{
	struct gov_attr_set *attr_set = container_of(kobj, struct gov_attr_set, kobj);

	kfree(to_sugov_tunables(attr_set));
}

static struct kobj_type sugov_tunables_ktype = {
	.default_groups = sugov_groups,
	.sysfs_ops = &governor_sysfs_ops,
	.release = &sugov_tunables_free,
};

/********************** cpufreq governor interface *********************/

struct cpufreq_governor schedutil_gov;

static struct sugov_policy *sugov_policy_alloc(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy = kzalloc(sizeof(*sg_policy), GFP_KERNEL);

	if (!sg_policy)
		return NULL;

	sg_policy->policy = policy;
	raw_spin_lock_init(&sg_policy->update_lock);
	return sg_policy;
}

static void sugov_policy_free(struct sugov_policy *sg_policy)
{
	kfree(sg_policy);
}

static int sugov_kthread_create(struct sugov_policy *sg_policy)
{
	struct task_struct *thread;
	struct sched_attr attr = {
		.size		= sizeof(struct sched_attr),
		.sched_policy	= SCHED_DEADLINE,
		.sched_flags	= SCHED_FLAG_SUGOV,
		.sched_nice	= 0,
		.sched_priority	= 0,
		/*
		 * Fake (unused) bandwidth; workaround to "fix"
		 * priority inheritance.
		 */
		.sched_runtime	=  1000000,
		.sched_deadline = 10000000,
		.sched_period	= 10000000,
	};
	struct cpufreq_policy *policy = sg_policy->policy;
	int ret;

	/* kthread only required for slow path */
	if (policy->fast_switch_enabled)
		return 0;

	trace_android_vh_set_sugov_sched_attr(&attr);
	kthread_init_work(&sg_policy->work, sugov_work);
	kthread_init_worker(&sg_policy->worker);
	thread = kthread_create(kthread_worker_fn, &sg_policy->worker,
				"sugov:%d",
				cpumask_first(policy->related_cpus));
	if (IS_ERR(thread)) {
		pr_err("failed to create sugov thread: %ld\n", PTR_ERR(thread));
		return PTR_ERR(thread);
	}

	ret = sched_setattr_nocheck(thread, &attr);
	if (ret) {
		kthread_stop(thread);
		pr_warn("%s: failed to set SCHED_DEADLINE\n", __func__);
		return ret;
	}

	sg_policy->thread = thread;
	kthread_bind_mask(thread, policy->related_cpus);
	init_irq_work(&sg_policy->irq_work, sugov_irq_work);
	mutex_init(&sg_policy->work_lock);

	wake_up_process(thread);

	return 0;
}

static void sugov_kthread_stop(struct sugov_policy *sg_policy)
{
	/* kthread only required for slow path */
	if (sg_policy->policy->fast_switch_enabled)
		return;

	kthread_flush_worker(&sg_policy->worker);
	kthread_stop(sg_policy->thread);
	mutex_destroy(&sg_policy->work_lock);
}

static struct sugov_tunables *sugov_tunables_alloc(struct sugov_policy *sg_policy)
{
	struct sugov_tunables *tunables = kzalloc(sizeof(*tunables), GFP_KERNEL);

	if (tunables) {
		gov_attr_set_init(&tunables->attr_set, &sg_policy->tunables_hook);
		if (!have_governor_per_policy())
			global_tunables = tunables;
	}
	return tunables;
}

static void sugov_clear_global_tunables(void)
{
	if (!have_governor_per_policy())
		global_tunables = NULL;
}

static int sugov_init(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy;
	struct sugov_tunables *tunables;
	int ret = 0;

	/* State should be equivalent to EXIT */
	if (policy->governor_data)
		return -EBUSY;

	cpufreq_enable_fast_switch(policy);

	sg_policy = sugov_policy_alloc(policy);
	if (!sg_policy) {
		ret = -ENOMEM;
		goto disable_fast_switch;
	}

	ret = sugov_kthread_create(sg_policy);
	if (ret)
		goto free_sg_policy;

	mutex_lock(&global_tunables_lock);

	if (global_tunables) {
		if (WARN_ON(have_governor_per_policy())) {
			ret = -EINVAL;
			goto stop_kthread;
		}
		policy->governor_data = sg_policy;
		sg_policy->tunables = global_tunables;

		gov_attr_set_get(&global_tunables->attr_set, &sg_policy->tunables_hook);
		goto out;
	}

	tunables = sugov_tunables_alloc(sg_policy);
	if (!tunables) {
		ret = -ENOMEM;
		goto stop_kthread;
	}

	tunables->rate_limit_us = cpufreq_policy_transition_delay_us(policy);

	policy->governor_data = sg_policy;
	sg_policy->tunables = tunables;

	ret = kobject_init_and_add(&tunables->attr_set.kobj, &sugov_tunables_ktype,
				   get_governor_parent_kobj(policy), "%s",
				   schedutil_gov.name);
	if (ret)
		goto fail;

out:
	mutex_unlock(&global_tunables_lock);
	return 0;

fail:
	kobject_put(&tunables->attr_set.kobj);
	policy->governor_data = NULL;
	sugov_clear_global_tunables();

stop_kthread:
	sugov_kthread_stop(sg_policy);
	mutex_unlock(&global_tunables_lock);

free_sg_policy:
	sugov_policy_free(sg_policy);

disable_fast_switch:
	cpufreq_disable_fast_switch(policy);

	pr_err("initialization failed (error %d)\n", ret);
	return ret;
}

static void sugov_exit(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy = policy->governor_data;
	struct sugov_tunables *tunables = sg_policy->tunables;
	unsigned int count;

	mutex_lock(&global_tunables_lock);

	count = gov_attr_set_put(&tunables->attr_set, &sg_policy->tunables_hook);
	policy->governor_data = NULL;
	if (!count)
		sugov_clear_global_tunables();

	mutex_unlock(&global_tunables_lock);

	if (hosterr_bd) {
		put_device(&hosterr_bd->dev);
		hosterr_bd = NULL;
	}
	sugov_kthread_stop(sg_policy);
	sugov_policy_free(sg_policy);
	cpufreq_disable_fast_switch(policy);
}

static int sugov_start(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy = policy->governor_data;
	void (*uu)(struct update_util_data *data, u64 time, unsigned int flags);
	unsigned int cpu;

	if (unlikely(!hosterr_works_init)) {
		INIT_DELAYED_WORK(&hosterr_background_work, hosterr_background_handler);
		register_pm_notifier(&hosterr_pm_nb);
		hosterr_works_init = true;

		spin_lock_init(&hosterr_cache_lock);
		hosterr_cached.max_mode	  = hosterr_max_mode;
		hosterr_cached.sleep		 = hosterr_sleep;
		hosterr_cached.down_damping  = hosterr_down_damping;

		if (!hosterr_bd)
			hosterr_bd = backlight_device_get_by_name("panel0-backlight");

		hosterr_timer_running = true;
		schedule_delayed_work(&hosterr_background_work, msecs_to_jiffies(500));
	}

	sg_policy->freq_update_delay_ns  = sg_policy->tunables->rate_limit_us * NSEC_PER_USEC;
	sg_policy->last_freq_update_time = 0;
	sg_policy->next_freq			 = 0;
	sg_policy->work_in_progress	  = false;
	sg_policy->limits_changed		= false;
	sg_policy->cached_raw_freq	   = 0;
	sg_policy->last_util			 = 0;
	sg_policy->need_freq_update	  = cpufreq_driver_test_flags(CPUFREQ_NEED_UPDATE_LIMITS);

	for_each_cpu(cpu, policy->cpus) {
		struct sugov_cpu *sg_cpu = &per_cpu(sugov_cpu, cpu);

		memset(sg_cpu, 0, sizeof(*sg_cpu));
		sg_cpu->cpu			= cpu;
		sg_cpu->sg_policy		= sg_policy;
	}

	if (policy_is_shared(policy))
		uu = sugov_update_shared;
	else if (policy->fast_switch_enabled && cpufreq_driver_has_adjust_perf())
		uu = sugov_update_single_perf;
	else
		uu = sugov_update_single_freq;

	for_each_cpu(cpu, policy->cpus) {
		struct sugov_cpu *sg_cpu = &per_cpu(sugov_cpu, cpu);

		cpufreq_add_update_util_hook(cpu, &sg_cpu->update_util, uu);
	}
	return 0;
}

static void sugov_stop(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy = policy->governor_data;
	unsigned int cpu;

	for_each_cpu(cpu, policy->cpus)
		cpufreq_remove_update_util_hook(cpu);

	synchronize_rcu();

	if (!policy->fast_switch_enabled) {
		irq_work_sync(&sg_policy->irq_work);
		kthread_cancel_work_sync(&sg_policy->work);
	}
}

static void sugov_limits(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy = policy->governor_data;

	if (!policy->fast_switch_enabled) {
		mutex_lock(&sg_policy->work_lock);
		cpufreq_policy_apply_limits(policy);
		mutex_unlock(&sg_policy->work_lock);
	}

	/*
	 * The limits_changed update below must take place before the updates
	 * of policy limits in cpufreq_set_policy() or a policy limits update
	 * might be missed, so use a memory barrier to ensure it.
	 *
	 * This pairs with the memory barrier in sugov_should_update_freq().
	 */
	smp_wmb();

	WRITE_ONCE(sg_policy->limits_changed, true);
}

struct cpufreq_governor schedutil_gov = {
	.name			= "schedutil",
	.owner			= THIS_MODULE,
	.flags			= CPUFREQ_GOV_DYNAMIC_SWITCHING,
	.init			= sugov_init,
	.exit			= sugov_exit,
	.start			= sugov_start,
	.stop			= sugov_stop,
	.limits			= sugov_limits,
};

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_SCHEDUTIL
struct cpufreq_governor *cpufreq_default_governor(void)
{
	return &schedutil_gov;
}
#endif

cpufreq_governor_init(schedutil_gov);
