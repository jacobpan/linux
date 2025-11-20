/*
 * VFIO API definition
 *
 * Copyright (C) 2012 Red Hat, Inc.  All rights reserved.
 *     Author: Alex Williamson <alex.williamson@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#ifndef _UAPIVFIO_H
#define _UAPIVFIO_H
#define _GNU_SOURCE  
#include <linux/types.h>
#include <linux/ioctl.h>
#include <sys/eventfd.h>
#include <sys/poll.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define VFIO_API_VERSION	0

enum {
	IOMMUFD_CMD_BASE = 0x80,
	IOMMUFD_CMD_DESTROY = IOMMUFD_CMD_BASE,
	IOMMUFD_CMD_IOAS_ALLOC = 0x81,
	IOMMUFD_CMD_IOAS_ALLOW_IOVAS = 0x82,
	IOMMUFD_CMD_IOAS_COPY = 0x83,
	IOMMUFD_CMD_IOAS_IOVA_RANGES = 0x84,
	IOMMUFD_CMD_IOAS_MAP = 0x85,
	IOMMUFD_CMD_IOAS_UNMAP = 0x86,
	IOMMUFD_CMD_OPTION = 0x87,
	IOMMUFD_CMD_VFIO_IOAS = 0x88,
	IOMMUFD_CMD_HWPT_ALLOC = 0x89,
	IOMMUFD_CMD_GET_HW_INFO = 0x8a,
	IOMMUFD_CMD_HWPT_SET_DIRTY_TRACKING = 0x8b,
	IOMMUFD_CMD_HWPT_GET_DIRTY_BITMAP = 0x8c,
	IOMMUFD_CMD_HWPT_INVALIDATE = 0x8d,
	IOMMUFD_CMD_FAULT_QUEUE_ALLOC = 0x8e,
	IOMMUFD_CMD_IOAS_MAP_FILE = 0x8f,
	IOMMUFD_CMD_VIOMMU_ALLOC = 0x90,
	IOMMUFD_CMD_VDEVICE_ALLOC = 0x91,
	IOMMUFD_CMD_IOAS_CHANGE_PROCESS = 0x92,
	IOMMUFD_CMD_IOAS_GET_PA = 0x95,
};

/* Kernel & User level defines for VFIO IOCTLs. */
#define IOMMUFD_TYPE (';')
/**
 * struct iommu_ioas_alloc - ioctl(IOMMU_IOAS_ALLOC)
 * @size: sizeof(struct iommu_ioas_alloc)
 * @flags: Must be 0
 * @out_ioas_id: Output IOAS ID for the allocated object
 *
 * Allocate an IO Address Space (IOAS) which holds an IO Virtual Address (IOVA)
 * to memory mapping.
 */
struct iommu_ioas_alloc {
	__u32 size;
	__u32 flags;
	__u32 out_ioas_id;
};
#define IOMMU_IOAS_ALLOC _IO(IOMMUFD_TYPE, IOMMUFD_CMD_IOAS_ALLOC)

struct iommu_destroy {
	__u32 size;
	__u32 id;
};
#define IOMMU_DESTROY _IO(IOMMUFD_TYPE, IOMMUFD_CMD_DESTROY)

/* Extensions */

#define VFIO_TYPE1_IOMMU		1
#define VFIO_NOIOMMU_IOMMU		8

/*
 * The IOCTL interface is designed for extensibility by embedding the
 * structure length (argsz) and flags into structures passed between
 * kernel and userspace.  We therefore use the _IO() macro for these
 * defines to avoid implicitly embedding a size into the ioctl request.
 * As structure fields are added, argsz will increase to match and flag
 * bits will be defined to indicate additional fields with valid data.
 * It's *always* the caller's responsibility to indicate the size of
 * the structure passed by setting argsz appropriately.
 */

#define VFIO_TYPE	(';')
#define VFIO_BASE	100

/* -------- IOCTLs for VFIO file descriptor (/dev/vfio/vfio) -------- */

/**
 * VFIO_GET_API_VERSION - _IO(VFIO_TYPE, VFIO_BASE + 0)
 *
 * Report the version of the VFIO API.  This allows us to bump the entire
 * API version should we later need to add or change features in incompatible
 * ways.
 * Return: VFIO_API_VERSION
 * Availability: Always
 */
#define VFIO_GET_API_VERSION		_IO(VFIO_TYPE, VFIO_BASE + 0)

/**
 * VFIO_CHECK_EXTENSION - _IOW(VFIO_TYPE, VFIO_BASE + 1, __u32)
 *
 * Check whether an extension is supported.
 * Return: 0 if not supported, 1 (or some other positive integer) if supported.
 * Availability: Always
 */
#define VFIO_CHECK_EXTENSION		_IO(VFIO_TYPE, VFIO_BASE + 1)

/**
 * VFIO_SET_IOMMU - _IOW(VFIO_TYPE, VFIO_BASE + 2, __s32)
 *
 * Set the iommu to the given type.  The type must be supported by an
 * iommu driver as verified by calling CHECK_EXTENSION using the same
 * type.  A group must be set to this file descriptor before this
 * ioctl is available.  The IOMMU interfaces enabled by this call are
 * specific to the value set.
 * Return: 0 on success, -errno on failure
 * Availability: When VFIO group attached
 */
#define VFIO_SET_IOMMU			_IO(VFIO_TYPE, VFIO_BASE + 2)

/* -------- IOCTLs for GROUP file descriptors (/dev/vfio/$GROUP) -------- */

/**
 * VFIO_GROUP_GET_STATUS - _IOR(VFIO_TYPE, VFIO_BASE + 3,
 *						struct vfio_group_status)
 *
 * Retrieve information about the group.  Fills in provided
 * struct vfio_group_info.  Caller sets argsz.
 * Return: 0 on succes, -errno on failure.
 * Availability: Always
 */
