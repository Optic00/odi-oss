#!/bin/sh
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

#
# Apply saved settings without a reboot. The web UI runs this after a save
# (odi-ui confd.h APPLY_PATH); it is as usable from a shell.
#
#     apply.sh network   LIVE. Re-apply the management addresses from the
#                        store: LAN_IP_ADDR/LAN_SUBNET (or /etc/config/lan-ip)
#                        and the second address, LAN_ENABLE_IP2, LAN_IP_ADDR2,
#                        LAN_SUBNET2, and the static routes LAN_ROUTES (odi.conf,
#                        docs/SETTINGS.md), which network.sh installs on the
#                        management device after the addresses: the routes of
#                        proto static on it are flushed and the stored ones
#                        replaced; nothing else is touched, and a safe-mode
#                        boot applies none. Prints the plan, then applies it one
#                        second later, in the background -- moving the
#                        primary drops the connection that asked for it, so
#                        the answer has to leave first.
#                        This applies what the store holds, with no way back;
#                        the web UI changes these keys through mgmt-addr.sh,
#                        a trial that reverts by itself, and runs this only in
#                        a safe-mode boot.
#
#     apply.sh omci [--confirm|--revert] [SECONDS]
#     apply.sh omci-live [SECONDS]
#                        Tells the running omcid to reread the config store
#                        (SIGHUP, the Unix convention for a reload) and waits,
#                        bounded, for the outcome. omcid compares the store
#                        with what it is running and does the least that
#                        applies:
#                          - nothing changed: says so, touches nothing;
#                          - only the VLAN keys (VLAN_CFG_TYPE, VLAN_MANU_MODE,
#                            VLAN_MANU_TAG_VID, VLAN_MANU_TAG_PRI,
#                            OMCI_CUSTOM_MCAST, RTK_DEVINFO_SPECVER): rebuilds
#                            the bridge connections in place from the MIB it
#                            holds. LIVE: the fibre stays in O5, nothing is
#                            re-ranged;
#                          - anything the OLT has to see again (GPON_SN, the
#                            PLOAM password, the LOID keys, the identity keys
#                            and switch, OMCI_UNKNOWN_ME_OK, OMCI_FIXED_ME,
#                            OMCI_PRIVATE_ME, OMCI_VEIP, OMCI_VEIP_SLOT_ID,
#                            OMCI_POTS_PORTS, OMCI_POTS_SLOT_ID): omcid
#                            deactivates the ONU, clears its MIB, hands the
#                            driver the new serial number and password and
#                            activates again, and the OLT provisions it from
#                            scratch. INTERRUPTS INTERNET for about ten
#                            seconds plus the OLT provisioning time. The
#                            process is never restarted.
#                        It waits SECONDS (1 or more, default 20, APPLY_WAIT; the web UI
#                        gives this script 25) in all, the wait for the lock
#                        and for the answer included, for the outcome: reload done,
#                        or for a re-registration back in O5 with services,
#                        and reports how long that took. A re-registration
#                        still running at the bound is reported as running and
#                        is not a failure; omcid carries on and finishes it.
#                        Exit 1 when no omcid runs, when omcid does not answer
#                        the signal, or when the reload failed; and, with
#                        nothing signalled, while omcid still runs the reload
#                        before (it takes the next signal only once that one
#                        is done).
#                        It never starts an omcid: init owns the process
#                        (inittab respawns svc-omcid.sh), and a second one
#                        made the ONU cycle through O1 to O3 on hardware.
#                        omcid records every re-registration and how the OLT
#                        answered it (/etc/config/odi-reregister, omcli
#                        state). After REREG_HOLD (2) refused changes in a row
#                        it holds a changed identity back (result held: the
#                        ONU stays on the identity it runs) until the reload
#                        is confirmed: --confirm, for one reload. --revert is
#                        the same confirmation from identity-trial.sh going
#                        back to the identity that worked, and is recorded as
#                        a revert, which the count of refusals skips. From
#                        one refusal on, and from REREG_DAY_WARN accepted
#                        changes in 24 h, the report carries a warning.
#                        omci-live (what the web UI sends after a VLAN
#                        save) applies only what keeps the ONU in O5: with
#                        an identity change also waiting in the store
#                        nothing is applied (result deferred, exit 3, so a
#                        caller can tell it from a failure), and a plain
#                        apply.sh omci applies both. After a reload that left
#                        the ONU off the line (a failed activation, no serial
#                        number) it re-registers all the same, the line being
#                        down already, and exits 4 when that worked or still
#                        runs at the bound: applied, but not in place. A verb
#                        of its own and not an option of omci: an apply.sh
#                        without it stops at the usage line before anything
#                        is signalled, where an option would be taken for the
#                        seconds and the whole store applied.
#                        One omci reload is signalled at a time: the
#                        confirmation file is shared, so a second apply.sh
#                        waits (ACK_WAIT) until omcid has taken the first.
#
#     apply.sh syslog    SERVICE RESTART. Restarts syslogd so it re-reads
#                        SYSLOG_SERVER. No OMCI interruption, no reboot: the
#                        respawn entry (svc-syslogd.sh) checks the store on
#                        every start the same way every other svc-*.sh does,
#                        so killing the running one is the whole apply.
#
#     apply.sh igmp      SERVICE RESTART. Restarts igmpd so it re-reads
#                        IGMP_SNOOPING, IGMP_PON_DROP and MLD_SNOOPING (read
#                        in svc-igmpd.sh), the
#                        same way as ntp below. The switch goes back to
#                        flooding for the moment the daemon is gone: its
#                        lease on the switch is its open /dev/odi_sw.
#
#     apply.sh sfp       LIVE. Re-reads SFP_HOST_DDM and turns the host-side
#                        SFP EEPROM on or off (sfp-host-ddm.sh apply): the
#                        module identity and live diagnostics the router
#                        reads, on board revisions that wire the SoC SFP
#                        slave. Waits, bounded, for the kernel outcome;
#                        exit 1 when it failed.
#
#     apply.sh ntp       SERVICE RESTART. Restarts ntpd so it re-reads
#                        NTP_SERVER -- starting it for the first time if the
#                        key was just set, or stopping it if just cleared
#                        (svc-ntpd.sh's own off-flag-shaped gate on the key
#                        being present at all).
#
#     syslog, igmp and ntp exit 1 when a process they had to signal is
#     still alive and could not be signalled.
#
# Why a signal and not a restart. omcid reads the store at start and builds
# connections only when the OLT provisions them, so a changed store needs
# either a new process with an empty MIB (which the OLT then has to be forced
# to reprovision, and which init would respawn on top of) or the running
# process rereading it. The second is what SIGHUP has always meant, and the
# only one that leaves a VLAN-only change without a re-ranging. omcid does the
# gpondeact/gponact steps itself when a re-registration is needed
# (src/omci/respond/reload.c, docs/SETTINGS.md "How apply works").
#
# What omcid tells this script is in /var/run/omcid-reload, one key=value per
# line, replaced whole by a rename: id (pid.sequence), state (running or
# done), changed, action, result, keys (names, never values), duration_ms,
# o5_ms, services.
#
set -u

