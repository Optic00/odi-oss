#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

#
# rootfs/skeleton/etc/scripts/diag-summary.sh against bundle directories
# made here, and diag-bundle.sh end to end with every device command a
# stub, for README.md and SUMMARY.txt.
#
# What is checked: each summary line from its file (image, uptime, slot,
# with sw_tryactive 2 as no trial, 0 or 1 pending, a failed read unknown,
# identity, ONU state, the last state change and how long ago, the O5
# drops by cause, LOS and LOF, the alarms set now (a failed or empty
# alarm read is unread, never none), the omcli state lines,
# the optics, the previous boot, a safe-mode boot, the failed steps); a
# bundle with none of those files gives "not in bundle" on every line and
# exits 0; no directory is refused. End to end: the bundle carries both
# files, listed in MANIFEST.txt, the README says whether the identity is
# masked, the summary says so too, and the summary is scrubbed like every
# other file: a serial and a password planted in a line it takes over come
# out masked and redacted, and with --full the serial stays and the
# password does not. The diag batch carries the five transceiver reads.
set -u
cd "$(dirname "$0")/.." || exit 1
S=$PWD/rootfs/skeleton/etc/scripts/diag-summary.sh
B=$PWD/rootfs/skeleton/etc/scripts/diag-bundle.sh
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
pass=0 fail=0
ok() { echo "ok    $1"; pass=$((pass + 1)); }
no() { echo "FAIL  $1"; [ -n "${2:-}" ] && printf '%s\n' "$2" | sed 's/^/        /'; fail=$((fail + 1)); }
# has FILE LINE: FILE has LINE, whole.
has() {
	if grep -qxF -- "$2" "$1"; then
		ok "$3"
	else
		no "$3" "want: $2
got:  $(grep -F -- "${2%%:*}" "$1" | head -n 3)"
	fi
}

# ------------------------------------------------------------ a full bundle
D=$T/odi-diag
mkdir -p "$D/pon"
cat > "$D/MANIFEST.txt" <<'EOF'
odi-oss diagnostics bundle
created (stick clock, UTC): 2026-11-15 12:00:00
uptime: 7322.50 s
redacted keys: GPON_PLOAM_PASSWD and any key containing: PASS
identity: masked: GPON_SN ELAN_MAC_ADDR LOID LOID_OLD, the serial and the MACs

rc      bytes  file
-       1000  ramlog_prev.txt
0        200  dmesg.txt
1          0  module/a0.txt
-          -  config/odi.conf (absent)
EOF
printf 'image=v1.16.0\nbuilt=2026-11-14T10:00Z\nversion:   v1.16.0\n' > "$D/odi-build.txt"
cat > "$D/nv.txt" <<'EOF'
getenv sw_active: sw_active=1
fallback sw_active: sw_active=1
getenv sw_commit: sw_commit=1
fallback sw_commit: sw_commit=1
getenv sw_tryactive: sw_tryactive=2
getenv sw_version0: sw_version0=v1.15.0
EOF
cat > "$D/dmesg.txt" <<'EOF'
[    5.100000] safe-mode: event=boot_count boot=1 short_before=0 limit=5 wdt_before=0 wdt_limit=3 prev=wdt_userland uptime=5
[   30.000000] odi_gpon: event=onu_state from=O1 to=O2 in_state_s=10.000 cause=los_cleared side=onu onu_id=0
[   32.500000] odi_gpon: event=onu_state from=O4 to=O5 in_state_s=1.200 cause=ranging_time side=olt onu_id=3
[ 1000.000000] odi_gpon: event=los state=on onu_state=O5
[ 1000.000100] odi_gpon: event=onu_state from=O5 to=O6 in_state_s=967.500 cause=los side=onu onu_id=3
[ 1050.000000] odi_gpon: event=los state=off lasted_s=50.000 onu_state=O6
[ 1060.000000] odi_gpon: event=onu_state from=O4 to=O5 in_state_s=1.000 cause=ranging_time side=olt onu_id=3
[ 2000.000000] odi_gpon: event=onu_state from=O5 to=O2 in_state_s=940.000 cause=deactivate_onu_id side=olt onu_id=3
[ 2100.000000] odi_gpon: event=onu_state from=O5 to=O6 in_state_s=10.000 cause=los side=onu onu_id=3
[ 2200.000000] odi_gpon: event=lof state=on onu_state=O5
[ 2322.500000] odi_gpon: event=onu_state from=O4 to=O5 in_state_s=1.000 cause=ranging_time side=olt onu_id=3
EOF
printf 'state 5 (O5)\no7_hold keep 0 held 0\nonu_id 3\n' > "$D/pon/odi_gpon-status.txt"
cat > "$D/pon/diag.txt" <<'EOF'
RTK.0> gpon get alarm-status
Alarm LOS, status: clear
Alarm LOF, status: occur
Alarm LOM, status: clear
RTK.0> pon get transceiver rx-power
Rx Power: -22.924298  dBm
RTK.0> pon get transceiver tx-power
Tx Power: 2.287339  dBm
RTK.0> pon get transceiver temperature
Temperature: 42.359375 C
RTK.0> pon get transceiver bias-current
Bias Current: 14.350000 mA
RTK.0> pon get transceiver voltage
Voltage: 3.170300 V
EOF
cat > "$D/pon/omcli_state.txt" <<'EOF'
serial     : MASKED-0001
device     : x
onu state  : O5
mib sync   : 184
programming: on
rx dropped : length 0  devid 0  crc 2  (trailer odd, answered: 0)
mib resets : olt 3  local 0  olt in last 600 s 3  loop yes  (loops 1)
EOF
printf 'omcid[1]: event=unknown_me class=351 op=create mt=4\nomcid[1]: event=unknown_me class=350 op=get mt=9\n' > "$D/syslog.txt"
printf 'previous boot: boot=7 slot=1 build=v1.16.0 crumb=userland/3 reason=wdt_userland\nline\n' > "$D/ramlog_prev.txt"

