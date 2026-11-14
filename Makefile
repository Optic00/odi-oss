# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

# odi-oss — firmware for the ODI DFP-34X-2C2, built from source.
#
# Read README.md and docs/FLASHING.md before expecting a working image.

.PHONY: help kernel-tree toolchain toolchain-audit kernel packages busybox \
	src releases image image-all clean distclean lint test test-host test-host-asan test-omci \
	test-diag test-rcs test-qemu test-omci-respond test-igmp

help:
	@echo "targets:"
	@echo "  kernel-tree     fetch Linux 6.18 and pack it for the kernel build"
	@echo "  toolchain       pull the pinned toolchain images (toolchain/images.env)"
	@echo "  toolchain-audit  prove the target libraries carry no illegal instruction"
	@echo "  kernel          Linux 6.18 for the RTL9602C (kernel/build.sh)"
	@echo "  busybox         upstream busybox, ISA-audited"
	@echo "  packages        every userland package"
	@echo "  src             our own binaries, freestanding, into out/bin"
	@echo "  releases        metricsd and confd from their own releases"
	@echo "  image           assemble a flashable tarball into out/image/"
	@echo "  image-all       everything above from a clean clone, in order (about 25 min native)"
	@echo "  test            lint + the host-side tests + diag + the OMCI daemon under qemu, side by side (a few minutes)"
	@echo "  test-omci-respond  omcid unit tests (class 171 decode), test-igmp  igmpd unit tests"
	@echo "  lint            shellcheck every script here, the repo rules, REUSE compliance"
	@echo "  test-rcs        the rcS action trace against its goldens (needs out/busybox, docker)"
	@echo "  test-qemu       boot the real rootfs under qemu-system-mips, ssh/web/exporter + resilience scenarios"
	@echo
	@echo "Nothing here flashes anything. See docs/FLASHING.md."
	@echo "An image is INERT when flashed; nv setenv sw_tryactive <slot> boots it once."

# The pristine kernel tree, fetched and packed into build/kernel-618/tree.tar.
# The kernel build reaches into it directly (kernel/tree.sh, kernel/build.sh);
# the toolchain is prebuilt now and no longer needs it.
kernel-tree:
	./kernel/tree.sh

# Both toolchains are prebuilt container images from the odi-toolchain
# repository, pinned by digest in toolchain/images.env: gcc 16.2.0, binutils
# 2.47 and uClibc-ng 1.0.59 for the kernel and packages, and Debian
# gcc-mips-linux-gnu for src/. This pulls them; every build step pulls what
# it needs on its own too. toolchain/README.md has how to build them from
# source instead (OSS_IMAGE=, DIAG_IMAGE=).
include toolchain/images.env
OSS_IMAGE ?= $(OSS_IMAGE_PINNED)
DIAG_IMAGE ?= $(DIAG_IMAGE_PINNED)
QEMU_KERNEL_IMAGE ?= $(QEMU_KERNEL_IMAGE_PINNED)
export OSS_IMAGE DIAG_IMAGE QEMU_KERNEL_IMAGE

toolchain:
	./toolchain/image.sh oss
	./toolchain/image.sh diag

# The image build already refused any trapping instruction in libc.a and
# libgcc.a; this runs the same audit again against the image in use.
toolchain-audit: toolchain
	docker run --rm "$(OSS_IMAGE)" odi-audit-libs

# The kernel is built with the toolchain above (our binutils carries the three
# Lexra opcodes it needs): Linux 6.18, pristine tree fetched by
# kernel/618/fetch.sh into kernel/618/mainline, patched from kernel/618/patches,
# config seeded from kernel/618/config. kernel/README.md has the rest.
kernel: toolchain
	./kernel/build.sh

busybox:
	./packages/busybox/build.sh

packages: busybox
	./packages/dropbear/build.sh
	./packages/iproute2/build.sh

src:
	./src/build.sh

releases:
	./src/fetch-releases.sh

image: src
	./image/build.sh

