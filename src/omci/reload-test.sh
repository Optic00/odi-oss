#!/bin/sh
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

# The SIGHUP reload: what omcid does when apply.sh omci tells it to reread
# the config store (respond/reload.c, docs/SETTINGS.md "How apply works").
#
# Three things are proved here, against omcid-drvtrace (every driver call
# logged and answered with success, drv-test.sh) running under qemu-user:
#
#   1. A VLAN-only change is rebuilt in place, and the rules are the ones a
#      fresh omcid builds. For each ISP session and two new VLAN settings
#      (another tag, and transparent mode) the daemon is run on the old
#      settings, sent SIGHUP with the store changed, and its bridge-rule
#      driver calls (command 51) and connection dump after the reload are
#      compared byte for byte with a second daemon that ran the same session
#      with the new settings from its first frame. The MIB is compared
#      before and after too: a reload does not touch it.
#   2. An identity change re-registers without a restart: the driver verbs
#      arrive in the order rcS uses (gpondeact, gponsn, gponpw, gponact,
#      written to a file with -i), the MIB is cleared, the loop is still
#      served during the hold, the reload finishes when the ONU is back in O5
#      with its services, and the pid never changes. The failure paths (a
#      verb that fails, O5 never reached, O5 without services) and a burst
#      of SIGHUPs during a re-registration are covered.
#   2b. Every re-registration is recorded with the OLT answer (rereg.c):
#      a refusal (three Deactivate_ONU-IDs before O5, or O7), a timeout,
#      the guard that holds the third change back until it is confirmed,
#      a revert that the count skips, and the record surviving a restart.
#   3. Which key lands in which class, one key at a time, in a dry-run
#      instance (no driver, no waiting), including the rule that the report
#      keys count only while the identity switch is on.
#
# Run from src/omci, inside the toolchain image, after drv-test.sh:
#   docker run --rm -v "$PWD/../..":/src -w /src/src/omci "$(../../toolchain/image.sh diag)" \
#          sh reload-test.sh
set -u
Q=qemu-mips-static
FIX=../../test/fixtures
fail=0
. ./testlib.sh

CAPS=020000ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00000000000000010000000300000002ffffffff000000000000001000000040000000800000000800000000000000100000000f00000440

CS=/var/config/lastgood.xml
HS=/var/config/lastgood_hs.xml
ODI=/var/config/odi.conf
SW=/var/config/omci-identity.on
STATUS=/var/run/omcid-reload
INITLOG=/tmp/rl-init
REREG=/var/config/odi-reregister
CONFIRM=/var/run/omcid-reregister.confirm
PLOAM=/tmp/rl-ploam

mkdir -p /var/config /var/run
clean_state() {
	rm -f "$HS" "$ODI" "$SW" "$REREG" "$CONFIRM" /var/run/omcid-mib.snap /var/run/omcid-mib.snap.tmp \
	      /var/run/omcid-resume-decision "$STATUS" "$INITLOG"
	: > "$INITLOG"
}

# The whole MIB, without what is live or follows the stored VLAN keys and so
# differs between two dumps by design: SysUpTime, and the private VLAN
# configuration (class 65535), which shows the stored manual VLAN.
mibdump() {
	$Q cli/build/omcli mib get all 2>&1 | grep -v '^    SysUpTime ' |
		awk '/^65535 /{skip=1; next} /^[0-9]+ /{skip=0} !skip'
}

