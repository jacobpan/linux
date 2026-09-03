// SPDX-License-Identifier: GPL-2.0-only
/*
 * Intel VT-d IOMMUFD integration.
 */

#define pr_fmt(fmt)     "DMAR: " fmt
#define dev_fmt(fmt)    pr_fmt(fmt)

#include <linux/file.h>
#include <linux/iommufd.h>
#include <linux/pci.h>
#include <linux/slab.h>

#include <asm/mshyperv.h>

#include "iommu.h"
#include "pasid.h"

struct intel_iommu_viommu {
	struct iommufd_viommu core;
	struct file *vm_file;
	u64 partid;
};

struct intel_iommu_external_domain {
	struct iommu_domain domain;
	struct iommufd_viommu *viommu;
};

static const struct iommufd_viommu_ops intel_hypervisor_viommu_ops;

static struct intel_iommu_viommu *
to_intel_iommu_viommu(struct iommufd_viommu *viommu)
{
	return container_of(viommu, struct intel_iommu_viommu, core);
}

static struct intel_iommu_external_domain *
to_intel_iommu_external_domain(struct iommu_domain *domain)
{
	return container_of(domain, struct intel_iommu_external_domain, domain);
}

static void intel_iommu_external_domain_free(struct iommu_domain *domain)
{
	kfree(to_intel_iommu_external_domain(domain));
}

static int intel_iommu_external_attach_dev(struct iommu_domain *domain,
					   struct device *dev,
					   struct iommu_domain *old)
{
	struct intel_iommu_external_domain *external_domain =
		to_intel_iommu_external_domain(domain);
	struct device_domain_info *info = dev_iommu_priv_get(dev);
	struct intel_iommu *iommu = info->iommu;
	unsigned long vdev_id;
	int ret;

	ret = iommufd_viommu_get_vdev_id(external_domain->viommu, dev,
					 &vdev_id);
	if (ret)
		return ret;

	dev_info(dev, "MSHV external attach vdev_id=%lu\n", vdev_id);

	device_block_translation(dev);

	if (dev_is_real_dma_subdevice(dev))
		return 0;

	if (sm_supported(iommu))
		ret = intel_pasid_setup_pass_through(iommu, dev,
						     IOMMU_NO_PASID);
	else
		ret = intel_iommu_setup_pass_through(dev);

	if (!ret)
		info->domain_attached = true;

	return ret;
}

/*
 * This is only a QEMU plumbing shim for IOMMUFD external attach HWPT testing.
 * Keep the domain type distinct for the UAPI flow, but reuse pass-through
 * programming until a real hypervisor-backed implementation exists.
 */
static const struct iommu_domain_ops intel_external_domain_ops = {
	.attach_dev = intel_iommu_external_attach_dev,
	.free = intel_iommu_external_domain_free,
};

static struct iommu_domain *
intel_iommu_alloc_domain_external(struct iommufd_viommu *viommu, u32 flags,
				  const struct iommu_user_data *user_data)
{
	struct intel_iommu_external_domain *external_domain;
	struct iommu_hwpt_external external = {};
	struct iommu_domain *domain;
	int ret;

	if (viommu->type != IOMMU_VIOMMU_TYPE_HYPERVISOR)
		return ERR_PTR(-EOPNOTSUPP);
	if (flags)
		return ERR_PTR(-EOPNOTSUPP);
	if (!user_data || user_data->type != IOMMU_HWPT_DATA_EXTERNAL)
		return ERR_PTR(-EOPNOTSUPP);

	ret = iommu_copy_struct_from_user(&external, user_data,
					  IOMMU_HWPT_DATA_EXTERNAL, flags);
	if (ret)
		return ERR_PTR(ret);
	if (external.flags || external.__reserved)
		return ERR_PTR(-EOPNOTSUPP);

	external_domain = kzalloc_obj(*external_domain, GFP_KERNEL_ACCOUNT);
	if (!external_domain)
		return ERR_PTR(-ENOMEM);

	external_domain->viommu = viommu;
	domain = &external_domain->domain;
	domain->type = IOMMU_DOMAIN_EXTERNAL;
	domain->ops = &intel_external_domain_ops;
	return domain;
}

static void intel_iommu_viommu_destroy(struct iommufd_viommu *viommu)
{
	struct intel_iommu_viommu *intel_viommu =
		to_intel_iommu_viommu(viommu);

	fput(intel_viommu->vm_file);
}

static const struct iommufd_viommu_ops intel_hypervisor_viommu_ops = {
	.destroy = intel_iommu_viommu_destroy,
	.alloc_domain_external = intel_iommu_alloc_domain_external,
};

size_t intel_iommu_get_viommu_size(struct device *dev,
				   enum iommu_viommu_type viommu_type)
{
	if (viommu_type != IOMMU_VIOMMU_TYPE_HYPERVISOR)
		return 0;
	return VIOMMU_STRUCT_SIZE(struct intel_iommu_viommu, core);
}

int intel_iommu_viommu_init(struct iommufd_viommu *viommu,
			    struct iommu_domain *parent_domain,
			    const struct iommu_user_data *user_data)
{
	struct intel_iommu_viommu *intel_viommu =
		to_intel_iommu_viommu(viommu);
	struct iommu_viommu_hypervisor hypervisor = {};
	int ret;

	if (viommu->type != IOMMU_VIOMMU_TYPE_HYPERVISOR)
		return -EOPNOTSUPP;
	if (parent_domain || !user_data)
		return -EINVAL;

	ret = iommu_copy_struct_from_user(&hypervisor, user_data,
					  IOMMU_VIOMMU_TYPE_HYPERVISOR, vm_fd);
	if (ret)
		return ret;
	if (hypervisor.flags || hypervisor.__reserved)
		return -EOPNOTSUPP;

	intel_viommu->vm_file = fget(hypervisor.vm_fd);
	if (!intel_viommu->vm_file)
		return -EBADF;

	intel_viommu->partid =
		mshv_partition_file_get_partid(intel_viommu->vm_file);
	if (intel_viommu->partid == HV_PARTITION_ID_INVALID) {
		ret = IS_REACHABLE(CONFIG_MSHV_ROOT) ? -EINVAL : -EOPNOTSUPP;
		goto out_put_file;
	}

	pr_info("hypervisor vIOMMU init: vm_fd=%u partid=0x%llx\n",
		hypervisor.vm_fd, intel_viommu->partid);

	viommu->ops = &intel_hypervisor_viommu_ops;
	return 0;

out_put_file:
	fput(intel_viommu->vm_file);
	intel_viommu->vm_file = NULL;
	return ret;
}