struct vfio_group_status {
	__u32	argsz;
	__u32	flags;
#define VFIO_GROUP_FLAGS_VIABLE		(1 << 0)
#define VFIO_GROUP_FLAGS_CONTAINER_SET	(1 << 1)
};
#define VFIO_GROUP_GET_STATUS		_IO(VFIO_TYPE, VFIO_BASE + 3)

/**
 * VFIO_GROUP_SET_CONTAINER - _IOW(VFIO_TYPE, VFIO_BASE + 4, __s32)
 *
 * Set the container for the VFIO group to the open VFIO file
 * descriptor provided.  Groups may only belong to a single
 * container.  Containers may, at their discretion, support multiple
 * groups.  Only when a container is set are all of the interfaces
 * of the VFIO file descriptor and the VFIO group file descriptor
 * available to the user.
 * Return: 0 on success, -errno on failure.
 * Availability: Always
 */
#define VFIO_GROUP_SET_CONTAINER	_IO(VFIO_TYPE, VFIO_BASE + 4)

/**
 * VFIO_GROUP_UNSET_CONTAINER - _IO(VFIO_TYPE, VFIO_BASE + 5)
 *
 * Remove the group from the attached container.  This is the
 * opposite of the SET_CONTAINER call and returns the group to
 * an initial state.  All device file descriptors must be released
 * prior to calling this interface.  When removing the last group
 * from a container, the IOMMU will be disabled and all state lost,
 * effectively also returning the VFIO file descriptor to an initial
 * state.
 * Return: 0 on success, -errno on failure.
 * Availability: When attached to container
 */
#define VFIO_GROUP_UNSET_CONTAINER	_IO(VFIO_TYPE, VFIO_BASE + 5)

/**
 * VFIO_GROUP_GET_DEVICE_FD - _IOW(VFIO_TYPE, VFIO_BASE + 6, char)
 *
 * Return a new file descriptor for the device object described by
 * the provided string.  The string should match a device listed in
 * the devices subdirectory of the IOMMU group sysfs entry.  The
 * group containing the device must already be added to this context.
 * Return: new file descriptor on success, -errno on failure.
 * Availability: When attached to container
 */
#define VFIO_GROUP_GET_DEVICE_FD	_IO(VFIO_TYPE, VFIO_BASE + 6)

/* --------------- IOCTLs for DEVICE file descriptors --------------- */

/**
 * VFIO_DEVICE_GET_INFO - _IOR(VFIO_TYPE, VFIO_BASE + 7,
 *						struct vfio_device_info)
 *
 * Retrieve information about the device.  Fills in provided
 * struct vfio_device_info.  Caller sets argsz.
 * Return: 0 on success, -errno on failure.
 */
struct vfio_device_info {
	__u32	argsz;
	__u32	flags;
#define VFIO_DEVICE_FLAGS_RESET	(1 << 0)	/* Device supports reset */
#define VFIO_DEVICE_FLAGS_PCI	(1 << 1)	/* vfio-pci device */
	__u32	num_regions;	/* Max region index + 1 */
	__u32	num_irqs;	/* Max IRQ index + 1 */
};
#define VFIO_DEVICE_GET_INFO		_IO(VFIO_TYPE, VFIO_BASE + 7)

/**
 * VFIO_DEVICE_GET_REGION_INFO - _IOWR(VFIO_TYPE, VFIO_BASE + 8,
 *				       struct vfio_region_info)
 *
 * Retrieve information about a device region.  Caller provides
 * struct vfio_region_info with index value set.  Caller sets argsz.
 * Implementation of region mapping is bus driver specific.  This is
 * intended to describe MMIO, I/O port, as well as bus specific
 * regions (ex. PCI config space).  Zero sized regions may be used
 * to describe unimplemented regions (ex. unimplemented PCI BARs).
 * Return: 0 on success, -errno on failure.
 */
struct vfio_region_info {
	__u32	argsz;
	__u32	flags;
#define VFIO_REGION_INFO_FLAG_READ	(1 << 0) /* Region supports read */
#define VFIO_REGION_INFO_FLAG_WRITE	(1 << 1) /* Region supports write */
#define VFIO_REGION_INFO_FLAG_MMAP	(1 << 2) /* Region supports mmap */
	__u32	index;		/* Region index */
	__u32	resv;		/* Reserved for alignment */
	__u64	size;		/* Region size (bytes) */
	__u64	offset;		/* Region offset from start of device fd */
};
#define VFIO_DEVICE_GET_REGION_INFO	_IO(VFIO_TYPE, VFIO_BASE + 8)

/**
 * VFIO_DEVICE_GET_IRQ_INFO - _IOWR(VFIO_TYPE, VFIO_BASE + 9,
 *				    struct vfio_irq_info)
 *
 * Retrieve information about a device IRQ.  Caller provides
 * struct vfio_irq_info with index value set.  Caller sets argsz.
 * Implementation of IRQ mapping is bus driver specific.  Indexes
 * using multiple IRQs are primarily intended to support MSI-like
 * interrupt blocks.  Zero count irq blocks may be used to describe
 * unimplemented interrupt types.
 *
 * The EVENTFD flag indicates the interrupt index supports eventfd based
 * signaling.
 *
 * The MASKABLE flags indicates the index supports MASK and UNMASK
 * actions described below.
 *
 * AUTOMASKED indicates that after signaling, the interrupt line is
 * automatically masked by VFIO and the user needs to unmask the line
 * to receive new interrupts.  This is primarily intended to distinguish
 * level triggered interrupts.
 *
 * The NORESIZE flag indicates that the interrupt lines within the index
 * are setup as a set and new subindexes cannot be enabled without first
 * disabling the entire index.  This is used for interrupts like PCI MSI
 * and MSI-X where the driver may only use a subset of the available
 * indexes, but VFIO needs to enable a specific number of vectors
 * upfront.  In the case of MSI-X, where the user can enable MSI-X and
 * then add and unmask vectors, it's up to userspace to make the decision
 * whether to allocate the maximum supported number of vectors or tear
 * down setup and incrementally increase the vectors as each is enabled.
 */
