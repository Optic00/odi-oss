# The kernel

Linux **6.18.65**, the current longterm release, built for the RTL9602C.
There is no dual-kernel build any more: `kernel/build.sh` builds 6.18 and
nothing else, from a pristine `cdn.kernel.org` tree with our own patches and
drivers on top — no vendor kernel tree is fetched, patched or linked at any
point in this build.

    kernel/618/fetch.sh    fetches and verifies linux-6.18.65.tar.xz
    kernel/618/mainline/   the pristine tree it extracts, gitignored
    kernel/618/patches/    our edits to mainline files, applied in order
    kernel/618/debug/      debug-only edits, applied with CRUMBS_CORE=1
    kernel/extra/          every file of our own, laid over the tree at its path
    kernel/618/config      the seed config; make olddefconfig completes it
    kernel/build.sh        builds it all, in a container, with our toolchain

## Patches and the overlay

The port is split by what it costs to carry to the next longterm kernel.
A file of our own is copied across; a line changed in a file mainline
already has must be re-applied, and re-read, against new code. So:

- `kernel/extra/` holds every file this port adds, at its path in the
  kernel tree: `arch/mips/rtl8686/` (the board file, the RLX5281 cache
  code `cache.c`, the interrupt controller `irq.c`, the TIMER0 clock
  event `time.c`, Kconfig, Makefile and Platform),
  `arch/mips/include/asm/mach-rtl8686/` (kernel entry, feature overrides,
  IRQ numbering, register map, the early-crumb helpers),
  `drivers/mtd/devices/rtl8686-spiflash.c` and our driver directory
  `drivers/net/ethernet/odi/`. `kernel/build.sh` copies every file but the
  `README.md` files to the same path under the build tree, after the
  patches, on every run; an edit to an overlay file alone rebuilds
  incrementally. Adding or removing an overlay file changes the patch
  stamp and re-extracts the tree, so a deleted file does not linger.
- `kernel/618/patches/` holds only edits to files mainline already has:
  the hooks that make the overlay reachable (Kconfig entries and
  `source` lines, Makefile `obj-` lines) and the few core changes the CPU
  needs. `0001` is the RLX5281 PRId probe case, `0002` the board's
  System type entry, its Kconfig `source` and the `Kbuild.platforms` line, `0004` the SPI NOR driver's Makefile line and
  `0005` the two lines that reach our driver directory. The numbers are
  stable names; gaps are patches that no longer exist.
- `kernel/618/debug/` holds patches the normal build does not apply
  (see Early crumbs below).

`tools/kernel-footprint.sh` counts the second kind, per patch: the lines
changed in mainline files and the number of those files. Run it after
touching a patch; the number is the porting cost.

Every file in `kernel/618/patches/` and `kernel/618/debug/` is `git
format-patch` style: `From:`, `Date:`, `Subject:` headers, the commit body,
a bare `---` separator line, then the plain unified diff -- no `diff --git`
extended header (`kernel/build.sh` applies them with `patch -p1`, not
`git am`, so it does not need one) and deliberately no `Signed-off-by:`
line: this project signs commits with the maintainer's own SSH key, not
the kernel community's DCO process, so a sign-off line here would assert
a process this repo does not follow. `checkpatch.pl --no-tree` on these
files is otherwise clean (aside from `TRAILING_WHITESPACE` inherited from
mainline context lines, which is not a patch-format issue and is left
alone).

## The board

A single fixed board, `MACH_RTL8686`: 32 MB DRAM, one UART, SPI NOR flash, no
device tree. There is no generic "RTL8686 platform" to configure around —
the board file registers exactly this stick's UART and flash controller and
reserves the PON DMA windows, and the boot command line (which slot, which
MAC, etc.) comes from U-Boot's `argv`, built per boot slot.

Two things in the board file exist because the hardware is not idle when
this kernel starts:

- **The NIC DMA is stopped in `prom_init()`.** The loader that runs before
  us leaves the CPU-port NIC's receive engine running, on its own
  descriptor ring and buffers. `odi_nic` resets and reprograms the NIC
  only at its first `ndo_open`, from rcS, so until then every frame that
  arrived was written through the loader's descriptors: the payload into
  its buffers, the descriptor write-back into its ring slot, both in pages
  this kernel had long since handed out. The symptom was a page of
  `/bin/busybox` text in the page cache overwritten after init started, and
  every exec of it taking the same SIGBUS until the watchdog reset the
  board, in roughly one boot in six on some kernel layouts. `prom_init()`
  now writes 0 to the NIC's two RUN registers (`RTL8686_NIC_RUN`,
  `RTL8686_NIC_RUN1`, `rtl8686regs.h`) with two uncached stores, before
  this kernel owns a single page; 20 of 20 boots of the same layout were
  clean after it.
- **The PBO DMA windows are reserved where the hardware has them.** The
  PON MAC's packet-buffer engine owns two DRAM ranges, each 1 MiB plus a
  4 KB barrier page: downstream `[0x016ff000, 0x01800000)` and upstream
  `[0x01eff000, 0x02000000)`, the bases every replayed switch init writes
  to the two PBO base registers. `plat_mem_setup()` `memblock_reserve()`s
  both. The downstream reservation used to start one page higher, at
  `0x01700000`, which gave the engine's first page to the page allocator.
  The two ramlog pages (below) are the barrier pages of these windows, so
  the same reservations protect them.

## The CPU

The RLX5281 is a Lexra core, R3000-class rather than a full MIPS32
implementation: it keeps CP0 `Status` as a KU/IE stack, returns from
exceptions with `rfe`, and uses a 6-bit ASID. The kernel is built as
plain `CONFIG_CPU_R3000` (the board selects `SYS_HAS_CPU_R3000`), so
mainline's own R3000 branches supply that exception model, the TLB code
and the context switch; no core file is edited for it. `objdump -d
vmlinux | grep -cw eret` must print 0. Its caches are driven
through a Lexra CP0 register (CCTL) and are board code:
`arch/mips/rtl8686/cache.c`, installed from `plat_mem_setup()`, before
mainline's own `cpu_cache_init()` (which then finds nothing to install
for this CPU and only builds the page protection map: the feature
overrides set `cpu_has_3k_cache` to 0, so the R3000 cache code, built
under `CPU_R3000`, never installs).

The icache does not snoop the dcache, and nothing in mainline
invalidates it when a physical page gets new contents. Every path that
does give a page new contents for a process ends in
`flush_data_cache_page()` before the page is mapped: `__update_cache()`
from `set_ptes()` for a page cache folio that `flush_dcache_folio()`
marked (squashfs, jffs2 and tmpfs fills, `write()`, a folio compaction
migrated -- `CONFIG_COMPACTION` is on), and `copy_user_highpage()` for
every COW copy. R3K TLBs have no no-exec bit (`_PAGE_NO_EXEC` is 0, so
`__update_cache()` treats every mapping as executable), and without
RIXI every binary lacking `PT_GNU_STACK` runs READ_IMPLIES_EXEC, so
nearly every VMA is `VM_EXEC`. Our `flush_data_cache_page()` writes the
page's dcache lines back (per-line op 0x1b through the KSEG0 alias,
which also drops the icache lines at that colour), drains the write
buffer with a `sync`, then invalidates the whole icache through CCTL.
`flush_cache_page()` on a `VM_EXEC` mapping (COW, reclaim, migration,
and `copy_to_user_page()` after a ptrace or `/proc/pid/mem` write) does
the same. `prom_free_prom_memory()`, the board hook `free_initmem()` calls
just before the `__init` sections are freed, flushes both caches once;
it is not `__init` itself, so no init line is refilled after it.

Why whole-cache: the per-line invalidate reaches only lines at the
colour of the address it is given. How the icache is indexed is not
documented in anything we have and was never measured; 64 KB with 32-byte
lines would need 16 ways to be free of aliases at 4 KB pages, so a
virtually indexed one very likely has 8 KB or more per way, and lines of
a physical page filled through a user mapping at another colour survive
an invalidate through the kernel alias. The fix assumes nothing about the
indexing: a whole-cache invalidate is correct for any of them. Not a
concern: an ASID-tagged virtual icache (no flush on ASID wrap,
`cpu_has_vtag_icache` 0) would fail on most boots, since rcS wraps the 64
ASIDs many times.

The history, corrected. This work started from boots where init or
busybox `login` died of SIGBUS right after `Run /sbin/init`, and a stale
icache fitted the first of them. The later ones showed that memory itself
was wrong: `print-fatal-signals=1` (see "The config") printed the
registers, and the faulting word in the page cache decoded as an RX
descriptor. The real cause was the NIC DMA the loader leaves running (see
"The board"), which is now stopped. The icache change stays: the hole it
closes is real (nothing else invalidates the icache when a page gets new
contents) and it cost no measurable boot time, but it is not what made
those boots fail.

