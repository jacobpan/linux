<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Microsoft Hypervisor PCI Passthrough via IOMMUFD

**Author:** Jacob Pan

## 1. Overview

When Linux runs as a privileged partition on Microsoft Hypervisor (MSHV), PCI
devices can be passed through to guest VMs. The IOMMUFD UAPI should distinguish
the attach mode from the host environment:

1. **Domain attach**: Linux owns an IOAS-backed paging domain. The VMM maps and
   unmaps guest memory through IOMMUFD, and the MSHV IOMMU driver translates
   those operations to device-domain hypercalls such as
   `HVCALL_MAP_DEVICE_GPA_PAGES`.

2. **Direct attach**: The hypervisor associates the physical device with the
   target VM's second-stage translation (EPT/NPT). Linux does not own per-page
   DMA mappings for that device, so IOAS map/unmap is not part of this attach
   mode.

Two host environments can expose either mode depending on MSHV capability. The
baremetal root partition commonly supports both. An L1 virtual host (L1VH)
commonly needs direct attach, but could use domain attach too if the hypervisor
exposes map/unmap hypercalls to the L1 host.

For historical reasons, Linux code and directory names often use "Hyper-V" or
"hyperv" for Microsoft hypervisor interfaces. Hyper-V is the Windows
virtualization stack, while MSHV refers specifically to the Microsoft
Hypervisor. This document uses MSHV for hypervisor-provided mechanisms, but
some existing or proposed Linux identifiers may continue to use Hyper-V naming
for consistency with the current kernel code.

This document describes an IOMMUFD design for both attach modes, with the
direct attach path modeled as a vIOMMU-inspired, VM-backed IOMMUFD object flow
instead of hacking a VFIO unmanaged paging domain for direct attach. This avoids
the fundamental problem in the previous proposal, where a single IOAS-backed
paging/unmanaged domain type was used for both attach modes. In that design:

* Direct attach domains must implement `map_pages`/`unmap_pages` as
  **no-ops** that silently discard the caller's work.
* The IOAS layer still performs full map/unmap bookkeeping, including interval
  tree maintenance and page pinning, all wasted because the ops discard
  everything.

The design follows the direction from Jason Gunthorpe's feedback [^1] [^2]:

* VFIO container cannot represent this model; the solution must be IOMMUFD
  based.
* The direct attach flow should accept an opaque FD representing the target VM
  (for example a KVM or mshv VM FD). IOMMUFD does not interpret the FD type;
  the IOMMU driver converts that FD into the hypervisor identity needed to
  configure DMA for that VM.
* The VM identity used at the hypercall boundary must be bound to a Linux FD,
  immutable for the lifetime of the IOMMUFD object, and held by the IOMMU
  driver while the domain exists.
* Direct attach should be rooted in a vIOMMU because vIOMMU is the IOMMUFD
  object representing the VM's isolated slice of a physical IOMMU. The vIOMMU
  carries the VM FD and pins the immutable VM identity. A new direct HWPT/domain
  is then allocated under that vIOMMU to represent the VM's externally managed
  second-stage translation [^2]. A vDEVICE records the device's virtual
  identity inside that VM.
* The UAPI should be consolidated with similar hypervisor needs instead of
  creating an MSHV-only ABI [^2]. Xen PV-IOMMU has the same high-level
  problem: Dom0 can use VFIO/IOMMUFD, but Xen owns the operation that moves a
  device's DMA context into another guest. The kernel needs a way to express
  "attach this device to this VM" and "this device is DMA-attached to this VM"
  without pretending that Dom0 owns a normal paging domain for it [^3].

The Xen PV-IOMMU discussion describes an externally managed domain model:
Xen provides Dom0 IOMMU support and has hypercalls to move a PCI device into
another guest, but current VFIO/IOMMUFD objects do not directly describe that
relationship. Toolstack-only coordination can leave Xen and Linux out of sync
about device ownership. This is the same class of problem as Direct
attach, so the design should share the same IOMMUFD object model across
hypervisor-backed direct attach implementations [^2] [^3].

Relevant feedback:

[^1]: https://lore.kernel.org/all/20260509170051.GD9285@ziepe.ca/
[^2]: https://lore.kernel.org/linux-iommu/20260519125206.GY7702@ziepe.ca/
[^3]: Teddy Astie, "How to express externally managed IOMMU domains for VFIO/IOMMUFD?", Apr 22 2026.

## 2. Design Goals