ok()  { echo "ok    $*"; }
bad() { echo "FAIL  $*"; fail=$((fail + 1)); }
check() {   # check <label> <want> <got>
	if [ "$2" = "$3" ]; then ok "$1"; else bad "$1: want '$2', got '$3'"; fi
}
# The line may trail what the check waited for: omcid writes its status file
# before the event line reaches the log through qemu, so a line that is not
# there yet gets up to 5 s.
check_grep() {   # check_grep <label> <pattern> <file>
	i=0
	while ! grep -q -- "$2" "$3" && [ "$i" -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
	if grep -q -- "$2" "$3"; then ok "$1"; else bad "$1: no '$2' in $3"; sed -n '1,3p' "$3"; fi
}

# The config store: the four VLAN keys, then whatever extra <Value> lines.
store() {   # store <mode> <vid> <pri> [extra lines]
	cat > "$CS" <<EOF
<Config Name="ROOT">
	<Dir Name="MIB_TABLE">
		<Value Name="VLAN_CFG_TYPE" Value="1"/>
		<Value Name="VLAN_MANU_MODE" Value="$1"/>
		<Value Name="VLAN_MANU_TAG_VID" Value="$2"/>
$(if [ -n "$3" ]; then printf '\t\t<Value Name="VLAN_MANU_TAG_PRI" Value="%s"/>\n' "$3"; fi)
${4:-}	</Dir>
</Config>
EOF
}
hs_sn() {   # hs_sn <label>
	cat > "$HS" <<EOF
<Config Name="ROOT">
	<Dir Name="HW_MIB_TABLE">
		<Value Name="GPON_SN" Value="$1"/>
	</Dir>
</Config>
EOF
}
val() { printf '\t\t<Value Name="%s" Value="%s"/>\n' "$1" "$2"; }

gpon() {   # gpon <file> <state> <alloc_ids line value>
	printf 'state %s (O%s)\nonu_id 26\nsn 4f44495400000001\neqd multiframe 0 inframe 0\nploam ds_rx 0 us_tx 0\nalloc_ids %s\n' \
		"$2" "$2" "$3" > "$1"
}
gpon /tmp/rl-gpon-isp1 5 "5 282 794 1050 1306 538"
gpon /tmp/rl-gpon-isp2 5 "1 348"

st() { sed -n "s/^$1=//p" "$STATUS" 2>/dev/null | head -n 1; }

# wait_count <file> <pattern> <count> <seconds>: the pattern shows at least
# <count> times within the time.
wait_count() {
	i=0
	while [ "$i" -lt $(( $4 * 10 )) ]; do
		[ "$(grep -c -- "$2" "$1")" -ge "$3" ] && return 0
		sleep 0.1
		i=$((i + 1))
	done
	return 1
}
# wait_done <previous id> <seconds>: the status file names a finished run
# other than the previous one.
wait_done() {
	i=0
	while [ "$i" -lt $(( $2 * 10 )) ]; do
		[ "$(st state)" = "done" ] && [ "$(st id)" != "$1" ] && return 0
		sleep 0.1
		i=$((i + 1))
	done
	return 1
}

# start_omcid <log> [extra args]: the traced daemon, in the background;
# sets pid. -d: it runs until told to stop.
start_omcid() {
	log=$1
	shift
	: > "$log"
	$Q respond/build/omcid-drvtrace -a -d -c "$CAPS" -i "$INITLOG" "$@" > "$log" 2>&1 &
	pid=$!
	wait_count "$log" 'answering as' 1 30 || bad "$log: omcid did not come up"
}
stop_omcid() {
	kill "$pid" 2>/dev/null
	wait "$pid" 2>/dev/null
}
inject() {   # inject <isp>
	$Q cli/build/omcli --inject-file "$FIX/omci-session-$1.txt" > /tmp/rl-inject.txt 2>&1
	frames=$(grep -c '^[0-9a-f]' "$FIX/omci-session-$1.txt")
	grep -q "injected $frames frames" /tmp/rl-inject.txt || bad "$1: omcli did not inject all $frames frames"
}
# settle <log> <rebuilds>: the quiet second after the last frame has passed
# and the bridge connections have been rebuilt <rebuilds> times in all.
settle() {
	wait_count "$1" 'bridge connections rebuilt' "$2" 15 || bad "$1: no bridge rebuild after the session"
	sync_send "$1"     # whatever the rebuild queued behind it has been served
}
rules_in_place() {   # the bridge rules of the rebuild a reload made
	awk '/event=reload changed=vlan/{on=1} on&&/^drv 51 /{print} on&&/bridge connections rebuilt/{exit}' "$1"
}
rules_last() {       # the bridge rules of the last rebuild of a run
	awk '/^drv 51 /{b=b $0 "\n"} /bridge connections rebuilt/{last=b; b=""} END{printf "%s", last}' "$1"
}

# ------------------------------------------------ 1. VLAN, in place vs fresh
#
# gpon file per ISP as drv-test.sh gives them: ISP1 binds five PLOAM Alloc-IDs.
for isp in isp1 isp2; do
	case $isp in
	isp1) vid0=11 ;;
	*)    vid0=10 ;;
	esac
	for variant in tag transparent; do
		case $variant in
		tag)         mode=1; vid=20; pri=3; keys=VLAN_MANU_TAG_VID,VLAN_MANU_TAG_PRI
			     [ "$vid0" = 20 ] && vid=21 ;;
		transparent) mode=0; vid=$vid0; pri=0; keys=VLAN_MANU_MODE ;;
		esac
		label="$isp -> $variant"

		# In place: the old settings, the session, then the change.
		clean_state
		store 1 "$vid0" 0
		log=/tmp/rl-inplace.log
		start_omcid "$log" -g /tmp/rl-gpon-$isp
		inject "$isp"
		settle "$log" 1
		rules_last "$log" > /tmp/rl-rules-before.txt
		mib_before=$(mibdump)
		conn_before=$($Q cli/build/omcli conn 2>&1)
		store "$mode" "$vid" "$pri"
		old_id=$(st id)
		kill -HUP "$pid"
		if ! wait_done "$old_id" 15; then
			bad "$label: the reload did not finish"
			cat "$STATUS" 2>/dev/null
		fi
		mib_after=$(mibdump)
		conn_after=$($Q cli/build/omcli conn 2>&1)
		rules_in_place "$log" > /tmp/rl-rules-inplace.txt
		alive=$(kill -0 "$pid" 2>/dev/null && echo yes || echo no)
		starts=$(grep -c 'event=start ' "$log")
		stop_omcid

		check_grep "$label: reload logged as a rebuild" \
			"event=reload changed=vlan action=rebuild keys=$keys\$" "$log"
		check_grep "$label: reload logged as done" \
			"event=reload_done changed=vlan action=rebuild result=ok" "$log"
		check "$label: the process is the same one, never restarted" "yes:1" "$alive:$starts"
		check "$label: the status file says done, ok, vlan" "done ok vlan rebuild" \
			"$(st state) $(st result) $(st changed) $(st action)"
		check "$label: the MIB is untouched by the reload" "$mib_before" "$mib_after"
		if [ -s /tmp/rl-rules-inplace.txt ]; then ok "$label: the reload sent bridge rules ($(wc -l < /tmp/rl-rules-inplace.txt))"
		else bad "$label: the reload sent no bridge rules"; fi
		if cmp -s /tmp/rl-rules-before.txt /tmp/rl-rules-inplace.txt; then
			bad "$label: the rules did not change with the setting"
		else ok "$label: the rules differ from the ones before the reload"; fi

		# Fresh: the new settings from the first frame.
		clean_state
		store "$mode" "$vid" "$pri"
		flog=/tmp/rl-fresh.log
		start_omcid "$flog" -g /tmp/rl-gpon-$isp
		inject "$isp"
		settle "$flog" 1
		rules_last "$flog" > /tmp/rl-rules-fresh.txt
		conn_fresh=$($Q cli/build/omcli conn 2>&1)
		stop_omcid

		if cmp -s /tmp/rl-rules-inplace.txt /tmp/rl-rules-fresh.txt; then
			ok "$label: the rebuilt rules are byte for byte a fresh daemon's"
		else
			bad "$label: rebuilt rules differ from a fresh daemon's"
			diff /tmp/rl-rules-inplace.txt /tmp/rl-rules-fresh.txt | head -20
		fi
		check "$label: the connection dump equals a fresh daemon's" "$conn_fresh" "$conn_after"
		if [ "$conn_before" = "$conn_after" ] && [ "$variant" = tag ]; then
			bad "$label: the connection dump did not change with the VID"
		fi
	done