# confd starts its children with no environment; export a PATH so that
# commands started through busybox timeout find /sbin and /usr/sbin too.
PATH=${PATH:+$PATH:}/sbin:/usr/sbin:/bin:/usr/bin
export PATH

RELOAD_STATUS=${RELOAD_STATUS:-/var/run/omcid-reload}
REREG_CONFIRM=${REREG_CONFIRM:-/var/run/omcid-reregister.confirm}
OMCI_LOCK=${OMCI_LOCK:-/var/run/apply-omci.lock}
NETWORK=${NETWORK:-/etc/scripts/network.sh}
MODULES_OFF=${MODULES_OFF:-/etc/config/modules.off}
SERVICES_LOG=${SERVICES_LOG:-/var/log/services.log}
SFP_HOST_DDM_SH=${SFP_HOST_DDM_SH:-/etc/scripts/sfp-host-ddm.sh}

# How long each wait may take, in seconds.
ACK_WAIT=${ACK_WAIT:-5}             # omcid takes the signal and starts the reload
APPLY_WAIT=${APPLY_WAIT:-20}        # the outcome; under the web UI 25 s bound
LINGER_MAX=${LINGER_MAX:-180}       # the lock after an unanswered signal (below)

say() { echo "apply: $*"; }
die() { echo "apply: $*" >&2; exit 1; }

network() {
	[ -x "$NETWORK" ] || die "no $NETWORK"
	# The plan, synchronously, so the caller sees what will change.
	sh "$NETWORK" addr -n || die "network.sh could not work out the addresses"
	setsid sh -c 'sleep 1; exec sh "$1" addr' apply "$NETWORK" \
		< /dev/null >> "$SERVICES_LOG" 2>&1 &
	say "applying in one second"
}

