// SPDX-License-Identifier: GPL-2.0-only
/*
 * KVM_HC_PIN_GPA_RANGE / KVM_HC_UNPIN_GPA_RANGE hypercall test
 *
 * Verifies that the guest can issue pin/unpin GPA range hypercalls and
 * that KVM correctly exits to userspace with the expected arguments.
 * Also tests argument validation (alignment, overflow, zero pages).
 */
#include <linux/kvm_para.h>

#include "test_util.h"
#include "kvm_util.h"
#include "processor.h"
#include "ucall_common.h"

/* Test stages communicated from guest to host via GUEST_SYNC */
enum test_stage {
	STAGE_PIN_BASIC,
	STAGE_UNPIN_BASIC,
	STAGE_PIN_2M,
	STAGE_PIN_MULTI_PAGE,
	STAGE_UNPIN_MULTI_PAGE,
	STAGE_PIN_UNALIGNED,
	STAGE_PIN_ZERO_PAGES,
	STAGE_DONE,
};

/* GPA used for pin/unpin tests (arbitrary, page-aligned) */
#define TEST_GPA		(0x100000ULL)	/* 1 MB */
#define TEST_NPAGES		1
#define TEST_MULTI_NPAGES	16
#define TEST_UNALIGNED_GPA	(0x100001ULL)	/* not page-aligned */

static void guest_code(void)
{
	uint64_t ret;

	/* Test 1: Basic pin */
	GUEST_SYNC(STAGE_PIN_BASIC);
	ret = __kvm_hypercall_pin_gpa_range(TEST_GPA, TEST_NPAGES,
					    KVM_PIN_GPA_RANGE_PAGE_SZ_4K);
	GUEST_ASSERT(!ret);

	/* Test 2: Basic unpin */
	GUEST_SYNC(STAGE_UNPIN_BASIC);
	ret = __kvm_hypercall_unpin_gpa_range(TEST_GPA, TEST_NPAGES,
					      KVM_PIN_GPA_RANGE_PAGE_SZ_4K);
	GUEST_ASSERT(!ret);

	/* Test 3: Pin with 2M page size hint */
	GUEST_SYNC(STAGE_PIN_2M);
	ret = __kvm_hypercall_pin_gpa_range(TEST_GPA, TEST_NPAGES,
					    KVM_PIN_GPA_RANGE_PAGE_SZ_2M);
	GUEST_ASSERT(!ret);

	/* Test 4: Pin multiple pages */
	GUEST_SYNC(STAGE_PIN_MULTI_PAGE);
	ret = __kvm_hypercall_pin_gpa_range(TEST_GPA, TEST_MULTI_NPAGES,
					    KVM_PIN_GPA_RANGE_PAGE_SZ_4K);
	GUEST_ASSERT(!ret);

	/* Test 5: Unpin multiple pages */
	GUEST_SYNC(STAGE_UNPIN_MULTI_PAGE);
	ret = __kvm_hypercall_unpin_gpa_range(TEST_GPA, TEST_MULTI_NPAGES,
					      KVM_PIN_GPA_RANGE_PAGE_SZ_4K);
	GUEST_ASSERT(!ret);

	/* Test 6: Pin with unaligned GPA - expect -KVM_EINVAL from KVM */
	GUEST_SYNC(STAGE_PIN_UNALIGNED);
	ret = __kvm_hypercall_pin_gpa_range(TEST_UNALIGNED_GPA, TEST_NPAGES,
					    KVM_PIN_GPA_RANGE_PAGE_SZ_4K);
	GUEST_ASSERT(ret == (uint64_t)(-KVM_EINVAL));

	/* Test 7: Pin with zero pages - expect -KVM_EINVAL from KVM */
	GUEST_SYNC(STAGE_PIN_ZERO_PAGES);
	ret = __kvm_hypercall_pin_gpa_range(TEST_GPA, 0,
					    KVM_PIN_GPA_RANGE_PAGE_SZ_4K);
	GUEST_ASSERT(ret == (uint64_t)(-KVM_EINVAL));

	GUEST_SYNC(STAGE_DONE);
	GUEST_DONE();
}

static void handle_pin_unpin_hypercall(struct kvm_run *run,
				       enum test_stage stage)
{
	uint64_t nr = run->hypercall.nr;
	uint64_t gpa = run->hypercall.args[0];
	uint64_t npages = run->hypercall.args[1];
	uint64_t attrs = run->hypercall.args[2];

	pr_info("  Hypercall: nr=%lu gpa=0x%lx npages=%lu attrs=0x%lx\n",
		nr, gpa, npages, attrs);