* Avoid VFIO-container extensions.
* Reuse the same IOMMUFD object-model shape for MSHV and other hypervisors
  with externally managed device-to-VM attach, such as Xen PV-IOMMU.
* Use vIOMMU as the per-VM isolation object. Direct vIOMMU allocation carries
  an opaque VM FD, and the IOMMU driver converts that FD into the immutable
  hypervisor identity needed at the hypercall boundary.
* Add or extend the vIOMMU child HWPT allocation path for externally managed
  direct domains. `IOMMU_HWPT_ALLOC` with `pt_id = viommu_id` should be able
  to allocate a direct HWPT/domain, not only `HWPT_NESTED`.
* Do not force direct attach into an IOAS-backed paging/unmanaged domain with
  no-op map/unmap ops, and do not model the base direct attach state as
  `HWPT_NESTED`.
* Reuse `IOMMU_VDEVICE_ALLOC` for the device's virtual identity in the VM.
  Direct attach should be allowed only for devices that have a vDEVICE under
  the same vIOMMU.
* Keep the direct attach path out of IOAS map/unmap. Any attempt to use IOAS
  map/unmap for this path is a design bug, not a no-op in the driver.
* Keep the VM memory map owned by the VM subsystem. IOMMUFD does not manage the
  VM's second-stage page tables; it only asks the IOMMU driver to bind a VFIO
  device's DMA context to the VM represented by the VM FD.

## 3. Object Model

Domain attach with map/unmap continues to use normal IOMMUFD objects:

```
IOAS -> HWPT_PAGING -> VFIO device
```

Direct attach uses a VM-FD-backed vIOMMU with a direct HWPT child:

```
VM fd -> vIOMMU(vm_fd) -> vDEVICE(virt_id)
                       -> HWPT_DIRECT(externally managed S2)
                       -> VFIO device attach
```

Object model diagram:

```
[1] VM identity              [2] VM IOMMU slice
   _____________                __________________
  |             |              |                  |
  |    VM fd    |------------->|      vIOMMU      |
  |_____________|              |     (vm_fd)      |
                               |__________________|
                                        |
                                        | IOMMU_VDEVICE_ALLOC
                                        | (dev_id, virt_id)
                                        v
[3] VFIO cdev FD               [4] vDEVICE link
   ___________________            ___________________
  |                   |   bind   |                   |
  | /dev/vfio/... fd  |--------->| dev_id, virt_id  |
  |___________________|          |___________________|
            |                              |
            | VFIO_DEVICE_BIND_IOMMUFD    | virt_id
            | returns dev_id              v
           v                    _____________________
      __________                | VM-visible device |
     |          |               | id for hypercall  |
     |  DEVICE  |               |___________________|
     |__________|                         |
           |                              | attach_dev uses
           |                              | virt_id
           v                              v
      ____v___                [5] Direct domain
     | struct |                   _______________
     | device |<-----------------|               |
     |________|      attach      |  HWPT_DIRECT  |
                                | (VM S2 DMA)   |
                                |_______________|
                                        |
                                  ______v______
                                 | direct      |
                                 | iommu_domain|
                                 |_____________|
```

The important object responsibilities are:

**vIOMMU**
VM-scoped IOMMU virtualization context and isolation object. Direct vIOMMU
data carries the VM FD. The hypervisor-specific IOMMU driver validates the FD,
obtains an immutable hypervisor VM/domain handle, and holds that handle for the
vIOMMU lifetime. Direct vIOMMU allocation should use an explicit
no-paging-parent mode instead of requiring an IOAS-backed nesting-parent HWPT.
`HWPT_DIRECT` cannot be passed as
`iommu_viommu_alloc::hwpt_id` because it is allocated later as a child of
the vIOMMU and holds a reference back to the vIOMMU.

**vDEVICE**
Binds the physical VFIO device to the vIOMMU and records the device's
virtual ID inside the VM. For MSHV this `virt_id` is the logical
device ID used by the direct attach hypercall. The existing
`IOMMU_VDEVICE_ALLOC` UAPI has the right parent and identity model. MSHV
direct attach uses this single logical-device-ID encoding for `virt_id`;
host representations such as SBDF must be translated or validated before the
vDEVICE is created, not passed as an alternate `virt_id` encoding.

**HWPT_DIRECT**
New attachable HWPT child allocated under a vIOMMU for a driver-managed VM
translation. It is not linked to an IOAS and IOMMUFD must not call
`iopt_table_add_domain()` or replay IOAS maps into it. The direct HWPT
holds a reference to the vIOMMU, uses the VM identity pinned by that vIOMMU,
and implements attach/detach operations that move the device's DMA
ownership to/from the target VM.

