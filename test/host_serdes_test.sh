#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

#
# rootfs/skeleton/etc/scripts/host-serdes.sh, off the device.
#
# diag is a stub with a register file: register get answers from it,
# register set writes it and logs "set ADDR VALUE" in order, so the
# read-modify-write steps see their own earlier writes as on hardware, and
# the log is the exact write sequence. The kernel parameter files are plain
# files, /proc/uptime is a file the sleep stub advances, and the host is a
# link model run after every second and before every command: "x2500" is a
# 2.5G host that syncs only while the stick runs 2500BASE-X with
# auto-negotiation off (it sets bit 17 of 0x214, as a stick in sync reads),
# "f1g" one that links only in Fiber 1G (host_link 1), "none" neither, and
# "late:<uptime>:<model>" behaves as "none" until that uptime, and
# "latch:<uptime>" is a 2.5G host that starts to send at that uptime and
# syncs only with a lane started (0x214 written in 2500BASE-X or HiSGMII
# with HOLD_CLK_GLITCH clear) at or after it, as a MikroTik hEX S 2025
# (E60iUGS) or RB5009 port switched to 2.5G-baseX after the stick does; it
# syncs 2500BASE-X with auto-negotiation off, and HiSGMII; with no model
# the registers are left as they are. With "$T/failrestore" the next write
# of 0x214 with HOLD_CLK_GLITCH clear fails and is not written, as a
# re-latch cut before its restore. Bit 17 is set in sync in either mode
# (a Fiber 1G stick linked to its host reads 0x260004). A forced port
# (host_forced 1) reads host_link 1 whatever the host does, as the kernel
# does. The real flash wrapper stores the key, in a temporary odi.conf.
# setsid only records what would be spawned; each test runs the spawned verb
# itself, so the order of events is the test's, not the scheduler's.
#
# What is checked: the key parsing; the boot in each mode (the write
# sequence of each, value for value and in order; the default writes
# nothing to the port side; a bad value, a safe-mode boot, no kernel
# parameter, sds.off); the 2500basex boot watch (sync, the fallback to
# Fiber 1G, a host that comes up late, no host at all); the trial (expiry,
# confirm, revert, a sequence that does not read back, a state file that
# cannot be written after the switch, every refusal); the
# auto-negotiation handed back to host-autoneg.sh in Fiber 1G; reapply;
# status; host-autoneg.sh standing down while a mode other than
# Fiber 1G runs. The safe-mode count is test/safe_mode_test.sh.
set -u
cd "$(dirname "$0")/.." || exit 1
# shellcheck source=test/host-flock.sh
. test/host-flock.sh
S=$PWD/rootfs/skeleton/etc/scripts/host-serdes.sh
AN=$PWD/rootfs/skeleton/etc/scripts/host-autoneg.sh
F=$PWD/rootfs/skeleton/etc/scripts/flash
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
pass=0 fail=0
ok() { echo "ok    $1"; pass=$((pass + 1)); }
no() { echo "FAIL  $1"; [ -n "${2:-}" ] && printf '%s\n' "$2" | sed 's/^/        /'; fail=$((fail + 1)); }
t() { if [[ $3 =~ $2 ]]; then ok "$1"; else no "$1" "wanted /$2/, got: $3"; fi; }
n() { if [[ $3 =~ $2 ]]; then no "$1" "did not want /$2/, got: $3"; else ok "$1"; fi; }
# same NAME FILE: the write log is exactly FILE.
same() { if cmp -s "$2" "$T/sdsset"; then ok "$1"; else no "$1" "$(diff "$2" "$T/sdsset" 2>&1)"; fi; }

mkdir -p "$T/bin" "$T/config" "$T/run" "$T/param"
cat > "$T/diag" <<EOF
#!/bin/sh
read -r line
set -- \$line
norm() { printf "0x%x" "\$(( \$1 ))"; }
case "\$1 \$2" in
"register get")
	a=\$(norm "\$3")
	v=\$(grep "^\$a=" "$T/regs" 2>/dev/null | head -n 1 | cut -d= -f2)
	[ -n "\$v" ] && printf "0x%08x 0x%08x\n" "\$(( a ))" "\$(( v ))"
	;;
"register set")
	a=\$(norm "\$3"); v=\$(printf "0x%x" "\$(( \$4 ))")
	echo "set \$a \$v" >> "$T/sdsset"
	[ -f "$T/readonly" ] && exit 0
	[ -f "$T/failrestore" ] && [ "\$a" = 0x214 ] && [ \$(( v & 0x2000 )) = 0 ] && { rm -f "$T/failrestore"; exit 1; }
	[ "\$a" = 0x214 ] && { [ \$(( v & 0x1f )) = 22 ] || [ \$(( v & 0x1f )) = 18 ]; } && [ \$(( v & 0x2000 )) = 0 ] && cut -d. -f1 "$T/uptime" > "$T/lanestart"
	grep -v "^\$a=" "$T/regs" > "$T/regs.n" 2>/dev/null; echo "\$a=\$v" >> "$T/regs.n"
	mv "$T/regs.n" "$T/regs"
	;;
esac
EOF
# model: the host link for the uptime, the mode the registers hold and the
# port force, written where the stick would show it.
cat > "$T/bin/model" <<EOF
#!/bin/sh
[ -f "$T/regs" ] && [ -f "$T/model" ] || exit 0
m=\$(cat "$T/model")
up=\$(cut -d. -f1 "$T/uptime")
case \$m in late:*)
	at=\${m#late:}; rest=\${at#*:}; at=\${at%%:*}
	if [ "\$up" -ge "\$at" ]; then m=\$rest; else m=none; fi ;;
esac
r=\$(grep '^0x214=' "$T/regs" | cut -d= -f2)
[ -n "\$r" ] || exit 0
mode=\$(( r & 0x1f ))
an=\$(cat "$T/param/host_autoneg" 2>/dev/null)
fo=\$(cat "$T/param/host_forced" 2>/dev/null)
sync=0 l=0
case \$m in
x2500) [ "\$mode" = 22 ] && { [ "\$an" = 0 ] || [ "\$an" = N ]; } && sync=1 ;;
latch:*)
	at=\${m#latch:}; ls=\$(cat "$T/lanestart" 2>/dev/null || echo 0)
	[ "\$up" -ge "\$at" ] && [ "\$ls" -ge "\$at" ] &&
		{ [ "\$mode" = 18 ] || { [ "\$mode" = 22 ] && { [ "\$an" = 0 ] || [ "\$an" = N ]; }; }; } && sync=1 ;;
f1g)   [ "\$mode" = 4 ] && l=1 sync=1 ;;
esac
{ [ "\$fo" = 1 ] || [ "\$fo" = Y ]; } && l=1
if [ "\$sync" = 1 ]; then r=\$(( r | 0x20000 )); else r=\$(( r & ~0x20000 )); fi
grep -v '^0x214=' "$T/regs" > "$T/regs.m"; printf '0x214=0x%x\n' "\$r" >> "$T/regs.m"; mv "$T/regs.m" "$T/regs"
echo \$l > "$T/param/host_link"
EOF
# An iohook, when present, runs once before the next command: the moment
# of a hardware write, between the load of the state file and its save.
cat > "$T/bin/timeout" <<EOF
#!/bin/sh
shift
"$T/bin/model"
[ -x "$T/iohook" ] && { mv "$T/iohook" "$T/iohook.run"; "$T/iohook.run"; }
exec "\$@"
EOF
cat > "$T/bin/setsid" <<EOF
#!/bin/sh
shift 2
echo "\$*" >> "$T/spawned"
EOF
cat > "$T/bin/sleep" <<EOF
#!/bin/sh
up=\$(cut -d. -f1 "$T/uptime")
echo "\$((up + \$1)).00 1.00" > "$T/uptime"
"$T/bin/model"
[ -x "$T/hook" ] && [ "\$1" -gt 1 ] && { "$T/hook"; rm -f "$T/hook"; }
[ -x "$T/everyhook" ] && "$T/everyhook"
exit 0
EOF
chmod +x "$T/bin/"* "$T/diag"

