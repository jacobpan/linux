/*
 * intel_pch_thermal.c - Intel PCH thermal reporting device driver
 *
 * Copyright (C) 2012 Intel Corporation
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.        See the GNU
 * General Public License for more details.
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 * Author:
 */
/*
 * TODO:
 * - abstraction for various pch, cpt, ppt. lpt
 * - PM features, s0 sensor shutdown, TSPM reg
 * - sensor reading slope and adjust, PTA
 * - DIMM sensor DTV in pch or b0d4
 */
#define DEBUG

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/moduleparam.h>
#include <linux/acpi.h>
#include <linux/pci-acpi.h>
#include <linux/thermal.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>


#define NR_PCH_TRIPS 0
/* Panther Point and Cougar Point (PPT/CPT) MMIO register Offset */

/* Software semaphore bit.  After a core-well reset, a read to this bit
 * returns a 0.  * After the first read, subsequent reads will return a 1.
 * A write of a 1 to this bit will reset the next read value to 0.
 * Writing a 0 to this bit has no effect.
 * Software can poll this bit until it reads a 0, and will then own the
 * usage of the thermal sensor.
 * This bit has no other effect on the hardware, and is only used as a
 * semaphore among various independent software threads that may need
 * to use the thermal sensor.
 * Software that reads this register but does not intend to claim exclusive
 * access of the thermal sensor must write a one to this bit if it reads a 0,
 * in order to allow other software threads to claim it.
 * We need to use this bit if we want to program AUX trip point via MMIO
 */
#define TSIU_PPT    0
#define TSIU_PPT_SEM    (1 << 0) /* Thermal sensor 0 in use */

/* Thermal Sensor Enable */
#define TSE_PPT    0x01 /* was TSC0 in DPTF */
/* Bit 7: Thermal sensor enable - must be enabled */
#define TSE_PPT_EN    (1 << 7)
/* Bit 6: Analog hysteresis control - must be disabled */
#define TSE_PPT_AHC   (1 << 6)
/* Bit 5-4: Digital hysteresis amount - must not be 00; 01 - 1C; 10 - 2C;
 *   11 - 3C
 */
#define TSE_PPT_DHA   (3 << 4)
/* Bit 3-2: Sequencer enable and rate - must not be 00 */
#define TSE_PPT_SER   (3 << 2)
/* Bit 1: Thermal sensor output select - 0: catastrophic, 1: hot;
 * recommended is 1
 */
#define TSE_PPT_OUT   (1 << 1)

#define TSC1_PPT    0x41    /* TBD thermal sensor 1 control? */

/* Thermal sensor 0 status */
#define TSS_PPT    0x02
/* Bit 7: Catastrophic Trip Indicator */
#define TSS_PPT_CTI    (1 << 7)
/* Bit 6: Hot Trip Indicator */
#define TSS_PPT_HTI    (1 << 6)
/* Bit 5: Auxiliary Trip Indicator */
#define TSS_PPT_ATI    (1 << 5)
/* Bit 4: Reserved */
/* Bit 3: Auxiliary2 Trip Indicator */
#define TSS_PPT_ATI2    (1 << 3)

/* Thermal sensor 0 thermometer read */
#define TSTR_PPT    0x03
/* Bit 7: Reserved */
/* Bit 6-0: Thermometer reading */

/* Thermal sensor 0 temperature trip point */
#define TSTTP_PPT    0x04
/* Bit 31-24: Auxiliary2 Trip point setting; loackable via TSPC bit 7 */
#define TSTTP_PPT_AUX_TRIP2    (0xff << 24)
/* Bit 23-16: Auxiliary Trip point setting; loackable via TSPC bit 7 */
#define TSTTP_PPT_AUX_TRIP    (0xff << 16)

#define TSCO_PPT    0x08 /* Thermal Sensor Catastrophic Lock-Down */
#define TSCO_PPT_LBC    (1 << 7)