Direct attach should not use `HWPT_NESTED`. Jason's point is that the direct
attach case needs a unique domain allocator for a special domain described by
the VM-FD-backed vIOMMU; nested-domain machinery is only for real nested
translation. If an unprivileged guest later needs nested translation, that is a
separate MSHV nested translation flow that needs a real nesting parent; it
is not provided by the direct-only `HWPT_DIRECT` object.

## 4. Domain Attach Flow

For domain attach, the hypervisor supports mapped device domains. The VMM
creates a paging domain, attaches the device, then issues IOAS map/unmap
operations which the MSHV IOMMU driver translates to hypercalls:

```
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
```

This path is a normal `HWPT_PAGING` path. The paging domain has real
`map_pages` and `unmap_pages` ops. The VMM may manually allocate the HWPT
to keep per-device domain control instead of relying on IOAS auto-domain
sharing.

## 5. Direct Attach Flow

For direct attach, the device is assigned directly to a target VM. IOMMUFD does
not map GPA pages. Instead, the MSHV IOMMU driver asks the hypervisor to
associate the physical device with the target VM's second-stage translation.

The flow uses the VFIO/IOMMUFD object model with the vIOMMU and direct-HWPT
extensions described below.

### Step 1: Bind the VFIO Device

```
iommufd = open("/dev/iommu", O_RDWR);
vfio_dev_fd = open("/dev/vfio/devices/vfioN", O_RDWR);

struct vfio_device_bind_iommufd bind = {
    .argsz = sizeof(bind),
    .iommufd = iommufd,
};
ioctl(vfio_dev_fd, VFIO_DEVICE_BIND_IOMMUFD, &bind);
dev_id = bind.out_devid;
```

### Step 2: Allocate a direct vIOMMU

The VMM allocates a vIOMMU using a new direct vIOMMU type. The
driver-specific data carries an opaque FD for the target VM. The FD type is
private to the MSHV IOMMU driver and the VM subsystem; IOMMUFD only carries
the data to the driver-specific `viommu_init` callback.

`IOMMU_VIOMMU_TYPE_DIRECT` is the explicit no-nesting-parent vIOMMU
type. The vIOMMU is still the per-VM IOMMU isolation object, but its base
direct attach mode does not need an IOAS-backed nesting parent. `hwpt_id` is
omitted and must be zero for this type.

Do not pass a `HWPT_DIRECT` ID in `iommu_viommu_alloc::hwpt_id`. The direct
HWPT is allocated after the vIOMMU, under the vIOMMU, and gets the VM identity
from the vIOMMU. Using it as the vIOMMU parent would create a circular
lifetime dependency.

```
struct iommu_viommu_direct direct_viommu = {
    .vm_fd = vm_fd,
};

struct iommu_viommu_alloc viommu_alloc = {
    .size = sizeof(viommu_alloc),
    .type = IOMMU_VIOMMU_TYPE_DIRECT,
    .dev_id = dev_id,
    .data_len = sizeof(direct_viommu),
    .data_uptr = (uintptr_t)&direct_viommu,
};
ioctl(iommufd, IOMMU_VIOMMU_ALLOC, &viommu_alloc);
viommu_id = viommu_alloc.out_viommu_id;
```

The direct `viommu_init` callback must:

* validate that `vm_fd` is a supported VM FD;
* obtain the immutable hypervisor partition/VM identity from that FD;
* take and hold a reference to the FD/file while the vIOMMU exists;
* reject raw partition IDs or process-ID based ownership checks.

For mshv, `vm_fd` is the partition FD returned by `MSHV_CREATE_PARTITION`
on `/dev/mshv`:

```
mshv_fd = open("/dev/mshv", O_RDWR);
vm_fd = ioctl(mshv_fd, MSHV_CREATE_PARTITION, &create_partition);
```

The mshv implementation creates a `struct mshv_partition`, obtains the
MSHV partition ID, then returns an anon-inode file named
`"mshv_partition"` whose `private_data` points at that partition. The
direct vIOMMU helper should validate that FD type, pin the partition/file for
the vIOMMU lifetime, and derive the immutable partition identity from the
pinned object. It must not use `mshv_current_partid()` or any other
`current`/`tgid` based lookup, because those do not provide FD-backed object
lifetime.