out=$T/summary.txt
sh "$S" "$D" > "$out" 2>&1
rc=$?
[ "$rc" = 0 ] && ok "a full bundle: exit 0" || no "a full bundle: exit 0" "rc $rc: $(cat "$out")"
has "$out" "image          : v1.16.0  (odi-build.txt)" "image from odi-build.txt"
has "$out" "taken          : 2026-11-15 12:00:00 stick clock  (MANIFEST.txt)" "taken from the manifest"
has "$out" "uptime         : 2h 02m  (MANIFEST.txt)" "uptime as hours and minutes"
has "$out" "slot           : active 1, committed 1, trial none  (nv.txt)" "slot: sw_tryactive 2 is no trial"
has "$out" "identity       : masked, serial, MACs and LOID cut to their last four  (MANIFEST.txt)" "identity from the manifest"
has "$out" "onu state      : O5  (pon/odi_gpon-status.txt)" "the ONU state"
has "$out" "last change    : at 2322.500000 s, 5000 s ago: from=O4 to=O5 cause=ranging_time side=olt  (dmesg.txt)" \
	"the last state change and how long ago"
has "$out" "left O5        : 3 this boot: los 2, deactivate_onu_id 1  (dmesg.txt)" "O5 drops by cause, first seen first"
has "$out" "LOS / LOF      : 1 / 1 this boot  (dmesg.txt)" "LOS and LOF counted on state=on only"
has "$out" "alarms now     : LOF  (pon/diag.txt)" "only the alarms that occur"
has "$out" "mib resets     : olt 3  local 0  olt in last 600 s 3  loop yes  (loops 1)  (pon/omcli_state.txt)" "the mib resets line"
has "$out" "rx dropped     : length 0  devid 0  crc 2  (trailer odd, answered: 0)  (pon/omcli_state.txt)" "the rx dropped line"
has "$out" "mib sync       : 184  (pon/omcli_state.txt)" "the MIB data sync"
has "$out" "unknown MEs    : 2 first requests logged  (syslog.txt; log/omcid-unknown.txt has the list)" "unknown MEs counted"
has "$out" "optics         : rx -22.92 dBm, tx 2.29 dBm, 42.4 C, bias 14.35 mA, 3.17 V  (pon/diag.txt)" "the optics"
has "$out" "previous boot  : ended by wdt_userland  (ramlog_prev.txt)" "the previous boot reason"
has "$out" "boot count     : boot=1 short_before=0 limit=5 wdt_before=0 wdt_limit=3 prev=wdt_userland uptime=5  (dmesg.txt)" "the boot count line"
has "$out" "failed steps   : module/a0.txt (rc 1)  (MANIFEST.txt)" "failed steps: a number other than 0, not a dash"
grep -q '^SAFE MODE' "$out" && no "no safe-mode line on a normal boot" "$(cat "$out")" || ok "no safe-mode line on a normal boot"
# The trial slot: 0 or 1 pending, a failed or empty read unknown, not none.
cp "$D/nv.txt" "$T/nv.keep"
sed -i 's/^getenv sw_tryactive: sw_tryactive=2$/getenv sw_tryactive: sw_tryactive=0/' "$D/nv.txt"
sh "$S" "$D" > "$T/s2" 2>&1
has "$T/s2" "slot           : active 1, committed 1, trial slot 0 pending  (nv.txt)" "slot: sw_tryactive 0 is a pending trial"
sed -i 's/^getenv sw_tryactive: .*/getenv sw_tryactive: (rc 1)/' "$D/nv.txt"
sh "$S" "$D" > "$T/s2" 2>&1
has "$T/s2" "slot           : active 1, committed 1, trial unknown  (nv.txt)" "slot: a failed sw_tryactive read is unknown"
cp "$T/nv.keep" "$D/nv.txt"
# The alarms: a failed or empty read is unread, never none.
cp "$D/pon/diag.txt" "$T/diag.keep"
printf 'RTK.0> gpon get alarm-status
%% alarm status read failed (/proc/odi_gpon/status and /dev/odi_sw)
' > "$D/pon/diag.txt"
sh "$S" "$D" > "$T/s2" 2>&1
has "$T/s2" "alarms now     : unread (no alarm-status lines in pon/diag.txt)" "alarms: a failed read is unread"
: > "$D/pon/diag.txt"
sh "$S" "$D" > "$T/s2" 2>&1
has "$T/s2" "alarms now     : unread (no alarm-status lines in pon/diag.txt)" "alarms: an empty diag.txt is unread"
sed 's/status: occur/status: clear/' "$T/diag.keep" > "$D/pon/diag.txt"
sh "$S" "$D" > "$T/s2" 2>&1
has "$T/s2" "alarms now     : none  (pon/diag.txt)" "alarms: all three clear is none"
cp "$T/diag.keep" "$D/pon/diag.txt"

