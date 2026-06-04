.. SPDX-License-Identifier: GPL-2.0-only

==============================================
Hyper-V PCI Passthrough via IOMMUFD
==============================================

:Author: Jacob Pan

Overview
========

When Linux runs as a privileged partition on Microsoft Hyper-V, PCI devices can
be passed through to guest VMs. Two host environments exist with different
capabilities:

1. **Baremetal root partition**: Linux runs directly on hardware as the root
   partition. The hypervisor provides device-domain hypercalls for explicit
   map/unmap of guest memory, such as ``HVCALL_MAP_DEVICE_GPA_PAGES``.
   Direct attach may also be supported.

2. **L1 virtual host (L1VH)**: Linux runs as a semi-privileged VM under a
   Windows root partition. Only **direct attach** is supported. The
   hypervisor uses the target VM's second-stage translation (EPT/NPT) for DMA
   translation, and no per-page map/unmap hypercalls are available to the
   Linux L1 host.

This document describes an IOMMUFD design for both cases, with the direct
attach path modeled through the existing vIOMMU/vDEVICE object model instead
of a new direct-attach object or a Hyper-V-specific VFIO UAPI.

The design follows the direction from Jason Gunthorpe's feedback:

* VFIO container cannot represent this model; the solution must be IOMMUFD
  based.
* The direct attach flow should accept an FD representing the target VM
  (for example a KVM or mshv VM FD). The driver converts that FD into the
  hypervisor identity needed to configure DMA for that VM.
* The VM identity used at the hypercall boundary must be bound to a Linux FD,
  immutable for the lifetime of the IOMMUFD object, and held by the IOMMU
  driver while the domain exists.
* Direct attach should look similar to vIOMMU setup: the vIOMMU represents the
  VM-scoped IOMMU virtualization context, vDEVICE represents the device's
  virtual identity inside that VM, and HWPT_NESTED represents the attachable
  direct-attach domain.

Relevant feedback:

* https://lore.kernel.org/all/20260509170051.GD9285@ziepe.ca/
* https://lore.kernel.org/linux-iommu/20260519125206.GY7702@ziepe.ca/

Design Goals
============

* Avoid VFIO-container extensions.
* Avoid a new IOMMUFD object type or new direct-attach ioctl.
* Reuse ``IOMMU_VIOMMU_ALLOC`` for the VM-scoped object.
* Reuse ``IOMMU_VDEVICE_ALLOC`` for the device's virtual identity in the VM.
* Reuse ``IOMMU_HWPT_ALLOC`` with ``pt_id = viommu_id`` to allocate an
  attachable direct-attach ``HWPT_NESTED``.
* Keep direct-attach domains free of ``map_pages`` and ``unmap_pages`` ops.
  Any attempt to use IOAS map/unmap for the L1VH direct attach path is a design
  bug, not a no-op in the driver.
* Keep the VM memory map owned by the VM subsystem. IOMMUFD does not manage the
  VM's second-stage page tables; it only asks the IOMMU driver to bind a VFIO
  device's DMA context to the VM represented by the VM FD.

Object Model
============

Baremetal mapped-device domains continue to use normal IOMMUFD objects::

    IOAS -> HWPT_PAGING -> VFIO device

The L1VH direct attach flow uses the existing vIOMMU shape::

    IOAS -> HWPT_PAGING(NEST_PARENT) -> vIOMMU(vm_fd)
                                      -> vDEVICE(virt_id)
                                      -> HWPT_NESTED(direct attach)
                                      -> VFIO device attach

The important object responsibilities are:

``HWPT_PAGING(NEST_PARENT)``
    Structural parent required by the vIOMMU API. It gives the vIOMMU a
    parent HWPT and associated physical IOMMU instance. For Hyper-V L1VH
    direct attach, it does not describe the VM memory map; the VM FD does.

``vIOMMU``
    VM-scoped IOMMU virtualization context. Hyper-V-specific vIOMMU data
    carries a VM FD. The Hyper-V IOMMU driver validates the FD, obtains an
    immutable hypervisor VM/partition handle, and holds the FD for the vIOMMU
    lifetime.