Cost. CCTL clears the icache as one operation; no document we have gives
its cycle count, and it is at most one cycle per line (2048 lines) if it
walks the array. The real cost is the refill afterwards: the kernel and
the faulting process fetch their working set again, a few hundred 32-byte
lines, on the order of 10-50 us per invalidate at this clock. It runs
once per newly filled page cache page at its first mapping, once or twice
per COW fault, and once per page reclaim or compaction unmaps from an
executable mapping; back-to-back invalidates cost only the lines fetched
between them. Boot (rcS forks a few hundred commands) should grow by well
under a second; steady state, with a handful of long-lived processes,
does almost none of these. The per-line hooks for kernel addresses
(`flush_icache_range()`, the DMA hooks) are unchanged.

Barriers are the one place where the core is not an R3000.
`CPU_R3000` turns `CPU_HAS_SYNC` off and `CPU_HAS_WB` on. Without
`CPU_HAS_SYNC`, mainline `wmb()`, `rmb()`, `dma_wmb()` and `__sync()`
emit no instruction. With `CPU_HAS_WB`, `mb()`, `iob()` and the barrier
`readl()`/`writel()` issue before each access call the board's
`__wbflush`, which `board.c` points at a `sync`: the RLX5281 implements
`sync`, and it drains the write buffer. So every MMIO access and every
`mb()`/`iob()` (the cache code's DMA hooks included) still issues a
`sync`, through an indirect call. Our own DMA-ordering points (a NIC
descriptor body before its ownership bit, the returned RX ownership bits
before the engine stops) use `rtl8686_sync()`
(`asm/mach-rtl8686/rtl8686-barrier.h`), an inline `sync` that does not
depend on `CPU_HAS_SYNC`; a plain `wmb()` there would silently be
nothing. The only `sync` this build drops is the `rmb()` at the end of
each `readl()`/`readw()`/`readb()`, which keeps later loads of DMA
memory from being satisfied ahead of the MMIO read. The NIC does not
depend on it: its rings come from `dma_alloc_coherent()` (uncached), and
its packet buffers are invalidated through the cache hooks, which do
issue a `sync`. Whether this core lets a load pass an outstanding
uncached load has not been measured. `BUG_ON()`
must stay trap-free: at `-march=mips2` mainline builds it on `tne`, one of
the instructions this core does not implement. `kernel/build.sh` passes
`-U_MIPS_ISA -D_MIPS_ISA=_MIPS_ISA_MIPS1` to the kernel C code, so
`asm/bug.h` falls back to the generic `if (cond) BUG()` form with no patch;
code generation is unchanged. `docs/CROSS-COMPILING.md`
has the full list of what this CPU does and does not implement, and why
every binary in the image is checked against it.

The core has to be named in the probe. It reports PRId `0x0000dc02`:
company byte 0 (legacy), implementation `0xdc`. Mainline builds
`cpu-r3k-probe.c` instead of `cpu-probe.c` whenever `CPU_R3K_TLB` is set,
and that file only knows the R2000 and R3000 implementation numbers. With
no case for `0xdc` the probe leaves the CPU name NULL, and the `BUG_ON()`
right after it executes a `break` before `trap_init()` has installed any
exception vector: a silent hang with nothing on any console. The fields
the probe sets are what the core has: a TLB of 64 entries, R3000-style
caches, no FPU. It does not call `__cpu_has_fpu()`, which toggles CP0
`Status` CU1 and reads it back, a sequence never tried on this core;
`cpu_has_fpu` is 0 at compile time anyway.

### Atomics, no thread pointer, and the empty first KiB

Three facts about this core and this image that look like gaps and are not,
or are gaps with a known cause.

**The core runs `ll`, `sc` and `sync`, so atomics are real ones.**
`cpu-feature-overrides.h` sets `cpu_has_llsc` to 1, and mainline then builds
`atomic_*`, bitops, `cmpxchg` and the spinlocks on them; uClibc-ng's own
atomics are the same instructions (`docs/CROSS-COMPILING.md`). We know it
three ways: the three instructions were executed on the device in user mode
(`docs/kb/rtl9601-isa-map.md`), a kernel and a userland built on them have
run on the sticks, and another open implementation of this board measured
the same independently (a million atomic increments exact, with `sync`
executing rather than trapping). `/proc/cpuinfo` does not list `ll/sc` among the options:
`show_cpuinfo()` just has no line for it, so its absence proves nothing.

**There is no thread pointer, so no TLS.** On MIPS Linux the thread pointer
is read with `rdhwr $3,$29` (hardware register 29, `ULR`). On our kernel it
does not work: a user-mode `rdhwr $3,$29` on 6.18.57 dies with SIGSEGV at the
instruction itself (`BadVA 00000000`, `epc` the `rdhwr`; measured on a stick,
2026-10-20) instead of returning a thread pointer. Whether the core or our
R3000-model kernel is to blame has not been investigated (the vendor's own
kernel configuration for this core lists a TLS option). So nothing that
needs `__thread`, C11 `thread_local` or NPTL can run here, since compiled TLS
access starts with that instruction. The userland is single-threaded or uses
processes, and the freestanding tools have no thread library at all.

**The first KiB of the kernel image is an empty exception-vector area, which
is why linking at `0x80000000` is safe.** Mainline's
`arch/mips/kernel/head.S` starts with `.fill 0x400` unless the machine
selects `NO_EXCEPT_FILL`, and `MACH_RTL8686` does not (the built `.config`
has no such symbol). So `_text` is `0x80000000`, but the first code,
`_stext` and `__kernel_entry`, is at `0x80000400`, the first function at
`0x80000408`, and `trap_init()` copies the exception handlers over the reserved
bytes at the start of KSEG0 rather than over any code. `kernel_entry`, the
real entry point, is far into the image (`0x80321220` in a recent build),
which is why `kernel/build.sh` reads it from `System.map` and never uses the
load address as the entry (above). A kernel that does select
`NO_EXCEPT_FILL` has to be linked higher, at `0x80100000` for example, to leave
that space free; ours does not, so `0x80000000` is the right address by design.

## IRQ and timer

The board's own interrupt controller (`arch/mips/rtl8686/irq.c`, called
from `arch_init_irq()`) is a legacy IRQ domain fed from GIMR/GISR
registers, routing to CPU interrupt lines IP2–IP7. The system tick
(`arch/mips/rtl8686/time.c`, from `plat_time_init()`) comes from hardware
timer TIMER0, a clockevent with two modes. Periodic mode (control bit 24
set) auto-reloads at HZ=250 and is what the kernel boots with. Counter mode
(bit 24 clear) counts DATA ticks from the enable write, raises the interrupt
and stops, which is a oneshot comparator: `set_next_event()` writes the
delta (28 bits) and re-enables. In counter mode the divisor is 64, the same
3.125 MHz as the clocksource (320 ns a tick), against 200 kHz for the
periodic tick; each state callback writes the whole control word, so the
periodic tick is programmed exactly as before. The RLX5281 has
no CP0 Count/Compare, so the clocksource is a second SoC timer, TIMER1, run
free with no interrupt: a 28-bit counter at the LX clock / 64 (3.125 MHz,
wrapping about every 85.9 s), registered as `rtl8686-timer1` with a
matching `sched_clock`. On hardware (ISP1, 2026-10-08) it is the current
clocksource, and `/proc/uptime` advanced 61.02 s over 61 s of wall time
across a counter wrap, with no timekeeping-watchdog warnings. At
init the driver checks that the counter moves (and whether it counts up or
down) and, if it does not, registers nothing and keeps the jiffies
clocksource, so `ktime_get()` is then coarse (~1/HZ) and stands still with
interrupts off.

**Oneshot is opt-in at boot, behind a self-test.** Before registering the clockevent, `rtl8686_oneshot_selftest()`
arms a 1 ms event and then a minimum-delta (32 tick, about 10 us) event
through the same code `set_next_event()` uses, and polls the pending bit,
timing the wait on TIMER1 (bounded by 20 ms of TIMER1 time and a read
count, since `udelay()` is not calibrated that early). Only if both fire does
the device advertise `CLOCK_EVT_FEAT_ONESHOT`, and the dmesg line
`rtl8686-timer0: oneshot self-test passed (1000 us event fired after N us)`
says so. Otherwise it prints one warning, `oneshot self-test failed, staying
periodic`, and the kernel keeps the HZ tick it had before. The board needs
TIMER1 for the test, so a board without a working clocksource stays
periodic too. With oneshot on and TIMER1 as a continuous clocksource, the
core switches to it for `CONFIG_HIGH_RES_TIMERS`. On hardware (ISP1,
2026-10-08) the self-test passed, `/proc/timer_list` shows the clockevent
in oneshot mode with a 1 ns resolution, and the watchdog still reset the
stick about 60 s after omcid was stopped.

The tick stays periodic (`CONFIG_HZ_PERIODIC`). `CONFIG_NO_HZ_IDLE` was
tried and saved nothing: the idle tick never stopped and the timer
interrupt kept running at about 240 a second, since kernel timers expired
about every other tick. It could not save power anyway while the idle loop
spun.

