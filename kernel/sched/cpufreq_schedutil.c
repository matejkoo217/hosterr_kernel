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
#define HOSTERR_DEFAULT_RATE_LIMIT_US   2000U
#define HOSTERR_NUM_CORES       8
#define HOSTERR_CORE_WHITELIST  ((1U << 0) | (1U << 1))

struct hosterr_cache {
	unsigned int max_mode;
	unsigned int sleep;
	unsigned int down_damping;
	unsigned int bend_shift;
};
unsigned int hosterr_max_mode	  = 0;

ATOMIC_NOTIFIER_HEAD(hosterr_max_mode_notifier_list);

static void update_hosterr_max_mode(unsigned int val)
{
	if (READ_ONCE(hosterr_max_mode) == val)
		return;
	WRITE_ONCE(hosterr_max_mode, val);
	atomic_notifier_call_chain(&hosterr_max_mode_notifier_list, val, NULL);
}

unsigned int hosterr_sleep		 = 0;
static unsigned int hosterr_down_damping  = 4;
static unsigned int hosterr_bend_shift	 = 3;
#define IOWAIT_BOOST_MIN	(SCHED_CAPACITY_SCALE / 8)
static struct backlight_device *hosterr_bd = NULL;
static bool hosterr_max_mode_saved = false;
static struct hosterr_cache hosterr_cached;
static DEFINE_SPINLOCK(hosterr_cache_lock);
static struct delayed_work hosterr_background_work;
static atomic_t hosterr_timer_running = ATOMIC_INIT(0);
static bool hosterr_works_init = false;
static int hosterr_zero_brightness_count = 0;
static atomic_t hosterr_brightness_cached = ATOMIC_INIT(-1);
static DEFINE_MUTEX(hosterr_init_lock);
static int hosterr_policy_count = 0;
static bool hosterr_bl_registered = false;
static enum cpuhp_state hosterr_hp_state = 0;

unsigned int hosterr_core_on[HOSTERR_NUM_CORES] = {
    [0 ... HOSTERR_NUM_CORES - 1] = 1
};

#define HOSTERR_REQ_USER   (1U << 0)
#define HOSTERR_REQ_SYSTEM (1U << 1)

static unsigned int hosterr_core_reqs[HOSTERR_NUM_CORES] = {
	[0 ... HOSTERR_NUM_CORES - 1] = HOSTERR_REQ_USER | HOSTERR_REQ_SYSTEM
};

