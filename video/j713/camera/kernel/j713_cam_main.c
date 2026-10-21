// SPDX-License-Identifier: GPL-2.0-only
#include <linux/ioport.h>
#include <linux/mm.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/utsname.h>
#include <linux/vmalloc.h>

#include "j713_cam.h"
#include "j713_isp_d.h"
#include "j713_isp_d_mmio.h"
#include "j713_isp_d_power.h"

#define D_VM_BASE CAM_VM_BASE
#define D_FW_END CAM_FW_END
#define D_PROT (IOMMU_READ | IOMMU_WRITE | IOMMU_CACHE)
#define CAM_CONTROL 0x44
#define CAM_STATUS 0x48
#define CAM_RUN BIT(4)
#define CAM_RUNNING BIT(0)
#define CAM_STOPPED BIT(1)

static_assert(CAM_CODE_SIZE == D_CODE_SIZE);
static_assert(CAM_DATA_SIZE == D_DATA_SIZE);

static unsigned int out_height = 1264;
module_param(out_height, uint, 0444);
MODULE_PARM_DESC(out_height, "Firmware output height: 1264 or fallback 720");
static const bool full_span;
const bool cam_fw_log;
const bool cam_fw_restart = true;
static unsigned int restart_settle_ms;
static unsigned int restart_off_ms;

static int cam_restart_ms_set(const char *value, const struct kernel_param *parameter)
{
	unsigned int maximum = parameter->arg == &restart_off_ms ? 10000 : 5000;
	unsigned int milliseconds;
	int ret = kstrtouint(value, 0, &milliseconds);

	if (ret)
		return ret;
	if (milliseconds > maximum)
		return -ERANGE;
	WRITE_ONCE(*(unsigned int *)parameter->arg, milliseconds);
	return 0;
}

static const struct kernel_param_ops cam_restart_ms_ops = {
	.set = cam_restart_ms_set,
	.get = param_get_uint,
};
module_param_cb(restart_off_ms, &cam_restart_ms_ops, &restart_off_ms, 0644);
MODULE_PARM_DESC(restart_off_ms,
		 "Minimum time after a completed power-stop, 0..10000 ms, default 0");
module_param_cb(restart_settle_ms, &cam_restart_ms_ops, &restart_settle_ms, 0644);
MODULE_PARM_DESC(restart_settle_ms,
		 "Settle after re-powering the ISP before a restart, 0..5000 ms, default 0");

static int status_state = -1;
static char dead_reason[160] = "none";
static unsigned int sessions, arena_entries, fw_starts;
static bool firmware_started;
static unsigned long long arena_live;
module_param_string(dead_reason, dead_reason, sizeof(dead_reason), 0444);
module_param(sessions, uint, 0444);
module_param(fw_starts, uint, 0444);
module_param(firmware_started, bool, 0444);
module_param(arena_live, ullong, 0444);
module_param(arena_entries, uint, 0444);

static int cam_state_get(char *buffer, const struct kernel_param *parameter)
{
	static const char * const names[] = { "probed", "idle", "streaming", "dead" };
	int state = READ_ONCE(status_state);

	return sysfs_emit(buffer, "%s\n", state < 0 ? "unbound" : names[state]);
}

static const struct kernel_param_ops cam_state_ops = { .get = cam_state_get };
module_param_cb(state, &cam_state_ops, NULL, 0444);

/* Called under service_lock, including the receive-side allocation path. */
void cam_publish_status(struct cam_device *cam)
{
	struct cam_arena_stats stats = cam_arena_stats(&cam->arena);

	WRITE_ONCE(arena_live, stats.live);
	WRITE_ONCE(arena_entries, stats.entries);
	WRITE_ONCE(sessions, cam->sessions);
}

void cam_set_state(struct cam_device *cam, enum cam_state state)
{
	unsigned long flags;

	lockdep_assert_held(&cam->video_lock);
	spin_lock_irqsave(&cam->service_lock, flags);
	if (!cam_dead(cam)) {
		WRITE_ONCE(cam->state, state);
		WRITE_ONCE(status_state, state);
	}
	spin_unlock_irqrestore(&cam->service_lock, flags);
}

void cam_fail(struct cam_device *cam, const char *reason)
{
	unsigned long flags;
	bool streaming;

	spin_lock_irqsave(&cam->service_lock, flags);
	if (cam_dead(cam)) {
		spin_unlock_irqrestore(&cam->service_lock, flags);
		return;
	}
	streaming = cam->state == CAM_STREAMING;
	strscpy(cam->dead_reason, reason, sizeof(cam->dead_reason));
	strscpy(dead_reason, reason, sizeof(dead_reason));
	WRITE_ONCE(cam->state, CAM_DEAD);
	WRITE_ONCE(status_state, CAM_DEAD);
	spin_unlock_irqrestore(&cam->service_lock, flags);
	complete_all(&cam->frames);
	if (streaming && READ_ONCE(cam->queue_ready))
		vb2_queue_error(&cam->queue);
	dev_err(cam->dev, "DEAD: %s; power off the Mac now before using the camera again\n", reason);
}

