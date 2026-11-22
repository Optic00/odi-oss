# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

# shellcheck shell=sh
#
# rcs-lib.sh -- functions shared by /etc/init.d/rcS (sysinit) and
# /etc/init.d/rcS.pon (the `once` entry that runs the PON steps after it,
# docs/BOOT.md). Split out so both scripts see the same config_mounted,
# crumb and confirm_watchdog logic rather than two copies drifting apart.
# rcS.dev, when present, is sourced by BOTH callers too and redefines crumb,
# dev_hook, dev_confirm_ok and pon_steps -- without it every one of those is
# the plain default below.

kmsg() { echo "rcS: $*" > /dev/kmsg 2>/dev/null || true; }
crumb() { kmsg "$*"; }

# Bounded write into a driver /proc verb file (odi_init, odi_omci, and
# friends). A bare `echo x > file` is a write(2) the calling shell performs
# directly, not a forked command -- `timeout` only bounds a process it
# starts, so the write has to run inside one for `timeout` to reach it at
# all. This is a documented limit, not a guarantee: a write the kernel driver
# blocks on in D state (uninterruptible sleep) ignores every signal,
# including the one `timeout` sends -- see rcS.dev, the note on why a diag
# ioctl gets no `timeout` wrapper there either. It still catches the ordinary
# case (a driver waiting on a lock or a condition that resolves on its own),
# which is what "odi_init step hangs the boot" would actually look like.
#
# The line goes through positional parameters ($1/$2 inside the inner shell),
# not string interpolation into the script text, so a value containing shell
# metacharacters (a password from the config store, say) is never re-parsed.
PROC_WRITE_TIMEOUT_S=${PROC_WRITE_TIMEOUT_S:-5}
write_proc_bounded() {
	path=$1 line=$2
	timeout "$PROC_WRITE_TIMEOUT_S" sh -c 'echo "$1" > "$2"' _ "$line" "$path"
}
read_proc_bounded() {
	path=$1
	timeout "$PROC_WRITE_TIMEOUT_S" cat "$path" 2>/dev/null
}

# uptime_s: seconds since boot, from $UPTIME (default /proc/uptime, a
# fixture path under test), for omci_reactivate below.
UPTIME=${UPTIME:-/proc/uptime}
uptime_s() { cut -d. -f1 "$UPTIME"; }

# cs_value <file> <key>: the value of <key> in a config store XML file, as the
# omcid config reader (cfgstore.c) sees it. xmlconfig writes the five XML
# entities into every value, so a PLOAM password such as a&b is stored as
# a&amp;b and has to be turned back before it is sent to the driver. &amp;
# goes last, so that &amp;lt; comes out as the text &lt; and not as <.
# Prints nothing when the key or the file is missing.
cs_value() {
	sed -n "s/.*Name=\"$2\" Value=\"\([^\"]*\)\".*/\1/p" "$1" 2>/dev/null | head -n 1 |
		sed -e 's/&quot;/"/g' -e "s/&apos;/'/g" -e 's/&lt;/</g' -e 's/&gt;/>/g' -e 's/&amp;/\&/g'
}

# omci_reactivate <t0> <register_wait> <deact_hold>
#
# The tail of the hardware-proven re-provisioning sequence: wait for
# whatever omcid is currently running to register with odi_omci (redirect
# type 1), re-apply the PLOAM password from the CS file, hold until
# <deact_hold> seconds have passed since <t0>, then reactivate (gponact).
# The caller must already have written gpondeact -- this only covers the
# part downstream of it. Its one caller is omci-respawn-reprovision.sh, the
# automatic re-provisioning after a respawn (docs/BOOT.md, "Respawn
# re-provisioning"), which must not touch the omcid process at all: init is
# already supervising it as a respawn entry, and killing or starting one
# from here would just trigger another respawn. (apply.sh omci does not use
# it: omcid re-registers itself on SIGHUP.)
#
# Echoes the pid that registered, or nothing if REGISTER_WAIT ran out
# (activation is attempted either way); returns 1 if the
# final gponact write itself failed, 0 otherwise.
omci_reactivate() {
	t0=$1 register_wait=$2 deact_hold=$3
	odi_init=${ODI_INIT:-/proc/odi_init}
	odi_omci=${ODI_OMCI:-/proc/odi_omci}
	cs=${CS:-/etc/config/lastgood.xml}

	found=
	i=0
	while [ "$i" -lt "$register_wait" ]; do
		sleep 1
		i=$((i + 1))
		p=$(pidof omcid)
		if [ -n "$p" ] && grep -q "registered:.*type=1 pid=$p" "$odi_omci" 2>/dev/null; then
			found=$p
			break
		fi
	done

	# The PLOAM password is read here the way rcS reads it at boot (the
	# gponpw auto step): from the CS file, hex, skipped when empty.
	pw=$(cs_value "$cs" GPON_PLOAM_PASSWD)
	[ -n "$pw" ] && { write_proc_bounded "$odi_init" "gponpw $pw" 2>/dev/null || true; }

	while [ $(( $(uptime_s) - t0 )) -lt "$deact_hold" ]; do
		sleep 1
	done
	write_proc_bounded "$odi_init" gponact 2>/dev/null || return 1
	echo "$found"
}

