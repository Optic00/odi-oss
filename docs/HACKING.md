# Hacking on odi-oss

For a developer who has never seen this project and wants to change it.
The rest of the docs describe the firmware as a user sees it; this one is
about how it is built, how it is kept from bricking a stick, how it is
tested, and which traps have already cost someone a day. Where another
document already says something well, this one links to it rather than
repeating it. Two things are complete here and nowhere else: the recovery
mechanism ([Recovery](#the-recovery-mechanism)) and the list of traps
([Gotchas](#gotchas)). [Feature toggles](#feature-toggles) is the
developer reference for every switch, parameter and build variable.

[`AGENTS.md`](../AGENTS.md) holds the house rules in short form. Read it
too; nothing here overrides it.

## Contents

1. [Orientation](#orientation)
2. [Building](#building)
3. [The recovery mechanism](#the-recovery-mechanism)
4. [Testing](#testing)
5. [Gotchas](#gotchas)
6. [Feature toggles](#feature-toggles)
7. [Making a change](#making-a-change)

## Orientation

### What the image is

A complete replacement firmware for the **ODI DFP-34X-2C2**, a GPON ONU in
an SFP stick: Linux **6.18** (the current longterm line), our own drivers
for the switch, the GPON MAC, the CPU-port NIC and the OMCI transport, our
own userland daemons and CLIs, and our own gcc/binutils/uClibc-ng
toolchain. Nothing proprietary is shipped or linked. The stock firmware
(Linux 2.6.30, closed) normally stays in the other boot slot.

| | |
|---|---|
| SoC | Realtek RTL9602C (the die; marketed as RTL9601D, [`docs/kb/rtl9602c-chip-identity.md`](kb/rtl9602c-chip-identity.md)) |
| CPU | Lexra RLX5281: MIPS-I plus a few MIPS-II instructions, big-endian, no FPU, R3000-style exceptions and caches |
| RAM / flash | 32 MB DRAM / 8 MB SPI NOR |
| ports | one UNI (the SFP edge, SerDes to the host), the PON, and the CPU port (the stick's own IP stack) |
| console | a UART on the board, no connector: in practice, no serial console |
| boot | U-Boot, two kernel+rootfs slots, a one-shot trial boot, a hardware watchdog |

The flash map comes from the `mtdparts=` table U-Boot passes (and the
fallback in `kernel/618/config`): `boot` 256K, `env` and `env2` 8K each,
`config` 240K (jffs2, shared by both slots), `k0` 1328K, `r0` 2512K, `k1`
1328K, `r1` 2512K. Slot 0 boots with `root=31:5`, slot 1 with `root=31:7`.

### The boot, in one paragraph

U-Boot picks a slot ([below](#the-recovery-mechanism)), decompresses the
LZMA uImage and jumps to `kernel_entry` with a per-slot command line in
`argv`. The board code (`kernel/extra/arch/mips/rtl8686/`) stops the NIC DMA
the loader left running, installs the RLX5281 cache code, reserves the PON
DMA windows and the two ramlog pages, and the drivers under
`kernel/extra/drivers/net/ethernet/odi/` come up built in (there are no
modules). `/etc/init.d/rcS` then mounts the config partition, writes the
switch SDK-init verbs to `/proc/odi_init`, brings up `br0` with
`/etc/scripts/network.sh` and returns. Init then starts the daemons (syslogd,
klogd, ntpd, metricsd, confd, dropbear, omcid) as `respawn` entries, and
`/etc/init.d/rcS.pon` runs beside them: it drives the optics and the PON
steps, **confirms userland to the watchdog** after the switch init, and
activates the GPON MAC once `omcid` has registered.
`docs/BOOT.md` has each stage and why it is where it is;
`/etc/init.d/rcS.dev` adds the development aids.

### Repo layout

| path | what | read first |
|---|---|---|
| `Makefile` | the whole build and the gates; `make help` | `lint:` and `test-host:` |
| `toolchain/` | gcc 16.2.0, binutils 2.47, uClibc-ng 1.0.59 for this CPU, built in a container into a Docker volume | [`toolchain/README.md`](../toolchain/README.md) |
| `kernel/618/fetch.sh`, `mainline/` | fetches and GPG-verifies linux-6.18.58; `mainline/` is the pristine tree (gitignored) | |
| `kernel/618/patches/` | **only** edits to files mainline already has: 19 lines in 6 files today (`tools/kernel-footprint.sh`) | [`docs/KERNEL.md`](KERNEL.md) "Patches and the overlay" |
| `kernel/618/debug/` | debug-only mainline edits, applied with `CRUMBS_CORE=1` | |
| `kernel/618/config` | the seed config, completed by `olddefconfig`; every line has its reason | "The config" in KERNEL.md |
| `kernel/extra/` | every file of our own, at its path in the kernel tree: board, cache, irqchip, timer, SPI NOR, `drivers/net/ethernet/odi/` | [`kernel/extra/README.md`](../kernel/extra/README.md), the odi driver `README.md` |
| `kernel/build.sh` | builds the kernel in a container with our toolchain | its header |
| `src/` | our own userland: `diag`, `omci` (`omcid`, `omcli`/`omcicli`, `omciprobe`, `omcicap`), `igmp`, `nv`; freestanding binaries | [`src/diag/README.md`](../src/diag/README.md), [`src/omci/README.md`](../src/omci/README.md) |
| `src/fetch-releases.sh` | pulls `metricsd` and `confd`, separate projects, as pinned checksummed releases | |
| `packages/` | upstream busybox, dropbear, iproute2, statically linked against our uClibc-ng; the ISA audits | `packages/isa-audit.sh` |
| `rootfs/skeleton/` | `/etc` (rcS, services, network.sh, apply.sh, flash, ...) and the register replay tables in `lib/firmware/odi/` | [`rootfs/README.md`](../rootfs/README.md), then `etc/init.d/rcS` |
| `image/` | squashfs + uImage into a flashable tarball; `fwu.sh`, the on-device flasher | [`image/README.md`](../image/README.md) |
| `test/` | host-side unit tests (C unity builds against a register mock, shell tests of the on-device scripts) and manual qemu harnesses | [Testing](#testing) |
| `tools/` | `remote-build.sh`, `kernel-footprint.sh`, `check-inline-quotes.sh`, `check-kernel-hardening.sh`, `fetch-refs.sh`, `bump-deps.sh` (moves the pinned upstream versions), `compare-images.sh` and `objdiff.sh`, `generate-sbom.sh`, `release-notes.sh`, `config-backup.sh`; `memprobe` (physical memory from any kernel), `regdump`, `regtrace` (offline tooling for the register captures the replay tables came from) | each directory's `README.md` |
| `docs/` | user and developer docs; `docs/kb/` field notes on the hardware and the stock firmware | [`docs/kb/README.md`](kb/README.md) |

### Where to start reading

- **Kernel or drivers**: `docs/KERNEL.md` end to end, then
  `kernel/extra/drivers/net/ethernet/odi/README.md`, then the driver you
  care about and its host test in `test/`.
- **Boot and userland**: `rootfs/skeleton/etc/init.d/rcS` with
  `docs/BOOT.md`, then `rcS.pon` and `etc/scripts/rcs-lib.sh` (the PON
  steps), `rcS.dev`, `docs/TOOLS.md`, `docs/SETTINGS.md`.
- **OMCI**: `src/omci/README.md`, `docs/REFERENCES.md` for the ITU-T
  specs (`tools/fetch-refs.sh` fetches them), and the OMCI notes in
  `docs/kb/`.
- **A tool of your own on the stick**: `docs/CROSS-COMPILING.md`.

## Building

`docs/BUILDING.md` is the full reference; this is the working summary.

### Prerequisites

Docker, bash, GNU make, git, `curl`, `xz`, `gpg`, `gpgv` and `shasum` (the
kernel fetch), and for the host tests a C compiler, Python 3 and `shellcheck`.
`make releases` downloads public releases with `gh` when installed, or
`curl` otherwise; no login or token is required
([confd and metricsd](#confd-and-metricsd)).
Everything that compiles for the target runs in containers. The toolchains
are prebuilt images, pinned by digest in `toolchain/images.env` and pulled
on first use ([Toolchain images](#toolchain-images)). The uclibc one is
`amd64`: on Apple silicon or any other non-x86_64 host it runs under
emulation, which is slow for the kernel. Use a remote build host
([below](#remote-builds)).

### Toolchain images

Both come from the odi-toolchain repository
(<https://github.com/AndreZiviani/odi-toolchain>), which builds and
publishes them; `toolchain/README.md` has what each contains. Nothing here
builds a compiler any more.

The packages are public: no login is needed. A failed pull says so and
prints the fallback: build the images from source in the odi-toolchain
repository and export `OSS_IMAGE=odi-toolchain-uclibc:local` /
`DIAG_IMAGE=odi-toolchain-freestanding:local`.

To move a pin: tag a new version in odi-toolchain, take the digest from its
publish run, change `toolchain/images.env`, rebuild, and compare the image
file by file against one built with the old pin.

### From a clean clone

    git clone git@github.com:AndreZiviani/odi-oss.git && cd odi-oss
    make image-all                 # out/image/<version>.tar, a few minutes on an 8-core x86_64 host

The toolchain images bring their own kernel headers, so `kernel-tree` no
longer gates them; `image-all` still runs it first, since `kernel` needs
it. `make image-all` runs, in order: `kernel-tree` (fetch and verify
linux-6.18.58, pack it into `build/kernel-618/tree.tar`), `toolchain` (pull
the two pinned images, under a minute), `toolchain-audit`, `kernel`
(2 min), `packages`, `src`, `releases`, `image`. After the first build
every target skips work it has already done; a second `make image-all`
takes about a minute. `docs/BUILDING.md` has the per-step timings.

### Targets

| target | what | output |
|---|---|---|
| `make kernel-tree` | `kernel/tree.sh`: fetch (`kernel/618/fetch.sh`) and pack the pristine tree; run by `kernel` (the toolchain is prebuilt and no longer needs it) | `kernel/618/mainline`, `build/kernel-618/tree.tar` |
| `make toolchain` / `toolchain-audit` | pull the pinned toolchain images; the audit re-proves the uclibc target libraries carry no instruction this CPU traps on | the images in `toolchain/images.env` |
| `make kernel` | `kernel/build.sh` | `build/kernel-618/{uImage,vmlinux,System.map,config}` |
| `make busybox`, `make packages` | busybox, then dropbear and iproute2, static, ISA-audited | `out/` |
| `make src` | our freestanding binaries | `out/bin/` |
| `make releases` | `src/fetch-releases.sh` | `out/bin/{metricsd,confd}`, `out/confd-assets/` |
| `make image` | `image/build.sh` (runs `make src` first; names every other missing input and stops) | `out/image/<version>.tar`, `root-password-<version>.txt` |
| `make image-all` | all of the above, in order, from a clean clone | |
| `make test` | `lint`, `test-host`, `test-diag`, `test-omci` | [Testing](#testing) |
| `make clean` / `distclean` | `build/` and `out/`; also the kernel tree and `dl/` | the Docker volumes survive both |

### Docker volumes and images

| name | kind | what |
|---|---|---|
| `odi-kbuild-618` | volume | the kernel build tree (`VOL=`); incremental between runs |
| `odi-toolchain-uclibc` | image | the uclibc-ng toolchain at `/opt/oss` and the kernel and package build container (`OSS_IMAGE=`, `packages/oss-env.sh`); pinned in `toolchain/images.env` |
| `odi-toolchain-freestanding` | image | Debian `gcc-mips-linux-gnu` plus `qemu-mips-static`: builds `src/`, runs `test-diag` and `test-omci` (`DIAG_IMAGE=`); pinned in `toolchain/images.env` |
| `odi-oss-image` | image | squashfs and tar tooling for `image/build.sh` |

`docker volume rm odi-kbuild-618` is the only way to drop the volume;
nothing in the tree depends on its exact contents. Every name in the table
has an override (`VOL`, `OSS_IMAGE`, `DIAG_IMAGE`, `TOOLS_IMAGE`), so two
builds on one Docker host can share nothing: `docs/BUILDING.md`, "Several
builds on one Docker host".

### Remote builds

    ODI_REMOTE=user@buildhost tools/remote-build.sh 'make image-all'

rsyncs the working tree (tracked and untracked, so uncommitted changes
build), runs the command there, and copies `out/image/*.tar`, the password
files and build logs back. `ODI_REMOTE` has no default; `ODI_REMOTE_DIR`
defaults to `/root/odi/odi-oss`. The kernel tree and `dl/` are excluded
from the sync (the tree is gigabytes); the first build on the remote host
fetches them. Downloads go to `ODI_REMOTE_DL` (default `dl/` beside
`ODI_REMOTE_DIR`, exported there as `DL`), shared by every tree under that
parent. `make releases` needs no token there either; an optional
`GITHUB_TOKEN`, if you want the higher rate limit, is passed to the remote
command on stdin. The command is any shell line, so
`'VERSION=odi-oss-260925-mine make kernel image'` works too.

### Iterating

- **Kernel**: an edit to an overlay file rebuilds incrementally. Adding or
  removing an overlay file, or changing a patch, the seed config or
  `kernel/build.sh`, re-extracts the tree. `FRESH=1` forces that.
  `COMPILE_ONLY=1` stops at `vmlinux` (no uImage), for disassembly checks.
- **Image**: `KERNEL=` and `KCONFIG=` point `image/build.sh` at a kernel
  built elsewhere; `VERSION=` names the build (and should match the
  `VERSION=` the kernel was built with, so the ramlog build id matches the
  image); `ALLOW_PARTIAL=1` builds without an optional package or confd
  assets. [Feature toggles](#build-variables) has every variable.
- **The root password** is generated per build and written to
  `out/image/root-password-<version>.txt`; `ROOT_PW=` chooses it,
  `ROOT_PW=none` leaves it empty, `ROOT_PW=locked` ships no password at all
  and starts dropbear with `-s` (`.github/workflows/release.yml` builds
  every release this way). It is baked into the read-only rootfs, so keep
  the file for as long as that image runs anywhere. All three shapes are
  `image/gen-root-account.sh`, tested by `test/root_pw_test.sh`.

### confd and metricsd

The web UI (`confd`, the odi-ui project) and the Prometheus exporter
(`metricsd`, the odi-sfp-exporter project) are separate repositories with their
own releases. `src/fetch-releases.sh` downloads the pinned tags
(`CONFD_TAG`, `METRICSD_TAG`; the defaults are in the script) with `gh`
when it is installed and logged in, otherwise with `curl` (`USE_CURL=1`
forces it); both repos are public, so neither path needs a token, verifies
each asset against the release's `SHA256SUMS`, caches them in
`dl/releases/`, and records the tags in `out/bin/releases.env`, which
`image/build.sh` copies into `/etc/odi-build`.

To ship a **local confd build**, set `CONFD_TAG` to the empty string and
point `CONFD_BIN` at the binary; its web assets are taken from `web/` and
`schema/` in the parent of the binary's directory (`odi-ui/build/confd`
means `odi-ui/`), or from what `CONFD_ASSETS=` names: an odi-ui checkout, or
the `build/confd-assets.tar.gz` its `make assets` builds. Relative paths are
taken from where you run `make`. If the binary or its assets cannot be found,
`make releases` stops rather than build an image around an older confd left
in `out/`:

    CONFD_TAG= CONFD_BIN=../odi-ui/build/confd make releases image

A **local metricsd build** is one self-contained binary: point
`METRICSD_BIN` at it (`odi-sfp-exporter/build/metricsd`) and the release
is not fetched; the manifest then says `exporter=local`:

    METRICSD_BIN=../odi-sfp-exporter/build/metricsd make releases image

confd reads `/etc/confd/` for the whole UI and its `.tsv` tables; a confd
without them starts and answers 404 to everything, so check the "asset
files" line the script prints. A UI change that adds a setting has to land
in odi-ui `schema/settings.tsv` and `docs/SETTINGS.md` together.

### What CI runs

`.github/workflows/ci.yml`, on pull requests and pushes to `main`: a
`changelog` check (a pull request has to touch `CHANGELOG.md`, unless it is
docs or CI only, or labelled `no-changelog`), `make lint`, `make
test-host`, `make test-diag` and `make test-omci`, each as its own parallel
job; `test-omci` is a matrix with one job per suite
(`make test-omci-<suite>`, after `make test-omci-build`). Two more jobs
run on every push: `clean-build` (`make kernel-tree` and `make src`, then
checks that `make image` on a partial tree fails closed and names what is
missing) and `test-qemu` (`make test-qemu`, below), whose last step is the
rcS trace goldens (`make test-rcs`). On release tags (`v*` through
`release.yml`, which calls this workflow; `odi-oss-*` directly) and manual
runs it adds `make src` on its own and a
`reproducible` job: `make image-all` twice in two checkouts, compared byte
for byte with `tools/compare-images.sh`. The kernel and the full image are
**not** built on a pull request: a kernel build is tens of minutes and
nothing in it runs without a stick. Build the kernel yourself before you
push a kernel change.

Every job that builds one of our binaries also checks its hardening: each
Makefile in `src/` runs `tools/check-hardening.sh` on the binary it ships
(stack protector present and seeded, no executable stack, no writable and
executable segment), and `make test-diag` runs the stack protector for real
under qemu (`make -C src/diag ssp-test`). `docs/CROSS-COMPILING.md`,
"Hardening the freestanding binaries", has the matrix.

`.github/workflows/release.yml`, on a `v*` tag only, first checks that a final
tag's commit is an ancestor of `origin/main` (`tools/check-release-ancestry.sh`;
pre-release tags such as `v1.2.2-beta.1` are exempt, they test unmerged work), then calls `ci.yml` as a reusable
workflow on that same commit and publishes nothing unless every job of it,
`test-qemu`, the rcS goldens, `full` and `reproducible` included, succeeded.
Only then does it build the full
image (`VERSION=` from the tag, `ROOT_PW=locked`) and publishes it as a
GitHub release: the tarball, `SHA256SUMS`, an SBOM, build-provenance
attestations, and notes from `tools/release-notes.sh` (the tag's own
`CHANGELOG.md` section plus a flash-and-first-login paragraph).
`.github/workflows/dependency-bump.yml` runs `tools/bump-deps.sh` every
Monday and opens a pull request only if the tests pass against the new pins.
A manual run of `release.yml` (`workflow_dispatch`) is a dry run of the gate:
the ancestry check and the whole CI call on the chosen ref, and never the
release job.

Workflow permissions: every workflow defaults to `contents: read`, and every
checkout that does not push uses `persist-credentials: false`, so code run by
a test job holds no write token. Write scopes sit on the one job that needs
them: `release.yml`'s `release` job (`contents: write` to publish,
`id-token: write` and `attestations: write` for the attestations) and
`dependency-bump.yml`'s `bump` job (`contents: write` to push its branch,
`pull-requests: write` to open the PR). Every job also sets
`timeout-minutes`.

## The recovery mechanism

Everything about working on this device rests on one fact: **a new image is
only ever booted once, on trial, and any failure puts the stick back on the
image that was already trusted.** Understand this section before flashing
anything. The user-facing procedure is `docs/FLASHING.md`; this is the
mechanism behind it.

### Two slots and three variables

Each slot is a kernel partition and a rootfs partition (`k0`/`r0`,
`k1`/`r1`). The config partition is shared. The U-Boot environment (`env`,
with a redundant copy in `env2`, read and written by our `nv`) decides
which slot boots:

| variable | meaning |
|---|---|
| `sw_commit` | the trusted slot: every ordinary boot goes here |
| `sw_tryactive` | `0` or `1`: boot that slot **once**; `2`: no trial pending |
| `sw_active` | U-Boot's record of what it last booted; never set it by hand |

`bootcmd` is (read off a stick with `nv getenv`; the full text is in
[`docs/kb/rtl9601-uboot-trial-boot.md`](kb/rtl9601-uboot-trial-boot.md)):

    bootcmd           = if sw_tryactive == 2; then boot_by_commit; else boot_by_tryactive; fi
    boot_by_commit    = boot the slot named by sw_commit
    boot_by_tryactive = setenv sw_tryactive 2; setenv sw_active <n>; saveenv; run en_wdt; boot slot <n>

The trial path writes `sw_tryactive` back to `2` and saves **before** it
arms the watchdog and jumps, so a trial cannot loop: whatever happens
next, the following boot takes `boot_by_commit`. Only `0` is tested for
slot 0; any other value that is not `2` boots slot 1, so write `0` or `1`
and nothing else.

### What sends a trial back

| event during a trial | next boot |
|---|---|
| kernel panics, hangs, never mounts root | the watchdog U-Boot armed resets the board: committed slot |
| kernel alive, but rcS never confirms within 120 s of uptime | `odi_wdt` forces the reset: committed slot |
| `reboot`, `reboot -f`, the web UI's reboot | committed slot |
| power cycle | committed slot (and the ramlog is lost) |
| image confirms, then misbehaves (no network, no ssh) | **stays up** until you power-cycle it |

`reboot` resets at once: odi_wdt registers a restart handler that stops
the NIC DMA, then forces the watchdog reset (enable alone, then a kick;
the kick is what restarts the count), about 0.3 s. Reboot to the stock
image answering again is about 73 s, most of it U-Boot and the stock boot.
`halt` and `poweroff` do not stop the board: the stick has no power
switch, so the watchdog is re-armed and resets it about 42 s later
(docs/KERNEL.md, "Reboot, halt and power-off").

On a **committed** slot of ours the same watchdog and deadline are armed
(`odi_wdt` arms the watchdog itself on every boot, not only on trials), so
a hang there becomes a reboot loop on that slot. That is the cost of
committing an image you have not watched come up.

### The userland deadline

`odi_wdt.c` kicks the hardware watchdog from a kernel thread, so a kernel
that is merely alive would never be reset. It therefore also holds a
deadline: unless something writes `1` to
`/proc/odi_wdt/userland_ok` within `ODI_WDT_USERLAND_DEADLINE_S`
(120 s) of uptime, it stops the NIC DMA (`wdt_pre_reset_hook`) and forces
the reset itself. The directory was named after the stock firmware
(`luna_watchdog`) until a cleanup gave it our own name, once rcS no longer
needed to share a line with the stock image.

`rcS.pon` writes it (`confirm_watchdog()` in
`rootfs/skeleton/etc/scripts/rcs-lib.sh`, called from `pon_steps_default`)
after the optics pins, the I2C, laser, GPON verbs and the switch init, and
**before** `gponsn`, `gponpw` and `gponact`, so a problem on the optics
side cannot cost it. It refuses to confirm when `/var` or `/var/tmp` is not
tmpfs, when the config partition is not mounted, when `network.sh
configured` finds no configured management address, or when the switch init
failed. It does not look at reachability: a trial whose management path
comes up wrong stays up, and you power-cycle it. For development there is
`/etc/config/confirm-arp` (read by `rcS.dev`; it holds the IPv4 address to ARP): with it, rcS
confirms only after an ARP reply from that address, so an unreachable trial reverts
by itself **and keeps its ramlog** (a watchdog reset preserves DRAM; a
power cycle does not). Never leave `confirm-arp` on a stick whose host may stop
answering there: every boot then resets at 120 s (`nv commit` refuses
while it exists; `--force` overrides). An empty or invalid file is no
target and the boot confirms as usual. The confirmation and the
deadline are one mechanism; change neither without reading `rcs-lib.sh`,
`rcS.dev` and `odi_wdt.c` together.

This one-shot boot deadline is one of three independent rules odi_wdt
enforces after boot too -- a per-client ping deadline (omcid pings its own
60 s deadline from its own main loop, `/proc/odi_wdt/ping`) and a
kernel-side `MemAvailable` floor -- `docs/SETTINGS.md`, "Watchdog rules"
has the full design and why a v1.0.2 userland version of the second rule
was withdrawn.

### Committing a trial

`sw_commit` is the only thing that makes an image permanent, and writing it
throws away the fallback: a committed image that later crashes resets into
itself, with no route back to the other slot. Write it by hand, from the
running trial image, with `nv commit <slot>` (both environment copies,
verified), after you have watched the trial work -- and never from a
script, a boot loop or a soak: every trial boot is its own `nv setenv
sw_tryactive <slot>`, and a slot can be re-tried any number of times
without reflashing. Until it is committed the image says so: the ssh login
banner, `gpon_uncommitted` and the `OdiUncommittedImage` alert
(`docs/FLASHING.md`, "Committing").

Two further facts:

- **Something other than you can move `sw_commit`.** On one stick it changed
  by itself about 80 s after a trial booted the stock image's management
  stack (an operator-driven OMCI software-image activate and commit). Our
  `omcid` never writes the boot environment, but read `sw_commit` back
  after every trial rather than assume it.
- **`fwu.sh` refuses to write the running slot or the slot `sw_commit`
  names** (in either environment copy), and the kernel's `mtdparts` marks
  the running pair read-only, so flashing the wrong slot takes more than one
  mistake. Check `sw_commit` equals the running slot before a trial anyway:
  a trial must revert *away* from the new image, not into it.

### Reading a boot you could not see

There is no serial console, so a trial that never answered is otherwise one
bit of information. Four records survive a revert:

- **The DRAM ramlog.** `odi_ramlog.c` mirrors every console line into two
  DRAM pages no kernel maps (`0x017ff000`: the first 3984 bytes, a 32-byte
  reset reason block and a 64-byte per-boot metadata block; `0x01fff000`: a
  ring of the last 4080). They survive a watchdog reset or `reboot`, not a
  power cycle.
  - From **our** image (the next boot of ours), before its own ramlog
    writes: `cat /proc/odi_ramlog_prev` (decoded) or
    `/proc/odi_ramlog_prev_raw` (8192 bytes). The first two lines give this
    boot's counter and slot and the previous boot's counter, slot, build id
    and last early crumb, and why it reset (`reason=`, below): **check the
    slot and build id are the trial's** before reading on. It reaches one
    boot back; a stock boot in between writes nothing.
  - **Why it reset.** The kernel records the reason at the moment it knows
    it, and the next boot prints it at the end of the `previous boot:`
    line, e.g.
    `previous boot: boot=30 slot=1 build=v1.0.9 crumb=TICK/15028 reason=wdt_client:omcid`:

    | `reason=` | recorded by |
    |---|---|
    | `wdt_client:<name>` | `odi_wdt`: that registered client missed its ping deadline |
    | `wdt_mem` | `odi_wdt`: MemAvailable stayed below the floor |
    | `wdt_cpu_rx` | `odi_wdt`: the switch kept offering frames to the CPU port and the NIC took none for 30 s |
    | `wdt_userland` | `odi_wdt`: rcS never confirmed the boot (`userland_ok`) |
    | `reboot`, `halt`, `poweroff` | the reboot notifier: the `reboot` syscall path |
    | `panic` | the panic notifier |
    | `oops` | the die notifier: a kernel oops that did not panic, followed by a reset nothing else recorded |
    | `power` | nothing: neither page magic survived, so DRAM lost its contents (a power cycle, a cold boot). The line is then `previous boot: none (...) reason=power` |
    | `unknown` | nothing: the pages survived but no reason was written -- a hang the hardware watchdog caught, an emergency restart (sysrq), a panic before `early_initcall`, or a previous boot of an image older than the reason block |

    The last writer wins, so an oops that then panics reads `panic`. The
    watchdog rules still log their own line too, so the free text at the
    end of page B says the same thing in more detail.
  - From the **stock** image: push `tools/memprobe` and run
    `tools/memprobe/ramlog-read.sh` ([`tools/memprobe/README.md`](../tools/memprobe/README.md)).
    Check `MemFree` first: pushing a file larger than free RAM into the
    stock image's ramfs has rebooted a stick.
- **Fatal-signal register dumps.** `print-fatal-signals=1` is on the
  built-in command line, so a process killed by SIGSEGV, SIGBUS or SIGILL
  prints `epc`, `ra`, `Status`, `Cause` and `BadVA` into the ramlog. That
  is how the loader's running NIC DMA was found (a page of `/bin/busybox`
  overwritten by an RX descriptor).
- **Early crumbs.** `CONFIG_ODI_EARLY_CRUMBS` stores a 4-byte tag and a
  step in page B at each board hook, from `K1EN` (first instruction) to
  `TICK` (timer interrupts counting): the last pair says how far a kernel
  got that died before its first console line. `CRUMBS_CORE=1` adds crumbs
  inside mainline's `head.S`, `start_kernel` and `cpu_probe`. The trail is
  in `docs/KERNEL.md` ("Early crumbs").
- **Boot breadcrumbs on flash.** `: > /etc/config/breadcrumbs.on`, set from
  the running image before the trial, makes rcS (through `rcS.dev`) append one timestamped line
  per stage to `/etc/config/breadcrumbs`, write a network snapshot to
  `/etc/config/trial-diag.txt`, log a heartbeat and lower the lockup
  thresholds. The config partition is shared and never written by
  `fwu.sh`, so read them from the image you land back on.

A trick for telling a timed event (a deadline, a watchdog) from a stalled
instruction: if the last line printed moves between otherwise identical
trials but the uptime at death does not, it is the timer, not the code at
that line ([`docs/kb/rtl9601-dram-ramlog-console.md`](kb/rtl9601-dram-ramlog-console.md)).

### When a stick seems bricked

In order:

1. **Wait two and a half minutes.** A hung trial resets at 120 s (the
   userland deadline), and the stock image then needs its own boot time.
2. **Power-cycle it.** With `sw_tryactive` already back at `2`, every boot
   after a trial is the committed slot. (This loses the ramlog; if the
   stick still answers ARP but not ssh, a reset that preserves DRAM is not
   available from outside, so take the loss.)
3. **Once it answers**, read `nv getenv` (`sw_commit`, `sw_tryactive`,
   `sw_active`), then the breadcrumbs and the ramlog as above.
4. **Up, but no service on the stock image?** The stock firmware runs a
   PON-mode detector: after about 24 s of light without PON sync it
   rewrites `PON_MODE` in the config store to the other PON type (GPON to
   EPON) and reboots, and it then looks dead on a GPON line. Anything that
   keeps the fibre lit while the stock image cannot sync can trigger it (a
   bench fibre, a fibre fault, an experiment that stops its OMCI stack),
   and no log survives the reboot to say so. `flash get
   PON_MODE`; on a GPON stick it is `1` (as in
   `rootfs/skeleton/etc/config_default_hs.xml`), and `flash set PON_MODE 1`
   puts it back. Setting `PON_DETECT_ENABLE` to 0 stops the rewrite but not
   the detector; a stick still on the factory placeholder serial never runs
   it. Our image is GPON only and never rewrites the PON type.
   [`docs/kb/rtl9601-pondetect-pon-mode-rewrite.md`](kb/rtl9601-pondetect-pon-mode-rewrite.md).
5. **Rebooting every 120 s?** `confirm-arp` left on a stick whose host no longer
   answers at the address in it holds the confirmation back on every boot; remove it
   ([Feature toggles](#switch-files-on-the-config-partition)). An older
   image of ours also rebooted on purpose with `bdgconn-probe` there. A
   committed image of ours that never confirms loops every 120 s: if it is reachable
   inside that window, `echo 1 > /proc/odi_wdt/userland_ok` stops
   this boot's reset, and `nv setenv sw_commit <the other slot>` gets you
   off it.
6. **Both slots unbootable**: the board UART is the only way in. Every rule
   above exists to keep you from here.

## Testing

### The gates

| gate | what it proves | needs |
|---|---|---|
| `make lint` | shellcheck over every script (on-device ones as POSIX `sh`, since busybox ash runs them), the inline-quote check, `reuse lint` (REUSE copyright and license information; skipped when the tool is absent, always run in CI), and the greps for private-workspace references, machine-local paths, trial history in comments, internal plan ids and vendor source citations | `shellcheck`, git, `reuse` (optional locally; `tools/reuse-requirements.txt`) |
| `make test-host` | about 60 host tests: driver logic, the on-device scripts, the replay tables and their generators | cc, bash, Python 3 |
| `make test-diag` | diag's parser and conversions natively, then under qemu: the conversion vectors, the L2 listing golden, **the exporter contract** | Docker |
| `make test-omci` | builds `src/`, runs `omcid` under qemu-user (about 130 checks), its seven suites side by side | Docker |
| `make test-qemu` | boots the real rootfs (busybox, inittab, services, dropbear, confd, metricsd) full-system under `qemu-system-mips -M malta`; checks ssh/web UI/exporter and the resilience scenarios (see below) | Docker, `qemu-system-mips`, `busybox`/`packages`/`src`/`releases` already built |
| `make test-rcs` | the rcS action trace (what rcS executes and writes under `/proc`, for three flag sets) against its goldens; not part of `make test` | Docker with ptrace, `out/busybox` |
| `make test` | `lint`, `test-host`, `test-diag`, `test-omci` (not `test-qemu`, which needs a built rootfs), run side by side: about a minute on a native x86_64 host | |
| kernel build | `BUILD OK: <n> warnings`: 22 today, all GCC 16 "retain attribute ignored" in mainline networking files; any other warning is ours and a regression | toolchain |
| kernel ISA and footprint | `packages/isa-audit.sh build/kernel-618/vmlinux` finds nothing; `objdump -d vmlinux` has no `eret` and no `tne`; `tools/kernel-footprint.sh` does not grow without a stated reason | kernel build |
| image build | every ELF in the rootfs passes `isa-audit.sh` and `isa-allowlist.sh`; uImage and rootfs fit their partitions | full build |
| trial | [On hardware](#on-hardware) | a stick of your own |

### Host tests

The kernel drivers are written so that their logic compiles on the host.
Each C test is a **unity build**: `test/odi_switch_test.c` `#include`s
`test/odi_switch_mock.h` and then the driver `.c` files themselves
(`kernel/extra/drivers/net/ethernet/odi/odi_switch_tbl.c`, ...), so the
test runs the same lines the kernel runs. `odi_switch_mock.h` replaces
`odi_reg_read()`/`odi_reg_write()` with a flat register model covering the
switch-core, GPON and counter windows plus a write log; an address outside
every window aborts the test rather than corrupting a slot, and the mock
aborts on a recursive lock or on releasing a lock not held. Kernel-only
code sits behind `#ifdef __KERNEL__`. A new driver file gets a
`test/<name>_test.c` and a `test/<name>_test.sh` wrapper (copy one, it is
five lines), and the wrapper goes into the `test-host` list in the
Makefile.

Tests replay what real sticks did: captured register sequences
(`test/fixtures/`), the committed replay tables in
`rootfs/skeleton/lib/firmware/odi/` (parsed by the C reader and dumped by
the Python writer, so both are checked against the same bytes), a captured
GPON re-activation, and OLT command sequences from both ISPs.

The on-device shell scripts (`apply.sh`, `network.sh`, `flash`, `fwu.sh`,
`fwu_starter.sh`) are tested on the host too: they take their paths from
overridable variables (`ODI_INIT=`, `CONF=`, `IFCONFIG=`, ...), and the
tests point those at FIFOs, stubs and captured `/proc` files. Keep that
shape when you add one.

### test-diag: the exporter contract

`metricsd` sends one fixed batch of commands to `diag` per scrape and
parses the output. The same exporter build also reads the **stock**
firmware's `diag`, so those commands keep the stock syntax and output byte
for byte. `make exporter-test` (inside `test-diag`) runs the batch in
`src/diag/test/exporter.txt` through the real binary under qemu, answered
by a fake hardware layer, and compares the output with
`src/diag/test/exporter.golden` (and once more without the trailing
`exit`). If you change anything the exporter reads, the golden files fail
and you have changed the contract: fix the code, or change the exporter
first. `src/diag/README.md` ("The exporter contract") has the rules.

### Optics model

`test/odi_optics_model.h` (host build only) is a scriptable SFF-8472
transceiver behind the same modelled I2C controller `odi_i2c_test.c` and
`odi_ddm_test.c` already use (`test/odi_switch_mock.h`): two 256-byte pages
(A0h identity, A2h DDMI), the `IO_GPIO_EN`/`PIN_GPIO_SELECT` routing gate
(`docs/kb/dfp34x-optics-on-i2c-port1-gated-by-io-gpio-en.md`) and an
"absent module" mode, both surfaced as I2C NACK exactly the way the real
device does.

To script a scenario in a new test:

1. `odi_mock_reset()`, then `odi_optics_model_reset()` (installs the write
   hook and resets both pages to the isp1 capture's identity strings).
2. `odi_optics_set_a2_word(offset, raw)` for any of the five numeric DDM
   fields (`odi_ddm.c` has the offsets), `odi_optics_set_alarms(alarms,
   warnings, los)` for the alarm/warning bits and the optical LOS status
   bit, `odi_optics_set_present(0)` for an absent module.
3. Write `ODI_SW_PIN_GPIO_SELECT(0)` yourself (`odi_reg_write`) with the
   value `odi_board_optics()` writes at boot, `ODI_OPTICS_GPIO_SELECT_VALUE`
   -- the model does not assume it, so a test can also exercise the
   not-yet-routed NACK path.
4. Drive `odi_i2c_read_bytes()`/`odi_ddm_get()` as usual; a scenario that
   should NACK gets `-ENXIO` back.

Alarm and warning bits are stated directly by the scenario, not derived
from threshold math this repo does not model: a real SFF-8472 module
computes them against its own internal calibration and only exposes the
resulting bits, which is exactly what `ODI_DDM_ALARM_STATUS` reads.
`test/odi_optics_model_test.c` has the routing-gate, absent-module,
rx-power-drift and LOS scenarios end to end; the exporter contract
(above) covers the same three scenarios at the `diag` CLI layer, in
`src/diag/test/hw_fake_rx_drift.c`/`hw_fake_los.c`/`hw_fake_absent.c` and
their own golden files.

The real-hardware `diag` optics readout is not exercised inside
`test-qemu`: that harness boots a stock, generic `qemu-system-mips` malta
kernel for userland-level testing (see its own section below), never our
RTL9602C kernel, so `/dev/odi_sw` and the real `odi_i2c`/`odi_ddm` drivers
do not exist in that guest. The host-side driver tests, the diag readout
test above and the exporter contract are the coverage for this path.

### test-omci

`src/omci/qemu-test.sh`, in the freestanding toolchain image as root
(it creates `/var/config` inside the container). qemu-user cannot open
our own netlink protocol number (`NETLINK_ODI`), so there is no line side:
it injects frames on the message queue (`omcli --inject` fills in the trailer and
CRC omcid requires of every received frame; `--inject-raw` does not), and checks the MIB store, the
45-command CLI protocol and
the dump renderers against output captured from live sticks. Full OLT
sessions are not covered here; they are exercised through the kernel side
in `test/odi_switch_isp2_test.c` and on hardware.

The driver-call goldens (`drv-test.sh`, the two ISP sessions through
`omcid-drvtrace`), `resume-test.sh`, `events-test.sh`, `swdl-test.sh`,
`vlan-test.sh` and `reload-test.sh` are suites of their own. `reload-test.sh` is the
SIGHUP reload: a VLAN change rebuilt in place is compared, driver call for
driver call and connection dump for connection dump, with a fresh omcid that
had the new settings from its first frame (both ISPs, a new tag and
transparent mode); an identity change is followed through the verbs
(`omcid -i file` collects them in rcS order), the MIB clear, the hold, O5
and services (`-g file` stands for `/proc/odi_gpon/status`), the failure results and
a burst of SIGHUPs; and a dry-run daemon walks every key through its class.
`test/apply_test.sh` covers `apply.sh omci` against a stub omcid that only
traps SIGHUP, and `make test-qemu` runs it under the real inittab: one omcid
pid across every apply, a child of init, no second `event=start`.

#### Running the suites, and writing one

`make test` builds omcli and omcid, then runs its legs (`lint`, `test-host`,
`test-diag` and one per OMCI suite) side by side with `tools/test-legs.sh`,
`TEST_JOBS` of them at once (default: the CPUs; `TEST_JOBS=1` is one by one).
It is `xargs -P` and not `make -j` because the make 3.81 macOS ships hangs at
the end of a `-j` run while a process a test left in the background is alive.
Each leg is its own container or host process and keeps `/tmp`, `/var/config`
and its SysV queues to itself. A leg's output goes to
`out/test-logs/<target>.log` and is printed only when it fails, so parallel
output is never interleaved; the other legs finish when one fails, and
`make test` exits non-zero. `make test-<leg>` runs one leg with
its output streaming. A suite that fails also leaves the daemon log it was
checking in `out/test-logs/`.

The OMCI suites share `src/omci/testlib.sh`, and a new check should not sleep
for a fixed time or wait out `omcid -w` (an idle exit in **5 second units**:
`-w 2` is ten seconds). omcid answers every injected frame in order, flushes
its log after each one and exits cleanly on SIGTERM, so a suite waits for the
line it is about to check and then stops the daemon itself:

- `wait_for <file> <pattern> <seconds> [count]` polls every 0.1 s and says
  what it was waiting for on timeout (`need` also fails the suite);
- `omcid_up <log>` waits for the start line, `inject_wait <log> <frame>`
  injects one frame and waits for its answer, `sync_send <log>` waits for
  everything injected so far to be handled, and `settled <log>` also for the
  one-second quiet rebuild after a session;
- `omcid_stop <pid>` is SIGTERM and a bounded wait; `-w` on the command line
  stays as a backstop for a suite that dies, and one run in `qemu-test.sh`
  keeps the idle exit itself under test.

Keep a `sleep` only where the check is about elapsed time (a hold, a rate
window, "nothing follows within a second"), and say so next to it.

### test-qemu: the real rootfs, full system, on a stock kernel

`make busybox packages src releases` first (or `make image` once, which
does all four); then `make test-qemu`, which runs
`test/qemu/build-initramfs.sh` and `test/qemu/run-qemu.sh`.

**What it covers.** The whole busybox init chain -- `inittab`, `rcS`,
`rcS.pon`, the `svc-*.sh` respawn entries -- unmodified, packed as an
initramfs instead of the flashed squashfs+uImage (no block device or MTD
in qemu, and no need to build our own RTL9602C kernel just to boot this):
our tmpfs caps, `oom_score_adj`, busybox init `respawn` and sysctls,
dropbear/confd/metricsd/omcid for real, and
the resilience scenarios from `docs/SETTINGS.md` ("Resilience") -- filling
`/tmp` to `ENOSPC`, an OOM (a busybox-only memory hog, no compiled tool
needed), and `kill -9` on each critical daemon, checked over real ssh
(a test-only key pair that `build-initramfs.sh` generates on every run, its
public half baked into the harness's initramfs only) and real HTTP to confd and metricsd. `svc-syslogd.sh`/`svc-klogd.sh`
run here too (unconditionally); `svc-ntpd.sh` is exercised against
`NTP_SERVER`, seeded with the real `flash set` into
`/var/config/odi.conf` (a plain writable directory, there is no real config
partition here) pointing at 10.0.2.2, the qemu user-net gateway address SLIRP maps to the
host's own loopback -- `run-qemu.sh` uses the NTP server the build host
already runs on UDP 123 if there is one, otherwise starts a host busybox
`ntpd -l` for the scenario, steps the guest clock to 2000-01-01, restarts
ntpd with `apply.sh ntp` and checks the clock is corrected within 60 s.
No responder on the build host: that one scenario is skipped, logged, not
failed. A further scenario saves `SYSLOG_SERVER` through the web UI
(`POST /api/config`, then `/api/apply what=syslog`) and asserts syslogd runs with
`-R <host>`, then clears it with `flash set KEY ""` and asserts `-R` is gone: the
real `flash` against a writable config dir, not a stub. The last scenario
plants known secrets (a PLOAM and a LOID password in `lastgood.xml`, a web UI
password in `confd.auth`, all three quoted in a log file, as text and hex),
runs `/etc/scripts/diag-bundle.sh`, and asserts the bundle holds the expected
files, a redacted config copy and an exporter scrape, and none of the
secrets anywhere; with a confd that has `GET /api/diag` it asserts the same
of the web UI download (an older confd answers 404, and that half is skipped).

**What it does NOT cover**, because the kernel underneath is a STOCK
mainline build (`odi-toolchain-qemu-kernel-malta`, below), never this
repository's own kernel: `kernel/extra`, every `odi_*` driver, the switch,
the GPON MAC, `/proc/odi_init`/`/proc/odi_omci`/`/proc/odi_wdt` -- none of
it exists under qemu. rcS's own fail-open checks (`[ -w /proc/odi_init ]`
and friends) already skip every hardware step cleanly when those are
absent, exactly as they would on any kernel without them, so no
qemu-specific flag or patch to rcS was needed for that part. Two
consequences worth knowing: omcid never starts (nothing this harness can
do about it -- there is no `/proc/odi_omci` to register against), so
`/bin/diag` is swapped for a stub (`test/qemu/diag-stub.sh`, initramfs-only,
never in a flashed image) that answers metricsd's fixed command batch with
`src/diag/test/exporter.golden`, the same fixture `test-diag` checks
byte-for-byte -- so the exporter has real, checkable data to serve; and
none of the odi_wdt watchdog rules (boot confirmation, per-client ping
deadlines, the memory floor) can be exercised end to end here, only that
rcS's client registration and omcid's own ping degrade harmlessly without
`/proc/odi_wdt` -- the rules themselves are host-tested
(`test/odi_wdt_test.c`) against a fake clock instead.

**The kernel image.** `odi-toolchain-qemu-kernel-malta`
(`toolchain/images.env`, pulled like the other two toolchain images) is
linux-6.18.53, `malta_defconfig` plus a small fragment (initramfs,
devtmpfs/tmpfs, pcnet32/virtio-net, and **big-endian** -- `malta_defconfig`
defaults to little-endian, which would refuse to exec this repository's
big-endian binaries), built in the odi-toolchain repository and published
from a `qemu-malta-vN` tag so this repository's CI never has to build a
kernel from source on every run. To rebuild it after a fragment change:
in a checkout of `odi-toolchain`, `make qemu-kernel-malta` (about ten
minutes), then `QEMU_KERNEL_IMAGE=odi-toolchain-qemu-kernel-malta:local
make test-qemu` here to try the local build before tagging a release
there.

**Filesystem/mm parity with `kernel/618/config`.** v1.0.2 through
v1.0.4-rc1 shipped a flashable kernel with neither `CONFIG_SHMEM` nor
`CONFIG_TMPFS` (a capped tmpfs mount silently falls back to a ramfs-backed
stub that rejects `size=`, so `/var` and `/var/tmp` never landed) while
this qemu kernel had `CONFIG_TMPFS` on the whole time -- `test-qemu` never
had a chance to catch the mismatch. `qemu-kernel-malta/config.fragment` (in
`odi-toolchain`) now pins `CONFIG_SHMEM`, `CONFIG_TMPFS`,
`CONFIG_TMPFS_POSIX_ACL`, `CONFIG_MTD` and `CONFIG_JFFS2_FS` explicitly --
the filesystem/mm symbols `rootfs/skeleton/etc/fstab` here actually
depends on -- rather than leaving them to whatever `malta_defconfig`
happens to default to. There is no automated inheritance from
`kernel/618/config` across the two repositories; keeping the fragment a
superset of that file's filesystem/mm block is a by-hand rule, enforced in
practice by `run-qemu.sh`'s fstab-mounts assertion (below), which fails
loudly if a required mount is not the type or size `/etc/fstab` says it
should be. When `kernel/618/config` gains a new filesystem/mm symbol its
runtime behavior depends on, mirror it into the fragment in the same
change and note it here.

**test-qemu asserts the fstab mounts, not just that ssh comes up.** Right
after ssh answers, `run-qemu.sh` reads `/proc/mounts` over ssh and checks
`/var` and `/var/tmp` are both tmpfs at the size `/etc/fstab` configures
(`size=6144k`/`size=8192k` -- the kernel echoes `/etc/fstab`'s `6m`/`8m`
back in KB) -- exactly the assertion that would have caught the
`CONFIG_SHMEM`/`CONFIG_TMPFS` regression above before it ever reached
hardware. It also checks that IF a mount shows up at `/var/config` (this
harness has no MTD device, so normally none does -- "What this does NOT
cover", above), it is a real `jffs2` mount, matching what a flashed image
gets, never something softer that would mask a mismatch.

**Two things this harness needed that a flashed image does not**, both
measured, not guessed, while first bringing qemu-system-mips boots up:
QEMU's `-net user` (SLIRP) hostfwd only delivers packets to a guest
address within SLIRP's OWN subnet (`10.0.2.0/24` by default) -- the
device's real static default (`192.168.1.1`, outside that subnet) never
received a single forwarded packet, even with a custom `net=`/`host=`
override on the qemu command line, while the plain default subnet worked
immediately; `run-qemu.sh` addresses the box at `10.0.2.15` through
`network.sh`'s own `/etc/config/lan-ip` override, applied from `rcS.dev`'s
`dev_hook` (wrapped, not replaced -- see `test/qemu/build-initramfs.sh`).
And `CONFIG_DEVTMPFS_MOUNT`'s automatic `/dev` populate only runs on the
normal root-mount path, which a pure initramfs boot (`-initrd` +
`rdinit=`, no `root=`) skips entirely -- without a `mount -t devtmpfs
devtmpfs /dev` first, `/dev/null`, `/dev/console`, `/dev/kmsg` and
`/dev/urandom` do not exist, and a shell redirection like `> /dev/kmsg`
then silently CREATES them as empty regular files instead of erroring,
which is a very quiet way to lose every `crumb` and have dropbear key
generation and `seedrng` look like they succeeded while writing into fake
files. The harness's `/init` (its only rootfs addition that is not "the
real rootfs, unmodified" -- one `mount` then `exec /sbin/init`) fixes this
before the real `rcS` ever runs.

### Manual harnesses

Under `test/`, not in any make target, each with its requirements in its
header: `fwu_stock_commands_test.sh` and `fwu_stock_flash_test.sh` (does
`fwu.sh` run on the stock image's busybox 1.12.4 with 52 applets, and
flash for real), `image_tar_format_test.sh` (can that busybox's `tar` read
our tarball), `rootfs_chroot_test.sh` (the boot scripts in a qemu-user
chroot of the staged rootfs), `uimage_header_test.sh` (the header U-Boot
reads). Run them when you touch `fwu.sh`, the tarball, or rcS.
`fwu_stock_flash_test.sh` exits 77, not 0, when it skips (no stock rootfs, no
docker, no tarball): a skip is not a pass, and a wrapper that wants to
tolerate it tests for 77.

The unit tests of omcid's class 171 decode (`src/omci/respond`, `make
test-omci-respond`) and of igmpd (`src/igmp/test`, `make test-igmp`) are legs
of `make test` and a CI job (`mips-units`).

### Pure refactors: prove the machine code did not change

A change that should not change behaviour (a comment pass, a rename, a
header move, a dead `#if` arm removed) is proven by the disassembly, not by
a trial:

    dis() {   # disassemble build/kernel-618/vmlinux with the toolchain objdump
        docker run --rm -v "$PWD/build/kernel-618:/k:ro" \
            "$(packages/oss-env.sh)" mips-linux-uclibc-objdump -d /k/vmlinux | tail -n +3
    }
    git checkout <base>;   VERSION=cmp COMPILE_ONLY=1 kernel/build.sh && dis > before.s
    git checkout <branch>; VERSION=cmp COMPILE_ONLY=1 kernel/build.sh && dis > after.s
    diff before.s after.s && echo identical

Pin `VERSION` so the ramlog build id string is the same in both builds.
An empty diff means the change is a no-op for the CPU; say so in the
commit message. For a file split or move, where addresses shift, compare
per function with the addresses normalised: every non-static function
identical, each static-inlining difference named. The same works for a
userland binary (`omcid`, `diag`) with its own objdump. `tools/kernel-footprint.sh`
before and after shows what a patch change costs the next LTS port.

### On hardware

Only on a stick you own and can power-cycle, with the stock firmware
committed in the other slot. The ISPs' OLTs differ in how they provision,
so a change to OMCI or the datapath needs a trial on a live line, not only
on a bench fibre.

- **A trial boot**: flash the inactive slot, `nv setenv sw_tryactive
  <slot>`, `reboot` (`docs/FLASHING.md`). Check: `/proc/cmdline` shows the
  trial slot, `cat /proc/odi_wdt/userland_ok` says `userland_ok=1`,
  `omcli state` reaches O5 with the services provisioned, `dmesg` has no
  new warning, `/etc/odi-build` names your build.
- **A boot loop**: repeat the trial from the committed image, one
  `sw_tryactive` per boot, and record each boot (ramlog counter, time to
  confirm, ONU state). The releases are qualified with 15 consecutive
  trials; bugs that live in the boot (the NIC DMA one hit one boot in six)
  need that many.
- **A soak**: hours with a sample every few minutes: ONU state, the switch
  MIB counters (`diag mib dump counter port all`), `ip -s link` errors,
  `dmesg`, `MemFree`, the exporter's scrape. **Subscriber traffic is
  forwarded by the switch hardware and never reaches the CPU**
  ([`docs/kb/rtl9601-forwarding-visibility.md`](kb/rtl9601-forwarding-visibility.md)),
  so a busy line exercises the switch and the GPON MAC but not `odi_nic`.
  To test the NIC path, send traffic to the stick itself: copy a file of a
  few MB to it and back over scp and compare checksums, in every sample.

## Gotchas

Each of these has cost real time. The pointer says where the evidence is.

**Hardware and boot**

- **No serial console.** The UART exists on the board, not on the SFP edge.
  Everything about debugging here goes through the ramlog, crumbs and
  breadcrumbs ([above](#reading-a-boot-you-could-not-see)), and every boot
  change goes through a trial.
- **The loader leaves the NIC DMA running.** The CPU-port NIC's receive
  engine keeps writing frames through the loader's descriptors into pages
  Linux has handed out; the symptom was a SIGBUS in `/bin/busybox` about one
  boot in six. `prom_init()` stops it with two uncached stores before the
  kernel owns a page. Do not move that later. `docs/KERNEL.md` "The board".
- **The RLX5281 needs the R3000 exception model.** It keeps CP0 `Status` as
  a KU/IE stack and returns with `rfe`; the kernel is `CONFIG_CPU_R3000`,
  and `objdump -d vmlinux | grep -cw eret` must print 0. The CPU also has
  to be named in the R3K probe (PRId `0x0000dc02`), or a `BUG_ON()` runs
  `break` before any exception vector exists: a silent hang.
  [`docs/kb/rtl9602c-rlx5281-kernel-needs-r3000-exception-model.md`](kb/rtl9602c-rlx5281-kernel-needs-r3000-exception-model.md).
- **The icache does not snoop the dcache**, and how it is indexed is
  unknown, so a per-line invalidate cannot be trusted to hit every alias.
  `flush_data_cache_page()` writes the dcache back and invalidates the
  **whole** icache. Any new path that gives a page new contents for
  execution has to end there. `docs/KERNEL.md` "The CPU".
- **`wmb()` is nothing on this build.** `CPU_R3000` turns `CPU_HAS_SYNC`
  off, so mainline `wmb()`, `rmb()` and `dma_wmb()` emit no instruction. Use
  `rtl8686_sync()` (`asm/mach-rtl8686/rtl8686-barrier.h`) for DMA ordering
  (a descriptor body before its ownership bit). `docs/KERNEL.md` "The CPU".
- **The clocksource is a SoC timer, and may fall back to jiffies.** No CP0
  Count/Compare; `rtl8686-timer1` (TIMER1, free-running, 3.125 MHz, 28 bits)
  is the clocksource and `sched_clock`, and the TIMER0 tick interrupt only
  drives the scheduler tick (verified on hardware).
  If TIMER1 does not count at boot the driver prints a warning and the
  kernel stays on the jiffies clocksource, where with interrupts off
  `jiffies` and `ktime_get()` stand still and a poll bounded by a clock in
  atomic context never times out. Do not rely on a clock-bounded atomic
  poll either way: `odi_poll_reg()` (`odi_switch_reg.h`) is built on 6.18
  `read_poll_timeout_atomic()`, which counts its bound in delay steps
  rather than reading a clock; use it, or count iterations yourself.
  `kernel/extra/arch/mips/rtl8686/time.c`.
- **Reading an undecoded SoC address stalls the bus** until the watchdog
  resets the stick. Do not probe with `devmem` or `memprobe` outside
  addresses you can account for; the drivers bound every MMIO access
  (`odi_switch_mmio_offset_in_bounds()`).
  [`docs/kb/rtl9602c-undecoded-address-read-takes-the-stick-down.md`](kb/rtl9602c-undecoded-address-read-takes-the-stick-down.md).
- **The laser stays off on a laser driver we have not validated.** The
  `laser` PON verb reads the laser driver id (A2h page 2, byte `0xa0`) and
  drives SoC GPIO 13 (TX disable) low only for the Semtech GN25L95 (`0x6a`).
  Any other id, or none answering, leaves the laser off and makes
  `gponact` fail with `-EPERM`; `gponsn` and `gponpw` still run. On another
  board the laser may need set-up this image does not do, and a laser lit
  outside its timeslots takes down every ONU on the OLT port.
  `ODI_LASER_UNVERIFIED=1` in `odi.conf` (`laser unverified`) overrides it,
  for an OLT port with no other ONU. `/proc/odi_gpon/status` shows `laser on X
  ldd_id Y unverified Z`. `odi_board.c` (`odi_board_laser()`), `odi_gpon.c`
  (`odi_gpon_verb()`), `docs/BOOT.md` stage 13.
- **The kernel partition is 1,359,872 bytes** (the LZMA uImage), and
  `image/build.sh` refuses a bigger one. The config is on a diet (no
  modules, no kallsyms, `SLUB_TINY`, a list of unreachable syscalls off).
  Before turning an option off, map it to every syscall it removes and
  scan the rootfs ELFs for the o32 number (4000 + N) loaded before
  `syscall`. Three must stay on: `POSIX_TIMERS` (on 6.18 it also builds
  `itimer.c`; off, `alarm`/`setitimer` return ENOSYS and busybox `arping`,
  `ping` and `timeout` never time out, which rcS depends on),
  `FILE_LOCKING` (busybox `flock`, iproute2, and `fcntl` locks a scan
  cannot see) and `VM_EVENT_COUNTERS` (the exporter's memory pressure
  signal). `docs/KERNEL.md` "The config".
- **Squashfs is XZ with 256 KiB blocks.** Mainline squashfs has no LZMA
  (the stock image's format), and 6.18 keeps caches of whole blocks, so
  the block size is RAM on a 32 MB board.
  [`docs/kb/rtl9602c-squashfs-block-size-costs-ram.md`](kb/rtl9602c-squashfs-block-size-costs-ram.md).
- **The L2 table's valid signal is the HIT bit of TABLE_STATUS after the
  read**, not the row's valid bit (bit 77 reads back set on every row). A
  hash *write* also reports HIT for an absent key, so a delete looks the key
  up with a hash *read* first. `docs/KERNEL.md` "The L2 table".

**Instruction set and ABI**

- **The CPU traps on instructions a normal MIPS toolchain emits**: `mul`,
  `clz` (all of SPECIAL2), `teq`/`tne`/`tge`/`tlt` and their forms, and the
  branch-likely `beql`/`bnel` (and `bltzl`, which does not trap but
  misexecutes: its delay slot always runs). Every build uses `-march=mips2
  -mno-branch-likely -mdivide-breaks`, including the toolchain's own target
  libraries, and every ELF in the image passes `packages/isa-audit.sh` (a
  deny list) and `packages/isa-allowlist.sh` (only mnemonics confirmed on
  the device). Run both on anything you build yourself. The kernel adds
  `-U_MIPS_ISA -D_MIPS_ISA=_MIPS_ISA_MIPS1` so `BUG_ON()` does not become
  `tne`. [`docs/CROSS-COMPILING.md`](CROSS-COMPILING.md),
  [`docs/kb/rtl9601-isa-map.md`](kb/rtl9601-isa-map.md).
- **MIPS o32 is not the generic Linux ABI**, and every difference fails
  silently. Syscalls are numbered from 4000 (`exit` is 4001), and an error
  comes back in `$a3`, not as a negative return. `struct sigaction` has
  `sa_flags` first. `SOCK_STREAM` is 2 and `SOCK_DGRAM` 1 (swapped),
  `SOL_SOCKET` is `0xffff`. errno values differ (`ETIMEDOUT` is 145,
  `EIDRM` 36). The ioctl direction bits sit elsewhere (a write is
  `0x80000000`). Take every constant from the MIPS `<asm/*.h>` headers,
  never from another architecture. `src/diag/src/sys.h` has the ones our
  tools use. [`docs/kb/mips-o32-syscall-abi-traps.md`](kb/mips-o32-syscall-abi-traps.md).
- **Two build styles.** Everything in `src/` is freestanding (`-nostdlib`,
  our own `_start` and syscall layer, built with Debian's
  `gcc-mips-linux-gnu` at `-march=mips1`), which also makes it run on the
  stock image. Everything in `packages/` links statically against our
  uClibc-ng; the builds refuse a binary with a `PT_INTERP`. There are no
  shared libraries in the image.
  [`docs/kb/rtl9601-freestanding-over-libc.md`](kb/rtl9601-freestanding-over-libc.md).

**Userland and data**

- **The config store is shared with the stock firmware.** The jffs2
  `config` partition is not part of a slot: both images read it, and a
  reflash never touches it. Of its 184 keys this image acts on 23 (22 the UI
  edits, and bit 256 of `OMCI_CUSTOM_ME`); the UI shows the rest read-only. **Never delete a key** this image does not use
  (the stock slot still reads it), never erase the partition (it holds the
  serial number and MAC the line authenticates on), and keep backups whole.
  `flash set` and `flash default` install the new file by rename on the
  config partition's own filesystem (`install_over()` in
  `rootfs/skeleton/etc/scripts/flash`), so a power cut leaves the old file,
  not half of the new one; keep that shape in any script that writes there.
  `docs/SETTINGS.md`, [`docs/kb/rtl9601-config-store.md`](kb/rtl9601-config-store.md).
- **Every setting has an apply class**: LIVE, SERVICE RESTART, INTERRUPTS
  INTERNET, REBOOT. A new setting needs its class decided, the apply path
  (`apply.sh network` or `apply.sh omci`, or a reboot), the odi-ui schema
  row and the `docs/SETTINGS.md` row, together.
- **The exporter contract.** The `diag` commands `metricsd` sends keep the
  stock CLI's syntax and output byte for byte; `make test-diag` pins them.
  `/proc/odi_gpon/` and anything else the exporter or confd parses are
  interfaces too.
- **`diag` waits forever on a stdin that never closes.** Give it a pipe or a
  file, and `timeout` in anything that must not stall (rcS, network.sh).
- **`omcicli get tables` wedges the stock OMCI daemon.** Ours answers it; do
  not run it on the stock slot. `omcicap` and `omciprobe -f` interfere with
  a running `omcid` (`docs/TOOLS.md`).
- **Daemons are `respawn` entries, not sessions.** syslogd, klogd, ntpd,
  metricsd, confd, dropbear and omcid are forked by busybox init itself and restarted
  as soon as one exits; `kill` alone brings a new one back within seconds
  (after a wait of a few seconds if it keeps dying young, and
  `/var/run/respawn/<name>` says so, `docs/SETTINGS.md` "Restart budget"), no
  `setsid` needed any more (`docs/TOOLS.md` "Restarting a daemon"). Use
  `/etc/init.d/services stop <name>` to keep one down across restarts.
- **`/var` is RAM.** Every log is gone at reboot; only the ramlog and the
  breadcrumbs survive.

**Repo rules**

- **No apostrophes in shell comments.** Several scripts run a block as
  `docker run ... bash -c '...'`, where the whole block is one
  single-quoted word; an apostrophe in a comment inside it ends the word,
  and the rest of the block runs in the outer shell. Rephrase ("the kernel
  own config", "does not"), do not escape. `tools/check-inline-quotes.sh`
  checks inline blocks; avoid them everywhere.
- **No vendor-sourced code, and no vendor source file or function names**
  in code, comments or docs. Describing what the stock firmware *does*, as a
  black box (its shipped binaries, their exported symbols, what it writes),
  is fine and often necessary. `make lint` greps for a narrow list of known
  vendor source citations; the review catches the rest. `AGENTS.md`.
- **No private references.** `make lint` greps tracked files for paths and
  document names of the private workspace this project grew up beside, and
  for home-directory paths. It greps only **tracked** files, so `git add`
  a new file before you trust a clean lint. Name the test sticks by their
  line, ISP1 and ISP2, and never commit a credential, serial number or MAC
  from a real stick.
- **Comments say what is true now.** The why, in the fewest lines that
  carry it: an invariant, a hardware fact, an ordering or lock rule, what
  a magic value means or that it is not decoded, and where the evidence
  is (a fixture, a test, a docs anchor). No trial or boot ids, dates, old
  patch numbers or "used to": git history is the record, and the commit
  body is where a finding goes. `make lint` greps `kernel/extra`,
  `rootfs/skeleton` and `src` for the common markers. Derivations belong
  in `docs/` (`docs/SWITCH.md`, `docs/KERNEL.md`, `docs/BOOT.md`).
- **Mainline footprint.** `kernel/618/patches/` is only for lines in files
  mainline already has; new files go in `kernel/extra/`. Fold a fix into
  the patch it belongs to. `tools/kernel-footprint.sh` only goes down
  without a stated reason.
- **Builds are reproducible, keep them so**: every time stamp comes from
  `SOURCE_DATE_EPOCH` (`tools/source-date-epoch.sh`), never from the clock,
  hostname or user, and nothing may depend on directory order or the host
  umask. A new build step that embeds one of those breaks
  `tools/compare-images.sh`; `docs/BUILDING.md`, "Reproducible builds".

## Feature toggles

Every switch, parameter and build variable, from the code on `k618`. The
**class** column says who it is for: *prod* (a supported setting) or *dev*
(a development aid, safe but not for a production stick).
`docs/SETTINGS.md` remains the user-facing description of the switch files
a user may set; this table adds where each one is read.

Line numbers are for `k618` at the time of writing and drift; grep for the
name.

### Switch files on the config partition

`/etc/config` is a symlink to `/var/config`, the jffs2 config partition; a
file there survives reflashing and applies to every later image. All are
read at boot unless stated. rcS paths are
`rootfs/skeleton/etc/init.d/rcS`.

| file | read at | effect | default | class |
|---|---|---|---|---|
| `syslogd.off`, `klogd.off`, `ntpd.off`, `metricsd.off`, `confd.off`, `dropbear.off`, `omcid.off`, `history.off` | the first lines of each `etc/scripts/svc-<name>.sh` (`omcid.off` also gates the watchdog registration in rcS); written by `services stop <name>` | that daemon's respawn entry runs a placeholder (`respawn-off.sh`) instead | absent: all run | prod |
| `lan-ip` | `etc/scripts/network.sh` (`OVERRIDE`, line 48) | management address, one line; overrides `LAN_IP_ADDR`, netmask then 255.255.255.0; live with `apply.sh network` | absent: `LAN_IP_ADDR`, else 192.168.1.1/24 | prod |
| `sds.off` | `etc/scripts/host-serdes.sh` (`SDS_OFF`), from `network.sh` | skip the host SerDes check and fix, and refuse a `HOST_SERDES` trial | absent | prod (escape hatch) |
| `optics.off` | `pon_step_laser()`, `etc/scripts/rcs-lib.sh` | skip the `laser` verb, so the laser TX enable (GPIO 13) is never driven: **laser off, `gponact` refused, no service**. The `optics` verb (`PIN_GPIO_SELECT`) still runs | absent | prod (escape hatch) |
| `modules.off` | `rcS:196`, `omci_start()` and `svc-omcid.sh`; `etc/scripts/apply.sh:77` | skip the switch init (platform settings and module-load replay) and `omcid`: **no OMCI**; `apply.sh omci` refuses | absent | prod (escape hatch) |
| `pon-steps` | `etc/init.d/rcS.dev:188` | replaces the built-in PON steps: one verb per line, or `omcimods` (switch init, then the watchdog confirmation), `gponsn auto`, `gponpw auto`, `gponact` (`docs/BOOT.md`); a plain `laser` line ignores `optics.off` and `ODI_LASER_UNVERIFIED`; a wrong list means no PON | absent | dev |
| `omci-identity.on` | `src/omci/respond/omcid.h:478` (`CFG_REPORT_SWITCH`), `cfgstore.c:882` | omcid reports the OLT identity keys (`OMCI_SW_VER1/2`, `GPON_ONU_MODEL`, ...); `apply.sh omci` | absent: built-in values | prod |
| `breadcrumbs.on` | `rcS.dev:27`, `:53`, `:62`, `:76`, `:87`, `:106` | boot crumbs to `breadcrumbs`, `trial-diag.txt`, a 30 s heartbeat to the kernel log, soft-lockup 20 s and hung-task 30 s | absent | dev |
| `confirm-arp` | `rcS.dev`, `dev_confirm_ok()`; `nv commit` (`src/nv/commit.c`); `slot-state.sh` | the file holds one IPv4 address: confirm the watchdog only after an ARP reply from it; **resets every 120 s if the host stops answering**; `nv commit` refuses while it exists (`--force`) and the login banner names it | absent, empty or not an IPv4 address: confirm unconditionally | dev |
| `odi.conf` | `etc/scripts/flash` (`ODI_KEYS`), written by `flash set`; `ODI_LASER_UNVERIFIED` is read by `pon_step_laser()` | odi-only keys: `SYSLOG_SERVER`, `NTP_SERVER`, `ONU_HW_VERSION`, `OMCI_UNKNOWN_ME_OK`, `OMCI_FIXED_ME`, `OMCI_PRIVATE_ME`, `OMCI_VEIP`, `OMCI_POTS_PORTS`, `OMCI_POTS_SLOT_ID`, `OMCI_VOIP_LINE_REPORT`, `OLT_SW_DOWNLOAD`, `OMCI_ACCEPT_REBOOT`, `OMCI_UNI_LOCK`, `VLAN_EAPOL_UNTAGGED`, `OMCI_TCONT_ALLOC_UNSET`, `HOST_AUTONEG`, `HOST_SERDES`, `LAN_ROUTES`, `ODI_LASER_UNVERIFIED` (`docs/SETTINGS.md`); `ODI_LASER_UNVERIFIED=1` lights the laser on an unvalidated laser driver | absent: none set | prod |
| `confd.auth`, `confd/`, `dropbear.d/authorized_keys` | confd (odi-ui); dropbear via `svc-dropbear.sh` (`-D`) | UI credential, UI overrides, root SSH keys | admin/admin; image assets; none | prod |

State files, not switches: `seedrng/` (entropy seed, rcS `:97`),
`dropbear.d/ed25519` (host key, generated on first boot), `breadcrumbs`,
`trial-diag.txt` (outputs of the dev rows).
Settings in the XML store itself (`lastgood.xml`, `lastgood_hs.xml`) are
`docs/SETTINGS.md`.

### Kernel parameters and the command line

Built-in drivers take parameters as `<object>.<name>=` on the command
line, or at run time through `/sys/module/<object>/parameters/<name>`. The
command line is `CONFIG_CMDLINE` (`kernel/618/config:61`) followed by
U-Boot's per-slot `argv` (`MIPS_CMDLINE_BUILTIN_EXTEND`), so a parameter
added to `CONFIG_CMDLINE` takes effect after a kernel rebuild; U-Boot's
`root=` and `mtdparts=` win because they come last.

| parameter | defined | effect | default | when | class |
|---|---|---|---|---|---|
| `odi_nic.debug` | `odi_nic.c:53` (bool, 0644) | trace the first interrupts and RX descriptors to the console; at the first open (so only when set on the command line) also the ring register read-backs and a NIC state dump 15 s and 45 s later | off | boot, or live for the traces | dev |
| `odi_nic.omci_ring` | `odi_nic.c` (bool, 0644) | run a second receive ring and route the OMCI frames the switch traps to the CPU (internal priority 7) to it, so a flood from the host side cannot fill the ring OMCI needs (`kernel/extra/drivers/net/ethernet/odi/README.md`); `N` (the default) runs one ring with the register values the NIC always had. Both rings are served either way, so a change loses nothing in flight. `/proc/odi_nic` `omci_ring` shows what is in effect | off | boot, or live: applied at once | experimental: not yet trialled on hardware; turn it on live from a script that turns it off again unless told to keep it |
| `odi_switch.host_pause` | `odi_switch.c:200` (bool, 0644) | let the host port send PAUSE frames to the host, as the stock image does (`docs/SWITCH.md`, "Host port PAUSE"); off, the switch drops at its own buffers instead | off | boot, or live: applied at once | prod (escape hatch) |
| `odi_switch.pon_isolate` | `odi_switch.c` (bool, 0644) | clear the CPU side from the PON port's isolation mask and the PON port from the CPU port's, so the switch never forwards a frame from the fibre to the CPU, nor one from the CPU to the fibre by lookup (`docs/SWITCH.md`, "PON port isolation"); `N` leaves both masks open, as the stock image does. The NIC drops non-OMCI frames from the PON port either way | on | boot (with the platform settings), or live: applied at once | prod (escape hatch) |
| `odi_switch.host_autoneg` | `odi_switch.c` (bool, 0644) | the 1000BASE-X auto-negotiation of the host port; `N` forces 1000BASE-X full duplex (`docs/SWITCH.md`, "Host port auto-negotiation"). `host-autoneg.sh` drives it from `HOST_AUTONEG`; set by hand it lasts until the next boot | on | boot (with the platform settings), or live: applied at once | prod (through `HOST_AUTONEG`) |
| `odi_switch.host_forced` | `odi_switch.c` (bool, 0644) | force the host port up, full duplex, 1000 Mb/s (`PORT_FORCE_ABILITY(0)` 0x16, `PORT_FORCE_SELECT(0)` 0xfff), as the 2500BASE-X and HiSGMII host SerDes modes need; cmd 32 then moves the forced link value (`docs/SWITCH.md`, "Host port SerDes mode"). `host-serdes.sh` drives it from `HOST_SERDES`; set by hand it lasts until the next boot | off | boot (with the platform settings, only when on), or live: applied at once; `N` while already `N` writes nothing | prod (through `HOST_SERDES`) |
| `odi_switch.host_link` | `odi_switch.c` (read-only) | `1` while the host port has a link (always `1` while `host_forced` is on) | -- | read at any time | prod |
| `odi_switch.sfp_mirror` | `odi_switch.c` (bool, 0644) | the host-side SFP EEPROM (`odi_sfp_mirror.c`, `docs/SETTINGS.md` "Host-side diagnostics"): `Y` fills the SoC SFP slave from the module and turns it on, in a work item, and refreshes A2h 96-119 every second; `N` turns it off before the write returns. `sfp-host-ddm.sh` drives it from `SFP_HOST_DDM`; set by hand it lasts until the next boot | off until written; the boot writes `Y` unless `SFP_HOST_DDM=0` or a safe-mode boot | live; refused on the command line (the optics I2C answers only after the PON steps) | prod (through `SFP_HOST_DDM`) |
| `odi_switch.sfp_mirror_state` | `odi_switch.c` (read-only) | `off`, `filling`, `on` (the host is served) or `failed` (the kernel log says why) | -- | read at any time | prod |
| `print-fatal-signals=1` | `kernel/618/config:61` (mainline) | register dump of any process killed by SIGSEGV/SIGBUS/SIGILL, into the ramlog | on | boot | prod (debug aid) |
| `panic=10` | `kernel/618/config` (mainline) | restart 10 s after a panic, through the watchdog restart handler, instead of waiting for the userland watchdog deadline (about 44 s) | on | boot | prod (availability) |
| `sysctl.kernel.kptr_restrict=2`, `sysctl.fs.protected_{symlinks,hardlinks,fifos,regular}=1` | `kernel/618/config` (mainline) | the hardening sysctls, set by the kernel before init; `tools/check-kernel-hardening.sh` fails the build without them (`docs/KERNEL.md`, "Hardening") | on | boot, or live through `/proc/sys` | prod (hardening) |

### Kconfig

Our symbols; the seed config (`kernel/618/config`) sets each one. The
four driver symbols sit in one menu that depends on `MACH_RTL8686`
(`olddefconfig` silently drops a symbol whose dependency is off; the build
prints the `CONFIG_ODI_*` it got).

| symbol | defined | builds | seed | class |
|---|---|---|---|---|
| `MACH_RTL8686` | `kernel/618/patches/0002` | the board (System type) | y | prod |
| `RTL8686_UART0`, `RTL8686_NOR` | `arch/mips/rtl8686/Kconfig:32`, `:39` | the 8250 UART; the SPI NOR MTD driver | y | prod |
| `ODI_EARLY_CRUMBS` | `arch/mips/rtl8686/Kconfig:52` | the early boot crumbs in the ramlog pages | y (Kconfig default n) | prod (debug aid) |
| `ODI_NIC` | `drivers/net/ethernet/odi/Kconfig:5` | `odi_nic.c`, the CPU-port NIC | y | prod |
| `ODI_SWITCH` | `:12` | the switch core, the OMCI transport and `/proc/odi_omci`, the GPON MAC and the switch interrupt, `/proc/odi_init` and the SDK-init replay, the board-init replay, `/dev/odi_sw`, I2C/DDM and the i2c adapter `i2c-0` (selects `FW_LOADER`, `CRC32`, `I2C`) | y | prod |
| `ODI_WDT` | `:29` | the watchdog kicker, the userland deadline and the restart handler | y | prod |
| `ODI_RAMLOG` | `:37` | the DRAM ramlog console and `/proc/odi_ramlog_prev*` | y | prod |

### Build variables

| variable | read in | effect | default |
|---|---|---|---|
| `VERSION` | `image/build.sh:36`, `kernel/build.sh:86` | image name, `/etc/version`, the ramlog build id | `odi-oss-<yymmdd>-<git describe>` |
| `ODI_BUILD_ID` | `kernel/build.sh:86` | the ramlog build id alone (39 chars, safe set) | `VERSION` |
| `FRESH=1` | `kernel/build.sh:114`; `packages/*/build.sh` | re-extract the kernel tree / rebuild a package from clean | 0 |
| `CRUMBS_CORE=1` | `kernel/build.sh:77` | also apply `kernel/618/debug/*.patch` (crumbs in mainline core files) | 0 |
| `COMPILE_ONLY=1` | `kernel/build.sh:159` | stop after `vmlinux`, no uImage | 0 |
| `VOL` | `kernel/build.sh:55` | kernel build volume | `odi-kbuild-618` |
| `WORK` | `kernel/build.sh:54` | kernel output directory | `build/kernel-618` |
| `KVER` | `kernel/build.sh:50` | accepted only as `6.18` | `6.18` |
| `OSS_IMAGE`, `DIAG_IMAGE` | `toolchain/image.sh`, `toolchain/images.env`, `src/*/Makefile`, `Makefile` (`test-omci`) | the uclibc and freestanding toolchain images, e.g. a local build from odi-toolchain | the pinned digests in `toolchain/images.env` |
| `TOOLS_IMAGE` | `image/build.sh` | squashfs and tar tooling image | `odi-oss-image` |
| `DL` | `kernel/618/fetch.sh`, `packages/fetch.sh`, `src/fetch-releases.sh` | download cache (an absolute path); several trees can share one | `dl/` |
| `OUT` | `kernel/618/fetch.sh` | kernel tree location | `kernel/618/mainline` |
| `ALT_URLS` | `packages/fetch.sh:34` | extra mirrors for package tarballs (a Software Heritage fallback is built in) | none |
| `KERNEL`, `KCONFIG` | `image/build.sh:48`, `:39` | uImage to ship, and its `.config` (shipped as `/etc/kernel-config`) | `build/kernel-618/uImage` and the `config` beside it |
| `BUSYBOX`, `BIN_DIR`, `PKG_DIR`, `ASSET_DIR` | `image/build.sh:37`, `:43`, `:46`, `:44` | where the pieces are | `out/busybox`, `out/bin`, `out`, `out/confd-assets` |
| `COMP`, `BS` | `image/build.sh:27`, `:28` | squashfs compressor and block size | `xz`, 262144 |
| `ROOT_PW` | `image/build.sh:292` | root password; `none` for empty, `locked` for no password at all (keys only) | generated, 14 characters |
| `ALLOW_PARTIAL=1` | `image/build.sh:74`, `:170`, `:198` | build without confd assets or a package | 0 |
| `ALLOW_NO_KCONFIG=1` | `image/build.sh:267` | build without `/etc/kernel-config` (the `.config`, shipped for reference) | 0 |
| `GITHUB_TOKEN`, `USE_CURL=1` | `src/fetch-releases.sh`, `tools/remote-build.sh` | optional token, raises the anonymous rate limit for the curl download; skip `gh` even when present | none; 0 |
| `METRICSD_TAG`, `CONFD_TAG`, `*_REPO` | `src/fetch-releases.sh` | releases to fetch; `CONFD_TAG=` (empty) with `CONFD_BIN` for a local confd | pinned in the script |
| `METRICSD_BIN` | `src/fetch-releases.sh:27` | local metricsd binary instead of the release | none |
| `CONFD_BIN`, `CONFD_ASSETS` | `src/fetch-releases.sh:`, `:` | local confd binary and its odi-ui checkout or assets tarball | none; `$(dirname $CONFD_BIN)/..` |
| `ODI_REMOTE`, `ODI_REMOTE_DIR`, `ODI_REMOTE_DL` | `tools/remote-build.sh` | remote build host, directory, and download cache (exported there as `DL`) | required; `/root/odi/odi-oss`; `dl/` beside it |

### /proc and /dev control files

Writing any of these by hand reprograms hardware under a running stack; do
it on a trial stick only. `docs/TOOLS.md` "Kernel control files" has the
read side.

| file | defined | write | read | class |
|---|---|---|---|---|
| `/proc/odi_init` | `odi_init.c` | one verb, optional argument: the SDK-init verbs (`intr` ... `ponmac`, `i2c`, `i2cen`, `gpon`, `rxsd`), `optics`, `laser` (or `laser unverified`: light the laser whatever the laser driver id says), and the GPON verbs (`gpondrv`, `gpondev`, `gponsn <sn>`, `gponpw <pw>`, `gpondg 0|1` (the GPON_DYING_GASP switch of the driver reboot notifier), `gponkeep 0|1` (the GPON_KEEP_DISABLE switch) and `gponhold 1|0|sim` (restore a kept Emergency Stop at boot, release it by hand, or the test hook that acts as if the OLT had disabled this ONU; `docs/SETTINGS.md`, "Keeping the OLT's disable"), `gponact` (refused with `-EPERM` while the laser is off), `gpondeact`, `gponstat`); `<pw>` is 20 hex digits, `0x` plus up to 20 hex digits, or up to 10 characters of text | the last verb's return code | prod (`rtk_init` under the stock firmware) |
| `/proc/odi_omci` | `odi_omci.c:452` | `switch_init` (`:415`): the platform settings and the module-load replay | redirect registrations and pids, frame and command counters (`igmp_delivered`, `igmp_dropped`: the host's IGMP copies to igmpd; `igmp_sent_to_pon`, `igmp_send_fail`: the reports igmpd sent up in their place) | prod |
| `/proc/odi_wdt/userland_ok` | `odi_wdt.c:790` | `1`: userland is up, cancel the 120 s reset | `userland_ok=N deadline=120 uptime=N` | prod (`luna_watchdog` under the stock firmware) |
| `/proc/odi_wdt/watchdog_flag` | `odi_wdt.c:788` | `1` arm and kick; `0` disable the watchdog and stop the kicker | `watchdog_flag=N` | dev |
| `/proc/odi_wdt/register` | `odi_wdt.c:792` | `"<name> <deadline_s>"`: register or update a watchdog client (rcS does this once per client at boot) | -- | prod |
| `/proc/odi_wdt/ping` | `odi_wdt.c:794` | `"<name>"`: a registered client's own ping; arms its deadline on the first call | -- | prod (omcid, every 15 s: `src/omci/respond/main.c`, `wdt_ping()`) |
| `/proc/odi_wdt/clients` | `odi_wdt.c:796` | none | one line per client: `name=... deadline=... armed=... last_ping_age=...` | dev (debugging) |
| `/proc/odi_gpon/status`, `counters`, `ploam` | `odi_gpon.c` | none | `status`: ONU state, `o7_hold keep X held Y` (GPON_KEEP_DISABLE: the feature is on, a kept Emergency Stop is in force), ONU id, serial, Alloc-IDs, the laser line (`laser on X ldd_id Y unverified Z`), alarm and state times; `counters`: the GPON link counters; `ploam`: PLOAM counters (`ploam_us_dropped`: upstream replies that found no room) and the ring (`docs/TOOLS.md`) | prod (`diag`, `omcid` and `metricsd` read it; its format is an interface) |
| `/proc/odi_ramlog_prev`, `_raw` | `odi_ramlog.c:645` | none | the previous boot's ramlog (root only) | prod (debug aid) |
| `/dev/odi_sw` | `odi_reg.c:191` | ioctls `REG_SET`, `L2_MC_ADD`/`DEL`, `MC_SNOOP` (the snooping lease, held by the open file), `ACL_PROBE` set and delete (`diag acl-probe`, held by the open file the same way) | ioctls `REG_GET`, `MIB_GET`, `DDM_GET`, `L2_GET`/`NEXT`, `L2_MODE`, `ACL_PROBE` get (`odi_reg.h`) | prod; the probes are a measuring aid nothing in the image uses |

The `odi_omci` netlink transport rides on its own private protocol number
(`NETLINK_ODI`; the stock firmware used `NETLINK_USERSOCK`) and refuses
senders without `CAP_NET_ADMIN`.

## Making a change

1. **Read first.** The file's header and the section of `docs/KERNEL.md`,
   `docs/TOOLS.md` or the driver README that covers it; `docs/kb/` for the
   hardware; `git log -p` for the file, since the commit bodies carry the
   reasoning.
2. **Small commits, one change each**, subject `area: what changed`
   (`odi_switch_l2: ...`, `rcS: ...`, `docs: ...`), a body that says why and
   what was measured. Sign them if you can (`git commit -S`; the
   maintainer signs with an SSH key). No `Signed-off-by:` in the kernel
   patch files (`docs/KERNEL.md` says why).
3. **Run the gates**: `make test` always; the kernel build, the ISA audit
   and the footprint for a kernel change; the image build for anything that
   ships; the disassembly diff for a refactor. Put the evidence in the
   commit body.
4. **Trial it on your own stick**, never on one carrying someone's only
   connection: stock committed in the other slot, `: >
   /etc/config/breadcrumbs.on` first, `echo <host address> > confirm-arp`
   if you want an unreachable trial to revert, `sw_tryactive`, never `sw_commit` before it has run. Record what
   you saw.
5. **Update the docs in the same change**: `docs/SETTINGS.md` for a
   setting, `docs/TOOLS.md` for a command, `docs/KERNEL.md` for a driver,
   this file for a toggle or a trap, `CHANGELOG.md` when a release is cut.
6. **Write down what you learned about the hardware** as a field note in
   `docs/kb/`: one claim per note, a title that states the claim, a
   one-paragraph summary, `*Last verified: <date>*`, then `## What`, `##
   Why it matters`, `## Evidence` (what was run, on which line, what came
   back) and `## See also`. Only what was observed or reproduced goes in; a
   hypothesis stays in the commit message or the issue. Add the note to
   the table in `docs/kb/README.md` by hand. Describe the stock firmware
   by its behaviour, never by its source.

[`CONTRIBUTING.md`](../CONTRIBUTING.md) has the short version, and how to
report an issue.
