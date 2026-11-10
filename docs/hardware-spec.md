# SGPU hardware specification

SGPU is a virtual PCI command processor implemented as a QEMU device model
(`qemu/sgpu.c`). It is not a real GPU and has no shader or display engine. It
models the parts of a GPU that a kernel driver has to manage: a command ring,
a doorbell, per-context page tables, fences, interrupts and fault reporting.

## PCI identity

| Item | Value |
|---|---|
| Vendor / device | `0x1234` / `0x5347` |
| Class | Display, other |
| BAR0 | 4 KiB memory-mapped registers, 32-bit access only |
| Interrupt | One MSI vector |

## Registers

| Offset | Name | Access | Description |
|---|---|---|---|
| 0x00 | ID | RO | `0x53475055` |
| 0x04 | VERSION | RO | `1` |
| 0x08 | STATUS | RO | bit 0 enabled, bit 1 busy, bit 2 faulted, bit 3 hung |
| 0x0c | CTRL | WO | bit 0 enable, bit 1 reset |
| 0x10, 0x14 | RING_BASE | RW | DMA address of the command ring |
| 0x18 | RING_SIZE | RW | Entries, a power of two, at most 4096. Resets head and tail |
| 0x1c | RING_HEAD | RO | Next entry the device will run |
| 0x20 | RING_TAIL | RW | Doorbell. Writing it starts processing |
| 0x24 | FENCE | RW | Last completed fence. Writable only while disabled |
| 0x28 | IRQ_STATUS | W1C | bit 0 fence, bit 1 fault |
| 0x2c | IRQ_MASK | RW | Enabled interrupt sources |
| 0x30 | FAULT_CODE | RO | See below |
| 0x34 | FAULT_CTX | RO | Context that faulted |
| 0x38, 0x3c | FAULT_VA | RO | Faulting virtual address |
| 0x40, 0x44 | CTX_TABLE | RW | DMA address of 16 little-endian 64-bit page table roots |

## Command format

Ring entries are 32 bytes and little-endian.

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | opcode |
| 4 | 4 | context index |
| 8 | 8 | source VA, or the fill byte for FILL |
| 16 | 8 | destination VA |
| 24 | 4 | length in bytes, 1 to 1 MiB |
| 28 | 4 | fence value written after completion |

| Opcode | Name | Effect |
|---|---|---|
| 0 | NOP | Advances the fence only |
| 1 | FILL | Writes the byte to `dst[len]` |
| 2 | COPY | Copies `src[len]` to `dst[len]` |
| 0x7f | HANG | Test only. Stops the device until reset, with no interrupt |

## Address translation

Each context has a flat table of 1024 little-endian 64-bit entries, covering a
4 MiB virtual address space in 4 KiB pages. An entry is the page's DMA address
with bit 0 meaning valid and bit 1 meaning writable. The device walks the table
through DMA for every page it touches and has no translation cache, so clearing
an entry takes effect for the next access.

## Faults

Any invalid command or translation stops the device, latches the fault
registers and raises the fault interrupt. Processing stays stopped until the
driver writes the reset bit.

| Code | Meaning |
|---|---|
| 1 | Unknown opcode or invalid length |
| 2 | Invalid context index or empty page table root |
| 3 | Address outside the context address space |
| 4 | Page table entry not valid |
| 5 | Write through a read-only entry |
| 6 | Bus error on a DMA access |
| 7 | Doorbell value out of range |

## Timing

Doorbell writes arm a timer 20 µs of guest time ahead. Each timer run executes
at most 32 commands and re-arms itself, so completion is asynchronous and the
driver must be correct under real interrupt ordering.