static int cam_check_power(struct device *dev)
{
	void __iomem *pmgr = ioremap(D_PMGR, D_PMGR_SIZE);
	int ret = 0;
	size_t i;

	if (!pmgr)
		return -ENOMEM;
	if (!d_power_active(pmgr, D_ISP_SYS))
		ret = -EHOSTDOWN;
	for (i = 0; i < ARRAY_SIZE(d_power_order); i++)
		if (!d_power_active(pmgr, d_power_order[i]))
			ret = -EHOSTDOWN;
	iounmap(pmgr);
	if (ret)
		dev_err(dev, "ISP power is not held by the Stage D loader\n");
	return ret;
}

static int cam_check_iommus(struct device *dev)
{
	static const u64 bases[] = { 0x48e8c0000, 0x48e8e0000, 0x48e900000 };
	struct of_phandle_args args;
	struct platform_device *dart;
	struct resource res;
	int i, ret;

	if (of_count_phandle_with_args(dev->of_node, "iommus", "#iommu-cells") != 3)
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(bases); i++) {
		ret = of_parse_phandle_with_args(dev->of_node, "iommus",
						 "#iommu-cells", i, &args);
		if (ret)
			return ret;
		ret = of_address_to_resource(args.np, 0, &res);
		if (!ret && (args.args_count != 1 || args.args[0] != 0 ||
			     res.start != bases[i] || resource_size(&res) != 0x4000 ||
			     !of_device_is_compatible(args.np, "apple,t8110-dart")))
			ret = -EINVAL;
		dart = of_find_device_by_node(args.np);
		if (!ret && (!dart || !dart->dev.driver))
			ret = -EPROBE_DEFER;
		if (dart)
			put_device(&dart->dev);
		of_node_put(args.np);
		if (ret)
			return ret;
	}
	return 0;
}

static void __iomem *cam_map_reg(struct platform_device *pdev, const char *name,
			       phys_addr_t phys, size_t size)
{
	struct resource *res = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);

	if (!res || res->start != phys || resource_size(res) != size)
		return ERR_PTR(-EINVAL);
	return devm_ioremap_resource(&pdev->dev, res);
}

static phys_addr_t cam_backing_phys(struct cam_device *cam, size_t offset)
{
	void *address = cam->ram + offset;
	struct page *page = is_vmalloc_addr(address) ? vmalloc_to_page(address) :
						     virt_to_page(address);

	if (!page || PageSlab(page) || PageReserved(page) || PageAnon(page) || PageLRU(page) ||
	    page_mapped(page) || !page_count(page) || folio_mapping(page_folio(page)))
		return 0;
	return page_to_phys(page) + offset_in_page(address);
}

static phys_addr_t cam_expected_phys(struct cam_device *cam, size_t offset)
{
	if (offset < D_CODE_SIZE)
		return D_CODE_PHYS + offset;
	if (offset < D_FW_END)
		return D_DATA_PHYS + offset - D_CODE_SIZE;
	return cam_backing_phys(cam, offset - D_FW_END);
}

static int cam_map_mmio(struct cam_device *cam)
{
	const struct d_mmio_window *window;
	u64 address;
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(d_mmio_windows); i++) {
		window = &d_mmio_windows[i];
		if (region_intersects(window->base, window->size, IORESOURCE_SYSTEM_RAM,
				      IORES_DESC_NONE) != REGION_DISJOINT)
			return -EBUSY;
		ret = iommu_map(cam->domain, window->base, window->base, window->size,
				D_PROT, GFP_KERNEL);
		if (ret)
			return ret;
		cam->mmio_mapped++;
		for (address = window->base; address < window->base + window->size;
		     address += PAGE_SIZE)
			if (iommu_iova_to_phys(cam->domain, address) != address)
				return -EFAULT;
		dev_info(cam->dev, "verified firmware identity map %#llx/%#zx (no AP MMIO access)\n",
			 window->base, window->size);
	}
	return 0;
}

static int cam_identity_alias(struct cam_device *cam)
{
	size_t size = cam->claim_count * PAGE_SIZE;
	struct page **pages;
	dma_addr_t dma;
	unsigned long i;

	dma = dma_map_page(cam->dev, cam->claim_pages, 0, size, DMA_BIDIRECTIONAL);
	if (dma_mapping_error(cam->dev, dma))
		return -ENOMEM;
	dma_unmap_page(cam->dev, dma, size, DMA_BIDIRECTIONAL);
	pages = kvmalloc_array(cam->claim_count, sizeof(*pages), GFP_KERNEL);
	if (!pages)
		return -ENOMEM;
	for (i = 0; i < cam->claim_count; i++)
		pages[i] = cam->claim_pages + i;
	cam->identity = vmap(pages, cam->claim_count, VM_MAP,
			      pgprot_dmacoherent(PAGE_KERNEL));
	kvfree(pages);
	return cam->identity ? 0 : -ENOMEM;
}

