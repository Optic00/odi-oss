# What this firmware does differently from the stock image

The stock ODI DFP-34X-2C2 firmware works. This project does not repack it —
it builds a complete replacement from source — and along the way that also
fixes or improves a number of things the stock firmware gets wrong, leaves
out, or does closed. This is the detail behind the table in the top-level
`README.md`; each point below is something you can read or run in this repo
to confirm for yourself.

## Fully open

Nothing in this build is proprietary. The kernel is mainline Linux plus our
own patches and drivers (`docs/KERNEL.md`); the toolchain is built from
source by the odi-toolchain repository and published as container images
pinned by digest, which any build can reproduce from that source
(`toolchain/README.md`); busybox, dropbear and iproute2 are unmodified
upstream releases; everything in `src/` is ours. `docs/LICENSING.md` is the
full accounting, package by package. Contrast this with the stock firmware,
which is closed and not distributable — the reason this project builds
firmware rather than repacking or redistributing anything from the device.

## Kernel

**Linux 6.18**, the current longterm release, instead of the 2.6.30 the
stock firmware runs. Everything the datapath needs — the switch fabric, the
GPON MAC, the OMCI transport, the CPU-port NIC — is our own driver, an
independent implementation built straight into the kernel; there is no proprietary
kernel module in this image at all. `docs/KERNEL.md` has the CPU, board and
driver detail.

**Cheap to carry forward.** The port touches 19 lines in 6 mainline files
(`tools/kernel-footprint.sh`): the CPU probe case, the board's System type
entry, and the Kconfig and Makefile lines that reach our overlay. Every
other line is a file of our own in `kernel/extra/`, copied over the tree.
The CPU is built as mainline's own `CPU_R3000`, so no core exception, TLB
or context-switch code is patched.

**No trust in what the hardware was left doing.** The loader leaves the
NIC's receive DMA running into memory the kernel then hands out; our board
code stops it before the kernel owns a page. The icache, which does not
snoop, is invalidated whenever a page gets new contents for a process. The
PON packet-buffer windows are reserved exactly where the hardware puts
them. `docs/KERNEL.md` has each one.

**Smaller and leaner.** A size-optimised config with every syscall family
nothing in the rootfs can reach turned off (the config says why, option by
option, and which ones are kept on purpose), and the register replay tables
loaded from `/lib/firmware/odi/` only while they are applied rather than
compiled in, and the subsystems for hardware the board does not have (VT,
input, HID, the FPU emulator, power management) off: the kernel is 15% under
its 1328 KB partition (210 KB spare), and about 484 KB of RAM is not held by
the tables.

**A hung kernel resets itself, and so does a stuck boot script.** The
watchdog kicker (`odi_wdt.c`) tracks a userland-confirmation deadline in
addition to "the kernel is alive," so a boot that hangs before its init
scripts finish still gets the stick back to a known-good image, not just a
kernel that panics outright. See `docs/KERNEL.md` and `docs/FLASHING.md`.

**A boot you cannot see is not a boot you cannot debug.** There is no serial
console on this device; the DRAM ramlog console (`odi_ramlog.c`) mirrors
every kernel console line into DRAM that survives a watchdog reset, so a
failed trial can be read back from the other slot afterward: with
`tools/memprobe` from the stock image, or `cat /proc/odi_ramlog_prev` from
the next boot of this one, which saves the previous boot's pages before it
writes its own and says which boot, slot and build they came from. A
process killed by a signal prints its registers there
(`print-fatal-signals=1`), and early-boot crumbs say how far a kernel got
that died before its first console line.

**One-shot trial boots by design.** `fwu.sh` refuses to write the slot you
are running from or the slot the bootloader still trusts, and a freshly
flashed image is inert until `nv setenv sw_tryactive <slot>` boots it
exactly once. `docs/FLASHING.md` is the full procedure.

## OMCI and the datapath

**Our own OMCI daemon (`omcid`)** answers the OLT and drives GPON
provisioning end to end: MIB handling, bridge/VLAN setup, T-CONT and GEM
queue programming, over our own in-kernel switch/GPON drivers — no
proprietary daemon or kernel module in the path. `docs/TOOLS.md` covers
`omcid` and the tooling around it (`omcli`/`omcicli`, `omciprobe`,
`omcicap`).