**Idle sleeps.** `CPU_R3000` has no wait instruction, so with no `cpu_wait`
the idle loop spins at full speed. The RLX5281 implements the Lexra SLEEP
instruction (0x42000038, written as a `.word` since the assembler does not
know it), which stops the core until the next interrupt: `board.c` sets
`cpu_wait` to it. Interrupts are enabled just before SLEEP, so one that
lands in between is taken first and SLEEP then waits for the next
interrupt, at most one 4 ms tick later; that bound is why the tick stays
periodic. On ISP1 (2026-10-08) the stick ran about 1.9 C cooler relative to
the ISP2 stick on the previous image (the gap between the two module
temperatures went from 6.1-6.8 C to 4.4-4.8 C), and idle ping went from
0.28 to 0.33 ms.

Reprogramming a timer that is a few microseconds from expiry can lose its
interrupt on this timer family. `rtl8686_program()` therefore never
reprograms a running timer: it stops the counter, clears the stale pending
bit, writes DATA, then the enabling control word. The minimum delta keeps
the event further out than that write sequence takes, and as a backstop
TIMER1 gives the elapsed time: if a whole delta has passed and the pending
bit is not set, `set_next_event()` returns `-ETIME` and the core retries
with a longer delta. The 32-tick minimum passed the boot self-test; a lost
interrupt near expiry has not been provoked on purpose.

## NOR flash

The SPI NOR flash is not a memory-mapped CFI chip: the SoC SPI controller
issues discrete command/address/data SPI transactions through a control
and a data register, and a third register opens a memory-mapped read
window on the same controller. `rtl8686-spiflash.c` (in the overlay, built
by `patches/0004`) is a real MTD driver for it, registered as `rtk_spi_nor_mtd` — the name
U-Boot's own partition table (`mtdparts`) expects — so the fourteen
partitions U-Boot already knows about (two kernel slots, two rootfs slots,
the bootloader environment in duplicate, the JFFS2 config store, and some
small placeholders) show up unchanged under Linux.

## Our drivers

`kernel/extra/drivers/net/ethernet/odi/` is the one driver directory this
port adds, hooked into `drivers/net/ethernet/` by `patches/0005`. Every file
in it is GPL-2.0 and our own code, an independent implementation written
from public specifications (ITU-T G.984.3 and G.988 for GPON and OMCI,
SFF-8472 for optics), from register traces taken off this board's own
hardware, and from behaviour observed on the stock firmware. It is not an
optional alternative to a vendor driver: on this kernel tree there is no
vendor driver to fall back to, so this is simply how the board's networking
and GPON hardware are driven.

