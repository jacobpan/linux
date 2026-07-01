// SPDX-License-Identifier: GPL-2.0-only
/*
 * Standalone VMM-oriented example for direct attach via IOMMUFD.
 *
 * Build from the kernel tree with:
 *   make -C tools/testing/selftests/iommu iommufd_direct_example
 *
 * Run as:
 *   ./iommufd_direct_example /dev/vfio/devices/vfioN <logical-device-id>
 *
 * The logical device ID is the VM-visible device identifier that the type-1
 * hypervisor expects when the IOMMU driver performs direct attach.
 */
#define __EXPORTED_HEADERS__
#include <linux/iommufd.h>
#include <linux/mshv.h>
#include <linux/vfio.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int parse_u64(const char *str, uint64_t *value)
{
	char *end;
	unsigned long long tmp;

	errno = 0;
	tmp = strtoull(str, &end, 0);
	if (errno || end == str || *end)
		return -EINVAL;
	*value = tmp;
	return 0;
}

static int do_ioctl(int fd, unsigned long request, void *arg, const char *name)
{
	if (ioctl(fd, request, arg)) {
		fprintf(stderr, "%s failed: %s\n", name, strerror(errno));
		return -errno;
	}
	return 0;
}

static int iommufd_destroy(int iommufd, uint32_t id)
{
	struct iommu_destroy destroy = {
		.size = sizeof(destroy),
		.id = id,
	};

	if (!id)
		return 0;
	return do_ioctl(iommufd, IOMMU_DESTROY, &destroy, "IOMMU_DESTROY");
}

static int bind_vfio_device(int vfio_fd, int iommufd, uint32_t *dev_id)
{
	struct vfio_device_bind_iommufd bind = {
		.argsz = sizeof(bind),
		.iommufd = iommufd,
	};
	int rc;

	rc = do_ioctl(vfio_fd, VFIO_DEVICE_BIND_IOMMUFD, &bind,
		      "VFIO_DEVICE_BIND_IOMMUFD");
	if (rc)
		return rc;
	*dev_id = bind.out_devid;
	return 0;
}

static int create_mshv_partition(int *mshv_fd, int *vm_fd)
{
	struct mshv_create_partition partition = {};

	*mshv_fd = open("/dev/mshv", O_RDWR | O_CLOEXEC);
	if (*mshv_fd < 0) {
		fprintf(stderr, "open /dev/mshv failed: %s\n", strerror(errno));
		return -errno;
	}

	*vm_fd = ioctl(*mshv_fd, MSHV_CREATE_PARTITION, &partition);
	if (*vm_fd < 0) {
		fprintf(stderr, "MSHV_CREATE_PARTITION failed: %s\n",
			strerror(errno));
		return -errno;
	}
	return 0;
}

static int alloc_direct_viommu(int iommufd, uint32_t dev_id, int vm_fd,
			     uint32_t *viommu_id)
{
	struct iommu_viommu_direct direct = {
		.vm_fd = vm_fd,
	};
	struct iommu_viommu_alloc alloc = {
		.size = sizeof(alloc),
		.type = IOMMU_VIOMMU_TYPE_DIRECT,
		.dev_id = dev_id,
		.data_len = sizeof(direct),
		.data_uptr = (uintptr_t)&direct,
	};
	int rc;

	rc = do_ioctl(iommufd, IOMMU_VIOMMU_ALLOC, &alloc,
		      "IOMMU_VIOMMU_ALLOC");
	if (rc)
		return rc;
	*viommu_id = alloc.out_viommu_id;
	return 0;
}

static int alloc_vdevice(int iommufd, uint32_t viommu_id, uint32_t dev_id,
			 uint64_t logical_device_id, uint32_t *vdevice_id)
{
	struct iommu_vdevice_alloc alloc = {
		.size = sizeof(alloc),
		.viommu_id = viommu_id,
		.dev_id = dev_id,
		.virt_id = logical_device_id,
	};
	int rc;

	rc = do_ioctl(iommufd, IOMMU_VDEVICE_ALLOC, &alloc,
		      "IOMMU_VDEVICE_ALLOC");
	if (rc)
		return rc;
	*vdevice_id = alloc.out_vdevice_id;
	return 0;
}

static int alloc_direct_hwpt(int iommufd, uint32_t dev_id, uint32_t viommu_id,
			     uint32_t *hwpt_id)
{
	struct iommu_hwpt_direct direct = {};
	struct iommu_hwpt_alloc alloc = {
		.size = sizeof(alloc),
		.dev_id = dev_id,
		.pt_id = viommu_id,
		.data_type = IOMMU_HWPT_DATA_DIRECT,
		.data_len = sizeof(direct),
		.data_uptr = (uintptr_t)&direct,
	};
	int rc;

	rc = do_ioctl(iommufd, IOMMU_HWPT_ALLOC, &alloc, "IOMMU_HWPT_ALLOC");
	if (rc)
		return rc;
	*hwpt_id = alloc.out_hwpt_id;
	return 0;
}

