# Hyper-V MSHV external attach ATS/PASID protocol

## Problem

The ATS/PASID protocol differs between the conventional KVM/QEMU model and
the Hyper-V/MSHV/OpenVMM model because the physical IOMMU ownership is
different.

In the KVM/QEMU model, the host attach and guest-visible attach decisions come
together in Linux. QEMU decides what the guest sees, but Linux owns both the
assigned physical device and the physical IOMMU programming. When userspace
asks IOMMUFD to attach an IOAS, nesting parent, or nested translation, the
Linux IOMMU driver can program the physical IOMMU and make the ATS/PASID
enablement decision at the same boundary.

In the Hyper-V/MSHV/OpenVMM model, those two decisions are separated. Linux
owns the physical device and exposes it through VFIO/IOMMUFD, but Hyper-V owns
the physical IOMMU programming, the VM/partition S2 I/O page table, and the
guest-visible vIOMMU/vSMMU emulation. OpenVMM controls whether the guest sees
ATS/PASID capabilities, while the Hyper-V pIOMMU root driver only executes the
later host external attach request through a hypercall.

That structural and temporal split is the root problem for ATS/PASID. Enabling
PASID or ATS in the root pIOMMU driver cannot be done blindly; it must match
what OpenVMM exposed to the guest and what Hyper-V created for the VM. If guest
ATS/PASID is possible, the guest-visible vIOMMU/vSMMU must exist first so
Hyper-V has the VM/device translation and command-queue invalidation context
before Linux asks Hyper-V to attach the physical device.

ATS/PASID therefore needs an explicit protocol among:

```text
guest VM
user-space VMM
/dev/mshv
Hyper-V
Hyper-V pIOMMU root driver
VFIO/IOMMUFD
```

IOMMUFD core must stay policy-neutral. It should not directly enable ATS or
PASID. Physical enablement belongs to the Hyper-V pIOMMU root driver and
Hyper-V integration.

## Baseline decision

Use a fixed-at-attach model.

A device assignment is created in one of two modes:

```text
1. S2-only external attach, no guest vIOMMU, no guest-visible PASID
2. guest-vIOMMU-capable external attach, nesting possible, PASID possible
```

The assignment mode and allowed capabilities are fixed before external attach.
The protocol does not support live transition from:

```text
S2-only external attach -> guest vIOMMU / nested translation
```

for the same active assignment. If the VMM wants to change that mode, it must
detach/reassign the device.

## Design goals

1. Have a clear ATS/PASID policy in the assignment contract, where VM
   vIOMMU presence and guest PCI config capability emulation are known.
2. Let user-space VMM decide whether the VM has a guest vIOMMU and coordinate
   device attachment explicitly through IOMMUFD.
3. Allow a VM without vIOMMU to use external attach safely.
4. Allow host physical ATS without guest awareness when ATC invalidation is
   handled transparently by Hyper-V.
5. Respect PCI ordering: enable PASID before ATS; disable ATS before PASID.

## Roles

| Component                       | Responsibility |
| ------------------------------- | -------------- |
| Guest VM                        | May or may not see a vIOMMU or virtual PCI ATS/PASID caps. |
| User-space VMM                  | Creates guest vIOMMU/vSMMU; traps PCI config; chooses attach policy. |
| `/dev/mshv`                     | Owns VM fd and Hyper-V objects; records per-assignment policy. |
| IOMMUFD core                    | Creates Linux-side vIOMMU/HWPT objects; does not enable ATS/PASID. |
| Hyper-V pIOMMU root driver      | Converts IOMMUFD external attach into a Hyper-V attach-device operation. |
| Hyper-V                         | Owns VM S2/nested translation state and guest vIOMMU invalidation routing. |
| VFIO PCI / VMM config emulation | Exposes or masks guest-visible ATS/PASID caps according to fixed policy. |

## Assignment modes