| file | what it drives |
|---|---|
| `odi_nic.c` | the CPU-port Ethernet DMA engine (rings, NAPI, TX/RX) |
| `odi_switch.c`, `odi_switch_{tbl,dal,cmd}.c` | the switch fabric: MMIO map, indirect table access, the OMCI command dispatch `omcid` drives it through |
| `odi_switch_l2.c` | the L2 lookup table: row readback and the valid-row walk (the MAC table), L2 multicast add/delete (igmpd), static unicast rows (the stick's MACs) |
| `odi_switch_mcast.c` | IGMP snooping's half of the switch: the ACL rule that copies the host port's IGMP to the CPU, the drop of unregistered IPv4 multicast from the PON, and the lease that takes both back (`docs/SWITCH.md`, "Multicast") |
| `odi_switch_pin.c` | the stick's MACs pinned to the CPU port as static L2 rows, kept in step by a netdevice notifier (`docs/SWITCH.md`, "The stick's MAC") |
| `odi_gpon*.c` | the GPON MAC block: the O1–O7 activation state machine and PLOAM codec (ITU-T G.984.3), register leaves, the interrupt handler, the switch interrupt line, whose one consumer it is, and the link counters (`odi_gpon_ctr.c`, below) |
| `odi_omci.c` | the netlink transport that carries OMCI frames between the kernel and `omcid`, and the copies of the host's IGMP to `igmpd` (redirect type 4, `odi_igmp_rx.h`) |
| `odi_board.c`, `odi_board_data.c` | board-init: LED and I2C core bring-up, an ordered 88-write replay built into the kernel (it runs before any filesystem), and the `laser` verb: the laser is switched on only when the laser driver id is the GN25L95 (0x6a), or after `laser unverified` (see `docs/BOOT.md`) |
| `odi_init.c`, `odi_switch_sdkinit.c`, `odi_gpon_init.c` | `/proc/odi_init`, the SDK-init and PON verbs rcS writes, and the register replays behind them |
| `odi_replay.c` | the one replay loop every captured write sequence goes through (`docs/SWITCH.md`, "Register replays") |
| `odi_soc.c` | the SoC system-controller window: one `ioremap()`, named offsets, the one allowlist every driver access there goes through (the replays, the watchdog, the optics) |
| `odi_replay_blob.c`, `odi_replay_fw.c` | the replay tables as firmware files: parser and loader (below) |
| `odi_i2c.c`, `odi_ddm.c` | the I2C bus to the optical module, and SFF-8472 DDM (temperature, voltage, bias, tx/rx power) readout |
| `odi_i2c_adapter.c` | that bus as i2c adapter `i2c-0` (SMBus byte-data reads, I2C block reads, and the A2h page-select write only), for `i2cget`/`i2cdump` on `/dev/i2c-0` |
| `odi_sfp_mirror.c` | the host-side SFP EEPROM: the SoC's SFP slave, which answers the host (the router) at 0x50/0x51 from a 512-byte SRAM (A0h 0-255, A2h 256-511), on the board revisions that wire it to the SFP edge (v08; on v06 the host reads a fixed EEPROM). `odi_switch.sfp_mirror=1` (written by `sfp-host-ddm.sh` at boot, from `SFP_HOST_DDM`) starts a work item that reads the module's A0h and A2h on the optics bus (each in one `odi_i2c_lock` hold, A2h 128-255 only from page 0, the password entry and page select zeroed), writes every SRAM byte through the CPU access register 0x2a8 (CMD bit 26, WRITE 25, ADDR 24:16, WDATA 15:8, RDATA 7:0; CMD clears when done, polled for at most 1 ms), reads all 512 back, sets the host write protect (0x268-0x2a4, one bit per byte, 1 = read-only; everything except A2h 128-247), and only then sets 0x2ac bit 0 (slave enable) and 0x23018 bit 23 (the SFP edge I2C pins to the slave), and clears bit 27 of PIN_GPIO_SELECT (0x48: 0x08082001 -> 0x00082001, as the stock v08 image runs it; on v08 the host reaches the slave only with pin 27 out of plain GPIO mode, Thaad-QR's comparison with a stock v08 stick), each a read-modify-write of that one bit; a self-check reads the three bits and a few SRAM bytes back and puts all three back on a mismatch. While the mirror holds pin 27 it tells `odi_board.c`, so a re-run of the `optics` verb writes 0x00082001 instead of taking the pin back (and a race with it is undone at the next refresh). Then every second A2h 96-119 (diagnostics, byte 110, the flags) is copied from the module into the bytes that changed, and the three bits are checked and set again if something undid them. Failures are logged once and once more when they end; the module is never written. `odi_switch.sfp_mirror=0` puts 0x48 bit 27 back as it found it, then clears 0x23018 bit 23, then 0x2ac bit 0: the pin, the host's way in, is given last and taken back first. Pin 27 on v06: nothing measured uses it (cleared for 30 s on a v06 stick, the module DDM kept updating, O5 and the host link stayed up). A0h byte 93 keeps only bits 7, 5 and 4 (what is served; host writes below A2h 128 are refused), CC_EXT moved by the same amount. On a stock v08 stick with host diagnostics working 0x23018 read 0x01824040 and 0x2ac 0x7 (measurements by Thaad-QR); this image sets the same two bits (and clears 0x48 bit 27) but leaves 0x23018 bit 13 (the low bit of the two-bit I2C master pin field, 14:13) as the board init sets it, where that stock stick has it clear: not explained, and not needed on the v08 board tested: pin 27 was what it took. Host-tested (`test/odi_sfp_mirror_test.c`) |
| `odi_reg.c` | `/dev/odi_sw`: register/SoC/MIB/DDM/L2-table access for our own userland (`diag`, `metricsd`, `igmpd`) |
| `odi_hwmon.c` | the SoC die temperature as a standard hwmon sensor, `/sys/class/hwmon/hwmonN` named `rtl9602c_soc`, `temp1_input` in millidegrees: switch-core words 0x138 (control, enable bit 16, left on by the boot) and 0x13c (status, bits 22:16 the reading in degrees C), both only read. The scale and offset are inferred, not calibrated (62 C on a stick whose optics module read 46 C; the stock firmware reads 63 to 65 C in the same state): a trend, good to a few degrees. Soft alarms on top: `temp1_max` (default 95000) and `temp1_crit` (default 110000), root-writable millidegrees in 0..150000 with max <= crit (raise crit before raising max past it), back to the defaults at every boot, and read-only `temp1_max_alarm` / `temp1_crit_alarm` (0/1). An alarm sets at reading >= limit and clears at reading <= limit minus 2 C; a reading with the sensor disabled changes nothing. Every sysfs read and a 10 s delayed work (one register read) go through the same path, which logs on transitions only (`pr_warn` max reached, `pr_crit` crit reached, `pr_info` per clear) and calls `hwmon_notify_event`. Nothing powers off or throttles: the scale is not verified, and the 95/110 defaults are the ones another open implementation's measurements chose for the same chip. Needs `CONFIG_HWMON`, which `ODI_HWMON` selects |
| `odi_wdt.c` | the watchdog kicker — see below |
| `odi_ramlog.c` | the DRAM ring-buffer console — see below |

The switch/GPON/OMCI/board pieces are all built statically into `vmlinux`;
there are no loadable kernel modules for the datapath, and none of this
repo's build can produce one — `image/build.sh` stages zero `.ko` files.
Every `CONFIG_ODI_*` symbol is `bool`, so nothing is exported either: the
drivers call each other through plain declarations in shared headers
(`odi_nic.h`, `odi_switch_api.h`).

### Register replay tables are firmware files

The three captured write sequences the drivers replay — the SDK-init
verbs (`sdkinit.bin`, 65 KB), the module-load replay (`modload.bin`,
26 KB) and the GPON boot init (`gpon_init.bin`, 29 KB) — are not compiled
into the kernel. They ship in the rootfs as `/lib/firmware/odi/*.bin`,
built by `image/build.sh` from their text under `tools/regtrace/tables/`
(`replayblob.py build`), and are loaded with
`request_firmware()` when a trigger needs one, then released: nothing
stays resident, which gave back about 484 KB of RAM and 8.5 KB of uImage.
Every trigger comes after the root filesystem is mounted: the sdkinit and
modload replays run from rcS writes to `/proc/odi_init` and
`/proc/odi_omci`, and the GPON init runs once, at the first activation of
a boot (it is loaded in process context before the GPON lock is taken).
Later re-activations take their own path and do not reload it.
`modload.bin` holds exactly what a boot replays: the selection that used
to be a boot-time mask is applied to the file (`replayblob.py filter`,
`docs/SWITCH.md`), and the kernel replays every record.

The format is our own (`odi_replay_blob.h` is the byte-level reference,
`tools/regtrace/replayblob.py` writes and dumps it): a 24-byte big-endian
header (magic, version, table id, header size, record size, count, a
reserved word, and a CRC-32 over the header and every record) followed by
36-byte records, parsed byte by byte. A truncated, corrupt or
mis-generated file is refused whole, never half applied.
`CONFIG_FW_LOADER` is on for this (the Kconfig `select`s it) with plain
filesystem lookup only: no user-mode helper, no compressed firmware, no
firmware cache. `test/odi_replay_blob_test.sh` (in `make test-host`) runs
the C parser and the GPON init replay against the host mock, and has the
Python writer validate and dump each built file, so the writer and the
reader are checked against the same bytes.

### Locking

The switch has one set of shared engines — the indirect table access, the
command state, the multi-register sequences, the I2C master — reached from
netlink (`omcid`), `/dev/odi_sw` (`diag`, `metricsd`), `/proc` writes and
the GPON interrupt path. `odi_switch.c` has the full comment; in short:

- `odi_switch_lock`, a mutex, serialises the table engine and every
  multi-register sequence. Process context only; it may sleep inside
  (the replays call `request_firmware()`).
- `odi_switch_dsf_lock`, an IRQ-safe spinlock, covers the few registers
  the GPON atomic path does touch: the downstream GEM and Alloc-ID CAM
  handshakes, the flow type and slot map, the encryption bits. It is a
  leaf, and nothing is printed under it.
- `odi_i2c_lock`, a mutex, covers the whole I2C byte sequence (setup,
  address, start, poll, read, per byte): the DDM ioctl, the i2c adapter
  (`/dev/i2c-0`), the transceiver command and the sdkinit I2C verbs.
  Without it an `i2cdump` and an exporter DDM poll could interleave and
  read each other's bytes.
- `odi_sfpm_lock` (`odi_sfp_mirror.c`), a mutex, covers the host-side SFP
  EEPROM: its state, its image and the SFP slave registers. It nests outside
  `odi_i2c_lock` (the module reads) and never meets `odi_switch_lock`;
  `odi_sfpm_req_lock` outside it serialises the on and off requests.
- `odi_wdt_flag_lock` serialises the watchdog enable writers, so two can
  no longer each start a kicker thread.

The order is `odi_switch_lock -> odi_i2c_lock`, the i2c core's bus lock
`-> odi_i2c_lock`, `odi_sfpm_req_lock -> odi_sfpm_lock -> odi_i2c_lock`, `odi_switch_lock ->
odi_gpon_lock -> odi_switch_dsf_lock` and, from the hard IRQ and the
GPON timers, `odi_gpon_lock -> odi_switch_dsf_lock`; the host mock
aborts on a recursive acquire or on releasing a lock not held.

### The OMCI netlink socket needs `CAP_NET_ADMIN`

`odi_omci` speaks on its own private netlink protocol number (`NETLINK_ODI`;
the stock firmware used `NETLINK_USERSOCK`), which like any netlink
protocol accepts senders without any privilege. Every message that
registers for a redirect type or sends a driver command is refused unless
the sender has `CAP_NET_ADMIN` (`netlink_capable()`), so only root can
answer the OLT or drive the switch
through it.

### The GPON link counters

The GPON MAC counts what happens on the line in 43 counters, in three
banks: the downstream framer block at `0x701180`-`0x7011c8` (BIP errors,
FEC corrections, PLend, BWmap, PLOAMd and GEM counts), the downstream GEM
counters behind the `DS_GEM_MISC_SEL` selector (Ethernet frames, FCS
errors, OMCI), and the upstream ones behind `USF_MISC_SEL` (bursts, PLOAMu,
DBRu, GEM frames and bytes). Every one **clears when it is read** and stays
at all ones once full: a second read 10 s after a first comes back small
(observed on ISP2), and a counter never read since the block was reset
reads `0xffffffff`. Several registers carry two counts in 16-bit halves
(one in an 8/8/16 split), so a register is read once and split, never read
per field.

A clear-on-read counter is only sound with one reader, so `odi_gpon_ctr.c`
is it. A timer runs it once a second from the boot `gponact` on, under
`odi_gpon_lock`, and it adds each read into a 64-bit total; a read of all
ones adds what it read and counts a saturated sample, since that second
lost counts. The first sample only clears, and is taken in the boot
`gponact` itself, right after the replay starts the block, so nothing
the framer counts is lost to it. One second keeps every 32-bit
count far from full at line rate; a 16-bit upstream GEM frame count fills
above 65535 frames a second, which the saturated count shows. The same
sample records the DS and US FEC state, the superframe count and the time
spent in each ONU state; the transition note counts every ONU state
change by state left and entered. The LOS, LOF and LOM bits of
`DSF_ALARM_STATE` are counted from every reading of the register, the
sample's and each of the line check's (every 100 ms while the ONU is
activated, and at every interrupt): a bit set that was clear at the
previous reading is an event, and the time from one reading to the next
is added to each alarm set at the first. So a loss is timed to the line
poll while it runs, and to the second after a `gpondeact`.

The REI the BER timer sends carries `DSF_BIP_ERR_BLOCKS` since the last
REI, and that register is one of these: the BER timer takes a sample
first and hands over the blocks counted in O5 since its last REI, so the
REI and the totals share the one read. A `diag register get` or `memprobe`
read of any of these registers by hand takes its count away from both.

`/proc/odi_gpon/counters` prints the totals (`docs/TOOLS.md`, "/proc/odi_gpon/")
from a copy taken under the lock; `metricsd` exports them.
`test/odi_gpon_ctr_test.c` runs the sampler against a register model that
clears on read: one read per register per sample, the field split,
accumulation past 2^32, saturation, the REI hand-over, the alarm and state
times across a wrapping clock, and the `/proc` lines against
`test/fixtures/odi_gpon_ctr_counters.txt` and `odi_gpon_ctr_status.txt`.

#### The per-flow GEM counters

`odi_gpon_flow.c` does the same for each GEM flow, as an extra in the same
second and under the same lock (`odi_gpon_ctr_sample_locked()` runs both).
The RTL9602C counts per flow, behind four indirect register pairs (write
the flow index, with the byte or Ethernet selector bit where there is one,
wait for the ACK bit, read the count): downstream GEM frames and bytes,
Ethernet frames received, Ethernet frames forwarded; upstream GEM frames and
Ethernet frames, and the upstream bytes in a 64-bit array. All of them
clear on read, measured on a stick, so the one reader rule holds for them
too, the vendor `gpon show counter flow` included.

Only flows that exist are read, never the 128 rows. `odi_switch_cmd.c` tells
`odi_gpon.c` (`struct odi_switch_flow_ops`) when cmd 25 creates a flow and
when the flow tables are emptied; creating a flow zeroes its totals and
clears its registers with a read nobody keeps, so counts from before it
existed never get in. The index is the driver flow id, not the GEM port id,
and the flow remembers the port it was created with. The ACK wait is a
fixed number of register reads, not a clock (this kernel has no clocksource
to bound one by): a count whose ACK never came is skipped, left in the
register for the next second, and tallied in `flow_ack_timeouts`. The
upstream byte count is read low word first; a second at line rate stays
far below 2^32 bytes, so its high word is normally 0. `/proc/odi_gpon/flows`
prints the totals (`docs/TOOLS.md`, "/proc/odi_gpon/"), and
`test/odi_gpon_flow_test.c` runs the sampler against a register model that
clears on read, withholds ACK on request and aborts on a register outside
the flow counters: a new flow starting from zero, accumulation past 2^32,
only active flows read, a flow created again, the selector bits, the 64-bit
byte count, the bounded ACK, saturation, and the `/proc` lines against
`test/fixtures/odi_gpon_flows.txt`.

### The NIC

`odi_nic.c` is a small driver for one DMA engine, but three of its error
paths were wrong before this release, none seen on a stick: an RX refill
that failed to allocate handed the hardware a descriptor still pointing at
a freed buffer (it now maps the replacement first and, on failure, drops
the frame and recycles the mapped buffer); a full TX ring stopped the
queue with nothing to wake it (reclaim now runs from NAPI on the
TX-completion interrupt, and two delayed works cover an interrupt that
does not come: one every 10 ms while a queue is stopped, one every 100 ms
while frames are in flight; the queue wakes at half ring); and no DMA mapping was checked (`dma_mapping_error()` now
guards all three). The init error path now unwinds in reverse.
With `odi_nic.omci_ring=1` (off by default) the OMCI frames the switch
traps to the CPU have a receive ring of their own, served first, so a
flood from the host side cannot starve them of descriptors.
`kernel/extra/drivers/net/ethernet/odi/README.md` has the rest.

### The L2 table

`odi_switch_l2.c` reads the switch L2 lookup table (1,024 hashed rows, four
ways per bucket, then 64 CAM rows, walked while L2_LOOKUP_SETUP.CAM_OFF is
clear -- it is, on this image) through the shared indirect table engine at 0x012000, one row per
access. A row is 78 bits in three words, read back from TABLE_READ_WORD
0..2 in that order with no reversal; `odi_switch_l2.h` has the field table.

**Validity is the engine answer, not a row bit.** After a by-row read,
TABLE_STATUS (0x012004) has HIT (bit 12) set when the row holds an entry and
clear when it is empty; its low ten bits echo the row read either way. Bit
77, the valid flag a write sends, reads back set on every row. Checked on
ISP1 (image 618p2) with four learned addresses, reading all 1,024 rows and
TABLE_STATUS after each (the MAC addresses are invented documentation
addresses; the row numbers, ports and VIDs are as read):

    row    status      raw (bits 95..64 63..32 31..0)     decodes as
    0x06c  0x0000106c  0x00002038 0x00000200 0x00000001   02:00:00:00:00:01 port 0 age 7
    0x270  0x00001270  0x0000203d 0x000e0200 0x00000002   02:00:00:00:00:02 port 2 VID 14 C-tag
    0x364  0x00001364  0x00002038 0x00000200 0x00000003   02:00:00:00:00:03 port 0 age 7
    0x390  0x00001390  0x0000203e 0x00000200 0x00000004   02:00:00:00:00:04 port 3 age 7
    other  the row     0x00002000 0x00000000 row / 4      nothing: 1,020 rows, HIT clear

HIT was set after exactly those four reads, and on none of the 64 CAM rows
(read the same way: leftover words, bit 77 clear, TABLE_STATUS.IN_CAM set). The 1,020 empty rows all read
0x00002000 0x00000000 and their bucket number (row / 4) in word 0 -- the low
MAC octet -- which suggests the table stores only the part of the MAC its
bucket does not already imply and rebuilds the rest on the way out. A driver
that took bit 77 for validity (618p2 did) lists every row of the table as a
learned `00:00:00:00:00:NN`. The host test (`test/odi_switch_l2_test.c`)
models the table this way and replays the readout above.

**Keys.** All four learned rows are SVL (bit 63 clear) on filtering id 0,
the VID-14-tagged one from the PON included; on a unicast row bits 48..59
hold the VID of the learned frame, not a lookup key. So every VLAN on this
image is shared, and the multicast entries igmpd writes are keyed the same
way: SVL, filtering id 0, static (bit 62), member ports in bits 66..69, and
valid (bit 77) set; a delete sends the key alone (MAC, filtering id, IVL)
with valid clear. Both go through the engine hash (TABLE_CMD method 0) and
report the row from TABLE_STATUS. On ISP1, `igmpd -w -j 239.1.2.3 -p 0x1`
landed on row 0x17c as 0x00002004 0x40000100 0x5e010203, and `-l` returned
that row to empty.

HIT after a hash *write* is not "the key was there": a delete write of a
key never in the table comes back with HIT set and the row the key hashes
to. A hash *read* (method 0, read, the key in TABLE_WRITE_WORD) does
answer it -- HIT and the row for a present key, HIT clear for an absent
one -- so a delete looks the key up first and writes only when it is
found.

## Building it

    make toolchain       # once: pull the pinned gcc/binutils/uClibc-ng image
    make kernel           # kernel/build.sh

`kernel/build.sh` runs in a container, using our own toolchain
(`toolchain/README.md`), with

    -march=mips2 -mno-branch-likely -mdivide-breaks

on top of the kernel's own flags — the same suppressions every other
component in this image needs; `docs/CROSS-COMPILING.md` explains why. The
seed config (`kernel/618/config`) is completed with `olddefconfig`, and the
uImage's entry point is read back out of the build's own `System.map`
(`kernel_entry`) rather than assumed, because it does not sit at the load
address on this kernel line. Output is `build/kernel-618/uImage`, plus
`System.map` and the `.config` actually used — both are picked up
automatically by `image/build.sh`, which ships the `.config` on the device
as `/etc/kernel-config` for reference.

`VERSION=<image version> kernel/build.sh` stamps that version into the
ramlog metadata block (below) as the build id; without it the kernel
takes the same `odi-oss-<date>-<rev>` default `image/build.sh` uses.
`CRUMBS_CORE=1` also applies `kernel/618/debug/` (see "Early crumbs").

## The config

`kernel/618/config` is a fragment, not a full `.config`: `olddefconfig`
completes it. It is sized for a 1328 KB kernel partition (1,359,872 bytes):
`-Os`, dead code elimination, `SLUB_TINY`, no modules, no kallsyms, a 16 KB
printk ring (`LOG_BUF_SHIFT=14`; the ramlog keeps the boot log anyway), IPv4,
bridge and 802.1Q only. Every line carries its reason in a comment there.
The LZMA uImage is 1,172,555 bytes, 187,317 spare (13.8%), with the hardening
options below; the uncompressed kernel is 3,544,984 bytes. `ODI_HWMON` (the die temperature sensor, which
turns `HWMON` back on) added 2,746 bytes to the uImage and 8,488 to the
uncompressed kernel.

**The syscall diet.** Options whose syscalls nothing in the rootfs can
reach are off: `AIO`, `SIGNALFD`, `TIMERFD`, `EVENTFD`, `INOTIFY_USER`,
`FHANDLE`, `ADVISE_SYSCALLS`, `RSEQ`, `MEMBARRIER`,
`CROSS_MEMORY_ATTACH`, `PROC_PAGE_MONITOR`, `COREDUMP`,
`ETHTOOL_NETLINK` and `SWAP`. "Unreachable" was established by scanning
every ELF in the rootfs for the syscall number loaded right before each
`syscall` instruction, using the o32 numbers (4000 + N), and by grepping
the scripts for the tools that would need each one. A few of those
syscalls are compiled into a shipped binary but reached only by a path this
image never runs (`inotify_init1` in `ip netns monitor`, `madvise` in
busybox's yescrypt code, the handle syscalls in iproute2's cgroup2
helpers); the config comments say which.

Deliberately kept on, and why:

- `POSIX_TIMERS`: on 6.18 it also builds `kernel/time/itimer.c`, so with
  it off `alarm` and `setitimer` return `ENOSYS`, and busybox `arping`,
  `ping` and `timeout` (rcS uses `arping`) stop timing out.
- `FILE_LOCKING`: busybox `flock` and iproute2 reach `flock`, and `fcntl`
  record locks cannot be ruled out by a syscall-number scan at all.
- `VM_EVENT_COUNTERS`: the `/proc/vmstat` event counters are the memory
  pressure signal the exporter is meant to read. About 1.4 KB.
- `SYSVIPC`: omcid serves `omcli`/`omcicli` over a SysV message queue.
- `MULTIUSER`: dropbear needs the uid/gid plumbing even for a root-only
  login.

**The hardware and subsystem diet.** A second pass turned off whole
subsystems for hardware the board does not have and interfaces nothing in the
image reaches. It took the uImage from 1,310,849 bytes (96% of the partition,
49,023 spare) to 1,149,701 (15% spare) and the uncompressed kernel from
3,961,476 to 3,460,192 bytes. The loader unpacks the LZMA image before the
kernel runs, so the smaller image is also fewer bytes for it to unpack; the
effect on boot time is measured on a stick, not here. Sizes are the object
code each option built, from `size` over the build's `built-in.a` files (before
the linker drops unreferenced sections), so they add up to more than the
saving:

| Option(s) off | Object code removed | Why nothing needs it |
|---|---|---|
| `VT` (`VT_CONSOLE`, `CONSOLE_TRANSLATIONS`, `DUMMY_CONSOLE`) | 78 KB | the console is the 8250 UART on ttyS0; nothing opens `/dev/tty0..63` |
| `INPUT`, `SERIO` (PS/2 keyboard and mouse) | 108 KB | no input device exists on the board |
| `HID_SUPPORT` (`HID`, `I2C_HID`, `HID_GENERIC`) | 55 KB | no input device exists on the board; `I2C` itself stays for the optics port |
| `MIPS_FP_SUPPORT` (FPU emulator) | 72 KB | the CPU has no FPU and every ELF in the image is soft-float with no coprocessor-1 instruction (readelf and objdump over all 13); `isa-audit` fails the build on floating point anyway |
| `PM`, `SUSPEND` | 56 KB | nothing suspends, no sysfs power user, no driver has power-management callbacks; reboot and restart are not part of it |
| `SECCOMP` | 9 KB | no ELF loads the seccomp syscall or carries the string |
| `PTP_1588_CLOCK`, `PPS` (and `NET_PTP_CLASSIFY` with them) | 23 KB | no driver registers a clock; nothing reads `/dev/ptp*` |
| `NVMEM`, `HW_RANDOM` | not remeasured (25 KB together with `HWMON`) | no provider or hardware generator registers with them; `HWMON` was in this row until `ODI_HWMON` began to select it ("Our drivers") |
| `MQ_IOSCHED_DEADLINE`, `MQ_IOSCHED_KYBER`, `MSDOS_PARTITION`, `EFI_PARTITION`, `BLOCK_LEGACY_AUTOLOAD` | 21 KB | mtdblock backs the squashfs root, so there is nothing to reorder and no partition table to read |
| `EPOLL`, `DNOTIFY` (and `FSNOTIFY` with it), `SGETMASK_SYSCALL`, `CACHESTAT_SYSCALL` | 32 KB | no ELF loads these syscall numbers, and none carries an `epoll_` symbol |
| `LEGACY_TIOCSTI`, `LDISC_AUTOLOAD` | a few KB | no tool injects tty input; the only line discipline in use, N_TTY, is built in |
| `BQL`, `NET_RX_BUSY_POLL` | a few KB | nothing reads the byte-queue-limit knobs or sets `SO_BUSY_POLL` |
| `SQUASHFS_ZLIB`, `XZ_DEC_*` BCJ filters, `WLAN`, `WIRELESS` | 1.5 KB of uImage | the rootfs is xz with no BCJ filter (the squashfs superblock has no compressor options); no wireless driver exists |

Every claim in the table was checked against the rootfs of the image built
from it: the ELF scan (soft-float, no coprocessor-1 instruction, none of the
syscall numbers above) and `grep` over the scripts, `src/` and the sibling
`confd` and `metricsd` sources for the `/proc`, `/sys` and `/dev` paths they
open. `qemu` runs a different kernel, so `make test` cannot catch a config
regression here; a stick has to.

What was measured and deliberately left on, with the saving in uImage bytes
had it been turned off (each on top of the trimmed config):

- `STACKPROTECTOR_STRONG`: 25,060 bytes. It is a security hardening against a
  stack overflow in network-reachable kernel code; it is one of the options
  "Hardening" (below) requires.
- `BRIDGE_IGMP_SNOOPING`, `COMPACTION` and `FUTEX_PI` together: 27,747 bytes.
  Bridge snooping changes how the kernel bridge forwards multicast it sees on
  the CPU port, and the sources do not show whether IGMP copied up for igmpd
  ever passes through it; `COMPACTION` is what keeps larger allocations
  possible after a long uptime on 32 MB (not measured either way); a PI
  futex in a threaded daemon cannot be ruled out by a syscall scan (the
  priority-inheritance command is an argument, not a syscall number).
- `LEGACY_PTYS`: rcS makes the BSD pty nodes and a test checks the count, so
  it stays; dropbear uses `/dev/ptmx`.
- Not configurable in 6.18: `net/ipv4/nexthop.c` (28 KB of object code) and
  the rest of the IPv4 core are built unconditionally with `INET`; dropping
  them would need a patch to mainline files, which this tree avoids carrying.

**Debug aids that stay on.** `print-fatal-signals=1` is on the built-in
command line: a process killed by SIGSEGV, SIGBUS or SIGILL prints its
registers (`epc`, `ra`, `Status`, `Cause`, `BadVA`) to the console, and so
to the ramlog, where a dying init otherwise leaves only "Attempted to kill
init!". The hung-task detector (30 s) and the soft-lockup detector are on
for the same reason, and so are the early crumbs (`CONFIG_ODI_EARLY_CRUMBS`,
below).

**The command line.** `CONFIG_CMDLINE` is only the slot-0 fallback
(`root=31:5` and the `mtdparts=` table). With
`MIPS_CMDLINE_BUILTIN_EXTEND` it goes first and U-Boot's per-slot
arguments after it, so the bootloader's `root=` and `mtdparts=` win (the
last occurrence of each is the one used); the default ordering made the
slot-0 `root=` win on a slot-1 boot.