# log_trim <file>: past LOG_MAX bytes, cut <file> back to its last LOG_KEEP.
# The copy goes to LOG_CUT_DIR, /var/tmp, a tmpfs of its own (/etc/fstab):
# next to the log there is no room for it when /var is full. busybox tail -c
# of a file exits 0 when its write fails, so the copy is judged by its size
# and not by the status. A copy that is not whole empties the log, which
# gives /var its space back all the same.
LOG_MAX=${LOG_MAX:-262144}
LOG_KEEP=${LOG_KEEP:-131072}
LOG_CUT_DIR=${LOG_CUT_DIR:-/var/tmp}
log_trim() {
	f=$1
	[ -f "$f" ] || return 0
	n=$(wc -c < "$f" 2>/dev/null)
	[ "${n:-0}" -gt "$LOG_MAX" ] || return 0
	cut=$LOG_CUT_DIR/${f##*/}.cut
	{ tail -c "$LOG_KEEP" "$f" > "$cut"; } 2>/dev/null
	n=$({ wc -c < "$cut"; } 2>/dev/null)
	if [ "${n:-0}" -eq "$LOG_KEEP" ]; then
		cat "$cut" > "$f"
	else
		: > "$f"
	fi
	rm -f "$cut" 2>/dev/null
}

dev_hook() { :; }
dev_confirm_ok() { return 0; }
pon_steps() { pon_steps_default; }	# the rcS.dev pon-steps override replaces this

# The one place anything asks "is the config partition actually mounted".
# The live mount table, checked fresh every call -- see rcS (this used to be
# defined only there; unchanged).
CONFIG_DIR=/var/config
config_mounted() {
	grep -qE "^[^ ]+ $CONFIG_DIR jffs2 " /proc/mounts 2>/dev/null && [ -d /etc/config ]
}

required_mounts_ok() {
	ok=1
	for m in /var /var/tmp; do
		grep -qE "^[^ ]+ $m tmpfs " /proc/mounts 2>/dev/null || {
			kmsg "required mount missing: $m (tmpfs)"
			ok=0
		}
	done
	[ "$ok" = 1 ]
}

# ---- the PON steps (docs/BOOT.md, "The PON steps"). Run from rcS.pon, the
# `once` inittab entry started right after sysinit returns -- concurrently
# with the respawn entries (dropbear, confd, metricsd, omcid) init starts in
# the same breath, exactly as these ran concurrently with the backgrounded
# `services start` before v1.0.4 (rcS launched it with a trailing `&` and
# moved straight on to the PON steps without waiting for it either).

pon_step() {
	crumb "pon step ${1%% *}: start"
	write_proc_bounded /proc/odi_init "$1" 2>/dev/null || \
		echo "rcS: odi_init ${1%% *} refused" >&2
	crumb "pon step ${1%% *}: ret $(read_proc_bounded /proc/odi_init)"
}

# The serial number (an HS key) and the PLOAM password (hex, in the CS
# file) come from the config store, never from a file of ours. An empty
# password (an OLT that authenticates on the serial alone) is not sent:
# the driver keeps its default.
store_sn() {
	sed -n "s/.*Name=\"GPON_SN\" Value=\"\([^\"]*\)\".*/\1/p" "${HS:-/etc/config/lastgood_hs.xml}" 2>/dev/null | head -n 1
}
# An empty GPON_SN (a store recreated by config-reseed.sh, or one the owner
# emptied) leaves the PON down: the gponsn step is skipped, and so is every
# gponact (pon_step_gponact, omci-respawn-reprovision.sh), rather than the
# ONU ranging with the serial the driver starts with.
pon_step_gponsn() {
	sn=$(store_sn)
	if [ -z "$sn" ]; then
		crumb "pon step gponsn: no GPON_SN in the store, the PON stays down"
		return 0
	fi
	pon_step "gponsn $sn"
}
pon_step_gponpw() {
	pw=$(cs_value "${CS:-/etc/config/lastgood.xml}" GPON_PLOAM_PASSWD)
	if [ -n "$pw" ]; then
		pon_step "gponpw $pw"
	else
		crumb "pon step gponpw: none configured, skipped"
	fi
}

# gponact activates the ONU, and the OLT can start sending OMCI frames the
# instant it does -- so omcid must already be registered with odi_omci
# (redirect type 1) or the first frames are dropped_unregistered
# (kernel/extra/drivers/net/ethernet/odi/odi_omci.c). Before v1.0.4, omcid
# was forked right here, inline, so it had already called nl_redirect()
# (its own registration) by the time this ran; now it starts as its own
# respawn entry, concurrently with this whole script, so its startup can
# lag this point by an unpredictable (if usually small) amount. Poll the
# live registration table instead of guessing a fixed delay -- bounded, so
# a stuck or missing omcid still lets activation happen rather than hang
# the boot.
# A gponact the driver refuses because the GPON tables did not come clean
# (odi_gpon_init_apply(), docs/SWITCH.md "GPON boot init") leaves the ONU
# in O1, and the driver does not try again by itself. Nothing else in the
# boot writes gponact a second time, so one failed clean would keep the
# stick off the line until the next reboot. Each new try runs the whole
# boot replay again, reset pulse and clean included, so the step tries
# GPONACT_TRIES times in all, GPONACT_RETRY_S seconds apart.
# The decision is taken from the boot_replay line of /proc/odi_gpon/status,
# not from /proc/odi_init, whose read side holds the result of whichever
# verb ran last: another try only while it reads "failed" with -EIO (-5)
# or -ETIMEDOUT (-145 on MIPS o32, not the generic 110). Anything else, a
# refusal for another reason included, ends the step: trying again would
# not change it. The tries write "gponact boot", which does nothing once
# another path (an omcid respawn) has activated the ONU meanwhile.
GPONACT_TRIES=${GPONACT_TRIES:-3}
GPONACT_RETRY_S=${GPONACT_RETRY_S:-5}
GPON_STATUS=${GPON_STATUS:-/proc/odi_gpon/status}
# boot_replay_failed: prints the errno of a boot replay that stopped after
# a failed table clean, nothing otherwise.
boot_replay_failed() {
	v=$(read_proc_bounded "$GPON_STATUS" | sed -n 's/^boot_replay failed //p' | head -n 1)
	case $v in -5 | -145) echo "$v" ;; esac
}
pon_step_gponact() {
	if [ -f "$SAFE_FLAG" ]; then
		crumb "pon step gponact: safe-mode boot, not activated"
		return 0
	fi
	if [ -z "$(store_sn)" ]; then
		crumb "pon step gponact: no GPON_SN in the store, not activated"
		return 0
	fi
	i=0
	while [ "$i" -lt 10 ]; do
		grep -q "type=1 " /proc/odi_omci 2>/dev/null && break
		sleep 1
		i=$((i + 1))
	done
	if grep -q "type=1 " /proc/odi_omci 2>/dev/null; then
		crumb "gponact: omcid registered with odi_omci after ${i}s"
	else
		crumb "gponact: omcid NOT registered with odi_omci after 10s, activating anyway"
	fi
	pon_step gponact
	# Anything but 1 to 9 tries and 1 to 59 s (not a number, too long for
	# the shell's arithmetic or for sleep) could leave the loop unbounded
	# or end rcS.pon under set -e: the defaults stand in for it.
	tries=$GPONACT_TRIES wait_s=$GPONACT_RETRY_S
	case $tries in [1-9]) ;; *) tries=3 ;; esac
	case $wait_s in [1-9] | [1-5][0-9]) ;; *) wait_s=5 ;; esac
	n=1
	while err=$(boot_replay_failed) && [ -n "$err" ]; do
		if [ "$n" -ge "$tries" ]; then
			crumb "gponact: GPON tables still not clean after $n tries, the PON stays down until a gponact succeeds"
			return 0
		fi
		n=$((n + 1))
		crumb "gponact: GPON tables not clean ($err), try $n of $tries in ${wait_s}s"
		sleep "$wait_s"
		# An omcid respawn may have activated the ONU meanwhile.
		[ -n "$(boot_replay_failed)" ] || return 0
		pon_step "gponact boot"
	done
}

