#!/bin/sh
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

# history.sh -- a short history of the line, one sample a minute, for
# a look back after a failure on a stick that no Prometheus scrapes
# (docs/TOOLS.md, "The minute history"). Run by the /etc/inittab respawn
# entry svc-history.sh; `services stop history` turns it off.
#
#     history.sh run       sample now and then every INTERVAL_S, forever
#     history.sh sample    one sample line to stdout, nothing written
#
# The ring, $RING (/var/run/history), holds the newest KEEP samples (120,
# two hours), one line each, oldest first: about 170 bytes a line, so about
# 20 KB of the /var tmpfs. It is RAM, so it is gone at every reboot, and it
# is written to a temporary name and renamed, so a reader (confd, the
# diagnostics bundle, cat over ssh) never sees half of it. Nothing else is
# kept and nothing is sent anywhere.
#
# A line is space-separated key=value pairs, so a reader takes the keys it
# knows and a new key breaks nobody. A key whose source is missing is left
# out (a stock slot has no /proc/odi_gpon, a diag that failed gives no
# optics), never written as zero:
#
#   t=      the wall clock, epoch seconds (meaningless before NTP synced)
#   up=     /proc/uptime, whole seconds; what orders the samples
#   st=     ONU state, 1 to 7 (diag gpon get onu-state)
#   al=     asserted GPON alarms, comma separated (LOS,LOF), or -
#   rx= tx= receive and transmit power, dBm; tc= module temperature, C;
#   bi=     bias current, mA (diag pon get transceiver)
#   di= do= octets in and out of switch port 2, the fibre side: totals,
#           the reader takes the difference between two samples
#   los= lof= lom=   alarm events since the driver loaded (odi_gpon)
#   o5i= o5o=        transitions into and out of O5 since then: a drop
#                    between two samples shows here even when both
#                    samples saw O5
#   bip=    downstream BIP error bits, total; fecu= uncorrectable FEC
#           codewords, total (/proc/odi_gpon/counters)
#   cb= ct= CPU jiffies busy and in all (/proc/stat), totals
#   ma=     MemAvailable, kB
#
# Cost: one diag run (the five questions on stdin, as confd and the exporter
# ask them), one awk and a rename a minute; the in-between is a sleep.
set -u

# Started by init with a bare environment.
PATH=${PATH:+$PATH:}/sbin:/usr/sbin:/bin:/usr/bin
export PATH

DIAG=${DIAG:-/bin/diag}
RING=${RING:-/var/run/history}
KEEP=${KEEP:-120}
INTERVAL_S=${INTERVAL_S:-60}
DIAG_TIMEOUT_S=${DIAG_TIMEOUT_S:-10}
PROC_UPTIME=${PROC_UPTIME:-/proc/uptime}
PROC_STAT=${PROC_STAT:-/proc/stat}
PROC_MEMINFO=${PROC_MEMINFO:-/proc/meminfo}
GPON_STATUS=${GPON_STATUS:-/proc/odi_gpon/status}
GPON_COUNTERS=${GPON_COUNTERS:-/proc/odi_gpon/counters}
TIMEOUT=${TIMEOUT:-timeout}
SLEEP=${SLEEP:-sleep}
NOW=${NOW:-date +%s}

# The same diag questions the Status tab asks, and only those that read
# without side effects; mib dump for port 2 alone, not all, keeps the reply
# under 2 KB.
DIAG_SCRIPT='pon get transceiver rx-power
pon get transceiver tx-power
pon get transceiver temperature
pon get transceiver bias-current
gpon get onu-state
gpon get alarm-status
mib dump counter port 2
exit
'

# A file that is not there reads as empty, so awk below sees nothing of it.
cat_or_empty() {
	[ -r "$1" ] && cat "$1" 2>/dev/null
	return 0
}

