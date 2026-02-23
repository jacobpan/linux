// SPDX-License-Identifier: GPL-2.0
/*
 * VFIO iommufd No-IOMMU Mode Selftest
 *
 * Tests VFIO cdev access with iommufd in noiommu mode.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <libvfio.h>
#include "kselftest_harness.h"

#define NR_PAGES 32

static const char *device_bdf;

FIXTURE(vfio_noiommu) {
	struct iommu *iommu;
	struct vfio_pci_device *device;
};

FIXTURE_SETUP(vfio_noiommu)
{
	self->iommu = iommu_init(MODE_IOMMUFD);
	self->device = vfio_pci_device_init(device_bdf, self->iommu);
}

FIXTURE_TEARDOWN(vfio_noiommu)
{
	if (self->device)
		vfio_pci_device_cleanup(self->device);
	if (self->iommu)
		iommu_cleanup(self->iommu);
}

static int map_region(struct iommu *iommu, struct dma_region *region,
		      iova_t iova, size_t length)
{
	int ret;

	region->vaddr = mmap(NULL, length, PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (region->vaddr == MAP_FAILED)
		return -errno;

	region->iova = iova;
	region->size = length;

	ret = __iommu_map(iommu, region);
	if (ret) {
		munmap(region->vaddr, region->size);
		region->vaddr = NULL;
	}
	return ret;
}

static int unmap_region(struct iommu *iommu, struct dma_region *region)
{
	int ret;

	if (!region->vaddr)
		return 0;

	ret = __iommu_unmap(iommu, region, NULL);
	if (ret)
		return ret;
	ret = munmap(region->vaddr, region->size);
	if (ret)
		return -errno;
	region->vaddr = NULL;
	return 0;
}

TEST_F(vfio_noiommu, device_get_info)
{
	ASSERT_NE(0, self->device->info.argsz);
}

TEST_F(vfio_noiommu, device_reset)
{
	if (!(self->device->info.flags & VFIO_DEVICE_FLAGS_RESET))
		SKIP(return, "Device does not support reset\n");

	vfio_pci_device_reset(self->device);
}

TEST_F(vfio_noiommu, ioas_map_unmap)
{
	struct dma_region region = {};
	long page_size = sysconf(_SC_PAGESIZE);
	int ret;

	ASSERT_GT(page_size, 0);

	ret = map_region(self->iommu, &region, 0x10000, page_size);
	if (ret)
		SKIP(return, "IOMMU_IOAS_MAP failed: %s\n", strerror(-ret));

	ASSERT_EQ(0, unmap_region(self->iommu, &region));
}

TEST_F(vfio_noiommu, ioas_noiommu_get_pa_mapped)
{
	struct dma_region region = {};
	long page_size = sysconf(_SC_PAGESIZE);
	u64 phys = 0;
	u64 length = 0;
	int ret;

	ASSERT_GT(page_size, 0);

	ret = map_region(self->iommu, &region, 0x200000,
			 page_size * NR_PAGES);
	if (ret)
		SKIP(return, "IOMMU_IOAS_MAP failed: %s\n", strerror(-ret));

	ret = __iommu_noiommu_get_pa(self->iommu, region.iova, 0, &phys,
				     &length);
	ASSERT_EQ(0, ret);
	ASSERT_NE(0, phys);
	ASSERT_LE(length, region.size);

	phys = 0;
	length = 0;
	ret = __iommu_noiommu_get_pa(self->iommu, region.iova + 0x80, 0,
				     &phys, &length);
	ASSERT_EQ(0, ret);
	ASSERT_NE(0, phys);
	ASSERT_LE(length, region.size - 0x80);
	ASSERT_EQ(0, (phys + length) % page_size);

	ASSERT_EQ(0, unmap_region(self->iommu, &region));
}

TEST_F(vfio_noiommu, ioas_noiommu_get_pa_unmapped_fails)
{
	ASSERT_NE(0, __iommu_noiommu_get_pa(self->iommu, 0x10000, 0, NULL,
					    NULL));
}

TEST_F(vfio_noiommu, ioas_noiommu_get_pa_length_zero_no_limit)
{
	struct dma_region region = {};
	long page_size = sysconf(_SC_PAGESIZE);
	u64 phys_nolimit = 0, phys_zero = 0;
	u64 len_nolimit = 0, len_zero = 0;
	int ret;

	ASSERT_GT(page_size, 0);

	ret = map_region(self->iommu, &region, 0x200000,
			 page_size * NR_PAGES);
	if (ret)
		SKIP(return, "IOMMU_IOAS_MAP failed: %s\n", strerror(-ret));

	ret = __iommu_noiommu_get_pa(self->iommu, region.iova, 0, &phys_zero,
				     &len_zero);
	ASSERT_EQ(0, ret);

	ret = __iommu_noiommu_get_pa(self->iommu, region.iova, 0,
				     &phys_nolimit, &len_nolimit);
	ASSERT_EQ(0, ret);
	ASSERT_EQ(phys_zero, phys_nolimit);
	ASSERT_EQ(len_zero, len_nolimit);

	ASSERT_EQ(0, unmap_region(self->iommu, &region));
}

TEST_F(vfio_noiommu, ioas_noiommu_get_pa_length_capped)
{
	struct dma_region region = {};
	long page_size = sysconf(_SC_PAGESIZE);
	u64 phys = 0;
	u64 len_full = 0, len_capped = 0;
	u64 cap;
	int ret;

	ASSERT_GT(page_size, 0);

	ret = map_region(self->iommu, &region, 0x200000,
			 page_size * NR_PAGES);
	if (ret)
		SKIP(return, "IOMMU_IOAS_MAP failed: %s\n", strerror(-ret));

	ret = __iommu_noiommu_get_pa(self->iommu, region.iova, 0, &phys,
				     &len_full);
	ASSERT_EQ(0, ret);
	ASSERT_NE(0, phys);
	ASSERT_NE(0, len_full);

	cap = page_size / 2;
	ret = __iommu_noiommu_get_pa(self->iommu, region.iova, cap, &phys,
				     &len_capped);
	ASSERT_EQ(0, ret);
	ASSERT_LE(len_capped, cap);
	ASSERT_NE(0, len_capped);

	if (len_full > cap)
		ASSERT_GT(len_full, len_capped);

	ret = __iommu_noiommu_get_pa(self->iommu, region.iova, UINT64_MAX,
				     &phys, &len_capped);
	ASSERT_EQ(0, ret);
	ASSERT_EQ(len_full, len_capped);

	ASSERT_EQ(0, unmap_region(self->iommu, &region));
}

int main(int argc, char *argv[])
{
	device_bdf = vfio_selftests_get_bdf(&argc, argv);

	if (!vfio_pci_noiommu_mode_enabled()) {
		ksft_print_msg("VFIO unsafe noiommu mode is not enabled\n");
		return KSFT_SKIP;
	}

	return test_harness_run(argc, argv);
}