done

# ------------------------------------------------ unchanged, and an old store
clean_state
store 1 11 0
log=/tmp/rl-none.log
start_omcid "$log" -g /tmp/rl-gpon-isp1
inject isp1
settle "$log" 1
: > "$INITLOG"
drv_before=$(grep -c '^drv ' "$log")
kill -HUP "$pid"
wait_count "$log" 'event=reload_done' 1 10 || bad "unchanged: no reload_done"
drv_after=$(grep -c '^drv ' "$log")
check_grep "unchanged: logged as changed=none" "event=reload changed=none action=none\$" "$log"
check "unchanged: no driver call" "$drv_before" "$drv_after"
check "unchanged: no driver verb" "0" "$(wc -c < "$INITLOG" | tr -d ' ')"
check "unchanged: status done none" "done none ok" "$(st state) $(st changed) $(st result)"
stop_omcid

# ------------------------------------------------ VLAN_EAPOL_UNTAGGED, latched
# The key is held with the VLAN keys: an edit of odi.conf changes nothing
# until the reload, even when the OLT makes omcid rebuild before it (here the
# session sent again), and the reload then sees the change and rebuilds.
clean_state
store 1 10 0
log=/tmp/rl-eapol.log
start_omcid "$log" -g /tmp/rl-gpon-isp2
inject isp2
settle "$log" 1
rules_last "$log" > /tmp/rl-eapol-off.txt
printf 'VLAN_EAPOL_UNTAGGED=1\n' > "$ODI"
inject isp2
settled "$log"
rules_last "$log" > /tmp/rl-eapol-edited.txt
if cmp -s /tmp/rl-eapol-off.txt /tmp/rl-eapol-edited.txt; then
	ok "EAPOL key set, no reload: a rebuild by the OLT keeps the rules without it"
else
	bad "EAPOL key set, no reload: a rebuild by the OLT changed the rules"
	diff /tmp/rl-eapol-off.txt /tmp/rl-eapol-edited.txt | head -10
fi
old_id=$(st id)
kill -HUP "$pid"
wait_done "$old_id" 15 || bad "EAPOL key set: the reload did not finish"
check_grep "EAPOL key set: the reload sees it" \
	"event=reload changed=vlan action=rebuild keys=VLAN_EAPOL_UNTAGGED\$" "$log"
rules_in_place "$log" > /tmp/rl-eapol-on.txt
check "EAPOL key set: the reload adds the three EAPOL rules" \
	"$(( $(wc -l < /tmp/rl-eapol-off.txt) + 3 ))" "$(wc -l < /tmp/rl-eapol-on.txt | tr -d ' ')"
printf 'VLAN_EAPOL_UNTAGGED=0\n' > "$ODI"
inject isp2
settled "$log"
rules_last "$log" > /tmp/rl-eapol-edited.txt
if cmp -s /tmp/rl-eapol-on.txt /tmp/rl-eapol-edited.txt; then
	ok "EAPOL key cleared, no reload: a rebuild by the OLT keeps the EAPOL rules"
else
	bad "EAPOL key cleared, no reload: a rebuild by the OLT changed the rules"
	diff /tmp/rl-eapol-on.txt /tmp/rl-eapol-edited.txt | head -10
fi
old_id=$(st id)
done_n=$(grep -c 'event=reload_done' "$log")
kill -HUP "$pid"
wait_done "$old_id" 15 || bad "EAPOL key cleared: the reload did not finish"
# The status file can read done a few milliseconds before the rebuild of the
# reload is logged; the rules are read once its end line is there.
wait_count "$log" 'event=reload_done' $((done_n + 1)) 15 || bad "EAPOL key cleared: no end line of the reload"
check "EAPOL key cleared: the reload sees it" \
	"event=reload changed=vlan action=rebuild keys=VLAN_EAPOL_UNTAGGED" \
	"$(grep 'event=reload ' "$log" | tail -n 1)"
