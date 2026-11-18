// SPDX-License-Identifier: GPL-2.0-only

#include <linux/iommufd.h>
#include <linux/kvm_host.h>

const struct iommufd_viommu_ops * __weak
kvm_arch_iommufd_get_viommu_ops(struct kvm *kvm, struct device *dev,
				enum iommu_viommu_type type)
{
	return ERR_PTR(-EOPNOTSUPP);
}

static const struct iommufd_viommu_ops *
kvm_iommufd_get_viommu_ops(struct file *vm_file, struct device *dev,
			   enum iommu_viommu_type type)
{
	struct kvm *kvm = file_to_kvm(vm_file);

	if (!kvm)
		return ERR_PTR(-EBADFD);
	if (type != IOMMU_VIOMMU_TYPE_HYPERVISOR)
		return ERR_PTR(-EOPNOTSUPP);
	return kvm_arch_iommufd_get_viommu_ops(kvm, dev, type);
}

static struct iommufd_viommu_file_provider kvm_iommufd_file_provider = {
	.get_ops = kvm_iommufd_get_viommu_ops,
};

int kvm_iommufd_init(struct module *owner)
{
	kvm_iommufd_file_provider.owner = owner;
	return iommufd_viommu_file_provider_register(
		&kvm_iommufd_file_provider);
}

void kvm_iommufd_exit(void)
{
	iommufd_viommu_file_provider_unregister(&kvm_iommufd_file_provider);
}

MODULE_IMPORT_NS("IOMMUFD");
