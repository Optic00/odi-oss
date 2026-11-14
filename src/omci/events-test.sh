#!/bin/sh
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

# The event lines (respond/events.c) and `omcli provision` (respond/show.c),
# against a whole ISP1 provisioning session: the same OLT side drv-test.sh
# replays for the driver-call golden (test/fixtures/omci-session-isp1.txt),
# so the counts below are the counts of that real session.
#
# What this covers: every omcid event line on the omcid.log side -- start,
# the PLOAM Alloc-IDs, the OLT MIB reset, the MIB upload, one provisioning burst and its summary,
# a CLI (local) MIB reset, an OLT reboot request, a software download start --
# and the rate limit. The syslog side, the same line through /dev/log to
# busybox syslogd, needs a syslogd, so test/qemu/run-qemu.sh checks it on the
# full-system image instead.
#
# Run from src/omci, inside the toolchain image (the Makefile test-omci-events
# target):
#   docker run --rm -v "$PWD/../..":/src -w /src/src/omci "$(../../toolchain/image.sh diag)" \
#          sh events-test.sh
set -u
Q=qemu-mips-static
FIX=../../test/fixtures
fail=0
. ./testlib.sh

check() {
	if [ "$2" = "$3" ]; then
		echo "ok    $1"
	else
		echo "FAIL  $1"
		echo "--- expected"; printf '%s\n' "$3"
		echo "--- got";      printf '%s\n' "$2"
		fail=$((fail + 1))
	fi
}

# One baseline frame: tci, message type (with AR), class, instance, zero
# contents, the trailer and a zero CRC (omcid does not check it, and the
# session fixtures carry zero too).
frame() {
	printf '%s%s0a%s%s%s0000002800000000\n' "$1" "$2" "$3" "$4" \
		"0000000000000000000000000000000000000000000000000000000000000000"
}

CAPS=020000ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00000000000000010000000300000002ffffffff000000000000001000000040000000800000000800000000000000100000000f00000440

mkdir -p /var/config /var/run /var/log
cp "$FIX/omci-store-isp1.xml" /var/config/lastgood.xml
# A fresh boot: no summary of unknowns yet, and OMCI_UNKNOWN_ME_OK unset.
rm -f /var/log/omcid-unknown.txt /var/config/odi.conf
rm -f /var/config/lastgood_hs.xml
# No decision file: to omcid this is the first start of the boot.
rm -f /var/run/omcid-resume-decision /var/run/omcid-resume-decision.prev

# A respawn: svc-omcid.sh has moved the previous omcid decision to .prev, so
# there is no decision file, and that alone must still read as a restart.
# Both files are gone once this omcid has written its own decision.
printf 'resumed\n' > /var/run/omcid-resume-decision.prev
rlog=/tmp/omcid-events-restart.log
: > "$rlog"
$Q respond/build/omcid -w 12 -c "$CAPS" -g /tmp/events-gpon-restart > "$rlog" 2>&1 &
rpid=$!
need "$rlog" '^event=start ' 10
got=$(grep '^event=start ' "$rlog" | sed 's/ mode=.*//')
check "start: only the .prev decision of the last run is left, so a restart" "$got" \
      "event=start run=restart"
got=$(cat /var/run/omcid-resume-decision 2>/dev/null)
check "and the decision file holds this run's own" "$got" "reprovision"
[ -e /var/run/omcid-resume-decision.prev ]; got=$?
check "and the .prev of the last run is gone" "$got" "1"
kill "$rpid" 2>/dev/null
wait "$rpid" 2>/dev/null
rm -f /var/run/omcid-resume-decision

log=/tmp/omcid-events.log
# The five Alloc-IDs ISP1 assigns by PLOAM, as /proc/odi_gpon/status lists them.
printf 'alloc_ids 5 282 794 1050 1306 538\n' > /tmp/events-gpon
: > "$log"
$Q respond/build/omcid -w 12 -c "$CAPS" -g /tmp/events-gpon > "$log" 2>&1 &
pid=$!
omcid_up "$log"

# No netlink under qemu, so no ONU state: not O5, so no resume.
got=$(grep '^event=start ' "$log")
check "start: first of the boot, back to re-registration, and why" "$got" \
      "event=start run=boot mode=reregister reason=not_o5 onu_state=unknown rows=0"

$Q cli/build/omcli --inject-file "$FIX/omci-session-isp1.txt" > /dev/null 2>&1
# The fixture was recorded when omcid announced 301 upload records, and its
# log keeps only the first three Nexts and the last (sequence 300). The
# upload is 400 records now (one per 26 bytes of each entity, as the stock
# ONU sends them), so the Next of the last one is sent here.
$Q cli/build/omcli --inject "$(printf '0f004e0a00020000018f%060d00000028%08d' 0 0)" > /dev/null 2>&1
# The burst closes EV_PROV_QUIET_S (10 s) after its last write; the check
# runs once a second. This is the one wait on a real timer, and the only run
# that sees the production value of it.
need "$log" '^event=provision_end ' 40

got=$(grep -c '^event=mib_reset side=olt ' "$log")
check "the OLT MIB reset, as the OLT's" "$got" "1"
got=$(grep '^event=mib_upload_begin ' "$log")
check "the MIB upload, with the count the OLT was told" "$got" \
      "event=mib_upload_begin entities=400"
