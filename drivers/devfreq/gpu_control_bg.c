// SPDX-License-Identifier: GPL-2.0
/*
 * GPU Control Background Handler
 *
 * Copyright (C) 2026 ~jkoo
 */

#include <linux/devfreq.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/pm_qos.h>
#include <linux/of_device.h>
#include <linux/math64.h>
#include <linux/suspend.h>
#include <linux/backlight.h>
#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/cpumask.h>

extern unsigned int hosterr_max_mode;
static struct gpu_control_bg *g_bg;
static struct delayed_work init_work;
extern struct atomic_notifier_head hosterr_max_mode_notifier_list;

#define PERCENTAGE_SCALE 100
#define DYNAMIC_CURVE_SHIFT 13
#define MIN_POLLING_MS 10
#define MAX_BEND_SHIFT 31
#define INIT_DELAY_MS 2000
#define RETRY_DELAY_MS 1000
#define DEFAULT_UP_THRESHOLD 30
#define DEFAULT_DOWN_THRESHOLD 20
#define DEFAULT_BEND_SHIFT 1
#define DEFAULT_DYNAMIC_CURVE_SCALE 0
#define DEFAULT_POLLING_MS 50

static inline bool gpu_control_boost(void)
{
	return READ_ONCE(hosterr_max_mode) == 1;
}

struct gpu_control_bg {
	struct devfreq *df;
	unsigned long hw_min_freq;
	unsigned long max_limit;
	unsigned int up_threshold;
	unsigned int down_threshold;
	unsigned int bend_shift;
	unsigned int dynamic_curve_scale;
	unsigned int polling_ms;
	unsigned long target_freq;
	unsigned long last_load;
	int current_idx;
	struct delayed_work work;
	struct mutex lock;
	struct dev_pm_qos_request qos_min_req;
	struct dev_pm_qos_request qos_max_req;
	bool qos_active;
	struct notifier_block pm_nb;
	struct notifier_block bl_nb;
	struct notifier_block transition_nb;
	struct notifier_block max_mode_nb;
	bool suspended;
	bool enabled;
	struct completion init_done;
	bool init_complete;
};

static int gpu_max_mode_notifier(struct notifier_block *nb,
				 unsigned long action, void *data)
{
	struct gpu_control_bg *bg = container_of(nb, struct gpu_control_bg, max_mode_nb);

	if (!bg->enabled)
		return NOTIFY_OK;

	mod_delayed_work(system_freezable_wq, &bg->work, 0);
	return NOTIFY_OK;
}

static inline unsigned long hosterr_bend_utilization(unsigned long util,
						     unsigned long max)
{
	unsigned int shift;
	if (!util || !max)
		return 0;
	if (util > max)
		util = max;
	shift = g_bg->bend_shift;
	if (!shift || util >= mult_frac(max, 7, 8))
		return util;
	return util - (util >> shift);
}

static inline unsigned long hosterr_dynamic_curve(unsigned long util,
						  unsigned long max,
						  unsigned int scale)
{
	if (!scale)
		return util;
	util += (unsigned long)((u64)util * scale >> DYNAMIC_CURVE_SHIFT);
	return min(util, max);
}

static int find_nearest_index(struct devfreq *df, unsigned long freq)
{
	int i, best_idx = 0;
	unsigned long diff, min_diff;
	unsigned long *freq_table;
	unsigned int max_state;

	if (!df->profile)
		return -1;

	freq_table = df->profile->freq_table;
	max_state = df->profile->max_state;

	if (!freq_table || max_state == 0)
		return -1;

	min_diff = abs((long)freq - (long)freq_table[0]);
	for (i = 1; i < max_state; i++) {
		diff = abs((long)freq - (long)freq_table[i]);
		if (diff < min_diff) {
			min_diff = diff;
			best_idx = i;
		}
	}
	return best_idx;
}

static int gpu_transition_notifier(struct notifier_block *nb,
				   unsigned long action, void *data)
{
	struct gpu_control_bg *bg = container_of(nb, struct gpu_control_bg, transition_nb);

	if (action != DEVFREQ_POSTCHANGE || !bg->enabled)
		return NOTIFY_DONE;

	mod_delayed_work(system_freezable_wq, &bg->work, 0);
	return NOTIFY_OK;
}