#define TSES_PPT    0x0c /* Thermal Sensor Error Status Register */
#define TSGPEN_PPT  0x0d /*Thermal Sensor General Purpose Event */

#define PTA_PPT    0x14    /* PCH temperature adjust */

/* Internal temperature values
 * Bit 15-8: MCH temp; ME writes the MCH temperature here after collecting
 *               from the PM_MSG and calculating the correct value
 * Bit 7-0: PCH temperature; ME writes the PCH temperature here
 * Size is four bytes */
#define ITV_PPT    0xD8




/* Bit 7: Policy lock down bit; must be 0 for programming AUX trip point
 * Size is one byte */

#define TSPC0_OFFSET_PPT    0x0E    /* Thermal sensor 0 policy control */
#define TSPC1_OFFSET_PPT    0x4E    /* Thermal sensor 1 policy control */


/* Bit 7: Aux2 high to low trip happened
 * Bit 6: Catastrophic high to low trip happened
 * Bit 5: Hot high to low trip happened
 * Bit 4: Aux high to low trip happened
 * Bit 3: Aux2 low to high trip happened
 * Bit 2: Catastrophic low to high trip happened
 * Bit 1: Hot low to high trip happened
 * Bit 0: Aux low to high trip happened
 * Size is one byte */

/* Software must write a 1 to clear the status bit */

#define TSES0_OFFSET_PPT    0x0C    /* Thermal sensor 0 error status */
#define TSES1_OFFSET_PPT    0x4C    /* Thermal sensor 1 error status */


/* Bit 7: Aux2 high to low enable
 * Bit 6: Catastrophic high to low enable
 * Bit 5: Hot high to low enable
 * Bit 4: Aux high to low enable
 * Bit 3: Aux2 low to high enable
 * Bit 2: Catastrophic low to high enable
 * Bit 1: Hot low to high enable
 * Bit 0: Aux low to high enable
 * Size is one byte */

#define TSPIEN0_OFFSET_PPT    0x82    /* PCI interrupt event enable */
#define TSPIEN1_OFFSET_PPT    0xC2    /* PCI interrupt event enable 1 */

#define PCH_THERMAL_DID_PPT  0x1e24
#define PCH_THERMAL_DID_CPT  0x1c24
#define PCH_THERMAL_DID_LPT  0x8c24
 /*Thermal Sensor Policy Control Register */
#define TSPC_PPT             0x0E
/*Thermal Reporting Control Register */
#define TRC_PPT              0x1A
/*Alert Enable Register */
#define AE_PPT               0x3F
/*Processor Temperature Value Register*/
#define PTL_PPT              0x56
/*Thermal Sensor PCI Interrupt Enable Register*/
#define TSPIEN_PPT           0x82
/*Processor Temperature Value Register*/
#define PTV_PPT              0x60
/*Thermal Throttling Register*/
#define TT_PPT               0x6C
/*Thermal Sensor Register Lock Control Register */
#define TSLOCK_PPT           0x83
/*Thermal Compares 2 Register */
#define TC2_PPT              0xAC
/*DIMM Temperature Values Register*/
#define DTV_PPT              0xB0
/*Internal Temperature Values Register*/
#define PHL_PPT              0X70
struct intel_pch_thermal_device;
struct thermal_cooling_device *cooling_dev;
struct pch_dev_ops {
	int (*irq_handle)(struct intel_pch_thermal_device *dev);
	int (*hw_init)(struct intel_pch_thermal_device *dev);
	int (*get_temp)(struct intel_pch_thermal_device *, unsigned long *);
	int (*get_cat_trip)(struct intel_pch_thermal_device *dev, unsigned long *temp);
	int (*set_thermal_alert_high)(struct intel_pch_thermal_device *dev,
			unsigned long temp);
	int (*set_thermal_alert_low)(unsigned long temp);
	int (*get_thermal_alert_high)(unsigned long *temp);
	int (*get_thermal_alert_low)(unsigned long *temp);
};

