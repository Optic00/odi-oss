#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

#
# rootfs/skeleton/etc/scripts/identity-trial.sh, off the device.
#
# apply.sh, flash and omcli are stubs: apply.sh writes the status file the way
# omcid does (src/omci/reload-test.sh checks omcid itself) with the result the
# case names, flash keeps KEY=value lines in a file, omcli prints the state and
# connections the case names. What is checked is the script: the values stay
# in the RAM file until the outcome is good, a bad outcome or a new MIB reset
# loop goes back with apply.sh omci --revert, a good one is stored (LOID_OLD
# too, where the store has it) after the watch, keep and revert from outside,
# a held change, the checks of try, and that no value reaches the state file,
# the log or the kernel log.
set -u
cd "$(dirname "$0")/.." || exit 1
S=$PWD/rootfs/skeleton/etc/scripts/identity-trial.sh
T=$(mktemp -d)
trap 'pkill -f "$T" 2>/dev/null; rm -rf "$T"' EXIT

pass=0 fail=0
ok() { echo "ok    $1"; pass=$((pass + 1)); }
no() { echo "FAIL  $1"; [ -n "${2:-}" ] && printf '%s\n' "$2" | sed 's/^/        /'; fail=$((fail + 1)); }
# t NAME PATTERN TEXT: TEXT matches the extended regex PATTERN.
t() { if [[ $3 =~ $2 ]]; then ok "$1"; else no "$1" "wanted /$2/, got: $3"; fi; }
n() { if [[ $3 =~ $2 ]]; then no "$1" "did not want /$2/, got: $3"; else ok "$1"; fi; }

mkdir -p "$T/bin"
# flash get/set over a KEY=value file; a key not in it is no key at all.
cat > "$T/bin/flash" <<EOF
#!/bin/sh
f="$T/store"
case \$1 in
get) grep "^\$2=" "\$f" || { echo "GET fail."; exit 1; } ;;
set) [ -f "$T/flash-fails" ] && exit 1
     grep -v "^\$2=" "\$f" > "\$f.new"; echo "\$2=\$3" >> "\$f.new"; mv "\$f.new" "\$f"; echo "\$2=\$3" ;;
esac
EOF
# apply.sh omci [--confirm|--revert] SECONDS: logs its arguments; a revert
# answers ok, anything else what $T/next says ("changed result refusals",
# after $T/delay seconds), as a new run in the status file.
cat > "$T/bin/apply" <<EOF
#!/bin/sh
echo "\$*" >> "$T/apply.log"
n=\$(( \$(cat "$T/seq" 2>/dev/null || echo 0) + 1 )); echo \$n > "$T/seq"
if [ "\$2" = --revert ]; then set -- identity ok 0
else [ -f "$T/delay" ] && sleep "\$(cat "$T/delay")"; set -- \$(cat "$T/next"); fi
printf 'id=1.%s\nstate=done\nchanged=%s\naction=reregister\nresult=%s\nrefusals=%s\n' \$n "\$1" "\$2" "\$3" > "$T/status"
[ "\$2" = ok ]
EOF
cat > "$T/bin/omcli" <<EOF
#!/bin/sh
case \$1 in
state) cat "$T/omcli-state" ;;
conn)  cat "$T/omcli-conn" ;;
esac
EOF
cat > "$T/bin/timeout" <<'EOF'
#!/bin/sh
shift
exec "$@"
EOF
cat > "$T/bin/setsid" <<'EOF'
#!/bin/sh
exec "$@"
EOF
chmod +x "$T/bin/"*

