#!/bin/sh
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

#
# host-serdes.sh -- the SerDes mode of the host port (the SFP edge, switch
# port 0, the only way into the stick), from the odi-only key HOST_SERDES:
#
#     fiber1g, empty or absent  1000BASE-X ("Fiber 1G"), the default and
#                               the mode the stock DFP-34X-2C2 image runs;
#                               its auto-negotiation is HOST_AUTONEG
#     2500basex                 2500BASE-X, for a 2.5G host port; the PCS
#                               auto-negotiation off, the port forced
#     hisgmii                   HiSGMII (3.125 Gbaud), the MAC forced to
#                               1000 Mbit/s; the 1000BASE-X PCS powered
#                               down, the port forced
#
# Any other value runs fiber1g, and says so. There is no board signal that
# tells one stick from another (the stock firmware of the HSGQ XPON stick
# runs 2500basex or hisgmii), so the key is the only input: the stock
# LAN_SDS_MODE is not read, since a stick that runs Fiber 1G can hold any
# value there. Fiber 1G is the fallback everywhere below: it is the most
# compatible host mode.
#
# The mode is a register sequence, run through diag in the order below: the
# mode parked, the analog block, the mode selected (docs/SWITCH.md, "Host
# port SerDes mode"). None of it survives a reset, so every boot applies it
# from scratch, and a wrong mode costs the host link until the next boot.
# The kernel does the port side (odi_switch.host_forced) and the PCS
# auto-negotiation bit (odi_switch.host_autoneg); this script decides what
# to write and when, and keeps the stick reachable when the choice is
# wrong. Nothing here persists a mode that has not been seen to work:
#
#     host-serdes.sh boot          network.sh, once, before the MAC and the
#                                  bridge: the mode of HOST_SERDES, and the
#                                  boot watch for 2500basex (below)
#     host-serdes.sh try MODE [SECONDS]
#                                  a trial: one second later the mode is
#                                  applied (so this answer leaves first), in
#                                  RAM only, and it reverts by itself after
#                                  SECONDS (default 120) unless confirmed
#     host-serdes.sh confirm       keep the trial mode: stores HOST_SERDES.
#                                  Only reaches the stick if the link works
#                                  in that mode, which is the point
#     host-serdes.sh revert        end the trial now, back to the mode before
#     host-serdes.sh reapply       run the sequence of the running mode
#                                  again, one second later; no trial, nothing
#                                  stored
#     host-serdes.sh status        the state as key=value lines
#
# The link each mode can see. host_link (odi_switch) is the port link, and
# a forced port reads it as up whatever the host does, so it means something
# in fiber1g only. In 2500basex bit 17 of HOST_SERDES_MODE (0x214) is set
# while the SerDes is in sync with the host and clear without it. HiSGMII
# has no signal known. So:
#     trial revert     every mode: the confirm has to arrive over the link
#     boot watch       2500basex only. A boot in it watches the sync for
#                      WATCH_S seconds; none: Fiber 1G for WATCH_S, watching
#                      host_link (the fallback, logged and shown in the web
#                      UI), then 2500basex again, and so on until one links,
#                      for at most WATCH_MAX_S; past that it stays on Fiber
#                      1G. The watch never changes the key.
#     safe mode        safe-mode.sh: three short boots in a row with a mode
#                      other than fiber1g make the next boot a recovery
#                      boot, which runs fiber1g and removes the key, as it
#                      removes HOST_AUTONEG=off; there this script refuses a
#                      trial.
#
# The PCS auto-negotiation. In 2500basex it is off (the only state seen to
# link to the 2.5G router port it was measured on), in hisgmii on (the stock HSGQ value, on a PCS that is
# powered down), whatever HOST_AUTONEG says: host-autoneg.sh applies nothing
# and refuses a trial while the port runs one of them, or while a trial or
# the boot watch here runs. Back in fiber1g the bit goes back to what
# host-autoneg.sh last ran (its state file; on when it has none).
#
# Every wait is bounded (diag, sysfs, flash, the lock) and every state
# change is one rename of the state file, under the host-port lock it
# shares with host-autoneg.sh, so the two never change the port at once.
set -u

# confd starts its children with no environment; export a PATH so that
# commands started through busybox timeout find /sbin and /usr/sbin too.
PATH=${PATH:+$PATH:}/sbin:/usr/sbin:/bin:/usr/bin
export PATH

