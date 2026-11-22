#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

#
# rootfs/skeleton/etc/scripts/gpon-hold.sh and the boot step in rcs-lib.sh
# (pon_step_keepdisable), off the device (GPON_KEEP_DISABLE). The driver is a
# status file the test writes, /proc/odi_init a plain file, the kernel log a
# plain file, and the config partition a temporary directory.
#
# What is checked: sync writes the marker when the driver says it holds O7 and
# removes it when it does not, says nothing and touches nothing when the driver
# says nothing, re-reads after writing (a state that moved meanwhile ends on
# the current one), and logs each change; clear removes the marker, tells the
# driver, and works with no driver and with no marker; status; and the boot
# step: nothing at all with the key at 0, gponkeep alone with no marker,
# gponkeep then gponhold with one (key absent, empty, 1 or any other value),
# and neither in a safe-mode boot, with the key set or absent.
# shellcheck disable=SC2329 # the stubs are called by the sourced step
set -u
cd "$(dirname "$0")/.." || exit 1
S=$PWD/rootfs/skeleton/etc/scripts/gpon-hold.sh
LIB=$PWD/rootfs/skeleton/etc/scripts/rcs-lib.sh
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
pass=0 fail=0
ok() { echo "ok    $1"; pass=$((pass + 1)); }
no() { echo "FAIL  $1"; printf '        wanted: %s\n        got:    %s\n' "$2" "$3"; fail=$((fail + 1)); }
eq() { if [ "$3" = "$2" ]; then ok "$1"; else no "$1" "$2" "$3"; fi; }

export GPON_HOLD_MARKER=$T/gpon-hold
export GPON_STATUS=$T/status
export ODI_INIT=$T/odi_init
export ODI_CONF=$T/odi.conf
export KMSG=$T/kmsg
export TIMEOUT=$T/timeout
: > "$KMSG"

# The timeout stub runs the command, and first a hook when there is one: the
# state the driver reports moves while the script is busy flushing.
cat > "$TIMEOUT" <<STUB
#!/bin/sh
shift
[ -f "$T/hook" ] && { sh "$T/hook"; rm -f "$T/hook"; }
exec "\$@"
STUB
chmod +x "$TIMEOUT"

status() { printf 'state 7 (O7)\no7_hold keep %s held %s\nonu_id -1\n' "$1" "$2" > "$GPON_STATUS"; }
marker() { [ -e "$GPON_HOLD_MARKER" ] && echo 1 || echo 0; }
events() { sed -n 's/^<5>gpon-hold: //p' "$KMSG" | tr '\n' '|'; }

# ---- sync
rm -f "$GPON_HOLD_MARKER"; : > "$KMSG"
status 1 1
sh "$S" sync
eq "sync: held 1 writes the marker" 1 "$(marker)"
eq "sync: the event is logged" "event=marker action=written|" "$(events)"
eq "sync: no temporary file left" 0 "$(find "$T" -maxdepth 1 -name 'gpon-hold.*' | wc -l | tr -d ' ')"
[ "$(grep -c . "$GPON_HOLD_MARKER")" = 1 ] && ok "sync: the marker is one line" || no "sync: the marker is one line" 1 "$(grep -c . "$GPON_HOLD_MARKER")"

: > "$KMSG"
sh "$S" sync
eq "sync: held 1 again leaves it" 1 "$(marker)"

: > "$KMSG"
status 1 0
sh "$S" sync
eq "sync: held 0 removes the marker" 0 "$(marker)"
eq "sync: the removal is logged" "event=marker action=removed|" "$(events)"

: > "$KMSG"
sh "$S" sync
eq "sync: held 0 with no marker logs nothing" "" "$(events)"

: > "$GPON_HOLD_MARKER"; : > "$KMSG"
rm -f "$GPON_STATUS"
sh "$S" sync
eq "sync: no status file leaves the marker" 1 "$(marker)"
printf 'state 5 (O5)\nonu_id 3\n' > "$GPON_STATUS"
sh "$S" sync
eq "sync: a driver with no o7_hold line leaves the marker" 1 "$(marker)"
eq "sync: and logs nothing" "" "$(events)"

# the state moves while the marker is being written: the run ends on the
# current state, not the one it started from
rm -f "$GPON_HOLD_MARKER"; : > "$KMSG"
status 1 1
printf '%s\n' "printf 'o7_hold keep 1 held 0\n' > $GPON_STATUS" > "$T/hook"
sh "$S" sync
eq "sync: overtaken by an enable, the marker ends removed" 0 "$(marker)"
eq "sync: both changes logged" "event=marker action=written|event=marker action=removed|" "$(events)"

rm -f "$GPON_HOLD_MARKER"; : > "$KMSG"
status 1 0
: > "$GPON_HOLD_MARKER"
printf '%s\n' "printf 'o7_hold keep 1 held 1\n' > $GPON_STATUS" > "$T/hook"
sh "$S" sync
eq "sync: overtaken by a disable, the marker ends written" 1 "$(marker)"

# a marker that cannot be written is reported, and sync still exits 0
rm -f "$GPON_HOLD_MARKER"; : > "$KMSG"
status 1 1
GPON_HOLD_MARKER=$T/no/such/dir/gpon-hold sh "$S" sync
eq "sync: an unwritable partition exits 0" 0 $?
eq "sync: and says so" "event=marker action=failed what=write|" "$(events)"

