#!/bin/sh
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

#
# identity-trial.sh -- a change of the line identity (GPON_SN,
# GPON_PLOAM_PASSWD, LOID, LOID_PASSWD) as a trial that goes back by itself.
# A wrong serial number or password leaves the line down until someone
# changes it back by hand, so nothing here stores a value the OLT has not
# taken:
#
#     identity-trial.sh try [--confirm] KEY=VALUE...
#                                   a trial: the new values go to
#                                   /var/run/identity-trial.conf, in RAM,
#                                   where omcid reads them over the store,
#                                   and omcid re-registers with them
#                                   (apply.sh omci). Returns at once; a
#                                   watcher in the background judges the
#                                   outcome. --confirm is passed to
#                                   apply.sh, for a change omcid holds back
#                                   after refused ones (docs/TOOLS.md,
#                                   "Re-registrations are counted")
#     identity-trial.sh keep        end the watch now and store the values;
#                                   only once the ONU is back in O5 with
#                                   services
#     identity-trial.sh revert      end the trial now, back to the stored
#                                   identity
#     identity-trial.sh status      the state as key=value lines, with the
#                                   before and after summary
#
# The trial goes back (the RAM file removed, apply.sh omci --revert, so the
# re-registration guard lets it through and does not count it) when:
#
#   - the re-registration does not end with the ONU in O5 and services:
#     result refused (the OLT deactivated the ONU three times, or disabled
#     it), timeout (no O5 within the 150 s omcid waits), no_services,
#     sn_not_applied or failed;
#   - within WATCH_S (600 s, the MIB reset loop window) of a good result, a
#     MIB reset loop starts on a line that had none before the trial
#     (omcli state, "mib resets");
#   - someone says revert.
#
# After WATCH_S without any of that (or at keep), the values are written to
# the store with flash set and the RAM file removed: omcid then reads from
# the store what it already runs, and nothing is re-registered. LOID and
# LOID_PASSWD go to LOID_OLD and LOID_PASSWD_OLD as well, where the store
# has them, so the LOID rule of docs/SETTINGS.md answers the new values.
#
# A reboot during the trial undoes it: the RAM file is gone, and rcS and
# omcid read the store, which still holds the identity before the trial. An
# omcid respawn during the trial reads the RAM file and keeps the trial
# values. The RAM file holds the values themselves (mode 0600, root only, on
# a tmpfs); the state file and the log hold key names, never values.
#
# What the trial changed shows in status: before and after, the ONU state,
# the services, the MIB resets from the OLT and whether they were a loop,
# and after it the reload result and the deactivations omcid counted.
#
# A revert asked for while omcid is still re-registering waits for that
# re-registration to end (omcid finishes one before the next, at most its
# 150 s O5 wait). Every wait is bounded; every state change is one rename
# of the state file, under a mkdir lock.
set -u

# confd starts its children with no environment; export a PATH so that
# commands started through busybox timeout find /sbin and /usr/sbin too.
PATH=${PATH:+$PATH:}/sbin:/usr/sbin:/bin:/usr/bin
export PATH

FLASH=${FLASH:-/etc/scripts/flash}
APPLY=${APPLY:-/etc/scripts/apply.sh}
OMCLI=${OMCLI:-omcli}
SELF=${SELF:-/etc/scripts/identity-trial.sh}
STATE=${STATE:-/var/run/identity-trial}
TRIAL=${TRIAL:-/var/run/identity-trial.conf}
REQ=${REQ:-/var/run/identity-trial.req}
LOCKDIR=${LOCKDIR:-/var/run/identity-trial.lock}
RELOAD_STATUS=${RELOAD_STATUS:-/var/run/omcid-reload}
UPTIME=${UPTIME:-/proc/uptime}
KMSG=${KMSG:-/dev/kmsg}
LOG=${LOG:-/var/log/services.log}
SLEEP=${SLEEP:-sleep}
TIMEOUT=${TIMEOUT:-timeout}

APPLY_S=${APPLY_S:-170}   # apply.sh omci: omcid waits 150 s for O5, plus the hold
WATCH_S=${WATCH_S:-600}   # after a good result, the MIB reset loop window
POLL_S=${POLL_S:-5}       # how often the watch looks
IO_S=${IO_S:-5}           # one flash or omcli call
LOCK_S=${LOCK_S:-10}      # waiting for the lock