/* Stage D's d_take_power: only the exact boot state a power-stop leaves is
 * powered up again.
 */
static int cam_take_power(struct cam_device *cam, bool *cycled)
{
	void __iomem *pmgr = ioremap(D_PMGR, D_PMGR_SIZE);
	int ret = 0;

	*cycled = false;
	if (!pmgr)
		return -ENOMEM;
	if (d_power_active(pmgr, D_ISP_SYS) && d_power_at_boot_state(pmgr)) {
		ret = d_power_up(pmgr);
		*cycled = !ret;
		dev_info(cam->dev, "ISP sub-domains powered up after the power-stop: %d\n", ret);
	}
	iounmap(pmgr);
	return ret;
}

static int cam_check_cpu(struct cam_device *cam, bool cycled)
{
	u64 rvbar = readq(cam->rvbar);
	u32 status = readl(cam->cpu + CAM_STATUS);

	cam->control = readl(cam->cpu + CAM_CONTROL);
	if (cycled)
		dev_info(cam->dev, "re-arm: CPU control=%08x status=%08x RVBAR=%#llx\n",
			 cam->control, status, rvbar);
	if ((cam->control & CAM_RUN) || (status & CAM_RUNNING) || !(status & CAM_STOPPED))
		return -EBUSY;
	if (rvbar != (CAM_VM_BASE | 1) && cycled && !rvbar) {
		/* Only after our own power cycle: put back the value iBoot programmed. */
		writeq(CAM_VM_BASE | 1, cam->rvbar);
		rvbar = readq(cam->rvbar);
		dev_info(cam->dev, "RVBAR was cleared by the power cycle; restored to %#llx\n", rvbar);
	}
	return rvbar == (CAM_VM_BASE | 1) ? 0 : -EINVAL;
}

static int cam_check_image(struct cam_device *cam, bool restore)
{
	const u8 *pristine = READ_ONCE(j713_isp_d_pristine_data);
	void __iomem *code;
	size_t off;
	u8 *bounce;
	u32 crc, changed = 0;

	if (!pristine)
		return -ENODEV;
	bounce = kmalloc(SZ_16K, GFP_KERNEL);
	code = ioremap_wc(D_CODE_PHYS, D_CODE_SIZE);
	if (!bounce || !code) {
		kfree(bounce);
		if (code)
			iounmap(code);
		return -ENOMEM;
	}
	crc = d_crc_io(code, D_CODE_SIZE, bounce);
	iounmap(code);
	for (off = 0; off < D_DATA_SIZE; off += SZ_16K) {
		memcpy_fromio(bounce, cam->data + off, SZ_16K);
		if (memcmp(bounce, pristine + off, SZ_16K))
			changed++;
	}
	kfree(bounce);
	dev_info(cam->dev, "firmware image: code CRC=%08x expected=%08x changed data pages=%u\n",
		 crc, j713_isp_d_code_crc, changed);
	if (!j713_cam_firmware_qualified(crc) || crc != j713_isp_d_code_crc)
		return -EILSEQ;
	if (changed && !restore)
		return -EBUSY;
	if (changed) {
		memcpy_toio(cam->data, pristine, D_DATA_SIZE);
		/* Finish the write-combined restore before RUN. */
		wmb();
	}
	return 0;
}

/* Per 2 MiB step: the 1080p claim, then the 82f9fd25 claim at the same start. */
static int cam_claim(struct cam_device *cam)
{
	unsigned long step = SZ_2M / PAGE_SIZE;
	unsigned long count = CAM_IDENTITY_SIZE / PAGE_SIZE + 2;
	unsigned long large = CAM_IDENTITY_MAX / PAGE_SIZE + 2;
	unsigned long pfn = ALIGN(PFN_UP(CAM_VM_BASE + CAM_CHANNEL_MAP_END), step);
	unsigned long end = PFN_DOWN(CAM_VM_BASE + CAM_SHARED_VA_END), last = end - count;
	unsigned long i;
	int ret;

	for (; pfn <= last; pfn += step) {
		if (pfn + large <= end &&
		    !alloc_contig_range(pfn, pfn + large, ACR_FLAGS_NONE, GFP_KERNEL)) {
			count = large;
			break;
		}
		if (!alloc_contig_range(pfn, pfn + count, ACR_FLAGS_NONE, GFP_KERNEL))
			break;
	}
	if (pfn > last) {
		dev_err(cam->dev, "no free %lu-page identity claim in proven shared-VA bounds\n", count);
		return -ENOMEM;
	}
	cam->capture_span = count == large ? CAM_CAPTURE_MAX : CAM_CAPTURE_SIZE;
	cam->claim_pages = pfn_to_page(pfn);
	cam->claim_count = count;
	cam->window_base = PFN_PHYS(pfn + 1);
	cam->arena_base = cam->window_base + CAM_EXTRA_SIZE;
	cam->capture_base = cam->arena_base + CAM_ARENA_SIZE;
	ret = cam_identity_alias(cam);
	if (ret)
		return ret;
	/* Once the alias exists even initialization must avoid the linear map. */
	memset(cam->identity + PAGE_SIZE, 0, CAM_IDENTITY_SPAN(cam));
	for (i = 0; i < count; i += count - 1) {
		u64 *words = cam->identity + i * PAGE_SIZE;
		unsigned int w;

		for (w = 0; w < PAGE_SIZE / sizeof(*words); w++)
			words[w] = CAM_GUARD_PATTERN(i);
	}
	dma_wmb();
	return 0;
}

