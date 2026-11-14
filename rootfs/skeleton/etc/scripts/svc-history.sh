#!/bin/sh
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

# svc-history.sh -- the /etc/inittab respawn entry for the minute history
# (history.sh, docs/TOOLS.md "The minute history"). `services stop history`
# writes history.off and the ring stops growing; what it holds stays until
# the next reboot.
[ -f /etc/config/history.off ] && exec /etc/scripts/respawn-off.sh
# oom_score_adj 0, as metricsd: losing it costs neither access nor GPON.
exec /etc/scripts/respawn.sh history 0 /var/log/services.log /etc/scripts/history.sh run
