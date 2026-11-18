.. SPDX-License-Identifier: GPL-2.0-only

=============================
MSHV direct device assignment
=============================

MSHV direct assignment lets a VMM transfer ownership of a VFIO device to a
Hyper-V partition without creating an IOMMUFD HW page table for the guest.
IOMMUFD provides the host-DMA isolation and object lifetime transaction, while
MSHV performs the Hyper-V assignment and reclaim hypercalls.

Userspace flow
--------------

1. Bind a VFIO cdev to an IOMMUFD and obtain its IOMMUFD device ID.
2. Create an MSHV partition and retain the partition file descriptor.
3. Allocate an ``IOMMU_VIOMMU_TYPE_HYPERVISOR`` vIOMMU using
   ``struct iommu_viommu_hypervisor`` with the partition fd in ``vm_fd``.
4. Allocate a vDEVICE with the per-partition logical device ID in ``virt_id``.

The vDEVICE allocation is the assignment operation. There is no external HWPT
allocation and no subsequent VFIO HWPT attach.

The VM fd selects the assignment provider. An MSHV partition fd selects the
MSHV provider, while a KVM VM fd is offered to the KVM architecture provider.
The physical IOMMU driver is not used to select a hypervisor provider.

Host isolation transaction
--------------------------

Before invoking the provider's vDEVICE initialization callback, IOMMUFD:

* requires the device to be the only member of its physical IOMMU group;
* rejects existing PASID attachments;
* replaces an existing RID attachment with the IOMMU driver's blocking domain;
* prevents concurrent host HWPT attach or replace operations.

If provider assignment fails, IOMMUFD restores the original RID attachment.
If assignment succeeds, IOMMUFD discards the old host attachment bookkeeping
and leaves the physical group in the blocking domain.

Reclaim
-------

Destroying the vDEVICE invokes the provider's reclaim callback. For MSHV this
issues ``HVCALL_DETACH_DEVICE``.

An explicit destroy failure leaves the vDEVICE published and the physical group
blocked, so userspace can retry reclaim. IOMMUFD context teardown logs reclaim
failures and continues cleanup, but never restores host DMA after an
unsuccessful reclaim.

MSHV provider
-------------

The MSHV provider pins the partition file for the vIOMMU lifetime and derives
the Hyper-V partition ID from that file. vDEVICE allocation first pins the
partition's registered memory regions, then issues ``HVCALL_ATTACH_DEVICE``.
When Hyper-V requests additional deposited memory, the provider deposits the
requested memory and retries the attach hypercall.

The physical device ID is derived from the host PCI device. The vDEVICE
``virt_id`` is supplied to Hyper-V as the logical device ID.

Limitations
-----------

The initial implementation supports only singleton physical IOMMU groups and
does not permit PASID attachments during direct assignment.