|                 | w/o guest vIOMMU  | w/ HV-emulated guest vIOMMU                         |
| --------------- | ----------------- | --------------------------------------------------- |
| Guest ATS cap   | Hidden; no enable | Expose if policy allows and ATC invalidation works  |
| Guest PASID cap | Hidden; no enable | Expose if policy allows; PASID before ATS           |
| HW PASID state  | Optional PASID 0  | Enable only if policy allows                        |
| HW ATS state    | Optional          | Optional; enabled based on guest use                |

## External attach flow

```text
1. VMM opens /dev/mshv and creates VM/partition fd.

2. VMM decides assignment mode:
     a. no guest vIOMMU
     b. guest vIOMMU present

3. If guest vIOMMU mode:
     VMM creates the guest-visible vIOMMU/vSMMU through /dev/mshv/Hyper-V.
     This must precede direct assignment/external attach.

4. VMM creates the Linux-side IOMMUFD hypervisor vIOMMU object using the
   partition fd:
     no guest vIOMMU:
       struct iommu_viommu_hypervisor.flags = 0
     guest vIOMMU present:
       flags = IOMMU_VIOMMU_HYPERVISOR_GUEST_VIOMMU

   This references the VM/partition for IOMMUFD external attach; it does not
   create the guest-visible vIOMMU/vSMMU. The flag records the VMM's intended
   VM-wide mode in the Linux-side vIOMMU object. Hyper-V already knows whether
   the guest-visible vIOMMU/vSMMU exists because it was created by hypercall.

   OPEN: Guest-vIOMMU awareness may ultimately need to be per assignment rather
   than per VM. For simplicity, this proposal records it in the hypervisor
   vIOMMU object, so all devices attached through that object share the same
   guest-vIOMMU-aware mode. Mixing guest-vIOMMU-aware and no-guest-vIOMMU
   assignments in the same VM would require a per-device flag or separate
   assignment policy.

5. VMM creates vDEVICE identity for assigned PCI device.

6. VMM configures fixed ATS/PASID policy for this assignment, keyed by the
   partition and vDEVICE/logical ID.

7. VMM binds VFIO device to IOMMUFD.

8. VMM creates IOMMUFD external HWPT from that vIOMMU.

9. VMM attaches VFIO device to external HWPT.

10. Hyper-V pIOMMU root driver:
     validates partition fd
     gets partition id
     gets vDEVICE/logical id
     checks the vIOMMU object's fixed guest-vIOMMU mode
     validates physical device/IOMMU/Hyper-V support
     enables physical PASID/ATS only if allowed by fixed assignment policy
     issues Hyper-V attach-device hypercall
```

## Recording guest-vIOMMU mode

A flag in `struct iommu_viommu_hypervisor.flags` can be useful if guest vIOMMU
presence is a VM-wide property known when the Linux-side IOMMUFD hypervisor
vIOMMU object is created. For example:

```text
IOMMU_VIOMMU_HYPERVISOR_GUEST_VIOMMU
```

would mean "this VM partition has a guest-visible Hyper-V vIOMMU/vSMMU already
created, and direct assignments under this object may use guest-vIOMMU
semantics."

That flag records the IOMMUFD-side mode for later external attach decisions.
It is not the source of truth for Hyper-V object existence: Hyper-V already
knows that from the guest-visible vIOMMU/vSMMU create hypercall. The Hyper-V
pIOMMU root driver still needs per-assignment policy, keyed by partition and
vDEVICE/logical device ID, for guest ATS/PASID exposure, physical PASID use,
physical ATS use, and ATC invalidation ownership.

## Guest PCI config trap behavior

The VMM emulates guest PCI config space and traps writes.

For no-vIOMMU assignment:

```text
ATS cap: hidden or read-only disabled
PASID cap: hidden or read-only disabled
PRI cap: hidden or read-only disabled
guest write to enable ATS/PASID: rejected or ignored as disabled
guest writes do not change physical state
```

For vIOMMU assignment:

```text
VMM exposes only capabilities allowed by fixed policy.
Guest writes update virtual config state.
Guest writes must not cause a new physical enable after attach.
If guest tries to enable a capability not in fixed policy, reject/mask it.
```

Important distinction:

```text
guest-visible enable != Linux IOMMUFD core action
```