KEYS="GPON_SN GPON_PLOAM_PASSWD LOID LOID_PASSWD"

say() { echo "identity-trial: $*"; }
die() { echo "identity-trial: $*" >&2; exit 1; }

uptime_s() { cut -d. -f1 "$UPTIME" 2>/dev/null; }

# event NAME TEXT: one line to the kernel log (dmesg, the ramlog, and syslog
# through klogd), at notice, and to stdout. Key names only.
event() {
	name=$1
	shift
	line="event=$name keys=${keys:-} $*"
	printf '<5>identity-trial: %s\n' "$line" 2>/dev/null >> "$KMSG"
	say "$line"
}

# ---- the state file: phase (steady, applying, watching), the keys, the
# outcome, and the summary before and after.

clear_after() {
	outcome='' reason='' result='' refusals=''
	after_onu='' after_services='' after_olt_resets='' after_loop=''
}

load() {
	phase=steady keys='' watcher='' started=''
	before_onu='' before_services='' before_olt_resets='' before_loop=''
	clear_after
	[ -f "$STATE" ] || return 0
	while IFS='=' read -r k v; do
		case $k in
		phase) phase=$v ;;
		keys) keys=$v ;;
		watcher) watcher=$v ;;
		started) started=$v ;;
		outcome) outcome=$v ;;
		reason) reason=$v ;;
		result) result=$v ;;
		refusals) refusals=$v ;;
		before_onu) before_onu=$v ;;
		before_services) before_services=$v ;;
		before_olt_resets) before_olt_resets=$v ;;
		before_loop) before_loop=$v ;;
		after_onu) after_onu=$v ;;
		after_services) after_services=$v ;;
		after_olt_resets) after_olt_resets=$v ;;
		after_loop) after_loop=$v ;;
		esac
	done < "$STATE"
}

print_state() {
	echo "phase=$phase"
	echo "keys=$keys"
	echo "watcher=$watcher"
	echo "started=$started"
	echo "outcome=$outcome"
	echo "reason=$reason"
	echo "result=$result"
	echo "refusals=$refusals"
	echo "before_onu=$before_onu"
	echo "before_services=$before_services"
	echo "before_olt_resets=$before_olt_resets"
	echo "before_loop=$before_loop"
	echo "after_onu=$after_onu"
	echo "after_services=$after_services"
	echo "after_olt_resets=$after_olt_resets"
	echo "after_loop=$after_loop"
}

save() {
	print_state > "$STATE.new.$$" ||
		{ rm -f "$STATE.new.$$"; say "could not write $STATE" >&2; return 1; }
	mv "$STATE.new.$$" "$STATE" 2>/dev/null ||
		{ rm -f "$STATE.new.$$"; say "could not install $STATE" >&2; return 1; }
}

# ---- the lock: one state change at a time. mkdir is atomic; a holder that
# died is found by its pid and its lock taken over.

lock() {
	i=0
	while ! mkdir "$LOCKDIR" 2>/dev/null; do
		p=$(cat "$LOCKDIR/pid" 2>/dev/null)
		if [ -n "$p" ] && ! kill -0 "$p" 2>/dev/null; then
			rm -rf "$LOCKDIR"
			continue
		fi
		if [ "$i" -ge "$LOCK_S" ] && [ -z "$p" ]; then
			rm -rf "$LOCKDIR"
			i=0
			continue
		fi
		[ "$i" -lt "$LOCK_S" ] || die "busy: another identity-trial.sh holds $LOCKDIR"
		"$SLEEP" 1
		i=$((i + 1))
	done
	echo $$ > "$LOCKDIR/pid"
}
unlock() { rm -rf "$LOCKDIR"; }

# ---- what the line looks like, from omcid