void cam_guard_audit(struct cam_device *cam)
{
	unsigned long i;

	dma_rmb();
	for (i = 0; i < cam->claim_count; i += cam->claim_count - 1) {
		const u64 *words = cam->identity + i * PAGE_SIZE;
		unsigned int w;

		for (w = 0; w < PAGE_SIZE / sizeof(*words); w++) {
			if (words[w] != CAM_GUARD_PATTERN(i)) {
				cam_fail(cam, "identity guard page changed");
				return;
			}
		}
	}
}

static int cam_map_firmware(struct cam_device *cam)
{
	phys_addr_t phys;
	size_t offset;
	int ret;

	if (region_intersects(D_CODE_PHYS, D_CODE_SIZE, IORESOURCE_SYSTEM_RAM,
			      IORES_DESC_NONE) != REGION_DISJOINT ||
	    region_intersects(D_DATA_PHYS, D_DATA_SIZE, IORESOURCE_SYSTEM_RAM,
			      IORES_DESC_NONE) != REGION_DISJOINT)
		return -EBUSY;
	cam->original = iommu_get_domain_for_dev(cam->dev);
	if (!cam->original || !(cam->original->type & __IOMMU_DOMAIN_DMA_API))
		return -ENODEV;
	ret = dma_set_mask_and_coherent(cam->dev, DMA_BIT_MASK(42));
	if (ret)
		return ret;
	ret = cam_claim(cam);
	if (ret)
		return ret;
	cam->ram = dma_alloc_coherent(cam->dev, cam->ram_size, &cam->ram_dma, GFP_KERNEL);
	if (!cam->ram)
		return -ENOMEM;
	cam->domain = iommu_paging_domain_alloc(cam->dev);
	if (IS_ERR(cam->domain)) {
		ret = PTR_ERR(cam->domain);
		cam->domain = NULL;
		return ret;
	}
	if (!(cam->domain->pgsize_bitmap & PAGE_SIZE) ||
	    cam->domain->geometry.aperture_start > CAM_VM_BASE ||
	    cam->domain->geometry.aperture_end < CAM_VM_BASE + cam->map_end - 1 ||
	    cam->domain->geometry.aperture_end < cam->window_base + CAM_IDENTITY_SPAN(cam) - 1)
		return -ERANGE;
	ret = iommu_map(cam->domain, CAM_VM_BASE, D_CODE_PHYS, D_CODE_SIZE, D_PROT, GFP_KERNEL);
	if (ret)
		return ret;
	cam->mapped = D_CODE_SIZE;
	ret = iommu_map(cam->domain, CAM_VM_BASE + D_CODE_SIZE, D_DATA_PHYS,
			D_DATA_SIZE, D_PROT, GFP_KERNEL);
	if (ret)
		return ret;
	cam->mapped += D_DATA_SIZE;
	for (offset = 0; offset < cam->ram_size; offset += PAGE_SIZE) {
		phys = iommu_iova_to_phys(cam->original, cam->ram_dma + offset);
		if (!phys || !IS_ALIGNED(phys, PAGE_SIZE) || phys != cam_backing_phys(cam, offset)) {
			cam->backing_unsafe = true;
			return -EFAULT;
		}
		ret = iommu_map(cam->domain, CAM_VM_BASE + cam->mapped, phys, PAGE_SIZE,
				D_PROT, GFP_KERNEL);
		if (ret)
			return ret;
		cam->mapped += PAGE_SIZE;
	}
	for (offset = 0; offset < cam->map_end; offset += PAGE_SIZE) {
		phys = cam_expected_phys(cam, offset);
		if (!phys || iommu_iova_to_phys(cam->domain, CAM_VM_BASE + offset) != phys)
			return -EFAULT;
	}
	ret = iommu_map(cam->domain, cam->window_base, cam->window_base,
			CAM_IDENTITY_SPAN(cam), D_PROT, GFP_KERNEL);
	if (ret)
		return ret;
	cam->identity_mapped = CAM_IDENTITY_SPAN(cam);
	for (offset = 0; offset < CAM_IDENTITY_SPAN(cam); offset += PAGE_SIZE)
		if (iommu_iova_to_phys(cam->domain, cam->window_base + offset) !=
		    cam->window_base + offset)
			return -EFAULT;
	memset(cam->ram, 0, cam->ram_size);
	ret = cam_map_mmio(cam);
	if (ret)
		return ret;
	ret = iommu_attach_device(cam->domain, cam->dev);
	if (!ret)
		cam->attached = true;
	return ret;
}