FLASH=${FLASH:-/etc/scripts/flash}
SELF=${SELF:-/etc/scripts/host-serdes.sh}
DIAG=${DIAG:-/bin/diag}
PARAM_DIR=${PARAM_DIR:-/sys/module/odi_switch/parameters}
STATE=${STATE:-/var/run/host-serdes}
AN_STATE=${AN_STATE:-/var/run/host-autoneg}
LOCKDIR=${LOCKDIR:-/var/run/host-autoneg.lock}
SAFE_FLAG=${SAFE_FLAG:-/var/run/safe-mode}
SDS_OFF=${SDS_OFF:-/etc/config/sds.off}
UPTIME=${UPTIME:-/proc/uptime}
KMSG=${KMSG:-/dev/kmsg}
LOG=${LOG:-/var/log/services.log}
SLEEP=${SLEEP:-sleep}
TIMEOUT=${TIMEOUT:-timeout}

TRIAL_S=${TRIAL_S:-120}         # a trial reverts after this long unconfirmed
TRIAL_MAX_S=${TRIAL_MAX_S:-600} # the longest trial a caller may ask for
WATCH_S=${WATCH_S:-30}          # each phase of the boot watch
WATCH_MAX_S=${WATCH_MAX_S:-600} # the whole boot watch
RESYNC_S=${RESYNC_S:-5}         # re-latch a lane out of sync this often (2500BASE-X, and HiSGMII in a trial)
DIAG_S=${DIAG_S:-10}            # one diag call
IO_S=${IO_S:-5}                 # one sysfs or flash call
LOCK_S=${LOCK_S:-10}            # waiting for the lock

say() { echo "host-serdes: $*"; }
die() { echo "host-serdes: $*" >&2; exit 1; }

# The lock, the state file, the event line and the trial run
# (trial-lib.sh). The lock is the host-port lock, shared with
# host-autoneg.sh (the directory keeps the name it had before this script).
TAG=host-serdes
LOCK_OWNER="host-port change"
LOCK_STRICT=1
STATE_KEYS="setting running phase fallback trial_mode trial_prev trial_id
	trial_deadline watch_until event:ev event_uptime:ev_up message:ev_msg"
# Next to this script; ${0%/*} rather than dirname, which would be one more
# process on the boot path (and in the rcS trace goldens).
case $0 in */*) TRIAL_LIB=${TRIAL_LIB:-${0%/*}/trial-lib.sh} ;; *) TRIAL_LIB=${TRIAL_LIB:-./trial-lib.sh} ;; esac
# shellcheck source=rootfs/skeleton/etc/scripts/trial-lib.sh
. "$TRIAL_LIB"

event_fields() { echo "setting=$setting running=$running"; }

# ---- the key, the modes

# parse VALUE: the mode HOST_SERDES means; anything unknown is fiber1g.
parse() {
	case $1 in
	2500basex) echo 2500basex ;;
	hisgmii)   echo hisgmii ;;
	*)         echo fiber1g ;;
	esac
}

valid() {
	case $1 in
	''|fiber1g|2500basex|hisgmii) return 0 ;;
	esac
	return 1
}

label() {
	case $1 in
	fiber1g)   echo "Fiber 1G" ;;
	2500basex) echo "2500BASE-X" ;;
	hisgmii)   echo "HiSGMII (3.125 Gbaud)" ;;
	*)         echo "$1" ;;
	esac
}

key_raw() {
	"$TIMEOUT" "$IO_S" "$FLASH" get HOST_SERDES 2>/dev/null | sed -n 's/^HOST_SERDES=//p' | head -n 1
}

# ---- the registers, through diag
#
# diag is INTERACTIVE and is fed on stdin, never as bare argv. Ours exits on
# EOF; the timeout is belt and braces against a hang that would stall the
# boot. Register and field names are ours (src/diag/tools/regnames.txt);
# the analog words are not in the register table, so they go by address.
sds_read() {
	printf "register get %s 1\n" "$1" | "$TIMEOUT" "$DIAG_S" "$DIAG" 2>/dev/null |
		sed -n "s/^0x[0-9a-fA-F]* *0x\([0-9a-fA-F]*\).*/\1/p" | head -n 1
}
# sds_put ADDR VALUE: one write; a failed one is logged and returns 1.
sds_put() {
	printf "register set %s %s\n" "$1" "$2" | "$TIMEOUT" "$DIAG_S" "$DIAG" >/dev/null 2>&1 && return 0
	echo "host-serdes: SerDes write $1 failed" >&2
	return 1
}
# sds_write ADDR VALUE: the same, for the sequences, which go on past a
# failed write and are judged by what reads back (sds_check).
sds_write() { sds_put "$1" "$2" || :; }
# read-modify-write: clear the bits in $2, set the bits in $3
sds_rmw() {
	cur=$(sds_read "$1")
	[ -n "$cur" ] || { echo "host-serdes: SerDes read $1 failed mid-sequence" >&2; return 1; }
	sds_write "$1" "$(( (0x$cur & ~$2) | $3 ))"
}

