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
# shellcheck source=test/host-flock.sh
. test/host-flock.sh
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
# [extra]" (keys - for none), delay in seconds before the line is written,
# extra more key=value lines joined by semicolons. The confirmation apply.sh
# leaves is taken away, as omcid does, and kept in confirm.seen for the
# check. No script: it ignores the signal, as an older build that does not
# write a status file would.
cat > "$T/daemon" <<DAEMON
#!/bin/bash
echo \$\$ > "$T/omcid.pid"
n=0
trap 'reload' HUP
reload() {
	[ -f "$T/script" ] || return
	n=\$((n + 1))
	# Read the script once, before the first line is acted on. A HUP that
	# arrives while a reload still runs nests this function, and the outer
	# loop would go on reading the file the test has meanwhile rewritten
	# for the next case, from its old offset: the tail of a new line becomes
	# a phase of its own (state=6 changed=1) and replaces the done status.
	local script
	script=\$(cat "$T/script")
	rm -f "$T/confirm.seen"
	[ -f "$T/confirm" ] && mv "$T/confirm" "$T/confirm.seen"
	while read -r state changed action result keys dur o5 svc delay extra; do
		[ "\$keys" = - ] && keys=
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
	done <<< "\$script"
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
	env PATH="$T/bin:$PATH" RELOAD_STATUS="$T/status" MODULES_OFF="$T/modules.off" REREG_CONFIRM="$T/confirm" OMCI_LOCK="$T/omci.lock" \
	    ACK_WAIT="${ACK:-15}" APPLY_WAIT=30 LINGER_MAX="${LINGER:-180}" NETWORK="$T/network.sh" SERVICES_LOG="$T/services.log" \
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
t "vlan: rebuilt in place, with no claim about the ONU state" "VLAN handling changed (VLAN_MANU_TAG_VID): connections rebuilt in place in 0.4 s, the ONU was not re-registered" "$out"
t "vlan: still the same omcid" "^$first$" "$(cat "$T/omcid.pid")"

# A re-registration that finishes in time.
printf 'running identity reregister pending GPON_SN 100 - 0 0\ndone identity reregister ok GPON_SN,LOID 2600 2100 6 1\n' > "$T/script"
out=$(run omci); rc=$?
t "identity: exit 0" "^0$" "$rc"
t "identity: back in O5 with services, and how long" "back in O5 with 6 service(s) after 2.1 s" "$out"

# A failure is reported and exits 1.
for r in failed timeout refused no_services sn_not_applied pw_not_applied; do
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
# omci-live (the web UI after a VLAN save): omcid finds it, and a deferred
# identity change is a failure that says how to apply it.
echo "done identity deferred deferred GPON_SN 5 - 6 0" > "$T/script"
out=$(run omci-live); rc=$?
t "omci-live: omcid finds it" "^live$" "$(cat "$T/confirm.seen" 2>/dev/null)"
t "deferred: exit 3, not a failure's 1" "^3$" "$rc"
t "deferred: names the keys and the plain apply" "identity changed (GPON_SN) and NOT applied, nor anything else.*Apply it with: apply.sh omci" "$out"
# What is not understood is refused before anything is signalled. omci-live
# is a verb because an option was taken for the seconds by an apply.sh that
# did not know it, and the whole store applied.
cp "$T/status" "$T/status.before"
out=$(run omci --bogus 2>&1); rc=$?
t "an unknown option is refused" "^1 .*unknown option --bogus" "$rc $out"
out=$(run omci --live 2>&1); rc=$?
t "omci --live is not an option: refused" "^1 .*unknown option --live" "$rc $out"
out=$(run omci-live --confirm 2>&1); rc=$?
t "omci-live takes no confirmation" "^1 .*unknown option --confirm for omci-live" "$rc $out"
out=$(run omci soon 2>&1); rc=$?
t "seconds that are no number: refused" "^1 .*whole number, not soon" "$rc $out"
out=$(run omci 0 2>&1); rc=$?
t "a bound of 0 s: refused" "^1 .*at least 1 s, not 0" "$rc $out"
out=$(run omci 00 2>&1); rc=$?
t "a bound of 00 s: refused" "^1 .*at least 1 s, not 00" "$rc $out"
t "and none of those signalled omcid" "^same$" "$(cmp -s "$T/status" "$T/status.before" && echo same)"

# One reload signalled at a time: the confirmation file is shared. Another
# apply.sh holding the lock past ACK_WAIT is a refusal, with nothing written
# or signalled.
echo "done vlan rebuild ok VLAN_MANU_TAG_VID 100 - 0 0" > "$T/script"
flock "$T/omci.lock" sleep 4 &
holder=$!
for _ in $(seq 50); do flock -n "$T/omci.lock" true || break; sleep 0.1; done
echo revert > "$T/confirm"
out=$(ACK=1 run omci --confirm); rc=$?
t "lock held by another apply.sh: exit 1, says so" "^1 .*another apply.sh omci is still waiting" "$rc $out"
t "and the other one's confirmation is left alone" "^revert$" "$(cat "$T/confirm" 2>/dev/null)"
t "and omcid was not signalled" "^same$" "$(cmp -s "$T/status" "$T/status.before" && echo same)"
wait "$holder"
rm -f "$T/confirm"
out=$(run omci --confirm); rc=$?
t "the lock free again: the reload runs" "^0$" "$rc"
t "and reports it" "connections rebuilt in place" "$out"
echo "done identity reregister ok GPON_SN 2600 2100 6 0 refusals=1;streak=0;today=5" > "$T/script"
out=$(run omci); rc=$?
t "accepted after a deactivation: exit 0, no refusal warning" "^0$" "$rc"
t "five accepted in a day: a warning" "warning: 5 identity changes in the last 24 h" "$out"
echo "done identity reregister timeout GPON_SN 2600 - 0 0 refusals=0;streak=1;today=0" > "$T/script"
out=$(run omci)
t "one refused: a warning" "warning: the OLT refused the last 1 identity change" "$out"

# Once omcid acknowledged a reload, another apply.sh may start the next one.
# The outcome reported is the one of this run, or none: never the result of
# the next run.
printf 'running vlan rebuild pending VLAN_MANU_TAG_VID 0 - 6 0\n' > "$T/script"
before=$(sed -n 's/^id=//p' "$T/status")
run omci 10 > "$T/out.overtaken" &
runner=$!
# Acknowledged: a new id, and the lock released for the next apply.sh.
for _ in $(seq 100); do
	[ "$(sed -n 's/^id=//p' "$T/status")" != "$before" ] && flock -n "$T/omci.lock" true && break
	sleep 0.1
done
printf 'id=next.1\nstate=done\nchanged=identity\naction=reregister\nresult=ok\n' > "$T/status.new"
mv "$T/status.new" "$T/status"
wait "$runner"; rc=$?
t "overtaken by the next reload: exit 1, says so, not its ok" \
	"^1 .*started the next reload before the outcome of this one was read" "$rc $(cat "$T/out.overtaken" | tr '\n' ' ')"

# Still running at the bound: reported, not a failure.
printf 'running identity reregister activated GPON_SN 100 - 0 0\n' > "$T/script"
out=$(run omci 2); rc=$?
t "running at the bound: exit 0" "^0$" "$rc"
t "running at the bound: says it carries on" "still working after 2 s" "$out"

# While that reload runs, omcid would take a signal only once it is done,
# up to 153 s on: refused, with nothing written or signalled.
cp "$T/status" "$T/status.before"
echo confirm > "$T/confirm.other"
out=$(run omci --confirm); rc=$?
t "a reload still running: the next one is refused, exit 1" "^1 .*still applying the last change (identity, activated)" "$rc $out"
t "and omcid was not signalled" "^same$" "$(cmp -s "$T/status" "$T/status.before" && echo same)"
t "and no confirmation was written" "^no$" "$([ -e "$T/confirm" ] && echo yes || echo no)"
t "and the lock is free" "^free$" "$(flock -n "$T/omci.lock" true && echo free)"
# A status that says running for an omcid that no longer lives is no
# reload: the one that died mid-reload left it, and the signal goes out.
sed "s/^id=.*/id=999999.4/" "$T/status.before" > "$T/status"
echo "done vlan rebuild ok VLAN_MANU_TAG_VID 400 - 6 0" > "$T/script"
out=$(run omci); rc=$?
t "running from an omcid that is gone: not refused, the reload runs" "^0 .*connections rebuilt in place" "$rc $(printf '%s\n' "$out" | tr '\n' ' ')"

# A reload that re-registers only because the last one left the ONU off the
# line (retry=1): reported as a re-registration, and omci-live exits 4.
echo "done identity reregister ok VLAN_MANU_TAG_VID 2600 2100 6 0 retry=1" > "$T/script"
out=$(run omci-live); rc=$?
t "retry, omci-live: exit 4" "^4$" "$rc"
t "retry: says why it re-registers" "the last reload left the ONU off the line: this one re-registers it (VLAN_MANU_TAG_VID)" "$out"
t "retry: and that it is back" "re-registered: the ONU is back in O5 with 6 service(s) after 2.1 s" "$out"
t "retry: not reported as an identity change" "^0$" "$(printf '%s\n' "$out" | grep -c 'identity changed')"
out=$(run omci); rc=$?
t "retry, plain omci: exit 0" "^0$" "$rc"
printf 'running identity reregister activated - 100 - 0 0 retry=1\n' > "$T/script"
out=$(run omci-live 2); rc=$?
t "retry still running at the bound, omci-live: exit 4" "^4$" "$rc"
t "retry with no keys: none named" "re-registers it$" "$(printf '%s\n' "$out" | grep 'off the line')"
sed 's/^state=running$/state=done/; s/^result=activated$/result=timeout/' "$T/status" > "$T/status.new"
mv "$T/status.new" "$T/status"
echo "done identity reregister failed - 900 - 0 0 retry=1" > "$T/script"
out=$(run omci-live); rc=$?
t "retry that fails: exit 1, no empty key list" "^1 .*identity: failed after 0.9 s" "$rc $(printf '%s\n' "$out" | tr '\n' ' ')"

# The seconds spent waiting for the lock count against the bound: a lock
# held 3 s and a bound of 4 s end after about 4 s, not 7. The bound caps the
# lock wait itself too: held past it, the run ends there, with nothing
# signalled.
flock "$T/omci.lock" sleep 4 &
holder=$!
for _ in $(seq 50); do flock -n "$T/omci.lock" true || break; sleep 0.1; done
cp "$T/status" "$T/status.before"
s0=$(date +%s)
out=$(ACK=5 run omci 2); rc=$?
took=$(( $(date +%s) - s0 ))
wait "$holder"
t "a lock held past a bound of 2 s: exit 1 after about 2 s ($took s)" "^1 [1-3] .*still waiting" "$rc $took $out"
t "and omcid was not signalled" "^same$" "$(cmp -s "$T/status" "$T/status.before" && echo same)"
printf 'running vlan rebuild pending VLAN_MANU_TAG_VID 0 - 6 0\n' > "$T/script"
flock "$T/omci.lock" sleep 3 &
holder=$!
for _ in $(seq 50); do flock -n "$T/omci.lock" true || break; sleep 0.1; done
s0=$(date +%s)
out=$(ACK=5 run omci 4); rc=$?
took=$(( $(date +%s) - s0 ))
wait "$holder"
t "the lock wait counts: still working after 4 s" "^0 .*still working after 4 s" "$rc $(printf '%s\n' "$out" | tr '\n' ' ')"
t "and it took about 4 s, not 7 ($took s)" "^[3-5]$" "$took"
sed 's/^state=running$/state=done/; s/^result=pending$/result=ok/' "$T/status" > "$T/status.new"
mv "$T/status.new" "$T/status"

# A stale status file is not an answer: the new run has to have a new id.
echo "done vlan rebuild ok VLAN_MANU_TAG_VID 100 - 0 0" > "$T/script"
cp "$T/status" "$T/status.old"
rm -f "$T/script"
out=$(ACK=2 run omci); rc=$?
t "an omcid that never answers: exit 1" "^1$" "$rc"
t "and it says why" "did not answer the signal" "$out"
# Its signal may still be taken, and the reload it starts would take the
# confirmation on disk then. Until omcid answers it, the lock stays held.
out=$(ACK=1 run omci --confirm); rc=$?
t "after an unanswered signal the lock is still held: exit 1" "^1 .*another apply.sh omci is still waiting" "$rc $out"
t "and no confirmation is left for that reload to take" "^no$" "$([ -e "$T/confirm" ] && echo yes || echo no)"
# A new run of an omcid it did not signal is not that answer.
printf 'id=999999.1\nstate=done\n' > "$T/status.new"
mv "$T/status.new" "$T/status"
sleep 2
t "a run of another omcid does not free the lock" "^held$" "$(flock -n "$T/omci.lock" true && echo free || echo held)"
printf 'id=%s.90\nstate=done\n' "$(cat "$T/omcid.pid")" > "$T/status.new"
mv "$T/status.new" "$T/status"
for _ in $(seq 50); do flock -n "$T/omci.lock" true && break; sleep 0.1; done
t "once omcid answers it, late, the lock is free" "^free$" "$(flock -n "$T/omci.lock" true && echo free)"

# Nor is such a run the acknowledgement of the signal.
( sleep 0.5; printf 'id=999999.2\nstate=done\n' > "$T/status.new"; mv "$T/status.new" "$T/status" ) &
out=$(ACK=2 run omci); rc=$?
wait $!
t "a run of another omcid is no acknowledgement: exit 1" "^1 .*did not answer the signal" "$rc $(printf '%s\n' "$out" | tr '\n' ' ')"
printf 'id=%s.91\nstate=done\n' "$(cat "$T/omcid.pid")" > "$T/status.new"
mv "$T/status.new" "$T/status"
for _ in $(seq 50); do flock -n "$T/omci.lock" true && break; sleep 0.1; done

# An answer that never comes while that omcid lives (a reload that could
# not write its status) holds the lock LINGER_MAX seconds, not for good.
out=$(ACK=1 LINGER=2 run omci); rc=$?
t "unanswered with a short LINGER_MAX: exit 1" "^1$" "$rc"
t "the lock is held at first" "^held$" "$(flock -n "$T/omci.lock" true && echo free || echo held)"
for _ in $(seq 60); do flock -n "$T/omci.lock" true && break; sleep 0.1; done
t "and free after LINGER_MAX, the omcid still alive" "^free alive$" "$(flock -n "$T/omci.lock" true && echo free) $(kill -0 "$(cat "$T/omcid.pid")" && echo alive)"

# Unanswered again, and this time the omcid it went to goes away: the lock
# is released when no omcid it signalled lives, not on a clock.
rm -f "$T/script"
out=$(ACK=2 run omci); rc=$?
t "unanswered again: exit 1" "^1 .*did not answer the signal" "$rc $(printf '%s\n' "$out" | tr '\n' ' ')"
sleep 2
t "the lock stays held while that omcid lives" "^held$" "$(flock -n "$T/omci.lock" true && echo free || echo held)"

# No omcid at all.
kill "$(cat "$T/omcid.pid")"; wait 2>/dev/null; rm -f "$T/omcid.pid"
for _ in $(seq 50); do flock -n "$T/omci.lock" true && break; sleep 0.1; done
t "once that omcid is gone, the lock is free" "^free$" "$(flock -n "$T/omci.lock" true && echo free)"
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
