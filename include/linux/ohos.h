/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Runtime switches for the OpenHarmony/HarmonyOS support code.
 *
 * Every hook can be disabled from the kernel command line, so a single build
 * can be bisected on the device without recompiling:
 *
 *   hmos.disable=nfc,memcg,fsemu,fsverity,drmauth,binder,access_token,noreboot,permissive
 *
 * With no hmos.disable= parameter everything is enabled (the normal case).
 */
#ifndef _LINUX_OHOS_H
#define _LINUX_OHOS_H

#define HMOS_OFF_ACCESS_TOKEN	(1u << 0)
#define HMOS_OFF_BINDER		(1u << 1)
#define HMOS_OFF_DRM_AUTH	(1u << 2)
#define HMOS_OFF_FS_EMU		(1u << 3)
#define HMOS_OFF_FSVERITY	(1u << 4)
#define HMOS_OFF_MEMCG		(1u << 5)
#define HMOS_OFF_NFC		(1u << 6)
#define HMOS_OFF_NOREBOOT	(1u << 7)
#define HMOS_OFF_PERMISSIVE	(1u << 8)

extern unsigned int hmos_disable_mask;

static inline bool hmos_disabled(unsigned int bit)
{
	return hmos_disable_mask & bit;
}

#endif /* _LINUX_OHOS_H */