got=$(grep -c '^event=provision_begin .*after_mib_reset=1$' "$log")
check "one burst opens, after the reset" "$got" "1"
got=$(grep '^event=mib_upload_end ' "$log" | sed 's/ duration_s=[0-9.]*//')
check "and its end, at the Next that answers the last entity" "$got" \
      "event=mib_upload_end entities=400"
# 83 creates in the session, one of them for class 351, which the model
# does not have: refused, so not counted.
# 185 rows: the OLT's 161, the tables (classes 49, 50, 79) omcid creates
# with each of the eight MAC bridge ports -- 24, one of them a row the OLT
# also sets (class 79 of port 1) -- and the class 46 it creates with the one
# MAC bridge service profile.
got=$(grep '^event=provision_end ' "$log" | sed 's/ duration_s=[0-9.]*//')
check "and closes with the session's own counts" "$got" \
      "event=provision_end creates=82 sets=102 deletes=0 rows=185 services=6 after_mib_reset=1"
got=$(grep '^event=alloc_ids ' "$log")
check "the PLOAM Alloc-IDs, once, when first read" "$got" \
      "event=alloc_ids count=5 ids=282,794,1050,1306,538"
# That refused create is the one class of the session omcid has no model
# for. The ISP1 OLT also sends ONU-G two message types G.988 does not give
# an OLT, 17 and 1, each three times in a row, with and without AR: one
# line for each, and one summary line with the count.
got=$(grep '^event=unknown_' "$log")
check "the unknowns of the session are logged, once each" "$got" \
      "event=unknown_me class=351 op=create mt=4
event=unknown_msg type=17 class=256
event=unknown_msg type=1 class=256"
UNK_ISP1='unknown_me class=351 op=create mt=4 count=1
unknown_msg type=17 class=256 count=9
unknown_msg type=1 class=256 count=6'
got=$(grep -v '^#' /var/log/omcid-unknown.txt | sed 's/ first_uptime_s=[0-9]*$//')
check "and summarised in /var/log/omcid-unknown.txt" "$got" "$UNK_ISP1"
# Every Get, Set and Delete of the ISP1 session names an instance omcid holds
# (one it created itself, or one the OLT created first): no event=no_instance.
got=$(grep -c '^event=no_instance ' "$log")
check "the ISP1 session addresses no instance omcid lacks" "$got" "0"
# Nothing per message: the whole session is a handful of lines.
got=$(grep -c '^event=' "$log")
check "a whole provisioning session is ten event lines" "$got" "10"

# `omcli provision`: what that session provisioned.
out=$($Q cli/build/omcli provision 2>&1)
got=$(echo "$out" | grep '^summary ')
check "provision: the summary" "$got" \
      "summary rows=185 tconts=5 gem_ports=6 vlans=6 traffic_descriptors=0 services=6 mib_data_sync=184"
got=$(echo "$out" | grep -c '^gem me=[0-9]* port=[0-9]* direction=[123] tcont_me=')
check "provision: one line per GEM port" "$got" "6"
got=$(echo "$out" | grep '^vlan ' | tr '\n' ';')
check "provision: the VLANs, once per source" "$got" \
      "vlan vid=1 source=ext_vlan_treatment;vlan vid=10 source=vlan_filter;vlan vid=11 source=vlan_filter;vlan vid=12 source=vlan_filter;vlan vid=13 source=vlan_filter;vlan vid=14 source=vlan_filter;"

# A MIB reset from the CLI is this side's, not the OLT's.
$Q cli/build/omcli -f mib reset > /dev/null 2>&1
need "$log" '^event=mib_reset side=local ' 10
got=$(grep -c '^event=mib_reset side=local rows=185 ' "$log")
check "a CLI MIB reset is logged as local" "$got" "1"

# What the OLT may act on: a reboot, which omcid refuses, and a software
# download, which it accepts and discards (OLT_SW_DOWNLOAD, swdl-test.sh).
$Q cli/build/omcli --inject "$(frame 0e01 59 0100 0000)" > /dev/null 2>&1
$Q cli/build/omcli --inject "$(frame 0e02 53 0007 0001)" > /dev/null 2>&1
sync_send "$log"
got=$(grep '^event=olt_reboot ' "$log")
check "an OLT reboot request" "$got" \
      "event=olt_reboot class=256 inst=0 result=refused"
# Command not supported (2), the answer to a refused Reboot of the ONU-G.
got=$(grep -o -- '-> 0e01390a0100000002' "$log" | head -1)
check "a refused OLT reboot answers command not supported" "$got" "-> 0e01390a0100000002"
got=$(grep -c '^reboot from the OLT refused (OMCI_ACCEPT_REBOOT=0)$' "$log")
check "and says so once" "$got" "1"
got=$(grep '^event=sw_image ' "$log")
check "a software download start, accepted by default (swdl-test.sh has the rest)" "$got" \
      "event=sw_image op=download_start inst=1 size=0 window=1 result=ok"