### Hardening

Every kernel hardening option this architecture and toolchain support was
considered; the ones below marked "on" are set in the seed config, with
their reasons, in its "Hardening" section. `tools/check-kernel-hardening.sh`
holds the list: `make lint` fails when a line of it is missing from the
seed, and `kernel/build.sh` runs the same check on the completed `.config`
right after `olddefconfig`, before compiling, because `olddefconfig` turns an
option off without a word when one of its dependencies goes away. The same
script checks that the hardening sysctls are on the built-in command line.

Sizes are uImage bytes (LZMA, what has to fit the 1,359,872-byte kernel
partition), each option built alone on top of the config of the time
(uImage 1,163,145 bytes), with the build `kernel/build.sh` runs. A few tens of
bytes either way is LZMA noise: where the uncompressed kernel came out
byte-for-byte the same size, the table says 0. Together, the options turned
on here took the uImage to 1,172,555 bytes (9,410 more, less than the sum:
LZMA compresses the checks together), 187,317 spare (13.8%).

| Option | On this arch and toolchain | uImage bytes | Run-time cost | Here |
|---|---|---|---|---|
| `STACKPROTECTOR_STRONG` | yes: MIPS selects `HAVE_STACKPROTECTOR`; the canary is per task, drawn at fork, and the context switch copies it into the global `__stack_chk_guard` (`r2300_switch.S`, `!SMP` only), so it works on this uniprocessor | 25,060 against none (4,416 of it the plain stack protector) | a load and compare in every function with an array or address-taken local | on (already by default) |
| `INIT_STACK_ALL_ZERO` | yes: gcc 16 has `-ftrivial-auto-var-init=zero` | 7,686 | a store per uninitialised local | on (already by default) |
| `FORTIFY_SOURCE` | yes: MIPS selects `ARCH_HAS_FORTIFY_SOURCE` | 4,940 | a size compare on `str*`/`mem*` calls whose object size is known; nothing on the others | on |
| `HARDENED_USERCOPY` | yes: no architecture dependency; works with `SLUB_TINY` | 2,127 | a few compares per `copy_to_user`/`copy_from_user` | on |
| `INIT_ON_ALLOC_DEFAULT_ON`, `INIT_ON_FREE_DEFAULT_ON` | yes | 34 (a static key) | a `memset` of every page and slab object on free (and on allocation, for memory not zeroed at free). Only management traffic (ssh, the web UI, the exporter, OMCI) passes through the CPU; the switch forwards the subscriber datapath in hardware. Not measured on a stick yet; `init_on_alloc=0` / `init_on_free=0` on the command line turn either off without a rebuild | on |
| `LIST_HARDENED`, `BUG_ON_DATA_CORRUPTION` | yes | 2,885, and 0 more for the second | pointer compares on every list add and delete | on |
| `SCHED_STACK_END_CHECK` | yes: its `DEBUG_KERNEL` dependency is on already (`EXPERT` selects it) | 93 | one compare per `schedule()` | on |
| `SECURITY_DMESG_RESTRICT` | yes | 0 | none | on (`kernel.dmesg_restrict=1`) |
| `COMPAT_BRK` off, `ARCH_MMAP_RND_BITS=15`, `DEFAULT_MMAP_MIN_ADDR=65536` | yes: 15 is the 32-bit MIPS maximum; every ELF in the rootfs is a static executable linked at `0x400000` (readelf over all 13) | 0 | none; the user heap start is randomised too (`randomize_va_space` 2) and the mmap base over 128 MB instead of 1 MB | on |
| `kernel.kptr_restrict=2`, `fs.protected_symlinks`, `_hardlinks`, `_fifos`, `_regular` = 1 | yes | the command-line string only | none | on, as `sysctl.*` words on the built-in command line, which the kernel applies before init on every boot (below) |
| `ZERO_CALL_USED_REGS` | yes: gcc 16 has `-fzero-call-used-regs=used-gpr` | 18,774 (the uncompressed kernel 6.5% larger) | extra instructions at every function return, on a core with small caches | off: 6.5% more kernel text to fetch through a small instruction cache is a cost to measure on a stick first; it removes return-oriented gadgets in a kernel whose addresses are public anyway (below) |
| `SLAB_FREELIST_RANDOM`, `SLAB_FREELIST_HARDENED`, `SLAB_BUCKETS` | only without `SLUB_TINY`: each `depends on !SLUB_TINY` | 9,782 for dropping `SLUB_TINY` alone, 11,214 with these three and `SHUFFLE_PAGE_ALLOCATOR` | dropping `SLUB_TINY` gives up the smallest slab footprint: larger slab orders, per-CPU slabs and separate reclaimable caches, on a 32 MB board that once ran short of slab | off: the memory it costs has to be measured on a stick (`Slab`, `MemAvailable`) before the uImage bytes are worth it |
| `SHUFFLE_PAGE_ALLOCATOR` | yes | 9,950 | it shuffles free memory in blocks of the largest page order, 4 MB here (`ARCH_FORCE_MAX_ORDER=10`), so a 32 MB board has only a handful of blocks to shuffle, and only with `page_alloc.shuffle=1` on the command line | off: next to nothing randomised for its size |
| `UBSAN_BOUNDS` with `UBSAN_TRAP` | no: the trap is gcc's `__builtin_trap`, which at `-march=mips2` is `teq`/`teqi`, a MIPS II trap this core does not implement; the build had 1,102 of them (the release kernel has 0) | 8,402 | | off; without `UBSAN_TRAP` it only logs, for more bytes |
| `PANIC_ON_OOPS` | yes | 0 | an oops anywhere reboots the stick (`panic=10`) instead of leaving a half-working kernel | off, a policy choice: the link goes down for every oops |
| gcc plugins (`GCC_PLUGIN_LATENT_ENTROPY`, `RANDSTRUCT`) | no: the toolchain image ships gcc plugin headers but not the GMP headers they include, so no plugin builds (`gmp.h: No such file or directory`) | | | off. A random struct layout would also have to be a fixed, published seed for the release build to stay reproducible, which leaves nothing secret |
| `STRICT_KERNEL_RWX`, `STRICT_MODULE_RWX`, `DEBUG_WX` | no: MIPS selects neither `ARCH_HAS_STRICT_KERNEL_RWX` nor `ARCH_HAS_DEBUG_WX`, and the kernel runs from KSEG0, which the MMU does not map, so there are no page permissions to set; modules are off anyway | | | not available |
| `VMAP_STACK`, `RANDOMIZE_KSTACK_OFFSET`, `KSTACK_ERASE` | no: MIPS selects none of `HAVE_ARCH_VMAP_STACK`, `HAVE_ARCH_RANDOMIZE_KSTACK_OFFSET`, `HAVE_ARCH_KSTACK_ERASE` | | | not available |
| `RELOCATABLE`, `RANDOMIZE_BASE` (KASLR) | no: `RELOCATABLE` depends on `SYS_SUPPORTS_RELOCATABLE` and on a MIPS32/64 R2 or later CPU; this board is `CPU_R3000` and selects neither | | | not available |
| `STRICT_DEVMEM` | no: MIPS has no `devmem_is_allowed()` (`ARCH_HAS_DEVMEM_IS_ALLOWED`) | | | not available; `/dev/mem` stays for root, as `tools/memprobe` and `tools/regdump` read registers through it |