# look: the ONU state, the services, the MIB resets from the OLT and whether
# they are a loop, from omcid, into look_onu, look_services, look_olt and
# look_loop. Unknown is empty.
look() {
	_st=$("$TIMEOUT" "$IO_S" "$OMCLI" state 2>/dev/null)
	look_onu=$(echo "$_st" | sed -n 's/^onu state  *: *//p' | sed -n 1p)
	look_olt=$(echo "$_st" | sed -n 's/^mib resets : olt \([0-9]*\) .*/\1/p' | sed -n 1p)
	look_loop=$(echo "$_st" | sed -n 's/^mib resets : .* loop \([a-z]*\) .*/\1/p' | sed -n 1p)
	look_services=$("$TIMEOUT" "$IO_S" "$OMCLI" conn 2>/dev/null | grep -c '^SERVID')
}
look_before() {
	look
	before_onu=$look_onu before_services=$look_services
	before_olt_resets=$look_olt before_loop=$look_loop
}
look_after() {
	look
	after_onu=$look_onu after_services=$look_services
	after_olt_resets=$look_olt after_loop=$look_loop
}

status_get() { sed -n "s/^$1=//p" "$RELOAD_STATUS" 2>/dev/null | sed -n 1p; }

# ---- the trial

# watcher_alive: the background watcher of this trial still runs.
watcher_alive() { [ -n "$watcher" ] && kill -0 "$watcher" 2>/dev/null; }

try() {
	confirm=
	if [ "${1:-}" = --confirm ]; then confirm=--confirm; shift; fi
	[ $# -gt 0 ] || die "usage: identity-trial.sh try [--confirm] KEY=VALUE..."
	for a in "$@"; do
		case $a in *=*) ;; *) die "$a is not KEY=VALUE" ;; esac
		k=${a%%=*} v=${a#*=}
		case " $KEYS " in *" $k "*) ;; *) die "$k is not an identity key ($KEYS)" ;; esac
		case $v in *"
"*) die "$k: a value cannot hold a newline" ;; esac
		# What the driver takes (gponsn): four characters, eight hex
		# digits. Anything else would leave the old serial in force.
		if [ "$k" = GPON_SN ]; then
			case $v in
			[A-Za-z0-9][A-Za-z0-9][A-Za-z0-9][A-Za-z0-9][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f]) ;;
			*) die "GPON_SN must be four letters or digits and eight hex digits" ;;
			esac
		fi
	done

	lock
	load
	if [ "$phase" != steady ] && watcher_alive; then
		unlock
		die "a trial is already running ($phase, keys $keys); keep or revert it first"
	fi
	keys=
	for a in "$@"; do
		case " $keys " in *" ${a%%=*} "*) ;; *) keys="${keys:+$keys,}${a%%=*}" ;; esac
	done
	look_before
	clear_after
	started=$(uptime_s)
	# The values: RAM only, readable by root alone.
	( umask 077; for a in "$@"; do printf '%s\n' "$a"; done > "$TRIAL.new.$$" ) &&
		mv "$TRIAL.new.$$" "$TRIAL" ||
		{ rm -f "$TRIAL.new.$$"; unlock; die "could not write $TRIAL"; }
	rm -f "$REQ"
	phase=applying
	watcher=
	save
	setsid sh "$SELF" _watch $confirm < /dev/null >> "$LOG" 2>&1 &
	watcher=$!
	save
	unlock
	event identity_trial "state=start before_onu=$before_onu before_services=$before_services"
	say "omcid re-registers with the new values now; the line drops for a while. identity-trial.sh status shows the outcome"
}

# finish_revert REASON: back to the store. The RAM file goes first, so the
# reload finds the stored identity; --revert, so the guard lets it through.
finish_revert() {
	reason=$1
	rm -f "$TRIAL"
	event identity_trial "state=revert reason=$reason"
	"$APPLY" omci --revert "$APPLY_S" || say "the revert did not end well: see apply.sh output above and /var/log/omcid.log" >&2
	_reason=$reason
	lock
	load
	look_after
	reason=$_reason
	phase=steady
	outcome=reverted
	save
	unlock
	event identity_trial "state=reverted reason=$reason after_onu=$after_onu after_services=$after_services"
}

# store_put KEY VALUE: one flash set, bounded.
store_put() {
	"$TIMEOUT" "$IO_S" "$FLASH" set "$1" "$2" > /dev/null 2>&1
}
store_has() {
	"$TIMEOUT" "$IO_S" "$FLASH" get "$1" 2>/dev/null | grep -q "^$1="
}

