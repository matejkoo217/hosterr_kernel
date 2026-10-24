// SPDX-License-Identifier: GPL-2.0
/*
 * /sys/nfc for the HarmonyOS NFC stack.
 *
 * Huawei's NFC HAL, NCI stack and vendor services read and write a set of
 * attributes that their own NFC driver exposes in /sys/nfc (chip type,
 * device node, configuration file name, open status...).  Qualcomm devices
 * are driven by the nq-nci driver (/dev/nq-nci), which has none of them:
 * this module provides the attributes as plain values.  Writable ones just
 * keep what userspace stores.
 *
 * Based on hmos2sweet src/kernel/patches/0017 for the PN553; the defaults
 * below are the ones the NXP (t001) HarmonyOS libraries expect and work for
 * the NQ/SN-series controllers Xiaomi phones use (/dev/nq-nci).
 *
 * Copyright (c) 2026 hmos2fuxi
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/ohos.h>

#define VAL_LEN 128

struct nfc_attr {
	struct kobj_attribute ka;
	char val[VAL_LEN];
};

static DEFINE_MUTEX(nfc_attr_lock);

static ssize_t nfc_attr_show(struct kobject *kobj, struct kobj_attribute *ka, char *buf)
{
	struct nfc_attr *a = container_of(ka, struct nfc_attr, ka);
	ssize_t n;

	mutex_lock(&nfc_attr_lock);
	n = scnprintf(buf, PAGE_SIZE, "%s", a->val);
	mutex_unlock(&nfc_attr_lock);
	return n;
}

static ssize_t nfc_attr_store(struct kobject *kobj, struct kobj_attribute *ka, const char *buf, size_t count)
{
	struct nfc_attr *a = container_of(ka, struct nfc_attr, ka);
	size_t n = min(count, (size_t)VAL_LEN - 1);

	mutex_lock(&nfc_attr_lock);
	memcpy(a->val, buf, n);
	a->val[n] = 0;
	mutex_unlock(&nfc_attr_lock);
	return count;
}

#define NFC_ATTR(_name, _init) \
	static struct nfc_attr nfc_attr_##_name = { \
		.ka = __ATTR(_name, 0664, nfc_attr_show, nfc_attr_store), \
		.val = _init, \
	}

/* the chip type selects the HAL/NCI libraries: "sn110u" -> the NXP (t001) ones */
NFC_ATTR(nfc_chip_type, "sn110u");
NFC_ATTR(nfc_dev_name, "/dev/nq-nci");
NFC_ATTR(nxp_config_name, "libnfc-nxp-sn110.conf");
NFC_ATTR(nfc_brcm_conf_name, "");
NFC_ATTR(nfc_spec_rf_conf_name, "libnfc-nxp-sn110_RF.conf");
NFC_ATTR(nfc_clk_src, "pll");
NFC_ATTR(nfc_calibration, "0");
NFC_ATTR(nfc_fwupdate, "0");
NFC_ATTR(nfc_fw_version, "11.1.24");
NFC_ATTR(nfc_need_reset, "0");
NFC_ATTR(nfc_open_status, "0");
NFC_ATTR(nfc_enable_status, "0");
NFC_ATTR(nfc_bootloader_status, "0");
NFC_ATTR(nfc_wired_ese_type, "0");
NFC_ATTR(nfc_ese_num, "0");
NFC_ATTR(nfc_sim_switch, "0");
NFC_ATTR(nfc_sim_status, "0");
NFC_ATTR(rd_nfc_sim_status, "0");
NFC_ATTR(nfc_card_num, "1");
NFC_ATTR(nfc_single_channel, "0");
NFC_ATTR(nfc_svdd_sw, "0");
NFC_ATTR(nfc_close_type, "0");
NFC_ATTR(nfc_switch_state, "0");
NFC_ATTR(nfc_at_result, "0");
NFC_ATTR(nfc_activated_se_info, "0");
NFC_ATTR(nfcservice_lock, "0");
NFC_ATTR(nfc_hal_dmd, "0");

static struct attribute *nfc_attrs[] = {
	&nfc_attr_nfc_chip_type.ka.attr,
	&nfc_attr_nfc_dev_name.ka.attr,
	&nfc_attr_nxp_config_name.ka.attr,
	&nfc_attr_nfc_brcm_conf_name.ka.attr,
	&nfc_attr_nfc_spec_rf_conf_name.ka.attr,
	&nfc_attr_nfc_clk_src.ka.attr,
	&nfc_attr_nfc_calibration.ka.attr,
	&nfc_attr_nfc_fwupdate.ka.attr,
	&nfc_attr_nfc_fw_version.ka.attr,
	&nfc_attr_nfc_need_reset.ka.attr,
	&nfc_attr_nfc_open_status.ka.attr,
	&nfc_attr_nfc_enable_status.ka.attr,
	&nfc_attr_nfc_bootloader_status.ka.attr,
	&nfc_attr_nfc_wired_ese_type.ka.attr,
	&nfc_attr_nfc_ese_num.ka.attr,
	&nfc_attr_nfc_sim_switch.ka.attr,
	&nfc_attr_nfc_sim_status.ka.attr,
	&nfc_attr_rd_nfc_sim_status.ka.attr,
	&nfc_attr_nfc_card_num.ka.attr,
	&nfc_attr_nfc_single_channel.ka.attr,
	&nfc_attr_nfc_svdd_sw.ka.attr,
	&nfc_attr_nfc_close_type.ka.attr,
	&nfc_attr_nfc_switch_state.ka.attr,
	&nfc_attr_nfc_at_result.ka.attr,
	&nfc_attr_nfc_activated_se_info.ka.attr,
	&nfc_attr_nfcservice_lock.ka.attr,
	&nfc_attr_nfc_hal_dmd.ka.attr,
	NULL,
};

static const struct attribute_group nfc_attr_group = {
	.attrs = nfc_attrs,
};

static struct kobject *nfc_kobj;

static int __init ohos_nfc_sysfs_init(void)
{
	int ret;

	if (hmos_disabled(HMOS_OFF_NFC)) {
		pr_info("ohos_nfc: disabled by hmos.disable\n");
		return 0;
	}
	nfc_kobj = kobject_create_and_add("nfc", NULL);
	if (!nfc_kobj)
		return -ENOMEM;
	ret = sysfs_create_group(nfc_kobj, &nfc_attr_group);
	if (ret) {
		kobject_put(nfc_kobj);
		nfc_kobj = NULL;
		return ret;
	}
	pr_info("ohos_nfc: /sys/nfc ready (nq-nci)\n");
	return 0;
}
late_initcall(ohos_nfc_sysfs_init);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("HarmonyOS /sys/nfc attributes");