Xen should follow the same FD-backed identity rule if it implements this common
direct attach model. The FD passed in vIOMMU driver data should represent an
immutable Xen domain identity and keep that domain identity valid for the
vIOMMU lifetime. A raw `domid_t` is not sufficient because Xen may recycle
domain IDs after a domain is destroyed.

The current Linux `/dev/xen/privcmd` interface is close in shape but is not
itself enough for this contract. `IOCTL_PRIVCMD_RESTRICT` stores a restricted
`domid` in `file->private_data` and prevents later operations on other
domains, but that file-private `domid` is still just an integer restriction;
it does not by itself pin the Xen domain object for IOMMUFD. A Xen vIOMMU
implementation would need either a real Xen domain FD or a helper that validates
a suitable Xen FD, pins the underlying domain identity, and exposes that pinned
identity to the IOMMU driver.

The VM subsystem remains responsible for the VM memory map. IOMMUFD only holds
the VM identity needed by the IOMMU driver to configure DMA ownership.

### Step 3: Allocate a vDEVICE

The VMM creates a vDEVICE for the VFIO device inside the vIOMMU. The existing
`virt_id` field carries the direct-attach logical device ID.

```
struct iommu_vdevice_alloc vdev_alloc = {
    .size = sizeof(vdev_alloc),
    .viommu_id = viommu_id,
    .dev_id = dev_id,
    .virt_id = logical_device_id,
};
ioctl(iommufd, IOMMU_VDEVICE_ALLOC, &vdev_alloc);
vdevice_id = vdev_alloc.out_vdevice_id;
```

This avoids putting per-device logical IDs into HWPT allocation data. That is
important because a single direct HWPT under the vIOMMU may be attached to
multiple devices, while each device can have a different virtual/logical ID
inside the VM.

### Step 4: Allocate a Direct HWPT Under the vIOMMU

The VMM allocates a direct HWPT from the vIOMMU. This uses the vIOMMU as the VM
isolation parent, but it must not call the existing
`alloc_domain_nested`/`HWPT_NESTED` path. The direct HWPT is an attachable
domain whose S2 translation is externally managed by the target VM/hypervisor.

```
struct iommu_hwpt_direct direct = {
    .flags = 0,
};

struct iommu_hwpt_alloc direct_alloc = {
    .size = sizeof(direct_alloc),
    .dev_id = dev_id,
    .pt_id = viommu_id,
    .data_type = IOMMU_HWPT_DATA_DIRECT,
    .data_len = sizeof(direct),
    .data_uptr = (uintptr_t)&direct,
};
ioctl(iommufd, IOMMU_HWPT_ALLOC, &direct_alloc);
direct_hwpt_id = direct_alloc.out_hwpt_id;
```

The direct HWPT allocator must:

* allocate an `iommu_domain` that is attachable but not IOAS-backed;
* hold a reference to the vIOMMU so the VM FD/identity remains valid for the
  direct HWPT lifetime;
* reject allocation if the vIOMMU does not represent the same physical IOMMU as
  the target device;
* not provide `map_pages` or `unmap_pages` ops.

### Step 5: Attach the VFIO Device to the Direct HWPT

The VMM attaches the VFIO device to the vIOMMU-backed direct HWPT. There is no
IOAS map/unmap flow and no `HWPT_NESTED` allocation for the base direct attach
case.

```
struct vfio_device_attach_iommufd_pt attach = {
    .argsz = sizeof(attach),
    .pt_id = direct_hwpt_id,
};
ioctl(vfio_dev_fd, VFIO_DEVICE_ATTACH_IOMMUFD_PT, &attach);
```

During `attach_dev` the MSHV driver:

* treats the direct HWPT as the VM's externally managed S2 translation;
* finds the vDEVICE for this physical device under the direct HWPT's vIOMMU;
* reads the logical device ID from `vdevice->virt_id`;
* uses the VM identity held by the vIOMMU;
* issues the direct attach hypercall.

No IOAS map/unmap operations are required or valid for this direct attach path.

### Step 6: Optional Nested Translation

If the target VM later needs guest-visible vIOMMU facilities or nested
translation, that should be modeled as a real nested translation flow. It must
not be confused with the base direct attach domain. A direct-only
`IOMMU_VIOMMU_TYPE_DIRECT` vIOMMU has no nesting-parent `HWPT_PAGING` for
the existing `HWPT_NESTED` path, so nested translation needs either a vIOMMU
allocated with a real nesting parent or a future UAPI that explicitly supplies a
nesting parent for that flow. `HWPT_NESTED` remains the object for a
guest-provided nested translation; it is not the base direct attach object.