n=$(wc -l < "$out")
[ "$n" -le 25 ] && ok "fits a screen ($n lines)" || no "fits a screen" "$n lines"

echo '[ 5.2] safe-mode: event=safe_mode cause=watchdog short_boots=0' >> "$D/dmesg.txt"
sh "$S" "$D" > "$out" 2>&1
has "$out" "SAFE MODE      : this boot is a safe-mode boot  (dmesg.txt)" "a safe-mode boot is named"

# ------------------------------------------------------- an empty bundle
E=$T/empty
mkdir "$E"
sh "$S" "$E" > "$out" 2>&1
rc=$?
[ "$rc" = 0 ] && ok "an empty bundle: exit 0" || no "an empty bundle: exit 0" "rc $rc"
for k in image uptime "onu state" "mib resets" optics "previous boot" "boot count"; do
	grep -q "^$k *: .*not in bundle" "$out" && ok "empty: $k says not in bundle" ||
		no "empty: $k says not in bundle" "$(grep "^$k" "$out")"
done
has "$out" "last change    : not in bundle (no dmesg.txt)" "empty: no dmesg, no state change claimed"
has "$out" "alarms now     : unread (no alarm-status lines in pon/diag.txt)" "empty: alarms unread, not none"
if sh "$S" "$T/none" > /dev/null 2>&1; then no "a missing directory is refused"; else ok "a missing directory is refused"; fi

# ------------------------------------------------- diag-bundle.sh end to end
# Every device command is a stub; omcli state prints a serial that the
# config store also holds, so the scrub has something to mask.
SN=ABCD12345678
PW=Ploam-Pw-4711
mkdir -p "$T/bin" "$T/cfg" "$T/log"
for c in i2cdump i2cget i2cset nv wget; do
	printf '#!/bin/sh\nexit 1\n' > "$T/bin/$c"