# What omcid does not model, once per boot. The same unknown create again,
# a Get of it, and a message type omcid does not handle (29, Set table),
# twice: one event line each for the new ones, a count for all.
$Q cli/build/omcli --inject "$(frame 0e03 44 015f 0001)" > /dev/null 2>&1
$Q cli/build/omcli --inject "$(frame 0e04 49 015f 0001)" > /dev/null 2>&1
$Q cli/build/omcli --inject "$(frame 0e05 5d 0100 0000)" > /dev/null 2>&1
$Q cli/build/omcli --inject "$(frame 0e06 5d 0100 0000)" > /dev/null 2>&1
# Get current data (28) is answered like a Get, not logged as unknown.
$Q cli/build/omcli --inject "$(frame 0e0a 5c 0100 0000)" > /dev/null 2>&1
sync_send "$log"
got=$(grep -c '^event=unknown_me class=351 op=create ' "$log")
check "an unknown class and operation is logged once, however often" "$got" "1"
got=$(grep '^event=unknown_me class=351 op=get ' "$log")
check "another operation on it is a line of its own" "$got" \
      "event=unknown_me class=351 op=get mt=9"
got=$(grep -c '^event=unknown_msg type=29 ' "$log")
check "an unknown message type is logged once" "$got" "1"
got=$(grep -c '^event=unknown_msg type=28 ' "$log")
check "Get current data is not an unknown message type" "$got" "0"
got=$(grep -o -- '-> 0e0a3c0a0100000000' "$log" | head -1)
check "Get current data is answered like a Get" "$got" "-> 0e0a3c0a0100000000"
# A new entry is written at once, a count the next time the one-second tick
# looks (respond/events.c).
need /var/log/omcid-unknown.txt '^unknown_msg type=29 class=256 count=2' 10
got=$(grep -v '^#' /var/log/omcid-unknown.txt | sed 's/ first_uptime_s=[0-9]*$//')
check "the summary counts every one of them" "$got" \
      "unknown_me class=351 op=create mt=4 count=2
unknown_msg type=17 class=256 count=9
unknown_msg type=1 class=256 count=6
unknown_me class=351 op=get mt=9 count=1
unknown_msg type=29 class=256 count=2"
# A modelled class at an instance omcid holds no row for (MAC bridge service
# profile 0x0999): one line per (class, instance) whatever the operation,
# the answer unchanged. Two Gets and the Set that creates the row are counted;
# the Get after it names a held instance. A Get of an instance omcid creates
# itself (ONU-G 0) is not one of these.
$Q cli/build/omcli --inject "$(frame 0e30 49 002d 0999)" > /dev/null 2>&1
$Q cli/build/omcli --inject "$(frame 0e31 49 002d 0999)" > /dev/null 2>&1
$Q cli/build/omcli --inject "$(frame 0e32 48 002d 0999)" > /dev/null 2>&1
$Q cli/build/omcli --inject "$(frame 0e33 49 002d 0999)" > /dev/null 2>&1
$Q cli/build/omcli --inject "$(frame 0e34 49 0100 0000)" > /dev/null 2>&1
sync_send "$log"
got=$(grep '^event=no_instance ' "$log")
check "a Get of an instance omcid does not hold is logged once" "$got" \
      "event=no_instance class=45 inst=2457 op=get mt=9"
got=$(grep -o -- '-> 0e30290a002d0999' "$log" | head -1)
check "and still answered as before (success)" "$got" "-> 0e30290a002d0999"
need /var/log/omcid-unknown.txt '^no_instance class=45 inst=2457 op=get mt=9 count=3' 10
got=$(grep '^no_instance ' /var/log/omcid-unknown.txt | sed 's/ first_uptime_s=[0-9]*$//')
check "the summary counts the requests until the instance exists" "$got" \
      "no_instance class=45 inst=2457 op=get mt=9 count=3"
# Without OMCI_UNKNOWN_ME_OK the answer is "unknown entity" (4), and an
# unknown type is "not supported" (1).
got=$(grep -o -- '-> 0e03240a015f000104' "$log" | head -1)
check "an unknown create is refused as an unknown entity" "$got" "-> 0e03240a015f000104"
got=$(grep -o -- '-> 0e04290a015f000104' "$log" | head -1)
check "and so is a Get of it" "$got" "-> 0e04290a015f000104"
got=$(grep -o -- '-> 0e053d0a0100000001' "$log" | head -1)
check "an unknown message type is not supported" "$got" "-> 0e053d0a0100000001"
got=$(grep '^unknown entities: ' "$log")
check "the key is off unless set" "$got" "unknown entities: answered unknown entity"

# Rate limited: 30 more reboot requests, back to back, and the window of 20
# lines a minute already holds 15 (start, Alloc-IDs, OLT reset, upload begin
# and end, provision begin and end, the three session unknowns, CLI reset,
# reboot, sw_image, the two unknowns and the no_instance line above).
: > /tmp/reboots.txt
i=0
while [ "$i" -lt 30 ]; do
	frame "0f$(printf '%02x' "$i")" 59 0100 0000 >> /tmp/reboots.txt
	i=$((i + 1))
done
$Q cli/build/omcli --inject-file /tmp/reboots.txt > /dev/null 2>&1
sync_send "$log"
got=$(grep -c '^event=' "$log")
check "a flood is cut at 20 event lines a minute" "$got" "20"

[ "$fail" -eq 0 ] || keep_log "$log" events-test.log

