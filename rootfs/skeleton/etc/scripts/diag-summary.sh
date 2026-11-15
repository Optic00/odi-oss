#!/bin/sh
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

#
# diag-summary.sh DIR -- one screen of facts from a diagnostics bundle,
# printed to stdout. DIR is the odi-diag/ directory of a bundle:
# diag-bundle.sh runs this on its own work directory and writes the output
# to SUMMARY.txt before the scrub, so the summary is scrubbed like every
# other file. It reads only the files already in DIR, never the device,
# so it runs as well on a PC against an unpacked bundle, also one taken
# before the bundle had a summary:
#
#     tar -xzf odi-diag.tar.gz && sh diag-summary.sh odi-diag
#
# Each line is a fact and the file it came from. A fact whose file is
# missing or does not have it says "not in bundle", never a guess. The
# counts from dmesg are those of the kernel ring as collected: the ring
# can have lost early lines, and odi_gpon rate-limits its event lines.
# Busybox only.

d=${1:-}
[ -n "$d" ] && [ -d "$d" ] || { echo "usage: diag-summary.sh DIR (the odi-diag/ directory of a bundle)" >&2; exit 1; }

na="not in bundle"

# val FILE: the file, or nothing if it is absent.
val() { [ -f "$d/$1" ] && cat "$d/$1"; }

# hms SECONDS: 3d 04h 12m, from the whole seconds.
hms() {
	awk -v s="$1" 'BEGIN {
		s = int(s)
		if (s >= 86400)
			printf "%dd %02dh %02dm\n", s / 86400, (s % 86400) / 3600, (s % 3600) / 60
		else if (s >= 3600)
			printf "%dh %02dm\n", s / 3600, (s % 3600) / 60
		else
			printf "%dm %02ds\n", s / 60, s % 60
	}'
}

image=$(val odi-build.txt | sed -n 's/^image=//p' | head -n 1)
up=$(val MANIFEST.txt | sed -n 's/^uptime: \([0-9][0-9.]*\) s$/\1/p' | head -n 1)
created=$(val MANIFEST.txt | sed -n 's/^created (stick clock, UTC): //p' | head -n 1)
ident=$(val MANIFEST.txt | sed -n 's/^identity: \([a-z]*\).*/\1/p' | head -n 1)

# nv.txt: "getenv sw_active: sw_active=1", or "(rc N)" after the colon.
nvval() {
	val nv.txt | sed -n "s/^getenv $1: $1=//p" | head -n 1
}
active=$(nvval sw_active)
commit=$(nvval sw_commit)
try=$(nvval sw_tryactive)

# /proc/odi_gpon/status: "state 5 (O5)".
state=$(val pon/odi_gpon-status.txt | sed -n 's/^state [0-9]* (\(O[1-7]\))$/\1/p' | head -n 1)

echo "odi-oss diagnostics summary (diag-summary.sh, from the files of this bundle)"
echo
echo "image          : ${image:-$na}${image:+  (odi-build.txt)}"
echo "taken          : ${created:-$na} stick clock  (MANIFEST.txt)"
if [ -n "$up" ]; then
	echo "uptime         : $(hms "$up")  (MANIFEST.txt)"
else
	echo "uptime         : $na"
fi
# sw_tryactive 2 is the committed state, no trial pending (src/nv/commit.c);
# 0 or 1 is the slot the next boot tries once.
case $try in
2) trial=none ;;
0 | 1) trial="slot $try pending" ;;
*) trial=unknown ;;
esac
echo "slot           : active ${active:-?}, committed ${commit:-?}, trial $trial  (nv.txt)"
case $ident in
masked) echo "identity       : masked, serial, MACs and LOID cut to their last four  (MANIFEST.txt)" ;;
kept) echo "identity       : KEPT, taken with --full: share this bundle privately only  (MANIFEST.txt)" ;;
*) echo "identity       : $na" ;;
esac
echo

# The ONU state, and the changes the kernel logged this boot, from dmesg:
# "[  123.456] odi_gpon: event=onu_state from=O5 to=O2 ... cause=X side=Y".
echo "onu state      : ${state:-$na}${state:+  (pon/odi_gpon-status.txt)}"
if [ -f "$d/dmesg.txt" ]; then
	awk -v up="$up" '
	function field(k,   i) {
		for (i = 1; i <= NF; i++)
			if (index($i, k "=") == 1)
				return substr($i, length(k) + 2)
		return ""
	}
	/event=onu_state / {
		n++
		t = $0
		sub(/^\[ */, "", t)
		sub(/\].*/, "", t)
		last_t = t
		last = "from=" field("from") " to=" field("to") " cause=" field("cause") " side=" field("side")
		if (field("from") == "O5") {
			drops++
			c = field("cause")
			if (!(c in cause))
				order[++nc] = c
			cause[c]++
		}
	}
	/event=los state=on/ { los++ }
	/event=lof state=on/ { lof++ }
	END {
		if (n == 0) {
			print "last change    : none in dmesg (in this state since boot, or the ring lost it)"
		} else {
			since = ""
			if (up != "" && last_t ~ /^[0-9.]+$/)
				since = sprintf(", %d s ago", up - last_t)
			printf "last change    : at %s s%s: %s  (dmesg.txt)\n", last_t, since, last
		}
		s = ""
		for (i = 1; i <= nc; i++)
			s = s (i > 1 ? ", " : "") order[i] " " cause[order[i]]
		if (drops)
			printf "left O5        : %d this boot: %s  (dmesg.txt)\n", drops, s
		else
			print "left O5        : 0 this boot  (dmesg.txt)"
		printf "LOS / LOF      : %d / %d this boot  (dmesg.txt)\n", los, lof
	}' "$d/dmesg.txt"
