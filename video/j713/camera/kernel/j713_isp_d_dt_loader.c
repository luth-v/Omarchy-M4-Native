// SPDX-License-Identifier: GPL-2.0-only
/* Per-boot power owner and overlay loader for the J713 camera. */
#define pr_fmt(fmt) "j713_isp_d_dt: " fmt

#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/ioport.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include "j713_isp_d.h"

static int setup_result = -EINPROGRESS;
module_param(setup_result, int, 0444);
MODULE_PARM_DESC(setup_result, "Overlay apply result; also verify all three DART drivers bound");
static unsigned int code_crc;
module_param(code_crc, uint, 0444);
MODULE_PARM_DESC(code_crc, "CRC32 of the firmware code segment at load, before any start");

const void *j713_isp_d_pristine_data;
EXPORT_SYMBOL_GPL(j713_isp_d_pristine_data);
u32 j713_isp_d_code_crc;
EXPORT_SYMBOL_GPL(j713_isp_d_code_crc);

static const char * const new_paths[] = {
	"/soc/iommu@48e8c0000", "/soc/iommu@48e8e0000",
	"/soc/iommu@48e900000", D_NODE,
	"/soc/mailbox@48d608000", "/soc/isp-stage-c@48c000000",
};

static int restore_power(void __iomem *pmgr, const u32 *saved, int count)
{
	u32 value;
	int ret = 0;

	while (count--) {
		writel(saved[count], pmgr + d_power_order[count]);
		if (readl_poll_timeout(pmgr + d_power_order[count], value,
				       FIELD_GET(D_PS_ACTUAL, value) ==
				       FIELD_GET(D_PS_TARGET, saved[count]), 10, 10000)) {
			pr_err("power restore timed out at +%x\n", d_power_order[count]);
			ret = -ETIMEDOUT;
		}
	}
	return ret;
}

/*
 * The loader refuses any ISP power state other than the boot state, so no
 * firmware has run yet. Keep a data copy and a code CRC for later restarts.
 */
static int d_snapshot_firmware(void)
{
	void __iomem *code, *data;
	void *copy, *bounce;
	int ret = -ENOMEM;

	if (region_intersects(D_CODE_PHYS, D_CODE_SIZE, IORESOURCE_SYSTEM_RAM,
			      IORES_DESC_NONE) != REGION_DISJOINT ||
	    region_intersects(D_DATA_PHYS, D_DATA_SIZE, IORESOURCE_SYSTEM_RAM,
			      IORES_DESC_NONE) != REGION_DISJOINT)
		return -EBUSY;
	copy = vmalloc(D_DATA_SIZE);
	bounce = kmalloc(SZ_16K, GFP_KERNEL);
	code = ioremap_wc(D_CODE_PHYS, D_CODE_SIZE);
	data = ioremap_wc(D_DATA_PHYS, D_DATA_SIZE);
	if (copy && bounce && code && data) {
		memcpy_fromio(copy, data, D_DATA_SIZE);
		j713_isp_d_code_crc = d_crc_io(code, D_CODE_SIZE, bounce);
		code_crc = j713_isp_d_code_crc;
		if (!j713_cam_firmware_qualified(code_crc)) {
			pr_err("unqualified ISP firmware CRC %08x; camera stays unloaded\n", code_crc);
			ret = -ENODEV;
			goto release;
		}
		j713_isp_d_pristine_data = copy;
		copy = NULL;
		ret = 0;
		pr_info("pristine data copy taken, code crc=%08x\n", code_crc);
	}
release:
	if (data)
		iounmap(data);
	if (code)
		iounmap(code);
	kfree(bounce);
	vfree(copy);
	return ret;
}

