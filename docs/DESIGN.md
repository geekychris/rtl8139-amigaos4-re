# rtl8139re — design notes

## Why this driver's structure is simpler than an e1000 / virtio-net
## driver, even though they all target the same OS

The RTL8139 uses **register-based DMA**: the transmit-buffer physical
address is written to a NIC I/O register (TSAD0-3), and the NIC pulls
from that address directly. e1000 and virtio-net use **descriptor-based
DMA**: the driver writes a 16-byte descriptor into guest RAM containing
`{addr, len, flags, ...}`, and the NIC DMA-reads the descriptor from
RAM before following the address to fetch the payload.

Those two data paths look similar at 10,000 ft but differ in three
concrete ways that matter to the driver:

### 1. Where the DMA address lives determines who byte-swaps it

Register-based (rtl8139):
```c
pciDevice->OutLong(bar_io + RTL_TSAD0, tx_buffer_phys);
```
The OS4 PCI stack knows PCI I/O is little-endian and byte-swaps the
`ULONG` for us on the way to the port. Guest driver ships a native
`ULONG` value; nothing to think about.

Descriptor-based (e1000, virtio-net):
```c
poke_le32(&desc[slot].buffer_addr, tx_buffer_phys);   // stwbrx
```
The address goes into guest RAM. The guest CPU stores in native
byte order (BE on PPC 460EX). The NIC DMA-reads that RAM and
interprets it as little-endian. If the driver forgets to `stwbrx`
(byte-reverse store) or does a plain native store, the NIC reads
garbage.

Symptom when this bug hits: descriptor addr_lo bytes in RAM look
sensible in `xp` output, but the NIC DMAs from a completely wrong
address, and the outbound packet is empty or drops silently. We hit
this on virtnet — `vio_le32_put` was doing a native store; changing
to `stwbrx` fixed the descriptor byte layout.

### 2. Descriptor-based requires cache coherency for the descriptor too

Register-based: the address doesn't sit in RAM. Only the payload buffer
does. One `CacheClearE(tx_buffer, len, CACRF_ClearD)` before the
doorbell ensures the CPU's writes reach RAM before the NIC's DMA reads
begin.

Descriptor-based: BOTH the payload buffer AND the descriptor sit in
RAM. Cache-flush both. Miss the descriptor flush and the NIC reads
a stale (or all-zero) descriptor and never fetches the payload.

### 3. StartDMA / GetDMAList discipline

`IExec->StartDMA(buf, size, flags)` is OS4's "pin this memory + return
DMA-visible address" primitive. It doesn't nest — a second `StartDMA`
on a buffer that hasn't been `EndDMA`d returns 0.

`AllocMem(size, MEMF_KICK|MEMF_CLEAR)` on OS4/sam460ex returns memory
that's already identity-mapped for PCI DMA. You can pass the pointer
straight to the NIC without any `StartDMA` dance.

Shipping rtl8139 uses `AllocMem` + `AllocMem`-style patterns; it
never touches `StartDMA`. This driver follows the same path.

virte1000 and virtnet inherited a `v1000_dma_phys` / `vn_dma_phys`
helper that called `StartDMA` at every request. Init called it once
on tx_scratch and never `EndDMA`'d. Then CMD_WRITE called it again
on the same buffer — the second `StartDMA` returned 0, driver broke
out with `S2ERR_NO_RESOURCES` before writing the descriptor, and the
NIC DMA'd from address 0 (= boot ROM = all zeros). Symptom: packets
appear in pcap with the right length but zero content.

Both drivers have been retrofitted to use the Init-time captured
`phys` + a manual `CacheClearE`, matching this driver's pattern.

### 4. Synchronous vs asynchronous

rtl8139 CMD_WRITE runs entirely in the caller's context: copy packet,
flush cache, write TSAD, write TSD (starts TX), poll TSD.TOK. Done
before BeginIO returns.

e1000 / virtio-net designs typically use a unit task + IRQ handler +
msgport-based dispatch because their intended throughput warrants it.
That machinery is where the Roadshow-crash and dispatch-fast-path
complications lived in virte1000's history — none of that surface
exists in rtl8139re, so those bug classes are absent.

### Tradeoff

Register-based DMA is simpler and safer but slower — one I/O port
write per TX address, no ring batching, no offload. For 10/100 Mbit
rtl8139 on QEMU that's nowhere near the ceiling. For gigabit e1000
or paravirt virtio it eventually becomes limiting; those chips are
designed to be driven via descriptor rings.

For AmigaOS 4 on sam460ex-class hardware, the practical throughput
ceiling on ALL of these NIC families is CPU / bsdsocket-copy bound
long before it's TSAD-write-rate bound.

## Concrete lessons carried in the code

- **`stwbrx` / `lwbrx` for anything the NIC will DMA-read** — not
  needed here because rtl8139 has no in-RAM descriptor, but if a
  future revision (e.g. RX ring with a status word the NIC writes)
  puts LE fields in RAM, use them.

- **`AllocMem(size, MEMF_KICK|MEMF_CLEAR)` — no AVT_* tags** —
  `AllocVecTags(..., AVT_Contiguous, AVT_PhysicalAlignment, ...)`
  silently returns NULL for KB-sized blocks on OS4/sam460ex.
  Plain `AllocMem` works. Over-allocate + manual align if you
  need alignment stricter than 16 bytes.

- **Never nest `StartDMA` on the same buffer** — if you use `StartDMA`
  at all, pair every call with an `EndDMA`. Better yet, don't use it
  — MEMF_KICK memory is DMA-visible without it.

- **`InByte` is unreliable for RTL8139 IDR registers on OS4/sam460ex**
  — returns stride-2 garbage. `InLong` gives correct byte-lane
  ordering (low byte of the ULONG = IDR0). Use `InLong` for register
  reads; extract bytes via `& 0xFF` / `>> 8` / etc.

- **`volatile` on locals that hold `InLong` return values** — gcc's
  CSE can re-issue the InLong for the byte-extract, and some registers
  change on read. `volatile ULONG lo_v = pd->InLong(...)` guarantees
  one read.

- **Writes to `ios2_SrcAddr[]` / `ios2_DstAddr[]` (offsets 40-71 of
  IOSana2Req) don't persist between BeginIO and DoIO-return on our
  setup** — ULONG-field writes do. Cause not isolated. Workaround:
  pack address bytes into `ios2_DataLength` + `ios2_PacketType`
  ULONGs. Visible in `S2_GETSTATIONADDRESS` here.
