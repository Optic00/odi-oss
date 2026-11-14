# odi-oss

Open firmware for the **ODI DFP-34X-2C2** GPON SFP ONU stick (Realtek
RTL9602C SoC, Lexra RLX5281 CPU, 32 MB RAM, 8 MB NOR flash), built from
source: a current mainline Linux kernel, our own userland, and our own
toolchain.

> [!WARNING]
> **For the ODI DFP-34X-2C2 only** (see "Supported hardware").
>
> **This repository was written with AI assistance ("vibecoded") and
> validated on real hardware, with no warranty of any kind.** Flashing a
> stick with the wrong image, or interrupting a flash, can brick the stick
> or take down the fiber (GPON) service it is carrying. Read "Safety model"
> below and "Quick start" before you flash anything, and never flash a
> stick that is your only connection to the Internet without a way back.

## Supported hardware

**The ODI DFP-34X-2C2, and the DFP-34X-2C3 and Luleey LL-XS2510 on user
reports.** The 2C2 is
the device this firmware was written for and the one it is tested on. The
image drives the board directly (GPIO numbers, I2C wiring of the optics,
flash layout, switch and PON registers), so on any other stick it will at
best not boot and at worst leave it unreachable.

- **DFP-34X-2C3:** reported working. It differs from the 2C2 in the polish
  of the fiber connector: the 2C2 is SC/UPC (blue), the 2C3 SC/APC (green).
  One user installed v1.14.0, unmodified, on a 2C3 that shipped with stock
  `V1.1.4-250620`, booted it on trial and committed it: O5 with full OMCI
  provisioning, internet over IPv4 and IPv6, stable. The image still calls
  the stick a 2C2 on a 2C3. Earlier reports ran a beta on a 2C3 (#42), and
  v1.15.0 flashed from the stock web page (#299). These are reports, not
  testing here; if you run a 2C3, a report either way helps.
- **Luleey LL-XS2510:** reported working. A rebrand on the same SoC
  (RTL9602C) and laser driver, whose host side runs at 2.5 Gb/s. One user
  trial-booted v1.15.0 and v1.16.0-beta.1 from stock `V1.0.2-241026`: O5
  with full OMCI provisioning and PPPoE. It needs `HOST_SERDES=2500basex`,
  written into `/var/config/odi.conf` from the stock shell before the first
  boot (`docs/SETTINGS.md`, "Host port SerDes mode"); it then linked within a
  second. The host was a UniFi Cloud Gateway Fiber whose SFP+ port a
  third-party tweak runs at 2.5G, not a stock setting. `hisgmii`, the mode
  the stock firmware runs, synced but passed no frames against that port;
  Fiber 1G is untested. One report, not testing here.

- **Anything else** (other ODI models, other RTL9601/RTL9602C sticks,
  rebrands with a different board): not supported, even with the same SoC.

[`docs/QUALIFICATION.md`](docs/QUALIFICATION.md) lists what each board, host
and feature path has been checked on, and by whom.

## Safety model

This firmware is meant to be flashed **into the slot the stick is not
currently running from**, and booted **once, on trial**, before it is ever
made permanent:

- The stick has **two boot slots** (kernel + rootfs pairs). You always flash
  the slot you are *not* running, so the working image — normally the stock
  (OEM) firmware — stays untouched in the other slot the whole time.
- A newly flashed slot is **inert** until you tell the bootloader to try it:
  `nv setenv sw_tryactive <slot>` boots that slot **exactly once**, with the
  hardware watchdog armed. If the new image does not come up — kernel panic,
  no root filesystem, `init` never starting — the watchdog resets the board
  and the bootloader falls back to the slot it already trusts. Nothing is
  lost, and nothing needs to be done by hand.
- **Commit a trial yourself, once you have watched it work.** Making a slot
  permanent (`sw_commit`) throws that safety net away: a committed image
  that later crashes reboots into itself, with no way back to the other
  slot. `nv commit <slot>`, run on the trial, writes both copies of the boot
  environment and reads each back. Until you do, the image says it is a
  trial: at every ssh login, and as `gpon_uncommitted` on the exporter. The full
  procedure, including recovery if something still goes wrong, is in
  [`docs/FLASHING.md`](docs/FLASHING.md).

## What this is

The device shipped with a closed firmware image running Linux 2.6.30. This
repository is not a repack of that image: it builds a complete replacement
firmware from source — kernel, userland tools, daemons and toolchain — for
the same hardware.

| area | stock (OEM) firmware | this firmware |
|---|---|---|
| source | closed | fully open: mainline Linux + our own drivers and userland, all built from source, no proprietary code shipped or linked |
| kernel | Linux 2.6.30 | Linux 6.18.58 (current LTS), carried as 19 changed lines in 6 mainline files plus our own overlay, so the next LTS port is cheap |
| switch / GPON MAC driver | closed | our own GPL driver, an independent implementation, built straight into the kernel |
| fibre loss (LOS/LOF) | leaves O5 on a loss and ranges again when the light is back (seen during a fibre swap) | the same, by G.984.3 Table 10-1: a loss of signal or of framing in O5 enters O6 and, with no POPUP within TO2 (100 ms), O1, giving up the ONU-ID, the Alloc-IDs and the OMCC port; the light back takes it to O2 and the OLT ranges it again. The line is sampled every 100 ms as well as on interrupts, so a pulled fibre is seen with no downstream traffic at all. v1.2.1 and earlier stayed in O5 through a fibre pull while the OLT deregistered them, until a reboot (ISP2). Host-tested against the register model (`test/odi_gpon_los_test.c`) and run on a stick: three fibre pulls of 16 to 27 s, each back to O5 with no reboot (release v1.3.0) |
| flow control toward the host | the host port negotiates PAUSE with the host, so a stick whose upstream stops draining sends a continuous stream of XOFF frames: an Intel 10G NIC behind it received about 5.9 million, stalled its transmit queue and reset itself in a loop | the host port never sends PAUSE (its transmit side forced off, receive left negotiated): when the upstream cannot drain, the switch drops at its own buffers, as an ONU should, and the host link stays up. Kernel parameter `odi_switch.host_pause` restores the stock behaviour, live or at boot ([`docs/SWITCH.md`](docs/SWITCH.md), "Host port PAUSE"). Host-tested against the register model (`test/odi_host_pause_test.c`) and verified on a stick: no PAUSE toward the host through a fibre pull |
| host link auto-negotiation | always on (1000BASE-X, clause 37: the PCS control word reads 0x1140 on the stock image too), with no setting to turn it off | on by default, as stock; `HOST_AUTONEG=off` forces 1000BASE-X full duplex for such a router. A change is a trial that reverts by itself after 120 s unless confirmed over the link it changed; a boot whose forced link does not come up falls back to auto-negotiation (and says so in the web UI); three short power-ups in a row (each cut about 80 s after power-on) make the next boot a safe-mode boot that clears the key. Host-tested (`test/odi_host_autoneg_test.c`, `test/host_autoneg_test.sh`) and run on a stick: the register write (PCS control 0x1140 to 0x0140, the link stays reported up), the trial, its expiry and the confirm over the same link, and a forced stick carrying traffic to a host whose SFP port has auto-negotiation off, which the stick with auto-negotiation on does not (link up, no traffic). The boot fallback has not been seen on hardware ([`docs/SETTINGS.md`(docs/SETTINGS.md), "Host port auto-negotiation") |
| host SerDes mode | Fiber 1G (1000BASE-X) on the DFP-34X-2C2, 2500BASE-X or HiSGMII on the HSGQ XPON stick, chosen by the stock firmware from its own config, with no trial and no fallback | Fiber 1G by default, as stock on the DFP-34X-2C2 and value for value the same sequence; `HOST_SERDES=2500basex` (a 2.5G host, or an HSGQ stick) or `hisgmii` selects another, with the port forced and the auto-negotiation set per mode. The stock `LAN_SDS_MODE` is not read. A change is a trial that reverts by itself after 120 s unless confirmed over the link it changed; a 2500BASE-X boot whose SerDes does not sync falls back to Fiber 1G, alternating, for up to 10 minutes, and stays in Fiber 1G if neither links; three short power-ups in a row make the next boot a safe-mode boot in Fiber 1G that clears the key. Fiber 1G, the most compatible host mode, is the fallback everywhere; whether the HSGQ board links in it is untested. The register values and the HSGQ port are the reporter's, read off a stock HSGQ stick and run on one (2500BASE-X against a 2.5G router port, HiSGMII against a media converter); host-tested here (`test/host_serdes_test.sh`, `test/odi_host_forced_test.c`); run on hardware with this image by one user, on a Luleey LL-XS2510: 2500BASE-X linked against a 2.5G router port, HiSGMII synced there but passed no frames ([`docs/SETTINGS.md`](docs/SETTINGS.md), "Host port SerDes mode") |
| GPON/OMCI daemon | closed | our own `omcid`, with `omcli`/`omcicli`, `omciprobe` and `omcicap` tooling around it |
| exploit mitigations in the daemons that parse network input | not measured | `omcid` (OMCI from the OLT) and `igmpd` (IGMP/MLD from both sides), like every tool of ours, are built with `-fstack-protector-strong` over a guard seeded per process from the kernel's random bytes: an overrun of a stack buffer ends the daemon with SIGABRT, logged to `logread` and its own log, and init restarts it, instead of returning into overwritten data. Also zero-initialised locals, wrapping signed overflow, stack clash probes, no executable stack declared and no writable-and-executable segment. Checked on every build (`tools/check-hardening.sh`) and under qemu (an overrun aborts); text +6.5% for omcid. The CPU has no execute-inhibit bit and the binaries are not position-independent, so this is not ASLR or NX ([`docs/CROSS-COMPILING.md`](docs/CROSS-COMPILING.md), "Hardening the freestanding binaries"); not yet trialled on hardware |
| switch/optics CLI (`diag`) | closed; some commands crash or hang the CLI | our own CLI over our kernel's interfaces: optics, GPON state, alarms and flows, port MIB counters, the MAC table, register access; batches commands and always exits cleanly; the exporter's commands are byte-compatible with the stock CLI |
| SoC temperature | not exported | the die temperature as a standard hwmon sensor, `/sys/class/hwmon/hwmonN/temp1_input` (name `rtl9602c_soc`, millidegrees), read-only from the thermal words the boot leaves enabled; the exporter serves it as `gpon_soc_temperature_celsius` from the release that carries it. The scale is inferred, not calibrated: 62 C on a stick whose optics module read 46 C, 63 to 65 C under the stock firmware; a trend, good to a few degrees. Decode host-tested (`test/odi_hwmon_test.c`); costs 2,746 bytes of uImage ([`docs/KERNEL.md`](docs/KERNEL.md), "Our drivers") |
| optics (DDM) readout | closed | our own SFF-8472 reader, exposed through `diag`: temperature, voltage, bias current, tx/rx power, plus alarm/warning flags and optical LOS status (`pon get transceiver alarm-status`); the same readings answer an OLT's optical test of the ANI-G over OMCI; the module's own I2C devices (A0h, A2h and the laser driver's pages) readable with `i2cget`/`i2cdump` on `/dev/i2c-0`, writes limited to the page select |
| optics diagnostics on the router (host-side SFP EEPROM) | v08 boards: the SoC's SFP EEPROM emulation serves the module identity and live diagnostics to the router (measured by a user on a stock v08 stick); v06 boards: a fixed EEPROM, no diagnostics | v08 boards: the same, on by default (`SFP_HOST_DDM`), A2h refreshed every second, the module never written; the router showed live diagnostics in a user's test on a v08 board. v06 boards: unchanged (hardware) |
| VLAN filtering (class 84) | users report multi-VLAN lines losing all but one VLAN (Anime4000/RTL960x issues #89, #447) | every entry of the VLAN filter list gets its rule (up to 12), and the forward operation is read (G.988 table 9.3.11-1): forward all, untagged only, tagged only, positive filtering by VID, priority or both, untagged bridged or discarded; the negative and MAC-address filtering codes, which a bridge rule cannot express, are built as the mandatory 0x10 and logged (`event=vlan_fwdop`); every code checked under qemu (`make test-omci`) |
| VLAN handling | manual tag (`VLAN_MANU_MODE` 1), or with `VLAN_CFG_TYPE` other than 1 the VLAN the OLT asks for in class 171 | the same, documented as a choice: "stick tags" (the manual VID), "router tags" (transparent, `VLAN_MANU_MODE` 0: no tag added or removed, every VLAN the OLT carries passes to the router, internet, IPTV and voice on one port), or, with `VLAN_CFG_TYPE` other than 1, the OLT's own class 171 table, entry by entry (an untagged frame given a tag, one VID translated to another, one VID split into rows by priority, S-tags pushed and popped, untagged frames of one EtherType (IPoE, PPPoE, ARP, IPv6) given their own tag, the C-tagged frames of one VID discarded, each with its downstream inverse; a table with an entry the classifier cannot carry out stays transparent and is logged once, entry and reason); the translation, split-by-priority, S-tag, EtherType and discard rows are checked as register words under qemu and in the host tests, not yet against an OLT that provisions them; both line shapes (with and without class 84) checked under qemu (`docs/SETTINGS.md`, "VLAN handling") |
| rebuilding after OMCI resume | closed | a rebuild withdraws the services a resumed omcid restored from its snapshot before it adds the new plan, so a VLAN change after a respawn leaves no old rule behind; in the resume regression, not yet run on hardware |
| applying a changed setting | closed (earlier builds of this firmware restarted omcid beside the one init respawns) | `apply.sh omci` sends the running omcid SIGHUP and it rereads the store itself: a changed VLAN setting rebuilds the bridge connections in place with the ONU still in O5 (rules byte for byte a fresh daemon's, checked against both ISP sessions under qemu); a changed identity key deactivates and re-registers the ONU inside the same process, each re-registration is recorded with the OLT answer (three Deactivate_ONU-IDs before O5 or a disabled serial number are a refusal), and after two refused changes in a row the next one waits for `apply.sh omci --confirm` (checked by the `reload` omcid test), and `identity-trial.sh` tries a new serial number, PLOAM password or LOID from RAM, stores it only once the ONU is back in O5 with services and no new MIB reset loop for 600 s, and otherwise goes back by itself (checked by `identity_trial_test.sh` and the `reload` test). Nothing starts a second omcid, which on hardware once cycled the ONU through O1 to O3 (`docs/SETTINGS.md`, "How apply works") |
| multicast | closed IGMP handling | multicast from the OLT's multicast GEM port reaches the host by flooding, never copied to the stick's CPU. IGMP snooping is opt-in (`IGMP_SNOOPING=1`) and follows what the OLT provisions: one static row a group (host and PON), the profile's group limit, access lists, timers and immediate leave, the upstream IGMP tag (add, replace, replace the VID) and rate, joins outside the lists kept from the OLT unless the profile forwards them, and, with the separate opt-in `IGMP_PON_DROP`, IPv4 multicast no group asked for dropped before the UNI. MLD (IPv6) snooping is a further opt-in (`MLD_SNOOPING=1`): the host's MLD copied or taken like its IGMP (and no other ICMPv6), IPv6 groups with their own rows and the profile's IPv6 access list rows, the same upstream tag and rate, and with `IGMP_PON_DROP` unjoined IPv6 multicast dropped too, while ff02::/16 and the solicited-node groups always flood, so router advertisements, neighbour discovery and DHCPv6 are never pruned. The snooper sees only the host's own IGMP and MLD; nothing from the fibre reaches the CPU. Not yet run against a real multicast stream (see [`docs/SWITCH.md`](docs/SWITCH.md), "Multicast", and [`docs/TOOLS.md`](docs/TOOLS.md)) |
| system log | none: no syslogd, nowhere central to read a log from | `syslogd`/`klogd`, a 64 KB circular buffer `logread` reads, optional remote forwarding (`SYSLOG_SERVER`); one greppable `event=` line per link or provisioning event -- every ONU state change with the state it left, how long it was there and whether the OLT, a timer, the fibre or this side caused it; OLT MIB resets with the last requests before each, a reset loop named as one (three OLT resets in ten minutes, also in `omcli state`), uploads, provisioning bursts (N creates/sets), reboot and software-download requests; every managed entity class or message type the OLT sends that omcid does not model, once per boot and counted in `/var/log/omcid-unknown.txt` (the ISP1 OLT sends three); omcid resuming or re-registering -- so the log says who started an outage (`docs/TOOLS.md`, "Link and provisioning events") |
| software download from the OLT | flashes the ISP image into the other slot and reboots into it, on the OLT command | never flashes and never reboots: the download is answered (G.988 windows, the image CRC-32 checked) and the image discarded, and the software image entity reports it activated and committed so the OLT is satisfied; `OLT_SW_DOWNLOAD=reject` refuses it instead (`docs/SETTINGS.md`). Tested under qemu, with a syscall trace showing no flash, no exec and no reboot |
| reboot request from the OLT | not measured | `OMCI_ACCEPT_REBOOT`: off (the default) refuses it with "command not supported", `1` answers success and reboots, `2` answers success and does not reboot, for an OLT that insists on a successful answer; every request is logged as `event=olt_reboot`. All three seen on a stick with an injected OLT Reboot (`docs/SETTINGS.md`) |
| OLT disable (Disable_Serial_Number, O7) | not measured: the OLT's message was never seen on either line | O7 as G.984.3 Table 10-1 has it (laser line high); with the opt-in `GPON_KEEP_DISABLE=1` the disable also survives this stick's own PON restarts and reboots until the OLT enables it, as the standard asks (host tests of the state logic and the boot step; no OLT has disabled either stick, so the live path is exercised by a test hook) |
| vendor-specific entities the OLT expects | not measured | `OMCI_FIXED_ME` answers a Get of a class omcid does not model with fixed bytes, and with the `CLASS:INSTANCE:ATTR:HEX` form makes an entity that exists at one instance only: it is listed in the MIB upload, a Get of another instance answers "unknown instance". Seen on a stick: the configured instance answered, another refused, the entity in the upload, and the OLT kept its six services (`docs/SETTINGS.md`) |
| values of the entities the ONU creates | stock answers from the ONU itself: serial and vendor id in the circuit pack, uptime, optical levels, link, PON state, flash slots | the same, from the configured serial, the monotonic clock, the transceiver readings, the link bit, the PON state and the flash slots; attributes an OLT writes read as their power-on values until it writes them (checked by the `live` omcid test against a stubbed driver, and on two sticks on two OLTs against the stock firmware `omcli mib get all` of the same line: only live readings, the slot flags and OLT-chosen instance ids differ); the T-CONT policy stays 1, as stock, since some OLTs provision the upstream queues only when it is not null |
| VEIP slot and vendor-private entities | fixed by the image; `OMCI_VEIP_SLOT_ID` (with `OMCI_CUSTOM_ME` bit 256) moves the VEIP slot | `OMCI_VEIP_SLOT_ID` as on the stock firmware, and `OMCI_PRIVATE_ME=0` to leave out ME 350, 370 and 373 (ME 373 carries a third-party product string) and `OMCI_VEIP=0` for a PPTP-only ONU with no VEIP; each re-registers the ONU. Checked by the `keys` omcid test (entity ids, MIB upload count, reload class); the defaults ran on two sticks on two OLTs; the non-default values are not yet run against an OLT (`docs/SETTINGS.md`) |
| phone (SIP over OMCI) | none: the stock image has no voice plugin, and the stick no phone socket | opt-in `OMCI_POTS_PORTS` (1 or 2, experimental) tells the OLT of phone ports, for a SIP line the router behind the stick registers: a POTS circuit pack in a free slot (`OMCI_POTS_SLOT_ID`), the ports, their line status and the VoIP config data (SIP over OMCI only), and every VoIP entity the OLT then provisions is kept and answered; the SIP password is never logged and is masked in MIB dumps. The line status says "config done", never "registered", since the stick cannot see the router's registration. The voice reaches the router on the host port, tagged with the VLAN the OLT sets (only the IP host a SIP agent names; any other IP host's VLAN stays off it), and no voice rule can take another service's untagged frames; the stick's own CPU stays cut from the fibre. Checked by the `voip`, `keys`, `reload` and `drv` omcid tests (with the key off the MIB upload and every driver golden are unchanged); not yet run against an OLT that offers voice (`docs/SETTINGS.md`) |
| clock | none: no RTC, no NTP client | opt-in `ntpd` (`NTP_SERVER`) |
| web UI | closed, minimal | `confd` (a separate project): same port; offers only the 21 settings this image reads, each marked LIVE, SERVICE RESTART, INTERRUPTS INTERNET or REBOOT, and applies them without a reboot where it can; the switch MAC table; firmware upload and write to the inactive slot; SSH-key management; build/version info |
| metrics | none | a Prometheus exporter, `metricsd` (a separate project), including whether the OLT actually provisioned service, not just link state |
| GPON line quality | 43 GPON MAC counters behind `diag gpon show counter`, each cleared by the read, so every reader takes counts from every other | the driver is their one reader: sampled every second into 64-bit totals in `/proc/odi_gpon/counters` (BIP errors, FEC corrected and uncorrectable, PLend, BWmap and GEM errors, the BWmap allocations the OLT grants this ONU, upstream bursts, PLOAMu and GEM bytes), with LOS, LOF and LOM events and seconds, time in each ONU state and every state transition, and per GEM flow (`/proc/odi_gpon/flows`) the GEM frames, bytes and Ethernet frames each way, which show a silent GPON port on a multi-service line (`diag gpon get flow-counters` prints them; the exporter serves them as `gpon_gem_*`); host-tested against a clear-on-read register model (`test/odi_gpon_ctr_test.c`), and seen accumulating on a stick |
| SSH | an old dropbear needing legacy algorithms re-enabled on the client | a current dropbear, ed25519 host key, `scp` in both directions |
| management from the fibre | the switch's port isolation all open (read on a stick), nothing in the kernel keyed on the ingress port | never reachable from the PON side, by two independent layers keyed on the port a frame entered by, whatever the OLT provisions: the switch never forwards a frame from the PON port to the CPU (its isolation mask), and the NIC driver drops every frame from the PON port that is not OMCI before the IP stack sees it, counted in `/proc/odi_nic` (`rx_pon_dropped`); OMCI is untouched. The other way, the stick's own frames are addressed to the host port only and the switch never forwards one from the CPU to the PON port by lookup, so only the OMCI replies go up the fibre; and the stick's MACs are static rows on the CPU port, so a frame from the fibre carrying one of them cannot divert the host's traffic to the stick upstream. Host-tested against the reset words read on a stick and descriptor words logged on one (`test/odi_pon_isolation_test.c`); not yet trialled on hardware ([`docs/SWITCH.md`](docs/SWITCH.md), "PON port isolation") |
| OMCI under a flood from the host side | not measured | opt-in (`odi_nic.omci_ring=1`, at boot or live; off by default): the OMCI frames the switch traps to the CPU arrive on a receive ring of their own, served first by every poll, so a flood toward the stick from the host side that fills the main ring cannot take the descriptors OMCI needs; with one ring, another open implementation for this chip measured 432 of 440 OMCI messages of a registration under a host flood, and 442 of 442 with the second ring. `/proc/odi_nic` counts each ring and the frames the NIC dropped for want of a descriptor (`rx2_taken`, `rx_missed`). Host-tested against a model of the flood and of the six receive rings' flow control (`test/odi_nic_hw_test.c`); not yet trialled on hardware |
| login security | telnet enabled by default, and a fixed, well-known default login (`admin`/`admin`) | no telnet; ssh only. A release image ships with root locked (keys only, no password); a source build defaults to a password generated per build and stored as a SHA-512 crypt hash. Either way, the web UI's own `admin`/`admin` default must still be changed by the operator |
| memory | about 1.1 MB free after a cache drop, with the stock services running | about 15 MB available: our drivers load the switch and GPON register tables from firmware files, apply them and free them |
| kernel size | fits its partition with little to spare | about 183 KB spare (1,172,555 of 1,359,872 bytes), after cutting kernel features and subsystems nothing on the stick uses and the driver bring-up scaffolding, and with the hardening below |
| kernel hardening | Linux 2.6.30, older than most of these options | stack protector (strong) and zeroed stack variables, `FORTIFY_SOURCE`, hardened user copies, heap zeroed on allocation and on free, list-corruption checks that stop the kernel, the stack-end check, `dmesg` for root only, `kptr_restrict=2`, the `fs.protected_*` links and files rules, and wider user address randomisation with nothing mapped below 64 KB; 9,410 bytes of uImage for those not already on. The options this CPU or toolchain cannot have (read-only kernel text, address randomisation of the kernel, virtually mapped stacks, trapping UBSAN) are listed with the reason, and a check fails the build if any chosen one drops out ([`docs/KERNEL.md`](docs/KERNEL.md), "Hardening"). Built and checked here; not yet trialled on hardware |
| reboot | not measured | `reboot` resets at once through a watchdog restart handler; back in the stock image about 73 s later |
| toolchain | a decade-old vendor cross-compiler | our own gcc / binutils / uClibc-ng, built from source, targeting the CPU's actual instruction set; published as prebuilt container images pinned by digest, so a build pulls the toolchain instead of compiling it for an hour: a clean image build takes about 3.5 minutes on a native 8-core host (building the toolchain from source is still one command) |
| changing the management address | applied at once and kept: a typo in the address locks you out of the stick | a change is a trial: the new address is applied in RAM, the old one stays up beside it, and the stick goes back by itself after 90 s unless the change is confirmed over the new address; only a confirmed trial reaches the config store, and a reboot mid-trial comes up on the old address. Host-tested (`test/mgmt_addr_test.sh`, against the real `network.sh` and `flash`); not yet run on a stick ([`docs/SETTINGS.md`](docs/SETTINGS.md), "Management address trial") |
| recovery without a login | no documented way back from a forgotten password | a safe mode from the power alone: five power cycles in a row, each cut about 80 s after power-on (three with `HOST_AUTONEG=off`), make the next boot a recovery boot for that boot only: the PON off (the OLT is not disturbed), the host port on auto-negotiation, the web UI and root on the default password `admin`, nothing in the saved configuration changed but `HOST_AUTONEG`; the web UI says so until acknowledged. Physical access to the power is enough, the documented trade-off ([`docs/SETTINGS.md`](docs/SETTINGS.md), "Recovery: the safe mode"). Host-tested (`test/safe_mode_test.sh`) and run on a stick: three real power cycles with `HOST_AUTONEG=off` gave the recovery boot (laser off, ONU in O1, key cleared, `admin`/`admin` on the web UI and over ssh) and an acknowledge plus a normal reboot returned to the saved password and keys; the five-cycle path was exercised by setting the counter, not by real power cycles |
| boot safety | undocumented | one-shot trial boot (`sw_tryactive`) plus a watchdog the kernel alone owns and enforces (`docs/SETTINGS.md`, "Watchdog rules"): it self-reverts a boot whose userland never confirms, a registered daemon (omcid) that stops pinging its own deadline, `MemAvailable` held below a floor, or a CPU-port NIC that takes no frame for 30 s while the switch keeps offering them (the stick unreachable with everything else healthy) -- boot confirmation does not depend on the network. Every rule is host-tested against a fake clock (`test/odi_wdt_test.c`); the omcid-heartbeat rule has reset a stick on hardware (three in a row ended in the safe mode); the other rules are host-tested only ([`docs/QUALIFICATION.md`](docs/QUALIFICATION.md)) |
| a trial nobody can reach | stays up until power-cycled | reverts to the committed slot by itself 300 s after boot unless the management path is seen working both ways (a TCP session from the host, or a neighbour that answered the stick), through an `odi_wdt` client only a trial registers (`docs/FLASHING.md`, "The trial guard"). Host-tested (`test/trial_guard_test.sh`, `test/odi_wdt_test.c`) and in the qemu harness; armed and released on lab trials; seen to revert a stick on user reports (#241, and #321: a trial boot whose host link passed no frames went back to the stock slot by itself after about seven minutes), not yet seen to revert one here |
| committing a trial | by hand (`nv setenv sw_commit`), no sign that a boot is a trial | by hand with `nv commit <slot>`: both environment copies, in a power-cut-safe order, each read back, refusing any slot that is not the running one or a trial queued since the boot, clearing a leftover `sw_tryactive` in the other copy, and writing nothing when a copy could not be read completely (host-tested against a fake flash that fails at every step, `src/nv/test/test_commit.c`, and run on a stick: both copies written and read back); an uncommitted image says so at every ssh login and as `gpon_uncommitted` / `gpon_committed_slot` on the exporter, from `/var/run/odi-slot`; the other slot gets a partial check at every boot (uImage magic, header and data CRC and size, squashfs superblock; host-tested against images built in memory, `src/nv/test/test_slot.c`, and passed by a stock image and a release of this one), and the login banner, and `nv commit` after it commits, say when it would not boot |
| boot reliability | not a concern of the stock image | two bootloader leftovers found and fixed: NIC DMA still running into memory the kernel reuses (stopped first thing at boot), and an instruction cache that does not see new code (invalidated whenever a page can run); release images boot 15 of 15 in a row. After a power cut the GPON MAC tables come up full of garbage (on one stick 120 of 128 GEM-port rows and all 32 Alloc-ID rows valid, and the OLT deactivated it in a loop): every boot now waits for the GPON MAC reset, clears every row of them (and of the downstream multicast filter) and reads every row back, logging what it found and what it left ([`docs/SWITCH.md`](docs/SWITCH.md), "GPON boot init"). The switch ACL tables, on for the UNI and PON ports, get the same clean and read-back before ACL ingress is enabled: after a boot of the stock firmware they still held its rules ("Platform init") |
| boot debugging | none (no serial console) | every console line mirrored into DRAM that survives a watchdog reset; the next boot of this image shows the failed one in `/proc/odi_ramlog_prev`, with fatal-signal register dumps and early-boot crumbs |
| a daemon that keeps crashing | init restarts it with no limit | busybox init restarts every daemon at once with no limit of its own; `respawn.sh` now waits 1, 2, 4 ... s before restarting one that died within 30 s of its start, up to a cap per daemon (omcid 2 s, dropbear 10 s, confd and syslogd 30 s, the rest 60 s), writes the count and state to `/var/run/respawn/<name>` and logs one `event=respawn` line per restart. Nothing is disabled or rebooted by it: omcid stays with the watchdog, the rest are retried at the cap. Tested with a fake clock for every `respawn` entry (`test/respawn_budget_test.sh`) and by repeated `kill -9` under qemu (`docs/SETTINGS.md`, "Restart budget") |
| memory pressure (OOM) | a 20 MB scp into `/tmp` (unbounded ramfs) took `MemAvailable` to 0.86 MB and the OOM killer took dropbear, confd and omcid, none of them restarted, unmanageable until a power cycle -- reproduced on ISP1, 2026-09-27 | `/tmp` and `/var` are size-capped tmpfs (8 MB / 6 MB, `docs/SETTINGS.md`), so a full `/tmp` gives ENOSPC instead of taking down the box; `oom_score_adj` biases the OOM killer away from omcid/dropbear/confd; the four critical daemons restart the instant they die, as `/etc/inittab` `respawn` entries busybox init forks and tracks itself, not a hand-rolled supervisor loop; the kernel itself resets the board if `MemAvailable` stays below a floor for several consecutive checks, in addition to (not instead of) the omcid own ping deadline -- a first version of the second check (v1.0.2, a separate userland process) shipped and was withdrawn the same day for withholding its kick with nothing kernel-side enforcing it, so a hang it correctly saw still never reset the board |
| testing | none published | host tests of the drivers against a register model, the OMCI daemon replayed under qemu against captures from two ISPs, golden tests of the exporter-facing CLI output, of the whole boot register stream, of every command and `/proc` write the boot script makes, and of the driver calls of an OMCI session per ISP; `make test-qemu` boots the real rootfs (busybox, inittab, services, dropbear, confd, metricsd) under `qemu-system-mips -M malta` on a stock kernel and checks ssh/web UI/exporter plus the resilience scenarios above end to end; CI on every change |
| diagnostics | none | one-click bundle from the web UI (Admin) or `/etc/scripts/diag-bundle.sh`: logs, the previous boot ramlog, dmesg, slot variables from both environment copies, an exporter scrape, the config, and the GPON/OMCI state (flows, T-CONTs, PON queue registers, PLOAM counters, the datapath MIB classes); every password redacted and scrubbed from every file, the serial number, MACs and LOID masked to their last four characters unless `--full`; `make test-qemu` plants both and asserts none reaches the bundle (`docs/TOOLS.md`) |
| config backup | a manual download from the vendor UI | `tools/config-backup.sh` pulls the same backup from a host on a schedule (cron or a systemd timer), keeps a dated copy only when a setting changed and the last 30, and exits non-zero when it could not (`docs/SETTINGS.md`); a whole-flash read, check and restore recipe is in `docs/FLASHING.md` |
| documentation | none | build, flashing and recovery, every setting and toggle, the boot stage by stage (`docs/BOOT.md`), the kernel port, a contributor guide (`docs/HACKING.md`) |

Every row above is backed by something you can read or run in this repo:
[`docs/IMPROVEMENTS.md`](docs/IMPROVEMENTS.md) has the detail and the
verification for each one.

## Screenshots

| Web UI ([odi-ui](https://github.com/AndreZiviani/odi-ui)) | `diag` on the stick |
|---|---|
| ![Web UI: forwarding counters and the learned MAC table](https://raw.githubusercontent.com/AndreZiviani/odi-ui/main/docs/screenshots/ui-mactable.png) | ![diag output: optics readings, ONU state O5, all GPON alarms clear](docs/images/term-diag.png) |

Identifiers in the screenshots are placeholders. More pages in the
[odi-ui README](https://github.com/AndreZiviani/odi-ui#screenshots).

## Quick start

**Get an image**, either a published release or your own build:

- **Release** (recommended if you are not changing the code): download
  `<version>.tar` and `SHA256SUMS` from
  [Releases](https://github.com/AndreZiviani/odi-oss/releases), and check
  the tarball against the checksum file before flashing anything. Release
  images ship keys-only — see "First login" below.
- **Build** (needs Docker; see [`docs/BUILDING.md`](docs/BUILDING.md) for the
  full breakdown and remote-build option):

      make image-all      # pulls the toolchain; kernel, packages, our tools, the image
      make image          # just the tarball, out/image/<version>.tar, once the pieces above exist

  A source build defaults to a random per-build root password
  (`ROOT_PW=locked` builds the same keys-only image the releases ship).

**Flash**, into the slot you are not running, and trial-boot it — commit
it with `nv commit <slot>` only once you have checked it yourself. Full procedure, including how to read a boot you
could not otherwise see: [`docs/FLASHING.md`](docs/FLASHING.md).

**Updating from an older release?** v1.6.0 changes what the stick tells the
OLT about itself (a complete MIB upload, as the stock firmware sends it) and
programs the upstream queues from what the OLT configures: expect the OLT to
provision a little differently the first time. The first omcid restart after
the update re-registers once (the resume snapshot format changed). In v1.5.0
`/proc/odi_gpon` became a
directory (`status`, `counters`, `ploam`, `flows`): a script that reads
`/proc/odi_gpon` directly must open the file it needs. The exporter and
the web UI in the image already do.

Two things changed in v1.4.0 that you can see on the line:

- **The laser stays off unless the laser driver chip is the one this image
  is tested with** (the Semtech GN25L95, id 0x6a, of the DFP-34X-2C2). On a
  stick with another laser driver, or none that answers, the image does not
  switch the laser on and refuses GPON activation, so the ONU stays dark
  and never registers. `dmesg | grep 'odi_board: laser'` says why, and
  `/proc/odi_gpon/status` shows `laser on 0 ldd_id <id> unverified 0`. If you know
  your stick's laser is fine (it ran an earlier release without trouble),
  `flash set ODI_LASER_UNVERIFIED 1` and a reboot switch it on anyway
  ([`docs/SETTINGS.md`](docs/SETTINGS.md), which says what a laser lit
  outside its timeslots does to the other ONUs on the port). Check this on
  the trial boot, before you commit.
- With `VLAN_CFG_TYPE` 0, the stick now applies the VLAN the OLT asks for
  (class 171) as the stock firmware does. On such a line, remove the VLAN
  from the router.

**First login**, once the trial image is up:

- **Release image, or a source build with `ROOT_PW=locked`**: root has no
  password at all (ssh with a password is refused). Open the web UI on
  port 80 — `admin`/`admin` until you change it — and add your SSH public
  key under its SSH-key admin page; `ssh root@<stick>` with that key then
  works, and keeps working across reboots and re-flashes (the key lives on
  the config partition, not in the image).
- **A default source build**: ssh as `root` with the password
  `image/build.sh` generated for that build
  (`out/image/root-password-<version>.txt`), or the web UI the same way as
  above. Add your own SSH key from there; the root password stays valid
  beside it.

Either way, change the web UI's own default credential first — it is what a
freshly reset or freshly flashed stick answers to. Full detail, including
every port and how to move files on and off the stick:
[`docs/ACCESS.md`](docs/ACCESS.md).

## Status and limitations

**Tested on real sticks.** What is maintainer-tested, user-reported,
host-tested or untested is in [`docs/QUALIFICATION.md`](docs/QUALIFICATION.md).
Releases are listed in
[`CHANGELOG.md`](CHANGELOG.md). The firmware runs on two DFP-34X-2C2
sticks on two different ISPs' GPON networks, and both now run it as their
committed image, with the stock firmware kept in the other slot. The
figures below were measured on an earlier release,
`odi-oss-260925-618z1`:

- **ISP1**: 15 of 15 consecutive trial boots reached a confirmed userland
  (75-81 s from `reboot`), then a 3-hour soak with a sample every 10
  minutes came out clean: ONU state O5 throughout, every data round trip
  through the stick's own NIC checksummed correctly, no receive or transmit
  errors, no kernel errors, flat free memory.
- **ISP2**: carrying a household's Internet link (PPPoE through the stick,
  about 52 MB/s on a 20 MB download, the same as the image before it).

Each stick was first run as a one-shot trial over the stock firmware, as
the safety model above describes, and committed only after that.

This is new, actively developed firmware. Before relying on it:

- It targets exactly one board: the ODI DFP-34X-2C2 / RTL9602C. It is not a
  general MIPS or GPON-ONU firmware.
- `diag` is not the stock CLI: it carries only the commands our kernel can
  answer (`diag help` lists them). The handful the Prometheus exporter uses
  keep the stock syntax and output, so the exporter reads either firmware.
- IGMP snooping is opt-in and unproven on a real stream: neither of our
  lines carries IPTV, so the copy rule, the group rows and the drop of
  unregistered multicast have been tested against a register model and the
  frame parsers, not against a multicast source, and no OLT has yet seen a
  report igmpd re-tagged and sent up. MLD snooping (opt-in) has run on a stick against a host's own MLD
  joins, never against a multicast stream from the fibre; its ACL rules and
  the IPv6 drop follow another open implementation's register measurements. The OLT's IGMP and MLD proxy
  modes are not done, and ME 311's IPv6 active group list stays empty.
- The config store is shared with the stock firmware: of its 184 keys this
  image acts on 23, and the UI shows the other 161 read-only.
  [`docs/SETTINGS.md`](docs/SETTINGS.md) has each one and what applying it
  costs.
- The stick's management (web UI, ssh, exporter) listens on every address
  of the stick, and is reachable from the host side only: nothing from the
  fibre reaches the stick's IP stack (the switch keeps frames from the PON
  port off the CPU, and the NIC driver drops any that get there,
  [`docs/SWITCH.md`](docs/SWITCH.md), "PON port isolation"). Anyone on the
  host side can reach it: change the web UI's `admin`/`admin`.
- The watchdog is armed on every boot: if the kernel hangs or the boot
  scripts never finish within 120 s, the board resets (a trial falls back
  to the committed slot). A stick that boots but is unreachable is not
  reset; power-cycle it. See [`docs/FLASHING.md`](docs/FLASHING.md).
- Builds are reproducible: two clean builds of one commit give the same
  tarball (`ROOT_PW=locked`; a random root password differs by design),
  checked by `tools/compare-images.sh` ([`docs/BUILDING.md`](docs/BUILDING.md),
  "Reproducible builds").
- There is no serial console on this device. If a trial boot goes wrong and
  you cannot reach the stick, `docs/FLASHING.md` covers what to do (and what
  it does not); a hardware UART is the last resort, and the point of the
  trial-boot safety model above is to make that resort as rare as possible.

## Documentation

- [`docs/KERNEL.md`](docs/KERNEL.md) — the 6.18 kernel port: CPU, board, boot,
  the config, our drivers, the ramlog, how to build and check it.
- [`docs/TOOLS.md`](docs/TOOLS.md) — every CLI and daemon: what it does, how
  to run and restart it, what it reads, where it logs, who starts it.
- [`docs/SETTINGS.md`](docs/SETTINGS.md) — every setting the web UI and the
  config partition offer: which ones this image actually uses, and which
  need a reboot or interrupt the internet connection.
- [`docs/BUILDING.md`](docs/BUILDING.md) — building the whole image, piece by
  piece, locally or on a remote build host.
- [`docs/FLASHING.md`](docs/FLASHING.md) — the trial-boot procedure and
  recovery.
- [`docs/QUALIFICATION.md`](docs/QUALIFICATION.md) — what has been checked on
  which board, optics, host, SerDes mode and OLT line, who checked it, on
  which release, and what a path must pass to be called supported.
- [`docs/ACCESS.md`](docs/ACCESS.md) — ports, the web UI, SSH keys, moving
  files on and off the stick.
- [`docs/CROSS-COMPILING.md`](docs/CROSS-COMPILING.md) — compiling your own C
  for this CPU, freestanding or against our libc.
- [`docs/IMPROVEMENTS.md`](docs/IMPROVEMENTS.md) — the improvements table
  above, expanded, with what backs each claim.
- [`docs/LICENSING.md`](docs/LICENSING.md) — what license covers what in this
  tree.
- [`docs/REFERENCES.md`](docs/REFERENCES.md) — the public specifications the
  GPON code is written against, and how to fetch them.
- [`CHANGELOG.md`](CHANGELOG.md) — what each release changed.
- [`docs/kb/README.md`](docs/kb/README.md) — field notes on the device and the
  stock firmware's behaviour, from reverse-engineering real sticks.
- [`docs/HACKING.md`](docs/HACKING.md) — the contributor guide: layout, the
  build, the recovery mechanism in full, testing, the known traps, and every
  feature toggle, for a developer who wants to change this firmware.
- [`CONTRIBUTING.md`](CONTRIBUTING.md) — scope, reporting issues, the gates.
- [`SECURITY.md`](SECURITY.md) — supported versions and how to report a vulnerability privately.
- [`AGENTS.md`](AGENTS.md) — repo layout, build/test commands and house rules,
  for anyone (human or AI) working on this codebase.

## License

Our own code (kernel patches and drivers, `src/`, the rootfs skeleton and the
build scripts) is GPL-2.0-or-later; see [`LICENSE`](LICENSE). The kernel
itself stays GPL-2.0-only, as upstream ships it — our patches and drivers
are GPL-2.0-or-later, combined into a GPL-2.0 kernel, which GPL-2.0-or-later
permits.
The toolchain and every upstream package it builds (busybox, dropbear,
iproute2) keep their own upstream licenses. See
[`docs/LICENSING.md`](docs/LICENSING.md) for the full breakdown.

## Related repositories

- [odi-ui](https://github.com/AndreZiviani/odi-ui) — the web UI, `confd`.
- [odi-sfp-exporter](https://github.com/AndreZiviani/odi-sfp-exporter) — the
  Prometheus exporter, `metricsd`.
- [odi-toolchain](https://github.com/AndreZiviani/odi-toolchain) — the
  prebuilt build images shared by all three.