static int __init d_dt_init(void)
{
	size_t size = __dtbo_j713_isp_stage_d_end - __dtbo_j713_isp_stage_d_begin;
	u32 saved[ARRAY_SIZE(d_power_order)], value;
	struct device_node *np;
	void __iomem *pmgr;
	phandle aic;
	__be32 *blob;
	int ret, id = -1, changed = 0, patched = 0;
	size_t i;

	if (!d_machine_ok())
		return -ENODEV;
	for (i = 0; i < ARRAY_SIZE(new_paths); i++) {
		np = of_find_node_by_path(new_paths[i]);
		if (np) {
			of_node_put(np);
			pr_err("node already exists: %s\n", new_paths[i]);
			return -EBUSY;
		}
	}
	np = of_find_node_by_path("/soc/interrupt-controller@381180000");
	aic = np ? np->phandle : 0;
	of_node_put(np);
	if (!aic)
		return -ENODEV;
	blob = kmemdup(__dtbo_j713_isp_stage_d_begin, size, GFP_KERNEL);
	if (!blob)
		return -ENOMEM;
	for (i = 0; i < size / sizeof(*blob); i++) {
		if (be32_to_cpu(blob[i]) == 0xfeed00c1) {
			blob[i] = cpu_to_be32(aic);
			patched++;
		}
	}
	if (patched != 4) {
		ret = -EINVAL;
		goto free_blob;
	}
	pmgr = ioremap(D_PMGR, D_PMGR_SIZE);
	if (!pmgr) {
		ret = -ENOMEM;
		goto free_blob;
	}
	if (!d_power_active(pmgr, D_ISP_SYS)) {
		ret = -EHOSTDOWN;
		goto unmap;
	}
	/* Only take ownership from the Stage A/B boot state. */
	for (i = 0; i < ARRAY_SIZE(d_power_order); i++) {
		u32 expected = d_power_order[i] == 0x4020 ? 4 : 0;

		saved[i] = readl(pmgr + d_power_order[i]);
		if (FIELD_GET(D_PS_ACTUAL, saved[i]) != expected ||
		    FIELD_GET(D_PS_TARGET, saved[i]) != expected) {
			pr_err("unexpected power state +%x=%08x, power-cycle first\n",
			       d_power_order[i], saved[i]);
			ret = -EBUSY;
			goto unmap;
		}
	}
	ret = d_snapshot_firmware();
	if (ret)
		goto unmap;
	for (i = 0; i < ARRAY_SIZE(d_power_order); i++) {
		/* Same PMGR sequence and order as Stage B. */
		value = saved[i] & ~(BIT(28) | BIT(12) | BIT(10) |
					BIT(9) | BIT(8) | D_PS_TARGET);
		writel(value | D_PS_ACTIVE, pmgr + d_power_order[i]);
		changed++;
		ret = readl_poll_timeout(pmgr + d_power_order[i], value,
					 FIELD_GET(D_PS_ACTUAL, value) == D_PS_ACTIVE, 10, 10000);
		pr_info("power +%x=%08x result=%d\n", d_power_order[i], value, ret);
		if (ret)
			goto restore;
	}
	/* DART probe reads and resets the hardware synchronously here. */
	ret = of_overlay_fdt_apply(blob, size, &id, NULL);
	setup_result = ret;
	pr_info("overlay result=%d id=%d; ISP power held until power-off\n", ret, id);
	if (ret && id < 0)
		goto restore;
	/*
	 * A failed apply with an ID can leave devices behind. Keep power and
	 * this module even on that failure; the runner checks setup_result.
	 */
	iounmap(pmgr);
	kfree(blob);
	return 0;

restore:
	if (restore_power(pmgr, saved, changed))
		pr_err("power restore incomplete; power-cycle before any further test\n");
	vfree(j713_isp_d_pristine_data);
	j713_isp_d_pristine_data = NULL;
unmap:
	iounmap(pmgr);
free_blob:
	kfree(blob);
	return ret;
}

/* No module_exit: keep the DART providers and their power for this boot. */
module_init(d_dt_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("J713 camera per-boot power, DART and RTKit mailbox overlay");