static void gpu_control_work(struct work_struct *work)
{
	struct gpu_control_bg *bg = container_of(work, struct gpu_control_bg, work.work);
	struct devfreq *df = bg->df;
	struct devfreq_dev_status *stat;
	unsigned long load, bent_load, range;
	unsigned long ideal_freq;
	int ideal_idx, min_idx, max_idx;
	bool ascending, need_step = false;
	unsigned int next_poll = bg->polling_ms;

	if (!df || bg->suspended)
		return;

	mutex_lock(&bg->lock);

	if (!bg->enabled) {
		if (bg->qos_active) {
			dev_pm_qos_update_request(&bg->qos_min_req, 0);
			dev_pm_qos_update_request(&bg->qos_max_req, PM_QOS_MAX_FREQUENCY_DEFAULT_VALUE);
		}
		goto out_unlock;
	}

	if (!df->profile || !df->profile->freq_table || df->profile->max_state == 0) {
		stat = &df->last_status;
		if (stat->total_time != 0)
			bg->last_load = (stat->busy_time * PERCENTAGE_SCALE) / stat->total_time;

		if (gpu_control_boost())
			bg->target_freq = bg->max_limit;
		else if (bg->last_load > bg->up_threshold)
			bg->target_freq = bg->max_limit;
		else if (bg->last_load < bg->down_threshold)
			bg->target_freq = bg->hw_min_freq;
		goto apply_qos;
	}

	ascending = df->profile->freq_table[0] < df->profile->freq_table[df->profile->max_state - 1];
	min_idx = find_nearest_index(df, bg->hw_min_freq);
	max_idx = find_nearest_index(df, bg->max_limit);

	stat = &df->last_status;
	if (stat->total_time == 0) {
		if (gpu_control_boost()) {
			ideal_idx = max_idx;
			goto apply_step;
		}
		goto out_unlock;
	}

	load = (stat->busy_time * PERCENTAGE_SCALE) / stat->total_time;
	bg->last_load = load;

	if (gpu_control_boost()) {
		ideal_idx = max_idx;
	} else if (load > bg->up_threshold) {
		bent_load = hosterr_bend_utilization(load, PERCENTAGE_SCALE);
		bent_load = hosterr_dynamic_curve(bent_load, PERCENTAGE_SCALE, bg->dynamic_curve_scale);
		if (bg->max_limit > bg->hw_min_freq) {
			range = bg->max_limit - bg->hw_min_freq;
			ideal_freq = bg->hw_min_freq + (range * bent_load) / PERCENTAGE_SCALE;
			ideal_idx = find_nearest_index(df, ideal_freq);
		} else {
			ideal_idx = min_idx;
		}
	} else if (load < bg->down_threshold) {
		ideal_idx = min_idx;
	} else {
		ideal_idx = bg->current_idx;
	}

apply_step:
	if (ideal_idx > bg->current_idx) {
		bg->current_idx++;
		need_step = true;
	} else if (ideal_idx < bg->current_idx) {
		bg->current_idx--;
		need_step = true;
	}

	if (ascending) {
		if (bg->current_idx < min_idx)
			bg->current_idx = min_idx;
		if (bg->current_idx > max_idx)
			bg->current_idx = max_idx;
	} else {
		if (bg->current_idx > min_idx)
			bg->current_idx = min_idx;
		if (bg->current_idx < max_idx)
			bg->current_idx = max_idx;
	}

	bg->target_freq = df->profile->freq_table[bg->current_idx];

apply_qos:
	if (bg->qos_active) {
		unsigned long freq = gpu_control_boost() ? bg->max_limit : bg->target_freq;
		dev_pm_qos_update_request(&bg->qos_min_req, freq / 1000);
		dev_pm_qos_update_request(&bg->qos_max_req, freq / 1000);
	}

	if (need_step && !bg->suspended)
		queue_delayed_work(system_freezable_wq, &bg->work, msecs_to_jiffies(next_poll));

out_unlock:
	mutex_unlock(&bg->lock);
}

static int gpu_control_bl_notifier(struct notifier_block *nb, unsigned long event, void *data)
{
	struct gpu_control_bg *bg = container_of(nb, struct gpu_control_bg, bl_nb);
	struct backlight_device *bd = data;

	if (event != BACKLIGHT_UPDATED || !bd)
		return NOTIFY_DONE;

	if (bd->props.brightness == 0) {
		mutex_lock(&bg->lock);
		bg->suspended = true;
		bg->target_freq = bg->hw_min_freq;
		if (bg->qos_active && bg->enabled) {
			dev_pm_qos_update_request(&bg->qos_min_req, bg->target_freq / 1000);
			dev_pm_qos_update_request(&bg->qos_max_req, bg->target_freq / 1000);
		}
		mutex_unlock(&bg->lock);
	} else if (bg->suspended && bg->enabled) {
		bg->suspended = false;
		mod_delayed_work(system_freezable_wq, &bg->work, 0);
	}
	return NOTIFY_OK;
}

