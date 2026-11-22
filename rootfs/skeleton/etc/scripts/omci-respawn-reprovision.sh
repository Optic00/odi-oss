#!/bin/sh
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

# omci-respawn-reprovision.sh -- run in the background by svc-omcid.sh
# whenever init has just respawned omcid (not the first start of this
# boot). See docs/BOOT.md, "Respawn re-provisioning" and "Resume without
# re-registration": omcid respawns and serves omcicli again within seconds
# of a kill; whether the OLT has to be forced through a fresh
# identification cycle to notice depends on whether the respawned omcid
# resumed its MIB from the snapshot it kept in /var/run, or came up empty.
#
# The respawned omcid itself decides that (docs/BOOT.md), and says so by
# writing $RESUME_DECISION before doing anything else that could block:
# "resumed" (it loaded a valid snapshot while the PON was still O5 and
# needs nothing from this script -- the datapath was never touched) or
# "reprovision" (no snapshot, a mismatched device, not O5, or a MIB the OLT
# has since reset). This script waits, briefly, for that decision, then
# either exits or forces the OLT to re-range and re-provision -- deactivate,
# wait for the (already respawned) omcid to register, re-apply the PLOAM
# password, hold, reactivate (rcs-lib.sh's omci_reactivate) --
# without touching the omcid process itself: init is already supervising
# it as a respawn entry, so killing or starting one from here would only
# trigger another respawn, and another run of this script.
set -u

RCS_LIB=${RCS_LIB:-$(dirname "$0")/rcs-lib.sh}
# shellcheck source=./rcs-lib.sh
. "$RCS_LIB"

ODI_INIT=${ODI_INIT:-/proc/odi_init}
REGISTER_WAIT=${REGISTER_WAIT:-10}  # the respawned omcid registers for redirect type 1
DEACT_HOLD=${DEACT_HOLD:-3}         # deactivated at least this long, as measured
RESUME_DECISION=${RESUME_DECISION:-/var/run/omcid-resume-decision}
DECISION_WAIT=${DECISION_WAIT:-5}   # the decision is written before anything
                                     # in main() that could block, so this is
                                     # a ceiling, not a normal wait

# Poll, bounded: the respawned omcid writes $RESUME_DECISION (temp file then
# rename, so a reader never sees a half-written one) within a few syscalls of
# starting. No file within DECISION_WAIT seconds -- omcid did not get that
# far, crashed, or is some older build with no such file -- is read the same
# as "reprovision": the safe, proven fallback. A plain iteration count, not
# uptime_s() (a static fixture under test, so a delta against it never
# advances).
decision=reprovision
w=0
while [ "$w" -lt "$DECISION_WAIT" ]; do
	if [ -s "$RESUME_DECISION" ]; then
		decision=$(cat "$RESUME_DECISION" 2>/dev/null || echo reprovision)
		break
	fi
	sleep 1
	w=$((w + 1))
done

if [ "$decision" = "resumed" ]; then
	crumb "omci respawn: resumed from snapshot, no re-provisioning needed"
	exit 0
fi

# No PON verbs on this kernel (test/qemu/run-qemu.sh's stock kernel, or a
# dev image with modules.off) -- nothing to reprovision.
[ -w "$ODI_INIT" ] || exit 0

# No serial in the store: the boot never activated the ONU (pon_step_gponact,
# rcs-lib.sh), and a respawn must not either.
if [ -z "$(store_sn)" ]; then
	crumb "omci respawn: no GPON_SN in the store, the ONU stays deactivated"
	exit 0
fi

t0=$(uptime_s)
if ! write_proc_bounded "$ODI_INIT" gpondeact 2>/dev/null; then
	crumb "omci respawn: gpondeact failed, leaving the ONU as it is"
	exit 1
fi
crumb "omci respawn: ONU deactivated, waiting for the respawned omcid to register"

new=$(omci_reactivate "$t0" "$REGISTER_WAIT" "$DEACT_HOLD") || {
	crumb "omci respawn: gponact failed, the ONU stays off the line until a gponact succeeds (a reboot, the next respawn, or by hand)"
	exit 1
}
if [ -n "$new" ]; then
	crumb "omci respawn: omcid $new registered, ONU reactivated"
else
	crumb "omci respawn: omcid did not register within ${REGISTER_WAIT}s, activated anyway"
fi