# From a clean clone. Each step is its own target above and skips work it has
# already done (docker layers, pinned tarballs, the .built-with stamps). One
# sub-make per step rather than a prerequisite list, so the order is the one
# written here even under make -j: the kernel tree before the toolchain, the
# toolchain before anything compiled with it.
image-all:
	$(MAKE) kernel-tree
	$(MAKE) toolchain
	$(MAKE) toolchain-audit
	$(MAKE) kernel
	$(MAKE) packages
	$(MAKE) src
	$(MAKE) releases
	$(MAKE) image

# What runs without a stick. test-host needs only bash and the repo (the flash
# accessor against a sample store, the fwu.sh guards against captured /proc
# files); test-omci builds our binaries and drives omcid under qemu-user in the
# diag toolchain container (src/omci/qemu-test.sh, about 130 checks). The
# QEMU system harnesses under test/ that need a staged rootfs or a built kernel
# stay manual; each says so in its header.
#
# The legs of `make test` -- lint, test-host, test-diag and one per OMCI suite
# -- run side by side (tools/test-legs.sh, which says why that is not `make
# -j`): each is its own docker container or its own host process and writes
# nothing in common (the OMCI suites keep /tmp, /var/config and the SysV queues
# inside their container; a failed one leaves its daemon log in out/test-logs/).
# A leg prints one line when it ends, and its whole output only when it
# failed, so parallel output is never interleaved. All legs finish when one
# fails, so a run reports every failure, and `make test` exits non-zero when
# any did. TEST_JOBS is the number of legs at once (default: the CPUs);
# `make test TEST_JOBS=1` runs them one by one. `make test-<leg>` runs one leg
# on its own, its output streaming.
OMCI_SUITES := qemu drv resume events swdl vlan reload live keys tables getall alarms writes mcast pm enc voip
TEST_LEGS := lint test-host test-diag test-omci-respond test-igmp $(OMCI_SUITES:%=test-omci-%)
TEST_JOBS ?= $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)

test:
	@MAKE='$(MAKE)' tools/test-legs.sh 1 test-omci-build
	@MAKE='$(MAKE)' tools/test-legs.sh $(TEST_JOBS) $(TEST_LEGS)
	@echo "ok"

# The C unit tests of test-host again, built with AddressSanitizer and
# UndefinedBehaviorSanitizer (CI job host-asan). Every test/*.sh that compiles
# a C program does so through cc ${ODI_SAN_CFLAGS-}, empty in test-host, so
# this runs those same scripts, plus the host-built tests of src/nv and src/diag
# (the parser, conversions and formatters; the target builds stay in test-diag
# under qemu), with the flags
# set: no second copy of any build line. The shell-only tests have nothing to
# instrument and are not run here. -fno-sanitize-recover makes a UB report
# fatal (the default is to print and carry on, exit 0); ASAN_OPTIONS keeps
# leak detection on, which is what turns a missing free() into a failure.
# Every leg runs, so one report does not hide the rest.
# ASAN_EXCLUDE names the scripts left out, each with the reason; none so far.
ASAN_EXCLUDE :=
test-host-asan:
	@command -v cc >/dev/null || { echo "test-host-asan: no host cc" >&2; exit 1; }
	@fail=0; \
	export ODI_SAN_CFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined -g'; \
	export ASAN_OPTIONS=detect_leaks=1:abort_on_error=0; \
	export UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1; \
	for t in $$(grep -l ODI_SAN_CFLAGS test/*.sh); do \
		case " $(ASAN_EXCLUDE) " in *" $$t "*) continue;; esac; \
		echo "== $$t"; bash $$t || { echo "FAIL $$t"; fail=1; }; \
	done; \
	echo "== src/nv"; $(MAKE) -C src/nv test || { echo "FAIL src/nv"; fail=1; }; \
	echo "== src/diag (host part of test)"; $(MAKE) -C src/diag test || { echo "FAIL src/diag"; fail=1; }; \
	[ $$fail = 0 ] || { echo "test-host-asan: failures above" >&2; exit 1; }; \
	echo "test-host-asan: ok"