**Optics (DDM) support**: our own SFF-8472 reader (temperature, voltage,
bias current, tx/rx power), exposed through `diag` and the Prometheus
exporter.

**IGMP snooping (`igmpd`)**: the group state machine and the switch
programming (static L2 multicast entries through `/dev/odi_sw`) are written
and host-tested, but it is shipped and not started: on this kernel no IGMP
frame reaches it, and it cannot yet send a trapped report or query on
(`docs/TOOLS.md` has why that rules out starting it). Treat multicast as an
area still maturing rather than a finished feature.

**The MAC table.** `diag l2-table get all` reads the switch L2 lookup table
back -- learned addresses with the port each was learned on, and multicast
groups with their members -- and the web UI shows it.

## The GPON link

**Loss of fibre is followed.** A loss of signal or of framing in O5 now
drives the ONU state machine as G.984.3 Table 10-1 says (to O6, then O1
when TO2 runs out, and back to O2 when the light returns), so the OLT ranges
the stick again instead of finding it still in O5. `docs/TOOLS.md`, "Link and
provisioning events"; `test/odi_gpon_los_test.c`.

**The GPON tables are cleaned at every boot.** The GEM-port and Alloc-ID
CAMs, the flow-type words and the downstream multicast filter come up full
of garbage after a power cut. The boot waits for the MAC reset, clears every
row, reads it back and logs what it found; the switch ACL tables get the same
treatment. `docs/SWITCH.md`, "GPON boot init" and "Platform init";
`test/odi_gpon_init_clean_test.c`, `test/odi_switch_acl_clean_test.c`.

**Downstream encryption is real.** Every Request_Key gets a fresh key from
the kernel RNG, and an Encrypted_Port-ID that arrives before OMCI has created
the GEM port is kept and applied (`docs/SWITCH.md`, "Downstream AES").

**The line counters have one reader.** The GPON MAC counters clear on read,
so the driver samples them once a second into 64-bit totals in
`/proc/odi_gpon/` (`docs/TOOLS.md`, "/proc/odi_gpon/: the link counters").

**The laser is not lit blind.** It goes on only for the laser driver chip
this image is tested with, and GPON activation is refused otherwise
(`docs/BOOT.md`, stage 13; `ODI_LASER_UNVERIFIED` in `docs/SETTINGS.md`;
`test/odi_board_test.c`).

**The host port does not send PAUSE** toward the host, so a stuck upstream
cannot stall the host's NIC (`docs/SWITCH.md`, "Host port PAUSE").

## The `diag` CLI

**Our own command set, over our own kernel.** `diag` is a hand-written
command table, each with its own help: the transceiver's DDM
readings, GPON state, alarms and GEM flows, per-port MIB counters, the L2
(MAC) table, and switch-core register access (`diag help` lists them, `src/diag/README.md`
explains each). The commands the Prometheus exporter runs keep the stock
CLI's syntax and output byte for byte, so one exporter build reads both
this firmware and the stock one; `make test-diag` pins that output under
qemu against golden files.

**Batched on stdin, always exits.** `diag` reads commands from stdin one per
line and stops cleanly at EOF, rather than requiring a terminal; see
`docs/TOOLS.md` for the one thing to watch — give it a way to see EOF, or
wrap it in `timeout`.

## Access

**A current dropbear** with `scp` in both directions (`scp -O` from a
modern OpenSSH client — the legacy scp protocol is all this dropbear speaks,
having no SFTP subsystem), an ed25519 host key kept on the config partition
across reflashes, and SSH-key management from the web UI or its API.
`docs/ACCESS.md` has the full detail.

**A per-build root password** (`out/image/root-password-<version>.txt`)
instead of a fixed shared default; no telnet (ssh is the only network
login), and every way in fails **open** — a missing config file gives you
the default credential, never a locked door, because the config partition
is exactly what a factory reset erases.