struct vfio_irq_info {
	__u32	argsz;
	__u32	flags;
#define VFIO_IRQ_INFO_EVENTFD		(1 << 0)
#define VFIO_IRQ_INFO_MASKABLE		(1 << 1)
#define VFIO_IRQ_INFO_AUTOMASKED	(1 << 2)
#define VFIO_IRQ_INFO_NORESIZE		(1 << 3)
	__u32	index;		/* IRQ index */
	__u32	count;		/* Number of IRQs within this index */
};
#define VFIO_DEVICE_GET_IRQ_INFO	_IO(VFIO_TYPE, VFIO_BASE + 9)

/**
 * VFIO_DEVICE_SET_IRQS - _IOW(VFIO_TYPE, VFIO_BASE + 10, struct vfio_irq_set)
 *
 * Set signaling, masking, and unmasking of interrupts.  Caller provides
 * struct vfio_irq_set with all fields set.  'start' and 'count' indicate
 * the range of subindexes being specified.
 *
 * The DATA flags specify the type of data provided.  If DATA_NONE, the
 * operation performs the specified action immediately on the specified
 * interrupt(s).  For example, to unmask AUTOMASKED interrupt [0,0]:
 * flags = (DATA_NONE|ACTION_UNMASK), index = 0, start = 0, count = 1.
 *
 * DATA_BOOL allows sparse support for the same on arrays of interrupts.
 * For example, to mask interrupts [0,1] and [0,3] (but not [0,2]):
 * flags = (DATA_BOOL|ACTION_MASK), index = 0, start = 1, count = 3,
 * data = {1,0,1}
 *
 * DATA_EVENTFD binds the specified ACTION to the provided __s32 eventfd.
 * A value of -1 can be used to either de-assign interrupts if already
 * assigned or skip un-assigned interrupts.  For example, to set an eventfd
 * to be trigger for interrupts [0,0] and [0,2]:
 * flags = (DATA_EVENTFD|ACTION_TRIGGER), index = 0, start = 0, count = 3,
 * data = {fd1, -1, fd2}
 * If index [0,1] is previously set, two count = 1 ioctls calls would be
 * required to set [0,0] and [0,2] without changing [0,1].
 *
 * Once a signaling mechanism is set, DATA_BOOL or DATA_NONE can be used
 * with ACTION_TRIGGER to perform kernel level interrupt loopback testing
 * from userspace (ie. simulate hardware triggering).
 *
 * Setting of an event triggering mechanism to userspace for ACTION_TRIGGER
 * enables the interrupt index for the device.  Individual subindex interrupts
 * can be disabled using the -1 value for DATA_EVENTFD or the index can be
 * disabled as a whole with: flags = (DATA_NONE|ACTION_TRIGGER), count = 0.
 *
 * Note that ACTION_[UN]MASK specify user->kernel signaling (irqfds) while
 * ACTION_TRIGGER specifies kernel->user signaling.
 */
struct vfio_irq_set {
	__u32	argsz;
	__u32	flags;
#define VFIO_IRQ_SET_DATA_NONE		(1 << 0) /* Data not present */
#define VFIO_IRQ_SET_DATA_BOOL		(1 << 1) /* Data is bool (u8) */
#define VFIO_IRQ_SET_DATA_EVENTFD	(1 << 2) /* Data is eventfd (s32) */
#define VFIO_IRQ_SET_ACTION_MASK	(1 << 3) /* Mask interrupt */
#define VFIO_IRQ_SET_ACTION_UNMASK	(1 << 4) /* Unmask interrupt */
#define VFIO_IRQ_SET_ACTION_TRIGGER	(1 << 5) /* Trigger interrupt */
	__u32	index;
	__u32	start;
	__u32	count;
	__u8	data[];
};
#define VFIO_DEVICE_SET_IRQS		_IO(VFIO_TYPE, VFIO_BASE + 10)

#define VFIO_IRQ_SET_DATA_TYPE_MASK	(VFIO_IRQ_SET_DATA_NONE | \
					 VFIO_IRQ_SET_DATA_BOOL | \
					 VFIO_IRQ_SET_DATA_EVENTFD)
#define VFIO_IRQ_SET_ACTION_TYPE_MASK	(VFIO_IRQ_SET_ACTION_MASK | \
					 VFIO_IRQ_SET_ACTION_UNMASK | \
					 VFIO_IRQ_SET_ACTION_TRIGGER)
/**
 * VFIO_DEVICE_RESET - _IO(VFIO_TYPE, VFIO_BASE + 11)
 *
 * Reset a device.
 */
#define VFIO_DEVICE_RESET		_IO(VFIO_TYPE, VFIO_BASE + 11)
struct vfio_device_bind_iommufd {
	__u32           argsz;
	__u32           flags;
	__s32           iommufd;
	__u32           out_devid;
};
#define VFIO_DEVICE_BIND_IOMMUFD	_IO(VFIO_TYPE, VFIO_BASE + 18)

/*
 * The VFIO-PCI bus driver makes use of the following fixed region and
 * IRQ index mapping.  Unimplemented regions return a size of zero.
 * Unimplemented IRQ types return a count of zero.
 */

