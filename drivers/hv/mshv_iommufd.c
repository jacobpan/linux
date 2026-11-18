// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026, Microsoft Corporation.
 */

#include <linux/hyperv.h>
#include <linux/iommufd.h>
#include <linux/pci.h>
#include <linux/file.h>
#include <asm/hypervisor.h>
#include <asm/mshyperv.h>

#include "mshv_root.h"

struct mshv_iommufd_viommu {
	struct iommufd_viommu core;
	u64 partition_id;
};

static const struct iommufd_viommu_ops mshv_iommufd_viommu_ops;

static struct mshv_iommufd_viommu *
to_mshv_iommufd_viommu(struct iommufd_viommu *viommu)
{
	return container_of(viommu, struct mshv_iommufd_viommu, core);
}

static bool mshv_iommufd_test_mode(void)
{
#ifdef CONFIG_X86
	return IS_ENABLED(CONFIG_IOMMUFD_TEST) &&
	       x86_hyper_type == X86_HYPER_KVM;
#else
	return false;
#endif
}

static u64 mshv_iommufd_host_device_id(struct pci_dev *pdev)
{
	u64 devid = hv_pci_vmbus_device_id(pdev);

	return devid ? devid : hv_build_devid_type_pci(pdev);
}

static int mshv_iommufd_attach_device(struct iommufd_vdevice *vdev)
{
	struct mshv_iommufd_viommu *mshv_viommu =
		to_mshv_iommufd_viommu(vdev->viommu);
	struct hv_input_attach_device test_input;
	struct hv_input_attach_device *input;
	struct pci_dev *pdev =
		to_pci_dev(iommufd_vdevice_get_device(vdev));
	unsigned long flags;
	u64 status;
	int rc;

	rc = mshv_partition_file_prepare_attach_internal(vdev->viommu->provider_file);
	if (rc)
		return rc;

	do {
		if (mshv_iommufd_test_mode()) {
			input = &test_input;
		} else {
			local_irq_save(flags);
			input = *this_cpu_ptr(hyperv_pcpu_input_arg);
		}
		memset(input, 0, sizeof(*input));

		input->partition_id = mshv_viommu->partition_id;
		input->device_id.as_uint64 =
			mshv_iommufd_host_device_id(pdev);
		input->attdev_flags.logical_id = 1;
		input->logical_devid = vdev->virt_id;

		if (mshv_iommufd_test_mode()) {
			pr_info("mshv: KVM test attach partition %#llx device %#llx vDEVICE %#llx\n",
				input->partition_id,
				input->device_id.as_uint64, vdev->virt_id);
			return 0;
		}

		status = hv_do_hypercall(HVCALL_ATTACH_DEVICE, input, NULL);
		local_irq_restore(flags);

		if (hv_result_needs_memory(status)) {
			rc = hv_deposit_memory(mshv_viommu->partition_id,
					       status);
			if (rc)
				return rc;
		}
	} while (hv_result_needs_memory(status));

	if (!hv_result_success(status))
		hv_status_err(status, "\n");
	return hv_result_to_errno(status);
}

static int mshv_iommufd_detach_device(struct iommufd_vdevice *vdev)
{
	struct mshv_iommufd_viommu *mshv_viommu =
		to_mshv_iommufd_viommu(vdev->viommu);
	struct hv_input_detach_device test_input;
	struct hv_input_detach_device *input;
	unsigned long flags;
	u64 status;

	if (mshv_iommufd_test_mode()) {
		input = &test_input;
	} else {
		local_irq_save(flags);
		input = *this_cpu_ptr(hyperv_pcpu_input_arg);
	}
	memset(input, 0, sizeof(*input));
	input->partition_id = mshv_viommu->partition_id;
	input->logical_devid = vdev->virt_id;

	if (mshv_iommufd_test_mode()) {
		pr_info("mshv: KVM test detach partition %#llx vDEVICE %#llx\n",
			input->partition_id, vdev->virt_id);
		return 0;
	}

	status = hv_do_hypercall(HVCALL_DETACH_DEVICE, input, NULL);
	local_irq_restore(flags);

	if (!hv_result_success(status))
		hv_status_err(status, "\n");
	return hv_result_to_errno(status);
}

static size_t mshv_iommufd_get_viommu_size(struct device *dev,
					   enum iommu_viommu_type type)
{
	if (type != IOMMU_VIOMMU_TYPE_HYPERVISOR)
		return 0;
	return VIOMMU_STRUCT_SIZE(struct mshv_iommufd_viommu, core);
}

static int mshv_iommufd_viommu_init(struct iommufd_viommu *viommu,
				    struct device *dev,
				    struct iommu_domain *parent_domain,
				    const struct iommu_user_data *user_data)
{
	struct mshv_iommufd_viommu *mshv_viommu =
		to_mshv_iommufd_viommu(viommu);

	if (parent_domain)
		return -EINVAL;
	if (!viommu->provider_file)
		return -EBADFD;
	mshv_viommu->partition_id =
		mshv_partition_file_get_partid_internal(viommu->provider_file);
	if (mshv_viommu->partition_id == HV_PARTITION_ID_INVALID)
		return -EBADFD;
	viommu->ops = &mshv_iommufd_viommu_ops;
	return 0;
}

static const struct iommufd_viommu_ops mshv_iommufd_viommu_ops = {
	.flags = IOMMUFD_VIOMMU_NO_HWPT,
	.get_viommu_size = mshv_iommufd_get_viommu_size,
	.viommu_init = mshv_iommufd_viommu_init,
	.vdevice_size = sizeof(struct iommufd_vdevice),
	.vdevice_init = mshv_iommufd_attach_device,
	.vdevice_unassign = mshv_iommufd_detach_device,
};

static const struct iommufd_viommu_ops *
mshv_iommufd_get_viommu_ops(struct file *vm_file, struct device *dev,
			    enum iommu_viommu_type type)
{
	if (mshv_partition_file_get_partid_internal(vm_file) ==
	    HV_PARTITION_ID_INVALID)
		return ERR_PTR(-EBADFD);
	if (!dev_is_pci(dev) || type != IOMMU_VIOMMU_TYPE_HYPERVISOR)
		return ERR_PTR(-EOPNOTSUPP);
	return &mshv_iommufd_viommu_ops;
}

static struct iommufd_viommu_file_provider mshv_iommufd_file_provider = {
	.owner = THIS_MODULE,
	.get_ops = mshv_iommufd_get_viommu_ops,
};

int mshv_iommufd_init(void)
{
	return iommufd_viommu_file_provider_register(
		&mshv_iommufd_file_provider);
}

void mshv_iommufd_exit(void)
{
	iommufd_viommu_file_provider_unregister(&mshv_iommufd_file_provider);
}

MODULE_IMPORT_NS("IOMMUFD");
MODULE_LICENSE("GPL");