# A respawn in the same boot (kill -9, as a crash leaves it): the summary is
# read back, so what the first process logged is not logged again. This one
# starts with OMCI_UNKNOWN_ME_OK=1, which answers unknown entities with
# success: nothing stored, an empty Get, MIB data sync counting the writes.
kill -9 "$pid" 2>/dev/null
wait "$pid" 2>/dev/null
printf 'SYSLOG_SERVER=\nOMCI_UNKNOWN_ME_OK=1\n' > /var/config/odi.conf
log2=/tmp/omcid-events2.log
: > "$log2"
$Q respond/build/omcid -w 12 -c "$CAPS" > "$log2" 2>&1 &
pid2=$!
omcid_up "$log2"
got=$(grep '^unknown entities: ' "$log2")
check "OMCI_UNKNOWN_ME_OK=1 is read from odi.conf" "$got" \
      "unknown entities: answered ok (OMCI_UNKNOWN_ME_OK=1)"
$Q cli/build/omcli -f mib reset > /dev/null 2>&1
$Q cli/build/omcli --inject "$(frame 0e07 44 015f 0002)" > /dev/null 2>&1
$Q cli/build/omcli --inject "$(frame 0e08 48 015f 0002)" > /dev/null 2>&1
$Q cli/build/omcli --inject "$(frame 0e09 49 015f 0002)" > /dev/null 2>&1
$Q cli/build/omcli --inject "$(frame 0e0a 44 fdfd 0001)" > /dev/null 2>&1
sync_send "$log2"
got=$(grep -c '^event=unknown_me class=351 op=\(create\|get\) ' "$log2")
check "a respawned omcid does not log what this boot already logged" "$got" "0"
got=$(grep '^event=unknown_me ' "$log2")
check "but a new unknown still is" "$got" "event=unknown_me class=351 op=set mt=8
event=unknown_me class=65021 op=create mt=4"
need /var/log/omcid-unknown.txt '^unknown_me class=351 op=create mt=4 count=3' 10
got=$(grep -v '^#' /var/log/omcid-unknown.txt | sed 's/ first_uptime_s=[0-9]*$//')
check "and the counts carry on across the respawn" "$got" \
      "unknown_me class=351 op=create mt=4 count=3
unknown_msg type=17 class=256 count=9
unknown_msg type=1 class=256 count=6
unknown_me class=351 op=get mt=9 count=2
unknown_msg type=29 class=256 count=2
no_instance class=45 inst=2457 op=get mt=9 count=3
unknown_me class=351 op=set mt=8 count=1
unknown_me class=65021 op=create mt=4 count=1"
got=$(grep -o -- '-> 0e07240a015f0002000000' "$log2" | head -1)
check "with the key, an unknown create answers success" "$got" "-> 0e07240a015f0002000000"
got=$(grep -o -- '-> 0e08280a015f000200' "$log2" | head -1)
check "an unknown set too" "$got" "-> 0e08280a015f000200"
got=$(grep -o -- '-> 0e09290a015f0002000000' "$log2" | head -1)
check "and a Get answers success with no attribute" "$got" "-> 0e09290a015f0002000000"
out=$($Q cli/build/omcli provision 2>&1 | grep '^summary ')
check "nothing is stored, and MIB data sync counts the faked writes" \
      "$(echo "$out" | sed 's/.*rows=\([0-9]*\).*mib_data_sync=\([0-9]*\).*/rows=\1 mib_data_sync=\2/')" \
      "rows=0 mib_data_sync=3"
[ "$fail" -eq 0 ] || keep_log "$log2" events-test2.log
omcid_stop "$pid2"
rm -f /var/config/odi.conf

# Receive-side validation (respond/omcimsg.c, rx_accept()): a frame whose
# length, device identifier or CRC is wrong is dropped before it is looked at --
# no answer, one counter per reason, one event line per reason. A trailer other
# than 00000028 under a valid CRC is answered, and counted and logged.
# --inject fills in the trailer and CRC (so only the field a case spoils is wrong);
# --inject-raw sends the bytes as given.
#
# VEC is a frame captured from an OLT: ONU2-G Get, trailer 00000028, CRC
# 624bea37 (the AAL5 CRC-32, poly 04c11db7, init and final xor ffffffff, no
# reflection). Sent raw, it is answered only if omcid computes that CRC.
VEC=b397490a01010000004000000000000000000000000000000000000000000000000000000000000000000028624bea37
rx_run() {
	printf 'SYSLOG_SERVER=\n' > /var/config/odi.conf
	: > /tmp/omcid-rx.log
	$Q respond/build/omcid -w 12 -c "$CAPS" > /tmp/omcid-rx.log 2>&1 &
	pid4=$!
	omcid_up /tmp/omcid-rx.log
}
rx_run
rxlog=/tmp/omcid-rx.log
got=$(printf '%s' "$VEC" | wc -c)
check "the captured frame is 48 bytes" "$got" "96"
$Q cli/build/omcli --inject-raw "$VEC" > /dev/null 2>&1
need "$rxlog" '^-> b397' 10
got=$(grep -c '^event=rx_reject ' "$rxlog")
check "the captured frame, CRC and all, is answered and not counted" "$got" "0"
# Each bad frame is a Get of ONU-G that would be answered if it got through.
base=$(frame 0f01 49 0100 0000)
bad_crc=$(printf '%s' "$base" | sed 's/00000000$/deadbeef/')
$Q cli/build/omcli --inject-raw "$bad_crc" > /dev/null 2>&1
# A trailer of 00000029 with a CRC that matches it: answered, but counted.
$Q cli/build/omcli --inject-raw 0f02490a01000000000000000000000000000000000000000000000000000000000000000000000000000029c44f7a32 > /dev/null 2>&1
$Q cli/build/omcli --inject "$(frame 0f03 49 0100 0000 | sed 's/^\(....49\)0a/\10b/')" > /dev/null 2>&1
$Q cli/build/omcli --inject-raw "$(frame 0f04 49 0100 0000 | sed 's/..$//')" > /dev/null 2>&1
$Q cli/build/omcli --inject "$(frame 0f05 49 0100 0000)" > /dev/null 2>&1
need "$rxlog" '^-> 0f05' 10
got=$(grep -c '^-> 0f02' "$rxlog")
check "a bad trailer under a valid CRC is answered" "$got" "1"
for t in 0f01 0f03 0f04; do
	got=$(grep -c "^-> $t" "$rxlog")
	check "a frame with a bad length, device id or CRC is not answered ($t)" "$got" "0"