Already off, and now checked to stay off, because each widens what a local
process can reach in the kernel: `MODULES`, `IO_URING`, `INET_DIAG`,
`KALLSYMS`, `BPF_SYSCALL` (so `kernel.unprivileged_bpf_disabled` does not
exist), `PERF_EVENTS` (likewise `kernel.perf_event_paranoid`), `USERFAULTFD`,
`KEXEC`, `MAGIC_SYSRQ`, `PROC_KCORE`, `DEBUG_FS`, `LEGACY_TIOCSTI` and
`LDISC_AUTOLOAD`. No security module (`SECURITY`, so no Yama ptrace scope) is
built: every process runs as root, where none of them would restrict
anything.

**The sysctls are on the command line, not in a script.** Since 5.8 the
kernel applies `sysctl.<name>=<value>` words of its command line itself,
after the initcalls and before it starts init (`do_sysctl_args()`), and a
word with a dot in it is never passed on to init. The built-in line always
goes first (`MIPS_CMDLINE_BUILTIN_EXTEND`, above), so they hold on every boot
whatever U-Boot passes, from before the first process runs, with no file in
the rootfs and no step in rcS. `kptr_restrict=2` prints every `%pK` pointer
(the socket addresses in `/proc/net`, for one) as zeros, root included. The
four `fs.protected_*` refuse to follow a symlink, make a hard link, or open a
FIFO or regular file for writing in a sticky world-writable directory such as
`/tmp` when it belongs to another user. With everything running as root those
four and `dmesg_restrict` change nothing today; they start to matter as soon as a daemon runs as another user.