# ---- clear
printf 'x\n' > "$GPON_HOLD_MARKER"; : > "$ODI_INIT"; : > "$KMSG"
sh "$S" clear > "$T/out" 2>&1
eq "clear: exit 0" 0 $?
eq "clear: the driver is told" "gponhold 0" "$(cat "$ODI_INIT")"
eq "clear: the marker is gone" 0 "$(marker)"
eq "clear: the removal is logged" "event=marker action=removed|" "$(events)"
sh "$S" clear > "$T/out" 2>&1
eq "clear: with no marker, still exit 0" 0 $?
grep -q "no marker" "$T/out" && ok "clear: says there was none" || no "clear: says there was none" "no marker" "$(cat "$T/out")"

printf 'x\n' > "$GPON_HOLD_MARKER"
ODI_INIT=$T/absent sh "$S" clear > "$T/out" 2>&1
eq "clear: with no driver, the marker still goes" 0 "$(marker)"

# ---- status
rm -f "$GPON_HOLD_MARKER"; printf 'GPON_KEEP_DISABLE=1\n' > "$ODI_CONF"; status 1 0
eq "status: key on, no marker, not held" "key=1 marker=0 held=0" "$(sh "$S" status | tr '\n' ' ' | sed 's/ $//')"
: > "$GPON_HOLD_MARKER"; printf 'GPON_KEEP_DISABLE=0\n' > "$ODI_CONF"; status 1 1
eq "status: key off, marker, held" "key=0 marker=1 held=1" "$(sh "$S" status | tr '\n' ' ' | sed 's/ $//')"
rm -f "$ODI_CONF"; status 1 1
eq "status: no odi.conf: the key is on by default" "key=1 marker=1 held=1" "$(sh "$S" status | tr '\n' ' ' | sed 's/ $//')"
sh "$S" bogus 2>/dev/null
eq "an unknown action exits 2" 2 $?

# ---- the boot step
# shellcheck source=/dev/null
. "$LIB"
GPON_HOLD_MARKER=$T/gpon-hold
SAFE_FLAG=$T/safe-mode
steps=''
pon_step() { steps="${steps}[$1]"; }
crumbs=''
crumb() { crumbs="${crumbs}{$*}"; }
run() { steps='' crumbs=''; pon_step_keepdisable; }

rm -f "$GPON_HOLD_MARKER" "$SAFE_FLAG" "$ODI_CONF"
run
eq "boot: no odi.conf: on by default, gponkeep only" "[gponkeep 1]" "$steps"
printf 'GPON_KEEP_DISABLE=0\n' > "$ODI_CONF"; : > "$GPON_HOLD_MARKER"
run
eq "boot: key 0 with a marker: nothing, the marker is not read" "" "$steps$crumbs"
printf 'GPON_KEEP_DISABLE=\n' > "$ODI_CONF"
run
eq "boot: key empty with a marker: on, gponkeep, then gponhold" "[gponkeep 1][gponhold 1]" "$steps"
printf 'SYSLOG_SERVER=\n' > "$ODI_CONF"; rm -f "$GPON_HOLD_MARKER"
run
eq "boot: key absent from odi.conf: on, gponkeep only" "[gponkeep 1]" "$steps"
printf 'GPON_KEEP_DISABLE=1\n' > "$ODI_CONF"; rm -f "$GPON_HOLD_MARKER"
run
eq "boot: key 1, no marker: gponkeep only" "[gponkeep 1]" "$steps"
: > "$GPON_HOLD_MARKER"
run
eq "boot: key 1 and a marker: gponkeep, then gponhold" "[gponkeep 1][gponhold 1]" "$steps"
: > "$SAFE_FLAG"
run
eq "boot: a safe-mode boot ignores key and marker" "" "$steps"
eq "boot: and says so" 1 "$(printf '%s' "$crumbs" | grep -c 'safe-mode boot')"
eq "boot: the marker is left as it was" 1 "$(marker)"
rm -f "$ODI_CONF"
run
eq "boot: safe mode, no odi.conf, a marker: nothing" "" "$steps"
eq "boot: and says so with no odi.conf" 1 "$(printf '%s' "$crumbs" | grep -c 'safe-mode boot')"
eq "boot: the marker is still there" 1 "$(marker)"
rm -f "$SAFE_FLAG"

# Only an exact GPON_KEEP_DISABLE=0 turns it off; anything else counts as on.
for v in off no false 00 '0 '; do
	printf 'GPON_KEEP_DISABLE=%s\n' "$v" > "$ODI_CONF"
	run
	eq "boot: key \"$v\" with a marker counts as on" "[gponkeep 1][gponhold 1]" "$steps"
done
printf 'GPON_KEEP_DISABLE=0\r\n' > "$ODI_CONF"
run
eq "boot: key \"0\" and a CR (a CRLF odi.conf) with a marker counts as on" "[gponkeep 1][gponhold 1]" "$steps"
printf 'GPON_KEEP_DISABLE=off\n' > "$ODI_CONF"; status 1 1
eq "status: key \"off\" counts as on" "key=1 marker=1 held=1" "$(sh "$S" status | tr '\n' ' ' | sed 's/ $//')"

# The default list calls the step, between the Dying_Gasp step and gponact.
order=$(sed -n '/^pon_steps_default()/,/^}/p' "$LIB" | grep -n 'pon_step_dyinggasp\|pon_step_keepdisable\|pon_step_gponact' | sed 's/^[0-9]*:[[:space:]]*//' | tr '\n' ' ')
eq "boot: the default steps run it before gponact" "pon_step_dyinggasp pon_step_keepdisable pon_step_gponact " "$order"
grep -q 'GPON_KEEP_DISABLE' rootfs/skeleton/etc/scripts/flash && ok "flash lists GPON_KEEP_DISABLE as an odi-only key" || no "flash lists the key" "listed" "missing"

echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
