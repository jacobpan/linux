/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_HYPERV_IOMMU_H
#define _LINUX_HYPERV_IOMMU_H

#include <linux/err.h>
#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/types.h>

struct device;
struct iommufd_viommu;
struct iommu_domain;
struct iommu_user_data;

#if IS_ENABLED(CONFIG_HYPERV_ROOT_IOMMU)
bool hv_iommu_root_managed(struct device *dev);
struct iommu_domain *
hv_iommufd_alloc_domain_external(struct iommufd_viommu *viommu,
				 u64 partition_id, u32 flags,
				 const struct iommu_user_data *user_data);
#else
static inline bool hv_iommu_root_managed(struct device *dev)
{
	return false;
}

static inline struct iommu_domain *
hv_iommufd_alloc_domain_external(struct iommufd_viommu *viommu,
				 u64 partition_id, u32 flags,
				 const struct iommu_user_data *user_data)
{
	return ERR_PTR(-EOPNOTSUPP);
}
#endif

#endif