bool cam_memory_release(struct cam_device *cam)
{
	if (WARN_ON(cam->firmware_started))
		return false;
	/* Preserve Stage D's quarantine if backing ownership itself is unsafe. */
	if (cam->backing_unsafe) {
		__module_get(THIS_MODULE);
		cam_fail(cam, "CPU/DART backing mismatch; resources retained");
		return false;
	}
	if (cam->rtk) {
		apple_rtkit_free(cam->rtk);
		cam->rtk = NULL;
	}
	if (cam->lifecycle)
		destroy_workqueue(cam->lifecycle);
	if (cam->attached) {
		iommu_detach_device(cam->domain, cam->dev);
		/* As in Stage D, never free backing through an unconfirmed domain. */
		if (WARN_ON(iommu_get_domain_for_dev(cam->dev) != cam->original)) {
			__module_get(THIS_MODULE);
			cam_fail(cam, "original DMA domain was not restored; resources retained");
			return false;
		}
		cam->attached = false;
	}
	if (cam->domain) {
		while (cam->mmio_mapped) {
			const struct d_mmio_window *w = &d_mmio_windows[--cam->mmio_mapped];

			iommu_unmap(cam->domain, w->base, w->size);
		}
		if (cam->identity_mapped)
			iommu_unmap(cam->domain, cam->window_base, cam->identity_mapped);
		if (cam->mapped)
			iommu_unmap(cam->domain, CAM_VM_BASE, cam->mapped);
		iommu_domain_free(cam->domain);
	}
	if (cam->ram)
		dma_free_coherent(cam->dev, cam->ram_size, cam->ram, cam->ram_dma);
	if (cam->identity)
		vunmap(cam->identity);
	if (cam->claim_pages)
		free_contig_range(page_to_pfn(cam->claim_pages), cam->claim_count);
	if (cam->data)
		iounmap(cam->data);
	if (cam->gpio)
		iounmap(cam->gpio);
	if (cam->cpu_page)
		iounmap(cam->cpu_page);
	return true;
}

static int cam_cpu_irq_init(struct cam_device *cam)
{
	unsigned int i;

	/* This runs once per firmware start; keep the first mapping. */
	if (!cam->cpu_page)
		cam->cpu_page = ioremap(0x48d600000ULL, SZ_4K);
	if (!cam->cpu_page)
		return -ENOMEM;
	for (i = 0; i < 4; i++)
		writel(0xffffffff, cam->cpu_page + 0xa00 + 4 * i);
	for (i = 0; i < 8; i++) {
		readl(cam->cpu_page + 0x818);
		readl(cam->cpu_page + 0x81c);
	}
	msleep(50);
	return 0;
}

static int cam_ipc_bootstrap(struct cam_device *cam)
{
	void *ipc = cam_pointer(cam, CAM_IPC_BASE, CAM_IPC_SIZE);
	struct cam_bootargs *args = ipc + CAM_ARGS_OFFSET;
	struct cam_channel channel;
	u32 gpio[8], value;
	u64 table;
	unsigned int i;
	int ret;

	ret = readl_poll_timeout(cam->gpio + 0x1c, value, value == 0x8042006, 1000, 1000000);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(gpio); i++)
		gpio[i] = readl(cam->gpio + 4 * i);
	if (gpio[0] != CAM_CHANNELS || gpio[1] != CAM_IPC_QUEUE_SIZE || gpio[2] != 2 ||
	    gpio[3] != CAM_EXTRA_SIZE || gpio[4] || gpio[5] != 0x00100090 || gpio[6])
		return -EPROTO;
	memset(args, 0, sizeof(*args));
	args->ipc_iova = cpu_to_le64(CAM_IPC_BASE);
	args->shared_base = cpu_to_le64(0x08000000);
	args->shared_size = cpu_to_le64(0xee000000);
	args->extra_iova = cpu_to_le64(cam->window_base);
	args->extra_size = cpu_to_le64(CAM_EXTRA_SIZE);
	args->platform_id = cpu_to_le32(CAM_PLATFORM_ID);
	args->log_iova = cpu_to_le64(CAM_PRINT_BASE);
	args->log_size = cpu_to_le64(CAM_PRINT_SIZE);
	args->log_entry_size = cpu_to_le64(CAM_PRINT_RECORD_SIZE);
	args->ipc_size = cpu_to_le64(CAM_IPC_QUEUE_SIZE + 1);
	args->descriptor_words = cpu_to_le32(0x40);
	args->descriptor[0xb0] = 1;
	dma_wmb();
	writel(lower_32_bits(CAM_ARGS_IOVA), cam->gpio);
	writel(upper_32_bits(CAM_ARGS_IOVA), cam->gpio + 4);
	/* Both pointer halves precede the ownership handoff. */
	wmb();
	writel(~0x8042006U, cam->gpio + 0x1c);
	ret = readl_poll_timeout(cam->gpio + 0x1c, value, value == 0x8042006, 1000, 2000000);
	if (ret)
		return ret;
	table = readl(cam->gpio) | (u64)readl(cam->gpio + 4) << 32;
	if (table != CAM_IPC_BASE)
		return -ERANGE;
	dma_rmb();
	for (i = 0; i < CAM_CHANNELS; i++) {
		memcpy(&channel, ipc + i * sizeof(channel), sizeof(channel));
		if (!cam_channel_valid(&channel, i))
			return -EPROTO;
	}
	/* Pair with the receive worker before any endpoint can deliver. */
	smp_store_release(&cam->service_active, true);
	for (i = 0; i < CAM_CHANNELS; i++) {
		ret = apple_rtkit_start_ep(cam->rtk, 0x20 + i);
		if (ret)
			return ret;
	}
	writel(0x8042006, cam->gpio + 0xc);
	ret = readl_poll_timeout(cam->gpio + 0xc, value, !value, 1000, 2000000);
	if (ret)
		return ret;
	if (!cam_wait(cam, &cam->ready, 5000, false))
		return -ETIMEDOUT;
	return cam_boot_queries(cam);
}