rules_last "$log" > /tmp/rl-eapol-back.txt
if cmp -s /tmp/rl-eapol-off.txt /tmp/rl-eapol-back.txt; then
	ok "EAPOL key cleared: the rules are the ones without it, byte for byte"
else
	bad "EAPOL key cleared: the rules differ from the ones without it"
	diff /tmp/rl-eapol-off.txt /tmp/rl-eapol-back.txt | head -10
fi
stop_omcid
rm -f "$ODI"

# ------------------------------------------------ 2. identity: re-register
clean_state
store 1 11 0 "$(val GPON_PLOAM_PASSWD 3132333435)
"
hs_sn ODIT00000001
gpon /tmp/rl-gpon-isp1 5 "5 282 794 1050 1306 538"
log=/tmp/rl-ident.log
start_omcid "$log" -g /tmp/rl-gpon-isp1
inject isp1
settle "$log" 1
mib_before=$(mibdump)
conn_before=$($Q cli/build/omcli conn 2>&1)
: > "$INITLOG"
old_id=$(st id)

# The new serial number, and the ONU never sees O5 until the test says so.
hs_sn ODIT00000002
gpon /tmp/rl-gpon-isp1 1 "0"
kill -HUP "$pid"
wait_count "$INITLOG" gpondeact 1 5 || bad "identity: no gpondeact"
# Elapsed time on purpose: the daemon holds the ONU down for a second after
# gpondeact, and nothing else may be written meanwhile.
sleep 1
check "identity: still held down a second in (only gpondeact so far)" "gpondeact" \
	"$(tr '\n' ' ' < "$INITLOG" | sed 's/ $//')"
check "identity: running, MIB cleared, no services" "running 0" "$(st state) $(st services)"
served=$($Q cli/build/omcli state 2>&1 | wc -l | tr -d ' ')
[ "$served" -gt 0 ] && ok "identity: the loop still answers the CLI during the hold" \
	|| bad "identity: the CLI got no answer during the hold"
wait_count "$INITLOG" gponact 1 8 || bad "identity: no gponact"
check "identity: the verbs, in rcS order" "gpondeact gponsn ODIT00000002 gponpw 3132333435 gponact" \
	"$(tr '\n' ' ' < "$INITLOG" | sed 's/ $//')"
check_grep "identity: the MIB reset is logged" "event=mib_reset side=local" "$log"
check "identity: activated, not yet done" "running activated" "$(st state) $(st result)"

# The OLT: O5 and the session again.
gpon /tmp/rl-gpon-isp1 5 "5 282 794 1050 1306 538"
inject isp1
if wait_done "$old_id" 20; then ok "identity: finished once O5 and the services were back"
else bad "identity: never finished"; cat "$STATUS"; fi
check "identity: result ok" "done ok identity reregister" \
	"$(st state) $(st result) $(st changed) $(st action)"
[ -n "$(st o5_ms)" ] && ok "identity: the time to O5 is reported ($(st o5_ms) ms)" || bad "identity: no o5_ms"
[ "$(st services)" -gt 0 ] && ok "identity: services are back ($(st services))" || bad "identity: no services"
check_grep "identity: logged" "event=reload changed=identity action=reregister keys=GPON_SN\$" "$log"
check_grep "identity: done logged" "event=reload_done changed=identity action=reregister result=ok" "$log"
check "identity: the process was never restarted" "1" "$(grep -c 'event=start ' "$log")"
kill -0 "$pid" 2>/dev/null && ok "identity: the process is still the same one" || bad "identity: the process died"
check "identity: the serial number was read from the kernel again" "2" \
	"$(grep -c 'serial number: from the kernel' "$log")"
sync_send "$log"
check "identity: the MIB is what the session builds" "$mib_before" "$(mibdump)"
check "identity: the connections are what the session builds" "$conn_before" "$($Q cli/build/omcli conn 2>&1)"
stop_omcid

# ------------------------------------------------ the failure paths
# -j 4: give up on O5 after four seconds instead of 150.
clean_state
store 1 11 0 "$(val LOID_OLD user1)
$(val LOID user1)
"
gpon /tmp/rl-gpon-isp1 1 "0"
log=/tmp/rl-fail.log
start_omcid "$log" -g /tmp/rl-gpon-isp1 -j 4
old_id=$(st id)
store 1 11 0 "$(val LOID_OLD user2)
$(val LOID user2)
"
kill -HUP "$pid"
if wait_done "$old_id" 20; then
	check "O5 never reached: result timeout" "done timeout" "$(st state) $(st result)"
else bad "O5 never reached: never gave up"; fi
check_grep "O5 never reached: logged" "event=reload_done changed=identity action=reregister result=timeout" "$log"
check "O5 never reached: no gponsn (serial unchanged)" "gpondeact gponact" \
	"$(tr '\n' ' ' < "$INITLOG" | sed 's/ $//')"