**What no option fixes on this board.**

- No kernel address randomisation, no read-only kernel text and no guard
  pages around kernel stacks (the rows marked "not available"), and the
  release image is public and reproducible: anyone can rebuild it and read
  every kernel address. Hardening here is about stopping a memory error from
  becoming a write (stack protector, `FORTIFY_SOURCE`, user-copy checks, list
  checks, zeroed memory), not about hiding where things are.
- The stack canary of the boot task, and so of the idle task, is drawn early
  in `start_kernel()`, before any interrupt has fed the entropy pool. On this
  core `random_get_entropy()` has no cycle counter (`cpu_has_counter` is 0);
  it falls back to the clocksource, still jiffies that early, and six bits of
  the TLB `Random` register. Tasks forked later draw from a pool that
  interrupts have fed, and rcS credits the seed saved by the previous boot
  (`seedrng`). The board has no hardware random generator to do better, and
  the latent-entropy plugin, made for exactly this, cannot be built (above).
- `/dev/mem` stays readable and writable by root (no `STRICT_DEVMEM` on MIPS),
  which on this board also means every SoC register.

`qemu` runs a stock malta kernel, so none of this is exercised by
`make test`; a stick has to: the boot log line `mem auto-init: stack:all(zero),
heap alloc:on, heap free:on`, `cat /proc/sys/kernel/kptr_restrict` (2),
`/proc/sys/kernel/dmesg_restrict` (1), `/proc/sys/fs/protected_regular` (1),
`/proc/sys/vm/mmap_min_addr` (65536), `/proc/sys/kernel/randomize_va_space`
(2), and no `Failed to set sysctl parameter` line in `dmesg`.

## The boot command line and the watchdog

U-Boot passes a per-slot command line in `argv`: which root device
(`root=31:5` for slot 0, `root=31:7` for slot 1), and the rest of what
`prom_init` needs. Nothing in the kernel hardcodes a slot.

U-Boot arms the hardware watchdog before handing over to a **trial** boot
(`nv setenv sw_tryactive <slot>`, see `docs/FLASHING.md`); the kernel's job
is to keep kicking it. A committed boot (`boot_by_commit`) gets no `en_wdt`:
the watchdog is off until `odi_wdt` arms it, and the driver logs what it
found (`found ctrl 0x00000000` on a committed boot, `0xe7c00000` on a trial),
so a committed image that hangs before that point is not reset. `odi_wdt.c` is a small, from-scratch kicker thread:
it arms the watchdog itself at init, on every boot, at the same operating
point U-Boot's own `en_wdt` sets, and — critically — it also tracks a
**userland confirmation** deadline, not just "the kernel is alive." Unless
something writes `1` to `/proc/odi_wdt/userland_ok` within 120 s of
uptime, the kernel forces the reset itself. rcS writes it once its own
steps have run (the network is not a condition; with the development
flag `/etc/config/confirm-arp` it waits for an ARP reply from the address
the file holds instead), so a kernel that boots fine but
whose init scripts hang still gets reset. Before that
forced reset, the watchdog calls `wdt_pre_reset_hook`, which `odi_nic.c`
sets to its own quiesce function, so the NIC's DMA engine is stopped the
same way an orderly shutdown would stop it. The proc directory keeps the
stock firmware's name so rcS works on both.

### Reboot, halt and power-off

The watchdog is the only reset this SoC has, so `odi_wdt.c` also registers
the kernel's restart handler (`register_restart_handler`, priority 128, the
level for a handler that restarts the whole system). The board file sets no
`_machine_restart` or `_machine_halt` hook, so mainline `machine_restart()`
runs that handler once the reboot notifiers and device shutdown are done, or
straight away on an emergency restart (panic reboot, sysrq). The handler
masks interrupts, calls `wdt_pre_reset_hook` (the NIC DMA stops; the NIC
driver reboot notifier has already done that on an orderly reboot, but an
emergency restart skips the notifiers), writes the watchdog control
register with ENABLE alone (`TIMEOUT1=0`, the shortest timeout) and kicks,
the same sequence the userland deadline uses. The kick matters: the
timeout fields do not restart the count. Measured on the stick, the control
write alone reset the board about 1.05 s later, and the write followed by a
kick about 0.33 s later. If the board were still running 3 s later, the
handler re-arms the watchdog at its normal operating point and returns;
`machine_restart()` then hangs with interrupts masked and the reset comes
one window later.