/* Stage D's d_rearm after its own power-stop, minus the diagnostic reads. */
static int cam_rearm(struct cam_device *cam)
{
	unsigned int off_ms = READ_ONCE(restart_off_ms);
	unsigned int settle_ms = READ_ONCE(restart_settle_ms);
	u64 deadline, now;
	bool cycled;
	int ret;

	if (cam->rtk || cam->thread || !cam->poweroff_ns)
		return -EIO;
	cam_guard_audit(cam);
	if (cam_dead(cam))
		return -EIO;
	cam_reset_memory(cam);
	deadline = cam->poweroff_ns + (u64)off_ms * NSEC_PER_MSEC;
	while ((now = ktime_get_ns()) < deadline)
		msleep(DIV_ROUND_UP_ULL(deadline - now, NSEC_PER_MSEC));
	dev_info(cam->dev, "re-arm: off %llu ms (minimum %u), on-settle %u ms\n",
		 (now - cam->poweroff_ns) / NSEC_PER_MSEC, off_ms, settle_ms);
	ret = cam_take_power(cam, &cycled);
	if (!ret)
		ret = cam_check_power(cam->dev);
	if (!ret)
		ret = cam_check_cpu(cam, cycled);
	if (ret) {
		cam_fail(cam, "ISP power-up or CPU check failed at re-arm");
		return ret;
	}
	if (settle_ms)
		msleep(settle_ms);
	cam->rtk = apple_rtkit_init(cam->dev, cam, "rtkit", 0, &cam_rtkit_ops);
	if (IS_ERR(cam->rtk)) {
		ret = PTR_ERR(cam->rtk);
		cam->rtk = NULL;
		cam_failf(cam, "RTKit init failed (%d) at re-arm", ret);
	}
	return ret;
}

int cam_boot(struct cam_device *cam)
{
	u64 start;
	u32 a2i, i2a;
	int ret;

	lockdep_assert_held(&cam->fw_lock);
	if (cam->fw_starts) {
		ret = cam_rearm(cam);
		if (ret)
			goto fail;
	}
	start = ktime_get_ns();
	ret = cam_check_image(cam, true);
	if (ret)
		goto fail;
	a2i = readl(cam->mbox->regs + 0x110);
	i2a = readl(cam->mbox->regs + 0x114);
	if (!(a2i & BIT(17)) || !(i2a & BIT(17))) {
		ret = -EBUSY;
		goto fail;
	}
	if (!cam->gpio)
		cam->gpio = ioremap(0x48e9241d0ULL, 0x20);
	if (!cam->gpio) {
		ret = -ENOMEM;
		goto fail;
	}
	ret = cam_cpu_irq_init(cam);
	if (ret)
		goto fail;
	if (!cam->firmware_started)
		__module_get(THIS_MODULE);
	WRITE_ONCE(cam->firmware_started, true);
	WRITE_ONCE(firmware_started, true);
	WRITE_ONCE(cam->firmware_running, true);
	cam->fw_sessions = 0;
	WRITE_ONCE(fw_starts, ++cam->fw_starts);
	dma_wmb();
	writel(cam->control | CAM_RUN, cam->cpu + CAM_CONTROL);
	/* The scratch survives a power-stop. Clear it before RTKit wake. */
	writel(0, cam->gpio + 0x1c);
	if (readl(cam->gpio + 0x1c)) {
		ret = -EBUSY;
		goto fail;
	}
	ret = apple_rtkit_wake(cam->rtk);
	if (ret)
		cam_failf(cam, "RTKit wake failed (%d) at firmware boot", ret);
	if (!ret && !apple_rtkit_is_running(cam->rtk))
		ret = -EIO;
	if (!ret)
		ret = cam_ipc_bootstrap(cam);
	if (!ret && !cam_dead(cam)) {
		dev_info(cam->dev, "firmware booted: sensor=%#x presets=%u in %llums (start %u)\n",
			 cam->sensor_version, cam->presets, (ktime_get_ns() - start) / NSEC_PER_MSEC,
			 cam->fw_starts);
		return 0;
	}
fail:
	cam_failf(cam, "firmware boot failed (%d)", ret);
	cam_dead_dump(cam);
	return ret ? ret : -EIO;
}

