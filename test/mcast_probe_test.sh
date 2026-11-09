#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

# mcast_probe_test.sh -- runs tools/mcast-probe/mcast-probe.sh against stub
# diag, igmpd, timeout and pidof: the order it installs and removes the
# probes, the results block, and that it puts everything back however it
# ends -- at the end of a run, on SIGTERM, and on SIGKILL, where only the
# end of input of the probe holder is left to do it (the stub diag drops the
# probes when its input ends or its holder is gone, as the kernel does when
# file of the holder closes). Part of `make test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SCRIPT=$ROOT/tools/mcast-probe/mcast-probe.sh
W=$(mktemp -d -t mcast_probe_test.XXXXXX)
trap '[ -n "${KEEP:-}" ] || rm -rf "$W"' EXIT
fail=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fail=1; }

mkdir -p "$W/bin" "$W/proc"

# The stub diag: the probes are files in $FAKE_STATE, owned by the stream
# diag that installed the first one.
cat > "$W/bin/diag" <<'EOF'
#!/bin/sh
S=$FAKE_STATE
gone() { rm -f "$S"/slot* "$S/owner"; }
# The holder dead, as after a SIGKILL: the kernel would have closed its file.
held() {
    owner=$(cat "$S/owner" 2>/dev/null) || return 0
    case "$owner" in *[!0-9]*|'') return 0 ;; esac
    kill -0 "$owner" 2>/dev/null || gone
    return 0
}
show() {
	if [ -f "$S/slot$1" ]; then cat "$S/slot$1"
	else echo "slot=$1 entry=$((88 + $1)) counter=$((16 + 2 * $1)) empty"; fi
}
run() {
	# shellcheck disable=SC2086 # words, on purpose
	set -- $1
	case "$1 ${2:-}" in
	"acl-probe show") held; show "$3" ;;
	"acl-probe delete")
		if [ "$3" = all ]; then rm -f "$S"/slot*; else rm -f "$S/slot$3"; fi
		ls "$S"/slot* > /dev/null 2>&1 || rm -f "$S/owner"
		echo "delete $3" >> "$S/log" ;;
	"acl-probe count"|"acl-probe bytes"|"acl-probe mirror"|"acl-probe trap")
		if [ "$OWNER" != 1 ]; then echo "% busy"; return; fi
		[ -f "$S/slot$3" ] && { echo "% busy"; return; }
		echo "$$" > "$S/owner.tmp"
		mv "$S/owner.tmp" "$S/owner"
		echo "slot=$3 entry=$((88 + $3)) counter=$((16 + 2 * $3)) port=0 action=$2 unit=frames proto=2 dst=0.0.0.0/0 counted=7 base=0x00000000 now=0x00000007 cpu=7 cpu_ports=0x1 cpu_bytes=448 uni_igmp_other=0 other_reason=0" > "$S/slot$3"
		echo "$*" >> "$S/log" ;;
	"mib dump")
		t=$(date +%s); echo "$t.00 1.00" > "$FAKE_PROC/uptime"
		for p in 0 2 3; do
			echo "Port: $p"
			echo "ifInMulticastPkts                  :                       $t"
			echo "ifOutMulticastPkts                 :                       $t"
		done ;;
	"l2-table get") cat "$S/rows" 2> /dev/null ;;
	"exit ") exit 0 ;;
	esac
}
if [ $# -gt 0 ]; then OWNER=0; run "$*"; exit 0; fi
OWNER=1
while read -r line; do echo "RTK.0> $line"; run "$line"; done
# The end of input closes the file: the close by the holder removes the probes.
[ "$(cat "$S/owner" 2> /dev/null)" = "$$" ] && gone
exit 0
EOF
cat > "$W/bin/igmpd" <<'EOF'
#!/bin/sh
case "$2" in
-j) echo "01:00:5E:7F:4D:01 -   -   -   0    Static -   SVL  mc   0x2   0x123" >> "$FAKE_STATE/rows" ;;
-l) : > "$FAKE_STATE/rows" ;;
esac
EOF
cat > "$W/bin/timeout" <<'EOF'
#!/bin/sh
[ "$1" = -s ] && shift 2
shift
exec "$@"
EOF
cat > "$W/bin/pidof" <<'EOF'
#!/bin/sh
[ -n "${FAKE_IGMPD:-}" ]
EOF
chmod +x "$W/bin/"*
echo "100.00 1.00" > "$W/proc/uptime"
printf 'rx_pon 5\nrx_pon_dropped 0\n' > "$W/proc/odi_nic"
printf 'frames_delivered 9\n' > "$W/proc/odi_omci"