Before this, `reboot` ended in a board hook that spun with interrupts off
until the watchdog window ran out: about 42 s of a dead stick on every
reboot, on top of U-Boot and the next boot.

Halt and power-off have nothing to act on: the stick has no power switch,
and a halted stick in an SFP cage is useful only once it resets. A reboot
notifier re-arms the watchdog at its normal operating point on both (even
if `watchdog_flag` had turned it off), and mainline `machine_hang()` then
spins with interrupts masked, so nothing kicks it: the board resets about
42 s later, and a trial falls back to the committed slot as on any hang.
The NIC reboot notifier stops its DMA on these paths too.

## The DRAM ramlog console

This device has no serial console, so a boot that never comes up on the
network leaves nothing to look at — unless something outside the network
stack recorded it. `odi_ramlog.c` (`CONFIG_ODI_RAMLOG`) is a `struct console`
driver that mirrors every kernel console line into two fixed physical DRAM
pages (`0x017ff000`, the first 4080 bytes; `0x01fff000`, a ring of the last
4080) that neither this kernel nor the stock kernel maps, and that survive a
watchdog reset — though not a power cycle. After a trial reverts, boot the
*other* (working) image and read those pages back with `tools/memprobe`
(`tools/memprobe/README.md`) to see exactly how far the failed trial got,
including a stamp confirming U-Boot actually jumped to the new kernel at
all. `docs/FLASHING.md` has the full read-back procedure.

### The previous boot, from our own image

Once both slots run this image, the boot that follows a failed trial is
ours too, and its ramlog would overwrite the failed log at startup. So
before the driver writes anything, it copies both pages (8 KB) into a
static buffer, and the copy stays readable for the whole boot:

    cat /proc/odi_ramlog_prev          # decoded, root only
    cat /proc/odi_ramlog_prev_raw > p  # 8192 bytes: page A, then page B

The decoded file starts with two lines, then the page text the same way
`tools/memprobe/ramlog-read.sh` prints it:

    this boot: boot=8 slot=0
    previous boot: boot=7 slot=1 build=odi-oss-260924-618k1 crumb=TICK/51234 reason=reboot
    ---- page A: first 3984 bytes ----
    ...
    ---- page B: last 4080 of 51234 bytes ----
    ...

It reaches one boot back, and a power cycle still wipes both pages. A
boot of the stock image in between writes nothing, so the "previous boot"
is then the last boot of ours before it; the boot counter and build id
say which one it was.

### The page A boot metadata block

The last 64 bytes of page A (`+4032`) hold one block per boot, written
right after the page headers (`odi_ramlog.h` has the layout):

| offset | field |
|---|---|
| `+0` | magic `RLGM` |
| `+4` | boot counter: the previous value + 1 when the magic was valid, else 1. It counts boots of this image since the last power cycle |
| `+8` | slot, 0 or 1, from the last `root=` on the command line (`31:5` is slot 0, `31:7` slot 1); `0xffffffff` when neither |
| `+12` | block format: 2 (1 before the reset reason block, below) |
| `+16` | build id, 40 bytes NUL padded: `ODI_BUILD_ID` from `kernel/build.sh`, which is the image `VERSION` when one is set for the kernel build, else the `odi-oss-<date>-<rev>` default `image/build.sh` uses |
| `+56`, `+60` | the crumb stash (below) |

Page A text now stops at 3984 bytes, so neither block is ever
overwritten. Readers that take `min(count, 4080)` bytes still read exactly
the text; a page written by an older image (text to 4080 bytes) has no
`RLGM` magic, and one written by a format 1 image (text to 4016 bytes, no
reason block) has format 1.

### The reset reason block

The 32 bytes before the metadata block (`+4000`) record why the boot
ended, so the next one does not have to guess from the free text at the
end of page B. The metadata block keeps its offset, so its crumb stash is
still where `kernel_entry_setup` writes it and a format 1 kernel still
counts boots across ours.

| offset | field |
|---|---|
| `+0` | magic `RLGR` |
| `+4` | reason code, `ODI_RAMLOG_REASON_*` in `odi_ramlog.h` |
| `+8` | detail, 16 bytes NUL padded: the client name for a client miss |
| `+24` | reserved, zero |

`odi_ramlog_meta_stamp()` writes it empty at boot. From then on the last
writer wins: `odi_wdt.c` records the rule that fired (`wdt_client`,
`wdt_mem`, `wdt_cpu_rx`, `wdt_userland`) just before it forces the reset, and
`odi_ramlog.c` registers, at `early_initcall`, a reboot notifier
(`reboot`, `halt`, `poweroff`; the reboot syscall path, which an emergency
restart skips), a panic notifier at the highest priority (`panic`) and a
die notifier (`oops`, kernel-mode only: `do_be()` in
`arch/mips/kernel/traps.c` sends every bus error, a user one too, down the
same chain). The next boot renders it as `reason=` at the end of the
`previous boot:` line: the recorded name, `power` when neither page magic
survived, and `unknown` when the pages survived but nothing wrote a
reason. The 6.18 kernel is `ARCH=mips`; none of `kernel/618/patches`
touches `traps.c`, `kernel/panic.c` or `kernel/reboot.c`, so these are the
mainline chains.
Build the kernel with the image version to have them match:
`VERSION=odi-oss-260924-618k1 kernel/build.sh`.

## Early crumbs

For a kernel that dies before its first console line reaches the ramlog,
`CONFIG_ODI_EARLY_CRUMBS` (on in the seed config) stores a 4-byte ASCII
tag and a step number in ramlog page B at `+8`/`+12` as boot passes each
board hook. The last pair written is how far the boot got;
`tools/memprobe/ramlog-read.sh` prints it. The trail, all in our own files:

| tag | step | where |
|---|---|---|
| `K1EN` | 1 | first instructions of `kernel_entry` (`kernel_entry_setup`) |
| `K2SU` | 2 | the CCTL/TCM/cache entry sequence done |
| `K8PB` | 8 | `prom_init()`, argv copied |
| `K9PA` | 9 | end of `prom_init()`; the ramlog console is live from here |
| `KAMS` / `KBCI` | 10 / 11 | `plat_mem_setup()` entered / caches installed |
| `KCIR` / `KDIR` | 12 / 13 | `arch_init_irq()` entered / done |
| `KETM` / `KFTM` | 14 / 15 | `plat_time_init()` entered / done |
| `KGLT` | 16 | `late_time_init`: the one-shot `odi_irqdbg:` dump to the ramlog |
| `IRQE` | count | every interrupt, in a stub ahead of `handle_int` |
| `IRQD` | count | the board interrupt controller dispatch |
| `TICK` | count | the TIMER0 interrupt |

`K1EN` overwrites the pair the previous boot left, so `kernel_entry_setup`
first moves that pair to the crumb stash at page A `+4088`/`+4092`
(`odi_crumb_stash`, eight uncached loads and stores), and the previous-boot
copy puts it back into its saved page B header: `/proc/odi_ramlog_prev`
shows the previous boot's last crumb. Read from the stock image,
`ramlog-read.sh` prints the stash as the crumb of the boot before the one
in page B.

On a healthy boot the pair read back is `TICK` with a large count. From
`K9PA` on, the ramlog text says more than the crumb. The stretch between
`K2SU` and `K8PB` (the end of `head.S`, `start_kernel`, `cpu_probe`) has no
board hook; `CRUMBS_CORE=1 kernel/build.sh` applies
`kernel/618/debug/0001-core-crumbs.patch`, which adds `K3BS`, `K4JS`,
`K5SK`, `K6CB`/`K7CA` and the `KR*` probe steps there. It edits mainline
files and is not part of the normal build.

## Verifying a build

`image/build.sh` disassembles every ELF in the rootfs and fails on an
instruction the RLX5281 does not implement (`packages/isa-audit.sh`,
`packages/isa-allowlist.sh`, see `docs/CROSS-COMPILING.md`). The kernel is
not in the rootfs, so check it by hand after a kernel change:

    packages/isa-audit.sh build/kernel-618/vmlinux    # must find nothing
    tools/kernel-footprint.sh                         # 19 lines, 6 mainline files today

and, with the toolchain's `mips-linux-uclibc-objdump -d` on that
`vmlinux`, count `eret` and `tne`: both must be 0 (the first proves the
R3000 exception model, the second a trap-free `BUG_ON()`).

`kernel/build.sh` ends with `BUILD OK: <n> warnings`. The release build
prints 22, all GCC 16's "'retain' attribute ignored" in mainline
networking files (`net/core/filter.c`, `net/core/xdp.c`,
`net/ipv4/tcp_cong.c`, `net/ipv4/tcp_cubic.c`): a toolchain and mainline
mismatch, not ours. Any other warning is from our files and is a
regression.

`make test-host` (no stick, no kernel build) covers the switch, GPON,
OMCI, NIC, watchdog, ramlog and replay-table logic that has no kernel
dependency, built and run natively against the same sources the kernel
build uses.
