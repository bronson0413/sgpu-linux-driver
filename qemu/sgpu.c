/*
 * SGPU: a small PCI command processor used to develop a GPU-style Linux driver.
 * It models a command ring, doorbell, per-context page tables, fences, MSI and
 * fault reporting. The programming model is in docs/hardware-spec.md.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/bswap.h"
#include "qemu/bitops.h"
#include "qemu/host-utils.h"
#include "qapi/error.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/msi.h"
#include "qom/object.h"
#include "sgpu_hw.h"

#define TYPE_SGPU "sgpu"
OBJECT_DECLARE_SIMPLE_TYPE(SgpuState, SGPU)

#define SGPU_EXEC_DELAY_NS	20000
#define SGPU_EXEC_BUDGET	32

struct SgpuState {
	PCIDevice parent_obj;
	MemoryRegion mmio;
	QEMUTimer *timer;

	uint32_t status;
	uint64_t ring_base;
	uint32_t ring_size;
	uint32_t head;
	uint32_t tail;
	uint32_t fence;
	uint32_t irq_status;
	uint32_t irq_mask;
	uint32_t fault_code;
	uint32_t fault_ctx;
	uint64_t fault_va;
	uint64_t ctx_table;
};

typedef struct QEMU_PACKED SgpuCmd {
	uint32_t opcode;
	uint32_t ctx;
	uint64_t src;
	uint64_t dst;
	uint32_t len;
	uint32_t fence;
} SgpuCmd;

QEMU_BUILD_BUG_ON(sizeof(SgpuCmd) != SGPU_HW_CMD_SIZE);

static void sgpu_notify(SgpuState *s)
{
	PCIDevice *pdev = PCI_DEVICE(s);

	if ((s->irq_status & s->irq_mask) && msi_enabled(pdev)) {
		msi_notify(pdev, 0);
	}
}

static void sgpu_raise(SgpuState *s, uint32_t bits)
{
	bool was_pending = s->irq_status & s->irq_mask;

	s->irq_status |= bits;
	if (!was_pending) {
		sgpu_notify(s);
	}
}

static bool sgpu_can_run(SgpuState *s)
{
	return (s->status & SGPU_STATUS_ENABLED) &&
	       !(s->status & (SGPU_STATUS_FAULTED | SGPU_STATUS_HUNG)) &&
	       s->ring_size && s->head != s->tail;
}

static void sgpu_kick(SgpuState *s)
{
	if (sgpu_can_run(s)) {
		timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
				    SGPU_EXEC_DELAY_NS);
	}
}

static void sgpu_fault(SgpuState *s, uint32_t code, uint32_t ctx, uint64_t va)
{
	s->status |= SGPU_STATUS_FAULTED;
	s->fault_code = code;
	s->fault_ctx = ctx;
	s->fault_va = va;
	sgpu_raise(s, SGPU_IRQ_FAULT);
}

static bool sgpu_translate(SgpuState *s, uint32_t ctx, uint64_t va, bool write,
			   hwaddr *pa)
{
	PCIDevice *pdev = PCI_DEVICE(s);
	uint64_t root, pte;

	if (ctx >= SGPU_NUM_CTX) {
		sgpu_fault(s, SGPU_FAULT_BAD_CTX, ctx, va);
		return false;
	}
	if (va >= SGPU_VA_SIZE) {
		sgpu_fault(s, SGPU_FAULT_BAD_VA, ctx, va);
		return false;
	}
	if (pci_dma_read(pdev, s->ctx_table + ctx * sizeof(root), &root,
			 sizeof(root))) {
		sgpu_fault(s, SGPU_FAULT_DMA, ctx, va);
		return false;
	}
	root = le64_to_cpu(root);
	if (!root) {
		sgpu_fault(s, SGPU_FAULT_BAD_CTX, ctx, va);
		return false;
	}
	if (pci_dma_read(pdev, root + (va >> SGPU_PAGE_SHIFT) * sizeof(pte), &pte,
			 sizeof(pte))) {
		sgpu_fault(s, SGPU_FAULT_DMA, ctx, va);
		return false;
	}
	pte = le64_to_cpu(pte);
	if (!(pte & SGPU_PTE_VALID)) {
		sgpu_fault(s, SGPU_FAULT_NOT_MAPPED, ctx, va);
		return false;
	}
	if (write && !(pte & SGPU_PTE_WRITE)) {
		sgpu_fault(s, SGPU_FAULT_READ_ONLY, ctx, va);
		return false;
	}
	*pa = (pte & SGPU_PTE_ADDR_MASK) | (va & (SGPU_PAGE_SIZE - 1));
	return true;
}

static bool sgpu_fill(SgpuState *s, uint32_t ctx, uint64_t dst, uint32_t len,
		      uint8_t value)
{
	uint8_t page[SGPU_PAGE_SIZE];
	hwaddr pa;

	memset(page, value, sizeof(page));
	while (len) {
		uint32_t n = MIN(len, SGPU_PAGE_SIZE - (dst & (SGPU_PAGE_SIZE - 1)));

		if (!sgpu_translate(s, ctx, dst, true, &pa)) {
			return false;
		}
		if (pci_dma_write(PCI_DEVICE(s), pa, page, n)) {
			sgpu_fault(s, SGPU_FAULT_DMA, ctx, dst);
			return false;
		}
		dst += n;
		len -= n;
	}
	return true;
}

static bool sgpu_copy(SgpuState *s, uint32_t ctx, uint64_t src, uint64_t dst,
		      uint32_t len)
{
	uint8_t page[SGPU_PAGE_SIZE];
	hwaddr spa, dpa;

	while (len) {
		uint32_t n = MIN(len, SGPU_PAGE_SIZE - (src & (SGPU_PAGE_SIZE - 1)));

		n = MIN(n, SGPU_PAGE_SIZE - (dst & (SGPU_PAGE_SIZE - 1)));
		if (!sgpu_translate(s, ctx, src, false, &spa) ||
		    !sgpu_translate(s, ctx, dst, true, &dpa)) {
			return false;
		}
		if (pci_dma_read(PCI_DEVICE(s), spa, page, n) ||
		    pci_dma_write(PCI_DEVICE(s), dpa, page, n)) {
			sgpu_fault(s, SGPU_FAULT_DMA, ctx, src);
			return false;
		}
		src += n;
		dst += n;
		len -= n;
	}
	return true;
}

static bool sgpu_exec_one(SgpuState *s)
{
	SgpuCmd cmd;
	uint32_t opcode, ctx, len;
	uint64_t src, dst;
	bool ok = true;

	if (pci_dma_read(PCI_DEVICE(s), s->ring_base + s->head * sizeof(cmd), &cmd,
			 sizeof(cmd))) {
		sgpu_fault(s, SGPU_FAULT_DMA, 0, 0);
		return false;
	}
	opcode = le32_to_cpu(cmd.opcode);
	ctx = le32_to_cpu(cmd.ctx);
	len = le32_to_cpu(cmd.len);
	src = le64_to_cpu(cmd.src);
	dst = le64_to_cpu(cmd.dst);

	switch (opcode) {
	case SGPU_HW_OP_NOP:
		break;
	case SGPU_HW_OP_FILL:
	case SGPU_HW_OP_COPY:
		if (!len || len > SGPU_MAX_XFER) {
			sgpu_fault(s, SGPU_FAULT_BAD_CMD, ctx, 0);
			return false;
		}
		ok = opcode == SGPU_HW_OP_FILL ? sgpu_fill(s, ctx, dst, len, src) :
						 sgpu_copy(s, ctx, src, dst, len);
		break;
	case SGPU_HW_OP_HANG:
		s->status |= SGPU_STATUS_HUNG;
		return false;
	default:
		sgpu_fault(s, SGPU_FAULT_BAD_CMD, ctx, 0);
		return false;
	}
	if (!ok) {
		return false;
	}

	s->head = (s->head + 1) & (s->ring_size - 1);
	s->fence = le32_to_cpu(cmd.fence);
	sgpu_raise(s, SGPU_IRQ_FENCE);
	return true;
}

static void sgpu_exec(void *opaque)
{
	SgpuState *s = opaque;
	int budget = SGPU_EXEC_BUDGET;

	while (sgpu_can_run(s) && budget--) {
		if (!sgpu_exec_one(s)) {
			return;
		}
	}
	sgpu_kick(s);
}

static void sgpu_do_reset(SgpuState *s)
{
	timer_del(s->timer);
	s->status = 0;
	s->ring_base = 0;
	s->ring_size = 0;
	s->head = 0;
	s->tail = 0;
	s->fence = 0;
	s->irq_status = 0;
	s->irq_mask = 0;
	s->fault_code = SGPU_FAULT_NONE;
	s->fault_ctx = 0;
	s->fault_va = 0;
	s->ctx_table = 0;
}

static uint64_t sgpu_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
	SgpuState *s = opaque;

	switch (addr) {
	case SGPU_REG_ID:
		return SGPU_ID_VALUE;
	case SGPU_REG_VERSION:
		return SGPU_VERSION_VALUE;
	case SGPU_REG_STATUS:
		return s->status | (sgpu_can_run(s) ? SGPU_STATUS_BUSY : 0);
	case SGPU_REG_RING_BASE_LO:
		return (uint32_t)s->ring_base;
	case SGPU_REG_RING_BASE_HI:
		return s->ring_base >> 32;
	case SGPU_REG_RING_SIZE:
		return s->ring_size;
	case SGPU_REG_RING_HEAD:
		return s->head;
	case SGPU_REG_RING_TAIL:
		return s->tail;
	case SGPU_REG_FENCE:
		return s->fence;
	case SGPU_REG_IRQ_STATUS:
		return s->irq_status;
	case SGPU_REG_IRQ_MASK:
		return s->irq_mask;
	case SGPU_REG_FAULT_CODE:
		return s->fault_code;
	case SGPU_REG_FAULT_CTX:
		return s->fault_ctx;
	case SGPU_REG_FAULT_VA_LO:
		return (uint32_t)s->fault_va;
	case SGPU_REG_FAULT_VA_HI:
		return s->fault_va >> 32;
	case SGPU_REG_CTX_TABLE_LO:
		return (uint32_t)s->ctx_table;
	case SGPU_REG_CTX_TABLE_HI:
		return s->ctx_table >> 32;
	default:
		return 0;
	}
}

static void sgpu_mmio_write(void *opaque, hwaddr addr, uint64_t val,
			    unsigned size)
{
	SgpuState *s = opaque;
	uint32_t v = val;

	switch (addr) {
	case SGPU_REG_CTRL:
		if (v & SGPU_CTRL_RESET) {
			sgpu_do_reset(s);
		}
		if (v & SGPU_CTRL_ENABLE) {
			s->status |= SGPU_STATUS_ENABLED;
		} else {
			s->status &= ~SGPU_STATUS_ENABLED;
		}
		sgpu_kick(s);
		break;
	case SGPU_REG_RING_BASE_LO:
		s->ring_base = deposit64(s->ring_base, 0, 32, v);
		break;
	case SGPU_REG_RING_BASE_HI:
		s->ring_base = deposit64(s->ring_base, 32, 32, v);
		break;
	case SGPU_REG_RING_SIZE:
		s->ring_size = (is_power_of_2(v) && v <= SGPU_RING_MAX) ? v : 0;
		s->head = 0;
		s->tail = 0;
		break;
	case SGPU_REG_RING_TAIL:
		if (!s->ring_size || v >= s->ring_size) {
			sgpu_fault(s, SGPU_FAULT_BAD_RING, 0, v);
			break;
		}
		s->tail = v;
		sgpu_kick(s);
		break;
	case SGPU_REG_FENCE:
		if (!(s->status & SGPU_STATUS_ENABLED)) {
			s->fence = v;
		}
		break;
	case SGPU_REG_IRQ_STATUS:
		s->irq_status &= ~v;
		sgpu_notify(s);
		break;
	case SGPU_REG_IRQ_MASK:
		s->irq_mask = v;
		sgpu_notify(s);
		break;
	case SGPU_REG_CTX_TABLE_LO:
		s->ctx_table = deposit64(s->ctx_table, 0, 32, v);
		break;
	case SGPU_REG_CTX_TABLE_HI:
		s->ctx_table = deposit64(s->ctx_table, 32, 32, v);
		break;
	default:
		break;
	}
}

static const MemoryRegionOps sgpu_mmio_ops = {
	.read = sgpu_mmio_read,
	.write = sgpu_mmio_write,
	.endianness = DEVICE_LITTLE_ENDIAN,
	.valid = { .min_access_size = 4, .max_access_size = 4 },
	.impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void sgpu_realize(PCIDevice *pdev, Error **errp)
{
	SgpuState *s = SGPU(pdev);

	s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, sgpu_exec, s);
	memory_region_init_io(&s->mmio, OBJECT(s), &sgpu_mmio_ops, s, "sgpu-mmio",
			      SGPU_BAR_SIZE);
	pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mmio);
	if (msi_init(pdev, 0, 1, true, false, errp)) {
		timer_free(s->timer);
		s->timer = NULL;
	}
}

static void sgpu_exit(PCIDevice *pdev)
{
	SgpuState *s = SGPU(pdev);

	timer_free(s->timer);
	msi_uninit(pdev);
}

static void sgpu_reset(DeviceState *dev)
{
	sgpu_do_reset(SGPU(dev));
}

static void sgpu_class_init(ObjectClass *klass, const void *data)
{
	DeviceClass *dc = DEVICE_CLASS(klass);
	PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

	k->realize = sgpu_realize;
	k->exit = sgpu_exit;
	k->vendor_id = SGPU_PCI_VENDOR;
	k->device_id = SGPU_PCI_DEVICE;
	k->revision = 1;
	k->class_id = PCI_CLASS_DISPLAY_OTHER;
	device_class_set_legacy_reset(dc, sgpu_reset);
	set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo sgpu_info = {
	.name = TYPE_SGPU,
	.parent = TYPE_PCI_DEVICE,
	.instance_size = sizeof(SgpuState),
	.class_init = sgpu_class_init,
	.interfaces = (const InterfaceInfo[]) {
		{ INTERFACE_CONVENTIONAL_PCI_DEVICE },
		{ },
	},
};

static void sgpu_register_types(void)
{
	type_register_static(&sgpu_info);
}

type_init(sgpu_register_types)