# finish_keep: the values into the store, then the RAM file away. A key the
# store refuses leaves the RAM file in place, so omcid keeps answering what
# the OLT took, and says so: the next reboot would read the store.
finish_keep() {
	bad=
	while IFS= read -r a; do
		k=${a%%=*} v=${a#*=}
		store_put "$k" "$v" || bad="$bad $k"
		case $k in
		LOID|LOID_PASSWD)
			if store_has "${k}_OLD"; then store_put "${k}_OLD" "$v" || bad="$bad ${k}_OLD"; fi ;;
		esac
	done < "$TRIAL"
	lock
	load
	look_after
	phase=steady
	if [ -n "$bad" ]; then
		outcome=store_failed
		save
		unlock
		event identity_trial "state=store_failed failed=$(echo $bad | tr ' ' ',')"
		say "the OLT took the new identity but the store refused$bad: omcid keeps the trial values until a reboot, which reads the store. Set them by hand" >&2
		return 1
	fi
	rm -f "$TRIAL"
	outcome=kept
	save
	unlock
	event identity_trial "state=kept after_onu=$after_onu after_services=$after_services"
}

# take_req: a keep or revert asked for during the watch, or nothing.
take_req() {
	r=$(cat "$REQ" 2>/dev/null)
	rm -f "$REQ"
	echo "$r"
}

# _watch [--confirm]: the background half. Applies, judges, watches, ends.
watch() {
	before=$(status_get id)
	"$APPLY" omci ${1:+"$1"} "$APPLY_S"
	lock
	load
	unlock
	# apply.sh reports one reload; make sure it is a new one.
	if [ "$(status_get id)" = "$before" ]; then
		finish_revert no_answer
		return
	fi
	res=$(status_get result)
	changed=$(status_get changed)
	lock
	load
	result=$res refusals=$(status_get refusals)
	save
	unlock
	case $changed:$res in
	none:ok)
		finish_keep
		return ;;
	identity:held)
		# omcid did not re-register: it runs what it ran, and the
		# store holds that too. Nothing to undo but the RAM file.
		rm -f "$TRIAL"
		lock
		load
		phase=steady
		outcome=held
		save
		unlock
		event identity_trial "state=held"
		return ;;
	identity:ok) ;;
	*)
		finish_revert "$res"
		return ;;
	esac

	[ "$(take_req)" != revert ] || { finish_revert asked; return; }
	lock
	load
	phase=watching
	save
	unlock
	event identity_trial "state=watching window_s=$WATCH_S"
	waited=0
	while [ "$waited" -lt "$WATCH_S" ]; do
		"$SLEEP" "$POLL_S"
		waited=$((waited + POLL_S))
		case $(take_req) in
		revert) finish_revert asked; return ;;
		keep)   break ;;
		esac
		look
		if [ "$look_loop" = yes ] && [ "$before_loop" = no ]; then
			finish_revert mib_reset_loop
			return
		fi
	done
	finish_keep
}

# ask VERB: hand keep or revert to the watcher, or do it here when no
# watcher runs any more (it was killed, or the stick lost it).
ask() {
	lock
	load
	unlock
	[ "$phase" != steady ] || die "no trial is running"
	if watcher_alive; then
		echo "$1" > "$REQ"
		if [ "$1" = keep ] && [ "$phase" != watching ]; then
			rm -f "$REQ"
			die "not yet: the ONU is not back in O5 with services ($phase)"
		fi
		say "$1 handed to the trial (pid $watcher); identity-trial.sh status shows when it is done"
		return 0
	fi
	case $1 in
	revert) finish_revert asked ;;
	keep)
		[ "$phase" = watching ] || die "not yet: the ONU is not back in O5 with services ($phase)"
		finish_keep ;;
	esac
}

status() {
	load
	if [ "$phase" != steady ] && ! watcher_alive; then
		echo "warning=the trial watcher is gone: identity-trial.sh revert or keep"
	fi
	if [ -f "$TRIAL" ]; then echo "trial_file=present"; else echo "trial_file=absent"; fi
	print_state
}

case "${1:-}" in
try)    shift; try "$@" ;;
keep)   ask keep ;;
revert) ask revert ;;
status) status ;;
_watch) shift; watch "$@" ;;
*)      echo "usage: $0 try [--confirm] KEY=VALUE...|keep|revert|status" >&2; exit 1 ;;
esac