### Step 7: Teardown

The VMM detaches the VFIO device, destroys the direct HWPT, destroys the
vDEVICE, then destroys the vIOMMU. Destroying the vIOMMU drops the held VM FD
reference. The VM FD cannot be reused or retargeted to change the identity of
an existing vIOMMU.

### Prototype Plan

The first prototype should prove the direct-attach object lifetime without
depending on real Hyper-V hardware assignment. It should use the IOMMUFD
selftest mock IOMMU driver and mock domains for the new direct HWPT path, then
add only the minimum MSHV hack needed for a QEMU/KVM environment to obtain a
VM-like FD for vIOMMU allocation.

#### Phase 1: Add a temporary MSHV VM FD hook for QEMU/KVM testing

For QEMU/KVM lifecycle testing, add a temporary MSHV-only hack that lets the
test environment obtain a VM-like FD even when the real MSHV partition backend
is not available. The FD only needs to satisfy the vIOMMU lifetime test: it
must be reference-counted, identifiable as an MSHV VM object by the prototype
validation path, and released when the vIOMMU is destroyed.

The prototype hack should be explicit and easy to remove:

* add a prototype-only module parameter or debug config knob, such as
  `mshv.fake_vm_fd=1`, and refuse the fake path unless that knob is enabled;
* in `mshv_parent_partition_init()`, when the host is not a Hyper-V parent
  partition but the fake knob is enabled, still register the `mshv` misc
  device and initialize only the minimal state needed by the fake path;
* do not call Hyper-V setup helpers such as SynIC setup, VMM capability
  discovery, root scheduler setup, or Hyper-V interrupt handler registration in
  the fake path;
* keep `mshv_dev_open()` as the trivial success path. Opening `/dev/mshv`
  should succeed in QEMU/KVM once the misc device has been registered;
* add a fake `MSHV_CREATE_PARTITION` path that returns an anon-inode VM FD
  without issuing `hv_call_create_partition()`;
* back that FD with a small fake partition object, a synthetic immutable
  partition ID, a refcount, and file operations whose release path frees only
  fake resources;
* make the vIOMMU allocation prototype validate the FD through an MSHV helper
  instead of looking up a partition from `current` or `current->tgid`.

The fake partition FD should model only the lifetime contract needed by
IOMMUFD: `get` when the vIOMMU is allocated, `put` when the vIOMMU is
destroyed, and rejection of non-MSHV FDs. It should not accept real partition
ioctls, create VPs, map guest memory, or issue any Hyper-V hypercalls.

This hook must remain clearly marked as prototype code. It must not use
`current` or process IDs as the VM identity for the direct attach path, and it
must be easy to remove once the real MSHV VM FD plumbing is available.

#### Phase 2: Add direct vIOMMU UAPI plumbing

Add the explicit `IOMMU_VIOMMU_TYPE_DIRECT` UAPI type and the matching
`struct iommu_viommu_direct` VM FD data. Use the IOMMUFD mock driver and
`tools/testing/selftests/iommu` to exercise:

* allocating a direct vIOMMU without a paging parent;
* passing a mock VM FD through the vIOMMU allocation data.

The goal is to exercise the VM-FD-backed vIOMMU allocation while using the same
explicit vIOMMU type that the eventual MSHV root IOMMU driver will support. The
vDEVICE, direct HWPT, and VFIO attach pieces are added by later phases. This
phase also teaches the IOMMUFD vIOMMU core that
`IOMMU_VIOMMU_TYPE_DIRECT` has no nesting-parent `HWPT_PAGING`, so the core
paths that create, destroy, and reference a vIOMMU must tolerate
`viommu->hwpt == NULL` for that type.

#### Phase 3.1: Add direct HWPT UAPI plumbing and allocation selftest

Add the initial direct-HWPT UAPI and IOMMUFD core plumbing without yet
implementing the full mock direct attach behavior. This phase should cover:

* adding `IOMMU_HWPT_DATA_DIRECT` and `struct iommu_hwpt_direct`;
* extending `IOMMU_HWPT_ALLOC` so `pt_id = viommu_id` can select a direct
  HWPT allocation path instead of the existing nested-domain path;
* adding the minimal IOMMUFD object/lifetime plumbing needed for a direct HWPT
  to hold a reference to its parent vIOMMU;
* adding selftest coverage for allocation success and rejection cases, including
  wrong vIOMMU type, unsupported flags, bad data length/type, and vIOMMU
  lifetime protection while a direct HWPT exists.

