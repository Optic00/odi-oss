# The boot, stage by stage

What `/etc/init.d/rcS` (and, after it, `rcS.pon`) does and why each stage
sits where it does. The scripts themselves say what each block does; this
is the reasoning behind the order, for someone about to change it.
`docs/TOOLS.md` ("How the image starts things") is the short list;
`docs/HACKING.md` has the recovery mechanism the boot confirms to.

rcS is the `/etc/inittab` `sysinit` entry; `rcS.pon` (the PON steps, stages
6 onward below) is a `once` entry started right after rcS returns, at the
same moment init starts the daemon `respawn` entries (syslogd, klogd, ntpd,
metricsd, confd, dropbear, omcid) -- init does not wait for a `once` entry,
so `rcS.pon` runs concurrently with all of them coming up. `rcs-lib.sh` holds
the functions both scripts share (`config_mounted`, `crumb`,
`confirm_watchdog`, the PON step helpers) so they never drift into two
copies of the same logic.

A second `once` entry, `/etc/scripts/slot-state.sh`, starts at the same
moment. It is not a boot stage: it reads the U-Boot environment (never
writes it), records the running slot and whether it is committed in
`/var/run/odi-slot`, and puts a notice in `/var/run/motd` (`/etc/motd` is a
link to it) when it is not (`docs/FLASHING.md`, "Committing"). The running
slot is the last `root=31:N` on the kernel command line. After that first
write it runs a partial check of the other slot (`nv slotcheck`, up to 1.3 MB
of flash read) and writes the state file again with the result, adding an
`OTHER SLOT BROKEN` notice when that slot would not boot as it is
(`docs/FLASHING.md`, "Checking the other slot"). Nothing in the boot
waits for it, and it is safe to run again by hand: the web UI runs it after a
commit to refresh the notice, and a run after the first of a boot does not
wait for syslogd.

## The rules every stage follows

**Nothing may block.** `/etc/inittab` runs rcS as `sysinit`, and busybox
init starts no `once` or `respawn` entry until sysinit returns. The serial
login and the daemons are all `respawn` entries: anything that blocks
in rcS costs every way back into the device at once. So every slow step
runs in the background, and every read that could stall (diag, arping,
anything over the network) runs under `timeout`. `rcS.pon`, once started,
can take its time -- a slow or stuck PON step no longer costs ssh, the web
UI or the exporter their turn to start, since those are independent
respawn entries by the time it runs.

**Nothing may abort.** rcS runs under `set -e`, which is what stops a
typo from going unnoticed, but an aborted sysinit loses the same things a
blocked one does. Every command that can fail is guarded (`|| echo "rcS:
..." >&2`, or an `if`) and the boot carries on with the damage
proportional: a failed `/var` mount costs the logs, not the shells. Beware
of `[ test ] && command` as the last line of a function: when the test is
false the function returns non-zero and `set -e` stops the boot there.

**Every stage is on record.** `crumb` writes one line per stage to the
kernel log (`rcS: ...`), which the DRAM ramlog keeps across a watchdog
reset: a trial that never answered still says how far it got
(`/proc/odi_ramlog_prev` on the next boot of this image). With
`breadcrumbs.on` the same lines go to flash too (below).

## The stages

1. **Mounts**, declaratively, from `/etc/fstab`: `mount -a`, called TWICE
   (the five `/var` subdirectories are created between the two calls --
   `mount` never creates a missing mountpoint, and `/var/tmp` and
   `/var/config` do not exist until the bare `/var` tmpfs line above them
   has already landed). `/proc`, `/sys` and `/var` -- tmpfs, capped at
   6 MB -- come in the first pass; if the tmpfs mount failed, `/var` is the
   read-only squashfs and the fallback is whatever `mount -a`'s own exit
   status says. `/var/tmp` (`/tmp` is a symlink to it) gets its OWN tmpfs in
   the second pass, capped separately at 8 MB -- the one place scp/sftp and
   a firmware upload's staging area write, and the place
   `docs/SETTINGS.md` ("Resilience") explains sizing for -- and `devpts` and
   the config partition (below) land in the SAME two passes, `devpts` in
   the first (its mountpoint, `/dev/pts`, is created unconditionally just
   before `mount -a` runs, since `/dev` is devtmpfs and already exists) and
   `mtd:config` in the second. Neither tmpfs cap reserves memory up front;
   both just turn "eats all of RAM" into "gives ENOSPC". `vm.min_free_kbytes` is
   raised to 1536 here too (`docs/SETTINGS.md` has the reasoning).
   Right after the second `mount -a`, `required_mounts_ok()` greps the live
   `/proc/mounts` for `/var` and `/var/tmp` both actually being tmpfs --
   the check that would have caught v1.0.2 through v1.0.4-rc1's root
   cause (`kernel/618/config` had neither `CONFIG_SHMEM` nor
   `CONFIG_TMPFS`, so a capped `size=` mount is rejected with EINVAL and
   falls back silently to a ramfs-backed stub, leaving `/var` whatever
   squashfs shipped, read-only) -- logged to kmsg the moment either is
   missing, and consulted again by the watchdog confirmation (stage 11).