# The switch init the stock OMCI modules made when they loaded (platform
# settings, module-load replay). /etc/config/modules.off skips it (and, as
# before, the PON activation steps still run -- only the switch program and
# the SWITCH_INIT_OK gate below are skipped). omcid itself no longer starts
# here: it is /etc/scripts/svc-omcid.sh, a respawn entry in /etc/inittab
# (docs/SETTINGS.md, "native over hand-rolled" -- supervise.sh is gone).
omci_start() {
	if [ -f /etc/config/modules.off ]; then
		crumb "omci: modules.off, switch init skipped"
		return 0
	fi
	if [ ! -w /proc/odi_omci ]; then
		crumb "omci: no /proc/odi_omci, switch init skipped"
		return 0
	fi
	if write_proc_bounded /proc/odi_omci switch_init 2>/var/log/switch_init.err; then
		crumb "odi_switch init: platform settings and module-load replay done"
	else
		SWITCH_INIT_OK=0
		crumb "odi_switch init: /proc/odi_omci write failed, see /var/log/switch_init.err"
	fi
}

# ---- the watchdog confirmation. See docs/BOOT.md, "The watchdog
# confirmation" -- unchanged by the respawn split: this never depended on
# omcid actually being up, only on switch_init not having failed
# (SWITCH_INIT_OK), the config and required mounts, and a real management
# address, all of which are decided inside this same script by the time it
# runs.
WDOK=/proc/odi_wdt/userland_ok
SWITCH_INIT_OK=1
confirm_watchdog() {
	[ -w "$WDOK" ] || return 0
	# A safe-mode boot (safe-mode.sh) exists to be reached, and it is entered
	# because boots were not confirming or the config partition is dead, so
	# the checks that fail then (a mount, the configured address, the
	# confirm-arp aid) would reset it again. Only the switch init stays: with
	# it failed nothing is reachable and a reset is a fresh try.
	if [ -f "$SAFE_FLAG" ]; then
		if [ "$SWITCH_INIT_OK" != 1 ]; then
			crumb "watchdog NOT confirmed: switch_init failed"
			return 0
		fi
		echo 1 > "$WDOK"
		crumb "safe-mode boot: userland up, watchdog confirmed without the mount and address checks"
		return 0
	fi
	if ! required_mounts_ok; then
		crumb "watchdog NOT confirmed: a required mount is missing"
		return 0
	fi
	if ! config_mounted; then
		crumb "watchdog NOT confirmed: config partition not mounted"
		return 0
	fi
	if ! /etc/scripts/network.sh configured >/dev/null 2>&1; then
		crumb "watchdog NOT confirmed: no management address configured"
		return 0
	fi
	if [ "$SWITCH_INIT_OK" != 1 ]; then
		crumb "watchdog NOT confirmed: switch_init failed"
		return 0
	fi
	dev_confirm_ok || return 0
	echo 1 > "$WDOK"
	crumb "userland up, watchdog confirmed"
}