struct intel_pch_thermal_device {
	char *name;
	void __iomem *mem_base;
	int (*probe)(void); /* TBD */
	const struct pch_dev_ops *ops;
	acpi_handle		acpi_handle;
	spinlock_t		lock;
	/* constants to calculate temp from tstr.*/
	unsigned long temp_slope;
	unsigned long temp_offset;

#ifdef DEBUG
	struct dentry		*debug_dir;
#endif
};

static inline void pch_thermal_mem_writel(struct intel_pch_thermal_device *ptdev,
					u32 reg, u32 v)
{
	writel(v, ptdev->mem_base + reg);
}

static inline u32 pch_thermal_mem_readl(struct intel_pch_thermal_device *ptdev,
					u32 reg)
{
	return readl(ptdev->mem_base + reg);
}

static inline u16 pch_thermal_mem_readw(struct intel_pch_thermal_device *ptdev,
					u32 reg)
{
	return readw(ptdev->mem_base + reg);
}

static inline u8 pch_thermal_mem_readb(struct intel_pch_thermal_device *ptdev,
					u32 reg)
{
	return readb(ptdev->mem_base + reg);
}
static inline void pch_thermal_mem_writeb(struct intel_pch_thermal_device *ptdev,
					u32 reg, u32 v)
{
	writeb(v, ptdev->mem_base + reg);
}

#define	DRIVER_DESC		"Intel PCH Thermal Reporting Driver"
#define	DRIVER_VERSION		"April 2012"
static const char driver_name[] = "intel_pch_thermal";

static DEFINE_PCI_DEVICE_TABLE(pci_ids) = {
	{PCI_DEVICE(PCI_VENDOR_ID_INTEL, 0x1e24)}, /* PPT */
	{PCI_DEVICE(PCI_VENDOR_ID_INTEL, 0x1c24)}, /* CPT */
	{ 0,}
};

MODULE_DEVICE_TABLE(pci, pci_ids);
static struct thermal_zone_device *tzd;

static irqreturn_t pch_thermal_irq(int irq, void *dev_id)
{
	return IRQ_HANDLED;
}

static void  intel_pch_thermal_remove(struct pci_dev *pdev)
{
	struct intel_pch_thermal_device	*ptdev = pci_get_drvdata(pdev);

	dev_dbg(&pdev->dev, "%s\n", __func__);
	iounmap(ptdev->mem_base);
	free_irq(pdev->irq, ptdev);
	pci_set_drvdata(pdev, NULL);
	kfree(ptdev);
	thermal_cooling_device_unregister(cooling_dev);
	pci_release_region(pdev, 0);
	thermal_zone_device_unregister(tzd);
	debugfs_remove_recursive(ptdev->debug_dir);
}

static int pch_cpt_ppt_handler(struct intel_pch_thermal_device *dev)
{
	pr_debug(KERN_DEBUG "%s\n", __func__);

	return 0;
}

static int pch_cpt_ppt_init(struct intel_pch_thermal_device *ptdev)
{
	u8 tser;

	/* REVISIT: sensor reading adjust constants for calculating real temp.
	 * Get from BIOS via PTA?
	 */

	tser = pch_thermal_mem_readb(ptdev, TSE_PPT);

	/* Y = temperature in C */
	/* X = decimal value DAC code */
	if (tser & TSE_PPT_OUT)	{
		/* Y = -0.7155X + 130.58 */
		ptdev->temp_slope = 7155;
		ptdev->temp_offset = 1305800;
	} else {
		/* Y = -0.7766X + 153.51 */
		ptdev->temp_slope = 7766;
		ptdev->temp_offset = 1535100;
	}

	return 0;
}

static int pch_cpt_ppt_get_ctt(struct intel_pch_thermal_device *ptdev, unsigned long *temp)
{
	/* FIXME: get real data */
	pr_debug(KERN_DEBUG "%s: dev %s\n", __func__, ptdev->name);
	*temp = 100000;
	return 0;
}