2. **The config partition**: mounted by the `mount -a` above
   (`mtd:config /var/config jffs2`, `/etc/fstab`) -- the jffs2 partition
   shared with the stock slot, found by MTD partition NAME, no
   `/dev/mtdblockN` node and no `/proc/mtd` lookup needed. `/etc/config`
   links to `/var/config`; host keys, the UI credentials and every switch
   file live there. `/etc/scripts/mount-config.sh` (run under `sh -x` into
   `/tmp/mount-config.trace`) only confirms that symlink resolves -- the
   one thing that is not a mount option. `config_mounted()` (in rcS) is
   what anything else asks: the live mount table (a real jffs2 mount at
   `/var/config`) plus that same symlink, checked fresh on every call,
   never a flag written once and trusted afterwards. A failure is said in
   the kernel log at once: everything that follows reads its switches from
   here, and a switch file that cannot be read is a switch that silently
   turns itself off. `rcS`'s watchdog confirmation (stage 11) refuses to
   confirm at all without this, except on a safe-mode boot.
   Right after it, a store that is missing is recreated
   (`/etc/scripts/config-reseed.sh run`, `docs/FLASHING.md`, "An erased
   config partition"): `lastgood.xml` or `lastgood_hs.xml`, missing or
   empty, is installed from `/etc/config-template/` with the line identity
   empty, before anything reads the store. With `GPON_SN` empty the
   `gponsn` and `gponact` steps are skipped and the PON stays down.
   Then the boot counters (`/etc/scripts/safe-mode.sh count`,
   `docs/SETTINGS.md`, "Recovery: the safe mode"): as early as the counter
   can be written, so a short power-up still counts. It logs the uptime
   of the count, and after enough short boots in a row, or three boots in a
   row that ended in a watchdog reset (the previous boot's `reason=` from
   `/proc/odi_ramlog_prev`), it makes this boot a safe-mode boot
   (`/var/run/safe-mode`): the PON steps then leave the laser off and skip
   `gponact`, the host port runs auto-negotiation, and the web UI and root
   accept the default password, for this boot only. With the config
   partition not mounted rcS calls `safe-mode.sh no-config` instead, which
   makes this boot a safe-mode boot at once (no record: nowhere to write
   it). The safe-mode boot confirms the watchdog in stage 11 without the
   mount and address checks, so it stays up; it is followed by a normal
   boot, never another safe one, so the state machine cannot loop
   (`docs/SETTINGS.md` has it whole).
   Then `identity.sh snapshot` (`docs/SETTINGS.md`, "The identity
   snapshot"): on the first boot of an image that has it, a copy of the
   provisioning identity to `/etc/config/identity.orig`; on every later
   boot, and on a safe-mode boot, nothing.
3. **Entropy.** Nothing on this board feeds the pool early, so without a
   seed every daemon starts on a nearly empty one (dropbear once logged 27
   bits available). `seedrng` credits the seed the previous boot saved
   under `/etc/config/seedrng` and saves a fresh one. At this point the pool
   is not initialised, so that seed cannot be credited; the second run, 90
   s in, saves one the next boot can credit. seedrng reads with
   `getrandom()`; it never reads `/dev/random`.
4. **Hostname and loopback.** Guarded like the rest: `sethostname` needs a
   privilege the chroot test harness does not have.
5. **The SDK-init verbs** through `/proc/odi_init`, `intr` to `ponmac`, in
   the order the stock firmware issues them. Before the network, because
   without them no frame passes between the CPU port and the host port,
   and because `network.sh` needs a working `diag` for its SerDes check.
   Each verb has a crumb before and after it, so one that hangs names
   itself. The kernel loads the replay behind each verb from
   `/lib/firmware/odi/sdkinit.bin` and releases it again.
6. **The management address** (`/etc/scripts/network.sh`), after the
   config partition, which holds the address, and before the services:
   dropbear bound on a box with no routable interface looks exactly like a
   panic from outside. Its first step is the host SerDes mode
   (`host-serdes.sh boot`, `HOST_SERDES`, `docs/SETTINGS.md`): Fiber 1G
   unless the key says otherwise, nothing written when the registers
   already hold the mode, and in 2500BASE-X a boot watch left running in
   the background. Then the host-port auto-negotiation
   (`/etc/scripts/host-autoneg.sh boot`), after `network.sh` has set the
   SerDes mode it changes one bit of: with `HOST_AUTONEG` unset it reads
   the key and returns; with it off it forces 1000BASE-X and leaves the boot watch running in the background
   (`docs/SETTINGS.md`, "Host port auto-negotiation"). It returns at once
   either way.
7. **ptys.** devpts is already mounted on `/dev/pts` by stage 1's
   `mount -a`, for the `/dev/ptmx` dropbear opens for an interactive
   session; the BSD pairs (`/dev/ptypN`/`/dev/ttypN`) made here are a
   fallback it can also scan. devtmpfs normally provides them; rcS makes
   sixteen pairs if `/dev/ptyp0` is missing.
8. **The daemons are not started here any more.** syslogd, klogd, ntpd,
   metricsd, confd, dropbear and omcid are each their own `respawn` entry
   in `/etc/inittab`
   (`/etc/scripts/svc-*.sh`, `docs/SETTINGS.md`, "native over hand-rolled"):
   busybox init starts all of them the moment this script returns and
   restarts whichever one exits, forever, on its own, with no chance of
   losing track of the pid the way the old shell-loop `supervise()` once
   did on hardware. A daemon that dies within 30 s of its start is
   restarted after a growing wait (1 s, 2 s ... up to a cap per daemon,
   `docs/SETTINGS.md`, "Restart budget"); the first start of the boot never
   waits, and none of it runs before rcS has returned. Each script sets its
   own `oom_score_adj` right before it execs the daemon in place
   (`/etc/scripts/respawn.sh`), with its output appended to
   `/var/log/services.log` (`/var/log/omcid.log` for omcid) -- a file
   rather than the console, because a trial has no console, and rather
   than a pipe, because a pipe the daemons inherit is one nothing ever
   closes.
9. **The log trim**, a background loop: `/var` is tmpfs now, capped, but
   still not evicted on its own, and omcid logs every OMCI frame (about
   1.7 MB a day on a busy OLT). Past 256 KB each log is cut back to its
   last 128 KB; the writers append, so they carry on at the new end. The
   copy for the cut is written to `/var/tmp`, a tmpfs of its own, so a
   full `/var` keeps those 128 KB too; a log whose copy cannot be made
   whole there either is emptied (`log_trim`, `rcs-lib.sh`).
10. **Registering the watchdog clients**
    (`echo "omcid 60" > /proc/odi_wdt/register`): a deadline the kernel
    enforces once omcid's own main loop starts pinging it, independent of
    the one-shot boot confirmation below -- missing it later in the boot
    resets the board just as surely as never confirming does. This still
    happens here, inside rcS, before the daemon respawn entries start:
    only the deadline is set, nothing is armed, so it does not matter that
    omcid itself has not started yet.

rcS returns here; everything from this point on is `rcS.pon`, the PON
steps, running concurrently with the respawn entries just started.

11. **The watchdog confirmation**, `confirm_watchdog()`
    (`echo 1 > /proc/odi_wdt/userland_ok`), called from `pon_steps_default`
    right after `omci_start` -- still before `gponsn`/`gponpw`/`gponact`, so
    a problem on the PON/optics side cannot cost it. This never depended on
    omcid actually being up (only on `SWITCH_INIT_OK`, decided a few lines
    above in the same script), so moving the omcid startup itself out to a
    respawn entry does not change anything about when this fires. odi_wdt
    resets the
    board at 120 s of uptime unless this is written, which is what reverts
    a trial that never gets here (a hung kernel, a wedged rcS, or one of
    the three checks below failing). It needs no network REACHABILITY --
    no ARP, no ping (`confirm-arp`, in `rcS.dev`, is the development
    exception that adds an ARP reply from a configured host on top) -- but it does refuse to confirm
    unless management is at least POSSIBLE:
    - `required_mounts_ok()`: every `/etc/fstab` mount the boot depends on
      other than the config partition -- `/var` and `/var/tmp`, both
      tmpfs -- actually landed (stage 1's live `/proc/mounts` check, not a
      flag; a missing one is logged to kmsg the moment `mount -a` finishes,
      not only when this asks);
    - `config_mounted()`: the config partition actually mounted (stage 2's
      live mount-table check, not a flag);
    - `network.sh configured`: a live re-check, off the interface itself
      (`br0`, or `eth0` if the bridge path failed), that the address
      currently applied is the one from config, not `network.sh`'s
      `DEF_IP` guess -- not a flag file written once at boot and never
      rechecked;
    - `SWITCH_INIT_OK`: `omci_start`'s `switch_init` did not fail.

    v1.0.2 and v1.0.3 both confirmed unconditionally and stayed up
    unreachable on real hardware for exactly this reason; v1.0.4 added the
    gate.
    `docs/SETTINGS.md` ("Watchdog rules") has every deadline, the memory
    floor, and why the userland process this replaced (v1.0.2) was
    withdrawn.
12. **The optics pins**, the `/proc/odi_init` verb `optics` (`odi_board.c`),
    because the SDK verbs never set them up on this board:
    `PIN_GPIO_SELECT` 0x048 = 0x08082001 (without it the port-1 I2C pins,
    DDM and the module status, stay muxed away and every transaction ends
    in NO_ACK). It runs before the PON steps and outside their list, so a
    `pon-steps` override keeps it.
13. **The PON steps**, each one `/proc/odi_init` verb: `i2c 1`, `i2cen 1`,
    `laser`, then the host-side SFP EEPROM (not a verb), `gpon`, `rxsd`,
    `gpondrv`, `gpondev`, then the switch init, then `gponsn`, `gponpw` and
    `gponact`.

    The host-side SFP EEPROM step runs `sfp-host-ddm.sh boot`: unless
    `SFP_HOST_DDM=0` or this is a safe-mode boot, it writes
    `odi_switch.sfp_mirror=1` and returns at once, and the kernel copies the
    module's A0h and A2h into the SoC SFP slave and turns it on in the
    background (`docs/SETTINGS.md`, "Host-side diagnostics"). It needs the
    I2C steps, and comes after `laser`, which selects A2h page 2 for a moment.

    `laser` (`odi_board.c`) needs the I2C controller, which answers only
    after `i2c` and `i2cen` (before them a read returns 0 with no error).
    It reads the laser driver chip's id (A2h page 2, byte 0xa0, putting
    the page select back) and switches the laser on only for the Semtech
    GN25L95 (0x6a) this image is validated with: SoC GPIO 13, the
    TX-disable input of the module, as an output driven low, the level
    set before the direction. As an input, its reset default, the laser
    stays off, the module reports TX_DISABLE_STATE and the OLT never hears
    the serial number; with it low the ONU went from O3 to O5 within 5 s.
    For any other laser driver, or none answering, the laser stays off
    and the verb fails: another board may need laser set-up this image
    does not do, and a laser lit outside its timeslots takes down every
    ONU on the OLT port. `gponact` is then refused (`-EPERM`, `odi_gpon:
    gponact refused: the laser is off` in the kernel log, whoever writes
    the verb). `gponsn` and `gponpw` still run. `gponact` also fails,
    with `-EIO` or `-ETIMEDOUT`, when the GPON MAC tables do not come
    clean (`docs/SWITCH.md`, "GPON boot init"): the ONU is not activated
    until a `gponact` succeeds (by hand, after an omcid respawn, or at
    the next boot), and the reason is in the kernel log
    (`/proc/odi_gpon/status` only shows O1). `ODI_LASER_UNVERIFIED=1`
    (odi.conf) makes `pon_step_laser` write `laser unverified`, which
    lights the laser whatever the id says; `optics.off` skips the verb and
    leaves it off. `/proc/odi_gpon/status` shows the result as
    `laser on X ldd_id Y unverified Z` (`ldd_id` -1: no answer). The order
    is fixed by the hardware.
    The switch init deactivates the ONU, so the activation steps come
    after it (with the switch init last, the ONU stayed in O1); the switch
    init is the `switch_init` write of `/proc/odi_omci`: the platform
    settings and the module-load replay the stock OMCI kernel modules made
    when they loaded (`docs/SWITCH.md`). The serial number and the PLOAM
    password come from the config store (`GPON_SN` in the HS file,
    `GPON_PLOAM_PASSWD` in the CS file); an empty password is not sent.
    `gponpw` takes the stored value in any of its forms: 20 hex digits,
    `0x` and up to 20 hex digits, or up to 10 characters of text
    (`odi_gpon_parse_pw`, `odi_gpon_pw.c`). The crumbs name the verb only, so neither value reaches a log.

    omcid itself no longer starts here -- it is its own `respawn` entry,
    running concurrently with this whole script -- but it must still
    register its receive path (redirect type 1, `/proc/odi_omci`) in a
    table that the `gpondrv`/`gpondev` init clears (before, the OMCI
    frames were counted and none delivered), and it must be registered
    before `gponact`, since the OLT can start sending OMCI frames the
    instant the ONU activates (a frame with nobody registered is
    `dropped_unregistered`, in the odi_omci kernel driver). So `gponact`
    (`pon_step_gponact`, `rcs-lib.sh`) polls the
    live `/proc/odi_omci` registration table first, once a second, bounded
    at 10 s, rather than assuming omcid has already caught up by the time
    this line runs -- and proceeds anyway if it never does, logging that it
    is activating without a registered omcid, rather than hanging the
    boot on a missing or wedged daemon.

## Respawn re-provisioning

What happens when omcid dies mid-boot, not at boot. Hardware finding (rc5,
ISP1, 2026-09-28): `kill -9 omcid` -- init respawns it within a few seconds
(`svc-omcid.sh`, `/etc/scripts/respawn.sh`), it pings the watchdog and
serves `omcicli` again, but `gpon_omci_services` stays 0 indefinitely. The
OLT has already provisioned this ONU from the first boot's MIB and does
not re-send it to a fresh omcid that starts with an empty one -- nothing
else on the ONU side asks it to. The proven fix, the same one omcid
makes for itself when an identity key changes (`apply.sh omci`, "SIGHUP
rereads the config store" in `docs/TOOLS.md`), is to force the OLT to
re-range: deactivate the ONU, let the OLT see it go, then reactivate --
which resets its MIB view and makes it provision the ONU again from
scratch, this time into the omcid that is actually running.

`svc-omcid.sh` tells a respawn apart from the first start of the boot with
a marker under `/var/run` (`SVC_OMCID_MARKER`, default
`/var/run/svc-omcid.started`): tmpfs, so it is gone at the next reboot,
same as every other per-boot flag this image keeps (`docs/BOOT.md`
elsewhere; `/var` is capped tmpfs, stage 1 above). The first start of the
boot creates the marker and does nothing else -- rcS.pon's own `gponact`
(stage 13) already provisions that one, the ordinary way. Every later
start (the marker already exists) instead backgrounds
`omci-respawn-reprovision.sh` and returns immediately, so `svc-omcid.sh`
still hands off to `respawn.sh`/`omcid` without waiting on anything --
nothing here may cost the respawn its own turn to run.

`omci-respawn-reprovision.sh` does NOT touch the omcid process itself:
by the time it runs, init is already supervising it as a `respawn` entry,
and killing or starting one from this script would just trigger another
respawn -- and another run of this script. It only drives `/proc/odi_init`:
write `gpondeact`, wait (bounded) for the omcid that is now running to
register with `/proc/odi_omci` (redirect type 1), re-apply the PLOAM
password from the config store, hold three seconds, then `gponact` --
`rcs-lib.sh`'s `omci_reactivate`. `apply.sh omci` no longer calls it: it
used to stop omcid and start its own beside init's respawn entry, and each
respawn then found that omcid still running, exited 1 and ran this script's
deactivation again (44 in two minutes on hardware, the ONU cycling O1 to O3).
`apply.sh omci` now sends SIGHUP and omcid re-registers itself
(`src/omci/respond/reload.c`); nothing outside init starts omcid, and this
script runs only after a genuine crash-respawn, as before.

On a kernel with no `/proc/odi_init` (the qemu system harness's stock
malta kernel, or a dev image with `modules.off`) `omci-respawn-reprovision.sh`
is a no-op: there are no PON verbs to drive, so it exits at once rather
than waiting out its own timeouts for nothing.

## Resume without re-registration

Re-provisioning above works, but costs a re-range: the ONU visibly drops
to O1 and climbs back to O5, an outage of ten to eighteen seconds measured
on ISP1. Since v1.0.5 `omcid` keeps enough of its own state on tmpfs
that most respawns need none of that: it resumes answering the OLT from
exactly where the killed process left off, with no outage at all, because
the switch datapath was never touched to begin with (see
`kb/rtl9601-omci-reapply-without-reboot.md` -- the hardware forwards
whether or not anything is alive on the control plane).

After every OMCI message that changes the MIB (Create, Set, Delete) `omcid`
writes `/var/run/omcid-mib.snap`: every managed entity it holds, the MIB
data sync counter, the (always zero today) alarm sequence number, and the
bookkeeping that maps a managed entity to what is actually programmed in
the switch -- the GEM flow ids, the T-CONT map, the broadcast flow, the
service (bridge connection) table. Written atomically, temp file then
rename, same as `cfgstore.c`'s own config writes, with a version header and
a CRC32 -- a torn write is never mistaken for a valid snapshot. A MIB reset
(from the OLT, or the CLI) deletes it at once: a MIB the OLT just discarded
must never be resumed into.

At startup, before anything that could block, a fresh `omcid` asks the
driver for the ONU state (the same call `onu_state_sample()` polls once a
second, command 13). If it is O5 -- the OLT is holding this ONU active, so
whatever provisioned it is still true -- and the snapshot on disk names the
same device (device id and serial, both read at startup regardless), it is
loaded: every array `omcid` keeps is repopulated by a plain memory copy,
nothing here calls `apply_entity()` or any driver function, so a resume
issues not one switch-programming driver call. Anything else -- no
snapshot, a mismatched device, a state below O5, or a snapshot whose CRC or
version does not check out -- falls back to the ordinary re-registration
path unchanged.

The snapshot also says which rebuild was still pending when it was written.
A Create or Set is answered and saved at once, and the rebuild that programs
it (the bridge connections, the upstream QoS) runs a quiet second later, at
most `REBUILD_MAX_S` after the change. An `omcid` killed inside that wait
leaves a MIB that is ahead of the switch; the respawned one takes the flags
from the snapshot and runs that rebuild after its own quiet second. That is
the one case in which a resume programs the switch. With nothing pending, which
is every snapshot written after a rebuild, it still makes no driver call.

`omcid` writes its decision to `/var/run/omcid-resume-decision`
(`"resumed"` or `"reprovision"`) before doing anything else that could
fail or block, so `omci-respawn-reprovision.sh` -- backgrounded by
`svc-omcid.sh` the same moment `omcid` starts -- only has to wait on that
file briefly (`DECISION_WAIT`, 5 s default), not for full registration.
The file still on tmpfs at that moment is the previous `omcid`'s, so
`svc-omcid.sh` moves it to `/var/run/omcid-resume-decision.prev` before it
backgrounds the helper: what the helper reads can only be the new `omcid`'s
own decision. `omcid` takes either file as proof that an earlier run existed
(a restart, not the first start of the boot) and removes both before it
writes its decision. A
`"resumed"` decision skips the deactivate/reactivate dance entirely and
exits; anything else (including no file at all inside the wait, the safe
default) runs the re-provisioning this section used to be the whole story
of.

An `omcid` that finds an earlier run and does not resume first removes what
that run left in the switch: the snapshot that describes it, every bridge
connection id, then the flows, T-CONTs and queues (driver command 0), the
clean-up `-r` asks for. The snapshot goes first, so a respawn in O5 after
this one dies early cannot resume services that are no longer there. Its own
tables start empty, so its rebuilds would never withdraw them, and a
provisioning with fewer services than the last one would leave the rest
forwarding until a reboot. A resumed `omcid` keeps them, with `-r` or
without: it took them over from the snapshot, and the datapath is the one
it describes. `svc-omcid.sh` passes `-r` on a respawn (its marker says an
`omcid` ran this boot), so this does not rest on the decision files alone.
An id whose deactivation fails is kept as pending and sent again, as after
a rebuild.

Test-only: `-s state` (main.c) uses a given ONU state for the resume
decision instead of asking the driver, the same reason `-c` takes a
capability blob -- a harness with no line side has no driver to answer
command 13 either (`src/omci/resume-test.sh`).

## The development aids: rcS.dev

`/etc/init.d/rcS.dev` ships in the image and is sourced by rcS. It
redefines four functions rcS calls at fixed points (`crumb`, `dev_hook`,
`dev_confirm_ok`, `pon_steps`); without the file each is a no-op or the
built-in behaviour. Each aid is off unless its file is on the config
partition, and each is set by hand on the running image before a trial:
the config partition is shared and `fwu.sh` never writes it, so what they
leave there is readable from the image the stick reverts to.

- **`breadcrumbs.on`**: every crumb is also appended, timestamped, to
  `/etc/config/breadcrumbs` (truncated each boot, synced after each line),
  with the host SerDes registers after the network and the PON registers
  after the PON steps. After the services it lowers the soft-lockup
  threshold to 20 s (the default 60 s is later than the watchdog, 42 s
  after the last kick, so it never spoke) and hung tasks to 30 s, starts a
  30 s heartbeat (uptime, free memory, load) to the kernel log, and writes
  `/etc/config/trial-diag.txt`, a network snapshot from the inside: one
  section per write, synced, each under `timeout 5`, and no `diag`, whose
  ioctl can block where `timeout` cannot end it. A write to flash through a
  path the trial is the first test of, so it is off unless asked for.
- The same questions go to the kernel log when `breadcrumbs.on` is set
  **or the config partition did not mount**: then this is the only
  diagnostic the boot gets.
- **`confirm-arp`**: the file holds one IPv4 address (one line, surrounding
  whitespace ignored), and the watchdog is confirmed only after an ARP reply
  from it: `: > /etc/config/confirm-arp` is not enough, write the address
  (`echo 192.168.1.2 > /etc/config/confirm-arp`). A trial that comes up
  unreachable then reverts by itself and keeps its ramlog (a watchdog reset
  keeps DRAM; a power cycle does not). An empty file, or content that is not
  a dotted IPv4 address, is no target: the boot says so (stderr and a crumb)
  and confirms the watchdog as usual, so a misconfigured toggle cannot start
  a reset loop. Nothing derives a host from the `br0` address; the other
  `arping` diagnostics use the same address and are skipped, with a log
  line, when there is none. Never leave it on a stick whose host stops
  answering there: every boot then resets at 120 s. Because of that, `nv
  commit` refuses while the file exists (`nv commit --force <slot>` commits
  anyway) and the ssh login banner names it, with its address, on every
  slot, committed or not.
- **`pon-steps`**: a list that replaces the built-in PON steps, one per
  line: a `/proc/odi_init` verb with its argument, or `omcimods` (the
  switch init, then the watchdog confirmation -- omcid starts as its own
  respawn entry regardless), `gponsn auto` or `gponpw auto` (the values from the config
  store), or `gponact` (waits for the omcid registration first, bounded,
  same as the built-in list). A plain `laser` line is the verb itself:
  it ignores `optics.off` and `ODI_LASER_UNVERIFIED`. It is how the order above was found, one step per
  boot. A wrong list means no PON.

`breadcrumbs.on` and `confirm-arp` are documented for users in
`docs/SETTINGS.md`; every switch, with where it is read, is in
`docs/HACKING.md` ("Feature toggles").