# mv: while $T/nomv exists, refuses to install the state file and nothing
# else, the state file that cannot be written.
printf '#!/bin/sh\nif [ -e "%s/nomv" ]; then case "$*" in *" %s/run/host-serdes") exit 1 ;; esac; fi\nexec /bin/mv "$@"\n' "$T" "$T" > "$T/bin/mv"
chmod +x "$T/bin/mv"

export PATH="$T/bin:$PATH"
export FLASH=$F SELF=$S DIAG=$T/diag PARAM_DIR=$T/param STATE=$T/run/host-serdes
export AN_STATE=$T/run/host-autoneg LOCKDIR=$T/run/lock SAFE_FLAG=$T/run/safe-mode SDS_OFF=$T/config/sds.off
export UPTIME=$T/uptime KMSG=$T/kmsg LOG=$T/services.log SLEEP=$T/bin/sleep TIMEOUT=$T/bin/timeout
export ODI_CONF=$T/config/odi.conf CS=$T/config/lastgood.xml HS=$T/config/lastgood_hs.xml TMPDIR_=$T
export WATCH_S=5 WATCH_MAX_S=40 TRIAL_S=20

st() { sed -n "s/^$1=//p" "$STATE" 2>/dev/null; }
key() { sed -n 's/^HOST_SERDES=//p' "$ODI_CONF" 2>/dev/null; }
setkey() { "$F" set HOST_SERDES "$1" > /dev/null; }
reg() { sed -n "s/^$1=//p" "$T/regs"; }
p() { cat "$T/param/$1" 2>/dev/null; }

# The Fiber 1G state as a running stick reads it, plus the words only the
# other modes compare (the 3.125G page reads 0 until a 3.125 Gbaud mode
# sets it, 0x21808 its reset value 0x70e0 until HiSGMII sets it).
fiber1g_regs() {
	cat > "$T/regs" <<'REGS'
0x214=0x260004
0x110=0x1
0x21c00=0x1140
0x22e4c=0x31b
0x21680=0x0
0x2168c=0x3001
0x21698=0x68
0x215b8=0x98c5
0x215b4=0x1264
0x215bc=0x400f
0x215e4=0xfc00
0x21700=0x0
0x2170c=0x0
0x21718=0x0
0x21808=0x70e0
0x21500=0x0
REGS
}

# A fresh boot: /var/run empty, the uptime again, the kernel parameters at
# their defaults (host_forced N, host_autoneg Y), the registers in Fiber 1G
# unless the test changes them; the config partition is kept.
newboot() {
	rm -rf "$T/run" "$T/spawned" "$T/hook" "$T/everyhook" "$T/model" "$T/sdsset" "$T/readonly" "$T/lanestart" "$T/failrestore"
	mkdir -p "$T/run"
	echo "15.00 1.00" > "$T/uptime"
	echo N > "$T/param/host_forced"
	echo Y > "$T/param/host_autoneg"
	echo 0 > "$T/param/host_link"
	fiber1g_regs
	: > "$T/kmsg"
}
spawned() {
	_i=0
	while [ ! -s "$T/spawned" ] && [ "$_i" -lt 100 ]; do /bin/sleep 0.05; _i=$((_i + 1)); done
	cat "$T/spawned" 2>/dev/null
}
spawned_none() { /bin/sleep 0.3; cat "$T/spawned" 2>/dev/null; }

# The write sequences, computed from the Fiber 1G starting values by the
# arithmetic of the read-modify-writes.
cat > "$T/want-fiber1g-from-sgmii" <<'WANT'
set 0x214 0x262002
set 0x214 0x26201f
set 0x21c00 0x1140
set 0x22e4c 0x31b
set 0x21680 0x0
set 0x2168c 0x3001
set 0x21698 0x68
set 0x215b8 0x98c5
set 0x215b4 0x1264
set 0x215bc 0x400f
set 0x215e4 0xfc00
set 0x214 0x260004
set 0x110 0x1
WANT
cat > "$T/want-2500basex" <<'WANT'
set 0x214 0x262004
set 0x214 0x26201f
set 0x21c00 0x1140
set 0x22e4c 0x431b
set 0x21680 0x0
set 0x2168c 0x3001
set 0x21698 0x68
set 0x215b8 0x98c5
set 0x215b4 0x1764
set 0x215bc 0x400f
set 0x215e4 0xfc00
set 0x21700 0x1800
set 0x2170c 0x7b01
set 0x21718 0x168
set 0x214 0x260016
set 0x110 0x1
set 0x21500 0x10
set 0x21500 0x30
set 0x21500 0x0
WANT
cat > "$T/want-hisgmii" <<'WANT'
set 0x214 0x262004
set 0x214 0x26201f
set 0x21c00 0x1940
set 0x22e4c 0x431b
set 0x21680 0x0
set 0x2168c 0x3001
set 0x21698 0x68
set 0x215b8 0x98c5
set 0x215b4 0x1564
set 0x215bc 0x400f
set 0x215e4 0xfc00
set 0x21700 0x1800
set 0x2170c 0x7b01
set 0x21718 0x168
set 0x214 0x260112
set 0x110 0x1
set 0x21500 0x10
set 0x21500 0x30
set 0x21500 0x0
set 0x21808 0x71e0
WANT
# Fiber 1G again from the 2500BASE-X state with no sync (bit 17 clear): the
# same thirteen writes, from those starting values.
cat > "$T/want-fiber1g-from-2500" <<'WANT'
set 0x214 0x242016
set 0x214 0x24201f
set 0x21c00 0x1140
set 0x22e4c 0x31b
set 0x21680 0x0
set 0x2168c 0x3001
set 0x21698 0x68
set 0x215b8 0x98c5
set 0x215b4 0x1264
set 0x215bc 0x400f
set 0x215e4 0xfc00
set 0x214 0x240004
set 0x110 0x1
WANT