done
got=$(grep '^event=rx_reject ' "$rxlog" | sed 's/ len=[0-9]*$//')
check "one event line per reason, with the count" "$got" \
"event=rx_reject reason=crc count=1 total=1 action=dropped
event=rx_reject reason=trailer count=1 total=1 action=accepted
event=rx_reject reason=devid count=1 total=1 action=dropped devid=11
event=rx_reject reason=length count=1 total=1 action=dropped"
got=$($Q cli/build/omcli state 2>&1 | grep -a '^rx dropped')
check "omcli state counts them per reason" "$got" "rx dropped : length 1  devid 1  crc 1  (trailer odd, answered: 1)"
# The same fault again inside the window is counted, not logged again.
$Q cli/build/omcli --inject-raw "$bad_crc" > /dev/null 2>&1
$Q cli/build/omcli --inject-raw "$bad_crc" > /dev/null 2>&1
sync_send "$rxlog"
got=$(grep -c '^event=rx_reject reason=crc ' "$rxlog")
check "a repeat inside the window is counted, not logged" "$got" "1"
got=$($Q cli/build/omcli state 2>&1 | grep -a '^rx dropped')
check "and the counter moved" "$got" "rx dropped : length 1  devid 1  crc 3  (trailer odd, answered: 1)"
omcid_stop "$pid4"
rm -f /var/config/odi.conf

# A MIB reset loop (respond/events.c, "mib_reset_loop"): three resets from
# the OLT within the window are one event=mib_reset_loop state=start, logged
# once however many follow, and each reset line carries the requests just
# before it (before=, oldest first) and the OLT resets within the window
# (recent=, this one included). The end of a loop needs a whole window
# (600 s) without a reset, longer than this suite should wait.
rx_run
mlog=/tmp/omcid-rx.log
reset() {   # reset <tci>: a MIB reset of ONU data, as the OLT sends it
	$Q cli/build/omcli --inject "$(frame "$1" 4f 0002 0000)" > /dev/null 2>&1
}
$Q cli/build/omcli --inject "$(frame 0e40 49 0100 0000)" > /dev/null 2>&1
$Q cli/build/omcli --inject "$(frame 0e41 49 0101 0000)" > /dev/null 2>&1
# A MIB upload and its first Next: the upload is kept, the Next is not.
$Q cli/build/omcli --inject "$(frame 0e46 4d 0002 0000)" > /dev/null 2>&1
$Q cli/build/omcli --inject "$(frame 0e47 4e 0002 0000)" > /dev/null 2>&1
reset 0e42
reset 0e43
sync_send "$mlog"
got=$(grep '^event=mib_reset side=olt ' "$mlog" | sed 's/ rows=.* recent=/ recent=/')
check "each OLT reset says how many came within the window, and what came before it" "$got" \
      "event=mib_reset side=olt recent=1 before=get:256.0,get:257.0,upload:2.0
event=mib_reset side=olt recent=2"
got=$(grep -c '^event=mib_reset_loop ' "$mlog")
check "two resets are not a loop yet" "$got" "0"
reset 0e44
reset 0e45
sync_send "$mlog"
got=$(grep '^event=mib_reset_loop ' "$mlog")
check "the third is, logged once" "$got" \
      "event=mib_reset_loop state=start resets=3 window_s=600 services=0"
check "every reset still has its own line" "$(grep -c '^event=mib_reset side=olt ' "$mlog")" "4"
got=$(grep '^event=mib_reset side=olt ' "$mlog" | sed 's/.* recent=/recent=/' | sed -n '3,4p')
check "and only the requests since the last reset (the sync Get, then none)" "$got" \
      "recent=3 before=get:256.0
recent=4"
got=$($Q cli/build/omcli state 2>&1 | grep -a '^mib resets')
check "omcli state shows the counts and the loop" "$got" \
      "mib resets : olt 4  local 0  olt in last 600 s 4  loop yes  (loops 1)"
$Q cli/build/omcli -f mib reset > /dev/null 2>&1
got=$($Q cli/build/omcli state 2>&1 | grep -a '^mib resets')
check "a local reset is counted apart, and is not part of a loop" "$got" \
      "mib resets : olt 4  local 1  olt in last 600 s 4  loop yes  (loops 1)"
[ "$fail" -eq 0 ] || keep_log "$mlog" events-test-loop.log
omcid_stop "$pid4"
rm -f /var/config/odi.conf

