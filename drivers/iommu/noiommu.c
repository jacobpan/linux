// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025, Microsoft Corporation.
 */
#define DEBUG
#define pr_fmt(fmt) "NOIOMMU: " fmt
#include <linux/device.h>
#include <linux/iommu.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/generic_pt/iommu.h>
#include <linux/vfio.h>

#include "iommu-priv.h"

struct noiommu_dev {
	struct iommu_device		iommu;
	struct device *dev;
};

struct noiommu_domain {
	union {
		struct iommu_domain domain;
		struct pt_iommu iommu;
		struct pt_iommu_amdv1 amdv1;
	};
};

static struct iommu_ops noiommu_ops;

struct noiommu_dev noiommu_dev = {
    .iommu = {
        .ops = &noiommu_ops,
    },
};

static void noiommu_release_device(struct device *dev)
{
}

static struct iommu_device *noiommu_probe_device(struct device *dev)
{
	/* Support VFIO PCI devices only */
	if (!dev_is_pci(dev))
		return ERR_PTR(-ENODEV);

	return &noiommu_dev.iommu;
}

static int noiommu_attach_dev(struct iommu_domain *domain, struct device *dev)
{
	return 0;
}

static void noiommu_domain_free(struct iommu_domain *domain)
{
	kfree(domain);
}

static const struct iommu_domain_ops noiommu_amdv1_ops = {
	IOMMU_PT_DOMAIN_OPS(amdv1),
	.free = noiommu_domain_free,
	.attach_dev = noiommu_attach_dev,
};

static struct iommu_domain *
noiommu_domain_alloc_paging_flags(struct device *dev, u32 flags,
			       const struct iommu_user_data *user_data)
{
	struct noiommu_domain *noiommu_dom;
	struct pt_iommu_amdv1_cfg cfg = {};
	int rc;

	if (user_data)
		return ERR_PTR(-EOPNOTSUPP);
#if 0 // no need to check, this module only loads when vfio_noiommu is enabled
	if (vfio_noiommu_enabled() == false) {
		pr_info("Must enable unsafe_noiommu_mode\n");
		return ERR_PTR(-ENODEV);
	}
#endif
	cfg.common.hw_max_vasz_lg2 = 64;
	cfg.common.hw_max_oasz_lg2 = 52;
	cfg.common.features = BIT(PT_FEAT_AMDV1_FORCE_COHERENCE);
	cfg.starting_level = 2;

	noiommu_dom = kzalloc(sizeof(*noiommu_dom), GFP_KERNEL);
	if (!noiommu_dom)
		return ERR_PTR(-ENOMEM);

	noiommu_dom->amdv1.iommu.nid = NUMA_NO_NODE;
	noiommu_dom->domain.ops = &noiommu_amdv1_ops;

	/* Use mock page table which is based on AMDV1 */
	rc = pt_iommu_amdv1_noiommu_init(&noiommu_dom->amdv1, &cfg, GFP_KERNEL);
	if (rc) {
		kfree(noiommu_dom);
		return ERR_PTR(rc);
	}

	return &noiommu_dom->domain;
}

static int noiommu_domain_nop_attach(struct iommu_domain *domain,
				  struct device *dev)
{
	return 0;
}

static const struct iommu_domain_ops noiommu_nop_ops = {
	.attach_dev = noiommu_domain_nop_attach,
};

static struct iommu_domain noiommu_identity_domain = {
	.type = IOMMU_DOMAIN_IDENTITY,
	.ops = &noiommu_nop_ops,
};

static struct iommu_domain noiommu_blocking_domain = {
	.type = IOMMU_DOMAIN_BLOCKED,
	.ops = &noiommu_nop_ops,
};

static bool noiommu_capable(struct device *dev, enum iommu_cap cap)
{
	switch (cap) {
	/* Fake cache coherency support to allow iommufd-dev bind */
	case IOMMU_CAP_CACHE_COHERENCY:
		return true;
	default:
		return false;
	}
}

static struct iommu_ops noiommu_ops = {
	.default_domain = &noiommu_identity_domain,
	.blocked_domain = &noiommu_blocking_domain,
	.capable		= noiommu_capable,
	.domain_alloc_paging_flags = noiommu_domain_alloc_paging_flags,
	.probe_device		= noiommu_probe_device,
	.release_device		= noiommu_release_device,
	.device_group = generic_device_group,
	.owner			= THIS_MODULE,
	.default_domain_ops = &(const struct iommu_domain_ops) {
		.attach_dev		= noiommu_attach_dev,
		.free			= noiommu_domain_free,
	}
};

struct notifier_block noiommu_bus_nb = {
	/* data */
};

static int iommu_noiommu_dev_add(struct device *dev, struct iommu_device *iommu)
{
	return iommu_fwspec_init(dev, iommu->fwnode);
}

int noiommu_init(void)
{
	struct pci_dev *pdev = NULL;

	if (iommu_is_registered()) {
		pr_info("IOMMU devices already registered, skipping No-IOMMU driver\n");
		return 0;
	}
    pr_debug("Initializing No-IOMMU driver\n");
    iommu_device_sysfs_add(&noiommu_dev.iommu, noiommu_dev.dev, NULL,
     "%s", "noiommu");

	if (iommu_device_register_bus(&noiommu_dev.iommu, &noiommu_ops,
		&pci_bus_type, &noiommu_bus_nb))
		return -ENODEV;

	for_each_pci_dev(pdev) {
		if (iommu_noiommu_dev_add(&pdev->dev, &noiommu_dev.iommu)) {
			dev_err(&pdev->dev, "Failed to add no-IOMMU fwspec \n");
			continue;
		}
		iommu_probe_device(&pdev->dev);
		dev_dbg(&pdev->dev, "Probed PCI device for no IOMMU\n");
	}

	return 0;
}
//early_initcall(noiommu_init);

static void __exit noiommu_exit(void)
{
    pr_debug("Exiting No-IOMMU driver\n");

	/* No hardware resources to clean up */
    iommu_device_unregister(&noiommu_dev.iommu);

}
#if 0
module_init(noiommu_init);
module_exit(noiommu_exit);

MODULE_DESCRIPTION("No-IOMMU driver for PCI devices without hardware IOMMU");
MODULE_AUTHOR("Anonymous");
MODULE_LICENSE("GPL v2");
#endif