enum {
	VFIO_PCI_BAR0_REGION_INDEX,
	VFIO_PCI_BAR1_REGION_INDEX,
	VFIO_PCI_BAR2_REGION_INDEX,
	VFIO_PCI_BAR3_REGION_INDEX,
	VFIO_PCI_BAR4_REGION_INDEX,
	VFIO_PCI_BAR5_REGION_INDEX,
	VFIO_PCI_ROM_REGION_INDEX,
	VFIO_PCI_CONFIG_REGION_INDEX,
	/*
	 * Expose VGA regions defined for PCI base class 03, subclass 00.
	 * This includes I/O port ranges 0x3b0 to 0x3bb and 0x3c0 to 0x3df
	 * as well as the MMIO range 0xa0000 to 0xbffff.  Each implemented
	 * range is found at it's identity mapped offset from the region
	 * offset, for example 0x3b0 is region_info.offset + 0x3b0.  Areas
	 * between described ranges are unimplemented.
	 */
	VFIO_PCI_VGA_REGION_INDEX,
	VFIO_PCI_NUM_REGIONS
};

enum {
	VFIO_PCI_INTX_IRQ_INDEX,
	VFIO_PCI_MSI_IRQ_INDEX,
	VFIO_PCI_MSIX_IRQ_INDEX,
	VFIO_PCI_NUM_IRQS
};

/**
 * VFIO_DEVICE_GET_PCI_HOT_RESET_INFO - _IORW(VFIO_TYPE, VFIO_BASE + 12,
 *                                            struct vfio_pci_hot_reset_info)
 *
 * Return: 0 on success, -errno on failure:
 *      -enospc = insufficient buffer, -enodev = unsupported for device.
 */
struct vfio_pci_dependent_device {
	__u32   group_id;
	__u16   segment;
	__u8    bus;
	__u8    devfn; /* Use PCI_SLOT/PCI_FUNC */
};

struct vfio_pci_hot_reset_info {
	__u32   argsz;
#define VFIO_PCI_HOT_RESET_FLAG_DEV_ID		(1 << 0)
#define VFIO_PCI_HOT_RESET_FLAG_DEV_ID_OWNED	(1 << 1)
	__u32   flags;
	__u32   count;
	struct vfio_pci_dependent_device        devices[];
};

#define VFIO_DEVICE_GET_PCI_HOT_RESET_INFO      _IO(VFIO_TYPE, VFIO_BASE + 12)

/**
 * VFIO_DEVICE_PCI_HOT_RESET - _IOW(VFIO_TYPE, VFIO_BASE + 13,
 *                                  struct vfio_pci_hot_reset)
 *
 * Return: 0 on success, -errno on failure.
 */
struct vfio_pci_hot_reset {
	__u32   argsz;
	__u32   flags;
	__u32   count;
	__s32   group_fds[];
};

#define VFIO_DEVICE_PCI_HOT_RESET       _IO(VFIO_TYPE, VFIO_BASE + 13)

/* -------- API for Type1 VFIO IOMMU -------- */

/**
 * VFIO_IOMMU_GET_INFO - _IOR(VFIO_TYPE, VFIO_BASE + 12, struct vfio_iommu_info)
 *
 * Retrieve information about the IOMMU object. Fills in provided
 * struct vfio_iommu_info. Caller sets argsz.
 *
 * XXX Should we do these by CHECK_EXTENSION too?
 */
struct vfio_iommu_type1_info {
	__u32	argsz;
	__u32	flags;
#define VFIO_IOMMU_INFO_PGSIZES (1 << 0)	/* supported page sizes info */
	__u64	iova_pgsizes;		/* Bitmap of supported page sizes */
};

#define VFIO_IOMMU_GET_INFO _IO(VFIO_TYPE, VFIO_BASE + 12)

/**
 * VFIO_IOMMU_MAP_DMA - _IOW(VFIO_TYPE, VFIO_BASE + 13, struct vfio_dma_map)
 *
 * Map process virtual addresses to IO virtual addresses using the
 * provided struct vfio_dma_map. Caller sets argsz. READ &/ WRITE required.
 */
struct vfio_iommu_type1_dma_map {
	__u32	argsz;
	__u32	flags;
#define VFIO_DMA_MAP_FLAG_READ (1 << 0)		/* readable from device */
#define VFIO_DMA_MAP_FLAG_WRITE (1 << 1)	/* writable from device */
	__u64	vaddr;				/* Process virtual address */
	__u64	iova;				/* IO virtual address */
	__u64	size;				/* Size of mapping (bytes) */
};

#define VFIO_IOMMU_MAP_DMA _IO(VFIO_TYPE, VFIO_BASE + 13)

/**
 * VFIO_IOMMU_UNMAP_DMA - _IOWR(VFIO_TYPE, VFIO_BASE + 14,
 *							struct vfio_dma_unmap)
 *
 * Unmap IO virtual addresses using the provided struct vfio_dma_unmap.
 * Caller sets argsz.  The actual unmapped size is returned in the size
 * field.  No guarantee is made to the user that arbitrary unmaps of iova
 * or size different from those used in the original mapping call will
 * succeed.
 */
struct vfio_iommu_type1_dma_unmap {
	__u32	argsz;
	__u32	flags;
	__u64	iova;				/* IO virtual address */
	__u64	size;				/* Size of mapping (bytes) */
};

#define VFIO_IOMMU_UNMAP_DMA _IO(VFIO_TYPE, VFIO_BASE + 14)
struct iommu_ioas_map {
	__u32 size;
	__u32 flags;
	__u32 ioas_id;
	__u32 __reserved;
	__aligned_u64 user_va;
	__aligned_u64 length;
	__aligned_u64 iova;
};
#define IOMMU_IOAS_MAP _IO(IOMMUFD_TYPE, IOMMUFD_CMD_IOAS_MAP)