# OMCI_FIXED_ME: a Get of a listed class, which omcid does not model,
# answers its bytes under the mask asked for, and the bytes are not logged.
# A class omcid does model is not overridden; a malformed list is off.
getmask() {   # getmask <tci> <class4> <mask4>: a Get with an attribute mask
	printf '%s490a%s0000%s%060d0000002800000000\n' "$1" "$2" "$3" 0
}
fixed_run() {   # fixed_run <log> <value>: an omcid with OMCI_FIXED_ME set
	printf 'OMCI_FIXED_ME=%s\n' "$2" > /var/config/odi.conf
	: > "$1"
	$Q respond/build/omcid -w 12 -c "$CAPS" > "$1" 2>&1 &
	pid3=$!
	omcid_up "$1"
}
fixed_end() {   # fixed_end [<log>]: everything injected has been answered; stop it
	[ -z "${1:-}" ] || sync_send "$1"
	omcid_stop "$pid3"
}
fixed_run /tmp/omcid-fixed.log '200:0x48656c6c6f, 201:0102'
$Q cli/build/omcli --inject "$(getmask 0e10 00c8 8000)" > /dev/null 2>&1
$Q cli/build/omcli --inject "$(getmask 0e11 00c9 c000)" > /dev/null 2>&1
$Q cli/build/omcli --inject "$(getmask 0e12 00ca 8000)" > /dev/null 2>&1
fixed_end /tmp/omcid-fixed.log
got=$(grep '^fixed entities: ' /tmp/omcid-fixed.log)
check "OMCI_FIXED_ME is read from odi.conf" "$got" \
      "fixed entities: 2 entr(ies) answered from OMCI_FIXED_ME"
got=$(grep -o -F -- '-> 0e10290a00c8000000800048656c6c6f' /tmp/omcid-fixed.log | head -1)
check "a Get of the first class answers its bytes under the mask asked for" "$got" \
      "-> 0e10290a00c8000000800048656c6c6f"
got=$(grep -o -F -- '-> 0e11290a00c9000000c0000102' /tmp/omcid-fixed.log | head -1)
check "and the second class its own" "$got" "-> 0e11290a00c9000000c0000102"
got=$(grep -o -- '-> 0e12290a00ca000004' /tmp/omcid-fixed.log | head -1)
check "a class not listed is still an unknown entity" "$got" "-> 0e12290a00ca000004"
got=$(grep -v '^-> ' /tmp/omcid-fixed.log | grep -c -i '48656c6c6f')
check "and the bytes are not logged" "$got" "0"
fixed_run /tmp/omcid-fixed2.log '256:48656c6c6f'
$Q cli/build/omcli --inject "$(getmask 0e13 0100 8000)" > /dev/null 2>&1
fixed_end /tmp/omcid-fixed2.log
got=$(grep -c -i -- '-> 0e13290a01000000008000.*48656c6c6f' /tmp/omcid-fixed2.log)
check "a class omcid models (ONU-G) is not overridden" "$got" "0"
got=$(grep -c -- '-> 0e13290a0100000000' /tmp/omcid-fixed2.log)
check "and still answers from its own model" "$got" "1"
fixed_run /tmp/omcid-fixed3.log '200:48656c6c6f,200:0102'
fixed_end
got=$(grep -c '^fixed entities: ' /tmp/omcid-fixed3.log)
check "a class listed twice turns the key off" "$got" "0"
fixed_run /tmp/omcid-fixed4.log '200:486'
fixed_end
got=$(grep -c '^fixed entities: ' /tmp/omcid-fixed4.log)
check "an odd number of hex digits turns the key off" "$got" "0"
# Per-instance entities, CLASS:INSTANCE:ATTR:HEX. Class 188 instance 0x8001
# has attributes 1 to 3; class 360 instance 5 has a one-byte attribute and
# two of 25 bytes, which take two upload records; 200 is the old form.
getmaski() {   # getmaski <tci> <class4> <inst4> <mask4> <mt2>: a Get of one instance
	printf '%s%s0a%s%s%s%060d0000002800000000\n' "$1" "$5" "$2" "$3" "$4" 0
}
nextframe() {  # nextframe <tci> <seq4>: a MIB upload next
	printf '%s4e0a00020000%s%060d0000002800000000\n' "$1" "$2" 0
}
fixed_run /tmp/omcid-fixed5.log '188:0x8001:1:0x0011aa 188:0x8001:3:ff 188:0x8001:2:2233 360:5:1:01 360:5:2:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa 360:5:3:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb 200:48'
$Q cli/build/omcli --inject "$(frame 0e40 4d 0002 0000)" > /dev/null 2>&1
$Q cli/build/omcli --inject "$(getmaski 0e41 00bc 8001 e000 49)" > /dev/null 2>&1
$Q cli/build/omcli --inject "$(getmaski 0e42 00bc 8001 4000 49)" > /dev/null 2>&1
$Q cli/build/omcli --inject "$(getmaski 0e43 00bc 8001 9000 49)" > /dev/null 2>&1
$Q cli/build/omcli --inject "$(getmaski 0e44 00bc 0000 e000 49)" > /dev/null 2>&1
$Q cli/build/omcli --inject "$(getmaski 0e45 00bc 8001 e000 5c)" > /dev/null 2>&1
$Q cli/build/omcli --inject "$(getmaski 0e46 00c8 0007 8000 49)" > /dev/null 2>&1
$Q cli/build/omcli --inject "$(getmaski 0e47 0168 0005 ffff 49)" > /dev/null 2>&1
for q in "0e48 0190" "0e49 0191" "0e4a 0192" "0e4b 0193"; do
	$Q cli/build/omcli --inject "$(nextframe $q)" > /dev/null 2>&1