# mode_vars MODE: the words that differ between the modes. Every other
# word of the sequence is the same in all three.
#
#   SDS_MODE  HOST_SERDES_MODE 0x214, under 0xa11f: PCIE_SELECT bit15,
#             HOLD_CLK_GLITCH bit13, PHY_SIDE_SEL bit8, LINK_MODE [4:0].
#             Fiber 1G 0x4; 2500BASE-X 0x16; HiSGMII 0x12 with
#             PHY_SIDE_SEL, 0x112
#   SDS_FIB   HOST_FIBER_CTRL 0x21c00 POWER_DOWN bit11: 0, or 1 in HiSGMII
#             (AUTONEG, bit12, is set after the sequence, below)
#   SDS_TX    HOST_FIBER_TX 0x22e4c TX_MODE [15:14]: 0 in Fiber 1G, 1 else
#   SDS_ANA   0x215b4, the TX amplitude word: 0x1264, 0x1764, 0x1564
#   SDS_ANF   SDS_AN_CTRL 0x21808 bit 8, in-band negotiation forced off on
#             the host lane: 0x100 in HiSGMII, 0 else (sds_anf, below)
mode_vars() {
	case $1 in
	2500basex) SDS_MODE=0x16  SDS_FIB=0x0   SDS_TX=0x4000 SDS_ANA=0x1764 SDS_ANF=0x0 ;;
	hisgmii)   SDS_MODE=0x112 SDS_FIB=0x800 SDS_TX=0x4000 SDS_ANA=0x1564 SDS_ANF=0x100 ;;
	*)         SDS_MODE=0x4   SDS_FIB=0x0   SDS_TX=0x0    SDS_ANA=0x1264 SDS_ANF=0x0 ;;
	esac
}

# sds_target MODE: addr mask want, one per line: the registers hold want
# under mask once the mode is set. Every target was read back from a stick
# running that mode. The two 3.125 Gbaud modes, 2500BASE-X and HiSGMII,
# also program the 3.125G analog page (0x21700, 0x2170c, 0x21718) before
# the mode is selected, and end with a receive pulse (0x21500 0x10, 0x30,
# 0). The pulse is a sequence, not a state: the word reads 0 after it, and
# is not compared.
#
# SDS_AN_CTRL 0x21808 bit 8, in-band negotiation forced off on the host
# lane (0x70e0 out of reset, bit 8 clear): set in HiSGMII, as the stock
# HSGQ kernel does in its LAN SerDes mode 4 (0x70e0 -> 0x71e0). Newer hosts
# passed frames only with it, the 3.125G page and the receive pulse
# together; which of the three they need is not isolated
# (docs/QUALIFICATION.md). Clear in Fiber 1G and 2500BASE-X,
# so a change out of HiSGMII (a trial, its revert, the boot watch,
# reapply) does not leave it set. Compared under 0x100 in all three modes.
sds_target() {
	mode_vars "$1"
	cat <<EOF
0x214   0xa11f      $SDS_MODE
0x110   0x1         0x1
0x21c00 0x800       $SDS_FIB
0x22e4c 0xc000      $SDS_TX
0x21680 0xffffffff  0x0
0x2168c 0xffffffff  0x3001
0x21698 0xffffffff  0x68
0x215b8 0xffffffff  0x98c5
0x215b4 0xffffffff  $SDS_ANA
0x215bc 0xffffffff  0x400f
0x215e4 0xffffffff  0xfc00
0x21808 0x100       $SDS_ANF
EOF
	case $1 in 2500basex|hisgmii)
		cat <<EOF
0x21700 0xffffffff  0x1800
0x2170c 0xffffffff  0x7b01
0x21718 0xffffffff  0x168
EOF
	esac
}

# sds_check MODE: prints "ok", "differs: <list>" or "unreadable"
sds_check() {
	diff=""
	for line in $(sds_target "$1" | tr -s ' ' ',' | grep .); do
		a=${line%%,*}; rest=${line#*,}; m=${rest%%,*}; w=${rest#*,}
		cur=$(sds_read "$a")
		[ -n "$cur" ] || { echo unreadable; return; }
		[ $(( 0x$cur & m )) -eq $(( w )) ] || diff="$diff $a=0x$cur"
	done
	if [ -z "$diff" ]; then echo ok; else echo "differs:$diff"; fi
}

# sds_seq MODE: the sequence, whole and IN ORDER: the order is the mode-set
# procedure (park the mode at 0x1f, program the analog block, then select
# the mode), not a list of independent fixes. For Fiber 1G it is the
# thirteen writes of the stock mode set, value for value.
sds_seq() {
	mode_vars "$1"
	sds_rmw 0x214 0x0 0x2000 &&
	sds_rmw 0x214 0x801f 0x1f &&
	sds_rmw 0x21c00 0x800 "$SDS_FIB" &&
	sds_rmw 0x22e4c 0xc000 "$SDS_TX" &&
	sds_write 0x21680 0x0 &&
	sds_write 0x2168c 0x3001 &&
	sds_write 0x21698 0x68 &&
	sds_write 0x215b8 0x98c5 &&
	sds_write 0x215b4 "$SDS_ANA" &&
	sds_write 0x215bc 0x400f &&
	sds_write 0x215e4 0xfc00 &&
	case $1 in 2500basex|hisgmii)
		sds_write 0x21700 0x1800 && sds_write 0x2170c 0x7b01 && sds_write 0x21718 0x168
	esac &&
	sds_rmw 0x214 0xa11f "$SDS_MODE" &&
	sds_rmw 0x110 0x0 0x1 &&
	case $1 in 2500basex|hisgmii)
		sds_write 0x21500 0x10 && sds_write 0x21500 0x30 && sds_write 0x21500 0x0
	esac &&
	sds_anf "$SDS_ANF"
}