# ---- the key
for v in fiber1g "" 2500basex hisgmii 2500BASEX 1g sideways; do
	got=$(sh "$S" parse "$v")
	case $v in 2500basex|hisgmii) want=$v ;; *) want=fiber1g ;; esac
	[ "$got" = "$want" ] && ok "parse '$v' -> $want" || no "parse '$v' -> $want" "got $got"
done

# ---- boot, key absent, already Fiber 1G: nothing at all
newboot
out=$(sh "$S" boot)
t "default boot: recognised, left alone" "SerDes already Fiber 1G \(option 1\)" "$out"
[ ! -f "$T/sdsset" ] && ok "default boot: no register write" || no "default boot: no register write" "$(cat "$T/sdsset")"
t "default boot: the port side untouched (host_forced N, host_autoneg Y)" "^N Y$" "$(p host_forced) $(p host_autoneg)"
t "default boot: setting and running fiber1g, steady" "^fiber1g fiber1g steady 0$" "$(st setting) $(st running) $(st phase) $(st fallback)"
t "default boot: nothing spawned" "^$" "$(spawned_none)"
t "default boot: nothing logged to the kernel" "^$" "$(cat "$T/kmsg")"

# ---- boot, key absent, the stick left in SGMII: the stock thirteen writes
newboot
sed -i.b 's/^0x214=.*/0x214=0x260002/; s/^0x215b4=.*/0x215b4=0x1564/; s/^0x21c00=.*/0x21c00=0x1940/' "$T/regs"
out=$(sh "$S" boot 2>&1)
t "default boot from SGMII: applies" "applying lan_sds_mode_set\(1\)" "$out"
same "default boot from SGMII: the thirteen writes, in order, the same values as ever" "$T/want-fiber1g-from-sgmii"
t "default boot from SGMII: reads back" "SerDes now Fiber 1G" "$out"
t "default boot from SGMII: the port side still untouched" "^N Y$" "$(p host_forced) $(p host_autoneg)"

# ---- unreadable registers: hands off
newboot; : > "$T/regs"
out=$(sh "$S" boot 2>&1)
t "unreadable: says so" "could not read the SerDes registers" "$out"
[ ! -f "$T/sdsset" ] && ok "unreadable: nothing written" || no "unreadable: nothing written"
t "unreadable: running unknown" "^unknown$" "$(st running)"

# ---- sds.off: no read, no write
newboot; : > "$SDS_OFF"
out=$(sh "$S" boot 2>&1)
[ ! -f "$T/sdsset" ] && ok "sds.off: no write" || no "sds.off: no write"
n "sds.off: no SerDes message" "SerDes" "$out"
t "sds.off: running unmanaged" "^unmanaged sds_off$" "$(st running) $(st event)"
out=$(sh "$S" try 2500basex 2>&1)
t "sds.off: a trial is refused" "sds.off" "$out"
rm -f "$SDS_OFF"

# ---- boot, 2500basex
setkey 2500basex
newboot
out=$(sh "$S" boot 2>&1)
echo x2500 > "$T/model"
same "2500basex boot: its nineteen writes, in order (3.125G page before the mode, the RX pulse after)" "$T/want-2500basex"
t "2500basex boot: reads back" "SerDes now 2500BASE-X" "$out"
t "2500basex boot: 0x21808 not written, its reset value" "^0x70e0$" "$(reg 0x21808)"
t "2500basex boot: port forced, auto-negotiation off" "^1 0$" "$(p host_forced) $(p host_autoneg)"
t "2500basex boot: running 2500basex, phase watch" "^2500basex watch$" "$(st running) $(st phase)"
t "2500basex boot: the watch spawned" "^watch$" "$(spawned)"
t "2500basex boot: event=applied" "event=applied setting=2500basex running=2500basex" "$(cat "$T/kmsg")"
t "2500basex boot: the target values (0x214 mode 0x16, 0x21c00 bit 11 clear, 0x22e4c 0x4000, 0x215b4 0x1764)" \
	"^0x16 0x0 0x4000 0x1764 0x0$" \
	"$(printf '0x%x 0x%x 0x%x %s %s' $(( $(reg 0x214) & 0xa11f )) $(( $(reg 0x21c00) & 0x800 )) $(( $(reg 0x22e4c) & 0xc000 )) "$(reg 0x215b4)" "$(reg 0x21500)")"
out=$(sh "$S" try fiber1g 2>&1); rc=$?
t "a trial is refused during the boot watch" "boot watch" "$out"
[ "$rc" = 1 ] && ok "  ... with exit 1" || no "  ... with exit 1" "rc $rc"
sh "$S" watch > /dev/null
t "watch, 2.5G host: in sync, no fallback" "^serdes_link 0 2500basex steady$" "$(st event) $(st fallback) $(st running) $(st phase)"
t "watch, 2.5G host: the sync bit is bit 17 of 0x214" "^1$" "$(( ($(reg 0x214) >> 17) & 1 ))"
t "status: link 1 from the SerDes sync" "link=1.*link_source=serdes_sync" "$(sh "$S" status | tr '\n' ' ')"
t "status: autoneg off, forced 1" "autoneg=off.*forced=1" "$(sh "$S" status | tr '\n' ' ')"

# The boot fallback: a host that links only in Fiber 1G.
newboot
sh "$S" boot > /dev/null 2>&1
echo f1g > "$T/model"
: > "$T/sdsset"
sh "$S" watch > /dev/null 2>&1
t "watch, Fiber 1G host: fallback, running fiber1g" "^fallback 1 fiber1g steady$" "$(st event) $(st fallback) $(st running) $(st phase)"
t "watch, Fiber 1G host: switched after WATCH_S" "event=watch_switch .*trying Fiber 1G" "$(cat "$T/kmsg")"
t "watch, Fiber 1G host: the port follows its SerDes again, auto-negotiation on" "^0 1$" "$(p host_forced) $(p host_autoneg)"
t "watch, Fiber 1G host: the registers are Fiber 1G" "^0x4$" "$(printf '0x%x' $(( $(reg 0x214) & 0xa11f )))"
same "watch, Fiber 1G host: the thirteen Fiber 1G writes" "$T/want-fiber1g-from-2500"
t "watch, Fiber 1G host: HOST_SERDES stays 2500basex" "^2500basex$" "$(key)"
t "status shows the fallback" "fallback=1.*link_source=host_link" "$(sh "$S" status | tr '\n' ' ')"

# The same with re-latches inside the 2500BASE-X phase (RESYNC_S below
# WATCH_S): the phase still ends after WATCH_S.
newboot
sh "$S" boot > /dev/null 2>&1
echo f1g > "$T/model"
WATCH_S=6 RESYNC_S=2 /usr/bin/timeout 60 sh "$S" watch > /dev/null 2>&1
t "watch, Fiber 1G host, re-latching in the phase: still falls back to Fiber 1G" "^fallback 1 fiber1g steady$" "$(st event) $(st fallback) $(st running) $(st phase)"

