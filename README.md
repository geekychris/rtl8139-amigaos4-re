# rtl8139re.device

AmigaOS 4 SANA-II network device driver for the Realtek RTL8139 family.

Written in C using the walkero AmigaOS 4 GCC cross-toolchain. Tested on
QEMU sam460ex; should work on real sam460ex / AmigaOne hardware with an
RTL8139-family PCI NIC.

## Status

| Feature | Status |
|---|---|
| Resident-tag device scaffolding | ✅ |
| Library / interface acquisition (dos, expansion, utility, pci) | ✅ |
| RTL8139-family PCI enumeration via FindDeviceTags | ✅ |
| BAR0 (I/O) mapping + PCI BusMaster/IO enable | ✅ |
| MAC read via IDR0..IDR5 | ✅ |
| S2_GETSTATIONADDRESS | ✅ |
| CMD_WRITE / S2_BROADCAST (register-based DMA to TSAD/TSD) | ✅ |
| S2_CONFIGINTERFACE / S2_ONLINE / S2_OFFLINE | pending |
| CMD_READ + RX ring | pending |
| Opener list + copy hooks (S2_CopyToBuff / S2_CopyFromBuff) | pending |
| IRQ handling (top + bottom half) | pending |
| Multicast filter | pending |
| Full command dispatcher (S2_TrackType, DeviceQuery reply, ...) | pending |

## Building

```
./scripts/build.sh
```

Requires Docker + the `walkero/amigagccondocker:os4-gcc11-arm64` (or `-amd64`)
image. Produces `build/rtl8139re.device`, `build/rtl8139re.device.debug`,
and the test binaries.

## Deploying to a QEMU sam460ex OS4 guest

```
./scripts/deploy.sh testopen testtx   # push driver + tests to DH1:
```

Requires the amiga-bridge devbench REST API running on `localhost:3000`.

On the guest, copy the driver to `DEVS:Networks/` (or `SYS:Kickstart/`
for kickstart-time loading) and reboot / avail flush.

## Testing

`tests/testopen` — verifies OpenDevice succeeds and reads MAC via
S2_GETSTATIONADDRESS. Also does an independent PCI probe for
cross-check.

`tests/testtx` — sends a 60-byte broadcast ARP frame via CMD_WRITE.
Verify on the host with `tcpdump -r /tmp/qemu-n3.pcap` (assuming
`filter-dump` is set up on the netdev the driver's chip is attached
to).

## Design notes

- **Single-unit**: driver binds one PCI device at Init and serves all
  Open requests for unit 0.
- **PCI I/O BAR only**: uses `PCIDevice.InLong/OutLong` for register
  access. The OS4 PCI stack auto-byteswaps for PPC BE guests, so
  extracting bytes from a `InLong` result is just standard
  little-endian byte selection.
- **TX**: 4 slots × 2 KB, MEMF_KICK|MEMF_CLEAR allocation, direct
  physical-address in TSAD register (no descriptor ring). Synchronous
  poll on TSD.TOK after doorbell. Round-robin slot picker.
- **Register-based DMA (not descriptor-based)**: RTL8139 puts the TX
  buffer DMA address directly in a NIC register (TSAD0-3). No in-RAM
  descriptor for the NIC to walk. This makes it simpler than e1000 or
  virtio-net-pci and avoids DMA-window issues seen on some emulated
  platforms.

## License

TBD — pending decision.
