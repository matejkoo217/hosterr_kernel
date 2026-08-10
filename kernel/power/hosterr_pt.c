// SPDX-License-Identifier: GPL-2.0
/*
 * HosterrPT - Performance and Efficiency Tuning Kernel Driver
 *
 * Credits & Inspiration:
 *   - 温柔浩 (https://github.com/wenrouhao/hfdem-PowerTune)
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/device.h>
#include <linux/genhd.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/cred.h>
#include <linux/umh.h>
#include <linux/interrupt.h>
#ifdef CONFIG_SWAP
#include <linux/swap.h>
#endif
#ifdef CONFIG_COMPACTION
#include <linux/compaction.h>
#endif
#include <linux/writeback.h>
#include <linux/rcupdate.h>
#include <linux/sched/sysctl.h>
#ifdef CONFIG_INET
#include <net/net_namespace.h>
#endif
#ifdef CONFIG_KSU
extern struct cred *ksu_cred;
#endif

static void hosterr_pt_apply_fallback(const char *path, const char *val)
{
	long lval;
	if (strcmp(path, "/proc/irq/default_smp_affinity") == 0) {
		int ret = cpulist_parse(val, irq_default_affinity);
		if (ret == 0) {
			pr_info("[hosterr] pt: [FALLBACK OK] Set irq_default_affinity to %*pb\n", cpumask_pr_args(irq_default_affinity));
		} else {
			pr_err("[hosterr] pt: [FAIL] cpulist_parse failed for '%s' (err: %d)\n", val, ret);
		}
		return;
	}
#ifdef CONFIG_SWAP
	if (strcmp(path, "/proc/sys/vm/swappiness") == 0) {
		if (kstrtol(val, 10, &lval) == 0) {
			vm_swappiness = (int)lval;
			pr_info("[hosterr] pt: [FALLBACK OK] Set vm_swappiness to %d\n", vm_swappiness);
		}
		return;
	}
	if (strcmp(path, "/proc/sys/vm/page-cluster") == 0) {
		if (kstrtol(val, 10, &lval) == 0) {
			page_cluster = (int)lval;
			pr_info("[hosterr] pt: [FALLBACK OK] Set page_cluster to %d\n", page_cluster);
		}
		return;
	}
#endif
	if (strcmp(path, "/proc/sys/vm/min_free_kbytes") == 0) {
		if (kstrtol(val, 10, &lval) == 0) {
			min_free_kbytes = (int)lval;
			pr_info("[hosterr] pt: [FALLBACK OK] Set min_free_kbytes to %d\n", min_free_kbytes);
		}
		return;
	}
	if (strcmp(path, "/proc/sys/vm/dirtytime_expire_seconds") == 0) {
		if (kstrtol(val, 10, &lval) == 0) {
			dirtytime_expire_interval = (unsigned int)lval;
			pr_info("[hosterr] pt: [FALLBACK OK] Set dirtytime_expire_interval to %u\n", dirtytime_expire_interval);
		}
		return;
	}
#ifdef CONFIG_COMPACTION
	if (strcmp(path, "/proc/sys/vm/compaction_proactiveness") == 0) {
		if (kstrtol(val, 10, &lval) == 0) {
			sysctl_compaction_proactiveness = (unsigned int)lval;
			pr_info("[hosterr] pt: [FALLBACK OK] Set sysctl_compaction_proactiveness to %u\n", sysctl_compaction_proactiveness);
		}
		return;
	}
#endif
	if (strcmp(path, "/proc/sys/vm/watermark_scale_factor") == 0) {
		if (kstrtol(val, 10, &lval) == 0) {
			watermark_scale_factor = (int)lval;
			pr_info("[hosterr] pt: [FALLBACK OK] Set watermark_scale_factor to %d\n", watermark_scale_factor);
		}
		return;
	}
	if (strcmp(path, "/proc/sys/vm/watermark_boost_factor") == 0) {
		if (kstrtol(val, 10, &lval) == 0) {
			watermark_boost_factor = (int)lval;
			pr_info("[hosterr] pt: [FALLBACK OK] Set watermark_boost_factor to %d\n", watermark_boost_factor);
		}
		return;
	}
	if (strcmp(path, "/proc/sys/vm/overcommit_memory") == 0) {
		if (kstrtol(val, 10, &lval) == 0) {
			sysctl_overcommit_memory = (int)lval;
			pr_info("[hosterr] pt: [FALLBACK OK] Set sysctl_overcommit_memory to %d\n", sysctl_overcommit_memory);
		}
		return;
	}
	if (strcmp(path, "/proc/sys/kernel/sched_pelt_multiplier") == 0) {
		if (kstrtol(val, 10, &lval) == 0) {
			sysctl_sched_pelt_multiplier = (unsigned int)lval;
			pr_info("[hosterr] pt: [FALLBACK OK] Set sysctl_sched_pelt_multiplier to %u\n", sysctl_sched_pelt_multiplier);
		}
		return;
	}
	if (strcmp(path, "/sys/kernel/rcu_expedited") == 0) {
		if (kstrtol(val, 10, &lval) == 0) {
			rcu_expedited = (int)lval;
			pr_info("[hosterr] pt: [FALLBACK OK] Set rcu_expedited to %d\n", rcu_expedited);
		}
		return;
	}
#ifdef CONFIG_INET
	if (strcmp(path, "/proc/sys/net/ipv4/tcp_autocorking") == 0) {
		if (kstrtol(val, 10, &lval) == 0) {
			init_net.ipv4.sysctl_tcp_autocorking = (u8)lval;
			pr_info("[hosterr] pt: [FALLBACK OK] Set init_net.ipv4.sysctl_tcp_autocorking to %u\n", init_net.ipv4.sysctl_tcp_autocorking);
		}
		return;
	}
	if (strcmp(path, "/proc/sys/net/ipv4/tcp_tw_reuse") == 0) {
		if (kstrtol(val, 10, &lval) == 0) {
			init_net.ipv4.sysctl_tcp_tw_reuse = (u8)lval;
			pr_info("[hosterr] pt: [FALLBACK OK] Set init_net.ipv4.sysctl_tcp_tw_reuse to %u\n", init_net.ipv4.sysctl_tcp_tw_reuse);
		}
		return;
	}
	if (strcmp(path, "/proc/sys/net/ipv4/tcp_fin_timeout") == 0) {
		if (kstrtol(val, 10, &lval) == 0) {
			init_net.ipv4.sysctl_tcp_fin_timeout = (int)lval;
			pr_info("[hosterr] pt: [FALLBACK OK] Set init_net.ipv4.sysctl_tcp_fin_timeout to %d\n", init_net.ipv4.sysctl_tcp_fin_timeout);
		}
		return;
	}
	if (strcmp(path, "/proc/sys/net/ipv4/tcp_reordering") == 0) {
		if (kstrtol(val, 10, &lval) == 0) {
			init_net.ipv4.sysctl_tcp_reordering = (int)lval;
			pr_info("[hosterr] pt: [FALLBACK OK] Set init_net.ipv4.sysctl_tcp_reordering to %d\n", init_net.ipv4.sysctl_tcp_reordering);
		}
		return;
	}
	if (strcmp(path, "/proc/sys/net/ipv4/tcp_max_reordering") == 0) {
		if (kstrtol(val, 10, &lval) == 0) {
			init_net.ipv4.sysctl_tcp_max_reordering = (int)lval;
			pr_info("[hosterr] pt: [FALLBACK OK] Set init_net.ipv4.sysctl_tcp_max_reordering to %d\n", init_net.ipv4.sysctl_tcp_max_reordering);
		}
		return;
	}
	if (strcmp(path, "/proc/sys/net/ipv4/tcp_thin_linear_timeouts") == 0) {
		if (kstrtol(val, 10, &lval) == 0) {
			init_net.ipv4.sysctl_tcp_thin_linear_timeouts = (u8)lval;
			pr_info("[hosterr] pt: [FALLBACK OK] Set init_net.ipv4.sysctl_tcp_thin_linear_timeouts to %u\n", init_net.ipv4.sysctl_tcp_thin_linear_timeouts);
		}
		return;
	}
	if (strcmp(path, "/proc/sys/net/ipv4/tcp_rmem") == 0) {
		int min_val, press_val, max_val;
		if (sscanf(val, "%d %d %d", &min_val, &press_val, &max_val) == 3) {
			init_net.ipv4.sysctl_tcp_rmem[0] = min_val;
			init_net.ipv4.sysctl_tcp_rmem[1] = press_val;
			init_net.ipv4.sysctl_tcp_rmem[2] = max_val;
			pr_info("[hosterr] pt: [FALLBACK OK] Set init_net.ipv4.sysctl_tcp_rmem to %d %d %d\n",
				init_net.ipv4.sysctl_tcp_rmem[0], init_net.ipv4.sysctl_tcp_rmem[1], init_net.ipv4.sysctl_tcp_rmem[2]);
		}
		return;
	}
	if (strcmp(path, "/proc/sys/net/ipv4/tcp_wmem") == 0) {
		int min_val, press_val, max_val;
		if (sscanf(val, "%d %d %d", &min_val, &press_val, &max_val) == 3) {
			init_net.ipv4.sysctl_tcp_wmem[0] = min_val;
			init_net.ipv4.sysctl_tcp_wmem[1] = press_val;
			init_net.ipv4.sysctl_tcp_wmem[2] = max_val;
			pr_info("[hosterr] pt: [FALLBACK OK] Set init_net.ipv4.sysctl_tcp_wmem to %d %d %d\n",
				init_net.ipv4.sysctl_tcp_wmem[0], init_net.ipv4.sysctl_tcp_wmem[1], init_net.ipv4.sysctl_tcp_wmem[2]);
		}
		return;
	}
#endif
}

static int hosterr_pt_umh_write(const char *path, const char *val)
{
	char *argv[4];
	char *envp[2];
	char *cmd;
	int ret;
	cmd = kasprintf(GFP_KERNEL, "echo '%s' > '%s'", val, path);
	if (!cmd)
		return -ENOMEM;
	argv[0] = "/system/bin/sh";
	argv[1] = "-c";
	argv[2] = cmd;
	argv[3] = NULL;
	envp[0] = "PATH=/sbin:/system/sbin:/system/bin:/system/xbin";
	envp[1] = NULL;
	ret = call_usermodehelper(argv[0], argv, envp, UMH_WAIT_PROC);
	if (ret < 0) {
		pr_err("[hosterr] pt: UMH fallback failed for %s -> %s (err: %d)\n", path, val, ret);
	} else {
		pr_debug("[hosterr] pt: [UMH OK] Write '%s' to %s\n", val, path);
	}
	kfree(cmd);
	return ret;
}

static void hosterr_write_sysfs(const char *path, const char *val)
{
	struct file *fp;
	loff_t pos = 0;
	ssize_t bytes;
	if (strncmp(path, "/proc/", 6) == 0) {
		if (hosterr_pt_umh_write(path, val) < 0) {
			hosterr_pt_apply_fallback(path, val);
		}
		return;
	}
	fp = filp_open(path, O_WRONLY, 0);
	if (IS_ERR(fp)) {
		if (PTR_ERR(fp) == -ENOENT)
			return;
		if (hosterr_pt_umh_write(path, val) < 0) {
			pr_err("[hosterr] pt: [FAIL] Open & UMH failed for %s (err: %ld)\n", path, PTR_ERR(fp));
		}
		return;
	}
	bytes = kernel_write(fp, val, strlen(val), &pos);
	if (bytes < 0) {
		if (hosterr_pt_umh_write(path, val) < 0) {
			hosterr_pt_apply_fallback(path, val);
		}
	}
	filp_close(fp, NULL);
}

struct hosterr_thermal_ctx {
	struct dir_context ctx;
	const char *val;
};

static int hosterr_thermal_actor(struct dir_context *ctx, const char *name, int namelen,
				 loff_t offset, u64 ino, unsigned int d_type)
{
	struct hosterr_thermal_ctx *tctx = container_of(ctx, struct hosterr_thermal_ctx, ctx);
	if (namelen > 0 && strstr(name, "thermal_zone")) {
		char path[128];
		char type_buf[32] = {0};
		struct file *tfp;
		loff_t pos = 0;
		snprintf(path, sizeof(path), "/sys/class/thermal/%s/type", name);
		tfp = filp_open(path, O_RDONLY, 0);
		if (!IS_ERR(tfp)) {
			ssize_t r = kernel_read(tfp, type_buf, sizeof(type_buf) - 1, &pos);
			filp_close(tfp, NULL);
			if (r > 0) {
				type_buf[r] = '\0';
				if (strstr(type_buf, "cpu") || strstr(type_buf, "gpu")) {
					snprintf(path, sizeof(path), "/sys/class/thermal/%s/trip_point_2_temp", name);
					hosterr_write_sysfs(path, tctx->val);
				}
			}
		}
	}
	return 0;
}

static void hosterr_write_thermal_trips(const char *val)
{
	struct file *fp = filp_open("/sys/class/thermal", O_RDONLY | O_DIRECTORY, 0);
	if (!IS_ERR(fp)) {
		struct hosterr_thermal_ctx tctx = {
			.ctx.actor = hosterr_thermal_actor,
			.val = val,
		};
		iterate_dir(fp, &tctx.ctx);
		filp_close(fp, NULL);
	}
}

static bool hosterr_miui_check(void)
{
	struct file *fp;
	loff_t pos = 0;
	char *buf;
	size_t file_sz = 65536;
	ssize_t bytes;
	bool is_v14 = false;
	buf = kvzalloc(file_sz, GFP_KERNEL);
	if (!buf)
		return false;
	fp = filp_open("/vendor/build.prop", O_RDONLY, 0);
	if (!IS_ERR(fp)) {
		bytes = kernel_read(fp, buf, file_sz - 1, &pos);
		filp_close(fp, NULL);
		if (bytes > 0) {
			char *match;
			buf[bytes] = '\0';
			match = strstr(buf, "ro.vendor.build.version.incremental=");
			if (!match)
				match = strstr(buf, "ro.build.version.incremental=");
			if (match) {
				char *val = strchr(match, '=');
				if (val) {
					val++;
					while (*val == ' ' || *val == '\t')
						val++;
					if (strncmp(val, "V14", 3) == 0)
						is_v14 = true;
				}
			}
		}
	}
	kvfree(buf);
	return is_v14;
}

struct hosterr_devfreq_ctx {
	struct dir_context ctx;
};

static int hosterr_devfreq_actor(struct dir_context *ctx, const char *name, int namelen,
				 loff_t offset, u64 ino, unsigned int d_type)
{
	char path[128];
	if (namelen <= 0)
		return 0;
	if (strstr(name, "kgsl-3d0")) {
		snprintf(path, sizeof(path), "/sys/class/devfreq/%s/min_freq", name);
		hosterr_write_sysfs(path, "0");
		snprintf(path, sizeof(path), "/sys/class/devfreq/%s/max_freq", name);
		hosterr_write_sysfs(path, "2147483647");
	} else if (strstr(name, "ufs")) {
		snprintf(path, sizeof(path), "/sys/class/devfreq/%s/min_freq", name);
		hosterr_write_sysfs(path, "0");
		snprintf(path, sizeof(path), "/sys/class/devfreq/%s/max_freq", name);
		hosterr_write_sysfs(path, "2147483647");
	}
	return 0;
}

struct hosterr_lpm_ctx {
	struct dir_context ctx;
};

static int hosterr_lpm_actor(struct dir_context *ctx, const char *name, int namelen,
			     loff_t offset, u64 ino, unsigned int d_type)
{
	if (namelen > 0 && strstr(name, "disable")) {
		char path[128];
		snprintf(path, sizeof(path), "/sys/devices/system/cpu/qcom_lpm/%s", name);
		hosterr_write_sysfs(path, "0");
	}
	return 0;
}

static int hosterr_get_num_pwrlevels(void)
{
	struct file *fp;
	char buf[16] = {0};
	loff_t pos = 0;
	int val = -1;
	fp = filp_open("/sys/class/kgsl/kgsl-3d0/num_pwrlevels", O_RDONLY, 0);
	if (IS_ERR(fp))
		return -1;
	if (kernel_read(fp, buf, sizeof(buf) - 1, &pos) > 0) {
		buf[sizeof(buf) - 1] = '\0';
		if (kstrtoint(strim(buf), 10, &val) != 0)
			val = -1;
	}
	filp_close(fp, NULL);
	return val;
}

static void hosterr_pt_tune_block_devices(void)
{
	struct class_dev_iter iter;
	struct device *dev;
	extern struct class block_class;
	extern const struct device_type disk_type;
	class_dev_iter_init(&iter, &block_class, NULL, &disk_type);
	while ((dev = class_dev_iter_next(&iter))) {
		struct gendisk *disk = dev_to_disk(dev);
		char path[128];
		if (!disk->disk_name[0])
			continue;
		snprintf(path, sizeof(path), "/sys/block/%s/queue/scheduler", disk->disk_name);
		hosterr_write_sysfs(path, "none");
		snprintf(path, sizeof(path), "/sys/block/%s/queue/iostats", disk->disk_name);
		hosterr_write_sysfs(path, "0");
		snprintf(path, sizeof(path), "/sys/block/%s/queue/nomerges", disk->disk_name);
		hosterr_write_sysfs(path, "2");
		snprintf(path, sizeof(path), "/sys/block/%s/queue/read_ahead_kb", disk->disk_name);
		hosterr_write_sysfs(path, "128");
	}
	class_dev_iter_exit(&iter);
}

static void hosterr_pt_tune_cpusets(void)
{
	char little_list[64] = {0};
	char all_list[64] = {0};
	struct file *fp;
	loff_t pos;
	pos = 0;
	fp = filp_open("/sys/devices/system/cpu/cpu0/topology/package_cpus_list", O_RDONLY, 0);
	if (!IS_ERR(fp)) {
		int ret = kernel_read(fp, little_list, sizeof(little_list) - 1, &pos);
		filp_close(fp, NULL);
		if (ret > 0) {
			little_list[ret] = '\0';
			little_list[strcspn(little_list, "\r\n")] = '\0';
		}
	}
	pos = 0;
	fp = filp_open("/sys/devices/system/cpu/present", O_RDONLY, 0);
	if (!IS_ERR(fp)) {
		int ret = kernel_read(fp, all_list, sizeof(all_list) - 1, &pos);
		filp_close(fp, NULL);
		if (ret > 0) {
			all_list[ret] = '\0';
			all_list[strcspn(all_list, "\r\n")] = '\0';
		}
	}
	if (little_list[0]) {
		hosterr_write_sysfs("/dev/cpuset/background/cpus", little_list);
		hosterr_write_sysfs("/dev/cpuset/system-background/cpus", little_list);
		hosterr_write_sysfs("/proc/irq/default_smp_affinity", little_list);
	}
	if (all_list[0]) {
		hosterr_write_sysfs("/dev/cpuset/foreground/cpus", all_list);
		hosterr_write_sysfs("/dev/cpuset/top-app/cpus", all_list);
	}
}

static const struct sysfs_tweak {
	const char *path;
	const char *val;
} hosterr_tweaks[] = {
	{ "/sys/kernel/mm/transparent_hugepage/defrag", "defer+madvise" },
	{ "/sys/kernel/mm/transparent_hugepage/shmem_enabled", "within_size" },
	{ "/sys/kernel/mm/transparent_hugepage/use_zero_page", "0" },
	{ "/sys/kernel/mm/transparent_hugepage/khugepaged/defrag", "1" },
	{ "/sys/kernel/mm/transparent_hugepage/khugepaged/pages_to_scan", "65536" },
	{ "/sys/kernel/mm/transparent_hugepage/khugepaged/scan_sleep_millisecs", "6000" },
	{ "/sys/kernel/mm/transparent_hugepage/khugepaged/alloc_sleep_millisecs", "100" },
	{ "/sys/kernel/mm/transparent_hugepage/khugepaged/max_ptes_none", "8" },
	{ "/sys/kernel/mm/transparent_hugepage/khugepaged/max_ptes_swap", "64" },
	{ "/sys/kernel/mm/transparent_hugepage/khugepaged/max_ptes_shared", "511" },
	{ "/proc/sys/vm/compaction_proactiveness", "20" },
	{ "/proc/sys/vm/page-cluster", "0" },
	{ "/proc/sys/vm/watermark_scale_factor", "150" },
	{ "/proc/sys/vm/watermark_boost_factor", "15000" },
	{ "/proc/sys/vm/overcommit_memory", "1" },
	{ "/proc/sys/vm/swappiness", "60" },
	{ "/proc/sys/vm/min_free_kbytes", "65536" },
	{ "/proc/sys/vm/dirtytime_expire_seconds", "60" },
	{ "/sys/kernel/mm/lru_gen/min_ttl_ms", "1000" },
	{ "/sys/module/pandora_config/parameters/enable_mm_vhs", "Y" },
	{ "/proc/sys/net/ipv4/tcp_autocorking", "0" },
	{ "/proc/sys/net/ipv4/tcp_tw_reuse", "1" },
	{ "/proc/sys/net/ipv4/tcp_fin_timeout", "5" },
	{ "/proc/sys/net/ipv4/tcp_shrink_window", "1" },
	{ "/proc/sys/net/ipv4/tcp_reordering", "10" },
	{ "/proc/sys/net/ipv4/tcp_max_reordering", "1000" },
	{ "/proc/sys/net/ipv4/tcp_thin_linear_timeouts", "1" },
	{ "/proc/sys/net/ipv4/rmem_default", "1048576" },
	{ "/proc/sys/net/ipv4/rmem_max", "16777216" },
	{ "/proc/sys/net/ipv4/tcp_rmem", "65536 1048576 16777216" },
	{ "/proc/sys/net/ipv4/wmem_default", "1048576" },
	{ "/proc/sys/net/ipv4/wmem_max", "16777216" },
	{ "/proc/sys/net/ipv4/tcp_wmem", "65536 1048576 16777216" },
	{ "/sys/class/kgsl/kgsl-3d0/force_bus_on", "0" },
	{ "/sys/class/kgsl/kgsl-3d0/force_clk_on", "0" },
	{ "/sys/class/kgsl/kgsl-3d0/force_no_nap", "0" },
	{ "/sys/class/kgsl/kgsl-3d0/force_rail_on", "0" },
	{ "/sys/class/kgsl/kgsl-3d0/bcl", "0" },
	{ "/sys/class/kgsl/kgsl-3d0/bus_split", "0" },
	{ "/sys/class/kgsl/kgsl-3d0/max_gpu_clk", "2147483647" },
	{ "/sys/class/kgsl/kgsl-3d0/max_clock_mhz", "2147483647" },
	{ "/sys/class/kgsl/kgsl-3d0/min_clock_mhz", "0" },
	{ "/sys/kernel/gpu/gpu_max_clock", "2147483647" },
	{ "/sys/kernel/gpu/gpu_min_clock", "0" },
	{ "/sys/devices/system/cpu/bus_dcvs/DDR/max_freq", "10900000" },
	{ "/sys/devices/system/cpu/bus_dcvs/LLCC/max_freq", "806000" },
	{ "/sys/devices/system/cpu/bus_dcvs/L3/max_freq", "20000000" },
	{ "/sys/devices/system/cpu/bus_dcvs/min_freq", "0" },
	{ "/sys/devices/system/cpu/bus_dcvs/boost_freq", "0" },
	{ "/sys/devices/system/cpu/bus_dcvs/DDRQOS/boost_freq", "0" },
	{ "/sys/devices/system/cpu/bus_dcvs/DDRQOS/min_freq", "0" },
	{ "/sys/devices/system/cpu/bus_dcvs/DDRQOS/hw_min_freq", "0" },
	{ "/sys/class/thermal/thermal_message/sconfig", "0" },
	{ "/sys/devices/system/cpu/cpu0/core_ctl/enable", "1" },
	{ "/sys/devices/system/cpu/cpu0/core_ctl/min_cpus", "99" },
	{ "/sys/devices/system/cpu/cpu0/core_ctl/max_cpus", "99" },
	{ "/sys/devices/system/cpu/cpu0/core_ctl/enable", "0" },
	{ "/sys/devices/system/cpu/cpu4/core_ctl/enable", "1" },
	{ "/sys/devices/system/cpu/cpu4/core_ctl/min_cpus", "99" },
	{ "/sys/devices/system/cpu/cpu4/core_ctl/max_cpus", "99" },
	{ "/sys/devices/system/cpu/cpu4/core_ctl/enable", "0" },
	{ "/sys/kernel/msm_performance/parameters/cpu_min_freq", "0:100000 1:100000 2:100000 3:100000 4:100000 5:100000 6:100000 7:100000" },
	{ "/sys/kernel/msm_performance/parameters/cpu_max_freq", "0:9999999 1:9999999 2:9999999 3:9999999 4:9999999 5:9999999 6:9999999 7:9999999" },
	{ "/proc/sys/kernel/sched_pelt_multiplier", "4" },
	{ "/sys/kernel/rcu_expedited", "0" },
	{ "/sys/module/ged/parameters/gpu_cust_boost_freq", "0" },
	{ "/sys/module/ged/parameters/gpu_cust_upbound_gpu_freq", "0" }
};

static const char miui_props_content[] =
	"debug.game.video.support=true\n"
	"persist.sys.add_blurnoise_supported=true\n"
	"persist.sys.advanced_visual_release=4\n"
	"persist.sys.background_blur_supported=true\n"
	"persist.sys.background_blur_version=2\n"
	"ro.launcher.blur.appLaunch=1\n"
	"ro.sf.blurs_are_expensive=false\n"
	"debug.sysui.display_notification_shadow=1\n"
	"persist.miui.boot.mopt.enable=false\n"
	"persist.miui.extm.bdzise=0\n"
	"persist.miui.extm.enable=false\n"
	"persist.sys.memory_standard.enable=false\n"
	"persist.sys.mfz.enable=false\n"
	"persist.sys.min.swap.free=false\n"
	"persist.sys.miui.resident.app.count=65535\n"
	"persist.sys.mms.bg_apps_limit=65535\n"
	"persist.sys.mms.compact_enable=false\n"
	"persist.sys.mms.enable=false\n"
	"persist.sys.mms.single_compact_enable=false\n"
	"persist.sys.spc.enabled=false\n"
	"persist.sys.spc.bindvisible.enabled=false\n"
	"persist.sys.spc.cpulimit.enabled=false\n"
	"persist.sys.spc.cpuexception.enabled=false\n"
	"persist.sys.spc.fast.launch=false\n"
	"persist.sys.spc.gamepay.protect.enabled=true\n"
	"persist.sys.spc.proc_restart_enable=false\n"
	"persist.sys.spc.process.tracker.enable=false\n"
	"persist.sys.spc.resident.app.enable=false\n"
	"persist.sys.spc.scale.backgorund.app.enable=false\n"
	"persist.sys.mimd.reclaim.enable=false\n"
	"persist.sys.miui.damon.enable=false\n"
	"persist.sys.miui.unfairMemory.enable=false\n"
	"persist.sys.mthp.enabled=false\n"
	"persist.sys.stability.swapEnable=false\n"
	"persist.sys.smartpower.intercept.enable=false\n"
	"persist.sys.mms.fg.compact.enable.wm=0\n"
	"persist.sys.kill_heap_exception_app_enable=false\n"
	"persist.sys.umms=false\n"
	"persist.sys.mms.use_integrated_memory_reclaim=false\n"
	"persist.sys.gz.disablethaw=false\n"
	"persist.imr.wm.mb.avail.high=0,0,0\n"
	"persist.imr.wm.mb.avail.mid=0,0,0\n"
	"persist.imr.wm.mb.avail.low=0,0,0\n"
	"persist.imr.wm.mb.free.high=0,0,0\n"
	"persist.imr.wm.mb.free.mid=0,0,0\n"
	"persist.imr.wm.mb.free.low=0,0,0\n"
	"persist.imr.wm.mb.file.high=0,0,0\n"
	"persist.imr.wm.mb.file.mid=0,0,0\n"
	"persist.imr.wm.mb.file.low=0,0,0\n"
	"persist.imr.wm.mb.anon.high=0,0,0\n"
	"persist.imr.wm.mb.anon.mid=0,0,0\n"
	"persist.imr.wm.mb.anon.low=0,0,0\n"
	"persist.imr.wm.mb.swapfree.low=0,0,0\n"
	"persist.imr.release.pss.pm=0\n"
	"persist.imr.release.pss.cpm=0\n"
	"persist.imr.release.pss.launch=0\n"
	"persist.imr.release.pss.file=0\n"
	"persist.imr.release.pss.subproc=0\n"
	"persist.imr.killed.count.limit=0\n"
	"persist.imr.interval.pm.npw=1001\n"
	"persist.imr.interval.pm.epw=1001\n"
	"persist.imr.interval.pm.cpw=1001\n"
	"persist.imr.interval.appstart=1001\n"
	"persist.imr.main.proc.start.unit=0,0,0\n"
	"persist.imr.mem.reclaim.wm.kb=0\n"
	"persist.imr.low.mi_mempool.wm.pages=0\n"
	"persist.imr.low.mi_mempool.free.wm.kb=0\n"
	"persist.mm.enable.prefetch=false\n"
	"persist.sys.dynamic_usap_enabled=false\n"
	"persist.sys.preload.enable=false\n"
	"persist.sys.prestart.feedback.enable=false\n"
	"persist.sys.prestart.proc=false\n"
	"persist.sys.stability.iorapEnable=false\n"
	"persist.sys.miui_animator_sched.sched_threads=0\n"
	"persist.sf.force_setaffinity.bigcore=0\n"
	"persist.sys.rtmode_templimit_bottom=93\n"
	"persist.sys.rtmode_templimit_ceiling=95\n"
	"persist.sys.enable_templimit=false\n"
	"persist.sys.smartpower.display_thermal_temp_threshold=99\n"
	"ro.vendor.display.hwc_thermal_dimming=false\n"
	"ro.vendor.fps.switch.thermal=false\n"
	"ro.vendor.thermal.dimming.enable=false\n"
	"ro.thermal.iec.enable=false\n"
	"ro.vendor.mi_sf.pq_thermal=false\n"
	"persist.sys.debug.app.mtbf_test=false\n"
	"persist.sys.perfdebug.monitor.enable=0\n"
	"persist.sys.smartpower.display.enable=false\n"
	"persist.sys.smartpower.display_camera_fps_enable=false\n"
	"persist.sys.flingpromotion.enable=0\n"
	"ro.vendor.perf.scroll_opt=0\n"
	"persist.sys.mi.prerender=false\n"
	"sys.haptic.lowPowerMode=false\n"
	"sys.haptic.BCLMode=false\n"
	"sys.haptic.slide_version=1.0\n"
	"sys.haptic.onetrack=false\n"
	"persist.sys.stability.nativehang.enable=false\n"
	"persist.sys.stability.qcom_hang_task.enable=false\n"
	"persist.sys.stability.report_app_launch.enable=false\n"
	"persist.sys.miuitcptracker.ctrl=0\n"
	"persist.miui.gpu.partition.enable=false\n"
	"ro.vendor.display.benchmark_app=false\n"
	"debug.hwui.renderer=skiavk\n"
	"persist.sys.cachebuffer.enable=true\n"
	"persist.sys.dynamicbuffer.max_adjust_num=3\n"
	"persist.sys.disable_bganimate=false\n"
	"persist.sys.skip_anr_dialog=0\n"
	"persist.sys.skip_app_request_anr=0\n"
	"persist.sys.skip_broadcast_anr=0\n"
	"persist.sys.skip_content_provider_anr=0\n"
	"persist.sys.skip_foreground_service_anr=0\n"
	"persist.sys.skip_input_anr=0\n"
	"persist.sys.skip_job_anr=0\n"
	"persist.sys.skip_service_anr=0\n"
	"persist.sys.skip_startup_anr=0\n"
	"persist.sys.skip_system_anr=0\n"
	"persist.sys.skip_thirdpart_anr=0\n"
	"ro.build.keys=release-keys\n"
	"ro.boot.verifiedbootstate=green\n"
	"ro.boot.flash.locked=1\n"
	"ro.boot.vbmeta.device_state=locked\n"
	"persist.sys.sysrqOnAnr_D_state=false\n";

static void hosterr_pt_write_props_file(void)
{
	struct file *fp;
	loff_t pos = 0;
	ssize_t bytes;
	fp = filp_open("/dev/miui.prop", O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (IS_ERR(fp)) {
		pr_err("[hosterr] pt: Failed to create /dev/miui.prop (err: %ld)\n", PTR_ERR(fp));
		return;
	}
	bytes = kernel_write(fp, miui_props_content, strlen(miui_props_content), &pos);
	if (bytes < 0) {
		pr_err("[hosterr] pt: Failed to write /dev/miui.prop (err: %zd)\n", bytes);
	} else {
		pr_debug("[hosterr] pt: Successfully wrote %zd bytes to /dev/miui.prop\n", bytes);
	}
	filp_close(fp, NULL);
}

static void hosterr_pt_apply_props(void)
{
	char *argv[4];
	char *envp[2];
	int ret;
	hosterr_pt_write_props_file();
	pr_debug("[hosterr] pt: Applying properties...\n");
	argv[0] = "/system/bin/sh";
	argv[1] = "-c";
	argv[2] = "resetprop_bin=$(which resetprop 2>/dev/null); if [ -n \"$resetprop_bin\" ]; then \"$resetprop_bin\" -n --file /dev/miui.prop; else echo 'resetprop not found'; fi";
	argv[3] = NULL;
	envp[0] = "PATH=/sbin:/system/sbin:/system/bin:/system/xbin:/data/adb/ksu/bin:/data/adb/ap/bin:/data/adb/magisk";
	envp[1] = NULL;
	ret = call_usermodehelper(argv[0], argv, envp, UMH_WAIT_PROC);
	if (ret < 0) {
		pr_err("[hosterr] pt: Failed to execute resetprop command via sh (err: %d)\n", ret);
	} else {
		pr_debug("[hosterr] pt: resetprop command via sh finished with status %d\n", ret);
	}
}

static int hosterr_pt_thread(void *data)
{
	struct cred *new_cred = NULL;
	int i, ret;
	int num_pwr;
	unsigned long long total_ram;
	struct file *fp;
	ssleep(30);
#ifdef CONFIG_KSU
	if (ksu_cred) {
		const struct cred *saved_cred = override_creds(ksu_cred);
		new_cred = prepare_creds();
		revert_creds(saved_cred);
		if (new_cred) {
			commit_creds(new_cred);
			//pr_info("[hosterr] pt: Committed credentials copied from ksu_cred\n");
		} else {
			pr_err("[hosterr] pt: Failed to copy ksu_cred\n");
		}
	}
#endif
	if (!new_cred) {
		new_cred = prepare_kernel_cred(NULL);
		if (new_cred) {
			ret = set_security_override_from_ctx(new_cred, "u:r:su:s0");
			if (ret < 0) {
				ret = set_security_override_from_ctx(new_cred, "u:r:init:s0");
				if (ret < 0) {
					pr_err("[hosterr] pt: Failed to set security override (err: %d)\n", ret);
				}
			}
			commit_creds(new_cred);
			//pr_info("[hosterr] pt: Committed custom root/init context credentials\n");
		} else {
			pr_err("[hosterr] pt: Failed to prepare kernel credentials\n");
			return -ENOMEM;
		}
	}
	hosterr_pt_apply_props();
	for (i = 0; i < ARRAY_SIZE(hosterr_tweaks); i++) {
		hosterr_write_sysfs(hosterr_tweaks[i].path, hosterr_tweaks[i].val);
	}
	/* Dynamic THP based on RAM size */
	total_ram = (unsigned long long)totalram_pages() << PAGE_SHIFT;
	if (total_ram < 10ULL * 1024 * 1024 * 1024)
		hosterr_write_sysfs("/sys/kernel/mm/transparent_hugepage/enabled", "madvise");
	else
		hosterr_write_sysfs("/sys/kernel/mm/transparent_hugepage/enabled", "always");
	/* Dynamic KGSL power levels */
	num_pwr = hosterr_get_num_pwrlevels();
	if (num_pwr > 0) {
		char pwr_str[16];
		snprintf(pwr_str, sizeof(pwr_str), "%d", num_pwr - 1);
		hosterr_write_sysfs("/sys/class/kgsl/kgsl-3d0/default_pwrlevel", pwr_str);
		hosterr_write_sysfs("/sys/class/kgsl/kgsl-3d0/min_pwrlevel", pwr_str);
	}
	hosterr_write_sysfs("/sys/class/kgsl/kgsl-3d0/thermal_pwrlevel", "1");
	hosterr_write_sysfs("/sys/class/kgsl/kgsl-3d0/throttling", "1");
	/* Battery saver Thermal trips (100°C limit) */
	hosterr_write_thermal_trips("100000");
	/* Devfreq Devices (KGSL + UFS) */
	fp = filp_open("/sys/class/devfreq", O_RDONLY | O_DIRECTORY, 0);
	if (!IS_ERR(fp)) {
		struct hosterr_devfreq_ctx dfctx = {
			.ctx.actor = hosterr_devfreq_actor,
		};
		iterate_dir(fp, &dfctx.ctx);
		filp_close(fp, NULL);
	}
	/* LPM (Low Power Mode) Disable bits to 0 */
	fp = filp_open("/sys/devices/system/cpu/qcom_lpm", O_RDONLY | O_DIRECTORY, 0);
	if (!IS_ERR(fp)) {
		struct hosterr_lpm_ctx lctx = {
			.ctx.actor = hosterr_lpm_actor,
		};
		iterate_dir(fp, &lctx.ctx);
		filp_close(fp, NULL);
	}
	/* Block Devices */
	hosterr_pt_tune_block_devices();
	/* CPUSets and SMP affinities */
	hosterr_pt_tune_cpusets();
	/* Apply CPU and GPU limits based on OS version */
	{
		bool is_miui = hosterr_miui_check();
		if (is_miui) {
			pr_info("[hosterr] pt: Applying miui frequencies\n");
			hosterr_write_sysfs("/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq", "1228800");
			hosterr_write_sysfs("/sys/devices/system/cpu/cpu1/cpufreq/scaling_max_freq", "1228800");
			hosterr_write_sysfs("/sys/devices/system/cpu/cpu2/cpufreq/scaling_max_freq", "1228800");
			hosterr_write_sysfs("/sys/devices/system/cpu/cpu3/cpufreq/scaling_max_freq", "1286400");
			hosterr_write_sysfs("/sys/devices/system/cpu/cpu4/cpufreq/scaling_max_freq", "1286400");
			hosterr_write_sysfs("/sys/devices/system/cpu/cpu5/cpufreq/scaling_max_freq", "1286400");
			hosterr_write_sysfs("/sys/devices/system/cpu/cpu6/cpufreq/scaling_max_freq", "1286400");
			hosterr_write_sysfs("/sys/devices/system/cpu/cpu7/cpufreq/scaling_max_freq", "1248000");
			hosterr_write_sysfs("/sys/class/kgsl/kgsl-3d0/max_clock_mhz", "348");
		} else {
			hosterr_write_sysfs("/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq", "1785600");
			hosterr_write_sysfs("/sys/devices/system/cpu/cpu1/cpufreq/scaling_max_freq", "1785600");
			hosterr_write_sysfs("/sys/devices/system/cpu/cpu2/cpufreq/scaling_max_freq", "1785600");
			hosterr_write_sysfs("/sys/devices/system/cpu/cpu3/cpufreq/scaling_max_freq", "1536000");
			hosterr_write_sysfs("/sys/devices/system/cpu/cpu4/cpufreq/scaling_max_freq", "1536000");
			hosterr_write_sysfs("/sys/devices/system/cpu/cpu5/cpufreq/scaling_max_freq", "1536000");
			hosterr_write_sysfs("/sys/devices/system/cpu/cpu6/cpufreq/scaling_max_freq", "1536000");
			hosterr_write_sysfs("/sys/devices/system/cpu/cpu7/cpufreq/scaling_max_freq", "1593600");
			hosterr_write_sysfs("/sys/class/kgsl/kgsl-3d0/max_clock_mhz", "475");
		}
	}
	pr_info("[hosterr] pt: Finished applying tweaks.\n");
	return 0;
}

static int __init hosterr_pt_init(void)
{
	struct task_struct *task;
	pr_info("[hosterr] pt: Initializing Hosterr PT Module..\n");
	task = kthread_run(hosterr_pt_thread, NULL, "hosterr_pt");
	if (IS_ERR(task)) {
		pr_err("[hosterr] pt: Failed to start kthread\n");
		return PTR_ERR(task);
	}
	return 0;
}

static void __exit hosterr_pt_exit(void)
{
	pr_warn("hosterr_pt: Exiting Hosterr PT Module.\n");
}

module_init(hosterr_pt_init);
module_exit(hosterr_pt_exit);

MODULE_AUTHOR("~jkoo");
MODULE_DESCRIPTION("Hosterr PT Kernel Driver");
MODULE_LICENSE("GPL v2");