# sds_anf WANT: 0x21808 bit 8 := WANT (0x100 or 0), read-modify-write. Set:
# always written. Clear: written only when the bit reads set, so the Fiber
# 1G sequence from a stick that never ran HiSGMII stays the stock one,
# write for write.
sds_anf() {
	if [ "$1" != 0x0 ]; then
		sds_rmw 0x21808 0x100 "$1"
		return
	fi
	cur=$(sds_read 0x21808)
	[ -n "$cur" ] || { echo "host-serdes: SerDes read 0x21808 failed mid-sequence" >&2; return 1; }
	[ $(( 0x$cur & 0x100 )) -eq 0 ] || sds_write 0x21808 "$(( 0x$cur & ~0x100 ))"
}

# sds_relatch MODE: the lane parked and started again in MODE, 2500basex
# or hisgmii: HOLD_CLK_GLITCH set, LINK_MODE parked (0x1f), 20 ms, then the
# mode word (SDS_MODE under 0xa11f, the other bits as read) with the clock
# running. On a lane in MODE that is the word it held. It is not the word
# read back: a re-latch cut before its restore (a diag timeout, a failed
# write) leaves the lane parked, and the next one starts it again. A lane
# started before the host sends 3.125 Gbaud does not sync when the host
# starts later (a host port switched to 2.5G by hand after the stick, as a
# trial asks), in 2500BASE-X and in HiSGMII alike; started again while the
# host sends, it syncs within seconds. Run only on a lane in MODE whose bit
# 17 reads clear: a lane in sync is never touched. Returns 1 when the read
# or any write fails; the restore is written even after a failed park.
sds_relatch() {
	mode_vars "$1"
	cur=$(sds_read 0x214)
	[ -n "$cur" ] || return 1
	rl=0
	sds_put 0x214 "$(printf '0x%x' $(( 0x$cur | 0x2000 )))" || rl=1
	sds_put 0x214 "$(printf '0x%x' $(( ((0x$cur | 0x2000) & ~0x801f) | 0x1f )))" || rl=1
	usleep 20000 2>/dev/null || "$SLEEP" 1
	sds_put 0x214 "$(printf '0x%x' $(( (0x$cur & ~0xa11f) | $SDS_MODE )))" || rl=1
	return "$rl"
}

# apply_boot MODE: what the boot does. Every register is read first; if all
# of them already hold MODE (the normal Fiber 1G case after the stock image
# or U-Boot configured it), nothing is written. Otherwise the sequence is
# applied whole. Returns 0 when the registers read back as MODE, 1 when
# they do not after the sequence, 2 when they could not be read (and
# nothing was written).
apply_boot() {
	l=$(label "$1")
	state=$(sds_check "$1")
	case $state in
	ok)
		if [ "$1" = fiber1g ]; then say "SerDes already Fiber 1G (option 1)"
		else say "SerDes already $l"; fi
		return 0
		;;
	unreadable)
		echo "host-serdes: could not read the SerDes registers, leaving them alone" >&2
		return 2
		;;
	esac
	if [ "$1" = fiber1g ]; then
		echo "host-serdes: SerDes not in Fiber 1G (${state#differs: }), applying lan_sds_mode_set(1)" >&2
	else
		echo "host-serdes: SerDes not in $l (${state#differs: }), applying its sequence" >&2
	fi
	sds_seq "$1"
	after=$(sds_check "$1")
	if [ "$after" = ok ]; then
		say "SerDes now $l"
		return 0
	fi
	echo "host-serdes: SerDes still not $l after the sequence ($after)" >&2
	return 1
}

# ---- the kernel parameters

param_ok() { [ -w "$PARAM_DIR/host_forced" ] && [ -w "$PARAM_DIR/host_autoneg" ]; }

