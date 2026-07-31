// SPDX-License-Identifier: GPL-2.0
/*
 * Record module file reads for R3 diagnostics without enforcing policy.
 */
#include <linux/fs.h>
#include <linux/kernel_read_file.h>
#include <linux/lsm_hooks.h>
#include <linux/printk.h>

static int r3_audit_kernel_read_file(struct file *file,
				    enum kernel_read_file_id id, bool unused)
{
	if (id == READING_MODULE) {
		if (file)
			pr_info_ratelimited("r3_audit: module read %pD\n", file);
		else
			pr_info_ratelimited("r3_audit: module read path-unavailable\n");
	}

	return 0;
}

static struct security_hook_list r3_audit_hooks[] __lsm_ro_after_init = {
	LSM_HOOK_INIT(kernel_read_file, r3_audit_kernel_read_file),
};

static int __init r3_audit_lsm_init(void)
{
	security_add_hooks(r3_audit_hooks, ARRAY_SIZE(r3_audit_hooks),
			   "r3_audit");
	return 0;
}

DEFINE_EARLY_LSM(r3_audit) = {
	.name = "r3_audit",
	.init = r3_audit_lsm_init,
};