This phase is only about proving the UAPI/core object shape. The mock direct
domain does not need to implement attach behavior yet, and VFIO attach remains
for later phases.

#### Phase 3.2: Implement a mock direct HWPT

Extend the IOMMUFD selftest mock IOMMU driver with a mock direct domain. The
domain should be attachable, but it must not be IOAS-backed and must not support
map or unmap operations. Device attach should validate that the physical device
has a vDEVICE under the same vIOMMU and should consume the vDEVICE
`virt_id` as the VM-visible device identity.

#### Phase 4: Add the lifecycle selftest

Add a selftest that runs the full object flow:

* create an IOMMUFD context;
* bind a mock VFIO device and get `dev_id`;
* create a mock VM FD;
* allocate an `IOMMU_VIOMMU_TYPE_DIRECT` vIOMMU from that VM FD;
* allocate a vDEVICE with a test `virt_id`;
* allocate a direct HWPT under the vIOMMU;
* attach and detach the VFIO device to the direct HWPT;
* destroy the direct HWPT, vDEVICE, and vIOMMU in the required order.

The test should also cover rejected orderings, such as destroying the vIOMMU
while a direct HWPT or vDEVICE still exists.

#### Phase 5: Run the prototype in the QEMU/KVM environment

Build the IOMMUFD selftests and run the new lifecycle test in the target
QEMU/KVM environment. The expected result is that object creation, attach,
detach, and teardown all succeed with the mock direct HWPT, while invalid
lifetime orderings fail before any object is freed out from under a dependent
object.

## 6. UAPI Additions

direct attach needs a direct vIOMMU type and a new vIOMMU child HWPT
allocation mode for externally managed direct domains. The preferred shape is
to reuse the existing vIOMMU/vDEVICE UAPI namespace and extend the
`IOMMU_HWPT_ALLOC` path where `pt_id` names a vIOMMU. The new child object
is `HWPT_DIRECT`, not `HWPT_NESTED`.

```c
enum iommu_viommu_type {
    ...
    IOMMU_VIOMMU_TYPE_DIRECT,
};

struct iommu_viommu_direct {
    __s32 vm_fd;
    __u32 flags;
    __aligned_u64 __reserved;
};

enum iommu_hwpt_data_type {
    ...
    IOMMU_HWPT_DATA_DIRECT,
};

struct iommu_hwpt_direct {
    __u32 flags;
    __u32 __reserved;
};
```

Open UAPI points:

* IOMMUFD should not standardize or interpret the VM FD type. The FD is
  driver-specific vIOMMU data and may be backed by mshv, KVM, or another VM
  subsystem.
* The UAPI requirement is lifetime alignment: once the FD is associated with a
  vIOMMU, the VM file/object and the vIOMMU object must remain mutually valid
  until the association is explicitly destroyed. Direct HWPT children hold a
  reference to the vIOMMU, so the VM identity remains valid while any attached
  direct domain exists.
* If direct attach needs output data from HWPT allocation, the
  driver-specific data structure can follow the existing vIOMMU pattern where
  driver data contains input and output fields.
* `IOMMU_VIOMMU_ALLOC` normally requires a nesting-parent `HWPT_PAGING`.
  `IOMMU_VIOMMU_TYPE_DIRECT` is the explicit exception for direct
  attach: `hwpt_id` must be zero and the core passes a `NULL` parent domain
  to the driver `viommu_init` callback. Other vIOMMU types keep the existing
  nesting-parent `HWPT_PAGING` requirement.
* A `HWPT_DIRECT` ID must not be accepted in `iommu_viommu_alloc::hwpt_id`.
  `HWPT_DIRECT` is a child of a vIOMMU, not the parent used to create one.
* `IOMMU_HWPT_ALLOC` with `pt_id = viommu_id` currently creates
  `HWPT_NESTED`. The core must distinguish direct-domain data from nested
  translation data and call a direct-domain allocator instead of
  `alloc_domain_nested`.
* The direct attach implementation uses one VM-visible device identifier
  encoding for `iommu_vdevice_alloc::virt_id` across supported MSHV host
  environments: the direct-attach logical device ID. If a baremetal root partition flow
  starts from SBDF, the MSHV driver or VM stack must translate or validate that
  SBDF against the logical device ID before `IOMMU_VDEVICE_ALLOC`.

## 7. Kernel Implementation Plan

### Phase 1: VM FD Binding Helper

* Define a helper, likely in the VM/hypervisor driver, that validates a VM FD
  and returns an immutable VM/partition handle usable by MSHV hypercalls.