static int gpu_control_pm_notifier(struct notifier_block *nb, unsigned long event, void *ptr)
{
	struct gpu_control_bg *bg = container_of(nb, struct gpu_control_bg, pm_nb);

	switch (event) {
	case PM_SUSPEND_PREPARE:
		mutex_lock(&bg->lock);
		bg->suspended = true;
		bg->target_freq = bg->hw_min_freq;
		if (bg->qos_active && bg->enabled) {
			dev_pm_qos_update_request(&bg->qos_min_req, bg->target_freq / 1000);
			dev_pm_qos_update_request(&bg->qos_max_req, bg->target_freq / 1000);
		}
		mutex_unlock(&bg->lock);
		cancel_delayed_work_sync(&bg->work);
		break;
	case PM_POST_SUSPEND:
		bg->suspended = false;
		if (bg->enabled)
			queue_delayed_work(system_freezable_wq, &bg->work, 0);
		break;
	}
	return NOTIFY_DONE;
}

static ssize_t bg_sched_enabled_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	if (!g_bg || !g_bg->init_complete)
		return -ENODEV;
	return sprintf(buf, "%d\n", g_bg->enabled);
}

static ssize_t bg_sched_enabled_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	unsigned int val;

	if (!g_bg || !g_bg->init_complete)
		return -ENODEV;
	if (kstrtouint(buf, 10, &val))
		return -EINVAL;

	mutex_lock(&g_bg->lock);
	g_bg->enabled = !!val;
	mutex_unlock(&g_bg->lock);

	mod_delayed_work(system_freezable_wq, &g_bg->work, 0);
	return count;
}

static ssize_t max_limit_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	if (!g_bg || !g_bg->init_complete)
		return -ENODEV;
	return sprintf(buf, "%lu\n", g_bg->max_limit);
}

static ssize_t max_limit_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	unsigned long val;

	if (!g_bg || !g_bg->init_complete)
		return -ENODEV;
	if (kstrtoul(buf, 10, &val))
		return -EINVAL;

	mutex_lock(&g_bg->lock);
	if (val < g_bg->hw_min_freq) {
		mutex_unlock(&g_bg->lock);
		return -EINVAL;
	}
	g_bg->max_limit = val;
	mutex_unlock(&g_bg->lock);
	return count;
}

static ssize_t up_threshold_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	if (!g_bg || !g_bg->init_complete)
		return -ENODEV;
	return sprintf(buf, "%u\n", g_bg->up_threshold);
}

static ssize_t up_threshold_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	unsigned int val;

	if (!g_bg || !g_bg->init_complete)
		return -ENODEV;
	if (kstrtouint(buf, 10, &val))
		return -EINVAL;
	if (val > PERCENTAGE_SCALE)
		return -EINVAL;

	mutex_lock(&g_bg->lock);
	if (val <= g_bg->down_threshold) {
		mutex_unlock(&g_bg->lock);
		return -EINVAL;
	}
	g_bg->up_threshold = val;
	mutex_unlock(&g_bg->lock);
	return count;
}

static ssize_t down_threshold_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	if (!g_bg || !g_bg->init_complete)
		return -ENODEV;
	return sprintf(buf, "%u\n", g_bg->down_threshold);
}

static ssize_t down_threshold_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	unsigned int val;

	if (!g_bg || !g_bg->init_complete)
		return -ENODEV;
	if (kstrtouint(buf, 10, &val))
		return -EINVAL;
	if (val > PERCENTAGE_SCALE)
		return -EINVAL;

	mutex_lock(&g_bg->lock);
	if (val >= g_bg->up_threshold) {
		mutex_unlock(&g_bg->lock);
		return -EINVAL;
	}
	g_bg->down_threshold = val;
	mutex_unlock(&g_bg->lock);
	return count;
}

static ssize_t bend_shift_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	if (!g_bg || !g_bg->init_complete)
		return -ENODEV;
	return sprintf(buf, "%u\n", g_bg->bend_shift);
}