# status_get <key>: one value of the status file, empty when absent.
status_get() { sed -n "s/^$1=//p" "$RELOAD_STATUS" 2>/dev/null | head -n 1; }
# snap_get <key>: the same from $snap, the file as read once.
snap_get() { printf '%s\n' "$snap" | sed -n "s/^$1=//p" | head -n 1; }

# run_alive <id> <pids>: 0 when the run <id> (pid.sequence) is of one of
# the omcid <pids>.
run_alive() {
	for _p in $2; do
		[ "${1%%.*}" = "$_p" ] && return 0
	done
	return 1
}

# secs <milliseconds>: whole and tenths of a second, for the report.
secs() { echo "$(( ${1:-0} / 1000 )).$(( ${1:-0} % 1000 / 100 ))"; }

# omci_lock: the one omci reload being signalled. The confirmation file is
# shared: a second apply.sh writing or removing it between the write of the
# first and omcid taking it would change what the first reload confirms.
# Held from that write until omcid answers, at most ACK_WAIT seconds to get,
# and no longer than the bound of the run. lock_s is the seconds it took,
# which count against that bound.
omci_lock() {
	command -v flock >/dev/null 2>&1 || die "no flock to take $OMCI_LOCK"
	( : >> "$OMCI_LOCK" ) 2>/dev/null || die "cannot open $OMCI_LOCK"
	exec 8>>"$OMCI_LOCK"
	lock_s=0
	while ! flock -n 8; do
		[ "$lock_s" -lt "$ACK_WAIT" ] && [ "$lock_s" -lt "$wait_s" ] ||
			die "another apply.sh omci is still waiting for omcid to answer; try again"
		sleep 1
		lock_s=$((lock_s + 1))
	done
}

# omci_lock_linger <id> <pids>: the signal went unanswered and may still be
# taken. The reload it starts would take whatever confirmation is on disk
# then, the one of the next apply.sh included, and leave the signal of that
# one with none. So the lock stays held, by a child with no output, until
# the status file shows a run other than <id> of one of the omcid <pids> it
# went to, or none of them lives: an omcid that took the signal answers, and
# one that never will is gone (an older build stops at SIGHUP) or, hung, is
# reset by its watchdog rule. A reload that was still running when the
# signal went out is refused before it (omci below), so what this waits out
# is an omcid slow to look, and the longest reload, a re-registration of up
# to 153 s, is the most such a signal can wait. LINGER_MAX (180 s) is the
# end all the same: an omcid that ran the reload but could not write its
# status (a full /var) would otherwise hold every later apply off for as
# long as it runs.
omci_lock_linger() {
	(
		_n=0
		while [ "$_n" -lt "$LINGER_MAX" ]; do
			_id=$(status_get id)
			[ -n "$_id" ] && [ "$_id" != "$1" ] && run_alive "$_id" "$2" && exit 0
			# shellcheck disable=SC2086 # one pid per word
			kill -0 $2 2>/dev/null || exit 0
			sleep 1
			_n=$((_n + 1))
		done
	) < /dev/null > /dev/null 2>&1 &
}