done
fixed_end /tmp/omcid-fixed5.log
L=/tmp/omcid-fixed5.log
A=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
B=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb
got=$(grep '^fixed entities: ' $L)
check "both forms together are read, one entry per attribute" "$got" \
      "fixed entities: 7 entr(ies) answered from OMCI_FIXED_ME"
got=$(grep 'upload records' $L | sed 's/.*(\(.*\))/\1/')
check "the MIB upload counts the entities, two records for the big one" "$got" \
      "277 autonomous, 0 held, 3 fixed"
got=$(grep -o -F -- '-> 0e482e0a0002000000bc8001e0000011aa2233ff' $L | head -1)
check "an upload record carries the attributes in order, with the mask" "$got" \
      "-> 0e482e0a0002000000bc8001e0000011aa2233ff"
got=$(grep -o -F -- "-> 0e492e0a0002000001680005c00001$A" $L | head -1)
check "a full record ends where the next attribute would not fit" "$got" \
      "-> 0e492e0a0002000001680005c00001$A"
got=$(grep -o -F -- "-> 0e4a2e0a00020000016800052000$B" $L | head -1)
check "and the next record continues with the rest, under its own mask" "$got" \
      "-> 0e4a2e0a00020000016800052000$B"
got=$(grep -o -- '-> 0e4b2e0a0002000003' $L | head -1)
check "past the last record is a parameter error" "$got" "-> 0e4b2e0a0002000003"
got=$(grep -o -F -- '-> 0e41290a00bc800100e0000011aa2233ff' $L | head -1)
check "a Get of the configured instance answers its attributes in mask order" "$got" \
      "-> 0e41290a00bc800100e0000011aa2233ff"
got=$(grep -o -F -- '-> 0e42290a00bc80010040002233' $L | head -1)
check "one attribute of them" "$got" "-> 0e42290a00bc80010040002233"
got=$(grep -o -F -- '-> 0e43290a00bc8001008000' $L | head -1)
check "an attribute it does not have drops out of the answered mask" "$got" \
      "-> 0e43290a00bc8001008000"
got=$(grep -o -F -- '-> 0e43290a00bc80010080000011aa' $L | head -1)
check "and the rest keeps its bytes" "$got" "-> 0e43290a00bc80010080000011aa"
got=$(grep -o -- '-> 0e44290a00bc000005' $L | head -1)
check "a Get of another instance of the class is unknown instance" "$got" \
      "-> 0e44290a00bc000005"
got=$(grep -o -F -- '-> 0e453c0a00bc800100e0000011aa2233ff' $L | head -1)
check "Get current data answers the same" "$got" \
      "-> 0e453c0a00bc800100e0000011aa2233ff"
got=$(grep -o -- '-> 0e46290a00c80007008000' $L | head -1)
check "the old form still answers any instance" "$got" "-> 0e46290a00c80007008000"
got=$(grep -v -e '^-> ' -e '^<- ' $L | grep -c -i -e 0011aa -e 2233)
check "and the bytes are not logged" "$got" "0"
rm -f /tmp/omcid-fixed5.log
# Malformed anything turns the whole key off.
for bad in '188:0x8001:1:00,188:0x8001:1:01' '188:00 188:0x8001:1:01' \
	'188:0x8001:1:01 188:00' '188:0x8001:0:00' '188:0x8001:17:00' \
	'188:0x10000:1:00' '188:65536:1:00' '188:1:2:3:00' '188:1:1' '188::1:00' \
	'188:1::00' '0xbc:1:1:00' '188:0x:1:00' '188:1:1:0' '188:1:1:0x' \
	'188:1:1:00112233445566778899aabbccddeeff00112233445566778899' \
	'188:1:0x1:00' '188:1:001:00' '188:1:1:00,200:00 188:1:2:00 188:1:3:00 188:1:4:00 188:1:5:00 188:1:6:00 188:1:7:00 188:1:8:00 188:1:9:00 188:1:10:00 188:1:11:00 188:1:12:00 188:1:13:00 188:1:14:00 188:1:15:00 188:1:16:00'; do
	fixed_run /tmp/omcid-fixed6.log "$bad"
	fixed_end
	got=$(grep -c '^fixed entities: ' /tmp/omcid-fixed6.log)
	check "malformed, the key is off: $bad" "$got" "0"
done
# 16 entries and the decimal instance are fine.
fixed_run /tmp/omcid-fixed6.log '188:32769:1:00 188:32769:2:00 188:1:1:00 188:1:2:00 188:1:3:00 188:1:4:00 188:1:5:00 188:1:6:00 188:1:7:00 188:1:8:00 188:1:9:00 188:1:10:00 188:1:11:00 188:1:12:00 188:1:13:00 188:1:14:00'
fixed_end
got=$(grep '^fixed entities: ' /tmp/omcid-fixed6.log)
check "16 entries are accepted, with a decimal instance" "$got" \
      "fixed entities: 16 entr(ies) answered from OMCI_FIXED_ME"
rm -f /var/config/odi.conf