The physical enable path remains in the Hyper-V pIOMMU root driver, not
IOMMUFD core.

## Physical enable ordering

Attach-time ordering:

```text
if PASID_ALLOWED:
    pci_enable_pasid()

if GUEST_ATS_ALLOWED:
    guest ATC invalidations arrive through the vIOMMU command queue
    pci_enable_ats()

if HOST_RID_ATS_ALLOWED:
    Hyper-V owns transparent ATC invalidation
    pci_enable_ats()
```

Teardown ordering:

```text
if ATS enabled:
    quiesce/block as needed
    invalidate ATC as needed
    pci_disable_ats()

if PASID enabled:
    pci_disable_pasid()

detach device from Hyper-V partition
```

Do not change PASID enable while ATS is enabled.

## Error handling

Fail external attach if requested policy cannot be honored.

Examples:

```text
GUEST_ATS_ALLOWED but device lacks ATS -> -EOPNOTSUPP
GUEST_ATS_ALLOWED but no guest vIOMMU command queue path exists -> -EOPNOTSUPP
PASID_ALLOWED but device/IOMMU lacks PASID -> -EOPNOTSUPP
policy requests guest ATS without required vIOMMU command queue path -> -EINVAL
guest config write exceeds fixed policy -> virtual failure/masked write
```

Do not silently downgrade:

```text
requested GUEST_ATS_ALLOWED -> attach with guest ATS disabled
```

unless userspace explicitly requested best-effort behavior, which should not
be the default.

## Why fixed-at-attach

The fixed part is the assignment contract, not necessarily the instantaneous
hardware bit state. At attach time, the VMM/Hyper-V/Linux path fixes:

```text
guest vIOMMU presence
guest-visible ATS/PASID capability exposure
whether PASID-backed DMA is allowed
whether physical ATS may be used, and who owns ATC invalidation
the Hyper-V translation and invalidation path for this assignment
```

Later guest config writes can consume that contract, but cannot expand it.
For example, a guest write cannot turn a no-vIOMMU assignment into a
vIOMMU/nested assignment, and cannot add PASID/ATS exposure that was not
allowed when the device was attached.

This avoids the hard transition:

```text
S2-only/no-guest-vIOMMU external attach with fixed guest-visible RID contract
  (hardware may still use PASID 0 for that traffic)
  -> later guest vIOMMU/nested assignment with guest ATS/PASID exposure
```

A dynamic transition would require:

```text
quiesce DMA
disable or invalidate ATC
change Hyper-V translation mode
establish guest invalidation routing
enable/re-enable ATS
resume DMA
```

That is possible as a later extension, but should not be required for the
initial protocol.

## Tests

Minimum coverage:

```text
1. no-vIOMMU external attach:
     guest ATS/PASID exposure off
     physical PASID off or fixed to Hyper-V-managed PASID 0
     physical ATS remains Hyper-V-managed and is not guest-driven

2. vIOMMU external attach, PASID off, ATS off:
     attach succeeds
     guest config cannot enable ATS/PASID

3. vIOMMU external attach, PASID allowed:
     physical PASID enable requested before attach completion

4. vIOMMU external attach, PASID + ATS allowed:
     PASID enabled before ATS
     guest ATC invalidation command reaches Hyper-V through vIOMMU command queue

5. ATS requested without vIOMMU command queue path:
     attach fails

6. guest config write exceeds policy:
     virtual write rejected/masked
     physical state unchanged

7. teardown:
     ATS disabled before PASID
```

## Summary

1. Guest-vIOMMU-aware direct/external attach: userspace marks the Linux-side
   hypervisor vIOMMU with `IOMMU_VIOMMU_HYPERVISOR_GUEST_VIOMMU`; Hyper-V
   already knows the real guest-visible vIOMMU/vSMMU state from the create
   hypercall; per-assignment ATS/PASID policy is keyed by partition plus
   vDEVICE/logical ID.
2. Ordering requirement: the guest-visible vIOMMU/vSMMU must be created before
   direct/external attach when the assignment may expose guest ATS/PASID.