test-host:
	sh src/omci/testlib-test.sh
	bash test/root_pw_test.sh
	bash test/bundle_sources_check_test.sh
	bash test/sbom_test.sh
	bash test/fetch_releases_test.sh
	bash test/flash_test.sh
	bash test/slot_state_test.sh
	bash test/rcs_dev_test.sh
	bash test/trial_guard_test.sh
	bash test/release_ancestry_test.sh
	bash test/toolchain_image_test.sh
	$(MAKE) -C src/nv test
	bash test/fwu_guard_test.sh
	bash test/fwu_minimal_path_test.sh
	bash test/fwu_write_order_test.sh
	bash test/igmp_rows_test.sh
	bash test/fwu_starter_test.sh
	bash test/network_addr_test.sh
	bash test/apply_test.sh
	bash test/host_autoneg_test.sh
	bash test/host_serdes_test.sh
	bash test/sfp_host_ddm_test.sh
	bash test/mgmt_addr_test.sh
	bash test/safe_mode_test.sh
	bash test/ntp_event_test.sh
	bash test/gpon_hold_test.sh
	bash test/svc_igmpd_test.sh
	bash test/omci_respawn_test.sh
	bash test/omcli_table_test.sh
	bash test/omci_defaults_test.sh
	bash test/rcs_cs_value_test.sh
	bash test/log_trim_test.sh
	bash test/services_off_test.sh
	bash test/respawn_budget_test.sh
	bash test/regtrace_decode_test.sh
	bash test/regtrace_compare_test.sh
	bash test/regtrace_compare_stream_test.sh
	bash test/odi_nic_hw_test.sh
	bash test/odi_switch_test.sh
	bash test/odi_switch_mcast_test.sh
	bash test/odi_switch_acl_probe_test.sh
	bash test/odi_switch_mc_count_test.sh
	bash test/mcast_probe_test.sh
	bash test/count_proof_test.sh
	bash test/odi_igmp_rx_test.sh
	bash test/odi_switch_mmio_bounds_test.sh
	bash test/odi_switch_tbl_desc_test.sh
	bash test/odi_switch_l2_test.sh
	bash test/odi_switch_dal_test.sh
	bash test/odi_reg_test.sh
	bash test/odi_switch_cmd_test.sh
	bash test/odi_switch_flood_test.sh
	bash test/odi_switch_uni_limits_test.sh
	bash test/odi_switch_dsq_test.sh
	bash test/odi_switch_dssteer_test.sh
	bash test/odi_switch_cmd10_test.sh
	bash test/odi_switch_isp2_test.sh
	bash test/odi_switch_bdgconn_vlan_test.sh
	bash test/odi_switch_bdgconn_extvlan_test.sh
	bash test/odi_switch_bdgconn_uplink_test.sh
	bash test/odi_switch_init_platform_test.sh
	bash test/odi_switch_acl_clean_test.sh
	bash test/odi_host_pause_test.sh
	bash test/odi_host_autoneg_test.sh
	bash test/odi_host_forced_test.sh
	bash test/odi_pon_isolation_test.sh
	bash test/odi_omci_test.sh
	bash test/regdump_test.sh
	bash test/mkmodload_test.sh
	bash test/odi_switch_modload_replay_test.sh
	bash test/mksdkinit_test.sh
	bash test/odi_switch_sdkinit_replay_test.sh
	bash test/odi_replay_blob_test.sh
	bash test/odi_gpon_init_clean_test.sh
	bash test/odi_switch_ds_encrypt_test.sh
	bash test/odi_switch_ds_flow_del_test.sh
	bash test/omci_ageing_time_test.sh
	bash test/odi_gpon_test.sh
	bash test/odi_gpon_pw_test.sh
	bash test/odi_gpon_aes_key_test.sh
	bash test/odi_gpon_replay_test.sh
	bash test/odi_reregister_test.sh
	bash test/odi_alloc_cam_safety_test.sh
	bash test/odi_gpon_irq_test.sh
	bash test/odi_gpon_us_ploam_test.sh
	bash test/odi_gpon_ds_ploam_test.sh
	bash test/odi_gpon_dlog_test.sh
	bash test/odi_gpon_cam_test.sh
	bash test/odi_gpon_o7_test.sh
	bash test/odi_gpon_hold_test.sh
	bash test/odi_gpon_los_test.sh
	bash test/odi_gpon_eqd_test.sh
	bash test/odi_gpon_ctr_test.sh
	bash test/odi_gpon_drop_test.sh
	bash test/odi_gpon_flow_test.sh
	bash test/odi_board_test.sh
	bash test/odi_i2c_test.sh
	bash test/odi_ddm_test.sh
	bash test/odi_sfp_mirror_test.sh
	bash test/odi_optics_model_test.sh
	bash test/odi_ddm_limits_test.sh
	bash test/odi_init_test.sh
	bash test/odi_wdt_test.sh
	bash test/odi_hwmon_test.sh
	bash test/rtl8686_time_test.sh
	bash test/odi_ramlog_test.sh
	bash test/procparse_test.sh
	bash test/odi_switch_flows_test.sh
	bash test/odi_switch_ponq_test.sh
	bash test/boot_golden_test.sh
	python3 test/regnames_kernel_test.py