# param_write NAME 0|1
param_write() {
	# shellcheck disable=SC2016 # the inner shell expands them
	"$TIMEOUT" "$IO_S" sh -c 'echo "$1" > "$2"' _ "$2" "$PARAM_DIR/$1" 2>/dev/null
}

param_read() {
	"$TIMEOUT" "$IO_S" cat "$PARAM_DIR/$1" 2>/dev/null | sed -n '1s/^\([YN01]\)$/\1/p'
}

# The host-autoneg.sh state: what it last ran (on/off) and its phase. Read
# with the shell alone, under the shared lock.
an_field() {
	_v=$2
	if [ -f "$AN_STATE" ]; then
		while IFS='=' read -r _k _x; do
			[ "$_k" = "$1" ] && _v=$_x
		done < "$AN_STATE"
	fi
	echo "$_v"
}

# port_for MODE: the port side and the PCS auto-negotiation for MODE, after
# its sequence: the force (on in 2500basex and hisgmii, off in fiber1g),
# then the auto-negotiation bit (off in 2500basex, on in hisgmii, and in
# fiber1g what host-autoneg.sh last ran). Each write is one sysfs write the
# kernel applies at once, and writes nothing when the bit already holds
# the value; host_forced N while already N is not written at all.
port_for() {
	case $1 in
	2500basex) param_write host_forced 1 && param_write host_autoneg 0 ;;
	hisgmii)   param_write host_forced 1 && param_write host_autoneg 1 ;;
	*)
		# Cleared unless it reads as clear: a value that cannot be
		# read is as likely the force a trial left. A kernel without
		# the parameter has nothing to clear.
		if [ -e "$PARAM_DIR/host_forced" ]; then
			case $(param_read host_forced) in
			N|0) ;;
			*) param_write host_forced 0 || return 1 ;;
			esac
		fi
		if [ "$(an_field running on)" = off ]; then param_write host_autoneg 0
		else param_write host_autoneg 1; fi
		;;
	esac
}

# switch_to MODE: a live change (a trial, its revert, the boot watch,
# reapply): the sequence unconditionally, the port side, then the read
# back. Returns 1 when the registers do not read back as MODE or the port
# side could not be written, and says which in unverified.
switch_to() {
	unverified=
	sds_seq "$1"
	port_for "$1" || unverified="port side (host_forced, host_autoneg) could not be written"
	[ "$(sds_check "$1")" = ok ] || unverified="registers did not all read back"
	[ -z "$unverified" ]
}

# sds_sync: bit 17 of 0x214, the lane in sync: 1, 0, or nothing when the
# read failed.
sds_sync() {
	v=$(sds_read 0x214)
	[ -n "$v" ] || return 0
	if [ $(( 0x$v & 0x20000 )) -ne 0 ]; then echo 1; else echo 0; fi
}

# link MODE: 1, 0, or nothing when this mode has no signal (hisgmii) or the
# read failed. In HiSGMII bit 17 reads set with the lane in sync whether or
# not frames pass (without 0x21808 bit 8 they did not), so it is not the
# link of the status; the trial window uses it only to see a lane out of
# sync.
link() {
	case $1 in
	2500basex) sds_sync ;;
	hisgmii) ;;
	*) "$TIMEOUT" "$IO_S" cat "$PARAM_DIR/host_link" 2>/dev/null | sed -n '1s/^\([01]\)$/\1/p' ;;
	esac
}

link_source() {
	case $1 in
	2500basex) echo serdes_sync ;;
	hisgmii)   echo none ;;
	*)         echo host_link ;;
	esac
}

# ---- the state file, /var/run/host-serdes

clear_trial() { trial_mode='' trial_prev='' trial_id='' trial_deadline=''; }

load() {
	setting=fiber1g running=fiber1g phase=steady fallback=0
	clear_trial
	watch_until='' ev='' ev_up='' ev_msg=''
	state_load
}
save() { state_save; }

# ---- boot