# A 2.5G host that comes up late: Fiber 1G, then 2500BASE-X again.
newboot
echo late:30:x2500 > "$T/model"
sh "$S" boot > /dev/null 2>&1; sh "$S" watch > /dev/null 2>&1
t "late 2.5G host: back to 2500basex, in sync, no fallback" "^serdes_link 0 2500basex$" "$(st event) $(st fallback) $(st running)"
t "late 2.5G host: it went to Fiber 1G, then 2500BASE-X again" "trying Fiber 1G.*trying 2500BASE-X" "$(tr '\n' ' ' < "$T/kmsg")"
t "late 2.5G host: port forced, auto-negotiation off again" "^1 0$" "$(p host_forced) $(p host_autoneg)"

# No host: bounded, and it ends on Fiber 1G, the most compatible mode;
# the key is not changed.
newboot
echo none > "$T/model"
sh "$S" boot > /dev/null 2>&1; sh "$S" watch > /dev/null 2>&1
t "no host: fallback_nolink, running fiber1g" "^fallback_nolink 1 fiber1g steady$" "$(st event) $(st fallback) $(st running) $(st phase)"
t "no host: the registers are Fiber 1G, the port follows its SerDes, auto-negotiation on" "^0x4 0 1$" "$(printf '0x%x' $(( $(reg 0x214) & 0xa11f ))) $(p host_forced) $(p host_autoneg)"
t "no host: HOST_SERDES stays 2500basex" "^2500basex$" "$(key)"
up=$(cut -d. -f1 "$T/uptime")
[ "$up" -le $((15 + 40 + 5 + 2)) ] && ok "no host: the watch ended within WATCH_MAX_S (uptime $up)" || no "no host: bounded" "uptime $up"

# ---- host-autoneg.sh stands down while 2500basex runs
newboot
echo x2500 > "$T/model"
sh "$S" boot > /dev/null 2>&1
"$F" set HOST_AUTONEG off > /dev/null
anrun() { STATE=$AN_STATE SERDES_STATE=$T/run/host-serdes sh "$AN" "$@"; }
anrun boot > /dev/null 2>&1
t "host-autoneg boot with 2500basex: event=serdes_mode, nothing written" "^serdes_mode 0$" "$(sed -n 's/^event=//p' "$AN_STATE") $(p host_autoneg)"
[ "$(spawned)" = watch ] && ok "host-autoneg boot with 2500basex: no boot watch of its own" || no "host-autoneg: no watch of its own" "$(cat "$T/spawned")"
sh "$S" watch > /dev/null 2>&1
out=$(anrun try on 2>&1); rc=$?
t "host-autoneg try with 2500basex running is refused" "HOST_SERDES 2500basex" "$out"
[ "$rc" = 1 ] && ok "  ... with exit 1" || no "  ... with exit 1" "rc $rc"
out=$(anrun status | tr '\n' ' ')
t "host-autoneg status with 2500basex: the stored setting, the live bit (off), owner host-serdes" \
	"setting=off running=off owner=host-serdes owner_message=the link mode HOST_SERDES=2500basex" "$out"
t "  ... and its own state file still holds what it runs in Fiber 1G (on)" "^on$" "$(sed -n 's/^running=//p' "$AN_STATE")"
"$F" set HOST_AUTONEG "" > /dev/null
# Back in Fiber 1G (a reverted trial), host-autoneg.sh owns the bit again.
sh "$S" try fiber1g > /dev/null
id=$(st trial_id)
printf '#!/bin/sh\nsh "%s" confirm >> "%s/confirm.out" 2>&1\n' "$S" "$T" > "$T/hook"; chmod +x "$T/hook"
sh "$S" trial-run "$id" 20 > /dev/null 2>&1
out=$(anrun status | tr '\n' ' ')
t "host-autoneg status in Fiber 1G: running on, owner host-autoneg, no owner_message" "running=on owner=host-autoneg phase=" "$out"
n "  ... no owner_message" "owner_message" "$out"
setkey ""

# ---- hisgmii
setkey hisgmii
newboot
out=$(sh "$S" boot 2>&1)
same "hisgmii boot: its twenty writes (PCS powered down, the 3.125G page, 0x214 mode 0x112, the RX pulse, 0x21808 last)" "$T/want-hisgmii"
t "hisgmii boot: port forced, auto-negotiation bit on" "^1 1$" "$(p host_forced) $(p host_autoneg)"
t "hisgmii boot: steady, no watch (no link signal)" "^hisgmii steady applied$" "$(st running) $(st phase) $(st event)"
t "hisgmii boot: nothing spawned" "^$" "$(spawned_none)"
t "hisgmii status: link unknown, no source" "link=unknown.*link_source=none" "$(sh "$S" status | tr '\n' ' ')"
t "hisgmii boot: reads back, the label" "SerDes now HiSGMII \(3\.125 Gbaud\)" "$out"
t "hisgmii boot: 0x21808 0x71e0" "^0x71e0$" "$(reg 0x21808)"
cp "$T/regs" "$T/regs.hisgmii"
# The next boot over a stick that holds HiSGMII, 0x21808 included: nothing.
newboot; cp "$T/regs.hisgmii" "$T/regs"
out=$(sh "$S" boot 2>&1)
t "hisgmii boot over HiSGMII: recognised" "SerDes already HiSGMII \(3\.125 Gbaud\)" "$out"
[ ! -f "$T/sdsset" ] && ok "hisgmii boot over HiSGMII: nothing written" || no "hisgmii boot over HiSGMII: nothing written" "$(cat "$T/sdsset")"
# The same with 0x21808 alone at its reset value: the whole sequence again.
newboot; sed 's/^0x21808=.*/0x21808=0x70e0/' "$T/regs.hisgmii" > "$T/regs"
out=$(sh "$S" boot 2>&1)
t "hisgmii boot, 0x21808 at 0x70e0: the twenty writes, 0x21808 last" "^20 set 0x21808 0x71e0$" "$(wc -l < "$T/sdsset" | tr -d ' ') $(tail -n 1 "$T/sdsset")"
t "hisgmii boot, 0x21808 at 0x70e0: reads back" "SerDes now HiSGMII \(3\.125 Gbaud\)" "$out"
rm -f "$T/regs.hisgmii"

# ---- a value that is none of them: Fiber 1G, logged
setkey sideways
newboot
sh "$S" boot > /dev/null 2>&1
t "HOST_SERDES=sideways runs fiber1g" "^fiber1g fiber1g$" "$(st setting) $(st running)"
t "  ... and says so" "event=bad_value" "$(cat "$T/kmsg")"
t "  ... with the port side untouched" "^N Y$" "$(p host_forced) $(p host_autoneg)"

# ---- no kernel parameter: Fiber 1G
setkey 2500basex
newboot; rm -f "$T/param/host_forced"
sh "$S" boot > /dev/null 2>&1
t "no host_forced parameter: running fiber1g, logged" "^no_kernel fiber1g$" "$(st event) $(st running)"
[ ! -f "$T/sdsset" ] && ok "  ... and the Fiber 1G registers left alone" || no "  ... left alone"
out=$(sh "$S" try 2500basex 2>&1)
t "  ... and a trial of 2500basex refused" "no odi_switch.host_forced" "$out"