# O5 without services, and a burst of SIGHUPs during the wait: exactly one
# more reload follows, and it finds nothing to do.
gpon /tmp/rl-gpon-isp1 5 "0"
old_id=$(st id)
store 1 11 0 "$(val LOID_OLD user3)
$(val LOID user3)
"
before=$(grep -c 'event=reload ' "$log")
kill -HUP "$pid"
wait_count "$INITLOG" gponact 2 10 || bad "no second gponact"
kill -HUP "$pid"; kill -HUP "$pid"; kill -HUP "$pid"
# The coalesced reload finishes within milliseconds of this one and takes the
# status file, so the outcome is read from the log line.
wait_count "$log" 'event=reload_done .*result=no_services' 1 20 || bad "O5 without services: never gave up"
check_grep "O5 without services: result no_services" \
	"event=reload_done changed=identity action=reregister result=no_services" "$log"
wait_count "$log" 'event=reload changed=none' 1 10 || bad "the coalesced SIGHUPs led to no reload"
# Elapsed time on purpose: the check is that no further reload follows.
sleep 1
after=$(grep -c 'event=reload ' "$log")
check "three SIGHUPs during a reload: one reload on top of the running one" "2" "$((after - before))"
stop_omcid

# A verb that fails: the daemon goes back to what it was running with.
clean_state
store 1 11 0 "$(val LOID_OLD user1)
$(val LOID user1)
"
gpon /tmp/rl-gpon-isp1 5 "5 282 794 1050 1306 538"
log=/tmp/rl-verbfail.log
start_omcid "$log" -g /tmp/rl-gpon-isp1 -i /nonexistent/odi_init
inject isp1
settle "$log" 1
mib_before=$(mibdump)
old_id=$(st id)
store 1 11 0 "$(val LOID_OLD user2)
$(val LOID user2)
"
# An EAPOL edit with it: the failed re-registration puts the old value back
# with the rest, so the retry still names it.
printf 'VLAN_EAPOL_UNTAGGED=1\n' > "$ODI"
kill -HUP "$pid"
wait_done "$old_id" 10 || bad "verb failure: no outcome"
check "verb failure: result failed" "done failed" "$(st state) $(st result)"
check_grep "verb failure: the step is logged" "event=reload_step verb=gpondeact rc=2" "$log"
check "verb failure: the MIB was not touched" "$mib_before" "$(mibdump)"
old_id=$(st id)
kill -HUP "$pid"
wait_done "$old_id" 10 || bad "verb failure: no outcome on the retry"
check "verb failure: the next SIGHUP still sees the change and tries again" "identity failed" \
	"$(st changed) $(st result)"
check "verb failure: and still names the EAPOL key" \
	"event=reload changed=identity action=reregister keys=LOID,VLAN_EAPOL_UNTAGGED" \
	"$(grep 'event=reload ' "$log" | tail -n 1)"
stop_omcid