static int hosterr_core_control(unsigned int cpu, unsigned int mask, bool on)
{
	unsigned long flags;
	unsigned int old_reqs, new_reqs;
	bool should_be_on, was_on;
	struct device *dev = get_cpu_device(cpu);
	int ret = 0;
	if (!dev || cpu >= HOSTERR_NUM_CORES)
		return -ENODEV;
	if (mask == HOSTERR_REQ_USER && !on && (HOSTERR_CORE_WHITELIST & (1U << cpu)))
		return -EPERM;
	spin_lock_irqsave(&hosterr_cache_lock, flags);
	old_reqs = hosterr_core_reqs[cpu];
	if (on)
		new_reqs = old_reqs | mask;
	else
		new_reqs = old_reqs & ~mask;
	if (new_reqs == old_reqs) {
		spin_unlock_irqrestore(&hosterr_cache_lock, flags);
		return 0;
	}
	hosterr_core_reqs[cpu] = new_reqs;
	should_be_on = (new_reqs == (HOSTERR_REQ_USER | HOSTERR_REQ_SYSTEM));
	was_on = (old_reqs == (HOSTERR_REQ_USER | HOSTERR_REQ_SYSTEM));
	if (should_be_on == was_on) {
		spin_unlock_irqrestore(&hosterr_cache_lock, flags);
		return 0;
	}
	hosterr_core_on[cpu] = should_be_on;
	spin_unlock_irqrestore(&hosterr_cache_lock, flags);
	if (should_be_on) {
		ret = device_online(dev);
		if (ret) {
			spin_lock_irqsave(&hosterr_cache_lock, flags);
			hosterr_core_on[cpu] = 0;
			spin_unlock_irqrestore(&hosterr_cache_lock, flags);
		}
	} else {
		ret = device_offline(dev);
	}
	return ret;
}

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
	unsigned int		calm_window_count;
	unsigned int		dynamic_curve_scale;

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
#define HOSTERR_PARAM_SYNC_SET(name, var) \
static int hosterr_##name##_sync_set(const char *val, const struct kernel_param *kp) \
{ \
	unsigned int v; \
	unsigned long flags; \
	int ret = kstrtouint(val, 10, &v); \
	if (ret) \
		return ret; \
	ret = param_set_uint(val, kp); \
	if (!ret) { \
		spin_lock_irqsave(&hosterr_cache_lock, flags); \
		hosterr_cached.name = var; \
		if (!strcmp(#name, "max_mode")) \
			update_hosterr_max_mode(var); \
		spin_unlock_irqrestore(&hosterr_cache_lock, flags); \
	} \
	return ret; \
} \
static const struct kernel_param_ops hosterr_##name##_ops = { \
	.set = hosterr_##name##_sync_set, \
	.get = param_get_uint, \
};
#define HOSTERR_PARAM_SYNC_SET_BOUNDS(name, var, min_val, max_val) \
static int hosterr_##name##_sync_set_bounds(const char *val, const struct kernel_param *kp) \
{ \
    unsigned int v; \
    unsigned long flags; \
    int ret = kstrtouint(val, 10, &v); \
    if (ret) \
        return ret; \
    if (v < (min_val) || v > (max_val)) \
        return -EINVAL; \
    ret = param_set_uint(val, kp); \
    if (!ret) { \
        spin_lock_irqsave(&hosterr_cache_lock, flags); \
        hosterr_cached.name = var; \
        spin_unlock_irqrestore(&hosterr_cache_lock, flags); \
    } \
    return ret; \
} \
static const struct kernel_param_ops hosterr_##name##_ops = { \
	.set = hosterr_##name##_sync_set_bounds, \
	.get = param_get_uint, \
};
HOSTERR_PARAM_SYNC_SET(max_mode, hosterr_max_mode)
HOSTERR_PARAM_SYNC_SET(sleep, hosterr_sleep)
HOSTERR_PARAM_SYNC_SET(bend_shift, hosterr_bend_shift)
HOSTERR_PARAM_SYNC_SET_BOUNDS(down_damping, hosterr_down_damping, 2, 32)
module_param_cb(max_mode, &hosterr_max_mode_ops, &hosterr_max_mode, 0644);
module_param_cb(sleep, &hosterr_sleep_ops, &hosterr_sleep, 0644);
module_param_cb(bend_shift, &hosterr_bend_shift_ops, &hosterr_bend_shift, 0644);
module_param_cb(down_damping, &hosterr_down_damping_ops, &hosterr_down_damping, 0644);

static int hosterr_cpu_online_prep(unsigned int cpu)
{
    if (cpu >= HOSTERR_NUM_CORES)
        return 0;
    if (!READ_ONCE(hosterr_core_on[cpu])) {
        return -EINVAL;
    }
    return 0;
}

#define HOSTERR_CORE_PARAM(n) \
static int hosterr_core##n##_set(const char *val, \
                                 const struct kernel_param *kp) \
{ \
    unsigned int v; \
    int ret; \
    ret = kstrtouint(val, 10, &v); \
    if (ret) \
        return ret; \
    return hosterr_core_control(n, HOSTERR_REQ_USER, !!v); \
} \
static const struct kernel_param_ops hosterr_core##n##_ops = { \
    .set = hosterr_core##n##_set, \
    .get = param_get_uint, \
}; \
module_param_cb(core##n##_on, &hosterr_core##n##_ops, \
                &hosterr_core_on[n], 0644)
HOSTERR_CORE_PARAM(0);
HOSTERR_CORE_PARAM(1);
HOSTERR_CORE_PARAM(2);
HOSTERR_CORE_PARAM(3);
HOSTERR_CORE_PARAM(4);
HOSTERR_CORE_PARAM(5);
HOSTERR_CORE_PARAM(6);
HOSTERR_CORE_PARAM(7);

static inline unsigned long hosterr_bend_utilization(unsigned long util,
						     unsigned long max)
{
	unsigned int shift;
	if (!util || !max)
		return 0;
	if (util > max)
		util = max;
	shift = READ_ONCE(hosterr_cached.bend_shift);
	if (!shift || util >= mult_frac(max, 7, 8))
		return util;
	return util - (util >> shift);
}

