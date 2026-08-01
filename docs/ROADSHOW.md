# Binding rtl8139re to Roadshow

Roadshow (the OS4 TCP/IP stack) reads interface configs from
`ENVARC:Sys/Net/Interfaces/<name>` at boot. Each config is a small
key=value text file listing which SANA-II device + unit to open, and
per-interface tuning (MTU, offloads, etc.).

## Minimum config to try

Create `ENVARC:Sys/Net/Interfaces/rtl8139re` on the guest:

```
device = DEVS:Networks/rtl8139re.device
unit = 0
IPtype = IPv4
```

And `ENVARC:Sys/Net/autoinit/rtl8139re` (whose presence signals "bring
this interface up at boot"):

```
# autoinit: presence-only file
```

Also copy the compiled `.device` into place (or reboot after `avail flush`):

```
copy DH1:rtl8139re.device DEVS:Networks/rtl8139re.device
```

## Test procedure

1. Save both config files, then either reboot or restart Roadshow:
   ```
   NetShutdown; NetStart
   ```
2. Verify the interface came up:
   ```
   IFConfig rtl8139re
   ```
   Expect output listing MAC, MTU=1500, HW=Ethernet.
3. If Roadshow errors on Open, watch the debug console (`sashimi` or
   the console log) for OpenDevice fail reason.

## What Roadshow will exercise (and what we support)

| Command | Support status |
|---|---|
| OpenDevice, CloseDevice | ✅ |
| NSCMD_DEVICEQUERY | ✅ (returns DeviceType=NSDEVTYPE_SANA2, 12-cmd list) |
| S2_DEVICEQUERY | ✅ (MTU=1500, BPS=100M, HW=Ethernet, AddrSize=48) |
| S2_GETSTATIONADDRESS | ✅ (via IDR0/IDR4) |
| S2_CONFIGINTERFACE | ✅ (returns MAC in DstAddr, sets is_online) |
| S2_ONLINE, S2_OFFLINE | ✅ (Reconfigurations counter bumps) |
| CMD_WRITE, S2_BROADCAST | ✅ (register-based DMA, poll TSD.TOK) |
| CMD_READ | ✅ (per-opener PacketType filter, RX ring drained by unit task) |
| S2_READORPHAN | ✅ (fallback queue for unclaimed ethertypes) |
| SANA2IOF_RAW copy hooks (via ios2_BufferManagement TagItem) | ✅ (S2_CopyToBuff / S2_CopyFromBuff) |
| S2_GETGLOBALSTATS | ✅ (Sana2DeviceStats) |
| S2_GETSPECIALSTATS | ✅ (zero records — no per-record backing yet) |
| S2_ADDMULTICASTADDRESS / S2_DELMULTICASTADDRESS | ❌ (multicast hash filter unimplemented) |
| S2_TRACKTYPE / S2_UNTRACKTYPE / S2_GETTYPESTATS | ❌ (per-packet-type stats unimplemented) |
| S2_PACKETFILTER hooks | ❌ (only Copy* hooks parsed on Open) |

Roadshow *typically* only requires the ✅ set for basic bind + ARP + ICMP.
Missing multicast means IPv6 NDP won't work — IPv4 should be fine.
Missing S2_TRACKTYPE means per-app packet stats won't populate but bind
itself won't fail.

## Known caveats when trying this

- **Don't overwrite the shipping Interfaces/rtl8139** — that's what
  amiga-bridge and the OS4 shell rely on for `10.0.2.15` connectivity.
  Use a NEW interface name (`rtl8139re`) so both coexist.
- **rtl8139re binds n3 by preference** (PCI Index=1) — n0's shipping
  driver still owns the amiga-bridge chip. Our chip is on subnet
  192.168.102.0/24.
- **QEMU SLIRP hostfwd** for our chip: UDP 17977, TCP 17978 both
  forward to guest 192.168.102.15. Test with:
  ```
  ping 192.168.102.2   # gateway from guest
  ```
  Once Roadshow binds, the guest DHCP will offer 192.168.102.15.

## If bind fails

Common causes and fixes:

1. **`Interfaces/rtl8139re` refers to a nonexistent device**: check
   `DEVS:Networks/rtl8139re.device` is present and readable.
2. **Open returns non-zero**: Init logs via DebugPrintF — read via
   `sashimi` on the guest, or check `Serial:` output if wired.
3. **Roadshow says `unsupported command`**: check NSCMD_DEVICEQUERY
   returned the expected supported list (bump the array in device.c
   if needed).
4. **Bind succeeds but no traffic**: check both TX pcap and RX ring.
   `testtx` proves TX still works standalone. `testrx_diag` shows
   CBA advancing on inbound.