# omci <confirm> [option] [SECONDS]: confirm is empty for apply.sh omci,
# which takes --confirm or --revert, and live for apply.sh omci-live,
# which takes none.
omci() {
	confirm=$1; shift
	if [ -z "$confirm" ]; then
		case ${1:-} in
		--confirm) confirm=confirm; shift ;;
		--revert)  confirm=revert; shift ;;
		esac
	fi
	case ${1:-} in
	-*) die "omci: unknown option $1${confirm:+ for omci-live}" ;;
	esac
	wait_s=${1:-$APPLY_WAIT}
	case $wait_s in
	''|*[!0-9]*) die "omci: the bound in seconds is a whole number, not $wait_s" ;;
	esac
	# At least one second: with none, the wait for the answer below ends
	# before its first look and the reload reads as unanswered.
	[ "$wait_s" -gt 0 ] || die "omci: the bound is at least 1 s, not $wait_s"
	[ ! -f "$MODULES_OFF" ] || die "$MODULES_OFF is set: this boot runs no omcid, nothing to reload"
	pids=$(pidof omcid 2>/dev/null)
	[ -n "$pids" ] || die "no omcid is running: nothing to reload (init respawns it; see /var/log/omcid.log)"

	omci_lock
	# The omcid to signal, named again under the lock: the answer counted
	# below is a run of one of these.
	pids=$(pidof omcid 2>/dev/null)
	[ -n "$pids" ] || die "no omcid is running: nothing to reload (init respawns it; see /var/log/omcid.log)"
	# omcid takes a signal only once the reload it runs is done, a
	# re-registration up to 153 s after it began. A signal sent now would
	# wait that long, past the bound here and past the lock, and the reload
	# it then starts would take the confirmation of whichever apply.sh
	# wrote one last. Refused instead, with nothing written or signalled.
	# Only a run of an omcid that still lives: the id is pid.sequence, and
	# a status left by one that died mid-reload is no reload at all (a new
	# omcid removes it at its start too).
	snap=$(cat "$RELOAD_STATUS" 2>/dev/null)
	if [ "$(snap_get state)" = running ] && run_alive "$(snap_get id)" "$pids"; then
		die "omcid is still applying the last change ($(snap_get changed), $(snap_get result)); try again once it is done (event=reload_done in /var/log/omcid.log)"
	fi
	before=$(snap_get id)
	# The confirmation omcid takes away with this reload (and any other):
	# written before the signal, so the reload it starts finds it.
	if [ -n "$confirm" ]; then
		echo "$confirm" > "$REREG_CONFIRM" || die "could not write $REREG_CONFIRM"
	else
		rm -f "$REREG_CONFIRM"
	fi
	# shellcheck disable=SC2086 # one pid per word
	kill -HUP $pids 2>/dev/null || die "could not signal omcid $pids"
	say "omcid $pids told to reload its configuration"

	# The acknowledgement: a new run of an omcid signalled here appears in
	# the status file (the id is pid.sequence). A daemon that never writes
	# one is an older build, where SIGHUP means stop. i counts the seconds
	# of the whole run, the wait for the lock first.
	i=$lock_s
	while :; do
		id=$(status_get id)
		[ -n "$id" ] && [ "$id" != "$before" ] && run_alive "$id" "$pids" && break
		if [ "$((i - lock_s))" -ge "$ACK_WAIT" ] || [ "$i" -ge "$wait_s" ]; then
			omci_lock_linger "$before" "$pids"
			die "omcid did not answer the signal within $((i - lock_s)) s (an older build treats SIGHUP as stop; init respawns it)"
		fi
		sleep 1
		i=$((i + 1))
	done
	# omcid has taken the confirmation: the next apply.sh may write its own.
	exec 8>&-

	# The outcome, bounded. Counted in iterations of one second: this is
	# also what runs under the test fixture, where uptime is a constant.
	# The seconds the lock and the acknowledgement took count against the
	# same bound, so neither a slow acknowledgement nor a second apply.sh
	# ahead of this one adds a full wait on top (ACK_WAIT 5 + APPLY_WAIT 20
	# went past the 25 s the web UI gives this script, by a hair). It is
	# counted in sleeps: the time the commands take comes on top. The
	# file is read whole each time (omcid replaces it by rename), and only
	# while it holds this run: once omcid acknowledged it, the next apply.sh
	# may start another reload, and the result of that is not this one.
	while :; do
		snap=$(cat "$RELOAD_STATUS" 2>/dev/null)
		[ "$(snap_get id)" = "$id" ] || break
		[ "$(snap_get state)" != "done" ] || break
		[ "$i" -lt "$wait_s" ] || break
		sleep 1
		i=$((i + 1))
	done
	if [ "$(snap_get id)" != "$id" ]; then
		say "another apply.sh omci started the next reload before the outcome of this one was read; see /var/log/omcid.log (event=reload_done)" >&2
		return 1
	fi

	changed=$(snap_get changed) result=$(snap_get result)
	keys=$(snap_get keys) retry=$(snap_get retry)
	# A reload that re-registers only because the last one left the ONU
	# off the line (a failed activation, no serial number). omci-live then
	# exits 4: applied, but not in place, which a caller can tell from both
	# a plain success and a failure.
	rereg_exit=0
	if [ "$retry" = 1 ]; then
		say "the last reload left the ONU off the line: this one re-registers it${keys:+ ($keys)}"
		[ "$confirm" != live ] || rereg_exit=4
	fi
	if [ "$(snap_get state)" != "done" ]; then
		say "$changed: still working after ${wait_s} s (${result}); omcid carries on and logs event=reload_done when it ends"
		return "$rereg_exit"
	fi
	dur=$(secs "$(snap_get duration_ms)")
	case "$changed:$result" in
	none:ok)
		say "nothing changed in the store; nothing done" ;;
	vlan:ok)
		say "VLAN handling changed ($keys): connections rebuilt in place in ${dur} s, the ONU was not re-registered" ;;
	identity:ok)
		if [ "$retry" = 1 ]; then
			say "re-registered: the ONU is back in O5 with $(snap_get services) service(s) after $(secs "$(snap_get o5_ms)") s (omcid $(snap_get duration_ms) ms in all)"
		else
			say "identity changed ($keys): the ONU re-registered and is back in O5 with $(snap_get services) service(s) after $(secs "$(snap_get o5_ms)") s (omcid $(snap_get duration_ms) ms in all)"
		fi
		rereg_warn
		return "$rereg_exit" ;;
	identity:held)
		say "identity changed ($keys) and NOT applied: the OLT refused the last $(snap_get streak) changes; the ONU stays on the identity it runs. Check the values, then: apply.sh omci --confirm" >&2
		return 1 ;;
	identity:deferred)
		say "identity changed ($keys) and NOT applied, nor anything else: omci-live changes nothing that re-registers the ONU. Apply it with: apply.sh omci" >&2
		return 3 ;;
	*)
		say "$changed${keys:+ ($keys)}: $result after ${dur} s -- see /var/log/omcid.log (event=reload_done)" >&2
		case $result in
		failed)         say "the driver refused a step; the ONU may be off the line until the next reload or a reboot" >&2 ;;
		timeout)        say "the ONU did not reach O5 in time; the OLT may not know the new identity" >&2 ;;
		refused)        say "the OLT refused the new identity ($(snap_get refusals) deactivation(s), or disabled the ONU)" >&2 ;;
		no_services)    say "the ONU is in O5 but the OLT built no services" >&2 ;;
		sn_not_applied) say "the ONU is back, but the driver did not take the new serial number" >&2 ;;
		pw_not_applied) say "the ONU is back, but the driver did not take the new PLOAM password; it runs the old one" >&2 ;;
		no_serial)      say "GPON_SN is empty: the ONU stays off the line until a serial number is set" >&2 ;;
		unreadable)     say "omcid could not read the config store; nothing was changed. Try again; if it stays unreadable, see /var/log/omcid.log" >&2 ;;
		esac
		[ "$changed" != identity ] || rereg_warn
		return 1 ;;
	esac
}

