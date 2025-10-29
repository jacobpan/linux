#define DEBUG
#define pr_fmt(fmt) "NOIOMMU: " fmt

#include <linux/device.h>
#include <linux/iommu.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/types.h>

#include "iommu-priv.h"

struct noiommu_dev {
   	struct iommu_device		iommu;
	struct device *dev;
};

struct noiommu_domain {
	struct iommu_domain		domain;
	struct xarray pfns;
};

struct noiommu_endpoint {
    struct device *dev;
    struct noiommu_dev *noiommu;
    struct noiommu_domain *domain;
};

static struct iommu_ops noiommu_ops;

/* HACK: see if we can pass probe */
struct noiommu_dev noiommu_dev = {
    .iommu = {
        .ops = &noiommu_ops,
    },
};

struct iommu_device *get_noiommu_dev(void)
{
	return &noiommu_dev.iommu;
}

static void noiommu_release_device(struct device *dev)
{
	/* No-op: No hardware resources to release */
}

static phys_addr_t noiommu_iova_to_phys(struct iommu_domain *domain,
					dma_addr_t iova)
{
	/* No translation, return invalid address */
	return 0;
}

static struct iommu_device *noiommu_probe_device(struct device *dev)
{
    pr_alert("no_iommu: %s for no IOMMU support\n", dev_name(dev));
	/* Only probe PCI devices */
	if (!dev_is_pci(dev))
		return ERR_PTR(-ENODEV);

	return &noiommu_dev.iommu; /* No IOMMU device instance */
}

static int noiommu_attach_dev(struct iommu_domain *domain, struct device *dev)
{
	/* Allow attachment but no actual hardware setup */
	return 0;
}

enum {
	NOIOMMU_IO_PAGE_SIZE = PAGE_SIZE / 2,
	NOIOMMU_APERTURE_START = 1UL << 24,
	NOIOMMU_APERTURE_LAST = (1UL << 31) - 1,
};
static struct iommu_domain *
noiommu_domain_alloc_paging_flags(struct device *dev, u32 flags,
			       const struct iommu_user_data *user_data)
{
	struct noiommu_domain *noiommu_dom;

	dev_alert(dev, "%s Allocating No-IOMMU domain\n", __func__);

	if (user_data)
		return ERR_PTR(-EOPNOTSUPP);

	noiommu_dom = kzalloc(sizeof(*noiommu_dom), GFP_KERNEL);
	if (!noiommu_dom)
		return ERR_PTR(-ENOMEM);

	noiommu_dom->domain.geometry.aperture_start = NOIOMMU_APERTURE_START;
	noiommu_dom->domain.geometry.aperture_end = NOIOMMU_APERTURE_LAST;
	noiommu_dom->domain.pgsize_bitmap = NOIOMMU_IO_PAGE_SIZE;

	noiommu_dom->domain.ops = noiommu_ops.default_domain_ops;
	noiommu_dom->domain.type = IOMMU_DOMAIN_UNMANAGED;
	xa_init(&noiommu_dom->pfns);

	return &noiommu_dom->domain;
}

static void noiommu_domain_free(struct iommu_domain *domain)
{
	kfree(domain);
}

static int noiommu_map_pages(struct iommu_domain *domain, unsigned long iova,
			     phys_addr_t paddr, size_t size, size_t count,
			     int prot, gfp_t gfp, size_t *mapped)
{
	pr_alert("no_iommu: mapping IOVA 0x%lx to PA 0x%lx size 0x%lx\n",
			iova, (unsigned long)paddr, (unsigned long)size);
	/* No-op: No page table mappings */
	if (mapped)
		*mapped = size;
	return 0;
}

static size_t noiommu_unmap_pages(struct iommu_domain *domain,
				  unsigned long iova, size_t size,
				  size_t count,
				  struct iommu_iotlb_gather *gather)
{
	pr_alert("no_iommu: unmapping IOVA 0x%lx size 0x%lx\n",
			iova, (unsigned long)size);
	/* No-op: No page table mappings to undo */
	return size;
}

static void noiommu_flush_iotlb_all(struct iommu_domain *domain)
{
    /* No-op: No hardware IOTLB to flush */
}

static void noiommu_iotlb_sync(struct iommu_domain *domain,
                   struct iommu_iotlb_gather *gather)
{
    /* No-op: No hardware IOTLB to sync */
}

static int noiommu_iotlb_sync_map(struct iommu_domain *domain,
                                  unsigned long iova, size_t size)
{
    /* No-op: No hardware IOTLB to sync */
    return 0;
}

static int mock_domain_nop_attach(struct iommu_domain *domain,
				  struct device *dev)
{
	/* No-op: No hardware IOMMU to attach */
	return 0;
}

static int mock_domain_set_dev_pasid_nop(struct iommu_domain *domain,
					 struct device *dev, ioasid_t pasid,
					 struct iommu_domain *old)
{
	/* No-op: No PASID support in this mock domain */
	return 0;
}

static const struct iommu_domain_ops mock_identity_ops = {
	.attach_dev = mock_domain_nop_attach,
	.set_dev_pasid = mock_domain_set_dev_pasid_nop
};

static struct iommu_domain noiommu_identity_domain = {
	.type = IOMMU_DOMAIN_IDENTITY,
	.ops = &mock_identity_ops,
};

static struct iommu_domain noiommu_blocking_domain = {
	.type = IOMMU_DOMAIN_BLOCKED,
	.ops = &mock_identity_ops,
};

static bool noiommu_capable(struct device *dev, enum iommu_cap cap)
{
	switch (cap) {
	case IOMMU_CAP_CACHE_COHERENCY:
		dev_alert(dev, "fake noIOMMU support for cache coherency\n");
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
	.no_iommu = true,
	.default_domain_ops = &(const struct iommu_domain_ops) {
		.attach_dev		= noiommu_attach_dev,
		.map_pages		= noiommu_map_pages,
		.unmap_pages		= noiommu_unmap_pages,
		.iova_to_phys		= noiommu_iova_to_phys,
		.flush_iotlb_all	= noiommu_flush_iotlb_all,
		.iotlb_sync		= noiommu_iotlb_sync,
		.iotlb_sync_map		= noiommu_iotlb_sync_map,
		.free			= noiommu_domain_free,
	}
};

struct notifier_block noiommu_bus_nb = {
	/* data */
};

static int __init noiommu_init(void)
{
	struct pci_dev *pdev = NULL;

    pr_debug("Initializing No-IOMMU driver\n");
    iommu_device_sysfs_add(&noiommu_dev.iommu, noiommu_dev.dev, NULL,
     "%s", "noiommu");

	if (iommu_device_register_bus(&noiommu_dev.iommu, &noiommu_ops,
		&pci_bus_type, &noiommu_bus_nb))
		return -ENODEV;

	for_each_pci_dev(pdev) {
		iommu_probe_device(&pdev->dev);
		dev_dbg(&pdev->dev, "Probed PCI device for no IOMMU\n");
	}

	return 0;
}
early_initcall(noiommu_init);

static void __exit noiommu_exit(void)
{
    pr_debug("Exiting No-IOMMU driver\n");

	/* No hardware resources to clean up */
    iommu_device_unregister(&noiommu_dev.iommu);

}

module_init(noiommu_init);
module_exit(noiommu_exit);

MODULE_DESCRIPTION("No-IOMMU driver for PCI devices without hardware IOMMU");
MODULE_AUTHOR("Anonymous");
MODULE_LICENSE("GPL v2");