# The order is fixed by the hardware (docs/BOOT.md). The switch init goes
# after gpondev: gpondrv/gpondev clears the table the omcid receive path
# registers in, so the switch program (and, in the respawn world, the
# omcid startup itself) must not race ahead of it; the switch init
# deactivates the ONU, so gponsn, gponpw and gponact -- gated on omcid,
# above -- come after it.
# The laser TX enable, once the I2C steps have the controller answering: on
# only for the laser driver this image was validated with, otherwise left
# off and gponact refused (odi_board.h). ODI_LASER_UNVERIFIED=1 in odi.conf
# forces it on (docs/SETTINGS.md); /etc/config/optics.off leaves it off.
# A safe-mode boot (safe-mode.sh) leaves the laser off and does not
# activate, so a recovery boot cannot disturb the OLT; the kernel refuses
# gponact with the laser off as well.
SAFE_FLAG=${SAFE_FLAG:-/var/run/safe-mode}
pon_step_laser() {
	if [ -f "$SAFE_FLAG" ]; then
		crumb "pon step laser: safe-mode boot, laser left off"
	elif [ -f /etc/config/optics.off ]; then
		crumb "pon step laser: optics.off, laser left off"
	elif grep -q '^ODI_LASER_UNVERIFIED=1$' /etc/config/odi.conf 2>/dev/null; then
		pon_step "laser unverified"
	else
		pon_step laser
	fi
}