static ssize_t bend_shift_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	unsigned int val;

	if (!g_bg || !g_bg->init_complete)
		return -ENODEV;
	if (kstrtouint(buf, 10, &val))
		return -EINVAL;
	if (val > MAX_BEND_SHIFT)
		return -EINVAL;

	mutex_lock(&g_bg->lock);
	g_bg->bend_shift = val;
	mutex_unlock(&g_bg->lock);
	return count;
}

static ssize_t dynamic_curve_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	if (!g_bg || !g_bg->init_complete)
		return -ENODEV;
	return sprintf(buf, "%u\n", g_bg->dynamic_curve_scale);
}

static ssize_t dynamic_curve_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	unsigned int val;

	if (!g_bg || !g_bg->init_complete)
		return -ENODEV;
	if (kstrtouint(buf, 10, &val))
		return -EINVAL;

	mutex_lock(&g_bg->lock);
	g_bg->dynamic_curve_scale = val;
	mutex_unlock(&g_bg->lock);
	return count;
}

static ssize_t polling_ms_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	if (!g_bg || !g_bg->init_complete)
		return -ENODEV;
	return sprintf(buf, "%u\n", g_bg->polling_ms);
}

static ssize_t polling_ms_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	unsigned int val;

	if (!g_bg || !g_bg->init_complete)
		return -ENODEV;
	if (kstrtouint(buf, 10, &val))
		return -EINVAL;
	if (val < MIN_POLLING_MS)
		val = MIN_POLLING_MS;

	mutex_lock(&g_bg->lock);
	g_bg->polling_ms = val;
	mutex_unlock(&g_bg->lock);
	return count;
}

static ssize_t current_load_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	if (!g_bg || !g_bg->init_complete)
		return -ENODEV;
	return sprintf(buf, "%lu\n", g_bg->last_load);
}

static ssize_t current_target_freq_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	if (!g_bg || !g_bg->init_complete)
		return -ENODEV;
	return sprintf(buf, "%lu\n", g_bg->target_freq);
}

static DEVICE_ATTR_RW(bg_sched_enabled);
static DEVICE_ATTR_RW(max_limit);
static DEVICE_ATTR_RW(up_threshold);
static DEVICE_ATTR_RW(down_threshold);
static DEVICE_ATTR_RW(bend_shift);
static DEVICE_ATTR_RW(dynamic_curve);
static DEVICE_ATTR_RW(polling_ms);
static DEVICE_ATTR_RO(current_load);
static DEVICE_ATTR_RO(current_target_freq);

static struct attribute *gpu_control_attrs[] = {
	&dev_attr_bg_sched_enabled.attr,
	&dev_attr_max_limit.attr,
	&dev_attr_up_threshold.attr,
	&dev_attr_down_threshold.attr,
	&dev_attr_bend_shift.attr,
	&dev_attr_dynamic_curve.attr,
	&dev_attr_polling_ms.attr,
	&dev_attr_current_load.attr,
	&dev_attr_current_target_freq.attr,
	NULL,
};

static const struct attribute_group gpu_control_group = {
	.name = "gpu_control",
	.attrs = gpu_control_attrs,
};

static void gpu_control_cleanup(struct gpu_control_bg *bg)
{
	if (!bg)
		return;

	cancel_delayed_work_sync(&bg->work);
	cancel_delayed_work_sync(&init_work);

	if (bg->qos_active) {
		dev_pm_qos_remove_request(&bg->qos_max_req);
		dev_pm_qos_remove_request(&bg->qos_min_req);
		bg->qos_active = false;
	}

	if (bg->df)
		sysfs_remove_group(&bg->df->dev.kobj, &gpu_control_group);

	unregister_pm_notifier(&bg->pm_nb);
	backlight_unregister_notifier(&bg->bl_nb);
	atomic_notifier_chain_unregister(&hosterr_max_mode_notifier_list, &bg->max_mode_nb);

	if (bg->df)
		devfreq_unregister_notifier(bg->df, &bg->transition_nb, DEVFREQ_TRANSITION_NOTIFIER);

	mutex_destroy(&bg->lock);
	kfree(bg);
}