``vDEVICE``
    Binds the physical VFIO device to the vIOMMU and records the device's
    virtual ID inside the VM. For Hyper-V this ``virt_id`` is the logical
    device ID used by the direct attach hypercall.

``HWPT_NESTED``
    Attachable direct-attach domain allocated from the vIOMMU via
    ``IOMMU_HWPT_ALLOC``. The Hyper-V driver allocates an
    ``IOMMU_DOMAIN_NESTED`` with attach/free ops only. The domain has no
    map/unmap ops because DMA translation is the VM's second-stage page table
    managed by the hypervisor/VM subsystem.

Baremetal Root Partition Flow
=============================

On baremetal, the hypervisor supports mapped device domains. The VMM creates a
paging domain, attaches the device, then issues IOAS map/unmap operations which
the Hyper-V IOMMU driver translates to hypercalls.

::

    VMM                         IOMMUFD / IOMMU Driver          Hypervisor
     |                                   |                           |
     | open /dev/iommu                   |                           |
     | open /dev/vfio/devices/vfioN      |                           |
     | VFIO_DEVICE_BIND_IOMMUFD          |                           |
     |---------------------------------->|                           |
     | IOMMU_IOAS_ALLOC                  |                           |
     |---------------------------------->|                           |
     | IOMMU_HWPT_ALLOC (PAGING)         | domain_alloc_paging()     |
     |  pt_id = ioas_id                  |--CREATE_DEVICE_DOMAIN---->|
     |---------------------------------->|                           |
     | VFIO_DEVICE_ATTACH_IOMMUFD_PT     | attach_dev()              |
     |  pt_id = hwpt_paging_id           |--ATTACH_DEVICE_DOMAIN---->|
     |---------------------------------->|                           |
     | IOMMU_IOAS_MAP (GPA ranges)       | map_pages()               |
     |---------------------------------->|--MAP_DEVICE_GPA_PAGES---->|
     | IOMMU_IOAS_UNMAP                  | unmap_pages()             |
     |---------------------------------->|--UNMAP_DEVICE_GPA_PAGES-->|

This path is a normal ``HWPT_PAGING`` path. The paging domain has real
``map_pages`` and ``unmap_pages`` ops. The VMM may manually allocate the HWPT
to keep per-device domain control instead of relying on IOAS auto-domain
sharing.

L1VH Direct Attach Flow
=======================

On L1VH, the device is assigned directly to a target VM. IOMMUFD does not map
GPA pages. Instead, the Hyper-V IOMMU driver asks the hypervisor to associate
the physical device with the target VM's second-stage translation.

The flow uses existing IOMMUFD ioctls.

Step 1: Bind the VFIO Device
----------------------------

::

    iommufd = open("/dev/iommu", O_RDWR);
    vfio_dev_fd = open("/dev/vfio/devices/vfioN", O_RDWR);

    struct vfio_device_bind_iommufd bind = {
        .argsz = sizeof(bind),
        .iommufd = iommufd,
    };
    ioctl(vfio_dev_fd, VFIO_DEVICE_BIND_IOMMUFD, &bind);
    dev_id = bind.out_devid;

Step 2: Allocate the vIOMMU Parent HWPT
---------------------------------------

The current vIOMMU UAPI requires a nesting parent ``HWPT_PAGING``. For the
direct attach path this parent is structural; the VM FD supplied to vIOMMU
allocation identifies the VM memory map.

::

    struct iommu_ioas_alloc ioas_alloc = {};
    ioctl(iommufd, IOMMU_IOAS_ALLOC, &ioas_alloc);
    ioas_id = ioas_alloc.out_ioas_id;

    struct iommu_hwpt_alloc parent_alloc = {
        .size = sizeof(parent_alloc),
        .dev_id = dev_id,
        .pt_id = ioas_id,
        .flags = IOMMU_HWPT_ALLOC_NEST_PARENT,
    };
    ioctl(iommufd, IOMMU_HWPT_ALLOC, &parent_alloc);
    parent_hwpt_id = parent_alloc.out_hwpt_id;

The VMM must not use this IOAS as the L1VH DMA map. It is not a substitute for
the VM FD and does not contain authoritative second-stage mappings for direct
attach.

Step 3: Allocate a Hyper-V vIOMMU
---------------------------------