omcli_state() {   # omcli_state <onu> <loop>
	printf 'serial     : ODIT00000001\nonu state  : %s\nmib resets : olt 2  local 0  olt in last 600 s 1  loop %s  (loops 0)\n' "$1" "$2" > "$T/omcli-state"
}
run() {
	env PATH="$T/bin:$PATH" FLASH="$T/bin/flash" APPLY="$T/bin/apply" OMCLI="$T/bin/omcli" \
	    SELF="$S" STATE="$T/state" TRIAL="$T/trial.conf" REQ="$T/req" LOCKDIR="$T/lock" \
	    RELOAD_STATUS="$T/status" UPTIME="$T/uptime" KMSG="$T/kmsg" LOG="$T/log" \
	    WATCH_S="${WATCH:-2}" POLL_S=1 \
	    sh "$S" "$@" 2>&1
}
st() { sed -n "s/^$1=//p" "$T/state" 2>/dev/null | head -n 1; }
# settle: the background watcher ended (the phase is steady again).
settle() {
	for _ in $(seq 100); do [ "$(st phase)" = steady ] && return 0; sleep 0.1; done
	no "the trial did not end" "$(cat "$T/state" "$T/log" 2>/dev/null)"
	return 1
}
fresh() {
	rm -f "$T/state" "$T/trial.conf" "$T/req" "$T/apply.log" "$T/delay" "$T/flash-fails"
	printf 'GPON_SN=ODIT00000001\nGPON_PLOAM_PASSWD=oldsecret\nLOID=olduser\nLOID_OLD=olduser\nLOID_PASSWD=oldpw\n' > "$T/store"
	printf 'SERVID 0: Used: 1\nSERVID 1: Used: 1\n' > "$T/omcli-conn"
	omcli_state O5 no
	echo "1000.00 1.00" > "$T/uptime"
}

# ---- 1. a good change: watched, then stored
fresh
echo "identity ok 1" > "$T/next"
out=$(run try GPON_SN=ODIT0000ABCD LOID=newuser GPON_PLOAM_PASSWD=newsecret); rc=$?
t "good: try returns at once" "re-registers with the new values" "$out"
t "good: exit 0" "^0$" "$rc"
settle
t "good: kept" "^kept$" "$(st outcome)"
t "good: the reload result and its deactivations" "^ok 1$" "$(st result) $(st refusals)"
t "good: the serial is stored" "GPON_SN=ODIT0000ABCD" "$(cat "$T/store")"
t "good: LOID_OLD follows, as the store has it" "LOID_OLD=newuser" "$(cat "$T/store")"
n "good: no LOID_PASSWD_OLD made up" "LOID_PASSWD_OLD" "$(cat "$T/store")"
t "good: the RAM file is gone" "^absent$" "$([ -e "$T/trial.conf" ] && echo present || echo absent)"
t "good: apply.sh was told once, without a flag" "^omci 170$" "$(cat "$T/apply.log")"
t "good: before and after" "^O5 2 O5 2$" "$(st before_onu) $(st before_services) $(st after_onu) $(st after_services)"
n "good: no value in the state file" "newsecret|ODIT0000ABCD|newuser" "$(cat "$T/state")"
n "good: no value in the log or the kernel log" "newsecret|ODIT0000ABCD|newuser" "$(cat "$T/log" "$T/kmsg")"
t "good: the key names are logged" "event=identity_trial keys=GPON_SN,LOID,GPON_PLOAM_PASSWD state=kept" "$(cat "$T/kmsg")"

# ---- 2. refused by the OLT: back, with --revert
for r in refused timeout no_services sn_not_applied failed; do
	fresh
	echo "identity $r 3" > "$T/next"
	run try GPON_PLOAM_PASSWD=wrong > /dev/null
	settle
	t "$r: reverted, and why" "^reverted $r$" "$(st outcome) $(st reason)"
	t "$r: the store is untouched" "GPON_PLOAM_PASSWD=oldsecret" "$(cat "$T/store")"
	t "$r: the RAM file is gone" "^absent$" "$([ -e "$T/trial.conf" ] && echo present || echo absent)"
	t "$r: the revert went through the guard" "omci --revert 170$" "$(tail -n 1 "$T/apply.log")"