boot() {
	locked=0
	if lock_try; then locked=1; else say "no lock ($LOCKDIR); applying anyway, the boot needs the link" >&2; fi
	load
	raw=$(key_raw)
	setting=$(parse "$raw")
	running=fiber1g phase=steady fallback=0
	clear_trial
	watch_until=''
	ev=boot ev_up=$(uptime_s) ev_msg=""
	if ! valid "$raw"; then
		event bad_value "HOST_SERDES=$raw is not fiber1g, 2500basex or hisgmii; running Fiber 1G"
	fi
	if [ -f "$SDS_OFF" ]; then
		running=unmanaged
		event sds_off "$SDS_OFF: nothing written to the host port, HOST_SERDES not applied"
		boot_end
		return 0
	fi
	if [ ! -x "$DIAG" ]; then
		running=unmanaged
		event no_diag "no $DIAG: nothing written to the host port"
		boot_end
		return 0
	fi
	mode=$setting
	if [ -f "$SAFE_FLAG" ]; then
		mode=fiber1g
		event safe_mode_boot "a safe-mode boot: Fiber 1G, nothing applied, trials refused (HOST_SERDES=${raw:-unset})"
	elif [ "$mode" != fiber1g ] && ! param_ok; then
		mode=fiber1g
		event no_kernel "this kernel has no odi_switch.host_forced; running Fiber 1G"
	fi
	apply_boot "$mode"
	rc=$?
	if [ "$rc" = 2 ]; then
		running=unknown
		[ "$mode" = fiber1g ] || event apply_failed "could not read the registers; nothing written, $(label "$mode") not applied"
		boot_end
		return 0
	fi
	running=$mode
	if [ "$mode" = fiber1g ]; then
		# The default is not an event: the port side and the
		# auto-negotiation are left to the kernel and host-autoneg.sh.
		[ "$ev" != boot ] || ev_msg="Fiber 1G"
		boot_end
		return 0
	fi
	port_for "$mode" || event apply_failed "could not write odi_switch.host_forced or host_autoneg for $(label "$mode")"
	[ "$rc" = 0 ] || event apply_unverified "the $(label "$mode") registers did not all read back; carrying on in that mode"
	if [ "$mode" = 2500basex ]; then
		phase=watch
		watch_until=$(( $(uptime_s) + WATCH_MAX_S ))
		event applied "2500BASE-X, port forced, auto-negotiation off; watching the SerDes sync for ${WATCH_S} s"
		boot_end
		spawn watch
	else
		event applied "HiSGMII (3.125 Gbaud), port forced; this mode has no link signal, so no boot watch"
		boot_end
	fi
	return 0
}

boot_end() {
	save
	[ "$locked" = 0 ] || unlock
}

# watch: the boot watch of 2500basex (above). Each phase polls the link of
# the mode it runs once a second.
watch() {
	t0=$(uptime_s)
	lock
	load
	unlock
	[ "$phase" = watch ] || return 0
	mode=$running
	while :; do
		i=0
		while :; do
			if [ "$(link "$mode")" = 1 ]; then
				lock
				load
				if [ "$phase" != watch ]; then
					unlock
					return 0
				fi
				phase=steady watch_until=
				if [ "$mode" = "$setting" ]; then
					fallback=0
					event serdes_link "the host SerDes is in sync in $(label "$mode") after $(( $(uptime_s) - t0 )) s"
				else
					fallback=1
					event fallback "no sync in $(label "$setting"); the host linked in $(label "$mode") after $(( $(uptime_s) - t0 )) s. HOST_SERDES stays $setting"
				fi
				save
				unlock
				return 0
			fi
			[ "$i" -lt "$WATCH_S" ] || break
			"$SLEEP" 1
			i=$((i + 1))
			# A 2.5G host that started to send after this phase started
			# the lane: start it again, every RESYNC_S s, while out of sync.
			# lock_try counts with i as well, so i is kept aside around it:
			# reset, it would restart the phase at every re-latch and
			# Fiber 1G would never be tried.
			if [ "$mode" = 2500basex ] && [ "$i" -lt "$WATCH_S" ] &&
				[ $(( i % RESYNC_S )) -eq 0 ] && [ "$(link 2500basex)" = 0 ]; then
				w_i=$i
				lock_try
				w_rc=$?
				i=$w_i
				if [ "$w_rc" != 1 ]; then
					load
					if [ "$phase" != watch ]; then
						unlock
						return 0
					fi
					sds_relatch 2500basex || say "the lane could not be re-latched" >&2
					unlock
				fi
			fi
		done
		lock
		load
		if [ "$phase" != watch ]; then
			unlock
			return 0
		fi
		if [ $(( $(uptime_s) - t0 )) -ge "$WATCH_MAX_S" ]; then
			[ "$running" = fiber1g ] || switch_to fiber1g || true
			running=fiber1g fallback=1 phase=steady watch_until=
			event fallback_nolink "no host link in either mode for $(( $(uptime_s) - t0 )) s; staying on Fiber 1G. HOST_SERDES stays $setting"
			save
			unlock
			return 0
		fi
		if [ "$mode" = "$setting" ]; then mode=fiber1g; else mode=$setting; fi
		switch_to "$mode" || say "$(label "$mode"): its $unverified" >&2
		running=$mode
		if [ "$mode" = "$setting" ]; then fallback=0; else fallback=1; fi
		event watch_switch "no host link for ${WATCH_S} s; trying $(label "$mode")"
		save
		unlock
	done
}

# ---- the trial