# The host-side SFP EEPROM (SFP_HOST_DDM in odi.conf, docs/SETTINGS.md):
# the module A0h and A2h, with live diagnostics, served to the host through
# the SoC SFP slave. After the laser step, which needs the same I2C and puts
# the A2h page select back. sfp-host-ddm.sh reads the key (on unless 0, off
# in a safe-mode boot), writes the kernel parameter and returns at once; the
# kernel fills the slave and turns it on in the background. Bounded inside.
SFP_HOST_DDM_SH=${SFP_HOST_DDM_SH:-/etc/scripts/sfp-host-ddm.sh}
pon_step_sfphostddm() {
	crumb "pon step sfp-host-ddm: start"
	sh "$SFP_HOST_DDM_SH" boot > /dev/null 2>&1 || echo "rcS: sfp-host-ddm.sh boot failed" >&2
	crumb "pon step sfp-host-ddm: done"
}

# Dying_Gasp on a reboot (GPON_DYING_GASP=1 in odi.conf, experimental,
# docs/SETTINGS.md): tells the driver to send it from its reboot notifier.
# Nothing is written, and nothing logged, while the key is not 1.
pon_step_dyinggasp() {
	if grep -q '^GPON_DYING_GASP=1$' /etc/config/odi.conf 2>/dev/null; then
		pon_step "gpondg 1"
	fi
}

# Keep the OLT Disable_Serial_Number across restarts and reboots
# (GPON_KEEP_DISABLE, on unless odi.conf has it at 0, docs/SETTINGS.md). gponkeep turns the
# driver feature on; with the marker file present (gpon-hold.sh: the OLT
# disabled this ONU, and has not enabled it since) gponhold 1 makes the
# activation that follows end in O7 with the laser line high, and nothing
# answers on the fibre until the OLT enables the ONU. A safe-mode boot does
# neither: it does not activate the PON, and it must not depend on a marker.
# Nothing is read, written or logged while the key is 0.
GPON_HOLD_MARKER=${GPON_HOLD_MARKER:-/etc/config/gpon-hold}
pon_step_keepdisable() {
	grep -q '^GPON_KEEP_DISABLE=0$' "${ODI_CONF:-/etc/config/odi.conf}" 2>/dev/null && return 0
	if [ -f "$SAFE_FLAG" ]; then
		crumb "pon step gponkeep: safe-mode boot, GPON_KEEP_DISABLE and its marker ignored"
		return 0
	fi
	pon_step "gponkeep 1"
	if [ -f "$GPON_HOLD_MARKER" ]; then
		crumb "pon step gponhold: marker present, the OLT disabled this ONU, starting in O7"
		pon_step "gponhold 1"
	fi
}

pon_steps_default() {
	pon_step "i2c 1"
	pon_step "i2cen 1"
	pon_step_laser
	pon_step_sfphostddm
	pon_step gpon
	pon_step rxsd
	pon_step gpondrv
	pon_step gpondev
	omci_start
	confirm_watchdog
	pon_step_gponsn
	pon_step_gponpw
	pon_step_dyinggasp
	pon_step_keepdisable
	pon_step_gponact
}