/* Stage D's d_stop with power_stop=1. Any failure is DEAD, and DEAD never
 * powers domains down: whatever is left stays pinned until power-off.
 */
int cam_fw_stop(struct cam_device *cam)
{
	void __iomem *pmgr;
	int ret;

	lockdep_assert_held(&cam->fw_lock);
	if (!cam->firmware_running || cam_dead(cam) || READ_ONCE(cam->shutting_down))
		return 0;
	cam_print_drain(cam);
	if (cam_dead(cam))
		return -EIO;
	ret = apple_rtkit_shutdown(cam->rtk);
	cam_print_drain(cam);
	if (ret || cam_dead(cam)) {
		cam_failf(cam, "RTKit sleep failed (%d) before the power-stop", ret);
		cam_dead_dump(cam);
		return ret ? ret : -EIO;
	}
	/* Nothing is sent from here on; the next start needs a fresh boot. */
	WRITE_ONCE(cam->firmware_running, false);
	writel(cam->control, cam->cpu + CAM_CONTROL);
	/* No mailbox IRQ or register access may follow the power-down. */
	apple_rtkit_free(cam->rtk);
	cam->rtk = NULL;
	WRITE_ONCE(cam->service_active, false);
	/* Free drains the old worker; its last callback may have latched DEAD. */
	if (cam_dead(cam)) {
		cam_dead_dump(cam);
		return -EIO;
	}
	pmgr = ioremap(D_PMGR, D_PMGR_SIZE);
	if (!pmgr) {
		cam_fail(cam, "PMGR could not be mapped for the power-stop");
		return -ENOMEM;
	}
	ret = d_power_down(pmgr);
	if (!ret && !d_power_at_boot_state(pmgr))
		ret = -EIO;
	iounmap(pmgr);
	if (ret) {
		cam_fail(cam, "ISP sub-domains did not all power down");
		return ret;
	}
	cam->poweroff_ns = ktime_get_ns();
	dev_info(cam->dev, "firmware start %u stopped: RTKit asleep, CPU and ISP sub-domains off; DARTs stay in ISP_SYS\n",
		 cam->fw_starts);
	return 0;
}

static bool cam_overlay_node(struct device_node *node)
{
	struct device_node *ours = of_find_node_by_path(D_NODE);
	bool same = ours && ours == node;

	of_node_put(ours);
	return same;
}

