// SPDX-License-Identifier: GPL-2.0
/*
 * hmos.disable=<feature>[,<feature>...] - turn individual OpenHarmony hooks
 * off at boot so a single kernel can be bisected on the device.
 */
#define pr_fmt(fmt) "ohos: " fmt

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/ohos.h>
#include <linux/string.h>

unsigned int hmos_disable_mask;

static const struct {
	const char *name;
	unsigned int bit;
} hmos_features[] = {
	{ "access_token", HMOS_OFF_ACCESS_TOKEN },
	{ "binder",	  HMOS_OFF_BINDER },
	{ "drmauth",	  HMOS_OFF_DRM_AUTH },
	{ "fsemu",	  HMOS_OFF_FS_EMU },
	{ "fsverity",	  HMOS_OFF_FSVERITY },
	{ "memcg",	  HMOS_OFF_MEMCG },
	{ "nfc",	  HMOS_OFF_NFC },
	{ "noreboot",	  HMOS_OFF_NOREBOOT },
	{ "permissive",	  HMOS_OFF_PERMISSIVE },
};

static int __init hmos_disable_setup(char *str)
{
	char *tok;

	while ((tok = strsep(&str, ",")) != NULL) {
		unsigned int i;

		if (!*tok)
			continue;
		for (i = 0; i < ARRAY_SIZE(hmos_features); i++) {
			if (!strcmp(tok, hmos_features[i].name)) {
				hmos_disable_mask |= hmos_features[i].bit;
				break;
			}
		}
		if (i == ARRAY_SIZE(hmos_features))
			pr_warn("unknown feature '%s'\n", tok);
	}
	pr_info("disabled by cmdline: mask %#x\n", hmos_disable_mask);
	return 1;
}
__setup("hmos.disable=", hmos_disable_setup);