# refuse_busy: exit when the port is not ours to change now. Called under
# the lock, with the state loaded.
refuse_busy() {
	case $phase in
	watch) unlock; die "the boot watch is still deciding the mode (until uptime $watch_until s); try again after it" ;;
	trial) unlock; die "a trial of $trial_mode is already running; confirm or revert it first" ;;
	reapply) unlock; die "a reapply is running; try again in a few seconds" ;;
	esac
	case $running in
	unmanaged|unknown) unlock; die "this boot does not manage the host SerDes (running=$running, event=$ev)" ;;
	esac
	case $(an_field phase steady) in
	watch|trial) unlock; die "host-autoneg.sh has a trial or its boot watch running; try again after it" ;;
	esac
}

# precheck MODE: what a live change needs, before the lock.
precheck() {
	[ ! -f "$SDS_OFF" ] || die "$SDS_OFF is set: the host SerDes is left alone"
	[ -x "$DIAG" ] || die "no $DIAG"
	[ "$1" = fiber1g ] || param_ok || die "this kernel has no odi_switch.host_forced"
	[ ! -f "$SAFE_FLAG" ] || die "this is a safe-mode boot: it runs Fiber 1G; reboot to the saved configuration first"
}

try() {
	mode=${1:-} secs=${2:-$TRIAL_S}
	case $mode in fiber1g|2500basex|hisgmii) ;; *) die "usage: host-serdes.sh try fiber1g|2500basex|hisgmii [SECONDS]" ;; esac
	case $secs in ''|*[!0-9]*) die "SECONDS must be a number" ;; esac
	[ "$secs" -ge 10 ] && [ "$secs" -le "$TRIAL_MAX_S" ] || die "SECONDS must be 10 to $TRIAL_MAX_S"
	precheck "$mode"
	lock
	load
	refuse_busy
	if [ "$mode" = "$running" ] && [ "$mode" = "$setting" ]; then
		unlock
		say "already $mode, nothing to try (reapply runs its sequence again)"
		return 0
	fi
	up=$(uptime_s)
	trial_id="$up.$$" trial_mode=$mode trial_prev=$running
	trial_deadline=$(( up + 1 + secs ))
	phase=trial
	event trial_started "trying $(label "$mode") for $secs s: confirm (host-serdes.sh confirm) or it reverts to $(label "$trial_prev")"
	save
	unlock
	spawn trial-run "$trial_id" "$secs"
	return 0
}

# The trial run (trial_run, trial-lib.sh): the switch, then the window.
trial_apply() { switch_to "$trial_mode"; }
# trial_window ID SECONDS: in 2500BASE-X and HiSGMII the window re-latches
# the lane every RESYNC_S s while bit 17 reads clear (sds_relatch, above):
# the host port is often switched to 2.5G by hand after the trial applied,
# over the link the trial just dropped. Fiber 1G sleeps the window as
# before. The window keeps its length; the expiry follows it as before.
trial_window() {
	case $trial_mode in
	2500basex|hisgmii) ;;
	*) "$SLEEP" "$2"; return 0 ;;
	esac
	w_id=$1 w_end=$(( $(uptime_s) + $2 )) w_n=0
	while :; do
		w_left=$(( w_end - $(uptime_s) ))
		[ "$w_left" -gt 0 ] || return 0
		[ "$w_left" -le "$RESYNC_S" ] || w_left=$RESYNC_S
		"$SLEEP" "$w_left"
		# Confirmed or reverted: the trial is over, so is the window.
		load
		if [ "$phase" != trial ] || [ "$trial_id" != "$w_id" ]; then
			return 0
		fi
		[ "$(sds_sync)" = 0 ] || continue
		# lock_try, not lock: a busy lock skips this re-latch; lock would
		# end the trial run here, before its expiry.
		lock_try
		[ $? != 1 ] || continue
		load
		if [ "$phase" != trial ] || [ "$trial_id" != "$w_id" ]; then
			unlock
			return 0
		fi
		if [ "$running" != "$trial_mode" ]; then
			unlock
			continue
		fi
		sds_relatch "$trial_mode" || say "the lane could not be re-latched" >&2
		unlock
		w_n=$((w_n + 1))
		[ "$w_n" != 1 ] || say "no sync in $(label "$trial_mode"): re-latching the lane every ${RESYNC_S} s until it syncs or the trial ends"
	done
}
trial_applied() {
	running=$trial_mode
	# expire and revert go by the file. If it cannot be written it still
	# says the mode before, and they would leave the port in the trial
	# mode: back at once. The expiry gets a second go at the file.
	save || back_to_prev trial_failed "could not write $STATE"
}
trial_apply_failed() {
	running=$trial_mode
	back_to_prev trial_failed "$(label "$trial_mode"): its ${unverified:-registers did not all read back}"
}

