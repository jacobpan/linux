/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_HYPERV_IOMMU_H
#define _LINUX_HYPERV_IOMMU_H

#include <linux/err.h>
#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/types.h>

struct device;
#if IS_ENABLED(CONFIG_HYPERV_ROOT_IOMMU)
bool hv_iommu_root_managed(struct device *dev);
#else
static inline bool hv_iommu_root_managed(struct device *dev)
{
	return false;
}

#endif

#endif