static int attach_direct_hwpt(int vfio_fd, uint32_t hwpt_id)
{
	struct vfio_device_attach_iommufd_pt attach = {
		.argsz = sizeof(attach),
		.pt_id = hwpt_id,
	};

	return do_ioctl(vfio_fd, VFIO_DEVICE_ATTACH_IOMMUFD_PT, &attach,
			"VFIO_DEVICE_ATTACH_IOMMUFD_PT");
}

static int detach_iommufd_pt(int vfio_fd)
{
	struct vfio_device_detach_iommufd_pt detach = {
		.argsz = sizeof(detach),
	};

	return do_ioctl(vfio_fd, VFIO_DEVICE_DETACH_IOMMUFD_PT, &detach,
			"VFIO_DEVICE_DETACH_IOMMUFD_PT");
}

static void usage(const char *argv0)
{
	fprintf(stderr, "Usage: %s /dev/vfio/devices/vfioN <logical-device-id>\n",
		argv0);
}

int main(int argc, char *argv[])
{
	uint64_t logical_device_id;
	uint32_t direct_hwpt_id = 0;
	uint32_t vdevice_id = 0;
	uint32_t viommu_id = 0;
	uint32_t dev_id = 0;
	int vfio_fd = -1;
	int iommufd = -1;
	int mshv_fd = -1;
	int vm_fd = -1;
	int attached = 0;
	int rc;

	if (argc != 3) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}

	rc = parse_u64(argv[2], &logical_device_id);
	if (rc) {
		fprintf(stderr, "Invalid logical device ID: %s\n", argv[2]);
		return EXIT_FAILURE;
	}

	iommufd = open("/dev/iommu", O_RDWR | O_CLOEXEC);
	if (iommufd < 0) {
		fprintf(stderr, "open /dev/iommu failed: %s\n", strerror(errno));
		return EXIT_FAILURE;
	}

	vfio_fd = open(argv[1], O_RDWR | O_CLOEXEC);
	if (vfio_fd < 0) {
		fprintf(stderr, "open %s failed: %s\n", argv[1], strerror(errno));
		rc = -errno;
		goto out;
	}

	rc = bind_vfio_device(vfio_fd, iommufd, &dev_id);
	if (rc)
		goto out;

	rc = create_mshv_partition(&mshv_fd, &vm_fd);
	if (rc)
		goto out;

	rc = alloc_direct_viommu(iommufd, dev_id, vm_fd, &viommu_id);
	if (rc)
		goto out;

	rc = alloc_vdevice(iommufd, viommu_id, dev_id, logical_device_id,
			   &vdevice_id);
	if (rc)
		goto out;

	rc = alloc_direct_hwpt(iommufd, dev_id, viommu_id, &direct_hwpt_id);
	if (rc)
		goto out;

	rc = attach_direct_hwpt(vfio_fd, direct_hwpt_id);
	if (rc)
		goto out;
	attached = 1;

	printf("Direct attach established:\n");
	printf("  dev_id=%" PRIu32 "\n", dev_id);
	printf("  viommu_id=%" PRIu32 "\n", viommu_id);
	printf("  vdevice_id=%" PRIu32 " virt_id=%" PRIu64 "\n",
	       vdevice_id, logical_device_id);
	printf("  direct_hwpt_id=%" PRIu32 "\n", direct_hwpt_id);

out:
	if (attached) {
		int detach_rc = detach_iommufd_pt(vfio_fd);

		if (!rc)
			rc = detach_rc;
	}
	if (direct_hwpt_id) {
		int destroy_rc = iommufd_destroy(iommufd, direct_hwpt_id);

		if (!rc)
			rc = destroy_rc;
	}
	if (vdevice_id) {
		int destroy_rc = iommufd_destroy(iommufd, vdevice_id);

		if (!rc)
			rc = destroy_rc;
	}
	if (viommu_id) {
		int destroy_rc = iommufd_destroy(iommufd, viommu_id);

		if (!rc)
			rc = destroy_rc;
	}
	if (vm_fd >= 0)
		close(vm_fd);
	if (mshv_fd >= 0)
		close(mshv_fd);
	if (vfio_fd >= 0)
		close(vfio_fd);
	if (iommufd >= 0)
		close(iommufd);

	return rc ? EXIT_FAILURE : EXIT_SUCCESS;
}