static int pch_cpt_ppt_get_temp(struct intel_pch_thermal_device *ptdev, unsigned long *temp)
{
	u8 tstr;

	/* TODO: cache value if read within 200ms ch 5.21.3.5 */
	tstr = pch_thermal_mem_readb(ptdev, TSTR_PPT) & 0x7F;
	*temp = (ptdev->temp_offset - (ptdev->temp_slope * tstr)) / 10;

	pr_debug(KERN_DEBUG "%s: tstr:%d, temp %lu\n", __func__, tstr, *temp);

	return 0;
}

static int pch_cpt_ppt_set_alert_high(struct intel_pch_thermal_device *ptdev, unsigned long temp)
{
	pr_debug(KERN_DEBUG "%s: dev %s\n", __func__, ptdev->name);

	return 0;
}

static inline int pch_get_crit_temp(struct thermal_zone_device *tzd, unsigned long *temp)
{
	struct intel_pch_thermal_device *ptdev = tzd->devdata;

	return ptdev->ops->get_cat_trip(ptdev, temp);
}

static struct pch_dev_ops pch_dev_ops_cpt_ppt = {
	.irq_handle = pch_cpt_ppt_handler,
	.hw_init = pch_cpt_ppt_init,
	.get_temp = pch_cpt_ppt_get_temp,
	.get_cat_trip = pch_cpt_ppt_get_ctt,
	.set_thermal_alert_high = pch_cpt_ppt_set_alert_high,
/*	.set_thermal_alert_low = pch_cpt_ppt_set_alert_low,
 *	.get_thermal_alert_high = pch_cpt_ppt_get_alert_high,
 *	.get_thermal_alert_low = pch_cpt_ppt_get_alert_low */
};

/* REVISIT: specific to LPT */
static struct pch_dev_ops pch_dev_ops_lpt = {
	.irq_handle = pch_cpt_ppt_handler,
	.hw_init = pch_cpt_ppt_init,
};

static void send_uevent(struct pci_dev *pdev)
{
	char event[] = "PCH ONLINE";
	char *envp[] = { event, NULL };

	kobject_uevent_env(&pdev->dev.kobj, KOBJ_CHANGE, envp);

}

static int pch_thermal_get_temp(struct thermal_zone_device *tzd, unsigned long *temp)
{
	struct intel_pch_thermal_device *ptdev = tzd->devdata;

	return ptdev->ops->get_temp(ptdev, temp);

}

/* FIXME: get pch specific trips */
static int pch_get_trip_type(struct thermal_zone_device *thermal, int trip,
				 enum thermal_trip_type *type)
{
	*type = THERMAL_TRIP_ACTIVE;

	return 0;
}

/* FIXME: get pch specific trips */
static int pch_get_trip_temp(struct thermal_zone_device *thermal, int trip,
				 unsigned long *temp)
{
	*temp = 100000;

	return 0;
}

/* TBD */
static int pch_get_acpi_handle(struct pci_dev *pdev)
{
	struct intel_pch_thermal_device	*ptdev = pci_get_drvdata(pdev);
	struct acpi_buffer output = { .length = ACPI_ALLOCATE_BUFFER };
	union acpi_object *out_obj;
	acpi_status status;
	int rc = -ENOENT;

	ptdev->acpi_handle = DEVICE_ACPI_HANDLE(&pdev->dev);
	/* get hysteresis */
	status = acpi_evaluate_object(ptdev->acpi_handle, "GTSH", NULL, &output);

	if (status == AE_NOT_FOUND) {
		dev_dbg(&pdev->dev, "no hysteresis data\n");
		goto out_free;
	}

	if (ACPI_FAILURE(status)) {
		dev_err(&pdev->dev, "ACPI get GTSH failed, 0x%x)\n",
			status);
		goto out_free;
	}

	out_obj = output.pointer;
	if (out_obj->type != ACPI_TYPE_INTEGER) {
		dev_err(&pdev->dev, "GTSH returned unexpected object type 0x%x\n",
			out_obj->type);
	}

	pr_info(KERN_INFO "GTSH %llu\n", out_obj->integer.value);

	rc = 0;
 out_free:
	kfree(output.pointer);
	return rc;
}