#define PREDICT_NOISE_FLOOR (SCHED_CAPACITY_SCALE >> 6)
#define OSC_MIN_DELTA(sg) ((sg)->policy->cpuinfo.max_freq >> 4) // ~6% of max

static inline unsigned long hosterr_predict_util(struct sugov_policy *sg_policy,
                                      unsigned long util, unsigned long max)
{
    long delta = (long)util - (long)sg_policy->last_util;
    long predicted;
    if (abs(delta) > PREDICT_NOISE_FLOOR)
        predicted = (long)util + (delta >> 3);
    else
        predicted = (long)util;
    if (predicted > (long)max)
        predicted = max;
    if (predicted < 0)
        predicted = 0;
    sg_policy->last_util = util;
    return (unsigned long)predicted;
}

static inline unsigned long hosterr_dynamic_curve(struct sugov_policy *sg_policy,
				       unsigned long util,
				       unsigned long max)
{
	unsigned int scale = sg_policy->dynamic_curve_scale;

	if (!scale)
		return util;
	util += (unsigned long)((u64)util * scale >> 13);
	return min(util, max);
}

static int hosterr_bl_notifier(struct notifier_block *nb,
                unsigned long event, void *data)
{
    struct backlight_device *bd = data;
    if (!bd || strcmp(dev_name(&bd->dev), "panel0-backlight"))
        return NOTIFY_DONE;
    if (event == BACKLIGHT_UPDATED) {
        atomic_set(&hosterr_brightness_cached, bd->props.brightness);
        mod_delayed_work(system_wq, &hosterr_background_work, 0);
    }
    return NOTIFY_OK;
}
static struct notifier_block hosterr_bl_nb = {
	.notifier_call = hosterr_bl_notifier,
};

static void hosterr_background_handler(struct work_struct *work)
{
    unsigned long flags;
    int brightness;
    int target_core7_state = -1;
    struct backlight_device *bd_local;
    bd_local = smp_load_acquire(&hosterr_bd);
    if (!bd_local) {
        mutex_lock(&hosterr_init_lock);
        bd_local = hosterr_bd;
        if (!bd_local) {
            bd_local = backlight_device_get_by_name("panel0-backlight");
            if (bd_local) {
                backlight_register_notifier(&hosterr_bl_nb);
                hosterr_bl_registered = true;
                smp_store_release(&hosterr_bd, bd_local);
            }
        }
        mutex_unlock(&hosterr_init_lock);
    }
    if (bd_local) {
        brightness = bd_local->props.brightness;
        atomic_set(&hosterr_brightness_cached, brightness);
    } else {
        brightness = atomic_read(&hosterr_brightness_cached);
    }
    if (brightness < 0)
        return;
    spin_lock_irqsave(&hosterr_cache_lock, flags);
    if (brightness == 0 && hosterr_cached.sleep == 0) {
        hosterr_zero_brightness_count++;
        if (hosterr_zero_brightness_count >= 2) {
            hosterr_zero_brightness_count = 0;
            WRITE_ONCE(hosterr_sleep, 1);
            hosterr_cached.sleep = 1;
            target_core7_state = 0;
            if (hosterr_cached.max_mode == 1) {
                hosterr_max_mode_saved = true;
                update_hosterr_max_mode(0);
                hosterr_cached.max_mode = 0;
            }
        } else {
            if (atomic_read(&hosterr_timer_running)) {
                schedule_delayed_work(&hosterr_background_work, msecs_to_jiffies(2000));
            }
        }
    } else if (brightness > 0 && (hosterr_cached.sleep == 1 || !READ_ONCE(hosterr_core_on[7]))) {
        hosterr_zero_brightness_count = 0;
        WRITE_ONCE(hosterr_sleep, 0);
        hosterr_cached.sleep = 0;
        target_core7_state = 1;
        if (hosterr_max_mode_saved) {
            update_hosterr_max_mode(1);
            hosterr_cached.max_mode = 1;
            hosterr_max_mode_saved = false;
        }
    } else {
        hosterr_zero_brightness_count = 0;
    }
    spin_unlock_irqrestore(&hosterr_cache_lock, flags);
    if (target_core7_state != -1)
        hosterr_core_control(7, HOSTERR_REQ_SYSTEM, !!target_core7_state);
}

