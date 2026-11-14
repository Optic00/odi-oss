#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

#
# rootfs/skeleton/etc/scripts/apply.sh, off the device.
#
# apply.sh omci sends SIGHUP to the running omcid and reads the outcome from
# the status file omcid writes. omcid here is a stub that traps SIGHUP and
# writes the file the way src/omci/respond/reload.c does (its real behaviour
# is src/omci/reload-test.sh). What is checked is the script: it never starts
# an omcid, never touches the driver, sends the signal to the pid pidof
# names, waits for a NEW run (a stale status file answers nothing), reports
# each outcome, and is bounded when the daemon is silent or slow.
set -u
cd "$(dirname "$0")/.." || exit 1
A=$PWD/rootfs/skeleton/etc/scripts/apply.sh
T=$(mktemp -d)
pass=0; fail=0
t() {
	if printf '%s' "$3" | grep -q -- "$2"; then echo "ok    $1"; pass=$((pass + 1))
	else echo "FAIL  $1"; echo "        wanted /$2/, got:"; printf '%s\n' "$3" | sed 's/^/        /'; fail=$((fail + 1)); fi
}
mkdir -p "$T/bin"
cleanup() { [ -f "$T/omcid.pid" ] && kill "$(cat "$T/omcid.pid")" 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT

# pidof: the stub daemon, while it lives. A plain shell cannot name its own
# processes, so the stub keeps its pid in a file.
cat > "$T/bin/pidof" <<PIDOF
#!/bin/sh
[ -f "$T/omcid.pid" ] && p=\$(cat "$T/omcid.pid") && kill -0 "\$p" 2>/dev/null && { echo "\$p"; exit 0; }
exit 1
PIDOF
# setsid is how the old script started its own omcid; it logs itself and
# runs the command, so the network apply further down still works.
cat > "$T/bin/setsid" <<SETSID
#!/bin/sh
echo "setsid \$*" >> "$T/started"
exec "\$@"
SETSID
chmod +x "$T/bin/pidof" "$T/bin/setsid"

# The stub daemon. On SIGHUP it writes what $T/script says: one line per
# phase, "state changed action result keys duration_ms o5_ms services delay
# [extra]", delay in seconds before the line is written, extra more key=value
# lines joined by semicolons. The confirmation apply.sh leaves is taken away,
# as omcid does, and kept in confirm.seen for the check. No script: it ignores the
# signal, as an older build that does not write a status file would.
cat > "$T/daemon" <<DAEMON
#!/bin/bash
echo \$\$ > "$T/omcid.pid"
n=0
trap 'reload' HUP
reload() {
	[ -f "$T/script" ] || return
	n=\$((n + 1))
	rm -f "$T/confirm.seen"
	[ -f "$T/confirm" ] && mv "$T/confirm" "$T/confirm.seen"
	while read -r state changed action result keys dur o5 svc delay extra; do
		[ "\$delay" -gt 0 ] && sleep "\$delay"
		{
			echo "id=\$\$.\$n"; echo "state=\$state"; echo "changed=\$changed"
			echo "action=\$action"; echo "result=\$result"; echo "keys=\$keys"
			echo "duration_ms=\$dur"
			[ "\$o5" != - ] && echo "o5_ms=\$o5"
			echo "services=\$svc"
			[ -n "\$extra" ] && echo "\$extra" | tr ';' '\n'
		} > "$T/status.new"
		mv "$T/status.new" "$T/status"
	done < "$T/script"
}
# The sleep in the background and a wait on it: bash runs a trap only once
# the foreground command ends, and wait returns at the signal, so the reload
# starts at once instead of up to a second later.
while :; do sleep 1 & wait \$!; done
DAEMON
chmod +x "$T/daemon"
"$T/daemon" > /dev/null 2>&1 &
# Up to 10 s for the stub to say its pid, not a fixed second.
for _ in $(seq 100); do [ -s "$T/omcid.pid" ] && break; sleep 0.1; done
first=$(cat "$T/omcid.pid")

# Bounds well past what a case needs, so a busy runner does not fail one: a
# case that ends returns as soon as it does. The two cases that test a bound
# set it small themselves (ACK for the acknowledgement, the argument for the
# outcome).
run() {
	env PATH="$T/bin:$PATH" RELOAD_STATUS="$T/status" MODULES_OFF="$T/modules.off" REREG_CONFIRM="$T/confirm" \
	    ACK_WAIT="${ACK:-15}" APPLY_WAIT=30 NETWORK="$T/network.sh" SERVICES_LOG="$T/services.log" \
	    sh "$A" "$@" 2>&1
}
: > "$T/started"

# Nothing changed.
echo "done none none ok - 12 - 6 0" > "$T/script"
out=$(run omci); rc=$?
t "unchanged: exit 0" "^0$" "$rc"
t "unchanged: says nothing was done" "nothing changed" "$out"
t "the signal went to the running omcid" "told to reload" "$out"

# A VLAN-only change.
printf 'running vlan rebuild pending VLAN_MANU_TAG_VID 0 - 6 0\ndone vlan rebuild ok VLAN_MANU_TAG_VID 400 - 6 0\n' > "$T/script"
out=$(run omci); rc=$?
t "vlan: exit 0" "^0$" "$rc"
t "vlan: rebuilt in place, and the ONU stayed" "VLAN handling changed (VLAN_MANU_TAG_VID): connections rebuilt in place in 0.4 s, the ONU stayed in O5" "$out"
t "vlan: still the same omcid" "^$first$" "$(cat "$T/omcid.pid")"

# A re-registration that finishes in time.
printf 'running identity reregister pending GPON_SN 100 - 0 0\ndone identity reregister ok GPON_SN,LOID 2600 2100 6 1\n' > "$T/script"
out=$(run omci); rc=$?
t "identity: exit 0" "^0$" "$rc"
t "identity: back in O5 with services, and how long" "back in O5 with 6 service(s) after 2.1 s" "$out"

# A failure is reported and exits 1.
for r in failed timeout refused no_services sn_not_applied; do
	printf 'done identity reregister %s GPON_SN 900 - 0 0\n' "$r" > "$T/script"
	out=$(run omci); rc=$?
	t "$r: exit 1" "^1$" "$rc"
	t "$r: named" "identity (GPON_SN): $r" "$out"
done

# The re-registration guard (src/omci/respond/rereg.c). Unconfirmed, no
# confirmation is left for omcid; --confirm and --revert leave theirs.
echo "done none none ok - 12 - 6 0" > "$T/script"
run omci > /dev/null
t "no flag: no confirmation reaches omcid" "^no$" "$([ -e "$T/confirm.seen" ] && echo yes || echo no)"
run omci --confirm > /dev/null
t "--confirm: omcid finds it" "^confirm$" "$(cat "$T/confirm.seen" 2>/dev/null)"
out=$(run omci --revert 2); rc=$?
t "--revert: omcid finds it" "^revert$" "$(cat "$T/confirm.seen" 2>/dev/null)"
t "--revert: the bound after it still counts" "^0$" "$rc"
echo "done identity held held GPON_SN 5 - 6 0 refusals=0;streak=2" > "$T/script"
out=$(run omci); rc=$?
t "held: exit 1" "^1$" "$rc"
t "held: says what to do" "NOT applied: the OLT refused the last 2 changes.*apply.sh omci --confirm" "$out"
echo "done identity reregister ok GPON_SN 2600 2100 6 0 refusals=1;streak=0;today=5" > "$T/script"
out=$(run omci); rc=$?
t "accepted after a deactivation: exit 0, no refusal warning" "^0$" "$rc"
t "five accepted in a day: a warning" "warning: 5 identity changes in the last 24 h" "$out"
echo "done identity reregister timeout GPON_SN 2600 - 0 0 refusals=0;streak=1;today=0" > "$T/script"
out=$(run omci)
t "one refused: a warning" "warning: the OLT refused the last 1 identity change" "$out"

# Still running at the bound: reported, not a failure.
printf 'running identity reregister activated GPON_SN 100 - 0 0\n' > "$T/script"
out=$(run omci 2); rc=$?
t "running at the bound: exit 0" "^0$" "$rc"
t "running at the bound: says it carries on" "still working after 2 s" "$out"

# A stale status file is not an answer: the new run has to have a new id.
echo "done vlan rebuild ok VLAN_MANU_TAG_VID 100 - 0 0" > "$T/script"
cp "$T/status" "$T/status.old"
rm -f "$T/script"
out=$(ACK=2 run omci); rc=$?
t "an omcid that never answers: exit 1" "^1$" "$rc"
t "and it says why" "did not answer the signal" "$out"

# No omcid at all.
kill "$(cat "$T/omcid.pid")"; wait 2>/dev/null; rm -f "$T/omcid.pid"
out=$(run omci); rc=$?
t "no omcid: exit 1" "^1$" "$rc"
t "no omcid: says so" "no omcid is running" "$out"
touch "$T/modules.off"
out=$(run omci); rc=$?
t "modules.off: exit 1, says why" "modules.off.*nothing to reload" "$out"
rm -f "$T/modules.off"

t "apply omci never started a process of its own (no setsid, no omcid)" "^0$" "$(wc -c < "$T/started" | tr -d ' ')"

cat > "$T/network.sh" <<'NET'
#!/bin/sh
echo "network.sh $*" >> "$STUBLOG"
NET
chmod +x "$T/network.sh"
export STUBLOG="$T/netcalls"
out=$(run network); rc=$?
# The apply is detached and delayed by one second: poll (bounded, 10 s) for
# its second call instead of sleeping a fixed time.
_i=0
while [ "$(wc -l < "$T/netcalls" 2>/dev/null | tr -d ' ')" -lt 2 ] && [ "$_i" -lt 100 ]; do sleep 0.1; _i=$((_i + 1)); done
t "apply network prints the plan first" "^network.sh addr -n" "$(sed -n 1p "$T/netcalls")"
t "then applies it, detached" "^network.sh addr$" "$(sed -n 2p "$T/netcalls")"
t "and returns at once" "applying in one second" "$out"

# A disabled service has no daemon pid. All anonymous placeholders must
# be woken, rather than selecting an unrelated first match. No real pid
# is signalled: the shell builtin is replaced in the child shell.
respawn_run() {
    env PATH="$T/bin:$PATH" APPLY_SCRIPT="$A" APPLY_ACTION="${1:-igmp}" bash -c '
        pidof() { printf "%s\n" "${DAEMON_PIDS:-}"; }
        pgrep() { printf "%s\n" "${OFF_PIDS:-}"; }
        kill() {
            if [ "$1" = -0 ]; then return "${KILL_PROBE_RC:-0}"; fi
            printf "signal %s\n" "$*"; return "${KILL_RC:-0}";
        }
        . "$APPLY_SCRIPT" "$APPLY_ACTION"
'
}
out=$(OFF_PIDS=$'1111\n2222' respawn_run)
t "disabled apply wakes both anonymous placeholders" "signal 1111.*2222" "$(echo "$out" | tr '\n' ' ')"
t "disabled apply reports placeholders honestly" "placeholders" "$out"
out=$(DAEMON_PIDS='3333 4444' OFF_PIDS=1111 respawn_run)
t "active apply signals every daemon pid separately" "signal 3333.*signal 4444" "$(echo "$out" | tr '\n' ' ')"
t "active apply leaves placeholders alone" "^0$" "$(echo "$out" | grep -c 'signal 1111')"
out=$(OFF_PIDS=2222 KILL_RC=1 respawn_run); rc=$?
t "failed placeholder signal fails apply" "^1$" "$rc"
out=$(OFF_PIDS=2222 KILL_RC=1 KILL_PROBE_RC=1 respawn_run); rc=$?
t "a placeholder that exited before the signal is harmless" "^0$" "$rc"
out=$(respawn_run)
t "no matching process is reported" "nothing to restart" "$out"

t "an unknown verb is refused" "usage" "$(run bogus)"

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