else
	echo "last change    : $na (no dmesg.txt)"
fi

# The GPON alarms set now, from gpon get alarm-status in pon/diag.txt:
# "Alarm LOF, status: occur". "none" only when the LOS, LOF and LOM lines
# are there and all clear: a failed read leaves the file with no such line
# (or "% alarm status read failed"), and that is no all-clear.
gal='^Alarm \(LOS\|LOF\|LOM\), status: \(clear\|occur\)'
if [ -f "$d/pon/diag.txt" ] && [ "$(grep -c "$gal" "$d/pon/diag.txt")" -ge 3 ]; then
	al=$(grep "$gal" "$d/pon/diag.txt" | sed -n 's/^Alarm \([^,]*\), status: occur.*/\1/p' | sort -u | tr '\n' ' ' | sed 's/ $//')
	echo "alarms now     : ${al:-none}  (pon/diag.txt)"
else
	echo "alarms now     : unread (no alarm-status lines in pon/diag.txt)"
fi

# omcli state: "mib resets : olt 3  local 0  ..." and "rx dropped : ...".
st=$d/pon/omcli_state.txt
for k in "mib resets" "rx dropped" "mib sync"; do
	v=$( [ -f "$st" ] && sed -n "s/^$k *: *//p" "$st" | head -n 1)
	printf '%-15s: %s\n' "$k" "${v:-$na}${v:+  (pon/omcli_state.txt)}"
done
n=$( [ -f "$d/syslog.txt" ] && grep -c 'event=unknown_me ' "$d/syslog.txt")
echo "unknown MEs    : ${n:-$na}${n:+ first requests logged  (syslog.txt; log/omcid-unknown.txt has the list)}"
echo

# Optics, from the transceiver lines of pon/diag.txt: "Rx Power: -22.92  dBm".
if [ -f "$d/pon/diag.txt" ]; then
	awk '
	function num(s) { sub(/^[^:]*: */, "", s); sub(/ +[A-Za-z]+$/, "", s); return s + 0 }
	/^Rx Power: / { rx = sprintf("%.2f dBm", num($0)) }
	/^Tx Power: / { tx = sprintf("%.2f dBm", num($0)) }
	/^Temperature: / { te = sprintf("%.1f C", num($0)) }
	/^Bias Current: / { bi = sprintf("%.2f mA", num($0)) }
	/^Voltage: / { vo = sprintf("%.2f V", num($0)) }
	END {
		if (rx == "" && tx == "" && te == "")
			print "optics         : not in bundle (no transceiver lines in pon/diag.txt)"
		else
			printf "optics         : rx %s, tx %s, %s, bias %s, %s  (pon/diag.txt)\n",
				rx ? rx : "?", tx ? tx : "?", te ? te : "?", bi ? bi : "?", vo ? vo : "?"
	}' "$d/pon/diag.txt"
else
	echo "optics         : $na"
fi

# The previous boot, from the ramlog: "previous boot: ... reason=wdt_userland".
pb=$(val ramlog_prev.txt | sed -n 's/^previous boot: .*reason=\([^ ]*\).*/\1/p' | head -n 1)
if [ -n "$pb" ]; then
	echo "previous boot  : ended by $pb  (ramlog_prev.txt)"
else
	echo "previous boot  : $na"
fi
sm=$( [ -f "$d/dmesg.txt" ] && sed -n 's/.*safe-mode: event=boot_count \(.*\)/\1/p' "$d/dmesg.txt" | tail -n 1)
echo "boot count     : ${sm:-$na}${sm:+  (dmesg.txt)}"
sf=$( [ -f "$d/dmesg.txt" ] && grep -c 'safe-mode: event=safe_mode ' "$d/dmesg.txt")
[ "${sf:-0}" -gt 0 ] && echo "SAFE MODE      : this boot is a safe-mode boot  (dmesg.txt)"
echo

# The steps that failed, from the manifest: "rc bytes file", rc a number.
if [ -f "$d/MANIFEST.txt" ]; then
	failed=$(awk '$1 ~ /^[0-9]+$/ && $1 != 0 && $2 ~ /^[0-9]+$/ { printf "%s%s (rc %s)", sep, $3, $1; sep = ", " }' "$d/MANIFEST.txt")
	echo "failed steps   : ${failed:-none}  (MANIFEST.txt)"
fi
exit 0
