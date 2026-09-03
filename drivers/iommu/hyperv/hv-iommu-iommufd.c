// SPDX-License-Identifier: GPL-2.0
/*
 * Hyper-V root vIOMMU IOMMUFD support.
 * Copyright (C) 2026, Microsoft, Inc.
 */

#include <linux/file.h>
#include <linux/iommufd.h>
#include <linux/slab.h>

#include <asm/mshyperv.h>

#include "hv-iommu.h"

struct hv_iommu_viommu {
	struct iommufd_viommu core;
	struct file *vm_file;
	u64 partid;
};

static const struct iommufd_viommu_ops hv_iommu_hypervisor_viommu_ops;

static struct hv_iommu_viommu *
to_hv_iommu_viommu(struct iommufd_viommu *viommu)
{
	return container_of(viommu, struct hv_iommu_viommu, core);
}

static void hv_iommu_viommu_destroy(struct iommufd_viommu *viommu)
{
	struct hv_iommu_viommu *hv_viommu = to_hv_iommu_viommu(viommu);

	fput(hv_viommu->vm_file);
}

static struct iommu_domain *
hv_iommu_alloc_domain_external(struct iommufd_viommu *viommu, u32 flags,
			       const struct iommu_user_data *user_data)
{
	struct hv_iommu_viommu *hv_viommu = to_hv_iommu_viommu(viommu);
	struct iommu_hwpt_external external = {};
	struct hv_domain *hvdom;
	int rc;

	if (viommu->type != IOMMU_VIOMMU_TYPE_HYPERVISOR)
		return ERR_PTR(-EOPNOTSUPP);
	if (flags)
		return ERR_PTR(-EOPNOTSUPP);
	if (!user_data || user_data->type != IOMMU_HWPT_DATA_EXTERNAL)
		return ERR_PTR(-EOPNOTSUPP);

	rc = iommu_copy_struct_from_user(&external, user_data,
					 IOMMU_HWPT_DATA_EXTERNAL, flags);
	if (rc)
		return ERR_PTR(rc);
	if (external.flags || external.__reserved)
		return ERR_PTR(-EOPNOTSUPP);

	hvdom = kzalloc_obj(*hvdom, GFP_KERNEL_ACCOUNT);
	if (!hvdom)
		return ERR_PTR(-ENOMEM);

	hvdom->iommu_dom.type = IOMMU_DOMAIN_EXTERNAL;
	hvdom->iommu_dom.ops = &hv_iommu_external_domain_ops;
	hvdom->iommu_dom.geometry = hv_iommu_default_geometry;
	hvdom->iommu_dom.pgsize_bitmap = HV_IOMMU_PGSIZES;
	hvdom->partid = hv_viommu->partid;

	return &hvdom->iommu_dom;
}

static const struct iommufd_viommu_ops hv_iommu_hypervisor_viommu_ops = {
	.destroy = hv_iommu_viommu_destroy,
	.alloc_domain_external = hv_iommu_alloc_domain_external,
};

size_t hv_iommufd_get_viommu_size(struct device *dev,
				  enum iommu_viommu_type viommu_type)
{
	if (viommu_type != IOMMU_VIOMMU_TYPE_HYPERVISOR)
		return 0;
	return VIOMMU_STRUCT_SIZE(struct hv_iommu_viommu, core);
}

int hv_iommufd_viommu_init(struct iommufd_viommu *viommu,
			   struct iommu_domain *parent_domain,
			   const struct iommu_user_data *user_data)
{
	struct hv_iommu_viommu *hv_viommu = to_hv_iommu_viommu(viommu);
	struct iommu_viommu_hypervisor hypervisor = {};
	int rc;

	if (viommu->type != IOMMU_VIOMMU_TYPE_HYPERVISOR)
		return -EOPNOTSUPP;
	if (parent_domain || !user_data)
		return -EINVAL;

	rc = iommu_copy_struct_from_user(&hypervisor, user_data,
					 IOMMU_VIOMMU_TYPE_HYPERVISOR, vm_fd);
	if (rc)
		return rc;
	if (hypervisor.flags || hypervisor.__reserved)
		return -EOPNOTSUPP;

	hv_viommu->vm_file = fget(hypervisor.vm_fd);
	if (!hv_viommu->vm_file)
		return -EBADF;

	hv_viommu->partid =
		mshv_partition_file_get_partid(hv_viommu->vm_file);
	if (hv_viommu->partid == HV_PARTITION_ID_INVALID) {
		rc = IS_REACHABLE(CONFIG_MSHV_ROOT) ? -EINVAL : -EOPNOTSUPP;
		goto out_put_file;
	}

	viommu->ops = &hv_iommu_hypervisor_viommu_ops;
	return 0;

out_put_file:
	fput(hv_viommu->vm_file);
	hv_viommu->vm_file = NULL;
	return rc;
}
