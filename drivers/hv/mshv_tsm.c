// SPDX-License-Identifier: GPL-2.0-only
#include <linux/file.h>
#include <linux/iommufd.h>
#include <linux/hyperv-iommu.h>
#include <linux/module.h>
#include <linux/pci-tsm.h>
#include <linux/slab.h>
#include <linux/tsm.h>
#include <asm/mshyperv.h>

#include "mshv_root.h"

struct mshv_pci_tsm {
	struct pci_tsm core;
};

struct mshv_tsm_viommu {
	struct iommufd_viommu core;
	struct file *vm_file;
	u64 partition_id;
};

static struct tsm_dev *mshv_tsm_dev;

static struct mshv_tsm_viommu *
to_mshv_tsm_viommu(struct iommufd_viommu *viommu)
{
	return container_of(viommu, struct mshv_tsm_viommu, core);
}

static bool mshv_tsm_match(struct pci_dev *pdev)
{
	return hv_iommu_root_managed(&pdev->dev);
}

static struct pci_tsm *mshv_tsm_probe(struct tsm_dev *tsm_dev,
				      struct pci_dev *pdev)
{
	struct mshv_pci_tsm *mshv_tsm;

	mshv_tsm = kzalloc_obj(*mshv_tsm, GFP_KERNEL);
	if (!mshv_tsm)
		return NULL;
	if (pci_tsm_assignment_constructor(pdev, &mshv_tsm->core, tsm_dev)) {
		kfree(mshv_tsm);
		return NULL;
	}
	return &mshv_tsm->core;
}

static void mshv_tsm_remove(struct pci_tsm *tsm)
{
	kfree(container_of(tsm, struct mshv_pci_tsm, core));
}

static size_t mshv_tsm_get_viommu_size(struct device *dev,
				       enum iommu_viommu_type type)
{
	if (type != IOMMU_VIOMMU_TYPE_HYPERVISOR)
		return 0;

	return VIOMMU_STRUCT_SIZE(struct mshv_tsm_viommu, core);
}

static int mshv_tsm_viommu_init(struct iommufd_viommu *viommu,
				struct device *dev,
				struct iommu_domain *parent_domain,
				const struct iommu_user_data *user_data)
{
	struct mshv_tsm_viommu *mshv_viommu =
		to_mshv_tsm_viommu(viommu);
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

	mshv_viommu->vm_file = fget(hypervisor.vm_fd);
	if (!mshv_viommu->vm_file)
		return -EBADF;

	mshv_viommu->partition_id =
		mshv_partition_file_get_partid_internal(mshv_viommu->vm_file);
	if (mshv_viommu->partition_id == HV_PARTITION_ID_INVALID) {
		fput(mshv_viommu->vm_file);
		mshv_viommu->vm_file = NULL;
		return -EINVAL;
	}

	return 0;
}

static void mshv_tsm_viommu_destroy(struct iommufd_viommu *viommu)
{
	struct mshv_tsm_viommu *mshv_viommu =
		to_mshv_tsm_viommu(viommu);

	fput(mshv_viommu->vm_file);
}

static struct iommu_domain *
mshv_tsm_alloc_domain_external(struct iommufd_viommu *viommu, u32 flags,
			       const struct iommu_user_data *user_data)
{
	struct mshv_tsm_viommu *mshv_viommu =
		to_mshv_tsm_viommu(viommu);

	return hv_iommufd_alloc_domain_external(
		viommu, mshv_viommu->partition_id, flags, user_data);
}

static int mshv_tsm_vdevice_init(struct iommufd_vdevice *vdev)
{
	struct mshv_tsm_viommu *mshv_viommu =
		to_mshv_tsm_viommu(vdev->viommu);
	struct tsm_dev *tsm_dev;
	int rc;

	tsm_dev = tsm_get_device(iommufd_vdevice_to_device(vdev));
	if (IS_ERR(tsm_dev))
		return PTR_ERR(tsm_dev);
	if (tsm_dev != vdev->viommu->tsm_dev)
		rc = -EXDEV;
	else
		rc = mshv_partition_file_prepare_attach_internal(
			mshv_viommu->vm_file);
	if (tsm_dev)
		tsm_put_device(tsm_dev);

	return rc;
}

static const struct iommufd_viommu_ops mshv_tsm_viommu_ops = {
	.flags = IOMMUFD_VIOMMU_NO_HWPT,
	.get_viommu_size = mshv_tsm_get_viommu_size,
	.viommu_init = mshv_tsm_viommu_init,
	.destroy = mshv_tsm_viommu_destroy,
	.alloc_domain_external = mshv_tsm_alloc_domain_external,
	.vdevice_size = sizeof(struct iommufd_vdevice),
	.vdevice_init = mshv_tsm_vdevice_init,
};

static const struct iommufd_viommu_ops *
mshv_tsm_viommu_get_ops(struct device *dev, enum iommu_viommu_type type)
{
	if (!dev_is_pci(dev) || !hv_iommu_root_managed(dev))
		return ERR_PTR(-EOPNOTSUPP);
	if (type != IOMMU_VIOMMU_TYPE_HYPERVISOR)
		return ERR_PTR(-EOPNOTSUPP);

	return &mshv_tsm_viommu_ops;
}

static struct pci_tsm_ops mshv_pci_tsm_ops = {
	.owner = THIS_MODULE,
	.viommu_get_ops = mshv_tsm_viommu_get_ops,
	.assignment_ops = {
		.match = mshv_tsm_match,
		.probe = mshv_tsm_probe,
		.remove = mshv_tsm_remove,
	},
};

int mshv_tsm_init(struct device *parent)
{
	mshv_tsm_dev = tsm_register(parent, &mshv_pci_tsm_ops);
	if (IS_ERR(mshv_tsm_dev)) {
		int rc = PTR_ERR(mshv_tsm_dev);

		mshv_tsm_dev = NULL;
		return rc;
	}
	return 0;
}

void mshv_tsm_exit(void)
{
	if (mshv_tsm_dev)
		tsm_unregister(mshv_tsm_dev);
	mshv_tsm_dev = NULL;
}