The VMM allocates a vIOMMU using a new Hyper-V vIOMMU type. The driver-specific
data carries an FD for the target VM.

::

    struct iommu_viommu_hyperv hv_viommu = {
        .vm_fd = vm_fd,
    };

    struct iommu_viommu_alloc viommu_alloc = {
        .size = sizeof(viommu_alloc),
        .type = IOMMU_VIOMMU_TYPE_HYPERV,
        .dev_id = dev_id,
        .hwpt_id = parent_hwpt_id,
        .data_len = sizeof(hv_viommu),
        .data_uptr = (uintptr_t)&hv_viommu,
    };
    ioctl(iommufd, IOMMU_VIOMMU_ALLOC, &viommu_alloc);
    viommu_id = viommu_alloc.out_viommu_id;

The Hyper-V ``viommu_init`` callback must:

* validate that ``vm_fd`` is a supported VM FD;
* obtain the immutable hypervisor partition/VM identity from that FD;
* take and hold a reference to the FD/file while the vIOMMU exists;
* reject raw partition IDs or process-ID based ownership checks.

The VM subsystem remains responsible for the VM memory map. IOMMUFD only holds
the VM identity needed by the IOMMU driver to configure DMA ownership.

Step 4: Allocate a vDEVICE
--------------------------

The VMM creates a vDEVICE for the VFIO device inside the vIOMMU. The existing
``virt_id`` field carries the Hyper-V logical device ID.

::

    struct iommu_vdevice_alloc vdev_alloc = {
        .size = sizeof(vdev_alloc),
        .viommu_id = viommu_id,
        .dev_id = dev_id,
        .virt_id = logical_device_id,
    };
    ioctl(iommufd, IOMMU_VDEVICE_ALLOC, &vdev_alloc);
    vdevice_id = vdev_alloc.out_vdevice_id;

This avoids putting per-device logical IDs into HWPT allocation data. That is
important because a single HWPT may be attached to multiple devices, while each
device can have a different virtual/logical ID inside the VM.

Step 5: Allocate the Direct-Attach HWPT
---------------------------------------

The VMM allocates an ``HWPT_NESTED`` from the vIOMMU. This uses the existing
``IOMMU_HWPT_ALLOC`` path where ``pt_id`` names a vIOMMU object.

::

    struct iommu_hwpt_hyperv_direct direct = {
        .flags = 0,
    };

    struct iommu_hwpt_alloc hwpt_alloc = {
        .size = sizeof(hwpt_alloc),
        .dev_id = dev_id,
        .pt_id = viommu_id,
        .data_type = IOMMU_HWPT_DATA_HYPERV_DIRECT,
        .data_len = sizeof(direct),
        .data_uptr = (uintptr_t)&direct,
    };
    ioctl(iommufd, IOMMU_HWPT_ALLOC, &hwpt_alloc);
    direct_hwpt_id = hwpt_alloc.out_hwpt_id;

The Hyper-V vIOMMU ``alloc_domain_nested`` callback allocates an
``IOMMU_DOMAIN_NESTED`` with no map/unmap ops. It records a reference to the
vIOMMU and obtains all VM identity through the vIOMMU object, not from raw UAPI
fields.

Step 6: Attach the VFIO Device
------------------------------

::

    struct vfio_device_attach_iommufd_pt attach = {
        .argsz = sizeof(attach),
        .pt_id = direct_hwpt_id,
    };
    ioctl(vfio_dev_fd, VFIO_DEVICE_ATTACH_IOMMUFD_PT, &attach);

During ``attach_dev`` the Hyper-V driver:

* finds the vDEVICE for this physical device and vIOMMU;
* reads the logical device ID from ``vdevice->virt_id``;
* uses the VM identity held by the vIOMMU;
* issues the Hyper-V direct attach hypercall.

No IOAS map/unmap operations are required or valid for this L1VH path.

Step 7: Teardown
----------------

The VMM detaches the VFIO device, destroys the direct HWPT, destroys the
vDEVICE, then destroys the vIOMMU. Destroying the vIOMMU drops the held VM FD
reference. The VM FD cannot be reused or retargeted to change the identity of an
existing vIOMMU.

UAPI Additions
==============