# OMCI_ACCEPT_REBOOT: a Reboot of the ONU-G is answered success, then init
# gets SIGTERM about a second later (what busybox reboot sends). The harness
# has no init to reboot, so -k names a sleeping stand-in and the test checks
# that it was signalled. Refused (the default), it must stay alive.
sleep 60 &
victim=$!
rm -f /var/config/odi.conf
: > /tmp/omcid-reboot0.log
$Q respond/build/omcid -w 12 -c "$CAPS" -k "$victim" > /tmp/omcid-reboot0.log 2>&1 &
pid4=$!
omcid_up /tmp/omcid-reboot0.log
$Q cli/build/omcli --inject "$(frame 0e20 59 0100 0000)" > /dev/null 2>&1
sync_send /tmp/omcid-reboot0.log
# Elapsed time on purpose: an accepted reboot signals init about a second
# after the answer, so "nothing is signalled" is only a check after 2 s.
sleep 2
kill -0 "$victim" 2>/dev/null; got=$?
check "reboot refused: nothing is signalled" "$got" "0"
omcid_stop "$pid4"
printf 'OMCI_ACCEPT_REBOOT=1\n' > /var/config/odi.conf
: > /tmp/omcid-reboot1.log
$Q respond/build/omcid -w 12 -c "$CAPS" -k "$victim" > /tmp/omcid-reboot1.log 2>&1 &
pid4=$!
omcid_up /tmp/omcid-reboot1.log
$Q cli/build/omcli --inject "$(frame 0e21 59 0100 0000)" > /dev/null 2>&1
$Q cli/build/omcli --inject "$(frame 0e22 59 0007 0000)" > /dev/null 2>&1
sync_send /tmp/omcid-reboot1.log
got=$(grep -o -- '-> 0e21390a0100000000' /tmp/omcid-reboot1.log | head -1)
check "reboot accepted: the answer is success" "$got" "-> 0e21390a0100000000"
got=$(grep -c '^reboot from the OLT accepted$' /tmp/omcid-reboot1.log)
check "and logged once" "$got" "1"
got=$(grep '^event=olt_reboot ' /tmp/omcid-reboot1.log | tr '\n' ';')
check "the event records both requests" "$got" \
      "event=olt_reboot class=256 inst=0 result=accepted;event=olt_reboot class=7 inst=0 result=not_supported;"
got=$(grep -o -- '-> 0e22390a0007000001' /tmp/omcid-reboot1.log | head -1)
check "a Reboot of another class keeps its answer" "$got" "-> 0e22390a0007000001"
wait_bounded "$victim" 15; got=$?
check "the stand-in for init got SIGTERM" "$got" "143"
omcid_stop "$pid4"
rm -f /var/config/odi.conf

# OMCI_ACCEPT_REBOOT=2: answered success, logged, and the stand-in for init is
# left alone. A fresh stand-in, since the one above was signalled.
sleep 60 &
victim=$!
printf 'OMCI_ACCEPT_REBOOT=2\n' > /var/config/odi.conf
: > /tmp/omcid-reboot2.log
$Q respond/build/omcid -w 12 -c "$CAPS" -k "$victim" > /tmp/omcid-reboot2.log 2>&1 &
pid4=$!
omcid_up /tmp/omcid-reboot2.log
$Q cli/build/omcli --inject "$(frame 0e23 59 0100 0000)" > /dev/null 2>&1
sync_send /tmp/omcid-reboot2.log
# Elapsed time on purpose, as for the refused reboot above.
sleep 2
got=$(grep -o -- '-> 0e23390a0100000000' /tmp/omcid-reboot2.log | head -1)
check "reboot acknowledged: the answer is success" "$got" "-> 0e23390a0100000000"
got=$(grep -c '^reboot from the OLT acknowledged, not rebooting (OMCI_ACCEPT_REBOOT=2)$' /tmp/omcid-reboot2.log)
check "and logged once" "$got" "1"
got=$(grep '^event=olt_reboot ' /tmp/omcid-reboot2.log | tr '\n' ';')
check "the event says acknowledged" "$got" \
      "event=olt_reboot class=256 inst=0 result=acknowledged;"
kill -0 "$victim" 2>/dev/null; got=$?
check "the stand-in for init is not signalled" "$got" "0"
kill "$victim" 2>/dev/null
wait "$victim" 2>/dev/null
omcid_stop "$pid4"
rm -f /var/config/odi.conf

# The ISP2 session, replayed into a fresh omcid of a fresh boot, addresses no
# instance omcid lacks either: the two OLTs on file never answer the question
# result 5 would raise (AUDIT B8 E3). A third OLT's session belongs here.
rm -f /var/log/omcid-unknown.txt
cp "$FIX/omci-store-isp2.xml" /var/config/lastgood.xml
: > /tmp/omcid-events-isp2.log
$Q respond/build/omcid -w 12 -c "$CAPS" > /tmp/omcid-events-isp2.log 2>&1 &
pid5=$!
omcid_up /tmp/omcid-events-isp2.log
$Q cli/build/omcli --inject-file "$FIX/omci-session-isp2.txt" > /dev/null 2>&1
sync_send /tmp/omcid-events-isp2.log
got=$(grep '^event=no_instance ' /tmp/omcid-events-isp2.log)
check "the ISP2 session addresses no instance omcid lacks" "$got" ""
omcid_stop "$pid5"

if [ "$fail" -eq 0 ]; then echo "events-test.sh: all checks passed"; else echo "FAILED ($fail)"; fi
suite_exit
