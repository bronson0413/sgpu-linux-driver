/* SPDX-License-Identifier: GPL-2.0 */
/* Register and command layout of the SGPU device. See docs/hardware-spec.md. */
#ifndef SGPU_HW_H
#define SGPU_HW_H

#define SGPU_PCI_VENDOR		0x1234
#define SGPU_PCI_DEVICE		0x5347
#define SGPU_BAR_SIZE		4096

#define SGPU_ID_VALUE		0x53475055u	/* "SGPU" */
#define SGPU_VERSION_VALUE	1u

#define SGPU_NUM_CTX		16
#define SGPU_PAGE_SHIFT		12
#define SGPU_PAGE_SIZE		(1u << SGPU_PAGE_SHIFT)
#define SGPU_VA_PAGES		1024
#define SGPU_VA_SIZE		((unsigned long long)SGPU_VA_PAGES << SGPU_PAGE_SHIFT)
#define SGPU_MAX_XFER		(1u << 20)
#define SGPU_RING_MAX		4096

/* Registers (32-bit access only) */
#define SGPU_REG_ID		0x00	/* RO */
#define SGPU_REG_VERSION	0x04	/* RO */
#define SGPU_REG_STATUS		0x08	/* RO */
#define SGPU_REG_CTRL		0x0c	/* WO */
#define SGPU_REG_RING_BASE_LO	0x10
#define SGPU_REG_RING_BASE_HI	0x14
#define SGPU_REG_RING_SIZE	0x18	/* entries, power of two */
#define SGPU_REG_RING_HEAD	0x1c	/* RO, next entry the device will run */
#define SGPU_REG_RING_TAIL	0x20	/* writing rings the doorbell */
#define SGPU_REG_FENCE		0x24	/* last fence; writable while disabled */
#define SGPU_REG_IRQ_STATUS	0x28	/* write 1 to clear */
#define SGPU_REG_IRQ_MASK	0x2c
#define SGPU_REG_FAULT_CODE	0x30	/* RO */
#define SGPU_REG_FAULT_CTX	0x34	/* RO */
#define SGPU_REG_FAULT_VA_LO	0x38	/* RO */
#define SGPU_REG_FAULT_VA_HI	0x3c	/* RO */
#define SGPU_REG_CTX_TABLE_LO	0x40
#define SGPU_REG_CTX_TABLE_HI	0x44

#define SGPU_STATUS_ENABLED	(1u << 0)
#define SGPU_STATUS_BUSY	(1u << 1)
#define SGPU_STATUS_FAULTED	(1u << 2)
#define SGPU_STATUS_HUNG	(1u << 3)

#define SGPU_CTRL_ENABLE	(1u << 0)
#define SGPU_CTRL_RESET		(1u << 1)

#define SGPU_IRQ_FENCE		(1u << 0)
#define SGPU_IRQ_FAULT		(1u << 1)

#define SGPU_FAULT_NONE		0u
#define SGPU_FAULT_BAD_CMD	1u	/* unknown opcode or invalid length */
#define SGPU_FAULT_BAD_CTX	2u	/* context index or missing page table */
#define SGPU_FAULT_BAD_VA	3u	/* outside the context address space */
#define SGPU_FAULT_NOT_MAPPED	4u	/* page table entry not valid */
#define SGPU_FAULT_READ_ONLY	5u	/* write through a read-only entry */
#define SGPU_FAULT_DMA		6u	/* bus error on a DMA access */
#define SGPU_FAULT_BAD_RING	7u	/* doorbell value out of range */

#define SGPU_PTE_VALID		(1ull << 0)
#define SGPU_PTE_WRITE		(1ull << 1)
#define SGPU_PTE_ADDR_MASK	(~0xfffull)

#define SGPU_HW_OP_NOP		0u
#define SGPU_HW_OP_FILL		1u	/* byte pattern in src */
#define SGPU_HW_OP_COPY		2u
#define SGPU_HW_OP_HANG		0x7fu	/* test only: stop until reset */

#define SGPU_HW_CMD_SIZE	32

#endif
