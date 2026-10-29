// SPDX-License-Identifier: GPL-2.0
#include <kunit/test.h>
#include <linux/pci-tsm.h>
#include <linux/slab.h>
#include <linux/tsm.h>

#include "pci.h"

struct pci_tsm_test_context {
	struct pci_tsm core;
};

static struct pci_dev *pci_tsm_test_match_dev;

static bool pci_tsm_test_match(struct pci_dev *pdev)
{
	return pdev == pci_tsm_test_match_dev;
}

static struct pci_tsm *pci_tsm_test_probe(struct tsm_dev *tsm_dev,
					  struct pci_dev *pdev)
{
	struct pci_tsm_test_context *ctx;

	ctx = kzalloc_obj(*ctx, GFP_KERNEL);
	if (!ctx)
		return NULL;
	if (pci_tsm_assignment_constructor(pdev, &ctx->core, tsm_dev)) {
		kfree(ctx);
		return NULL;
	}
	return &ctx->core;
}

static void pci_tsm_test_remove(struct pci_tsm *tsm)
{
	kfree(container_of(tsm, struct pci_tsm_test_context, core));
}

static struct pci_tsm_ops pci_tsm_test_ops = {
	.assignment_ops = {
		.match = pci_tsm_test_match,
		.probe = pci_tsm_test_probe,
		.remove = pci_tsm_test_remove,
	},
};

static void pci_tsm_assignment_match_test(struct kunit *test)
{
	struct tsm_dev tsm_dev = {
		.pci_ops = &pci_tsm_test_ops,
	};
	struct pci_dev *pdev;

	pdev = kunit_kzalloc(test, sizeof(*pdev), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, pdev);
	pci_tsm_test_match_dev = pdev;

	KUNIT_EXPECT_TRUE(test,
			  pci_tsm_assignment_probe(pdev, &tsm_dev));
	KUNIT_ASSERT_NOT_NULL(test, pdev->tsm);
	KUNIT_EXPECT_PTR_EQ(test, pdev->tsm->tsm_dev, &tsm_dev);

	pci_tsm_destroy(pdev);
	pci_tsm_test_match_dev = NULL;
}

static void pci_tsm_assignment_nonmatch_test(struct kunit *test)
{
	struct tsm_dev tsm_dev = {
		.pci_ops = &pci_tsm_test_ops,
	};
	struct pci_dev *pdev;

	pdev = kunit_kzalloc(test, sizeof(*pdev), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, pdev);
	pci_tsm_test_match_dev = NULL;

	KUNIT_EXPECT_FALSE(test,
			   pci_tsm_assignment_probe(pdev, &tsm_dev));
	KUNIT_EXPECT_PTR_EQ(test, pdev->tsm, NULL);
}

static void pci_tsm_assignment_conflict_test(struct kunit *test)
{
	struct tsm_dev tsm_dev = {
		.pci_ops = &pci_tsm_test_ops,
	};
	struct pci_tsm existing = {};
	struct pci_dev *pdev;

	pdev = kunit_kzalloc(test, sizeof(*pdev), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, pdev);
	pdev->tsm = &existing;
	pci_tsm_test_match_dev = pdev;

	KUNIT_EXPECT_FALSE(test,
			   pci_tsm_assignment_probe(pdev, &tsm_dev));
	KUNIT_EXPECT_PTR_EQ(test, pdev->tsm, &existing);
	pci_tsm_test_match_dev = NULL;
}

static struct kunit_case pci_tsm_test_cases[] = {
	KUNIT_CASE(pci_tsm_assignment_match_test),
	KUNIT_CASE(pci_tsm_assignment_nonmatch_test),
	KUNIT_CASE(pci_tsm_assignment_conflict_test),
	{}
};

static struct kunit_suite pci_tsm_test_suite = {
	.name = "pci_tsm",
	.test_cases = pci_tsm_test_cases,
};

kunit_test_suite(pci_tsm_test_suite);

MODULE_LICENSE("GPL");