# ------------------------------------------------ 3. which key, which class
# A dry-run daemon (no -a, no driver): every reload is decided and logged,
# and a re-registration completes at once, so a long list of edits is quick.
clean_state
store 1 11 0
log=/tmp/rl-keys.log
: > "$log"
$Q respond/build/omcid -d -c "$CAPS" -g /tmp/rl-gpon-isp1 > "$log" 2>&1 &
pid=$!
wait_count "$log" 'answering as' 1 30 || bad "$log: omcid did not come up"
nrel=0
# reload_is <label> <changed> <keys>: SIGHUP, then the line that came of it.
reload_is() {
	nrel=$((nrel + 1))
	kill -HUP "$pid"
	wait_count "$log" 'event=reload_done' "$nrel" 10 || { bad "$1: no reload"; tail -n 6 "$log"; return; }
	line=$(grep 'event=reload ' "$log" | sed -n "${nrel}p")
	if [ -n "${3:-}" ]; then
		check "$1" "event=reload changed=$2 action=$4 keys=$3" "$line"
	else
		check "$1" "event=reload changed=$2 action=$4" "$line"
	fi
}
reload_is "same store"                         none ""                    none
store 1 11 ""
reload_is "VLAN_MANU_TAG_PRI removed (absent reads as 0, but the tag stops applying)" vlan VLAN_MANU_TAG_PRI rebuild
store 1 11 0
reload_is "VLAN_MANU_TAG_PRI back, still 0" vlan VLAN_MANU_TAG_PRI     rebuild
store 1 11 5
reload_is "VLAN_MANU_TAG_PRI"                  vlan VLAN_MANU_TAG_PRI     rebuild
store 1 12 5
reload_is "VLAN_MANU_TAG_VID"                  vlan VLAN_MANU_TAG_VID     rebuild
# Bit 1 of OMCI_CUSTOM_MCAST changes the multicast rule (309 attribute 16
# forced to strip): a rebuild. Its other bits select stock plugins omcid does
# not act on.
store 1 12 5 "$(val OMCI_CUSTOM_MCAST 1)
"
reload_is "OMCI_CUSTOM_MCAST bit 1"            vlan OMCI_CUSTOM_MCAST     rebuild
store 1 12 5 "$(val OMCI_CUSTOM_MCAST 3)
"
reload_is "OMCI_CUSTOM_MCAST 1 to 3, bit 1 kept: not a change" none ""   none
store 1 12 5
reload_is "OMCI_CUSTOM_MCAST removed"          vlan OMCI_CUSTOM_MCAST     rebuild
# The stock FwdOp override changes the rules of the ports it names: a rebuild.
store 1 12 5 "$(val RTK_DEVINFO_SPECVER 0x1102:0x00)
"
reload_is "RTK_DEVINFO_SPECVER"                vlan RTK_DEVINFO_SPECVER   rebuild
store 1 12 5 "$(val RTK_DEVINFO_SPECVER 0x1102:0x02)
"
reload_is "RTK_DEVINFO_SPECVER, another code"  vlan RTK_DEVINFO_SPECVER   rebuild
store 1 12 5
reload_is "RTK_DEVINFO_SPECVER removed"        vlan RTK_DEVINFO_SPECVER   rebuild
# VLAN_EAPOL_UNTAGGED, an odi-only key in odi.conf, changes the rules of the
# manual tag: a rebuild, and 0 for absent is not a change.
store 1 12 5
printf 'VLAN_EAPOL_UNTAGGED=1\n' > "$ODI"
reload_is "VLAN_EAPOL_UNTAGGED 1"              vlan VLAN_EAPOL_UNTAGGED   rebuild
printf 'VLAN_EAPOL_UNTAGGED=0\n' > "$ODI"
reload_is "VLAN_EAPOL_UNTAGGED 0"              vlan VLAN_EAPOL_UNTAGGED   rebuild
rm -f "$ODI"
reload_is "VLAN_EAPOL_UNTAGGED absent, still off: not a change" none ""   none
store 0 12 5
reload_is "VLAN_MANU_MODE 0, transparent"      vlan VLAN_MANU_MODE        rebuild
store 0 12 5 "$(val VLAN_CFG_TYPE 2)
"
# A second VLAN_CFG_TYPE line is ignored: the first one wins, as elsewhere.
reload_is "a duplicate key is not a change"    none ""                    none
store 1 12 5 "$(val OMCI_SW_VER1 V1.2.3)
"
reload_is "VLAN_MANU_MODE back, and an SW version while the switch is off" vlan VLAN_MANU_MODE rebuild
: > "$SW"
reload_is "the identity switch turned on"      identity "omci-identity.on" reregister
store 1 12 5 "$(val OMCI_SW_VER1 V1.2.4)
"
reload_is "OMCI_SW_VER1"                       identity OMCI_SW_VER1      reregister
store 1 12 5 "$(val OMCI_SW_VER1 V1.2.4)
$(val OMCI_SW_VER2 V2.0)
"
reload_is "OMCI_SW_VER2"                       identity OMCI_SW_VER2      reregister
store 1 12 5 "$(val OMCI_SW_VER1 V1.2.4)
$(val OMCI_SW_VER2 V2.0)
$(val OMCC_VER 160)
"
reload_is "OMCC_VER"                           identity OMCC_VER          reregister
store 1 12 5 "$(val OMCI_SW_VER1 V1.2.4)
$(val OMCI_SW_VER2 V2.0)
$(val OMCC_VER 160)
$(val OMCI_VENDOR_PRODUCT_CODE 4242)
"
reload_is "OMCI_VENDOR_PRODUCT_CODE"           identity OMCI_VENDOR_PRODUCT_CODE reregister
X="$(val OMCI_SW_VER1 V1.2.4)
$(val OMCI_SW_VER2 V2.0)
$(val OMCC_VER 160)
$(val OMCI_VENDOR_PRODUCT_CODE 4242)
"
store 1 12 5 "$X$(val GPON_ONU_MODEL MODEL-X)
"
reload_is "GPON_ONU_MODEL"                     identity GPON_ONU_MODEL    reregister
X="$X$(val GPON_ONU_MODEL MODEL-X)
"
printf 'ONU_HW_VERSION=HW-2\n' > "$ODI"
reload_is "ONU_HW_VERSION"                     identity ONU_HW_VERSION    reregister
printf 'ONU_HW_VERSION=HW-2\nOMCI_UNKNOWN_ME_OK=1\n' > "$ODI"
reload_is "OMCI_UNKNOWN_ME_OK"                 identity OMCI_UNKNOWN_ME_OK reregister
printf 'ONU_HW_VERSION=HW-2\nOMCI_UNKNOWN_ME_OK=1\nOMCI_FIXED_ME=200:0102\n' > "$ODI"
reload_is "OMCI_FIXED_ME"                     identity OMCI_FIXED_ME reregister
printf 'ONU_HW_VERSION=HW-2\nOMCI_UNKNOWN_ME_OK=1\nOMCI_FIXED_ME=200:0102\nOMCI_PRIVATE_ME=0\n' > "$ODI"
reload_is "OMCI_PRIVATE_ME"                   identity OMCI_PRIVATE_ME reregister
printf 'ONU_HW_VERSION=HW-2\nOMCI_UNKNOWN_ME_OK=1\nOMCI_FIXED_ME=200:0102\nOMCI_PRIVATE_ME=0\nOLT_SW_DOWNLOAD=reject\nOMCI_ACCEPT_REBOOT=1\nSYSLOG_SERVER=10.0.0.1\n' > "$ODI"
reload_is "OLT_SW_DOWNLOAD, OMCI_ACCEPT_REBOOT and other odi keys are not a reload" none ""   none
# OMCI_POTS_PORTS: 0 is what an absent key reads as, so writing it is no
# change; 2 is, and so is a value out of range, which reads as 0 again. The
# classes the ONU accepts change with it, so it re-registers.
ODIX='ONU_HW_VERSION=HW-2\nOMCI_UNKNOWN_ME_OK=1\nOMCI_FIXED_ME=200:0102\nOMCI_PRIVATE_ME=0\nOLT_SW_DOWNLOAD=reject\nOMCI_ACCEPT_REBOOT=1\nSYSLOG_SERVER=10.0.0.1\n'
# shellcheck disable=SC2059  # ODIX is the format on purpose: its \n are the lines
printf "${ODIX}OMCI_POTS_PORTS=0\n" > "$ODI"
reload_is "OMCI_POTS_PORTS=0 is the same as absent" none ""                none
# shellcheck disable=SC2059
printf "${ODIX}OMCI_POTS_PORTS=2\n" > "$ODI"
reload_is "OMCI_POTS_PORTS"                    identity OMCI_POTS_PORTS   reregister
# shellcheck disable=SC2059
printf "${ODIX}OMCI_POTS_PORTS=3\n" > "$ODI"
reload_is "OMCI_POTS_PORTS out of range reads as 0, a change back" identity OMCI_POTS_PORTS reregister
# OMCI_POTS_SLOT_ID moves the phone ports, which the OLT has to see again;
# with no ports it changes nothing.
# shellcheck disable=SC2059
printf "${ODIX}OMCI_POTS_SLOT_ID=9\n" > "$ODI"
reload_is "OMCI_POTS_SLOT_ID with no ports is not a change" none ""      none
# shellcheck disable=SC2059
printf "${ODIX}OMCI_POTS_SLOT_ID=9\nOMCI_POTS_PORTS=1\n" > "$ODI"
reload_is "OMCI_POTS_PORTS on, in slot 9"      identity OMCI_POTS_PORTS   reregister
# shellcheck disable=SC2059
printf "${ODIX}OMCI_POTS_SLOT_ID=11\nOMCI_POTS_PORTS=1\n" > "$ODI"
reload_is "OMCI_POTS_SLOT_ID"                  identity OMCI_POTS_SLOT_ID reregister
# shellcheck disable=SC2059
printf "${ODIX}OMCI_POTS_SLOT_ID=11\nOMCI_POTS_PORTS=2\n" > "$ODI"
reload_is "OMCI_POTS_PORTS from 1 to 2"        identity OMCI_POTS_PORTS   reregister
# shellcheck disable=SC2059
printf "${ODIX}OMCI_POTS_PORTS=0\n" > "$ODI"
reload_is "OMCI_POTS_PORTS off, the slot key gone with it" identity OMCI_POTS_PORTS reregister
X="$X$(val OMCI_VEIP_SLOT_ID 14)
"
store 1 12 5 "$X"
reload_is "OMCI_VEIP_SLOT_ID"                  identity OMCI_VEIP_SLOT_ID reregister
X="$X$(val OMCI_CUSTOM_ME 65792)
"
store 1 12 5 "$X"
reload_is "OMCI_CUSTOM_ME bit 256 puts the VEIP in the slot" identity OMCI_VEIP_SLOT_ID reregister
store 1 12 5 "$X$(val GPON_PLOAM_PASSWD 3132333435)
"
reload_is "GPON_PLOAM_PASSWD"                  identity GPON_PLOAM_PASSWD reregister
store 1 12 5 "$X$(val GPON_PLOAM_PASSWD 3132333435)
$(val LOID_OLD u1)
$(val LOID u1)
"
reload_is "LOID"                               identity LOID              reregister
store 1 12 5 "$X$(val GPON_PLOAM_PASSWD 3132333435)
$(val LOID_OLD u1)
$(val LOID u1)
$(val LOID_PASSWD p1)
$(val LOID_PASSWD_OLD p1)
"
reload_is "LOID_PASSWD"                        identity LOID_PASSWD       reregister
hs_sn ODIT00000009
reload_is "GPON_SN"                            identity GPON_SN           reregister
hs_sn ODIT00000009
store 1 13 5 "$X$(val GPON_PLOAM_PASSWD 3132333436)
$(val LOID_OLD u1)
$(val LOID u1)
$(val LOID_PASSWD p1)
$(val LOID_PASSWD_OLD p1)
"
reload_is "an identity and a VLAN key together" identity "GPON_PLOAM_PASSWD,VLAN_MANU_TAG_VID" reregister
rm -f "$SW"
store 1 13 5 "$X$(val GPON_PLOAM_PASSWD 3132333436)
$(val LOID_OLD u1)
$(val LOID u1)
$(val LOID_PASSWD p1)
$(val LOID_PASSWD_OLD p1)
"
reload_is "the identity switch turned off"     identity "omci-identity.on" reregister
store 1 13 5 "$(val OMCI_VEIP_SLOT_ID 14)
$(val OMCI_CUSTOM_ME 65792)
$(val OMCI_SW_VER1 V9)
$(val GPON_PLOAM_PASSWD 3132333436)
$(val LOID_OLD u1)
$(val LOID u1)
$(val LOID_PASSWD p1)
$(val LOID_PASSWD_OLD p1)
"
reload_is "a report key while the switch is off is not a reload" none "" none
kill -0 "$pid" 2>/dev/null && ok "dry run: still the same process after $nrel reloads" || bad "dry run: died"
check "dry run: never restarted" "1" "$(grep -c 'event=start ' "$log")"
check "dry run: no driver verb was written" "0" "$(wc -c < "$INITLOG" | tr -d ' ')"
stop_omcid