static int cam_probe(struct platform_device *pdev)
{
	struct cam_device *cam;
	int ret;

	if (!d_machine_ok() || !cam_overlay_node(pdev->dev.of_node))
		return -ENODEV;
	ret = cam_check_iommus(&pdev->dev);
	if (ret)
		return ret;
	ret = cam_check_power(&pdev->dev);
	if (ret)
		return ret;
	cam = kzalloc_obj(*cam);
	if (!cam)
		return -ENOMEM;
	cam->dev = get_device(&pdev->dev);
	mutex_init(&cam->video_lock);
	mutex_init(&cam->fw_lock);
	spin_lock_init(&cam->service_lock);
	spin_lock_init(&cam->buffers_lock);
	INIT_LIST_HEAD(&cam->buffers);
	INIT_DELAYED_WORK(&cam->idle_work, cam_idle_work);
	INIT_WORK(&cam->lifecycle_work, cam_lifecycle_work);
	init_completion(&cam->reply);
	init_completion(&cam->ready);
	init_completion(&cam->frames);
	init_completion(&cam->stream_started);
	cam->state = CAM_PROBED;
	cam->auto_white_balance = true;
	cam->map_end = full_span ? CAM_CHANNEL_MAP_END : CAM_PRINT_BASE + CAM_PRINT_SIZE - CAM_VM_BASE;
	cam->ram_size = cam->map_end - CAM_FW_END;
	cam->print_cursor = CAM_PRINT_RECORD_SIZE;
	cam_geometry_init(&cam->geometry, out_height);
	cam->format = cam->geometry;
	cam->out_height = out_height;
	cam->cpu = cam_map_reg(pdev, "cpu", 0x48d600000ULL, 0x100);
	if (IS_ERR(cam->cpu)) {
		ret = PTR_ERR(cam->cpu);
		goto free;
	}
	cam->rvbar = cam_map_reg(pdev, "rvbar", 0x48d050000ULL, 0x40);
	if (IS_ERR(cam->rvbar)) {
		ret = PTR_ERR(cam->rvbar);
		goto free;
	}
	ret = cam_check_cpu(cam, false);
	if (ret)
		goto free;
	if (region_intersects(D_CODE_PHYS, D_CODE_SIZE, IORESOURCE_SYSTEM_RAM,
			      IORES_DESC_NONE) != REGION_DISJOINT ||
	    region_intersects(D_DATA_PHYS, D_DATA_SIZE, IORESOURCE_SYSTEM_RAM,
			      IORES_DESC_NONE) != REGION_DISJOINT) {
		ret = -EBUSY;
		goto free;
	}
	cam->data = ioremap_wc(D_DATA_PHYS, D_DATA_SIZE);
	if (!cam->data) {
		ret = -ENOMEM;
		goto free;
	}
	ret = cam_check_image(cam, false);
	if (ret)
		goto release;
	cam->arena.entries = kvcalloc(CAM_ARENA_ENTRIES, sizeof(*cam->arena.entries), GFP_KERNEL);
	if (!cam->arena.entries) {
		ret = -ENOMEM;
		goto release;
	}
	cam->arena.capacity = CAM_ARENA_ENTRIES;
	cam->arena.limit = CAM_ARENA_SIZE;
	ret = cam_map_firmware(cam);
	if (ret)
		goto release;
	cam->mbox = apple_mbox_get_byname(cam->dev, "rtkit");
	if (IS_ERR(cam->mbox)) {
		ret = PTR_ERR(cam->mbox);
		goto release;
	}
	/* Not freezable: .shutdown must be able to run a step at any time. */
	cam->lifecycle = alloc_ordered_workqueue("j713-cam-lifecycle", 0);
	if (!cam->lifecycle) {
		ret = -ENOMEM;
		goto release;
	}
	cam->rtk = apple_rtkit_init(cam->dev, cam, "rtkit", 0, &cam_rtkit_ops);
	if (IS_ERR(cam->rtk)) {
		ret = PTR_ERR(cam->rtk);
		cam->rtk = NULL;
		goto release;
	}
	mutex_lock(&cam->video_lock);
	cam_set_state(cam, CAM_PROBED);
	mutex_unlock(&cam->video_lock);
	ret = cam_v4l2_register(cam);
	if (ret)
		return ret; /* The V4L2 release callback owns cleanup from registration. */
	platform_set_drvdata(pdev, cam);
	dev_info(cam->dev, "probed: claim=%#llx..%#llx held=%zu+%zu bytes; %s NV12 1280x720%s\n",
		 cam->window_base - PAGE_SIZE, cam->window_base + CAM_IDENTITY_SPAN(cam) + PAGE_SIZE,
		 CAM_IDENTITY_SPAN(cam) + 2 * PAGE_SIZE, cam->ram_size, video_device_node_name(&cam->video),
		 cam->capture_span == CAM_CAPTURE_MAX ? ", 1920x1080" : "; no 1080p tail");
	return 0;
release:
	if (!cam_memory_release(cam))
		return ret;
free:
	kvfree(cam->arena.entries);
	put_device(cam->dev);
	kfree(cam);
	return ret;
}

static void cam_remove(struct platform_device *pdev)
{
	struct cam_device *cam = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&cam->idle_work);
	cam_v4l2_unregister(cam);
}

/* System shutdown's lifecycle step: the sensor stop, then RTKit sleep only. */
static int cam_fw_sleep(struct cam_device *cam)
{
	int ret = 0;

	lockdep_assert_held(&cam->fw_lock);
	cam_session_stop(cam);
	if (cam->firmware_running && !cam_dead(cam)) {
		ret = apple_rtkit_shutdown(cam->rtk);
		if (ret)
			cam_failf(cam, "RTKit sleep failed (%d) at shutdown", ret);
		else
			WRITE_ONCE(cam->firmware_running, false);
	}
	return ret;
}

static void cam_shutdown(struct platform_device *pdev)
{
	struct cam_device *cam = platform_get_drvdata(pdev);

	mutex_lock(&cam->video_lock);
	WRITE_ONCE(cam->shutting_down, true);
	cam_stream_join(cam);
	cam_lifecycle(cam, cam_fw_sleep);
	mutex_unlock(&cam->video_lock);
	cancel_delayed_work_sync(&cam->idle_work);
}

static int cam_suspend(struct device *dev)
{
	struct cam_device *cam = dev_get_drvdata(dev);

	int ret;

	mutex_lock(&cam->video_lock);
	ret = cam->firmware_started ? -EBUSY : 0;
	mutex_unlock(&cam->video_lock);
	return ret;
}

static const struct dev_pm_ops cam_pm_ops = {
	.suspend = cam_suspend,
	.freeze = cam_suspend,
	.poweroff = cam_suspend,
};

static const struct of_device_id cam_match[] = {
	{ .compatible = "omarchy,j713-isp-stage-d" },
	{ }
};

static struct platform_driver cam_driver = {
	.probe = cam_probe,
	.remove = cam_remove,
	.shutdown = cam_shutdown,
	.driver = {
		.name = "j713-isp-cam",
		.of_match_table = cam_match,
		.suppress_bind_attrs = true,
		.pm = &cam_pm_ops,
	},
};

static int __init cam_init(void)
{
	if (out_height != 720 && out_height != 1264)
		return -EINVAL;
	return platform_driver_register(&cam_driver);
}
module_init(cam_init);

static void __exit cam_exit(void)
{
	platform_driver_unregister(&cam_driver);
}
module_exit(cam_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("J713 V4L2 camera on the Stage D identity layout");