static int hosterr_pm_callback(struct notifier_block *nb, unsigned long action,
				   void *ptr)
{
    unsigned long flags;
    switch (action) {
    case PM_SUSPEND_PREPARE:
        spin_lock_irqsave(&hosterr_cache_lock, flags);
        WRITE_ONCE(hosterr_sleep, 1);
        hosterr_cached.sleep = 1;
        if (hosterr_cached.max_mode == 1) {
            hosterr_max_mode_saved = true;
            update_hosterr_max_mode(0);
            hosterr_cached.max_mode = 0;
        } else {
            hosterr_max_mode_saved = false;
        }
        spin_unlock_irqrestore(&hosterr_cache_lock, flags);
        atomic_set(&hosterr_timer_running, 0);
        cancel_delayed_work_sync(&hosterr_background_work);
        break;
    case PM_POST_SUSPEND:
        atomic_set(&hosterr_timer_running, 1);
        spin_lock_irqsave(&hosterr_cache_lock, flags);
        if (hosterr_max_mode_saved) {
            update_hosterr_max_mode(1);
            hosterr_cached.max_mode = 1;
            hosterr_max_mode_saved = false;
        }
        WRITE_ONCE(hosterr_sleep, 0);
        hosterr_cached.sleep = 0;
        hosterr_zero_brightness_count = 0;
        spin_unlock_irqrestore(&hosterr_cache_lock, flags);
        hosterr_core_control(7, HOSTERR_REQ_SYSTEM, true);
        queue_delayed_work(system_wq, &hosterr_background_work, msecs_to_jiffies(500));
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
			sg_cpu->iowait_boost += (sg_cpu->max >> 3);
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

    if (!cpufreq_this_cpu_can_update(sg_policy->policy))
        return false;

    /* Use smp_load_acquire to properly pair with smp_store_release
     * in sugov_limits(). Ensures we see all policy limit updates
     * before observing limits_changed = true.
     */
    if (unlikely(smp_load_acquire(&sg_policy->limits_changed))) {
        /* smp_store_release ensures the write is visible after
         * all prior reads of policy limits in this function.
         */
        smp_store_release(&sg_policy->limits_changed, false);
        sg_policy->need_freq_update = true;
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
        if (sg_policy->last_dir != 0 && current_dir != sg_policy->last_dir) {
            unsigned int delta = abs((long)next_freq - (long)sg_policy->next_freq);
            if (delta > OSC_MIN_DELTA(sg_policy))
                sg_policy->osc_change_count++;
        }
		sg_policy->last_dir = current_dir;
		if (sg_policy->osc_change_count > 6)
			sg_policy->is_oscillating = true;
	} else {
		sg_policy->osc_window_start = time;
		sg_policy->osc_change_count = 0;
		sg_policy->is_oscillating = false;
		sg_policy->last_dir = 0;
		sg_policy->calm_window_count = 0;
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

static unsigned int sugov_apply_smoothing(struct sugov_policy *sg_policy,
					 unsigned int freq, unsigned int prev_raw)
{
	unsigned int w_prev, w_new, damping;
	bool do_smooth = false;
	bool is_prime = (arch_scale_cpu_capacity(sg_policy->policy->cpu) >= SCHED_CAPACITY_SCALE);
	bool is_oscillating = sg_policy->is_oscillating;
	if (is_prime) {
		if (freq > prev_raw) {
			w_prev = 7;
			w_new  = 3;
			do_smooth = true;
		} else {
			w_prev = 0;
			w_new  = 10;
			do_smooth = (freq < prev_raw) || is_oscillating;
		}
	} else {
		w_prev = 7;
		w_new  = 3;
		do_smooth = (freq < prev_raw) || is_oscillating;
	}
	if (do_smooth) {
		u64 acc = (u64)prev_raw * w_prev + (u64)freq * w_new;
		freq = (unsigned int)(acc / 10);
	}
	if (freq > prev_raw) {
		freq = prev_raw + ((freq - prev_raw) >> 1);
	} else if (freq < prev_raw) {
		damping = READ_ONCE(hosterr_cached.down_damping);
		if (damping > 1) {
			if (likely(damping == 4)) {
				if (freq < (sg_policy->policy->cpuinfo.min_freq << 1))
					freq = (prev_raw + (freq * 3)) >> 2;
				else if (arch_scale_cpu_capacity(sg_policy->policy->cpu) >= (SCHED_CAPACITY_SCALE * 3 / 4))
					freq = ((prev_raw * 2) + freq) / 3;
				else
					freq = ((prev_raw * 3) + freq) >> 2;
			} else if (is_power_of_2(damping)) {
				unsigned int shift = ilog2(damping);
				freq = ((prev_raw * (damping - 1)) + freq) >> shift;
			} else {
				freq = ((prev_raw * (damping - 1)) + freq) / damping;
			}
		}
		{
			unsigned int drop = prev_raw - freq;
			unsigned int max_drop = (prev_raw * 512) >> 10;
			if (drop > max_drop)
				freq = prev_raw - max_drop;
		}
		if (unlikely(is_oscillating)) {
			unsigned int osc_limit = (prev_raw * 512) >> 10;
			if (freq < osc_limit && freq > (sg_policy->policy->cpuinfo.min_freq << 1))
				freq = osc_limit;
		}
	}
	return freq;
}

/**
 * get_next_freq - Compute a new frequency for a given cpufreq policy.
 * @sg_policy: schedutil policy object to compute the new frequency for.
 * @time: the update time from the caller
 * @util: Current CPU utilization.
 * @max: CPU capacity.
 *
 * Modified to integrate Hosterr logic:
 * Frame pressure adaptation, sleep/max overrides, predictive utilization
 * curves, and adaptive frequency smoothing filters.
 */
static unsigned int get_next_freq(struct sugov_policy *sg_policy, u64 time,
				  unsigned long util, unsigned long max)
{
	struct cpufreq_policy *policy = sg_policy->policy;
	unsigned int freq, prev_raw;
	if (unlikely(!policy))
		return 0;
	if (unlikely(READ_ONCE(hosterr_cached.sleep))) {
		policy->min = policy->cpuinfo.min_freq;
		freq = policy->cpuinfo.min_freq;
		goto resolve;
	}
	if (unlikely(READ_ONCE(hosterr_cached.max_mode))) {
		freq = policy->cpuinfo.max_freq;
		goto resolve;
	}
	if (policy->cpu == 7) {
        policy->min = policy->cpuinfo.min_freq;
    }
	prev_raw = sg_policy->cached_raw_freq;
	util = hosterr_predict_util(sg_policy, util, max);
	util = hosterr_bend_utilization(util, max);
	util = hosterr_dynamic_curve(sg_policy, util, max);
	util = map_util_perf(util);
	freq = map_util_freq(util, policy->cpuinfo.max_freq, max);
	if (likely(prev_raw))
		freq = sugov_apply_smoothing(sg_policy, freq, prev_raw);
resolve:
	if (freq == sg_policy->cached_raw_freq && likely(!sg_policy->need_freq_update))
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
	if (!sugov_should_update_freq(sg_cpu->sg_policy, time))
		return false;
	sugov_iowait_boost(sg_cpu, time, flags);
	sg_cpu->last_update = time;

	ignore_dl_rate_limit(sg_cpu);

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

	next_f = get_next_freq(sg_policy, time, sg_cpu->util, sg_cpu->max);
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

	return get_next_freq(sg_policy, time, util, max);
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
	mutex_lock(&hosterr_init_lock);
	if (--hosterr_policy_count == 0 && hosterr_works_init) {
		if (hosterr_hp_state > 0) {
            cpuhp_remove_state_nocalls(hosterr_hp_state);
            hosterr_hp_state = 0;
        }
		atomic_set(&hosterr_timer_running, 0);
		cancel_delayed_work_sync(&hosterr_background_work);
		unregister_pm_notifier(&hosterr_pm_nb);
		if (hosterr_bl_registered) {
			backlight_unregister_notifier(&hosterr_bl_nb);
			hosterr_bl_registered = false;
		}
		if (hosterr_bd) {
			put_device(&hosterr_bd->dev);
			hosterr_bd = NULL;
		}
		hosterr_works_init = false;
	}
	mutex_unlock(&hosterr_init_lock);
	sugov_kthread_stop(sg_policy);
	sugov_policy_free(sg_policy);
	cpufreq_disable_fast_switch(policy);
}

static int sugov_start(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy = policy->governor_data;
	void (*uu)(struct update_util_data *data, u64 time, unsigned int flags);
	unsigned int cpu;
	unsigned long flags;
	int hp_ret;
	mutex_lock(&hosterr_init_lock);
	if (!hosterr_works_init) {
		INIT_DELAYED_WORK(&hosterr_background_work, hosterr_background_handler);
		register_pm_notifier(&hosterr_pm_nb);
		spin_lock_init(&hosterr_cache_lock);
		spin_lock_irqsave(&hosterr_cache_lock, flags);
		hosterr_cached.max_mode	  = hosterr_max_mode;
		hosterr_cached.sleep		 = hosterr_sleep;
		hosterr_cached.down_damping  = hosterr_down_damping;
		hosterr_cached.bend_shift    = hosterr_bend_shift;
		spin_unlock_irqrestore(&hosterr_cache_lock, flags);

		if (!hosterr_bd)
			hosterr_bd = backlight_device_get_by_name("panel0-backlight");

		if (hosterr_bd) {
			backlight_register_notifier(&hosterr_bl_nb);
			hosterr_bl_registered = true;
		}
		hp_ret = cpuhp_setup_state_nocalls(CPUHP_BP_PREPARE_DYN, "cpufreq/hosterr:prepare",
                                           hosterr_cpu_online_prep, NULL);
        if (hp_ret >= 0)
            hosterr_hp_state = hp_ret;
		atomic_set(&hosterr_timer_running, 1);
        queue_delayed_work(system_wq, &hosterr_background_work, msecs_to_jiffies(500));
		hosterr_works_init = true;
	}

	hosterr_policy_count++;
	mutex_unlock(&hosterr_init_lock);
	sg_policy->freq_update_delay_ns  = sg_policy->tunables->rate_limit_us * NSEC_PER_USEC;
	sg_policy->last_freq_update_time = 0;
	sg_policy->next_freq			 = 0;
	sg_policy->work_in_progress	  = false;
	sg_policy->limits_changed		= false;
	sg_policy->cached_raw_freq	   = 0;
	sg_policy->last_util			 = 0;
	sg_policy->calm_window_count	 = 0;
	sg_policy->need_freq_update	  = cpufreq_driver_test_flags(CPUFREQ_NEED_UPDATE_LIMITS);
	{
		unsigned long window = policy->max - policy->min;

		if (!window || window < (policy->cpuinfo.max_freq >> 1)) {
			sg_policy->dynamic_curve_scale = 0;
		} else {
			unsigned long scale = ((u64)policy->max << 10) / window;
			unsigned long max_scale = (arch_scale_cpu_capacity(policy->cpu) >= (SCHED_CAPACITY_SCALE * 3 / 4)) ? 1024 : 2048;
			if (scale > max_scale)
				scale = max_scale;
			sg_policy->dynamic_curve_scale = (unsigned int)scale;
		}
	}
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
    unsigned long window;

    if (!policy->fast_switch_enabled) {
        mutex_lock(&sg_policy->work_lock);
        cpufreq_policy_apply_limits(policy);
        mutex_unlock(&sg_policy->work_lock);
    }
    window = policy->max - policy->min;
    if (!window || window < (policy->cpuinfo.max_freq >> 1)) {
        sg_policy->dynamic_curve_scale = 0;
    } else {
        unsigned long scale = (policy->max << 10) / window;
        unsigned long max_scale = (arch_scale_cpu_capacity(policy->cpu) >= (SCHED_CAPACITY_SCALE * 3 / 4)) ? 1024 : 2048;
        if (scale > max_scale)
            scale = max_scale;
        sg_policy->dynamic_curve_scale = (unsigned int)scale;
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
EXPORT_SYMBOL_GPL(hosterr_core_on);
EXPORT_SYMBOL_GPL(hosterr_max_mode);
EXPORT_SYMBOL_GPL(hosterr_max_mode_notifier_list);