static void gpu_control_init_work(struct work_struct *work)
{
	struct device_node *np;
	struct devfreq *df;
	unsigned long *ft;
	unsigned int ms;

	np = of_find_compatible_node(NULL, NULL, "qcom,kgsl-3d0");
	if (!np) {
		queue_delayed_work(system_wq, &init_work, msecs_to_jiffies(RETRY_DELAY_MS));
		return;
	}

	df = devfreq_get_devfreq_by_node(np);
	of_node_put(np);

	if (IS_ERR_OR_NULL(df)) {
		queue_delayed_work(system_wq, &init_work, msecs_to_jiffies(RETRY_DELAY_MS));
		return;
	}

	g_bg = kzalloc(sizeof(*g_bg), GFP_KERNEL);
	if (!g_bg)
		return;

	init_completion(&g_bg->init_done);
	g_bg->df = df;
	mutex_init(&g_bg->lock);

	g_bg->transition_nb.notifier_call = gpu_transition_notifier;
	if (devfreq_register_notifier(g_bg->df, &g_bg->transition_nb, DEVFREQ_TRANSITION_NOTIFIER)) {
		kfree(g_bg);
		g_bg = NULL;
		return;
	}

	g_bg->up_threshold = DEFAULT_UP_THRESHOLD;
	g_bg->down_threshold = DEFAULT_DOWN_THRESHOLD;
	g_bg->bend_shift = DEFAULT_BEND_SHIFT;
	g_bg->dynamic_curve_scale = DEFAULT_DYNAMIC_CURVE_SCALE;
	g_bg->polling_ms = DEFAULT_POLLING_MS;
	g_bg->enabled = true;

	if (df->profile && df->profile->freq_table && df->profile->max_state > 0) {
		ft = df->profile->freq_table;
		ms = df->profile->max_state;
		if (ft[0] < ft[ms - 1]) {
			g_bg->hw_min_freq = ft[0];
			g_bg->max_limit = ft[ms - 1];
		} else {
			g_bg->hw_min_freq = ft[ms - 1];
			g_bg->max_limit = ft[0];
		}
	} else {
		g_bg->hw_min_freq = df->scaling_min_freq;
		g_bg->max_limit = df->scaling_max_freq;
	}

	g_bg->target_freq = g_bg->hw_min_freq;
	g_bg->current_idx = find_nearest_index(df, g_bg->target_freq);

	if (sysfs_create_group(&df->dev.kobj, &gpu_control_group)) {
		devfreq_unregister_notifier(df, &g_bg->transition_nb, DEVFREQ_TRANSITION_NOTIFIER);
		kfree(g_bg);
		g_bg = NULL;
		return;
	}

	if (dev_pm_qos_add_request(df->dev.parent, &g_bg->qos_min_req,
				   DEV_PM_QOS_MIN_FREQUENCY, g_bg->target_freq / 1000) >= 0 &&
	    dev_pm_qos_add_request(df->dev.parent, &g_bg->qos_max_req,
				   DEV_PM_QOS_MAX_FREQUENCY, g_bg->target_freq / 1000) >= 0) {
		g_bg->qos_active = true;
	}

	g_bg->pm_nb.notifier_call = gpu_control_pm_notifier;
	register_pm_notifier(&g_bg->pm_nb);

	g_bg->bl_nb.notifier_call = gpu_control_bl_notifier;
	backlight_register_notifier(&g_bg->bl_nb);

	g_bg->max_mode_nb.notifier_call = gpu_max_mode_notifier;
	atomic_notifier_chain_register(&hosterr_max_mode_notifier_list, &g_bg->max_mode_nb);

	INIT_DELAYED_WORK(&g_bg->work, gpu_control_work);
	queue_delayed_work(system_freezable_wq, &g_bg->work, 0);

	g_bg->init_complete = true;
	complete_all(&g_bg->init_done);
}

static int __init gpu_control_bg_init(void)
{
	INIT_DELAYED_WORK(&init_work, gpu_control_init_work);
	queue_delayed_work(system_wq, &init_work, msecs_to_jiffies(INIT_DELAY_MS));
	return 0;
}

static void __exit gpu_control_bg_exit(void)
{
	struct gpu_control_bg *bg = g_bg;

	g_bg = NULL;
	synchronize_rcu();
	gpu_control_cleanup(bg);
}

module_init(gpu_control_bg_init);
module_exit(gpu_control_bg_exit);

MODULE_AUTHOR("~jkoo");
MODULE_DESCRIPTION("GPU Control Background Handler");
MODULE_LICENSE("GPL v2");