static int pch_thermal_get_mode(struct thermal_zone_device *thermal,
			    enum thermal_device_mode *mode)
{
	/* FIXME: based on user/dptf or kernel usage */
	*mode = THERMAL_DEVICE_ENABLED;

	return 0;
}
static int pch_thermal_set_mode(struct thermal_zone_device *tzd,
				enum thermal_device_mode mode)
{
	struct intel_pch_thermal_device *ptdev = tzd->devdata;

	pr_info("%s: dev %s\n", __func__, ptdev->name);
	/* TBD: enable/disable */

	return 0;
}

#ifdef CONFIG_THERMAL_EMULATION
static int intel_pch_set_emul_temp(struct thermal_zone_device *tzd, unsigned long temp)
{

	return 0;
}
#endif
/*Thermal cooling device bind*/
static int pch_thermal_cooling_device(struct thermal_zone_device *thermal, struct thermal_cooling_device *cdev, bool bind)
{
	struct pch_device *device = cdev->devdata;
	struct intel_pch_thermal *tdz = thermal->devdata;
	struct intel_pch_device *dev;
	int trip = -1;
	int result = 0;
	if (bind)
		result = thermal_zone_bind_cooling_device
			(thermal, trip, cdev,
			 THERMAL_NO_LIMIT, THERMAL_NO_LIMIT);
	else
		result = thermal_zone_unbind_cooling_device
			(thermal, trip, cdev);
}
static int
intel_thermal_zone_bind_cooling_device_cb(struct thermal_zone_device *thermal,
		struct thermal_cooling_device *cdev)
{
	return pch_thermal_cooling_device(thermal, cdev, true);
}
static int
intel_thermal_zone_unbind_cooling_device_cb(struct thermal_zone_device *thermal,
		struct thermal_cooling_device *cdev)
{
	return pch_thermal_cooling_device(thermal, cdev, false);
}
/*end of cooling device bind*/
static struct thermal_zone_device_ops tzd_ops = {
	.bind = intel_thermal_zone_bind_cooling_device_cb,
	.unbind = intel_thermal_zone_unbind_cooling_device_cb,
#if 0
	.bind = pch_thermal_bind_cdev, /* bind throttling control cdev to itself? */
	.unbind = pch_thermal_unbind_cdev,
#endif
	.get_temp = pch_thermal_get_temp,
	/*.set_emul_temp = intel_pch_set_emul_temp, */
	.get_mode = pch_thermal_get_mode,
	.set_mode = pch_thermal_set_mode,
	.get_trip_type = pch_get_trip_type,
	.get_trip_temp = pch_get_trip_temp,
	.get_crit_temp = pch_get_crit_temp,
};


