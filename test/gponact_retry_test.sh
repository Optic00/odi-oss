#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

#
# The boot gponact step (pon_step_gponact, rcs-lib.sh), off the device: while
# the boot_replay line of /proc/odi_gpon/status says the boot replay stopped
# after a failed table clean (-5 -EIO, -145 -ETIMEDOUT on MIPS), the step
# writes "gponact boot" again, up to GPONACT_TRIES writes in all,
# GPONACT_RETRY_S seconds apart; anything else ends it. pon_step, sleep and
# crumb are stubs, and the status file is a plain file: each gponact the
# stub sees takes the next boot_replay value from a list.
# shellcheck disable=SC2329,SC2034 # the stubs are called by the sourced step
set -u
cd "$(dirname "$0")/.." || exit 1
LIB=$PWD/rootfs/skeleton/etc/scripts/rcs-lib.sh
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
pass=0 fail=0
ok() { echo "ok    $1"; pass=$((pass + 1)); }
no() { echo "FAIL  $1"; printf '        wanted: %s\n        got:    %s\n' "$2" "$3"; fail=$((fail + 1)); }
eq() { if [ "$3" = "$2" ]; then ok "$1"; else no "$1" "$2" "$3"; fi; }

# shellcheck source=/dev/null
. "$LIB"
SAFE_FLAG=$T/safe-mode
GPON_STATUS=$T/status
store_sn() { echo ODIT00000001; }
grep() { [ "$*" = '-q type=1  /proc/odi_omci' ] && return 0; command grep "$@"; }
answers='' steps='' crumbs='' sleeps='' on_sleep=''
# Each gponact moves boot_replay to the next value of the list ("failed -5",
# "done", ...; "-" leaves the line as it is, "none" removes the file).
pon_step() {
	steps="${steps}[$1]"
	a=${answers%%,*}
	answers=${answers#*,}
	case $a in
	-) ;;
	none) rm -f "$GPON_STATUS" ;;
	*) printf 'state 1 (O1)\nboot_replay %s\nonu_id -1\n' "$a" > "$GPON_STATUS" ;;
	esac
}
sleep() {
	sleeps="${sleeps}($1)"
	[ -n "$on_sleep" ] && printf 'boot_replay %s\n' "$on_sleep" > "$GPON_STATUS"
	return 0
}
crumb() { crumbs="${crumbs}{$*}"; }
run() { rm -f "$GPON_STATUS"; answers="$1," steps='' crumbs='' sleeps=''; pon_step_gponact; }
B='[gponact boot]'

run "done"
eq "clean on the first try: one gponact" "[gponact]" "$steps"
eq "  no wait" "" "$sleeps"

run "failed -5,done"
eq "-EIO, then clean: a retry with gponact boot" "[gponact]$B" "$steps"
eq "  one wait of GPONACT_RETRY_S" "(5)" "$sleeps"
eq "  the retry is logged" 1 "$(printf '%s' "$crumbs" | command grep -c 'not clean (-5), try 2 of 3 in 5s')"

run "failed -145,failed -5,done"
eq "-ETIMEDOUT (MIPS -145), -EIO, then clean: three gponacts" "[gponact]$B$B" "$steps"
eq "  two waits" "(5)(5)" "$sleeps"

run "failed -5,failed -5,failed -5,failed -5"
eq "never clean: GPONACT_TRIES gponacts, no more" "[gponact]$B$B" "$steps"
eq "  two waits" "(5)(5)" "$sleeps"
eq "  and says the PON stays down" 1 "$(printf '%s' "$crumbs" | command grep -c 'still not clean after 3 tries, the PON stays down')"

run "failed -110"
eq "-110 is not ETIMEDOUT on MIPS: no retry" "[gponact]" "$steps"
run "failed -1"
eq "another error: no retry" "[gponact]" "$steps"
run "not_run"
eq "no replay ran (laser off, no boot table): no retry" "[gponact]" "$steps"
run none
eq "no status file: no retry" "[gponact]" "$steps"

# Another path activates the ONU while the step waits: the status turns to
# done and the step stops without writing again.
on_sleep='done'
run "failed -5,failed -5"
eq "activated elsewhere during the wait: no further gponact" "[gponact]" "$steps"
on_sleep=''

GPONACT_TRIES=1
run "failed -5,done"
eq "GPONACT_TRIES=1: a single gponact" "[gponact]" "$steps"
GPONACT_TRIES=4 GPONACT_RETRY_S=2
run "failed -5,failed -5,failed -5,done"
eq "GPONACT_TRIES=4, GPONACT_RETRY_S=2" "[gponact]$B$B$B" "$steps"
eq "  three waits of 2 s" "(2)(2)(2)" "$sleeps"
GPONACT_TRIES=x GPONACT_RETRY_S=1.5
run "failed -5,failed -5,failed -5,failed -5"
eq "values that are not numbers fall back to 3 tries" "[gponact]$B$B" "$steps"
eq "  and 5 s" "(5)(5)" "$sleeps"
GPONACT_TRIES=99999999999999999999999 GPONACT_RETRY_S=99999999999999999999
run "failed -5,failed -5,failed -5,failed -5"
eq "values too long for the shell fall back to 3 tries" "[gponact]$B$B" "$steps"
eq "  and 5 s" "(5)(5)" "$sleeps"
GPONACT_TRIES=0 GPONACT_RETRY_S=60
run "failed -5,failed -5,failed -5,failed -5"
eq "0 tries and 60 s are out of range: 3 tries, 5 s" "[gponact]$B$B(5)(5)" "$steps$sleeps"
GPONACT_TRIES=3 GPONACT_RETRY_S=5

# A try the driver refuses before the replay (the laser off since, the boot
# table gone) shows as another errno and ends the step.
run "failed -5,failed -1"
eq "a clean that failed, then a refusal: no third try" "[gponact]$B" "$steps"
eq "  and no claim of a third failed clean" 0 "$(printf '%s' "$crumbs" | command grep -c 'still not clean')"

: > "$SAFE_FLAG"
run "failed -5,done"
eq "a safe-mode boot writes no gponact" "" "$steps"
rm -f "$SAFE_FLAG"

# The step runs under set -e in rcS.pon: a failed gponact must not end it.
out=$(set -e; rm -f "$GPON_STATUS"; answers='failed -5,failed -5,failed -5,'; pon_step_gponact; echo survived)
eq "under set -e the step returns 0 after the last try" survived "$out"
out=$(set -e; answers='none,'; pon_step_gponact; echo survived)
eq "under set -e an unreadable status ends it quietly" survived "$out"

echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