# ---- a safe-mode boot: Fiber 1G, no trial. Removing the key is
# safe-mode.sh's (test/safe_mode_test.sh); this script writes none.
newboot
echo "short_boots=3" > "$SAFE_FLAG"
sh "$S" boot > /dev/null 2>&1
t "safe-mode boot: Fiber 1G, nothing on the port side" "^fiber1g N Y safe_mode_boot$" "$(st running) $(p host_forced) $(p host_autoneg) $(st event)"
t "safe-mode boot: host-serdes.sh does not write the key" "^2500basex$" "$(key)"
t "safe-mode boot: no watch" "^$" "$(spawned_none)"
out=$(sh "$S" try 2500basex 2>&1)
t "safe-mode boot: a trial is refused" "safe-mode boot" "$out"
t "status says so" "safe_mode_boot=1" "$(sh "$S" status)"
rm -f "$SAFE_FLAG"
out=$(sh "$S" clear 2>&1); rc=$?
t "there is no clear verb" "usage" "$out"
[ "$rc" = 1 ] && ok "  ... exit 1" || no "  ... exit 1" "rc $rc"
setkey ""

# ---- the trial, from Fiber 1G
setkey ""
newboot
echo x2500 > "$T/model"
sh "$S" boot > /dev/null
out=$(sh "$S" try 2500basex)
t "try 2500basex: started" "event=trial_started .*trying 2500BASE-X for 20 s" "$out"
[ ! -f "$T/sdsset" ] && ok "try: nothing written yet (the answer leaves first)" || no "try: nothing written yet"
t "try: phase trial, deadline uptime+21" "^trial 36$" "$(st phase) $(st trial_deadline)"
id=$(st trial_id)
t "try: trial-run spawned with its id" "^trial-run $id 20$" "$(spawned)"
out=$(sh "$S" confirm 2>&1)
t "confirm before the switch is refused" "not applied yet" "$out"
out=$(sh "$S" try hisgmii 2>&1)
t "a second trial is refused" "already running" "$out"
out=$(sh "$S" reapply 2>&1)
t "reapply during a trial is refused" "already running" "$out"
sh "$S" trial-run "$id" 20 > /dev/null 2>&1
t "expiry: back to fiber1g" "^trial_expired fiber1g steady$" "$(st event) $(st running) $(st phase)"
t "expiry: the port follows its SerDes, auto-negotiation on" "^0 1$" "$(p host_forced) $(p host_autoneg)"
t "expiry: the registers are Fiber 1G" "^0x4 0x1264$" "$(printf '0x%x' $(( $(reg 0x214) & 0xa11f ))) $(reg 0x215b4)"
t "expiry: the key is not stored" "^$" "$(key)"
t "expiry: logged" "event=trial_expired .*not confirmed in time" "$(cat "$T/kmsg")"

# confirm inside the window: stored, the expiry after it a no-op
: > "$T/spawned"
sh "$S" try 2500basex > /dev/null
id=$(st trial_id)
printf '#!/bin/sh\nsh "%s" confirm >> "%s/confirm.out" 2>&1\n' "$S" "$T" > "$T/hook"; chmod +x "$T/hook"
sh "$S" trial-run "$id" 20 > /dev/null 2>&1
t "confirm: stored HOST_SERDES=2500basex" "^2500basex$" "$(key)"
t "confirm: state" "^trial_confirmed 2500basex 2500basex steady$" "$(st event) $(st setting) $(st running) $(st phase)"
t "confirm: the expiry after it changed nothing" "^1 0 0x16$" "$(p host_forced) $(p host_autoneg) $(printf '0x%x' $(( $(reg 0x214) & 0xa11f )))"
out=$(sh "$S" confirm 2>&1); rc=$?
t "confirm with no trial is refused" "no trial is running" "$out"
[ "$rc" = 1 ] && ok "  ... with exit 1" || no "  ... with exit 1" "rc $rc"

# revert inside the window, from 2500basex to fiber1g and back
: > "$T/spawned"
sh "$S" try fiber1g > /dev/null
id=$(st trial_id)
printf '#!/bin/sh\nsh "%s" revert >> "%s/revert.out" 2>&1\n' "$S" "$T" > "$T/hook"; chmod +x "$T/hook"
sh "$S" trial-run "$id" 20 > /dev/null 2>&1
t "revert: back to 2500basex, the key unchanged" "^trial_reverted 2500basex 2500basex$" "$(st event) $(st running) $(key)"
t "revert: forced, auto-negotiation off again" "^1 0 0x16$" "$(p host_forced) $(p host_autoneg) $(printf '0x%x' $(( $(reg 0x214) & 0xa11f )))"

# confirming fiber1g removes the key
sh "$S" try fiber1g > /dev/null
id=$(st trial_id)
printf '#!/bin/sh\nsh "%s" confirm >> "%s/confirm.out" 2>&1\n' "$S" "$T" > "$T/hook"; chmod +x "$T/hook"
sh "$S" trial-run "$id" 20 > /dev/null 2>&1
t "confirm fiber1g: the key is removed" "^$" "$(key)"
t "confirm fiber1g: setting fiber1g, the port follows its SerDes" "^trial_confirmed fiber1g 0 1$" "$(st event) $(st setting) $(p host_forced) $(p host_autoneg)"

# ---- the race: a host switched to 2.5G after the trial applied
# The trial writes the sequence 1 s after try; the host port is set to
# 2.5G-baseX by hand after that, over the link the trial just dropped. A
# lane started before the host sends never syncs; while bit 17 stays clear
# the trial re-latches the lane every RESYNC_S seconds, and leaves a lane in
# sync alone.
# confirm_on_sync: an everyhook that confirms once the lane reads in sync,
# as a user does once the ping answers.
confirm_on_sync() {
	cat > "$T/everyhook" <<EOF
#!/bin/sh
[ -f "$T/confirmed" ] && exit 0
r=\$(sed -n 's/^0x214=//p' "$T/regs")
[ \$(( (r >> 17) & 1 )) = 1 ] || exit 0
: > "$T/confirmed"
sh "$S" confirm >> "$T/confirm.out" 2>&1
EOF
	chmod +x "$T/everyhook"
	rm -f "$T/confirmed"
}
setkey ""
newboot
sh "$S" boot > /dev/null
echo latch:20 > "$T/model"
: > "$T/spawned"
sh "$S" try 2500basex > /dev/null
id=$(st trial_id)
confirm_on_sync
out=$(sh "$S" trial-run "$id" 20 2>&1)
t "race: a host at 2.5G only after the apply, the trial still syncs and is confirmed" \
	"^trial_confirmed 2500basex 2500basex$" "$(st event) $(key) $(st running)"
t "race: said so" "re-latching the lane every 5 s" "$out"
[ "$(printf '%s\n' "$out" | grep -c 're-latching')" = 1 ] &&
	ok "  ... in one line, not one per re-latch" || no "  ... in one line" "$out"