# ------------------------------------------------ 2b. record and guard
#
# The ploam file stands in for /proc/odi_gpon/ploam: only the type count of
# Deactivate_ONU-ID matters. -j 4 as for the failure paths.
ploam() { printf 'ploam ds_rx 10 us_tx 10\nploam_type ds 0x01 4\nploam_type ds 0x05 %s\nploam_ring 0 entries\n' "$1" > "$PLOAM"; }
rr_line() { sed -n "${1}p" "$REREG" 2>/dev/null | cut -d' ' -f2-; }
rr_state() { $Q cli/build/omcli state 2>&1 | sed -n 's/^reregister : //p'; }
# change <serial>: a new serial in the store, the signal, the outcome.
change() {
	hs_sn "$1"
	old_id=$(st id)
	kill -HUP "$pid"
	wait_done "$old_id" 15 || bad "$1: the reload did not finish"
}

clean_state
ploam 2
store 1 11 0 "$(val LOID_OLD user1)
$(val LOID user1)
"
hs_sn ODIT00000001
gpon /tmp/rl-gpon-isp1 5 "0"
log=/tmp/rl-rereg.log
start_omcid "$log" -g /tmp/rl-gpon-isp1 -P "$PLOAM" -j 4
check "record: none at the start" "0 recorded  refused in a row 0" \
	"$(rr_state | sed 's/  accepted.*//')"

