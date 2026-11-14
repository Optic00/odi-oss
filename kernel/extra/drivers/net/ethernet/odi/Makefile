# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Built-in initcalls run in link order, which is the order below:
# odi_switch.o maps the switch-core window before the others touch it. The board replay does not depend on this: its caller
# (arch/mips/rtl8686/board.c) links earlier, so odi_board_init() maps the
# window itself (odi_switch_mmio_ensure()).
obj-$(CONFIG_ODI_NIC) += odi_nic.o
obj-$(CONFIG_ODI_SWITCH) += odi_omci.o
obj-$(CONFIG_ODI_SWITCH) += odi_switch.o odi_switch_cmd.o odi_switch_bdgconn.o odi_switch_tbl.o odi_switch_l2.o odi_switch_mcast.o odi_switch_acl_probe.o odi_switch_log.o odi_switch_mc_count.o odi_switch_pin.o odi_reg.o odi_i2c.o odi_ddm.o
obj-$(CONFIG_ODI_SWITCH) += odi_i2c_adapter.o
obj-$(CONFIG_ODI_SWITCH) += odi_switch_port.o odi_switch_qos.o odi_switch_ds_gem.o odi_switch_cf.o odi_sfp_mirror.o \
			   odi_switch_platform.o odi_switch_mib.o
obj-$(CONFIG_ODI_SWITCH) += odi_replay.o odi_replay_blob.o odi_replay_fw.o
obj-$(CONFIG_ODI_SWITCH) += odi_switch_sdkinit.o
obj-$(CONFIG_ODI_SWITCH) += odi_board.o odi_board_data.o
obj-$(CONFIG_ODI_SWITCH) += odi_init.o
obj-$(CONFIG_ODI_WDT) += odi_wdt.o
obj-$(CONFIG_ODI_HWMON) += odi_hwmon.o
# The SoC-window accessors, for the switch replays and the watchdog.
ifneq ($(CONFIG_ODI_SWITCH)$(CONFIG_ODI_WDT),)
obj-y += odi_soc.o
endif
obj-$(CONFIG_ODI_RAMLOG) += odi_ramlog.o
obj-$(CONFIG_ODI_SWITCH) += odi_gpon.o odi_gpon_hw.o odi_gpon_isr.o odi_gpon_fsm.o \
			   odi_gpon_ploam.o odi_gpon_pw.o odi_gpon_init.o odi_gpon_ctr.o odi_gpon_flow.o \
			   odi_gpon_drop.o

# The build id the ramlog stamps into each boot (kernel/build.sh). Only this
# object carries it, so a new id rebuilds one file, not the tree.
ifneq ($(ODI_BUILD_ID),)
CFLAGS_odi_ramlog.o += -DODI_RAMLOG_BUILD_ID=\"$(ODI_BUILD_ID)\"
endif