# rereg_warn: what omcid recorded about re-registrations, when it calls for
# care: refusals in a row, or many accepted changes in a day.
rereg_warn() {
	streak=$(snap_get streak) today=$(snap_get today)
	if [ "${streak:-0}" -ge 1 ]; then
		say "warning: the OLT refused the last ${streak} identity change(s) in a row; some operators block a line after a few. From ${REREG_HOLD:-2} on, a change needs apply.sh omci --confirm" >&2
	fi
	if [ -n "$today" ] && [ "$today" -ge "${REREG_DAY_WARN:-5}" ]; then
		say "warning: ${today} identity changes in the last 24 h" >&2
	fi
}

# restart_respawn <name>: kill whatever runs under the respawn entry of
# this daemon and let init bring it back, rereading the store.
# respawn.sh execs each daemon in place, so pidof names its init entry.
# Disabled services share anonymous respawn-off.sh placeholders. Wake all
# of them on the fallback path, as services start does: each wrapper reads
# its own enable flag again, and unrelated disabled services stay disabled.
# Returns 1 when a process that is still alive could not be signalled.
restart_respawn() {
	name=$1
	pids=$(pidof "$name" 2>/dev/null)
	what="$name"
	if [ -z "$pids" ]; then
		pids=$(pgrep -f "/etc/scripts/respawn-off.sh" 2>/dev/null)
		what="service placeholders"
	fi
	if [ -z "$pids" ]; then
		say "$name: no running process found, nothing to restart"
		return 0
	fi
	failed=0
	for pid in $pids; do
		if ! kill "$pid" 2>/dev/null && kill -0 "$pid" 2>/dev/null; then
			say "$what: could not signal pid $pid"
			failed=1
		fi
	done
	[ "$failed" = 0 ] || return 1
	say "$what restarted; init will reread the service settings"
}

syslog() {
	restart_respawn syslogd
}

ntp() {
	restart_respawn ntpd
}

igmp() {
	restart_respawn igmpd
}

sfp() {
	sh "$SFP_HOST_DDM_SH" apply
}

case "${1:-}" in
network) network ;;
omci)    shift; omci "" "$@" || exit $? ;;
omci-live) shift; omci live "$@" || exit $? ;;
syslog)  syslog || exit 1 ;;
ntp)     ntp || exit 1 ;;
igmp)    igmp || exit 1 ;;
sfp)     sfp || exit 1 ;;
*)       echo "usage: $0 network|omci [--confirm|--revert] [seconds]|omci-live [seconds]|syslog|ntp|igmp|sfp" >&2; exit 1 ;;
esac
exit 0