# diag's parser and conversion tests, natively, then under qemu in the diag
# toolchain container: the conversion vectors and the exporter contract (the
# batch metricsd sends, byte for byte against test/exporter.golden).
test-diag:
	$(MAKE) -C src/diag test selftest

# Runs as root in the container: the harness creates /var/config and other
# system paths inside it, and writes nothing into the mounted tree.
# drv-test.sh is the driver-call golden, against a test build of omcid.
# resume-test.sh replays that same golden across a kill -9 and respawn:
# the resumed instance's MIB and switch bookkeeping must come back
# identical, with zero driver calls. swdl-test.sh runs a whole OLT software
# download, activate and commit in both OLT_SW_DOWNLOAD modes, under
# qemu -strace, and asserts nothing opens /dev/mtd, execs or reboots.
# vlan-test.sh builds both ISP sessions with the manual tag on and off
# (transparent) and checks the bridge rules. voip-test.sh provisions a SIP
# line the way an OLT does, with OMCI_POTS_PORTS on, and checks that with it
# off the VoIP classes are still unknown to the OLT.
# One target per suite (test-omci-<suite>) so CI can run them as parallel
# jobs; test-omci runs them all side by side, as `make test` does (see there).
# Each suite needs only omcli and omcid (plus its drvtrace build), which
# test-omci-build makes: the rest of src/ is built by `make src`. The suites
# share src/omci/testlib.sh: bounded waits for the line a check needs, and a
# SIGTERM to stop the daemon instead of waiting out its idle exit.
test-omci:
	@MAKE='$(MAKE)' tools/test-legs.sh 1 test-omci-build
	@MAKE='$(MAKE)' tools/test-legs.sh $(TEST_JOBS) $(OMCI_SUITES:%=test-omci-%)

# The unit tests of two freestanding binaries, built and run under qemu-user in
# the same container as the suites: omcid's class 171 decode and OLT-tag rule
# (src/omci/respond/../test/extvlan_test.c), its class 309 multicast tag rules
# (mcastvlan_test.c beside it) and igmpd's frame parse, group
# table and switch-entry tests (src/igmp/test). Each is its own leg, with its
# own build directory, so they run beside the suites.
test-omci-respond:
	$(MAKE) -C src/omci/respond test

test-igmp:
	$(MAKE) -C src/igmp test

test-omci-build:
	$(MAKE) -C src/omci/cli
	$(MAKE) -C src/omci/respond cap drvtrace
	$(MAKE) -C src/igmp igmpd

$(OMCI_SUITES:%=test-omci-%): test-omci-%:
	docker run --rm -v "$(CURDIR)":/src -w /src/src/omci "$(DIAG_IMAGE)" sh $*-test.sh