* The helper must establish the lifetime coupling between the VM file/object and
  the vIOMMU object. The exact FD type remains opaque to IOMMUFD.
* For mshv, the accepted FD is the partition FD returned by
  `MSHV_CREATE_PARTITION`. The helper should validate the anon-inode file
  type, pin the `struct mshv_partition`/file object, and return the MSHV
  partition identity from that pinned object.
* For Xen, the equivalent helper must not treat a bare `domid_t` as the VM
  identity. It should validate a Xen domain FD or another Xen FD that can pin an
  immutable domain identity for the vIOMMU lifetime. A restricted
  `/dev/xen/privcmd` fd is only usable for this purpose if the Xen side also
  provides lifetime pinning of the underlying domain object.
* Do not accept raw partition IDs from userspace for IOMMUFD direct attach.
* Do not infer ownership from `current`, `tgid`, or process lifetime.

Deliverable: internal kernel API for "FD -> immutable hypervisor VM identity".

### Phase 2: Direct vIOMMU Type

* Add `IOMMU_VIOMMU_TYPE_DIRECT` and `struct iommu_viommu_direct` carrying
  an opaque VM FD.
* Implement `get_viommu_size` and `viommu_init`.
* In `viommu_init`, copy the VM FD data, validate it with the Phase 1 helper,
  and hold the VM reference for the vIOMMU lifetime. The parent-domain argument
  must be `NULL` for `IOMMU_VIOMMU_TYPE_DIRECT`.
* Update IOMMUFD core paths that currently assume `viommu->hwpt` is present:
  direct vIOMMUs must not dereference a missing parent HWPT during destroy,
  internal access attachment, or nested HWPT allocation.
* Implement vIOMMU destroy to release the VM reference.

Deliverable: `IOMMU_VIOMMU_ALLOC` creates a direct vIOMMU bound to a VM FD
and representing that VM's isolated IOMMU slice.

### Phase 3: Direct vDEVICE Support

* Use the existing `IOMMU_VDEVICE_ALLOC` parented by `viommu_id`.
* Treat `iommu_vdevice_alloc::virt_id` as the direct-attach logical device ID.
* Validate logical device ID uniqueness within the vIOMMU, and validate that
  the physical device can be represented by that logical ID, including any
  SBDF-to-logical-ID translation required by baremetal root partition flows.
* Ensure vDEVICE destruction reverses any per-device vIOMMU state before the
  vIOMMU drops the VM identity.

Deliverable: IOMMUFD records the VM-visible identity of each assigned device.

### Phase 4: Direct HWPT Child of vIOMMU

* Add an IOMMUFD HWPT subtype for externally managed direct domains allocated
  under a vIOMMU.
* Add `IOMMU_HWPT_DATA_DIRECT` and `struct iommu_hwpt_direct` for
  direct-domain allocation options. The VM FD is not repeated here; it comes
  from the parent vIOMMU.
* Extend `IOMMU_HWPT_ALLOC` with `pt_id = viommu_id` so direct-domain data
  allocates `HWPT_DIRECT` instead of `HWPT_NESTED`.
* Add a vIOMMU op such as `alloc_domain_direct`. Do not reuse
  `alloc_domain_nested` for the base direct attach domain.
* The core must not call `iopt_table_add_domain()`,
  `iopt_fill_domain()`, or IOAS map/unmap fan-out for this HWPT.
* The direct HWPT must hold a reference to the vIOMMU for its lifetime.
* The domain must not provide `map_pages` or `unmap_pages`. It must provide
  whatever attach/detach operations are required by the IOMMU core to move a
  physical device into and out of the target VM.

Deliverable: `IOMMU_HWPT_ALLOC` with `pt_id = viommu_id` creates an
attachable direct HWPT representing the vIOMMU VM's S2 translation.

### Phase 5: VFIO Attach/Detach Integration

* Use existing `VFIO_DEVICE_ATTACH_IOMMUFD_PT` and
  `VFIO_DEVICE_DETACH_IOMMUFD_PT` to attach/detach the direct HWPT.
* `attach_dev` must verify that the device has a vDEVICE under the direct
  HWPT's vIOMMU and then issue the direct attach hypercall using:

  * the VM identity held by the vIOMMU;
  * the physical device being attached;
  * the logical device ID from the vDEVICE.

* On detach, issue the MSHV detach hypercall and return the device to a safe
  host-owned domain.
