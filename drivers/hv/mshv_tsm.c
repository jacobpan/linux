// SPDX-License-Identifier: GPL-2.0-only
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

static struct tsm_dev *mshv_tsm_dev;

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

static const struct iommufd_viommu_ops *
mshv_tsm_viommu_get_ops(struct device *dev, enum iommu_viommu_type type)
{
	return NULL;
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
