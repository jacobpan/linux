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

struct hv_domain {
	struct iommu_domain iommu_dom;
	u32 domid_num;			      /* as opposed to domain_id.type */
	spinlock_t mappings_lock;	      /* protects mappings_tree */
	struct rb_root_cached mappings_tree;  /* iova to pa lookup tree */
};

#define to_hv_domain(d) container_of(d, struct hv_domain, iommu_dom)

#endif /* __HYPERV_IOMMU_H */