static int pch_debug_show(struct seq_file *m, void *unused)
{
	struct intel_pch_thermal_device *ptdev = m->private;

	/* clear semaphore bit */
	pch_thermal_mem_writeb(ptdev, 0, 1),

	seq_printf(m, "%s:%p\n"
		"TSIU  : 0x%02X\n"
		"TSE  : 0x%02X\n"
		"TSS  : 0x%02X\n"
		"TSTR  : 0x%02X\n"
		"TSTTP  : 0x%08X\n"
		"PTA  : 0x%04X\n"
		"ITV  : 0x%08X\n"
		"TSCO  : 0x%02X\n"
		"TSES  : 0x%02X\n"
		"TSGPEN  : 0x%02X\n"
		"TSPC    :0x%02X\n"
		"TRC  : 0x%04X\n"
		"AE  : 0x%02X\n"
		"PTL  : 0x%04X\n"
		"PTV  : 0x%04X\n"
		"TT  : 0x%08X\n"
		"PHL  : 0x%02X\n"
		"TSPIEN  : 0x%02X\n"
		"TSLOCK  : 0x%02X\n"
		"TC2  : 0x%08X\n"
		"DTV  : 0x%08X\n",
		driver_name, ptdev,
		pch_thermal_mem_readb(ptdev, TSIU_PPT),
		pch_thermal_mem_readb(ptdev, TSE_PPT),
		pch_thermal_mem_readb(ptdev, TSS_PPT),
		pch_thermal_mem_readb(ptdev, TSTR_PPT),
		pch_thermal_mem_readl(ptdev, TSTTP_PPT),
		pch_thermal_mem_readw(ptdev, PTA_PPT),
		pch_thermal_mem_readl(ptdev, ITV_PPT),
		pch_thermal_mem_readb(ptdev, TSCO_PPT),
		pch_thermal_mem_readb(ptdev, TSES_PPT),
		pch_thermal_mem_readb(ptdev, TSGPEN_PPT),
		pch_thermal_mem_readb(ptdev, TSPC_PPT),
		pch_thermal_mem_readb(ptdev, TRC_PPT),
		pch_thermal_mem_readb(ptdev, AE_PPT),
		pch_thermal_mem_readb(ptdev, PTL_PPT),
		pch_thermal_mem_readb(ptdev, PTV_PPT),
		pch_thermal_mem_readb(ptdev, TT_PPT),
		pch_thermal_mem_readb(ptdev, PHL_PPT),
		pch_thermal_mem_readb(ptdev, TSPIEN_PPT),
		pch_thermal_mem_readb(ptdev, TSLOCK_PPT),
		pch_thermal_mem_readb(ptdev, TC2_PPT),
		pch_thermal_mem_readb(ptdev, DTV_PPT)
		);

	return 0;
}

static int pch_debug_open(struct inode *inode,
			struct file *file)
{
	pr_info(KERN_INFO "%s:%p\n", __func__, inode->i_private);
	return single_open(file, pch_debug_show, inode->i_private);
}

static const struct file_operations pch_debug_fops = {
	.open		= pch_debug_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
	.owner		= THIS_MODULE,
};

static inline void pch_create_debug_files(struct intel_pch_thermal_device *ptdev)
{
	ptdev->debug_dir = debugfs_create_dir(ptdev->name, NULL);
	if (!ptdev->debug_dir)
		return;

	if (!debugfs_create_file("pch_debug_reg", S_IRUGO, ptdev->debug_dir, ptdev,
						    &pch_debug_fops))
		goto file_error;

	return;

file_error:
	debugfs_remove_recursive(ptdev->debug_dir);
}
/*bind to generic thermal layer as cooling device*/
static int pch_get_max_state(struct thermal_cooling_device *cdev,
		unsigned long *state)
{
	*state = 1;
	return 0;
}
static int pch_get_cur_state(struct thermal_cooling_device *cdev,
		unsigned long *state)
{
	*state = -1;
	return 0;
}
static int pch_set_cur_state(struct thermal_cooling_device *cdev,
		unsigned long new_target_ratio)
{
	int ret = 0;
	return ret;
}
static struct thermal_cooling_device_ops pch_cooling_ops = {

	.get_max_state = pch_get_max_state,
	.get_cur_state = pch_get_cur_state,
	.set_cur_state = pch_set_cur_state,
};
/* end of cooling stuffs*/
/*	intel_pch_thermal_probe
 *	@dev: the PCI device matching
 *	@id: entry in the match table
 */
