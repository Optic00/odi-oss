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
#                        It waits SECONDS (default 20, APPLY_WAIT; the web UI
#                        gives this script 25), counted from the signal, for the outcome: reload done,
#                        or for a re-registration back in O5 with services,
#                        and reports how long that took. A re-registration
#                        still running at the bound is reported as running and
#                        is not a failure; omcid carries on and finishes it.
#                        Exit 1 when no omcid runs, when omcid does not answer
#                        the signal, or when the reload failed.
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
NETWORK=${NETWORK:-/etc/scripts/network.sh}
MODULES_OFF=${MODULES_OFF:-/etc/config/modules.off}
SERVICES_LOG=${SERVICES_LOG:-/var/log/services.log}
SFP_HOST_DDM_SH=${SFP_HOST_DDM_SH:-/etc/scripts/sfp-host-ddm.sh}

# How long each wait may take, in seconds.
ACK_WAIT=${ACK_WAIT:-5}             # omcid takes the signal and starts the reload
APPLY_WAIT=${APPLY_WAIT:-20}        # the outcome; under the web UI 25 s bound

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

# secs <milliseconds>: whole and tenths of a second, for the report.
secs() { echo "$(( ${1:-0} / 1000 )).$(( ${1:-0} % 1000 / 100 ))"; }

omci() {
	confirm=
	case ${1:-} in
	--confirm) confirm=confirm; shift ;;
	--revert)  confirm=revert; shift ;;
	esac
	wait_s=${1:-$APPLY_WAIT}
	[ ! -f "$MODULES_OFF" ] || die "$MODULES_OFF is set: this boot runs no omcid, nothing to reload"
	pids=$(pidof omcid 2>/dev/null)
	[ -n "$pids" ] || die "no omcid is running: nothing to reload (init respawns it; see /var/log/omcid.log)"

	before=$(status_get id)
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

	# The acknowledgement: a new run appears in the status file. A daemon
	# that never writes one is an older build, where SIGHUP means stop.
	i=0
	while :; do
		id=$(status_get id)
		[ -n "$id" ] && [ "$id" != "$before" ] && break
		[ "$i" -lt "$ACK_WAIT" ] || die "omcid did not answer the signal within ${ACK_WAIT} s (an older build treats SIGHUP as stop; init respawns it)"
		sleep 1
		i=$((i + 1))
	done

	# The outcome, bounded. Counted in iterations of one second: this is
	# also what runs under the test fixture, where uptime is a constant.
	# The seconds the acknowledgement took count against the same bound, so
	# a slow acknowledgement plus a full wait cannot outlast the 25 s the
	# web UI gives this script (ACK_WAIT 5 + APPLY_WAIT 20 did, by a hair).
	while [ "$(status_get state)" != "done" ] && [ "$i" -lt "$wait_s" ]; do
		sleep 1
		i=$((i + 1))
	done

	changed=$(status_get changed) result=$(status_get result)
	keys=$(status_get keys)
	if [ "$(status_get state)" != "done" ]; then
		say "$changed: still working after ${wait_s} s (${result}); omcid carries on and logs event=reload_done when it ends"
		return 0
	fi
	dur=$(secs "$(status_get duration_ms)")
	case "$changed:$result" in
	none:ok)
		say "nothing changed in the store; nothing done" ;;
	vlan:ok)
		say "VLAN handling changed ($keys): connections rebuilt in place in ${dur} s, the ONU stayed in O5" ;;
	identity:ok)
		say "identity changed ($keys): the ONU re-registered and is back in O5 with $(status_get services) service(s) after $(secs "$(status_get o5_ms)") s (omcid $(status_get duration_ms) ms in all)"
		rereg_warn ;;
	identity:held)
		say "identity changed ($keys) and NOT applied: the OLT refused the last $(status_get streak) changes; the ONU stays on the identity it runs. Check the values, then: apply.sh omci --confirm" >&2
		return 1 ;;
	*)
		say "$changed ($keys): $result after ${dur} s -- see /var/log/omcid.log (event=reload_done)" >&2
		case $result in
		failed)         say "the driver refused a step; the ONU may be off the line until the next reload or a reboot" >&2 ;;
		timeout)        say "the ONU did not reach O5 in time; the OLT may not know the new identity" >&2 ;;
		refused)        say "the OLT refused the new identity ($(status_get refusals) deactivation(s), or disabled the ONU)" >&2 ;;
		no_services)    say "the ONU is in O5 but the OLT built no services" >&2 ;;
		sn_not_applied) say "the ONU is back, but the driver did not take the new serial number" >&2 ;;
		esac
		[ "$changed" != identity ] || rereg_warn
		return 1 ;;
	esac
}

# rereg_warn: what omcid recorded about re-registrations, when it calls for
# care: refusals in a row, or many accepted changes in a day.
rereg_warn() {
	streak=$(status_get streak) today=$(status_get today)
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
omci)    shift; omci "$@" || exit 1 ;;
syslog)  syslog || exit 1 ;;
ntp)     ntp || exit 1 ;;
igmp)    igmp || exit 1 ;;
sfp)     sfp || exit 1 ;;
*)       echo "usage: $0 network|omci [--confirm|--revert] [seconds]|syslog|ntp|igmp|sfp" >&2; exit 1 ;;
esac
exit 0