up=$(cut -d. -f1 "$T/uptime")
[ "$up" -lt $((15 + 1 + 20)) ] && ok "  ... and the window ends with the confirm (uptime $up)" ||
	no "  ... and the window ends with the confirm" "uptime $up"
rm -f "$T/everyhook"
# Reverted within the window: the window ends there too.
setkey ""
newboot
sh "$S" boot > /dev/null
echo none > "$T/model"
: > "$T/spawned"
sh "$S" try 2500basex > /dev/null
id=$(st trial_id)
printf '#!/bin/sh\nsh "%s" revert >> "%s/revert.out" 2>&1\n' "$S" "$T" > "$T/hook"
chmod +x "$T/hook"
sh "$S" trial-run "$id" 20 > /dev/null 2>&1
up=$(cut -d. -f1 "$T/uptime")
t "race, reverted: back to fiber1g" "^trial_reverted fiber1g$" "$(st event) $(st running)"
[ "$up" -lt $((15 + 1 + 20)) ] && ok "  ... and the window ends with the revert (uptime $up)" ||
	no "  ... and the window ends with the revert" "uptime $up"
# A host already at 2.5G: in sync from the apply, never re-latched.
setkey ""
newboot
sh "$S" boot > /dev/null
echo x2500 > "$T/model"
: > "$T/spawned"
sh "$S" try 2500basex > /dev/null
id=$(st trial_id)
out=$(sh "$S" trial-run "$id" 20 2>&1)
t "race control: a host in sync, the trial expires as before" "^trial_expired fiber1g$" "$(st event) $(st running)"
t "race control: the lane parked by the sequences only (in, and back out)" "^2$" "$(grep -c 'set 0x214 0x[0-9a-f]*201f' "$T/sdsset")"
n "race control: nothing re-latched" "re-latching" "$out"
# No host at all: re-latched within the window, then the expiry as before.
setkey ""
newboot
sh "$S" boot > /dev/null
echo none > "$T/model"
: > "$T/spawned"
sh "$S" try 2500basex > /dev/null
id=$(st trial_id)
sh "$S" trial-run "$id" 20 > /dev/null 2>&1
t "race, no host: the expiry back to fiber1g, the key unchanged" "^trial_expired fiber1g $" "$(st event) $(st running) $(key)"
t "race, no host: the registers are Fiber 1G again" "^0x4 0x1264$" "$(printf '0x%x' $(( $(reg 0x214) & 0xa11f ))) $(reg 0x215b4)"
up=$(cut -d. -f1 "$T/uptime")
[ "$up" -le $((15 + 1 + 20 + 6)) ] && ok "race, no host: the window not stretched by the re-latches (uptime $up)" ||
	no "race, no host: the window not stretched" "uptime $up"
# HiSGMII has the same race (a host port switched to 2.5G after a live
# switch to it, as measured on a MikroTik RB5009): the window re-latches it
# too, back into HiSGMII. Bit 17 is the sync check of the window only; the
# status keeps no link source for HiSGMII.
setkey ""
newboot
sh "$S" boot > /dev/null
echo latch:20 > "$T/model"
: > "$T/spawned"
sh "$S" try hisgmii > /dev/null
id=$(st trial_id)
confirm_on_sync
out=$(sh "$S" trial-run "$id" 20 2>&1)
t "race, hisgmii: a host at 2.5G only after the apply, the trial still syncs and is confirmed" \
	"^trial_confirmed hisgmii hisgmii$" "$(st event) $(key) $(st running)"
t "race, hisgmii: said so" "no sync in HiSGMII.*re-latching the lane every 5 s" "$out"
t "race, hisgmii: re-latched into HiSGMII (0x112 under 0xa11f), 0x21808 bit 8 kept" "^0x112 0x71e0$" \
	"$(printf '0x%x' $(( $(reg 0x214) & 0xa11f ))) $(reg 0x21808)"
t "race, hisgmii: the status keeps no link source" "link_source=none" "$(sh "$S" status | tr '\n' ' ')"
rm -f "$T/everyhook"
# A Fiber 1G trial sleeps its window as before: no re-latch.
echo none > "$T/model"
: > "$T/spawned"
sh "$S" try fiber1g > /dev/null
id=$(st trial_id)
: > "$T/sdsset"
out=$(sh "$S" trial-run "$id" 20 2>&1)
n "race: a fiber1g trial is not re-latched" "re-latching" "$out"
t "  ... the lane parked by the sequences only (in, and back out)" "^2$" "$(grep -c 'set 0x214 0x[0-9a-f]*1f$' "$T/sdsset")"
setkey ""
# The boot watch: a host switched to 2.5G after the boot applied. Each
# 2500BASE-X phase re-latches every RESYNC_S seconds while out of sync, so
# it syncs within the phase, with no fallback to Fiber 1G.
setkey 2500basex
newboot
sh "$S" boot > /dev/null 2>&1
echo latch:17 > "$T/model"
RESYNC_S=2 sh "$S" watch > /dev/null 2>&1
t "race, boot watch: in sync in 2500basex within the first phase, no fallback" "^serdes_link 0 2500basex$" "$(st event) $(st fallback) $(st running)"
n "race, boot watch: no switch to Fiber 1G" "watch_switch" "$(cat "$T/kmsg")"
# A re-latch cut between the park and the restore (a diag timeout, a
# failed write) leaves the lane parked. The next one starts it again in
# 2500BASE-X, not with the parked word it reads, and the failed write is
# reported.
setkey 2500basex
newboot
sh "$S" boot > /dev/null 2>&1
echo latch:17 > "$T/model"
: > "$T/failrestore"
out=$(RESYNC_S=2 sh "$S" watch 2>&1)
t "re-latch cut before its restore: the failed write reported" "SerDes write 0x214 failed.*could not be re-latched" "$(printf '%s' "$out" | tr '\n' ' ')"
t "  ... the next re-latch syncs the lane, no fallback" "^serdes_link 0 2500basex$" "$(st event) $(st fallback) $(st running)"
n "  ... within the first phase, no switch to Fiber 1G" "watch_switch" "$(cat "$T/kmsg")"
t "  ... the lane in 2500BASE-X, the clock running" "^0x16$" "$(printf '0x%x' $(( $(reg 0x214) & 0xa11f )))"
setkey ""
rm -f "$T/model"