struct iommu_ioas_unmap {
	__u32 size;
	__u32 ioas_id;
	__aligned_u64 iova;
	__aligned_u64 length;
};
#define IOMMU_IOAS_UNMAP _IO(IOMMUFD_TYPE, IOMMUFD_CMD_IOAS_UNMAP)
struct iommu_ioas_get_pa {
	__u32 size;
	__u32 flags;
	__u32 ioas_id;
	__u32 __reserved;
	__aligned_u64 iova;
	__aligned_u64 length;
	__aligned_u64 phys;
};
#define IOMMU_IOAS_GET_PA _IO(IOMMUFD_TYPE, IOMMUFD_CMD_IOAS_GET_PA)

#endif /* _UAPIVFIO_H */

#include <errno.h>
#include <libgen.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/vfs.h>

#include <linux/ioctl.h>
#include <stdbool.h>
void usage(char *name)
{
	printf("usage: %s  c|g <cdev id>|<iommu group id> <ssss:bb:dd.f>\n", name);
}

static int trace_fd;
void trace_write(const char *fmt, ...)
{
	va_list ap;
	char buf[256];
	int n;

	if (trace_fd < 0)
		return;

	va_start(ap, fmt);
	n = vsnprintf(buf, 256, fmt, ap);
	va_end(ap);

	write(trace_fd, buf, n);
}

static int __iommufd = -1;
void trace_write(const char *fmt, ...);
static int iommufd_open(void)
{
	__iommufd = open("/dev/iommu", O_RDWR);
	if (__iommufd < 0)
		return -1;
	printf("opened iommufd %d\n", __iommufd);
	trace_fd = open("/sys/kernel/tracing/trace_marker", O_WRONLY);
	return 0;
}

/*
 * /dev/vfio/
 *|-- devices
 *|   `-- noiommu-vfio0 // cdev node
 *|-- noiommu-0
 * `-- vfio
	*/
char *pci_get_device_vfio_id(const char *bdf)
{
	char *path = NULL;
	char *vfio_id = NULL;
	struct dirent *dentry;
	DIR *dp;

	if (asprintf(&path, "/sys/bus/pci/devices/%s/vfio-dev", bdf) < 0) {
		printf("asprintf failed\n");
		return NULL;
	}

	dp = opendir(path);
	if (!dp) {
		printf("could not open directory; is %s bound to vfio-pci?\n", bdf);
		return NULL;
	}

	do {
		/*
		 * If readdir() reaches the end of the directory stream, errno
		 * is NOT changed. errno may have been left at some non-zero
		 * value, so reset it.
		 */
		errno = 0;

		dentry = readdir(dp);
		if (!dentry) {
			if (!errno)
				errno = EINVAL;

			goto out;
		}

		if (strncmp("noiommu-vfio", dentry->d_name, 4) == 0)
			break;
	} while (dentry != NULL);

	if (dentry == NULL) {
		errno = EINVAL;
		goto out;
	}

	vfio_id = strdup(dentry->d_name);
out:
	if (closedir(dp))
			printf("closedir");
	printf("vfio id %s\n", vfio_id);
	return vfio_id;
}

static int vfio_test_device_info(int device)
{
	int i;
	struct vfio_device_info device_info = {
		.argsz = sizeof(device_info)
	};
	struct vfio_region_info region_info = {
		.argsz = sizeof(region_info)
	};


	if (ioctl(device, VFIO_DEVICE_GET_INFO, &device_info)) {
		printf("Failed to get device info\n");
		return -1;
	}

	printf("Device supports %d regions, %d irqs\n",
	       device_info.num_regions, device_info.num_irqs);

	if (device_info.num_regions > 6) {
		printf("Unexpected number of regions %d\n", device_info.num_regions);
		/* override */
		device_info.num_regions = 6;
	}
	for (i = 0; i < device_info.num_regions -1; i++) {
		printf("Region %d: ", i);
		region_info.index = i;
		if (ioctl(device, VFIO_DEVICE_GET_REGION_INFO, &region_info)) {
			printf("Failed to get info\n");
			continue;
		}

		printf("size 0x%lx, offset 0x%lx, flags 0x%x\n",
		       (unsigned long)region_info.size,
		       (unsigned long)region_info.offset, region_info.flags);
		if (0 && region_info.flags & VFIO_REGION_INFO_FLAG_MMAP) {
			void *map = mmap(NULL, (size_t)region_info.size,
					 PROT_READ, MAP_SHARED, device,
					 (off_t)region_info.offset);
			if (map == MAP_FAILED) {
				printf("mmap failed\n");
				continue;
			}

			printf("[");
			fwrite(map, 1, region_info.size > 16 ? 16 :
						region_info.size, stdout);
			printf("]\n");
			munmap(map, (size_t)region_info.size);
		}
	}
	return 0;
}

