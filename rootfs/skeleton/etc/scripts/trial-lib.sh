# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

# shellcheck shell=sh
# The state variables (phase, trial_id, ev ...) are set and read across this
# file and the script that sources it, which shellcheck cannot follow alone.
# shellcheck disable=SC2034,SC2154
#
# trial-lib.sh -- the trial machinery shared by host-autoneg.sh,
# host-serdes.sh and mgmt-addr.sh, sourced by each. Split out so the three
# see the same lock, state file, event line and trial run rather than three
# copies drifting apart (the same reason as rcs-lib.sh).
#
# The sourcing script sets, before it sources this file:
#
#     TAG          the name in front of every event line (host-autoneg)
#     LOCK_OWNER   who lock names when it gives up (host-autoneg.sh)
#     LOCK_STRICT  set: lock also gives up when the pid file cannot be
#                  written (host-serdes.sh, as before trial-lib.sh)
#     STATE        the state file; STATE_KEYS, its keys in file order, each
#                  KEY (held in the variable of that name) or KEY:VAR
#     LOCKDIR LOCK_S SLEEP UPTIME KMSG LOG SELF, as their comments there say
#
# and defines say, die, load (its defaults, then state_load), event_fields
# (the fields every event line carries after its name), back_to_prev NAME
# WHY [defer] (its status is that of expire and revert), and for trial_run
# the hooks trial_apply, trial_applied and trial_apply_failed.

uptime_s() { cut -d. -f1 "$UPTIME" 2>/dev/null; }

# event NAME TEXT: one line to the kernel log (dmesg, the ramlog, and syslog
# through klogd), at notice, and to stdout.
event() {
	name=$1
	shift
	line="event=$name $(event_fields) $*"
	printf '<5>%s: %s\n' "$TAG" "$line" 2>/dev/null >> "$KMSG"
	say "$line"
	ev=$name
	ev_up=$(uptime_s)
	ev_msg=$*
}

# put_in_place FILE: puts FILE.new.$$ in place of FILE in one rename on the same
# filesystem, so a reader or a power cut sees the old file or the new one.
put_in_place() {
	mv "$1.new.$$" "$1" 2>/dev/null || { rm -f "$1.new.$$"; return 1; }
}

# ---- the state file: KEY=VALUE lines, in STATE_KEYS order

state_save() {
	{
		for _kv in $STATE_KEYS; do
			_k=${_kv%%:*} _var=${_kv#*:}
			eval "echo \"\$_k=\${$_var:-}\""
		done
	} > "$STATE.new.$$" || { rm -f "$STATE.new.$$"; say "could not write $STATE" >&2; return 1; }
	put_in_place "$STATE" || { say "could not install $STATE" >&2; return 1; }
}

# state_load: the keys STATE holds, over the defaults load set. A key not in
# STATE_KEYS is ignored.
state_load() {
	[ -f "$STATE" ] || return 0
	while IFS='=' read -r _k _v; do
		for _kv in $STATE_KEYS; do
			[ "${_kv%%:*}" = "$_k" ] || continue
			eval "${_kv#*:}=\$_v"
			break
		done
	done < "$STATE"
}

# ---- the lock: one state change at a time. mkdir is atomic; a holder that
# died is found by its pid and its lock taken over. lock_try returns 1 when
# the lock cannot be had within LOCK_S, or cannot be made at all, and 2 when
# it holds the lock but could not write the pid file; lock exits on 1, and
# on 2 only with LOCK_STRICT.

lock_try() {
	i=0
	while ! mkdir "$LOCKDIR" 2>/dev/null; do
		[ -d "$LOCKDIR" ] || [ -d "${LOCKDIR%/*}" ] || return 1
		p=$(cat "$LOCKDIR/pid" 2>/dev/null)
		if [ -n "$p" ] && ! kill -0 "$p" 2>/dev/null; then
			rm -rf "$LOCKDIR"
			continue
		fi
		# A holder killed between its mkdir and its pid write leaves no
		# pid: after the wait, that lock is stale too.
		if [ "$i" -ge "$LOCK_S" ] && [ -z "$p" ]; then
			rm -rf "$LOCKDIR"
			i=0
			continue
		fi
		[ "$i" -lt "$LOCK_S" ] || return 1
		"$SLEEP" 1
		i=$((i + 1))
	done
	echo $$ > "$LOCKDIR/pid" || return 2
}
lock() {
	lock_try
	_rc=$?
	[ "$_rc" = 0 ] && return 0
	[ "$_rc" = 2 ] && [ -z "${LOCK_STRICT:-}" ] && return 0
	die "busy: another $LOCK_OWNER holds $LOCKDIR"
}
unlock() { rm -rf "$LOCKDIR"; }

# spawn VERB ARGS...: this script again, detached, so it outlives the caller
# (rcS, network.sh, confd). Its output goes to the services log.
spawn() {
	setsid sh "$SELF" "$@" < /dev/null >> "$LOG" 2>&1 &
}

# ---- the trial

# trial-run ID SECONDS: one second for the caller answer to leave, the
# apply (trial_apply), then the window (trial_window), then the expiry.
# trial_applied and trial_apply_failed record the outcome, under the lock.
# trial_window ID SECONDS sleeps the window; a script that has to act
# during it (host-serdes.sh) defines its own after sourcing this file.
trial_window() { "$SLEEP" "$2"; }
trial_run() {
	id=$1 secs=$2
	"$SLEEP" 1
	lock
	load
	if [ "$phase" != trial ] || [ "$trial_id" != "$id" ]; then
		unlock
		return 0
	fi
	if trial_apply; then
		trial_applied
	else
		trial_apply_failed
		unlock
		return 1
	fi
	unlock
	trial_window "$id" "$secs"
	expire "$id"
}

expire() {
	id=$1
	lock
	load
	_rc=0
	if [ "$phase" = trial ] && [ "$trial_id" = "$id" ]; then
		back_to_prev trial_expired "not confirmed in time" || _rc=1
	fi
	unlock
	return "$_rc"
}

# revert: ends the running trial on request. REVERT_HOW is passed on to
# back_to_prev (mgmt-addr.sh sets defer).
revert() {
	lock
	load
	if [ "$phase" != trial ]; then
		unlock
		die "no trial is running"
	fi
	_rc=0
	back_to_prev trial_reverted "reverted on request" ${REVERT_HOW:+"$REVERT_HOW"} || _rc=1
	unlock
	return "$_rc"
}