# back_to_prev NAME WHY: the mode before the trial, live. A sequence that
# does not read back, or a port side that cannot be written, is not "back
# to": the event is revert_unverified, as apply_unverified at boot, the port
# is taken to run that mode (reapply runs it again), and the status is 1. So
# it is when the state file cannot be written: the trial would still be
# there to confirm.
back_to_prev() {
	name=$1 why=$2 rc=0
	if [ "$running" != "$trial_prev" ] && ! switch_to "$trial_prev"; then
		name=revert_unverified rc=1
	fi
	running=$trial_prev
	phase=steady
	if [ "$rc" = 0 ]; then
		event "$name" "$why: back to $(label "$running"); HOST_SERDES unchanged ($setting)"
	else
		event "$name" "$why: back to $(label "$running"), but its $unverified (host-serdes.sh reapply runs the sequence again); HOST_SERDES unchanged ($setting)"
	fi
	clear_trial
	save || rc=1
	return "$rc"
}

store() {
	v=$1
	"$TIMEOUT" "$IO_S" "$FLASH" set HOST_SERDES "$v" > /dev/null 2>&1
	[ "$(key_raw)" = "$v" ]
}

confirm() {
	lock
	load
	if [ "$phase" != trial ]; then
		unlock
		die "no trial is running"
	fi
	if [ "$running" != "$trial_mode" ]; then
		unlock
		die "the trial mode is not applied yet; confirm again in a few seconds"
	fi
	if [ "$trial_mode" = fiber1g ]; then v=""; else v=$trial_mode; fi
	if ! store "$v"; then
		unlock
		die "could not store HOST_SERDES=$v; the trial carries on and reverts unless confirmed"
	fi
	setting=$trial_mode fallback=0 phase=steady
	event trial_confirmed "$(label "$setting") confirmed and stored"
	clear_trial
	save
	unlock
	return 0
}

# reapply: the sequence of the running mode again, live, one second later
# (so this answer leaves first). No trial and nothing stored: it is the mode
# the port runs already, and a reboot applies the same one.
reapply() {
	[ ! -f "$SDS_OFF" ] || die "$SDS_OFF is set: the host SerDes is left alone"
	[ -x "$DIAG" ] || die "no $DIAG"
	lock
	load
	refuse_busy
	if [ "$running" != fiber1g ] && ! param_ok; then
		unlock
		die "this kernel has no odi_switch.host_forced"
	fi
	phase=reapply
	event reapply_started "running the $(label "$running") sequence again in a second"
	save
	unlock
	spawn reapply-run
	return 0
}

reapply_run() {
	"$SLEEP" 1
	lock
	load
	if [ "$phase" != reapply ]; then
		unlock
		return 0
	fi
	phase=steady
	if switch_to "$running"; then
		event reapplied "the $(label "$running") sequence ran again and reads back"
	else
		event reapply_failed "the $(label "$running") sequence ran again, but its $unverified"
	fi
	save
	unlock
}

status() {
	load
	if [ ! -f "$STATE" ]; then
		setting=$(parse "$(key_raw)")
		running=unknown
	fi
	echo "setting=$setting"
	echo "running=$running"
	echo "phase=$phase"
	echo "fallback=$fallback"
	echo "trial_mode=$trial_mode"
	echo "trial_deadline=$trial_deadline"
	echo "watch_until=$watch_until"
	echo "uptime=$(uptime_s)"
	case $(param_read host_autoneg) in Y|1) echo "autoneg=on" ;; N|0) echo "autoneg=off" ;; *) echo "autoneg=unknown" ;; esac
	case $(param_read host_forced) in Y|1) echo "forced=1" ;; N|0) echo "forced=0" ;; *) echo "forced=unknown" ;; esac
	m=$running
	case $m in fiber1g|2500basex|hisgmii) ;; *) m=fiber1g ;; esac
	l=$(link "$m")
	echo "link=${l:-unknown}"
	echo "link_source=$(link_source "$m")"
	echo "event=$ev"
	echo "event_uptime=$ev_up"
	echo "message=$ev_msg"
	[ -f "$SAFE_FLAG" ] && echo "safe_mode_boot=1" || echo "safe_mode_boot=0"
	return 0
}

case "${1:-}" in
boot)        boot ;;
watch)       watch ;;
try)         shift; try "$@" ;;
trial-run)   shift; trial_run "$@" ;;
expire)      shift; expire "$@" ;;
confirm)     confirm ;;
revert)      revert ;;
reapply)     reapply ;;
reapply-run) reapply_run ;;
status)      status ;;
parse)       parse "${2:-}" ;;
*)           echo "usage: $0 boot|try fiber1g|2500basex|hisgmii [SECONDS]|confirm|revert|reapply|status" >&2; exit 1 ;;
esac