done
printf '#!/bin/sh\ncat >> %s\ncat %s\n' "$T/diag.stdin" "$D/pon/diag.txt" > "$T/bin/diag"
# The boot count line goes into SUMMARY.txt whole: the serial and the
# password planted in it must come out masked and redacted there.
printf '#!/bin/sh\necho "[ 1.0] safe-mode: event=boot_count boot=1 sn=%s pw=%s"\n' "$SN" "$PW" > "$T/bin/dmesg"
printf '#!/bin/sh\necho "syslog line"\n' > "$T/bin/logread"
cat > "$T/bin/omcli" <<EOF
#!/bin/sh
case "\$1" in
state) printf 'serial     : %s\nonu state  : O5\nmib resets : olt 0  local 0  olt in last 600 s 0  loop no  (loops 0)\n' "$SN" ;;
*) echo "\$1" ;;
esac
EOF
chmod +x "$T/bin/"*
printf 'GPON_SN=%s\nGPON_PLOAM_PASSWD=%s\n' "$SN" "$PW" > "$T/cfg/odi.conf"
echo "a log line" > "$T/log/messages"
run_bundle() {
	CONFIG_DIR="$T/cfg" LOG_DIR="$T/log" GPON_PROC_DIR="$T/nogpon" PATH="$T/bin:$PATH" \
		METRICS_TIMEOUT_S=1 STEP_TIMEOUT_S=2 timeout 120 sh "$B" "$@" > "$T/run.out" 2>&1
}
run_bundle "$T/out.tar.gz"
[ "$(tail -n 1 "$T/run.out")" = "$T/out.tar.gz" ] && ok "the bundle is written" || no "the bundle is written" "$(cat "$T/run.out")"
mkdir "$T/x"
tar -xzf "$T/out.tar.gz" -C "$T/x" 2>/dev/null
X=$T/x/odi-diag
for f in README.md SUMMARY.txt; do
	[ -s "$X/$f" ] && ok "the bundle has $f" || no "the bundle has $f"
	awk -v f="$f" '$3 == f { n++ } END { exit !n }' "$X/MANIFEST.txt" && ok "MANIFEST lists $f" ||
		no "MANIFEST lists $f" "$(cat "$X/MANIFEST.txt")"
done
awk '$3 == "SUMMARY.txt" && $1 == 0 { n++ } END { exit !n }' "$X/MANIFEST.txt" && ok "the summary step is listed with status 0" ||
	no "the summary step is listed with status 0" "$(grep SUMMARY "$X/MANIFEST.txt")"
grep -q '^- Identity: masked' "$X/README.md" && ok "README: identity masked" || no "README: identity masked" "$(cat "$X/README.md")"
grep -q '^| ONU state, alarms' "$X/README.md" && ok "README: the where-to-look table" || no "README: the where-to-look table"
grep -q '^optics         : rx -22.92 dBm' "$X/SUMMARY.txt" && ok "the summary has the optics from the diag batch" ||
	no "the summary has the optics from the diag batch" "$(cat "$X/SUMMARY.txt")"
grep -q '^identity       : masked' "$X/SUMMARY.txt" && ok "the summary says masked" || no "the summary says masked"
if grep -r -l -i -F -e "$SN" -e "$PW" "$X"; then no "neither the serial nor the password is in any file of the bundle"; else ok "neither the serial nor the password is in any file of the bundle"; fi
has "$X/SUMMARY.txt" "boot count     : boot=1 sn=MASKED-5678 pw=REDACTED  (dmesg.txt)" "the summary is scrubbed: serial masked, password redacted"
for c in rx-power tx-power temperature bias-current voltage; do
	grep -qx "pon get transceiver $c" "$T/diag.stdin" && ok "the diag batch reads $c" || no "the diag batch reads $c" "$(cat "$T/diag.stdin")"
done
grep -q 'pon get transceiver' "$T/run.out" && no "no error about the diag batch" "$(cat "$T/run.out")" || ok "no error about the diag batch"

rm -rf "$T/x"
mkdir "$T/x"
run_bundle --full "$T/full.tar.gz"
tar -xzf "$T/full.tar.gz" -C "$T/x" 2>/dev/null
grep -q '^- Identity: NOT masked' "$T/x/odi-diag/README.md" && ok "README with --full: NOT masked, share privately" ||
	no "README with --full: NOT masked" "$(cat "$T/x/odi-diag/README.md")"
has "$T/x/odi-diag/SUMMARY.txt" "boot count     : boot=1 sn=$SN pw=REDACTED  (dmesg.txt)" "--full: the summary keeps the serial and still redacts the password"
grep -q '^identity       : KEPT, taken with --full' "$T/x/odi-diag/SUMMARY.txt" && ok "the summary with --full says KEPT" ||
	no "the summary with --full says KEPT" "$(grep identity "$T/x/odi-diag/SUMMARY.txt")"

echo
echo "diag_summary_test: $pass passed, $fail failed"
[ "$fail" = 0 ]