No new IOMMUFD object type or ioctl is proposed. Hyper-V direct attach only
needs Hyper-V-specific enum values and data structures for existing ioctls.

.. code-block:: c

    enum iommu_viommu_type {
        ...
        IOMMU_VIOMMU_TYPE_HYPERV,
    };

    struct iommu_viommu_hyperv {
        __s32 vm_fd;
        __u32 flags;
        __aligned_u64 reserved;
    };

    enum iommu_hwpt_data_type {
        ...
        IOMMU_HWPT_DATA_HYPERV_DIRECT,
    };

    struct iommu_hwpt_hyperv_direct {
        __u32 flags;
        __u32 reserved;
    };

Open UAPI points:

* The exact FD type should be coordinated with the VM subsystem. It may be a
  KVM VM FD, an mshv VM/partition FD, or a common FD abstraction if multiple
  hypervisors share the flow.
* If Hyper-V direct attach needs output data from vIOMMU allocation, the data
  structure can follow the existing vIOMMU pattern where driver-specific data
  contains input and output fields.
* The direct HWPT data may remain empty if all information is carried by the
  vIOMMU and vDEVICE. It still provides an explicit driver-specific HWPT type
  so unsupported uses can be rejected cleanly.

Kernel Implementation Plan
==========================

Phase 1: Refactor Hyper-V IOMMU Driver Skeleton
-----------------------------------------------

* Move Hyper-V IOMMU code under ``drivers/iommu/hyperv/``.
* Split IRQ remapping from DMA translation/direct attach code.
* Add a Hyper-V IOMMU driver that probes pass-through-capable devices and
  advertises the needed IOMMUFD/vIOMMU hooks.
* Keep baremetal mapped domains and L1VH direct attach as separate domain
  implementations.

Deliverable: no UAPI change; code organization and minimal driver skeleton.

Phase 2: Baremetal HWPT_PAGING Support
--------------------------------------

* Implement ``domain_alloc_paging_flags`` or ``domain_alloc_paging`` for
  Hyper-V mapped device domains.
* Implement real ``map_pages`` and ``unmap_pages`` callbacks that issue
  Hyper-V map/unmap hypercalls.
* Implement attach/detach for mapped device domains.
* Add error unwinding for partial map/unmap failures.

Deliverable: root-partition flow through IOAS, HWPT_PAGING, and VFIO device.

Phase 3: VM FD Binding Helper
-----------------------------

* Define a helper, likely in the VM/hypervisor driver, that validates a VM FD
  and returns an immutable VM/partition handle usable by Hyper-V hypercalls.
* The helper must take a file reference or otherwise provide a lifetime object
  held by the IOMMU driver.
* Do not accept raw partition IDs from userspace for IOMMUFD direct attach.
* Do not infer ownership from ``current``, ``tgid``, or process lifetime.

Deliverable: internal kernel API for "FD -> immutable Hyper-V VM identity".

Phase 4: Hyper-V vIOMMU Type
----------------------------

* Add ``IOMMU_VIOMMU_TYPE_HYPERV`` and ``struct iommu_viommu_hyperv``.
* Implement ``get_viommu_size`` and ``viommu_init``.
* In ``viommu_init``, copy the VM FD data, validate it with the Phase 3 helper,
  and hold the VM reference for the vIOMMU lifetime.
* Implement vIOMMU destroy to release the VM reference.

Deliverable: ``IOMMU_VIOMMU_ALLOC`` creates a Hyper-V vIOMMU bound to a VM FD.

Phase 5: Hyper-V vDEVICE Support
--------------------------------

* Implement vIOMMU ``vdevice_size`` and ``vdevice_init`` if Hyper-V needs
  driver-private vDEVICE state.
* Treat ``iommu_vdevice_alloc::virt_id`` as the Hyper-V logical device ID.
* Validate logical device ID uniqueness within the vIOMMU, and validate that
  the physical device can be represented by that logical ID.
* Ensure vDEVICE destruction reverses any per-device vIOMMU state.

Deliverable: ``IOMMU_VDEVICE_ALLOC`` records the VM-visible identity of each
assigned device.

Phase 6: Direct-Attach HWPT_NESTED
----------------------------------