* Ensure device destruction, vDEVICE destruction, direct HWPT destruction,
  vIOMMU destruction, and VM FD close orderings cannot leave a device attached
  to a dead VM identity.

Deliverable: VFIO cdev assigned devices can enter and leave Direct
attach via existing VFIO/IOMMUFD ioctls.

### Phase 6: Selftests and Validation

* Extend the IOMMUFD selftest mock vIOMMU to model a VM-FD-backed vIOMMU,
  vDEVICE identity binding, and direct HWPT child.
* Add tests for:

  * vIOMMU allocation rejects invalid VM FDs;
  * direct HWPT allocation requires a compatible direct vIOMMU parent;
  * vDEVICE logical IDs are unique and associated with the correct vIOMMU;
  * no direct attach `HWPT_NESTED` allocation is needed;
  * attaching without a vDEVICE fails;
  * map/unmap on the direct attach path is not possible;
  * destroying objects in different orders preserves VM FD and device
    lifetimes.

* Add MSHV integration tests when the hypervisor test environment is
  available.

Deliverable: selftest coverage for the IOMMUFD object model before MSHV
hardware/hypercall testing is required.

### Phase 7: MSHV Driver Refactor

* Move MSHV IOMMU code under `drivers/iommu/hyperv/` once the required
  IOMMUFD UAPI and core object model are in place.
* Split IRQ remapping from DMA translation/direct attach code.
* Keep domain attach and direct attach as separate domain implementations.

Deliverable: MSHV driver organization ready for the final IOMMUFD
UAPI/object model.

### Phase 8: MSHV Root Driver UAPI Adaptation

* Adapt the MSHV root IOMMU driver to the established IOMMUFD
  vIOMMU/vDEVICE/direct-HWPT UAPI instead of shaping UAPI around the initial
  driver layout.
* Implement MSHV `get_viommu_size` and `viommu_init` for
  `IOMMU_VIOMMU_TYPE_DIRECT`.
* Implement MSHV externally managed domain allocation for direct HWPTs under
  `IOMMU_HWPT_DATA_DIRECT`.
* Implement driver-private state for the direct-attach logical device ID if needed by
  the vDEVICE.
* Wire `attach_dev`/detach to the direct attach/detach hypercalls
  on the direct HWPT, using the VM identity held by the vIOMMU and the logical
  device ID held by the vDEVICE.

Deliverable: the MSHV root IOMMU driver supports direct attach using the
new IOMMUFD vIOMMU/vDEVICE/direct-HWPT UAPI.

## 8. Comparison with the Previous Proposal

The previous design put `partid` and `logical_devid` together in
MSHV-specific allocation data for a custom nested direct domain. That has
two problems:

* A raw partition ID is not a lifetime-safe Linux object. The IOMMU driver must
  hold an FD-backed VM identity instead.
* A logical device ID is per virtual device, not per HWPT. Putting it in HWPT
  allocation data does not work cleanly for multi-device domains.

The vIOMMU plus direct-HWPT design fixes both:

* `IOMMU_VIOMMU_ALLOC` with `IOMMU_VIOMMU_TYPE_DIRECT` carries and pins
  the VM FD for the vIOMMU lifetime without a nesting parent.
* `IOMMU_VDEVICE_ALLOC` carries the logical device ID under that vIOMMU.
* `IOMMU_HWPT_ALLOC` from the vIOMMU produces an explicit direct HWPT, not an
  IOAS-backed paging domain and not `HWPT_NESTED`.

## 9. Open Questions

* Should `IOMMU_HWPT_ALLOC` with `pt_id = viommu_id` dispatch between
  `HWPT_NESTED` and `HWPT_DIRECT` based on `data_type`, or should the
  direct HWPT use a more explicit flag/type?
* Whether any common helper is needed for drivers to pin an opaque VM FD and
  obtain an immutable hypervisor VM identity, without exposing the FD type to
  IOMMUFD core.
* What domain type should represent an externally managed, attachable,
  non-IOAS-backed direct domain in the core IOMMU API?
* What exact detach semantics are required when the VM FD is closed while VFIO
  devices remain attached? The preferred rule is that direct HWPTs and vDEVICEs
  hold references to the vIOMMU, and the vIOMMU holds the VM reference, so VM
  teardown must first destroy or forcibly detach the IOMMUFD objects.

## 10. References

* `Documentation/userspace-api/iommufd.rst` -- IOMMUFD object model,
  including vIOMMU and vDEVICE.
* `include/uapi/linux/iommufd.h` -- IOMMUFD UAPI definitions.
