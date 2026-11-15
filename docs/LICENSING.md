# Licensing

What license covers what in this tree, by piece. Written because a public
repository built to replace a closed firmware image deserves a clear answer
to "does this ship or link anything proprietary," not just an implied one.

**Short answer: no.** This build fetches, builds and ships nothing
proprietary. The kernel it produces contains no vendor-supplied object code
and no copied vendor source; every kernel module that used to be a closed
binary blob on this class of device has an open replacement of our own
here instead, built straight into the kernel (`docs/KERNEL.md`).

## The kernel: GPL-2.0-only, with our additions GPL-2.0-or-later

`kernel/618/mainline/` is a pristine, GPG-verified `cdn.kernel.org` release
tarball (`kernel/618/fetch.sh`), never committed. `kernel/618/patches/` are
our own changes to its files, and `kernel/extra/` holds the files we add:
the board and CPU support, the interrupt controller, timer and SPI NOR
drivers, and `drivers/net/ethernet/odi/`, our own driver directory
(switch, GPON MAC, CPU-port NIC, OMCI transport, watchdog, DRAM ramlog
console) — an independent implementation,
our own code written from public specifications (ITU-T G.984.3 and G.988 for
GPON and OMCI, SFF-8472 for optics), hardware behaviour observed on this
board, and the behaviour of the stock firmware observed as a black box.

The kernel tree itself stays **GPL-2.0-only**, as upstream ships it. Our
patches are hunks against upstream files and follow the license of the file
they modify (GPL-2.0-only); the files in `kernel/extra/` are ours and are
**GPL-2.0-or-later**, combined into the GPL-2.0-only kernel, which
GPL-2.0-or-later permits.

### The register replay tables

`rootfs/skeleton/lib/firmware/odi/` holds three binary files
(`sdkinit.bin`, `modload.bin`, `gpon_init.bin`) that the kernel loads with
`request_firmware()`. They are data, not code, and not vendor files: each
is a list of register writes (address, value, table words) recorded with
`tools/regtrace` from this board's own hardware while the stock firmware
initialised it, and packed by our own generators into our own format
(`odi_replay_blob.h`). They carry no strings and no identity: the serial
number words of the GPON table are zero, and the kernel fills them in from
the config store at boot. They are committed rather than generated at build
time because the register listing the generators need is itself read from
the stock firmware binary (below) and is not in this tree.

## The toolchain and upstream packages: their own licenses

Built from source, unmodified except where noted, each keeping its own
upstream license:

| component | version | license |
|---|---|---|
| gcc | 16.2.0 | GPL-3.0 (with the GCC runtime library exception) |
| binutils | 2.47 | GPL-3.0 (a small patch adds three CPU-specific instruction encodings binutils never had; same license) |
| uClibc-ng | 1.0.59 | LGPL-2.1 |
| Linux kernel headers (used to build uClibc-ng) | matches the kernel version they are paired with | GPL-2.0, with the usual "headers do not make your program a derivative work" exception |
| busybox | 1.38.0 | GPL-2.0 |
| dropbear (+ its `scp`) | 2026.94 | MIT-style (dropbear's own license, not GPL) |
| iproute2 | 7.2.0 | GPL-2.0 |

`bridge-utils` is deliberately not built at all (`packages/bridge-utils/README.md`),
so it does not appear above.

## Source for each release

Every release carries, as an asset next to the image, `<version>-sources.tar`
(its sha256 is in the release `SHA256SUMS`, and the attestation covers it):
the complete corresponding source of what the image contains, built by
`tools/bundle-sources.sh` in the release job. Inside, byte for byte what the
build downloaded and checked against its pinned SHA-256: the Linux kernel
tarball and its signature, busybox, dropbear, iproute2 and uClibc-ng (the
LGPL library linked statically into every binary); the odi-toolchain tag
with the patches, configs and build scripts of the toolchain image;
`confd` and `metricsd` at the tags the image has; and this repository at the
release commit (our kernel patches, drivers, configs, rootfs and build
scripts). gcc, binutils and the Linux headers the toolchain was built with
are build tools that do not ship, so they are not included: `SOURCES.md`
names them with upstream URL and pinned sha256 (gcc's runtime library parts
linked into binaries fall under the GCC Runtime Library Exception).
`SOURCES.md` lists each included component with version, license, upstream
URL and sha256, and `SOURCES.SHA256SUMS` checks every file. The release job
fails if the bundle is missing, empty or does not match its own sums.

To rebuild, unpack the `odi-oss-*` archive and follow `docs/BUILDING.md`
(`make image-all`; or drop the files into `dl/` to build without downloads).
To relink the statically linked LGPL library, uClibc-ng, against a modified
copy: build the toolchain image from the `odi-toolchain` archive with your
uClibc-ng source (its build also downloads the build tools above), point
`OSS_IMAGE=` at it (`toolchain/README.md`) and run
`make packages src image`; the packages and our tools are rebuilt from
source, so the relink is a rebuild with the same flags. `SOURCES.md` has the
same steps and the written offer for anything else.

## Our own tools: `src/`, the rootfs skeleton, every build script

`src/diag`, `src/omci`, `src/igmp`, `src/nv`, the rootfs skeleton
(`rootfs/skeleton/`), and every build script in this repo are entirely
ours, written for this project. They are licensed under the **GNU General
Public License, version 2 or later** (GPL-2.0-or-later). The drivers and
board code under `kernel/extra/` are also GPL-2.0-or-later, combined into a
GPL-2.0-only kernel as upstream ships it; the patches under
`kernel/618/patches/` modify upstream files and stay GPL-2.0-only.
The full GPL-2.0 text (correct for "or later") is in [`LICENSE`](../LICENSE).

### `src/diag`: what comes from the stock firmware, and what is ours

`diag` itself takes nothing from the stock firmware: its command table,
handlers and help text are written here, and it talks only to our own
kernel. The one stock-firmware dependency is compatibility, not content:
the commands the Prometheus exporter runs keep the stock CLI's syntax and
output format.

`src/diag/tools/regmap-extract.py` reads the switch register table out of
the stock `librtk.so` for the register listings `tools/regtrace` and
`tools/regdump` use, as data, at the time it is run, and never
redistributed as code. The table is numbers only -- each register's
address, width and array/port range, each field's bit position, width and
numeric id -- and those numbers are the whole of what is read.

**The register and field names are ours.** The binary carries none we use;
`src/diag/tools/regnames.txt` names, in our own words, the registers this
project's code and scripts touch, and every other register and field gets a
mechanical name built from its address and bit range (`SW_0x023038`,
`F_15_8`). Nothing here is taken from, or depends on, any other source of
register descriptions. `src/diag/README.md` has the details.

## `confd` and `metricsd`

Separate projects, fetched here as pinned release assets
(`src/fetch-releases.sh`) rather than built from source in this repository.
Both are licensed **GPL-2.0-or-later**, as the README of each states (the
`LICENSE` file in each is the GPL version 2 text): `confd` is built from
[odi-ui](https://github.com/AndreZiviani/odi-ui), `metricsd` from
[odi-sfp-exporter](https://github.com/AndreZiviani/odi-sfp-exporter). The
SBOM of a release (`tools/generate-sbom.sh`) records that license for both
instead of `NOASSERTION`, together with the SHA-256 of each binary and of the
web assets, which is the one in `src/release-sha256.txt`. `metricsd` also bundles one small piece of MIT code
that its own repository documents; the terms of the release a given image
carries are those of that tag in the project's repository.

## How each file says which license it has (REUSE)

The tree follows the [REUSE](https://reuse.software) specification. Source
files (C, headers, shell, Python, Makefiles, Kconfig, workflows, the rootfs
scripts) carry an `SPDX-FileCopyrightText` and an
`SPDX-License-Identifier` header. Files that take no comment, or that tests
compare byte for byte (documents, test goldens and fixtures, the register
replay tables, images, the kernel config, the rootfs config files, the
patches, the generated sources in `src/omci/generated/`), are annotated in
[`REUSE.toml`](../REUSE.toml). The license texts are in `LICENSES/`;
`LICENSE` stays as the top-level GPL-2.0 text. Everything is
GPL-2.0-or-later except the kernel patches (GPL-2.0-only, as above).
`make lint` runs `reuse lint` when the tool is installed, and CI always runs
it, with the version pinned and hash-checked in
`tools/reuse-requirements.txt`. A new file gets a header in the same
change; `reuse annotate --copyright "<year> <name>" --license
GPL-2.0-or-later <file>` writes it. `REUSE.toml` names the kinds of file
that take no header (markdown, text, XML and goldens, the replay data, a
few config files), never the whole tree, so `reuse lint` fails on a source
file that has no header.

## The OMCI information model

`src/omci/generated/omci_mib.[ch]` and `omci_mib.json` are generated by our
own `src/omci/tools/mib-gen.py` from two checked-in data files.
`src/omci/model/g988.json` holds facts of ITU-T G.988 (class and attribute
numbers, widths, access, flags, actions) with clause references; it holds
no wording, descriptions or tables of the Recommendation. Our developer
tool `src/omci/tools/g988-extract.py` reads them from the PDF, which is
neither stored nor fetched here; the edition, URL and SHA-256 of each PDF
used are recorded in the table. `src/omci/model/profile.json` holds the
device profile: names, local layout, the private classes and every
deliberate deviation from the derived values, taken from the model that
was already committed. No vendor binary, root filesystem or third-party
model code is an input of the generator.

## The SBOM

Each release carries `sbom.cdx.json` (CycloneDX 1.5), which this document's
tables feed: the license of each component is the one in `tools/sbom.py`, so
change it there when a license changes here. Runtime components (what is on
the flashed rootfs, with its files, hashes, purls and dependency graph) are
told apart from build tools (gcc, binutils: scope `excluded`).
`tools/verify-sbom.sh` checks it; `docs/FLASHING.md`, "Verifying a release",
says how.

## Public references this code is written against

`docs/REFERENCES.md` — the ITU-T documents the GPON code implements (G.984.3
and its Amendment 1), with their identity and checksums, fetched by
`tools/fetch-refs.sh` rather than stored here (their own copyright terms
forbid redistribution). G.988 (OMCI) and SFF-8472 (optics) are cited by
clause in the code but not fetched. Register names in the drivers and
scripts are ours (`src/diag/tools/regnames.txt`, above); no register
description from any other source is included in this repository.