* Add ``IOMMU_HWPT_DATA_HYPERV_DIRECT`` if an explicit direct-attach HWPT data
  type is required.
* Implement vIOMMU ``alloc_domain_nested`` to allocate a direct-attach
  ``IOMMU_DOMAIN_NESTED``.
* The domain must provide ``attach_dev`` and ``free``. It must not provide
  ``map_pages`` or ``unmap_pages``.
* ``attach_dev`` must verify that the device has a vDEVICE in the target
  vIOMMU and then issue the direct attach hypercall using:

  * the VM identity held by the vIOMMU;
  * the physical device being attached;
  * the logical device ID from the vDEVICE.

Deliverable: ``IOMMU_HWPT_ALLOC`` with ``pt_id = viommu_id`` returns an
attachable direct-attach HWPT.

Phase 7: VFIO Attach/Detach Integration
---------------------------------------

* Use existing ``VFIO_DEVICE_ATTACH_IOMMUFD_PT`` and
  ``VFIO_DEVICE_DETACH_IOMMUFD_PT``.
* On detach, issue the Hyper-V detach hypercall and return the device to a safe
  host-owned domain.
* Ensure device destruction, vDEVICE destruction, HWPT destruction, and VM FD
  close orderings cannot leave a device attached to a dead VM identity.

Deliverable: VFIO cdev assigned devices can enter and leave Hyper-V direct
attach via existing VFIO/IOMMUFD ioctls.

Phase 8: Selftests and Validation
---------------------------------

* Extend the IOMMUFD selftest mock vIOMMU to model a VM-FD-backed vIOMMU and a
  direct-attach HWPT with no map/unmap ops.
* Add tests for:

  * vIOMMU allocation rejects invalid VM FDs;
  * vDEVICE logical IDs are unique and associated with the correct vIOMMU;
  * direct HWPT allocation from the vIOMMU succeeds only with the Hyper-V data
    type;
  * attaching without a vDEVICE fails;
  * map/unmap on the L1VH direct path is not possible;
  * destroying objects in different orders preserves VM FD and device
    lifetimes.

* Add Hyper-V integration tests when the hypervisor test environment is
  available.

Deliverable: selftest coverage for the IOMMUFD object model before Hyper-V
hardware/hypercall testing is required.

Comparison with the Previous Proposal
=====================================

The previous design put ``partid`` and ``logical_devid`` in Hyper-V-specific
``IOMMU_HWPT_ALLOC`` data for a custom nested direct domain. That has two
problems:

* A raw partition ID is not a lifetime-safe Linux object. The IOMMU driver must
  hold an FD-backed VM identity instead.
* A logical device ID is per virtual device, not per HWPT. Putting it in HWPT
  allocation data does not work cleanly for multi-device domains.

The vIOMMU design fixes both:

* ``IOMMU_VIOMMU_ALLOC`` carries and pins the VM FD.
* ``IOMMU_VDEVICE_ALLOC`` carries the logical device ID.
* ``IOMMU_HWPT_ALLOC`` from the vIOMMU produces the attachable direct domain
  without introducing a new IOMMUFD object or ioctl.

Open Questions
==============

* Which VM FD type should be standardized for Hyper-V: KVM VM FD, mshv
  partition FD, or a small common hypervisor FD abstraction?
* Should ``IOMMU_HWPT_DATA_HYPERV_DIRECT`` carry any fields, or should all
  information come from vIOMMU and vDEVICE?
* Does Hyper-V require one direct HWPT per device, or can one direct HWPT be
  shared by multiple vDEVICEs under the same vIOMMU?
* What exact detach semantics are required when the VM FD is closed while VFIO
  devices remain attached? The preferred rule is that vIOMMU holds the VM
  reference, so VM teardown must first destroy or forcibly detach the IOMMUFD
  objects.

References
==========

* ``Documentation/userspace-api/iommufd.rst`` -- IOMMUFD object model,
  including vIOMMU and vDEVICE.
* ``include/uapi/linux/iommufd.h`` -- IOMMUFD UAPI definitions.
* https://lore.kernel.org/all/20260509170051.GD9285@ziepe.ca/
* https://lore.kernel.org/linux-iommu/20260519125206.GY7702@ziepe.ca/