sample() {
	up=''
	read -r up _ < "$PROC_UPTIME" 2>/dev/null
	up=${up%%.*}
	t=$($NOW 2>/dev/null)
	{
		echo '@diag'
		if [ -x "$DIAG" ]; then
			printf '%s' "$DIAG_SCRIPT" | "$TIMEOUT" "$DIAG_TIMEOUT_S" "$DIAG" 2>/dev/null
		fi
		echo '@status'
		cat_or_empty "$GPON_STATUS"
		echo '@counters'
		cat_or_empty "$GPON_COUNTERS"
		echo '@stat'
		cat_or_empty "$PROC_STAT"
		echo '@meminfo'
		cat_or_empty "$PROC_MEMINFO"
	} | awk -v t="$t" -v up="$up" '
	function num(s) { return s ~ /^-?[0-9]+(\.[0-9]+)?$/ }
	function put(k, v) { if (v != "") line = line " " k "=" v }
	# The figure after the colon, rounded to two decimals.
	function fig(s,   v) {
		sub(/^[^:]*:[ \t]*/, "", s)
		sub(/[ \t].*$/, "", s)
		if (!num(s)) return ""
		v = sprintf("%.2f", s)
		return v
	}
	# diag puts a carriage return before some prompts (the stock
	# firmware does, and the exporter contract keeps it)
	{ gsub(/\r/, "") }
	/^@/ { sec = substr($0, 2); next }
	sec == "diag" && /^RTK\.0> / { cmd = substr($0, 8); next }
	sec == "diag" && cmd == "pon get transceiver rx-power" && /:/ { rx = fig($0) }
	sec == "diag" && cmd == "pon get transceiver tx-power" && /:/ { tx = fig($0) }
	sec == "diag" && cmd == "pon get transceiver temperature" && /:/ { tc = fig($0) }
	sec == "diag" && cmd == "pon get transceiver bias-current" && /:/ { bi = fig($0) }
	sec == "diag" && cmd == "gpon get onu-state" && match($0, /\(O[1-7]\)/) {
		st = substr($0, RSTART + 2, 1)
	}
	sec == "diag" && cmd == "gpon get alarm-status" && /^Alarm / {
		seen = 1
		if ($0 !~ /clear/) { n = $2; sub(/,$/, "", n); al = al (al == "" ? "" : ",") n }
	}
	sec == "diag" && cmd == "mib dump counter port 2" && $1 == "ifInOctets" && num($NF) { di = $NF }
	sec == "diag" && cmd == "mib dump counter port 2" && $1 == "ifOutOctets" && num($NF) { do_ = $NF }
	sec == "status" && $1 == "alarm_events" && num($3) { ev[$2] = $3 }
	sec == "status" && $1 == "onu_transitions" && num($4) {
		if ($3 == "O5") o5i += $4
		if ($2 == "O5") o5o += $4
		trans = 1
	}
	sec == "status" && $1 == "state" && num($2) { pst = $2 }
	sec == "counters" && $1 == "ctr" && $2 == "ds_bip_err_bits" && num($3) { bip = $3 }
	sec == "counters" && $1 == "ctr" && $2 == "ds_fec_uncorrectable_cw" && num($3) { fecu = $3 }
	sec == "stat" && $1 == "cpu" {
		tot = 0
		for (i = 2; i <= NF; i++) tot += $i
		# idle and iowait are the 4th and 5th figure
		cb = sprintf("%.0f", tot - $5 - (NF >= 6 ? $6 : 0)); ct = sprintf("%.0f", tot)
	}
	sec == "meminfo" && $1 == "MemAvailable:" && num($2) { ma = $2 }
	END {
		line = ""
		put("t", num(t) ? t : "")
		put("up", num(up) ? up : "")
		# diag first; the driver state when diag gave none (a diag that
		# timed out still leaves the kernel view)
		put("st", st != "" ? st : pst)
		if (seen) put("al", al == "" ? "-" : al)
		put("rx", rx); put("tx", tx); put("tc", tc); put("bi", bi)
		put("di", di); put("do", do_)
		put("los", ev["los"]); put("lof", ev["lof"]); put("lom", ev["lom"])
		if (trans || pst != "") { put("o5i", o5i + 0); put("o5o", o5o + 0) }
		put("bip", bip); put("fecu", fecu)
		put("cb", cb); put("ct", ct)
		put("ma", ma)
		sub(/^ /, "", line)
		print line
	}'
}

# append LINE: the ring with LINE last and at most KEEP lines, by rename.
append() {
	tmp=$RING.tmp
	{
		[ -f "$RING" ] && tail -n "$((KEEP - 1))" "$RING" 2>/dev/null
		printf '%s\n' "$1"
	} > "$tmp" && mv -f "$tmp" "$RING"
}

case ${1:-} in
sample)
	sample
	;;
run)
	while :; do
		l=$(sample)
		[ -n "$l" ] && append "$l"
		"$SLEEP" "$INTERVAL_S"
	done
	;;
*)
	echo "usage: $0 run|sample" >&2
	exit 2
	;;
esac