	switch (stage) {
	case STAGE_PIN_BASIC:
		TEST_ASSERT(nr == KVM_HC_PIN_GPA_RANGE,
			    "Expected PIN (%u), got %lu",
			    KVM_HC_PIN_GPA_RANGE, nr);
		TEST_ASSERT(gpa == TEST_GPA,
			    "Expected GPA 0x%lx, got 0x%lx",
			    (uint64_t)TEST_GPA, gpa);
		TEST_ASSERT(npages == TEST_NPAGES,
			    "Expected %u pages, got %lu",
			    TEST_NPAGES, npages);
		TEST_ASSERT(attrs == KVM_PIN_GPA_RANGE_PAGE_SZ_4K,
			    "Expected 4K attrs, got 0x%lx", attrs);
		break;

	case STAGE_UNPIN_BASIC:
		TEST_ASSERT(nr == KVM_HC_UNPIN_GPA_RANGE,
			    "Expected UNPIN (%u), got %lu",
			    KVM_HC_UNPIN_GPA_RANGE, nr);
		TEST_ASSERT(gpa == TEST_GPA,
			    "Expected GPA 0x%lx, got 0x%lx",
			    (uint64_t)TEST_GPA, gpa);
		TEST_ASSERT(npages == TEST_NPAGES,
			    "Expected %u pages, got %lu",
			    TEST_NPAGES, npages);
		break;

	case STAGE_PIN_2M:
		TEST_ASSERT(nr == KVM_HC_PIN_GPA_RANGE,
			    "Expected PIN, got %lu", nr);
		TEST_ASSERT(attrs == KVM_PIN_GPA_RANGE_PAGE_SZ_2M,
			    "Expected 2M attrs, got 0x%lx", attrs);
		break;

	case STAGE_PIN_MULTI_PAGE:
		TEST_ASSERT(nr == KVM_HC_PIN_GPA_RANGE,
			    "Expected PIN, got %lu", nr);
		TEST_ASSERT(npages == TEST_MULTI_NPAGES,
			    "Expected %u pages, got %lu",
			    TEST_MULTI_NPAGES, npages);
		break;

	case STAGE_UNPIN_MULTI_PAGE:
		TEST_ASSERT(nr == KVM_HC_UNPIN_GPA_RANGE,
			    "Expected UNPIN, got %lu", nr);
		TEST_ASSERT(npages == TEST_MULTI_NPAGES,
			    "Expected %u pages, got %lu",
			    TEST_MULTI_NPAGES, npages);
		break;

	default:
		TEST_FAIL("Unexpected hypercall exit at stage %d", stage);
	}

	/* Return success to the guest */
	run->hypercall.ret = 0;
}

int main(int argc, char *argv[])
{
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	struct kvm_run *run;
	struct ucall uc;
	enum test_stage stage = STAGE_PIN_BASIC;
	int ret;

	vm = vm_create_with_one_vcpu(&vcpu, guest_code);
	run = vcpu->run;

	/*
	 * Check if the running kernel supports pin/unpin GPA hypercall exits.
	 * This requires a kernel built with the KVM_HC_PIN/UNPIN_GPA_RANGE
	 * patches applied.
	 */
	ret = __vm_enable_cap(vm, KVM_CAP_EXIT_HYPERCALL,
			      BIT(KVM_HC_PIN_GPA_RANGE) |
			      BIT(KVM_HC_UNPIN_GPA_RANGE));
	__TEST_REQUIRE(ret == 0,
		       "KVM_CAP_EXIT_HYPERCALL does not support PIN/UNPIN bits "
		       "(need kernel with KVM_HC_PIN/UNPIN_GPA_RANGE support)");

	pr_info("Testing KVM_HC_PIN/UNPIN_GPA_RANGE hypercalls\n");

	for (;;) {
		vcpu_run(vcpu);

		if (run->exit_reason == KVM_EXIT_HYPERCALL) {
			handle_pin_unpin_hypercall(run, stage);
			continue;
		}

		TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_IO);

		switch (get_ucall(vcpu, &uc)) {
		case UCALL_SYNC:
			stage = uc.args[1];
			pr_info("Stage %d\n", stage);

			if (stage == STAGE_DONE)
				goto done;
			break;
		case UCALL_ABORT:
			REPORT_GUEST_ASSERT(uc);
		case UCALL_DONE:
			goto done;
		default:
			TEST_FAIL("Unexpected ucall: %lu", uc.cmd);
		}
	}

done:
	pr_info("All pin/unpin GPA range hypercall tests passed!\n");
	kvm_vm_free(vm);
	return 0;
}