# The rcS action trace: what rcS executes and writes under /proc, for
# three flag sets, against test/fixtures/rcs-trace-*.txt. Not part of `test`:
# it needs a built busybox and a container allowed to ptrace.
test-rcs:
	bash test/rcs_trace_test.sh

# Full-system: the real rootfs (busybox, inittab, services, dropbear,
# confd, metricsd, our sysctls and tmpfs caps) booted under
# qemu-system-mips on a STOCK malta kernel (odi-toolchain-qemu-kernel-malta,
# toolchain/images.env), never our own RTL9602C kernel -- see
# test/qemu/run-qemu.sh header and docs/HACKING.md for exactly what this
# does and does not cover. Needs busybox, packages, src and releases built
# first (out/busybox, out/*), and qemu-system-mips on PATH.
test-qemu:
	./toolchain/image.sh qemu-kernel >/dev/null
	./test/qemu/build-initramfs.sh
	./test/qemu/run-qemu.sh

lint:
	@# Every rule below that starts with git grep passes when git grep cannot
	@# run at all (exit 128, no repository): an if on its exit status reads
	@# that as "no match". A tree whose .git is missing or dangling -- a
	@# worktree copied without its gitdir -- would pass lint vacuously, so
	@# refuse to run unless git has an index with this Makefile in it.
	@git ls-files --error-unmatch Makefile >/dev/null 2>&1 || { \
		echo "lint: not a usable git checkout (git cannot list the tracked files here); the git grep rules would pass without checking anything. tools/remote-build.sh builds a throwaway repository for a worktree." >&2; \
		exit 1; \
	}
	shellcheck -S warning toolchain/*.sh kernel/*.sh packages/*.sh packages/*/*.sh \
	          image/*.sh tools/*.sh src/*.sh test/*.sh test/qemu/*.sh
	@# The scripts that actually run ON the device, checked as POSIX sh
	@# because busybox ash is what interprets them -- not bash. These were
	@# outside the lint entirely until 2026-09-14, which is backwards: they
	@# are the only ones whose failure costs a stick rather than a build.
	@# tools/regdump/dump.sh and tools/mcast-probe/*.sh join them
	@# here for the same reason: each is pushed to and run on the stick,
	@# against a minimal busybox, not built or run on the host. test/qemu/diag-stub.sh and memhog.sh are
	@# the same shape one step removed: they run under busybox ash INSIDE
	@# the qemu guest, never on the build host.
	shellcheck -S warning -s sh rootfs/skeleton/etc/init.d/* rootfs/skeleton/etc/scripts/*.sh rootfs/skeleton/etc/scripts/flash tools/regdump/dump.sh tools/mcast-probe/mcast-probe.sh tools/mcast-probe/count-proof.sh test/qemu/diag-stub.sh test/qemu/memhog.sh
	@echo "shellcheck: clean"
	@./tools/check-inline-quotes.sh toolchain/*.sh kernel/*.sh packages/*.sh \
	          packages/*/*.sh image/*.sh src/*.sh tools/*.sh test/*.sh test/qemu/*.sh
	@# This repo is public; the private investigation workspace it was
	@# developed alongside is not, and none of its paths or document names
	@# may leak into tracked files here. Excludes this Makefile itself,
	@# since the pattern below necessarily contains the strings it looks for.
	@if git grep -nE 'investigations/|odi-sfp-re|SPEC-(NIC|GPON)|NIC-ABI|PLAN-[A-Z]|INVENTORY-|ANALYSIS-|RESEARCH-|REVIEW-|NOTES\.md|KB-DRAFTS|vendor-src/|regtrace/(omci|gpon|sdkinit|nic)/|kb/systems/|kb/decisions/|kernel/patches/|kernel/vendor/|~/git/' -- ':!Makefile'; then \
		echo "lint: found a reference to the private investigation workspace above -- restate the point in this repo own words instead" >&2; \
		exit 1; \
	fi
	@echo "no private-workspace references: clean"
	@# Paths that exist on one machine only, or in private material: a home
	@# directory in any spelling, /root/ outside the documented remote
	@# build directory and the container paths of the test harnesses, the
	@# backups/ of a stick and the private stick inventory. A script that
	@# needs such a path takes it from a variable (STOCK_ROOTFS, ...).
	@if git grep -nE '~/[A-Za-z]|\$$\{?HOME\}?/|\$$\(HOME\)/|/Users/|/home/[a-z]|/root/|backups/|OUR-STICKS' -- ':!Makefile' | \
		grep -vE '/root/(odi/odi-oss|fs|stick|p[0-9]+\.img)([^A-Za-z0-9_-]|$$)|/root/fs-'; then \
		echo "lint: found a machine-local or private path above -- take it from a variable, or describe it instead" >&2; \
		exit 1; \
	fi
	@echo "no machine-local or private paths: clean"
	@# Trial history in code and scripts: trial, image and boot ids, dates,
	@# old patch numbers, "this pass". Git history is the record; a comment
	@# says what is true of the code now (docs/HACKING.md, "Repo rules").
	@# Test fixtures are exempt, their names carry dates on purpose.
	@if git grep -n -i -E 'this pass|coordinator|trial (618|[a-z]{1,2}[0-9])|(^|[^a-z0-9_.-])618[a-z]{1,2}[0-9]*([^a-z0-9_]|$$)|(^|[^a-z0-9_])[a-z][0-9]{1,2} (trial|fixes|fix)([^a-z]|$$)|20[0-9][0-9]-[01][0-9]-[0-3][0-9]|0004(.s| own| did)|(^|[^a-z0-9_])boots? [0-9]+(-[0-9]+)?,? 20[0-9][0-9]' \
		-- kernel/extra rootfs/skeleton src ':!src/omci/test' ':!src/omci/generated' ':!*.golden' ':!*fixtures*'; then \
		echo "lint: trial history in a comment above -- say what is true now; git history keeps the story" >&2; \
		exit 1; \
	fi
	@echo "no trial history in code: clean"
	@# Internal plan/task IDs (R1.1, D3, M9, U1, G-trace, T2, "batch A"):
	@# a goldens name is described by what it is, not by its gate name; a
	@# milestone by what it does, not by a trial image name. CHANGELOG.md
	@# is the one place a release name belongs.
	@if git grep -n -I -E '(^|[^A-Za-z0-9_.])R[1-4]\.[0-9]+([^A-Za-z0-9_]|$$)|(^|[^A-Za-z0-9_.])D[1-9]([^A-Za-z0-9_]|$$)|(^|[^A-Za-z0-9_.])M(1[0-3]|[1-9])([^A-Za-z0-9_]|$$)|(^|[^A-Za-z0-9_.])U1([^A-Za-z0-9_]|$$)|G-(trace|drv|rcs|host|ident|size|hw)|(^|[^A-Za-z0-9_.])T[1-4]([^A-Za-z0-9_]|$$)|\bbatch [AB]\b|since (the )?(step )?(R[1-4]\.[0-9]+|D[1-9]|M[1-9][0-3]?|U1|T[1-4])\b' \
		-- kernel/extra rootfs/skeleton src tools test docs '*.md' \
		':!Makefile' ':!CHANGELOG.md' ':!src/omci/test' ':!src/omci/generated' \
		':!*.golden' ':!*fixtures*'; then \
		echo "lint: an internal plan/task id in a comment above -- describe what the thing is, not the step that made it" >&2; \
		exit 1; \
	fi
	@echo "no internal plan/task ids in code: clean"
	@# AGENTS.md: describing how the stock (OEM) firmware behaves, as an
	@# observed black box (a shipped binary own exported symbols, librtk.so
	@# and the like), is fine; naming a file or function read out of the
	@# vendor own SOURCE is not. This catches known vendor SOURCE citations
	@# specifically -- it is intentionally narrow (not e.g. a bare "rtk_" or
	@# "bsp_" prefix) because those prefixes are also how this tree spells
	@# sockopt ABI names and its own driver symbols.
	@# The headers of the vendor SDK count too (rtk/l2.h, rtk_*.h): a
	@# header name says where a fact was read.
	@if git grep -nE 'prom\.c|re8686|c-rlx\.c|apollo|bsp_[a-z_]+\(|(^|[^A-Za-z0-9_])(rtk|rtdrv|hal|dal|osal|ioal)/[A-Za-z0-9_/]+\.h|(^|[^A-Za-z0-9_])(rtk|rtdrv|rtusr)_[a-z0-9_]+\.h' -- . ':!Makefile'; then \
		echo "lint: found a vendor SOURCE file/function reference above -- describe the observed (black-box) behavior instead, and put vendor attribution in commit history/docs, per AGENTS.md" >&2; \
		exit 1; \
	fi
	@echo "no vendor source citations: clean"
	@# CI runs each OMCI suite as a job of its own, from a matrix that is a
	@# second list of them; three suites were left out of it and ran only
	@# under make test. The two lists must name the same suites.
	@m=$$(sed -n 's/^ *suite: \[\(.*\)\]$$/\1/p' .github/workflows/ci.yml | tr -d ' ' | tr ',' '\n' | sort); \
	o=$$(printf '%s\n' $(OMCI_SUITES) | sort); \
	if [ "$$m" != "$$o" ]; then \
		echo "lint: the omci matrix in .github/workflows/ci.yml and OMCI_SUITES differ:" >&2; \
		printf '%s\n' "$$m" > /tmp/odi-lint-m; printf '%s\n' "$$o" > /tmp/odi-lint-o; \
		diff /tmp/odi-lint-m /tmp/odi-lint-o >&2; rm -f /tmp/odi-lint-m /tmp/odi-lint-o; \
		exit 1; \
	fi
	@echo "CI omci matrix matches OMCI_SUITES: clean"
	@# Every action in the workflows is pinned to a full commit SHA with its
	@# version in a trailing comment; a tag can be moved, a SHA cannot.
	@if grep -nE '^[[:space:]]*(-[[:space:]]+)?uses:[[:space:]]*[^./[:space:]][^@[:space:]]*@' .github/workflows/*.yml | \
		grep -vE '@[0-9a-f]{40}[[:space:]]+#[[:space:]]*v[0-9]'; then \
		echo "lint: a workflow action above is not pinned to a full commit SHA with a version comment" >&2; \
		exit 1; \
	fi
	@echo "workflow actions pinned to commit SHAs: clean"
	@./tools/check-doc-pins.sh
	@echo "documented version pins match the code: clean"
	@# REUSE: every file has a copyright and a license, in its header or in
	@# REUSE.toml, and every license used has its text in LICENSES/. The tool
	@# is optional locally (pip install --require-hashes -r
	@# tools/reuse-requirements.txt); CI sets ODI_REQUIRE_REUSE=1 so a missing
	@# tool fails there instead of passing silently.
	@if command -v reuse >/dev/null 2>&1; then \
		reuse lint -q && echo "REUSE compliance: clean"; \
	elif [ -n "$$ODI_REQUIRE_REUSE" ]; then \
		echo "lint: reuse is required here (ODI_REQUIRE_REUSE) but is not installed" >&2; \
		exit 1; \
	else \
		echo "REUSE compliance: skipped, reuse not installed (see tools/reuse-requirements.txt)"; \
	fi
	@# The kernel hardening options stay stated in the seed; kernel/build.sh
	@# runs the same script on the completed .config (docs/KERNEL.md,
	@# "Hardening").
	@./tools/check-kernel-hardening.sh

clean:
	rm -rf build out

# Also drops the kernel trees and the download cache. The Docker volume the
# build uses -- odi-kbuild-618 (the kernel build workdir) -- survives this on
# purpose: it costs real time to rebuild and nothing in the tree depends on
# its contents. The toolchain images are docker images, not files here.
# Remove the volume deliberately:
#
#     docker volume rm odi-kbuild-618
distclean: clean
	rm -rf kernel/618/mainline dl