# 0x21808 bit 8 (in-band negotiation forced off on the host lane): HiSGMII
# sets it, Fiber 1G and 2500BASE-X clear it, so no change out of HiSGMII
# leaves it set. A revert from an applied HiSGMII to Fiber 1G:
setkey ""
newboot
sh "$S" boot > /dev/null
: > "$T/spawned"
sh "$S" try hisgmii > /dev/null
id=$(st trial_id)
printf '#!/bin/sh\ncp "%s/regs" "%s/regs.trial"; sh "%s" revert >> "%s/revert.out" 2>&1\n' "$T" "$T" "$S" "$T" > "$T/hook"; chmod +x "$T/hook"
sh "$S" trial-run "$id" 20 > /dev/null 2>&1
t "hisgmii trial: applied, 0x21808 bit 8 set" "^0x71e0$" "$(sed -n 's/^0x21808=//p' "$T/regs.trial")"
t "revert from hisgmii: back to fiber1g" "^trial_reverted fiber1g$" "$(st event) $(st running)"
t "revert from hisgmii: 0x21808 bit 8 clear" "^0x70e0$" "$(reg 0x21808)"
t "revert from hisgmii: the clear is the last write" "^set 0x21808 0x70e0$" "$(tail -n 1 "$T/sdsset")"
t "revert from hisgmii: Fiber 1G mode, PCS powered up" "^0x4 0x0$" "$(printf '0x%x 0x%x' $(( $(reg 0x214) & 0xa11f )) $(( $(reg 0x21c00) & 0x800 )))"
rm -f "$T/regs.trial"
: > "$T/sdsset"
sh "$S" reapply > /dev/null 2>&1
sh "$S" reapply-run > /dev/null 2>&1
t "reapply of fiber1g, bit 8 already clear: the thirteen writes, none to 0x21808" "^13 0$" "$(wc -l < "$T/sdsset" | tr -d ' ') $(grep -c 0x21808 "$T/sdsset")"

# HiSGMII confirmed, then 2500BASE-X confirmed: the stored mode runs with
# bit 8 clear, as the next cold boot runs it.
: > "$T/spawned"
sh "$S" try hisgmii > /dev/null
id=$(st trial_id)
printf '#!/bin/sh\nsh "%s" confirm >> "%s/confirm.out" 2>&1\n' "$S" "$T" > "$T/hook"; chmod +x "$T/hook"
sh "$S" trial-run "$id" 20 > /dev/null 2>&1
t "confirm hisgmii: stored, 0x21808 0x71e0" "^hisgmii 0x71e0$" "$(key) $(reg 0x21808)"
: > "$T/spawned"
echo x2500 > "$T/model"
sh "$S" try 2500basex > /dev/null
id=$(st trial_id)
printf '#!/bin/sh\nsh "%s" confirm >> "%s/confirm.out" 2>&1\n' "$S" "$T" > "$T/hook"; chmod +x "$T/hook"
sh "$S" trial-run "$id" 20 > /dev/null 2>&1
t "hisgmii -> 2500basex confirmed: stored, 0x21808 bit 8 clear" "^2500basex 2500basex 0x70e0$" "$(key) $(st running) $(reg 0x21808)"
rm -f "$T/model"

# A Fiber 1G boot over a stick that still holds bit 8 (a warm reboot out of
# HiSGMII): the compare sees it, the sequence runs and clears it, last.
setkey ""
newboot
sed -i.b 's/^0x21808=.*/0x21808=0x71e0/' "$T/regs"
out=$(sh "$S" boot 2>&1)
t "fiber1g boot, bit 8 set: not taken as Fiber 1G" "SerDes now Fiber 1G" "$out"
t "fiber1g boot, bit 8 set: fourteen writes, the clear last" "^14 set 0x21808 0x70e0$" "$(wc -l < "$T/sdsset" | tr -d ' ') $(tail -n 1 "$T/sdsset")"

# The state file cannot be written after the switch (/var full). The file
# still says the mode before, and expire and revert go by it, so the port
# has to go back at once. The state directory is the link $T/varlink to
# $T/run here: the iohook takes it away at the first register access and
# the trial window puts it back, which stops root too (a read-only
# directory would not).
newboot
echo x2500 > "$T/model"
ln -s run "$T/varlink"
STATE=$T/varlink/host-serdes sh "$S" boot > /dev/null
STATE=$T/varlink/host-serdes sh "$S" try 2500basex > /dev/null
id=$(st trial_id)
printf '#!/bin/sh\nmv "%s/varlink" "%s/varlink.off"\n' "$T" "$T" > "$T/iohook"; chmod +x "$T/iohook"
printf '#!/bin/sh\ngrep "^0x214=" "%s/regs" > "%s/window.reg"\nmv "%s/varlink.off" "%s/varlink"\n' "$T" "$T" "$T" "$T" > "$T/hook"; chmod +x "$T/hook"
out=$(STATE=$T/varlink/host-serdes sh "$S" trial-run "$id" 20 2>&1)
t "no state file after the switch: said so" "could not write" "$out"
t "  the registers are Fiber 1G before the window" "^0x4$" "$(printf '0x%x' $(( $(sed 's/^0x214=//' "$T/window.reg") & 0xa11f )))"
t "  and after it, the port following its SerDes" "^0x4 0 1$" "$(printf '0x%x' $(( $(reg 0x214) & 0xa11f ))) $(p host_forced) $(p host_autoneg)"
t "  logged" "event=trial_failed .*could not write .*back to Fiber 1G" "$(cat "$T/kmsg")"
t "  the expiry clears the trial once the file can be written" "^fiber1g steady $" "$(st running) $(st phase) $(st trial_id)"

# Back in Fiber 1G the auto-negotiation is what host-autoneg.sh last ran.
newboot
printf 'setting=off\nrunning=off\nphase=steady\n' > "$AN_STATE"
echo N > "$T/param/host_autoneg"
sh "$S" boot > /dev/null
sh "$S" try 2500basex > /dev/null
id=$(st trial_id)
sh "$S" trial-run "$id" 20 > /dev/null 2>&1
t "HOST_AUTONEG=off running: back from a trial, auto-negotiation off again" "^trial_expired 0 0$" "$(st event) $(p host_forced) $(p host_autoneg)"
printf 'setting=off\nrunning=off\nphase=trial\n' > "$AN_STATE"
out=$(sh "$S" try 2500basex 2>&1)
t "a trial is refused while host-autoneg.sh runs one" "host-autoneg.sh has a trial" "$out"
rm -f "$AN_STATE"

# A sequence that does not read back (diag takes no write): back at once.
newboot
sh "$S" boot > /dev/null
sh "$S" try hisgmii > /dev/null
id=$(st trial_id)
: > "$T/readonly"
sh "$S" trial-run "$id" 20 > /dev/null 2>&1
t "a trial whose registers do not read back: trial_failed, back to fiber1g" "^trial_failed fiber1g steady$" "$(st event) $(st running) $(st phase)"
t "  ... with the port side back too" "^0 1$" "$(p host_forced) $(p host_autoneg)"
rm -f "$T/readonly"