## Web UI and exporter

**`confd`**, our own web UI (a separate project), on the same port the
stock firmware's UI uses, reading the OMCI daemon through the same command
shapes as `omcli`.

**Settings that say what they cost.** Of the 184 keys in the config store
the stock firmware shares with this image, the UI offers for editing only
the 22 stock keys this image acts on (and its own), each marked with what applying it costs:
LIVE, SERVICE RESTART, INTERRUPTS INTERNET or REBOOT, the last two behind a
confirmation. The other 161 are shown read-only and still round-trip
through backup and restore. `apply.sh` applies the management addresses
live and the OMCI settings without a reboot (it re-ranges the ONU), the
firmware page writes an uploaded image to the inactive slot
(`fwu_starter.sh`), and the OLT identity keys are reported only behind an
explicit switch. `docs/SETTINGS.md` has every key and control. **`metricsd`**, a Prometheus exporter (also a separate
project), including `gpon_omci_services` — how many services the OLT has
actually provisioned, so a stick that is optically up but not provisioned
is visible as such rather than reading as healthy.

**A look back without Prometheus.** `history.sh` keeps the last two hours
of the line, one sample a minute (optics, ONU state, alarms, fibre-side
traffic, BIP and FEC errors, O5 drops, CPU, memory), in a RAM ring of about
20 KB that the Status tab of the web UI draws and the diagnostics bundle
carries (`docs/TOOLS.md`, "The minute history"). A drop out of O5 between
two samples still shows, from the driver transition counters.
`test/history_test.sh` checks every key against the diag reply the exporter
contract pins, a stock slot, a failed diag, LOS and the ring.

## Resilience

A 20 MB `scp` into `/tmp` exhausted RAM on a running stick (`/tmp` was
ramfs, unbounded and unreclaimable): the OOM killer took dropbear, confd
and omcid, none of them restarted, and the box stayed unmanageable until a
power cycle, even though the hardware datapath kept forwarding on its own.
Fixed several ways, together (`docs/SETTINGS.md`, "Resilience" and
"Watchdog rules"): `/tmp` and `/var` are size-capped tmpfs instead of
unbounded ramfs, so a full `/tmp` gives `ENOSPC` rather than taking the box
down with it; `oom_score_adj` biases the OOM killer away from omcid,
dropbear and confd; those four critical daemons (omcid, dropbear, confd,
metricsd) restart automatically the instant they die, forked and tracked
by busybox init itself (`/etc/inittab` `respawn` entries, not a hand-rolled
supervisor loop);
and the kernel's own odi_wdt watchdog -- the only owner of the hardware
watchdog, with no separate userland process guessing at anything from
`/proc` -- stops kicking (so the board resets) if omcid misses its own
ping deadline (60 s, `/proc/odi_wdt/ping`) or if `MemAvailable` stays below
a floor for several consecutive checks, in addition to the one-shot boot
confirmation (`/proc/odi_wdt/userland_ok`) this image already had. A first
version of the periodic check (v1.0.2, a separate userland health-kicker
process) shipped briefly and was withdrawn: it could withhold its kick
without the kernel enforcing the deadline, so a hang it correctly detected
still did not reset the board (`CHANGELOG.md`, v1.0.2).

## Build and verification

Every ELF that reaches the image — kernel, packages, our own tools — passes
an instruction audit (`packages/isa-audit.sh`, `packages/isa-allowlist.sh`)
against what the RLX5281 actually implements, as a build gate rather than a
report: a binary carrying an instruction this CPU traps on does not ship.
`make test` runs lint, the host-side unit tests, `diag`'s tests (the
exporter contract included) and the OMCI daemon under qemu (about 130
checks) without touching a stick, and CI runs the same on every push and
pull request (`.github/workflows/ci.yml`); `make test-qemu` goes further,
booting the real rootfs full-system on a stock kernel and checking
ssh/web UI/exporter plus the resilience scenarios above end to end
(`docs/HACKING.md`); `make image-all` builds the whole image from a clean
clone. See `docs/BUILDING.md` and `docs/CROSS-COMPILING.md`.