done

# ---- 3. a MIB reset loop that starts during the watch
fresh
echo "identity ok 0" > "$T/next"
WATCH=4 run try GPON_SN=ODIT0000ABCD > /dev/null
for _ in $(seq 50); do [ "$(st phase)" = watching ] && break; sleep 0.1; done
omcli_state O5 yes
settle
t "loop: reverted" "^reverted mib_reset_loop$" "$(st outcome) $(st reason)"
t "loop: the store is untouched" "GPON_SN=ODIT00000001" "$(cat "$T/store")"
t "loop: shown after" "^no yes$" "$(st before_loop) $(st after_loop)"

# A loop that was on before the trial is not the trial's.
fresh
omcli_state O5 yes
echo "identity ok 0" > "$T/next"
run try GPON_SN=ODIT0000ABCD > /dev/null
settle
t "loop before: kept" "^kept$" "$(st outcome)"

# ---- 4. keep and revert from outside
fresh
echo "identity ok 0" > "$T/next"
echo 2 > "$T/delay"
WATCH=30 run try LOID=newuser > /dev/null
out=$(run keep)
t "keep while applying: refused" "not yet" "$out"
for _ in $(seq 50); do [ "$(st phase)" = watching ] && break; sleep 0.1; done
out=$(run keep)
t "keep while watching: handed over" "keep handed to the trial" "$out"
settle
t "keep: stored before the window ends" "^kept$" "$(st outcome)"

fresh
echo "identity ok 0" > "$T/next"
WATCH=30 run try LOID=newuser > /dev/null
for _ in $(seq 50); do [ "$(st phase)" = watching ] && break; sleep 0.1; done
out=$(run try LOID=other)
t "a second try while one runs: refused" "already running" "$out"
run revert > /dev/null
settle
t "revert: asked" "^reverted asked$" "$(st outcome) $(st reason)"
t "revert: the store is untouched" "LOID=olduser" "$(cat "$T/store")"

# ---- 5. held by the re-registration guard
fresh
echo "identity held 0" > "$T/next"
run try --confirm GPON_SN=ODIT0000ABCD > /dev/null
settle
t "held: outcome" "^held$" "$(st outcome)"
t "--confirm reaches apply.sh" "^omci --confirm 170$" "$(cat "$T/apply.log")"
t "held: the RAM file is gone, no revert" "^absent 1$" "$([ -e "$T/trial.conf" ] && echo present || echo absent) $(wc -l < "$T/apply.log" | tr -d ' ')"

# ---- 6. the store refuses the keep
fresh
echo "identity ok 0" > "$T/next"
touch "$T/flash-fails"
out=$(run try GPON_SN=ODIT0000ABCD)
settle
t "store refused: said so, the RAM file stays" "^store_failed present$" "$(st outcome) $([ -e "$T/trial.conf" ] && echo present || echo absent)"

# ---- 7. what try refuses, and the RAM file itself
fresh
t "a serial number the driver would refuse" "four letters or digits and eight hex" "$(run try GPON_SN=ODIT00000G01)"
t "a key that is not an identity key" "not an identity key" "$(run try VLAN_MANU_TAG_VID=10)"
t "not KEY=VALUE" "is not KEY=VALUE" "$(run try GPON_SN)"
t "nothing asked for" "usage" "$(run try)"
echo "identity ok 0" > "$T/next"
echo 2 > "$T/delay"
run try GPON_PLOAM_PASSWD=newsecret > /dev/null
t "the RAM file: root only, the values" "^-rw------- GPON_PLOAM_PASSWD=newsecret$" \
	"$(ls -l "$T/trial.conf" | cut -c1-10) $(cat "$T/trial.conf")"
out=$(run status)
t "status: the phase and the file" "phase=applying" "$out"
t "status: the RAM file is there" "trial_file=present" "$out"
n "status: no value" "newsecret" "$out"
settle

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