# A move back that does not read back is not reported as back: the trial
# went (HiSGMII), then diag takes no write, so the registers stay HiSGMII.
# The mode before is taken as running, so reapply can run it again.
newboot
sh "$S" boot > /dev/null
sh "$S" try hisgmii > /dev/null
id=$(st trial_id)
printf '#!/bin/sh\n: > "%s/readonly"\n' "$T" > "$T/hook"; chmod +x "$T/hook"
sh "$S" trial-run "$id" 20 > /dev/null 2>&1
t "an expiry that does not read back: revert_unverified, fiber1g, steady" "^revert_unverified fiber1g steady$" "$(st event) $(st running) $(st phase)"
t "  ... logged as such, naming reapply" "event=revert_unverified .*not confirmed in time: back to Fiber 1G, but its registers did not all read back \(host-serdes.sh reapply" "$(cat "$T/kmsg")"
n "  ... and not as trial_expired" "event=trial_expired" "$(cat "$T/kmsg")"
rm -f "$T/readonly"
out=$(sh "$S" reapply 2>&1); rc=$?
t "  ... reapply is not refused after it" "^0 reapply$" "$rc $(st phase)"
newboot
sh "$S" boot > /dev/null
sh "$S" try hisgmii > /dev/null
id=$(st trial_id)
printf '#!/bin/sh\n: > "%s/readonly"\nsh "%s" revert > "%s/revert.out" 2>&1; echo $? > "%s/revert.rc"\n' "$T" "$S" "$T" "$T" > "$T/hook"; chmod +x "$T/hook"
sh "$S" trial-run "$id" 20 > /dev/null 2>&1
t "a revert that does not read back: exit 1, revert_unverified" "^1 revert_unverified fiber1g$" "$(cat "$T/revert.rc") $(st event) $(st running)"
t "  ... and its answer says so" "reverted on request: back to Fiber 1G, but its registers did not all read back" "$(cat "$T/revert.out")"
rm -f "$T/readonly"

# The port side is part of the way back: a host_autoneg that cannot be
# written is revert_unverified too, although the registers read back.
newboot
sh "$S" boot > /dev/null
sh "$S" try hisgmii > /dev/null
id=$(st trial_id)
printf '#!/bin/sh\nrm -f "%s/param/host_autoneg"; mkdir "%s/param/host_autoneg"\n' "$T" "$T" > "$T/hook"; chmod +x "$T/hook"
sh "$S" trial-run "$id" 20 > /dev/null 2>&1
t "an expiry whose port side cannot be written: revert_unverified" "^revert_unverified fiber1g steady$" "$(st event) $(st running) $(st phase)"
t "  ... naming the port side" "event=revert_unverified .*back to Fiber 1G, but its port side \(host_forced, host_autoneg\) could not be written" "$(cat "$T/kmsg")"
rmdir "$T/param/host_autoneg"; echo 1 > "$T/param/host_autoneg"

# A host_forced that cannot be read on the way back is cleared all the
# same: it is as likely the force the trial left as anything else.
newboot
sh "$S" boot > /dev/null
sh "$S" try hisgmii > /dev/null
id=$(st trial_id)
printf '#!/bin/sh\necho garbage > "%s/param/host_forced"\n' "$T" > "$T/hook"; chmod +x "$T/hook"
sh "$S" trial-run "$id" 20 > /dev/null 2>&1
t "an expiry with host_forced unreadable: cleared, back to fiber1g" "^0 trial_expired fiber1g$" "$(p host_forced) $(st event) $(st running)"

# A revert whose state file cannot be written leaves the trial on disk, so
# it is no success either.
newboot
sh "$S" boot > /dev/null
sh "$S" try hisgmii > /dev/null
id=$(st trial_id)
printf '#!/bin/sh\n: > "%s/nomv"\nsh "%s" revert > /dev/null 2>&1; echo $? > "%s/revert.rc"\nrm -f "%s/nomv"\n' "$T" "$S" "$T" "$T" > "$T/hook"; chmod +x "$T/hook"
sh "$S" trial-run "$id" 20 > /dev/null 2>&1
t "a revert whose state file cannot be written: exit 1" "^1$" "$(cat "$T/revert.rc")"
t "  ... though the way back itself read back" "event=trial_reverted .*reverted on request: back to Fiber 1G;" "$(cat "$T/kmsg")"

# the refusals
out=$(sh "$S" try fiber1g 2>&1)
t "try fiber1g while fiber1g: nothing to try, and reapply named" "already fiber1g.*reapply" "$out"
out=$(sh "$S" try 1g 2>&1)
t "try with a bad mode is refused" "usage" "$out"
out=$(sh "$S" try 2500basex 5 2>&1)
t "try with too short a window is refused" "10 to" "$out"
out=$(sh "$S" try 2500basex 9999 2>&1)
t "try with too long a window is refused" "10 to" "$out"
sh "$S" try 2500basex > /dev/null
sh "$S" expire "1.1" > /dev/null
t "an expiry with a stale id does nothing" "^trial$" "$(st phase)"
sh "$S" revert > /dev/null 2>&1

# ---- reapply: the running mode again, unconditionally
newboot
sh "$S" boot > /dev/null
: > "$T/spawned"
out=$(sh "$S" reapply)
t "reapply: started, nothing written yet" "event=reapply_started" "$out"
[ ! -f "$T/sdsset" ] && ok "reapply: the answer leaves first" || no "reapply: the answer leaves first"
t "reapply: reapply-run spawned" "^reapply-run$" "$(spawned)"
out=$(sh "$S" try 2500basex 2>&1)
t "a trial is refused while a reapply runs" "reapply is running" "$out"
sed -i.b 's/^0x214=.*/0x214=0x260002/; s/^0x215b4=.*/0x215b4=0x1564/; s/^0x21c00=.*/0x21c00=0x1940/' "$T/regs"
sh "$S" reapply-run > /dev/null 2>&1
same "reapply in Fiber 1G: the thirteen writes" "$T/want-fiber1g-from-sgmii"
t "reapply: reads back, steady, the key untouched" "^reapplied steady fiber1g $" "$(st event) $(st phase) $(st running) $(key)"
t "reapply: host_forced left alone" "^N$" "$(p host_forced)"

# ---- status
out=$(sh "$S" status)
t "status: the fields" "setting=fiber1g.*running=fiber1g.*phase=steady.*fallback=0.*uptime=[0-9]+.*autoneg=on.*forced=0.*link=[01].*link_source=host_link.*event=.*safe_mode_boot=0" "$(printf '%s' "$out" | tr '\n' ' ')"
rm -f "$STATE"
t "status with no state: running unknown, the key read" "setting=fiber1g.*running=unknown" "$(sh "$S" status | tr '\n' ' ')"

# ---- the lock
sh "$S" boot > /dev/null
mkdir -p "$LOCKDIR"; echo 999999 > "$LOCKDIR/pid"
out=$(sh "$S" try 2500basex 2>&1)
t "a stale lock is taken over" "trial_started" "$out"
sh "$S" revert > /dev/null 2>&1
mkdir -p "$LOCKDIR"; echo $$ > "$LOCKDIR/pid"
out=$(sh "$S" try 2500basex 2>&1)
t "a lock held by a live process makes the caller give up, bounded" "busy" "$out"
rm -rf "$LOCKDIR"
out=$(LOCKDIR=$T/no/such/dir/lock sh "$S" boot 2>&1)
t "boot with no place for the lock still applies, and says so" "no lock.*SerDes already Fiber 1G" "$(printf '%s' "$out" | tr '\n' ' ')"

echo
echo "host_serdes_test: $pass passed, $fail failed"
[ "$fail" = 0 ]