/* Helper function to check ioctl return value and print error */
#define CHECK_IOCTL(fd, cmd, arg, msg) do { \
    if (ioctl(fd, cmd, arg) < 0) { \
        perror(msg); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

static void test_device_msix(int device_fd)
{
	struct vfio_irq_set *irq_set;
    size_t irq_set_size;
    uint32_t vector_count;
	int *event_fds;

	/* Query MSI interrupt info */
    struct vfio_irq_info irq_info = { .argsz = sizeof(irq_info), .index = VFIO_PCI_MSIX_IRQ_INDEX };
    CHECK_IOCTL(device_fd, VFIO_DEVICE_GET_IRQ_INFO, &irq_info, "Failed to get MSI info");
    if (irq_info.count == 0) {
        fprintf(stderr, "Device does not support MSI interrupts\n");
        exit(EXIT_FAILURE);
    }
    printf("MSI-X interrupt count: %u\n", irq_info.count);

    vector_count = irq_info.count > 4 ? 4 : irq_info.count; /* Limit to 4 vectors for simplicity */
    event_fds = calloc(vector_count, sizeof(int));
    if (!event_fds) {
        perror("Failed to allocate eventfds array");
        exit(EXIT_FAILURE);
    }
    for (uint32_t i = 0; i < vector_count; i++) {
        event_fds[i] = eventfd(0, EFD_NONBLOCK);
        if (event_fds[i] < 0) {
            perror("Failed to create eventfd");
            exit(EXIT_FAILURE);
        }
    }

	irq_set_size = sizeof(struct vfio_irq_set) + sizeof(int) * vector_count;
    /* Step 10: Enable MSI-X interrupts with eventfds */
    irq_set = calloc(1, irq_set_size);
    if (!irq_set) {
        perror("Failed to allocate IRQ set");
        exit(EXIT_FAILURE);
    }
    irq_set->argsz = irq_set_size;
    irq_set->flags = VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER;
    irq_set->index = VFIO_PCI_MSIX_IRQ_INDEX;
    irq_set->start = 0;
    irq_set->count = vector_count;
    memcpy(irq_set->data, event_fds, sizeof(int) * vector_count);
    CHECK_IOCTL(device_fd, VFIO_DEVICE_SET_IRQS, irq_set, "Failed to enable MSI-X interrupts");
    free(irq_set);
}

int ioas_alloc(int iommufd)
{
    struct iommu_ioas_alloc ioas_alloc = {
        .size = sizeof(ioas_alloc),
    };

    if (ioctl(iommufd, IOMMU_IOAS_ALLOC, &ioas_alloc) != 0) {
        perror("IOAS_ALLOC");
		return -1;
    }

    printf("Allocated IOAS with ID: %u\n", ioas_alloc.out_ioas_id);
	return ioas_alloc.out_ioas_id;
}

int ioas_destroy(int iommufd, uint32_t ioas_id) {
    struct iommu_destroy destroy = {
        .size = sizeof(struct iommu_destroy),
        .id = ioas_id,
    };
	printf("Destroying IOAS with ID: %u\n", ioas_id);
    return ioctl(iommufd, IOMMU_DESTROY, &destroy);
}

static int vfio_test_hot_reset(int devfd)
{
	struct vfio_pci_hot_reset_info *reset_info;
	struct vfio_pci_hot_reset *reset;
	int i, ret, *pfd;

	printf("Get info on HOT RESET\n");	
	reset_info = malloc(sizeof(*reset_info));
	if (!reset_info) {
		printf("Failed to alloc info struct\n");
		return -ENOMEM;
	}

	reset_info->argsz = sizeof(*reset_info) + 64;

	ret = ioctl(devfd, VFIO_DEVICE_GET_PCI_HOT_RESET_INFO, reset_info);
	if (ret && errno == ENODEV) {
		printf("Device does not support hot reset\n");
		return 0;
	}

	printf("Dependent device count: %d\n", reset_info->count);
	printf("Dependent device flags: 0x%x\n", reset_info->flags);
	for (i = 0; i < reset_info->count; i++) {
		printf("Dependent device %d: %s %d, segment %d, bus %d, devfn %d\n",
		       i,  (reset_info->flags & VFIO_PCI_HOT_RESET_FLAG_DEV_ID) ?
		       "dev_id" : "group_id", reset_info->devices[i].group_id,
			   reset_info->devices[i].segment,
		       reset_info->devices[i].bus, reset_info->devices[i].devfn);
	}
	
	printf("Attempting HOT reset: 0 count FD array, 0 group fd\n");
	fflush(stdout);
	reset = malloc(sizeof(*reset) + sizeof(*pfd));
	pfd = &reset->group_fds[0];
	// no group?
	*pfd = 0;

	reset->argsz = sizeof(*reset) + (reset_info->count * sizeof(*reset_info->devices));
	reset->count = 0;
	reset->flags = 0;

	ret = ioctl(devfd, VFIO_DEVICE_PCI_HOT_RESET, reset);
	printf("HOT RESET : %s, ret %d errno %d\n", ret ? "Failed" : "Pass", ret, errno);

	return 0;
}

int iommufd_bind( int iommufd, int devfd)
{
	struct vfio_device_bind_iommufd bind = {
		.argsz = sizeof(bind),
		.flags = 0,
		.iommufd = iommufd,
		.out_devid = 0,
	};

	if (ioctl(devfd, VFIO_DEVICE_BIND_IOMMUFD, &bind)) {
		printf("Failed to bind device to iommufd %d\n", iommufd);
		return -1;
	}
	printf("IOMMUFD bind succeeded\n");
	return 0;
}
enum iommufd_ioas_map_flags {
	IOMMU_IOAS_MAP_FIXED_IOVA = 1 << 0,
	IOMMU_IOAS_MAP_WRITEABLE = 1 << 1,
	IOMMU_IOAS_MAP_READABLE = 1 << 2,
};

void device_reset(int devfd)
{
	if (ioctl(devfd, VFIO_DEVICE_RESET))
	    perror("VFIO_DEVICE_RESET failed");
	else
	    printf("reset device successfully\n");
}

static uint64_t iommufd_ioas_map(int iommufd, int ioas_id, uint64_t iova,
							 uint64_t uvaddr, uint64_t size)
{
	uint32_t flags = IOMMU_IOAS_MAP_READABLE | IOMMU_IOAS_MAP_WRITEABLE;

	/* If iova is provided, use it; otherwise let the kernel choose */
	if (iova) {
		printf("Mapping with fixed IOVA 0x%lx\n", (unsigned long)iova);
		flags |= IOMMU_IOAS_MAP_FIXED_IOVA;
	} else {
		printf("Mapping without fixed IOVA\n");
	}
	struct iommu_ioas_map map = {
		.size = sizeof(map),
		.flags = flags,
		.ioas_id = ioas_id,
		.iova = iova,
		.user_va = uvaddr,
		.length = size,
	};

	if (ioctl(iommufd, IOMMU_IOAS_MAP, &map) != 0) {
		perror("IOMMU_IOAS_MAP");
		return 0;
	}
	printf("Successfully mapped iommufd %d IOAS %d IOVA 0x%lx to VA 0x%lx size 0x%lx\n",
	       iommufd, ioas_id, (unsigned long)map.iova, (unsigned long)uvaddr,
	       (unsigned long)size);

	return map.iova;
}

static void iommufd_ioas_unmap(int iommufd, int ioas_id, uint64_t iova,
							   uint64_t size)
{
	struct iommu_ioas_unmap args = {
		.size = sizeof(args),
		.iova = iova,
		.length = size,
		.ioas_id = ioas_id,
	};

	if (ioctl(iommufd, IOMMU_IOAS_UNMAP, &args) != 0) {
		perror("IOMMU_IOAS_UNMAP");
		return;
	}
	printf("Successfully unmapped iommufd %d IOAS %d IOVA 0x%lx size 0x%lx\n",
	       iommufd, ioas_id, (unsigned long)iova,
	       (unsigned long)size);
}

struct vfio_device_attach_iommufd_pt {
	__u32	argsz;
	__u32	flags;
#define VFIO_DEVICE_ATTACH_PASID	(1 << 0)
	__u32	pt_id;
	__u32	pasid;
};
#define VFIO_DEVICE_ATTACH_IOMMUFD_PT		_IO(VFIO_TYPE, VFIO_BASE + 19)

struct vfio_device_detach_iommufd_pt {
	__u32	argsz;
	__u32	flags;
#define VFIO_DEVICE_DETACH_PASID	(1 << 0)
	__u32	pasid;
};

#define VFIO_DEVICE_DETACH_IOMMUFD_PT		_IO(VFIO_TYPE, VFIO_BASE + 20)

static int vfio_device_attach_iommufd_pt_ioctl(int cdev_fd, unsigned int pt_id)
{
	struct vfio_device_attach_iommufd_pt attach_args = {
		.argsz = sizeof(attach_args),
		.pt_id = pt_id,
	};

	return ioctl(cdev_fd, VFIO_DEVICE_ATTACH_IOMMUFD_PT, &attach_args);
}
static int vfio_device_detach_iommufd_pt_ioctl(int cdev_fd)
{
	struct vfio_device_detach_iommufd_pt detach_args = {
		.argsz = sizeof(detach_args),
	};

	return ioctl(cdev_fd, VFIO_DEVICE_DETACH_IOMMUFD_PT, &detach_args);
}

static int iommufd_ioas_test_get_pa(int iommufd, int ioas_id, uint64_t iova)
{
	struct iommu_ioas_get_pa get_pa = {
		.size = sizeof(get_pa),
		.flags = 0,
		.ioas_id = ioas_id,
		.iova = iova,
		.length = 0,
		.phys = 0,
	};

	if (ioctl(iommufd, IOMMU_IOAS_GET_PA, &get_pa) != 0) {
		perror("IOMMU_IOAS_GET_PA");
		return -1;
	}
	printf("Successfully got PA 0x%lx for IOVA 0x%lx, length 0x%lx\n",
	       (unsigned long)get_pa.phys, (unsigned long)iova,
		   (unsigned long)get_pa.length);

	return 0;
}

static int ioas_map_test_mmap(int iommufd, int ioas_id)
{
	uint64_t uvaddr, iova;
	int len = 0x10000 * 4; // 4 pages of 64k

	uvaddr = (uint64_t)mmap(NULL, len, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (uvaddr == (uint64_t)MAP_FAILED) {
		printf("mmap failed\n");
		return -1;
	}
	printf("mmap: Allocated user VA at 0x%lx\n", (unsigned long)uvaddr);
	iova = iommufd_ioas_map(iommufd, ioas_id, 0xffff0000, uvaddr, len);
	if (iova == 0) {
		munmap((void *)uvaddr, len);
		return -1;
	}
	iommufd_ioas_test_get_pa(iommufd, ioas_id, iova);
	iommufd_ioas_unmap(iommufd, ioas_id, iova, len);
	
	iova = iommufd_ioas_map(iommufd, ioas_id, 0, uvaddr, len);
	if (iova == 0) {
		munmap((void *)uvaddr, len);
		return -1;
	}
	iommufd_ioas_test_get_pa(iommufd, ioas_id, iova+0x10000);
	iommufd_ioas_unmap(iommufd, ioas_id, iova, len);
	munmap((void *)uvaddr, len);
	return 0;
}

#if 0
static int ioas_map_test_memfd(int iommufd, int ioas_id)
{
	int memfd;
	void *uvaddr;

	memfd = memfd_create("iommufd_test_memfd", MFD_CLOEXEC);
	if (memfd < 0) {
		printf("memfd_create failed\n");
		return -1;
	}

	if (ftruncate(memfd, 0x2000) != 0) {
		printf("ftruncate failed\n");
		close(memfd);
		return -1;
	}

	uvaddr = mmap(NULL, 0x2000, PROT_READ | PROT_WRITE,
				MAP_SHARED, memfd, 0);
	if (uvaddr == MAP_FAILED) {
		printf("mmap failed\n");
		close(memfd);
		return -1;
	}
	printf("memfd: Allocated user VA at %p\n", uvaddr);
	iommufd_ioas_map(iommufd, ioas_id, 0x30000, (uint64_t)uvaddr, 0x10000);
	iommufd_ioas_unmap(iommufd, ioas_id, 0x30000, 0x10000);
	munmap(uvaddr, 0x2000);
	close(memfd);
	return 0;
}
#endif

int iommufd_noiommu_test(const char *bdf)
{
	char *vfio_id = NULL;
	char *path = NULL;
	int devfd;
	int ioas_id;

	struct vfio_device_bind_iommufd bind = {
		.argsz = sizeof(bind),
		.flags = 0,
	};

	if (iommufd_open()) {
		printf("Failed to open /dev/iommu!\n");
		return -1;
	}

	vfio_id = pci_get_device_vfio_id(bdf);
	if (!vfio_id) {
		printf("could not determine the vfio device id for %s\n", bdf);
		return -1;
	}

	trace_write("IOMMU open device file\n");
    if (asprintf(&path, "/dev/vfio/devices/%s", vfio_id) < 0) {
		printf("asprintf failed\n");
		return -1;
	}
	trace_write("open vfio cdev %s\n", path);
	devfd = open(path, O_RDWR);
	if (devfd < 0) {
		printf("could not open the device cdev at %s\n", path);
		return -1;
	}
	printf("Opened device node %s %s\n", path, bdf);

	ioas_id = ioas_alloc(__iommufd);
	printf("IOAS id %d\n", ioas_id);

	iommufd_bind(__iommufd, devfd);
	//ioas_destroy(__iommufd, ioas_id);
	if (vfio_device_attach_iommufd_pt_ioctl(devfd, ioas_id)) {
		printf("Failed to attach pt to device\n");
		return -1;
	}
	printf("Successfully attached PT to device\n");

	ioas_map_test_mmap(__iommufd, ioas_id);
//	ioas_map_test_memfd(__iommufd, ioas_id);

	if (vfio_device_detach_iommufd_pt_ioctl(devfd)) {
		printf("Failed to detach PT from device\n");
		return -1;
	}
	printf("Successfully detached PT from device\n");

	device_reset(devfd);

	vfio_test_hot_reset(devfd);
	vfio_test_device_info(devfd);

	test_device_msix(devfd);

	close(devfd);
	return 0;
}

int main(int argc, char **argv)
{
	int ret, container, group, device, groupid;
	char path[PATH_MAX], mode;
	bool use_cdev = false; //iommufd only, no vfio compatibility mode
	int seg, bus, dev, func;

	struct vfio_group_status group_status = {
		.argsz = sizeof(group_status)
	};

	if (argc < 4) {
		usage(argv[0]);
		return -1;
	}

    ret = sscanf(argv[1], "%c", &mode);
    if (ret != 1) {
		usage(argv[0]);
		return -1;
	}

	if (mode == 'c') {
		use_cdev = true;
		printf("Using IOMMUFD cdev noiommu mode\n");
	} else if (mode == 'g') {
		printf("Using VFIO group noiommu mode\n");
	} else {
		usage(argv[0]);
		return -1;
	}

	ret = sscanf(argv[2], "%d", &groupid);
	if (ret != 1) {
		usage(argv[0]);
		return -1;
	}

	ret = sscanf(argv[3], "%04x:%02x:%02x.%d", &seg, &bus, &dev, &func);
	if (ret != 4) {
		usage(argv[0]);
		return -1;
	}

	printf("Using PCI device %04x:%02x:%02x.%d in %s %d\n",
	       seg, bus, dev, func, use_cdev ? "device id": "group id", groupid);

	if (use_cdev)
		return iommufd_noiommu_test(argv[3]);

	container = open("/dev/vfio/vfio", O_RDWR);
	if (container < 0) {
		printf("Failed to open /dev/vfio/vfio, %d (%s)\n",
		       container, strerror(errno));
		return container;
	}

	snprintf(path, sizeof(path), "/dev/vfio/noiommu-%d", groupid);
	group = open(path, O_RDWR);
	if (group < 0) {
		printf("Failed to open %s, %d (%s)\n",
		       path, group, strerror(errno));
		return group;
	}

	ret = ioctl(group, VFIO_GROUP_GET_STATUS, &group_status);
	if (ret) {
		printf("ioctl(VFIO_GROUP_GET_STATUS) failed\n");
		return ret;
	}

	if (!(group_status.flags & VFIO_GROUP_FLAGS_VIABLE)) {
		printf("Group not viable, are all devices attached to vfio?\n");
		return -1;
	}

	printf("pre-SET_CONTAINER:\n");
	printf("VFIO_CHECK_EXTENSION VFIO_TYPE1_IOMMU: %sPresent\n",
	       ioctl(container, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU) ?
	       "" : "Not ");
	printf("VFIO_CHECK_EXTENSION VFIO_NOIOMMU_IOMMU: %sPresent\n",
	       ioctl(container, VFIO_CHECK_EXTENSION, VFIO_NOIOMMU_IOMMU) ?
	       "" : "Not ");

	ret = ioctl(group, VFIO_GROUP_SET_CONTAINER, &container);
	if (ret) {
		printf("Failed to set group container\n");
		return ret;
	}

	printf("post-SET_CONTAINER:\n");
	printf("VFIO_CHECK_EXTENSION VFIO_TYPE1_IOMMU: %sPresent\n",
	       ioctl(container, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU) ?
	       "" : "Not ");
	printf("VFIO_CHECK_EXTENSION VFIO_NOIOMMU_IOMMU: %sPresent\n",
	       ioctl(container, VFIO_CHECK_EXTENSION, VFIO_NOIOMMU_IOMMU) ?
	       "" : "Not ");

	ret = ioctl(container, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU);
	if (!ret) {
		printf("ERROR, was able to use type1 IOMMU with no-iommu\n");
		return -1;
	}

	ret = ioctl(container, VFIO_SET_IOMMU, VFIO_NOIOMMU_IOMMU);
	if (ret) {
		printf("Failed to set IOMMU\n");
		return ret;
	}

	snprintf(path, sizeof(path), "%04x:%02x:%02x.%d", seg, bus, dev, func);

	device = ioctl(group, VFIO_GROUP_GET_DEVICE_FD, path);
	if (device < 0) {
		printf("Failed to get device %s\n", path);
		return -1;
	}

	if (vfio_test_device_info(device))
		return -1;

	printf("Success\n");
	printf("Press any key to exit\n");
	fgetc(stdin);

	return 0;
}