# 1. Refused: the OLT deactivates the ONU three times before O5. One or two
# do not end the wait (an OLT may need a few windows); the third does.
gpon /tmp/rl-gpon-isp1 3 "0"
hs_sn ODIT00000002
old_id=$(st id)
kill -HUP "$pid"
wait_count "$INITLOG" gponact 1 8 || bad "refused: no gponact"
ploam 4
sleep 1.5
check "refused: two deactivations are not yet a refusal" "running" "$(st state)"
ploam 5
if wait_done "$old_id" 3; then ok "refused: the third ends the wait before the deadline"
else bad "refused: the wait did not end"; fi
check "refused: result and count" "refused 3 1" "$(st result) $(st refusals) $(st streak)"
check_grep "refused: logged" "event=reload_done changed=identity action=reregister result=refused .*refusals=3" "$log"
check "refused: recorded" "c refused 3 GPON_SN" "$(rr_line 1)"

# 2. Timeout: no O5, no deactivation (an unknown serial is never ranged).
change ODIT00000003
check "timeout: result, two in a row" "timeout 0 2" "$(st result) $(st refusals) $(st streak)"
check "timeout: recorded" "c timeout 0 GPON_SN" "$(rr_line 2)"

# 3. Held: the third change in a row, unconfirmed. Nothing reaches the
# driver and nothing is recorded.
: > "$INITLOG"
change ODIT00000004
check "held: result" "done held identity held" "$(st state) $(st result) $(st changed) $(st action)"
check "held: no driver verb" "0" "$(wc -c < "$INITLOG" | tr -d ' ')"
check "held: not recorded" "2" "$(wc -l < "$REREG" | tr -d ' ')"
check_grep "held: logged" "event=reload changed=identity action=held keys=GPON_SN" "$log"
case $(rr_state) in
*"refused in a row 2"*"held: apply.sh omci --confirm") ok "held: omcli state says so" ;;
*) bad "held: omcli state says '$(rr_state)'" ;;
esac
# A further reload is held again: the daemon kept the identity it ran.
change ODIT00000004
check "held: still held on the next signal" "held" "$(st result)"

# 4. Confirmed: the change goes through. O7 now, the OLT disabled it.
gpon /tmp/rl-gpon-isp1 7 "0"
echo confirm > "$CONFIRM"
change ODIT00000004
check "confirmed: re-registered, O7 is a refusal at once" "refused 0 3" "$(st result) $(st refusals) $(st streak)"
[ ! -e "$CONFIRM" ] && ok "confirmed: the confirmation is used up" || bad "confirmed: the file is still there"

# 5. A revert is recorded and skipped by the count; accepted resets it.
gpon /tmp/rl-gpon-isp1 5 "0"
echo revert > "$CONFIRM"
change ODIT00000001
check "revert: recorded as one" "r no_services 0 GPON_SN" "$(rr_line 4)"
check "revert: the count is unchanged" "3" "$(st streak)"
stop_omcid

# 6. The record survives a restart; an accepted change ends the run.
start_omcid "$log" -g /tmp/rl-gpon-isp1 -P "$PLOAM" -j 4
check "restart: the record is read back" "4 recorded  refused in a row 3" \
	"$(rr_state | sed 's/  accepted.*//')"
echo confirm > "$CONFIRM"
change ODIT00000005
check "accepted: the run of refusals ends" "no_services 0" "$(st result) $(st streak)"
check "accepted: five lines" "5" "$(wc -l < "$REREG" | tr -d ' ')"
# A confirmation left for a reload that does not need one is taken anyway.
echo confirm > "$CONFIRM"
change ODIT00000005
[ ! -e "$CONFIRM" ] && ok "unused confirmation: taken by the reload" || bad "unused confirmation: left behind"
stop_omcid

echo
[ "$fail" -eq 0 ] && echo "reload-test.sh: all checks passed" || echo "reload-test.sh: $fail check(s) failed"
suite_exit
