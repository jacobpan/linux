# IOMMUFD VM-File Assignment Upstream Series Design

## Goal

Rewrite only the commits after `e040f1610961` into a reviewer-oriented
upstream series. Preserve all earlier commits exactly. Keep production
infrastructure and MSHV support separate from KVM-hosted test shims, and mark
every commit containing those shims with a `TEST_ONLY:` subject prefix.

## Constraints

- Preserve every commit through `e040f1610961` without modification.
- Rewrite the three later commits while preserving their final behavior.
- Keep every commit message line shorter than 80 characters.
- Do not add `Signed-off-by` trailers.
- Add the configured Copilot `Co-authored-by` trailer to rewritten commits.
- Leave unrelated untracked files untouched.
- Keep test-only commits at the end of the series.
- Include the pending VFIO bridge comparison in the documentation commit.

## Alternatives

### Reviewer-oriented split

Separate the UAPI, provider discovery, KVM integration, IOMMUFD isolation,
vDEVICE lifecycle, MSHV integration, selftests, documentation, and test shims.
This produces more commits, but each commit has one reviewable purpose and a
clear subsystem owner.

### Subsystem-oriented split

Use one large IOMMUFD commit, one KVM/MSHV commit, one test commit, one
documentation commit, and test-only commits. This reduces patch count but
retains large commits spanning unrelated abstractions.

### Minimal split

Separate only production code, tests, documentation, and test-only code. This
is easiest to construct but does not provide suitable upstream review
boundaries.

The reviewer-oriented split is selected.

## Commit Sequence

1. `iommufd: Add hypervisor-backed vIOMMU UAPI`

   Add `IOMMU_VIOMMU_TYPE_HYPERVISOR` and
   `struct iommu_viommu_hypervisor`. Add only the generic parsing and
   validation needed to accept the VM file argument.

2. `iommufd: Add VM-file vIOMMU providers`

   Add the typed provider registry, provider lookup semantics, module
   ownership, and VM-file lifetime management. Preserve TSM-first selection;
   VM-file providers are consulted only for the hypervisor vIOMMU type.

3. `KVM: Register a VM-file vIOMMU provider`

   Add KVM VM-file recognition, the architecture callback, registration during
   KVM initialization, and unregister ordering during KVM shutdown.

4. `iommufd: Add external device isolation transactions`

   Add the singleton-group and PASID checks, transition the RID attachment to
   the blocking domain, exclude concurrent attach/replace operations, and
   provide commit, rollback, and quarantine operations.

5. `iommufd: Add reclaimable external vDEVICE assignment`

   Add vDEVICE assignment states, provider reclaim callbacks, retryable
   explicit destruction, forced teardown handling, copyout-failure handling,
   and integration with the external isolation transaction.

6. `mshv: Add partition-file helpers for device assignment`

   Add production helpers that validate an MSHV partition file, return its
   partition ID, and prevent movable memory regions once device assignment
   starts. Add production initialization and cleanup hooks required by the
   provider.

7. `mshv: Add IOMMUFD device assignment provider`

   Add MSHV VM-file provider registration and vIOMMU/vDEVICE operations. Move
   attach and detach hypercalls into the MSHV provider and use the vDEVICE
   logical ID supplied by userspace.

8. `selftests/iommu: Cover VM-file backed assignment`

   Add the generic mock VM-file provider and focused coverage for lookup,
   external isolation, rollback, reclaim, and teardown. Do not include fake
   MSHV partition support or standalone MSHV examples.

9. `docs: Describe MSHV IOMMUFD device assignment`

   Document the UAPI flow, isolation transaction, reclaim semantics, provider
   lifetime, limitations, and the relationship to the KVM and MSHV VFIO bridge
   devices.

10. `TEST_ONLY: mshv: Add fake partition files for assignment tests`

    Add the fake `/dev/mshv` mode and fake partition file descriptors used on a
    KVM host. Keep all fake-file branches out of the production MSHV helper
    commit.

11. `TEST_ONLY: iommu/hyperv: Emulate root-IOMMU calls under KVM`

    Add KVM-hosted Hyper-V root-IOMMU detection and successful fake hypercall
    responses required to exercise blocking-domain transitions. Keep any
    supporting internal structure movement in this test-only commit.

12. `TEST_ONLY: selftests: Add MSHV direct assignment examples`

    Add the fake-MSHV open test and direct assignment example together with
    their selftest build integration.

## Excluded Changes

Drop unrelated changes from the post-`e040f1610961` series:

- unused Intel IOMMU include cleanup;
- the unused exported `hv_iommu_root_managed()` interface;
- Hyper-V root-IOMMU structure extraction unless required solely by the
  test-only emulation patch.

These changes do not implement VM-file provider assignment and would distract
from upstream review.

## Rewrite Method

Save the current tracked documentation change, create a safety reference for
the existing tip, reset the branch to `e040f1610961`, and reconstruct each
commit from the existing combined diff. Stage changes by file and hunk so
production and test-only code in `mshv_root_main.c`,
`mshv_iommufd.c`, and `hv-iommu-root.c` remain separated.

Do not modify or delete unrelated untracked files.

## Validation

After constructing each logical group, verify that the tree compiles at the
smallest relevant scope. Before completion:

- compare the rewritten final tree with the intended original tree, accounting
  only for explicitly excluded changes;
- build the focused IOMMUFD, KVM, MSHV, and Hyper-V objects;
- build the IOMMU selftests and standalone examples;
- run `scripts/checkpatch.pl` over the rewritten range;
- run `git diff --check`;
- verify every subject and body line is shorter than 80 characters;
- verify all commits after the documentation patch are prefixed
  `TEST_ONLY:`;
- rerun the existing QEMU IOMMUFD vIOMMU fixture if the environment remains
  available.