static int intel_pch_thermal_probe(struct pci_dev *pdev,
				const struct pci_device_id *id)
{
	int err = 0;
	struct intel_pch_thermal_device *ptdev;

	ptdev = kzalloc(sizeof(*ptdev), GFP_KERNEL);
	if (!ptdev)
		return -ENOMEM;

	pci_set_drvdata(pdev, ptdev);
	err = pci_enable_device(pdev);
	if (err) {
		dev_err(&pdev->dev, "failed to enable pci dev\n");
		goto error_free;
	}

	if (!(pci_resource_flags(pdev, 0) & IORESOURCE_MEM)) {
		dev_err(&pdev->dev, "Cannot find base address\n");
		err = -ENXIO;
		goto error_disable;
	}

	err = pci_request_regions(pdev, driver_name);
	if (err) {
		dev_dbg(&pdev->dev, "failed to request pci region\n");
		goto error_disable;
	}

	if (request_irq(pdev->irq, pch_thermal_irq, IRQF_SHARED,
				driver_name, ptdev)) {
		dev_err(&pdev->dev, "failed to request irq %d\n",
			pdev->irq);
		err = -EBUSY;
		goto error_release;
	}
	ptdev->mem_base = pci_iomap(pdev, 0, 0);

	if (!ptdev->mem_base) {
		err = -ENOMEM;
		dev_err(&pdev->dev, "failed to map iomem\n");
		goto error_free_irq;
	}
	/* Assign device specific op functions since regiser map may vary across
	 * different PCH chipsets.
	 */
	switch (pdev->device) {
	case PCH_THERMAL_DID_PPT:
	case PCH_THERMAL_DID_CPT:
		ptdev->name = "pch_thermal_ppt";
		dev_info(&pdev->dev, "Intel Panther Point thermal device detected\n");
		ptdev->ops = &pch_dev_ops_cpt_ppt;
		break;
	case PCH_THERMAL_DID_LPT:
		ptdev->name = "pch_thermal_lpt";
		ptdev->ops = &pch_dev_ops_lpt;
		break;
	default:
		dev_err(&pdev->dev, "unknown pch thermal device\n");
		err = -ENODEV;
		goto error_cleanup;
	}
/* register  cooling device */
	cooling_dev = thermal_cooling_device_register("intel_pch_cooling", NULL,
			&pch_cooling_ops);
	if (IS_ERR(cooling_dev))
		pr_info(KERN_INFO "failed to register cooling device pch\n");
/* end cooling device register*/
	tzd = thermal_zone_device_register(ptdev->name,
			NR_PCH_TRIPS, 0, ptdev, &tzd_ops, NULL, 0, 0);
	if (IS_ERR(tzd)) {
		dev_err(&pdev->dev, "failed to register thermal zone pch\n");
		goto error_cleanup;
	}
	ptdev->ops->hw_init(ptdev);
	pch_get_acpi_handle(pdev);
#ifdef DEBUG
	pch_create_debug_files(ptdev);
#endif
	send_uevent(pdev);
	return 0;

error_cleanup:
	pci_iounmap(pdev, ptdev->mem_base);
error_free_irq:
	free_irq(pdev->irq, ptdev);
error_release:
	pci_release_regions(pdev);
error_disable:
	pci_disable_device(pdev);
error_free:
	kfree(ptdev);

	return err;
}


static void intel_pch_thermal_shutdown(struct pci_dev *pdev)
{
	dev_dbg(&pdev->dev, "%s()\n", __func__);
}

static struct pci_driver intel_pch_thermal_driver = {
	.name =		(char *) driver_name,
	.id_table =	pci_ids,

	.probe =	intel_pch_thermal_probe,
	.remove =	intel_pch_thermal_remove,

	.shutdown =	intel_pch_thermal_shutdown,
};


static int __init init(void)
{
	return pci_register_driver(&intel_pch_thermal_driver);
}
module_init(init);


static void __exit cleanup(void)
{
	pci_unregister_driver(&intel_pch_thermal_driver);
}
module_exit(cleanup);


MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPL");
