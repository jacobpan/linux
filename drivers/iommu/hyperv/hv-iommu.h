/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Hyper-V root vIOMMU driver.
 * Copyright (C) 2026, Microsoft, Inc.
 */

#ifndef __HYPERV_IOMMU_H
#define __HYPERV_IOMMU_H

#include <linux/interval_tree.h>
#include <linux/iommu.h>
#include <linux/sizes.h>
#include <linux/spinlock.h>

#define HV_IOMMU_PGSIZES SZ_4K  /* for now, to be enhanced */

struct iommufd_viommu;
struct iommu_user_data;

struct hv_domain {
	struct iommu_domain iommu_dom;
	u32 domid_num;			      /* as opposed to domain_id.type */
	u64 partid;			      /* partition id for external attach */
	spinlock_t mappings_lock;	      /* protects mappings_tree */
	struct rb_root_cached mappings_tree;  /* iova to pa lookup tree */
};

#define to_hv_domain(d) container_of(d, struct hv_domain, iommu_dom)

extern const struct iommu_domain_geometry hv_iommu_default_geometry;
extern const struct iommu_domain_ops hv_iommu_external_domain_ops;

size_t hv_iommufd_get_viommu_size(struct device *dev,
				  enum iommu_viommu_type viommu_type);
int hv_iommufd_viommu_init(struct iommufd_viommu *viommu,
			   struct iommu_domain *parent_domain,
			   const struct iommu_user_data *user_data);

#endif /* __HYPERV_IOMMU_H */