# run NAME: a fresh state, the script with one-second windows.
setup() {
	rm -rf "$W/state" "$W/out"
	mkdir -p "$W/state"
	export FAKE_STATE=$W/state FAKE_PROC=$W/proc
	export PATH="$W/bin:$PATH" DEV=/dev/null PROC=$W/proc OUT=$W/out
	export COUNT_S=1 BYTES_S=1 OVERLAP_S=1 CPU_S=1 ROW_S=1
}
probes_left() { ls "$W/state"/slot* 2> /dev/null | wc -l | tr -d ' '; }

# A reader must not treat an incomplete owner publication as a dead pid.
setup
: > "$W/state/owner"
echo "slot=0 synthetic" > "$W/state/slot0"
diag acl-probe show 0 > "$W/empty-owner.log"
[ -f "$W/state/slot0" ] && ok "an incomplete owner does not delete live probes" || bad "incomplete owner deleted probes"
# A genuinely dead owner still releases its slots.
sleep 0.1 & dead=$!
wait "$dead"
echo "$dead" > "$W/state/owner"
diag acl-probe show 0 > "$W/dead-owner.log"
[ "$(probes_left)" = 0 ] && ok "a dead owner releases probes" || bad "dead owner probes left"

# 1. A whole run.
setup
if sh "$SCRIPT" > "$W/run.log" 2>&1; then ok "a run ends with status 0"; else bad "a run failed"; cat "$W/run.log"; fi
[ "$(probes_left)" = 0 ] && ok "no probe is left" || bad "probes left after a run"
[ ! -s "$W/state/rows" ] && ok "no row is left" || bad "the row is left"
grep -q '^restored: probes left 0, rows for 239.255.77.1 left 0$' "$W/out/results.txt" &&
	ok "the results say so" || { bad "no restored line in the results"; cat "$W/out/results.txt"; }
for w in "count (frames" "bytes (" "overlap (" "mirror (" "trap (" "row (" "isolation, the whole run"; do
	grep -q "^$w" "$W/out/results.txt" && ok "results: $w" || bad "results lack: $w"
done
grep -q '^  UNI in, IGMP, counted by the rule  *7 ' "$W/out/results.txt" &&
	ok "a probe count reaches its line" || bad "the mirror count is not in the results"
# The order of the windows: the rules of each go before those of the next.
want='count 0 port 2 proto 2|count 1 port 2 group 0xe0000000 4|count 2 port 0 proto 2|count 3 port 0 group 0xefff4d01 32|count 4 port 0 group 0xe0000000 4|delete all|bytes 0 port 2 proto 2'
got=$(head -7 "$W/state/log" | sed 's/^acl-probe //' | paste -sd'|' -)
[ "$got" = "$want" ] && ok "the count window's rules, then the bytes window's" || bad "rule order: $got"
grep -q '^trap 0 igmp bytes$' <(sed 's/^acl-probe //' "$W/state/log") && ok "the trap counts bytes" ||
	bad "no trap in bytes"

# 2. igmpd running: refused before anything is installed.
setup
if FAKE_IGMPD=1 sh "$SCRIPT" > "$W/run.log" 2>&1; then bad "ran beside igmpd"; else ok "refuses beside igmpd"; fi
[ ! -f "$W/state/log" ] && ok "and installed nothing" || bad "installed a probe beside igmpd"

# 3. SIGTERM mid-window: the trap puts everything back.
setup
export COUNT_S=20
sh "$SCRIPT" > "$W/run.log" 2>&1 &
pid=$!
for _ in $(seq 50); do [ "$(probes_left)" -ge 5 ] && break; sleep 0.1; done
[ "$(probes_left)" -ge 5 ] && ok "five probes in for the count window" || bad "the count window never started"
kill -TERM "$pid"
wait "$pid" 2> /dev/null || true
cp "$W/run.log" "$W/term.log"
[ "$(probes_left)" = 0 ] && ok "SIGTERM: no probe is left" || bad "SIGTERM: probes left"
grep -q 'restored: probes left 0' "$W/run.log" && ok "SIGTERM: it says so" || bad "SIGTERM: no restored line"

# 4. SIGKILL mid-window: no trap runs; the holder input ends and the probes go.
setup
export COUNT_S=20
sh "$SCRIPT" > "$W/run.log" 2>&1 &
pid=$!
for _ in $(seq 50); do [ "$(probes_left)" -ge 5 ] && break; sleep 0.1; done
[ "$(probes_left)" -ge 5 ] && ok "SIGKILL: count window started" || bad "SIGKILL: count window never started"
{ kill -KILL "$pid"; wait "$pid"; } 2> /dev/null || true
for _ in $(seq 50); do [ "$(probes_left)" = 0 ] && break; sleep 0.1; done
if [ "$(probes_left)" = 0 ]; then
    ok "SIGKILL: the probes go with the holder"
else
    bad "SIGKILL: probes left"
    ls -l "$W/state"
    cat "$W/state/owner" "$W/out/holder.out" "$W/run.log" 2>/dev/null || true
fi

[ "$fail" = 0 ] && echo "mcast_probe_test: all checks passed"
exit "$fail"
