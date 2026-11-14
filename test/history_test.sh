#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

#
# rootfs/skeleton/etc/scripts/history.sh, off the device. diag is a
# stub that prints the reply src/diag/test/exporter.golden pins (the stock
# output format both diags keep), /proc files are fixtures. Checks the
# sample line for a healthy line, a stock slot (no /proc/odi_gpon), a diag
# that fails, LOS, and that the ring keeps KEEP lines, oldest first, and
# is replaced by rename.
set -u
cd "$(dirname "$0")/.." || exit 1
S=$PWD/rootfs/skeleton/etc/scripts/history.sh
GOLDEN=$PWD/src/diag/test/exporter.golden
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
pass=0; fail=0
ok() { echo "ok    $1"; pass=$((pass + 1)); }
no() { echo "FAIL  $1"; fail=$((fail + 1)); }
# has <what> <line> <key=value>: the pair is in the line.
has() { case " $2 " in *" $3 "*) ok "$1" ;; *) no "$1 (wanted $3 in: $2)" ;; esac; }
# lacks <what> <line> <key>: no pair with that key.
lacks() { case " $2 " in *" $3="*) no "$1 (wanted no $3= in: $2)" ;; *) ok "$1" ;; esac; }

mkdir -p "$T/bin"
# The stub answers only what it is asked: the golden reply for each command
# on stdin, cut at the next prompt. DIAG_MODE=los swaps in the LOS
# reply, DIAG_MODE=fail prints nothing and exits 1.
cat > "$T/bin/diag" <<EOF
#!/bin/sh
case "\${DIAG_MODE:-}" in fail) exit 1 ;; esac
while read -r cmd; do
	[ "\$cmd" = exit ] && break
	echo "RTK.0> \$cmd"
	if [ "\$cmd" = "mib dump counter port 2" ]; then
		awk '/^Port: 2\$/{p=1} /^Port: [013-9]/{p=0} /^\r?RTK/{p=0} p' "$GOLDEN"
		continue
	fi
	awk -v c="RTK.0> \$cmd" '{l=\$0; sub(/^\r/, "", l)} l==c{p=1;next} l ~ /^RTK/{p=0} p' "$GOLDEN"
done | if [ "\${DIAG_MODE:-}" = los ]; then
	sed -e 's/^Rx Power: .*/% transceiver read failed/' -e 's/^Alarm LOS, status: clear/Alarm LOS, status: occur/' -e 's/Operation State(O5)/Operation State(O1)/'
else
	cat
fi
EOF
chmod +x "$T/bin/diag"

echo '86512.33 170000.10' > "$T/uptime"
cat > "$T/stat" <<'EOF'
cpu  1000 0 500 8000 100 0 50 0 0 0
cpu0 1000 0 500 8000 100 0 50 0 0 0
EOF
cat > "$T/meminfo" <<'EOF'
MemTotal:          56000 kB
MemFree:           20000 kB
MemAvailable:      31000 kB
EOF
cat > "$T/status" <<'EOF'
state 5 (O5)
onu_id 3
alarm_events los 2
alarm_events lof 3
alarm_events lom 0
onu_transitions O1 O2 1
onu_transitions O4 O5 3
onu_transitions O5 O1 1
onu_transitions O5 O6 1
EOF
cat > "$T/counters" <<'EOF'
ctr_samples 100
ctr ds_bip_err_bits 12345678901234
ctr ds_fec_uncorrectable_cw 7
EOF

run() {
	env DIAG="$T/bin/diag" PROC_UPTIME="$T/uptime" PROC_STAT="$T/stat" \
		PROC_MEMINFO="$T/meminfo" GPON_STATUS="$T/status" \
		GPON_COUNTERS="$T/counters" NOW="echo 1763150000" "$@"
}

l=$(run sh "$S" sample)
echo "        $l"
has "wall clock" "$l" t=1763150000
has "uptime, whole seconds" "$l" up=86512
has "ONU state from diag" "$l" st=5
has "LOF asserted (the golden has it occur)" "$l" al=LOF
has "rx power, two decimals" "$l" rx=-22.92
has "tx power" "$l" tx=2.29
has "temperature" "$l" tc=42.36
has "bias" "$l" bi=14.35
has "fibre port octets in" "$l" di=3000
has "fibre port octets out" "$l" do=18005
has "LOS events" "$l" los=2
has "LOF events" "$l" lof=3
has "into O5" "$l" o5i=3
has "out of O5" "$l" o5o=2
has "BIP bits, a 64-bit total kept whole" "$l" bip=12345678901234
has "uncorrectable FEC" "$l" fecu=7
has "cpu busy (all but idle and iowait)" "$l" cb=1550
has "cpu total" "$l" ct=9650
has "MemAvailable" "$l" ma=31000

l=$(run GPON_STATUS="$T/none" GPON_COUNTERS="$T/none" sh "$S" sample)
lacks "stock slot: no LOS events" "$l" los
lacks "stock slot: no transitions" "$l" o5i
lacks "stock slot: no BIP" "$l" bip
has "stock slot: diag still answers" "$l" st=5

l=$(run DIAG_MODE=fail sh "$S" sample)
lacks "diag failed: no rx" "$l" rx
lacks "diag failed: no alarms key" "$l" al
has "diag failed: state from the driver" "$l" st=5
has "diag failed: the rest is there" "$l" ma=31000

l=$(run DIAG_MODE=los sh "$S" sample)
lacks "LOS: no rx reading" "$l" rx
has "LOS: alarms" "$l" al=LOS,LOF
has "LOS: state" "$l" st=1

# The ring: KEEP lines, the newest last, by rename (no .tmp left behind).
# The sleep stub ends the run loop after its first sample.
cat > "$T/bin/stop" <<'EOF'
#!/bin/sh
kill -TERM "$PPID"
EOF
chmod +x "$T/bin/stop"
printf 'up=1\nup=2\nup=3\n' > "$T/ring"
run RING="$T/ring" KEEP=3 SLEEP="$T/bin/stop" sh "$S" run 2>/dev/null
if [ "$(wc -l < "$T/ring")" -eq 3 ] && [ "$(sed -n 1p "$T/ring")" = up=2 ] &&
	[ "$(sed -n 2p "$T/ring")" = up=3 ] && sed -n 3p "$T/ring" | grep -q ' up=86512 '; then
	ok "ring: KEEP lines, oldest dropped, newest last"
else
	no "ring: $(tr '\n' '|' < "$T/ring")"
fi
if [ ! -e "$T/ring.tmp" ]; then ok "ring: written by rename"; else no "ring: .tmp left behind"; fi

echo "history: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
