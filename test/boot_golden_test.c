/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * boot_golden_test.c -- the register stream of a production boot, reads
 * included, through the host mock, in the order a boot issues it:
 *
 *   1. the board replay (odi_board_init());
 *   2. the odi_init verbs of the rcS main loop, through the real
 *      /proc/odi_init dispatch (odi_init_apply());
 *   3. the PON steps up to gpondev: optics, i2c, i2cen, laser, gpon, rxsd,
 *      gpondrv;
 *   4. init_platform, every item, as the rcS /proc/odi_omci write asks;
 *   5. the module-load replay (modload.bin carries the production
 *      selection);
 *   6. gponsn with a made-up serial number, then the first gponact;
 *   7. the driver commands of one ISP: ISP1, the 91 commands of the boot5
 *      capture (isp1_boot5_cmds.h); ISP2, its two cmd 51 services.
 *
 * Usage: boot_golden_test isp1|isp2 <out-file>. boot_golden_test.sh
 * compares phases 1-6 with test/fixtures/boot-golden-common.txt and phase
 * 7 with test/fixtures/boot-golden-<isp>.txt.
 *
 * One line per log entry, "<kind> <addr> <value>" in hex (the kinds of
 * odi_switch_mock.h), with no timestamps, so a change shows as the lines
 * it adds or removes.
 *
 * The GPON verbs go through a host stand-in for odi_gpon_verb() (odi_gpon.c
 * is kernel only) that does what that function does on the boot path:
 * gpondrv enables the GPON interrupt type, gponsn keeps the serial number,
 * the first gponact replays gpon_init.bin with it (the GPON MAC reset in
 * it completes at once, golden_write_hook()). The boot-time reset of
 * the interrupt line (odi_gpon_chip_irq_reset(), at module init) is not a
 * boot step of rcS, and not here.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The largest single phase (one sdkinit verb, one cmd 51) must fit. */
#define ODI_MOCK_LOG_MAX	(1U << 18)

#include "odi_switch_unity.h"
#include "odi_replay_fw_host.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_sdkinit.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_board_data.c"
/* odi_board_optics() identifies the laser driver over I2C before it lights
 * the laser (odi_board.h). A GN25L95 here, with no register traffic, so the
 * captured sequence is unchanged: page select 2, byte 0xa0 reads 0x6a.
 * Only odi_board.c is pointed at these two (the defines around its
 * include); everything else, the cmd 10 DDM reads included, goes through
 * the real odi_i2c.c that odi_switch_unity.h links.
 */
static uint8_t fake_a2_page = 0;
static int fake_i2c_read_bytes(uint32_t sel, uint32_t addr, uint8_t *out, unsigned int n)
{
	(void)sel;
	if (n != 1)
		return -EINVAL;
	*out = addr == 0x7fU ? fake_a2_page : (fake_a2_page == 2U && addr == 0xa0U ? 0x6aU : 0U);
	return 0;
}
static int fake_i2c_write_byte(uint32_t sel, uint32_t addr, uint8_t val)
{
	(void)sel;
	if (addr != 0x7fU)
		return -EPERM;
	fake_a2_page = val;
	return 0;
}
#define odi_i2c_read_bytes fake_i2c_read_bytes
#define odi_i2c_write_byte fake_i2c_write_byte
#include "../kernel/extra/drivers/net/ethernet/odi/odi_board.c"
#undef odi_i2c_read_bytes
#undef odi_i2c_write_byte
#include "../kernel/extra/drivers/net/ethernet/odi/odi_gpon_ploam.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_gpon_fsm.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_gpon_hw.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_gpon_isr.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_gpon_init.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_cmd.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_bdgconn.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_init.c"

#include "../kernel/extra/drivers/net/ethernet/odi/uapi/omci_gemflow.h"
#include "../kernel/extra/drivers/net/ethernet/odi/uapi/omci_bdgconn.h"
#include "bdgconn_rules.h"
#include "isp1_boot5_cmds.h"

/* A serial number that belongs to no stick. */
#define GOLDEN_SN		"TEST00000001"

static FILE *out;

/* Writes the entries logged since the last flush and empties the log; the
 * register file itself is kept, so each phase sees what the one before it
 * left.
 */
static void flush(void)
{
	unsigned int i;

	odi_mock_table_flush();
	for (i = 0; i < odi_mock.log_n; i++) {
		const struct odi_mock_write *e = &odi_mock.log[i];

		fprintf(out, "%c %x %x\n", e->kind, e->addr, e->val);
	}
	if (odi_mock.log_n >= ODI_MOCK_LOG_MAX) {
		fprintf(stderr, "boot_golden_test: log full, raise ODI_MOCK_LOG_MAX\n");
		exit(1);
	}
	odi_mock.log_n = 0;
}

/* ---- host stand-in for odi_gpon_verb() (odi_gpon.c) ---------------------- */

static uint8_t golden_sn[8];
static int golden_gpon_booted;

static int golden_hex_byte(const char *p, uint8_t *b)
{
	unsigned int v;

	if (sscanf(p, "%2x", &v) != 1)
		return -1;
	*b = (uint8_t)v;
	return 0;
}

int odi_gpon_verb(const char *verb, const char *arg)
{
	static const uint8_t no_password[10];
	struct odi_replay_fw fw;
	unsigned int i;
	int rc;

	if (!strcmp(verb, "gpondrv")) {
		odi_gpon_chip_irq_enable();
		return 0;
	}
	if (!strcmp(verb, "gpondev"))
		return 0;
	if (!strcmp(verb, "gponsn")) {
		if (!arg || strlen(arg) != 12U)
			return -EINVAL;
		memcpy(golden_sn, arg, 4U);
		for (i = 0; i < 4U; i++)
			if (golden_hex_byte(&arg[4U + 2U * i], &golden_sn[4U + i]))
				return -EINVAL;
		return 0;
	}
	if (!strcmp(verb, "gponact") && !golden_gpon_booted) {
		rc = odi_replay_fw_load(ODI_REPLAY_TABLE_GPON_INIT, &fw);
		if (rc)
			return rc;
		rc = odi_gpon_init_apply(&fw.blob, golden_sn, no_password);
		odi_replay_fw_release(&fw);
		if (rc)
			return rc;
		golden_gpon_booted = 1;
		return 0;
	}
	return -ENOENT;
}

/* ---- the phases ------------------------------------------------------------ */

/* errno values differ between hosts; the golden names them. */
static const char *rc_name(int rc)
{
	static char buf[16];

	switch (rc) {
	case 0: return "0";
	case -ENOENT: return "-ENOENT";
	case -ENOSYS: return "-ENOSYS";
	case -EINVAL: return "-EINVAL";
	case -EPERM: return "-EPERM";
	}
	snprintf(buf, sizeof(buf), "%d", rc);
	return buf;
}

static void verb(const char *name, const char *arg)
{
	int rc = odi_init_apply(name, arg);

	flush();
	fprintf(out, "# odi_init %s%s%s: rc %s\n", name, arg ? " " : "", arg ? arg : "", rc_name(rc));
}

static void call(uint32_t cmd, void *buf, uint32_t len)
{
	int rc;

	odi_mock_mark(cmd);
	rc = odi_switch_cmd(cmd, buf, len);
	odi_mock_mark(0x80000000U | cmd);
	flush();
	fprintf(out, "# cmd %u: rc %s\n", cmd, rc_name(rc));
}

static void isp2_cmds(void)
{
	struct omci_bdgconn b;
	struct omci_vlan_oper vr;

	/* The two services omcid builds on ISP2 (odi_switch_isp2_test.c). */
	bdg_gen_manual(&vr, 10, 0, 0);
	bdg_conn(&b, 0, OMCI_DIR_BI, 5, 0, 0, &vr);
	call(51, &b, sizeof b);
	bdg_gen_manual(&vr, 10, 0, 1);
	bdg_conn(&b, 1, OMCI_DIR_DS, 5, 0, 1, &vr);
	call(51, &b, sizeof b);
}

/* The one piece of hardware the boot stream waits on: the GPON MAC reset
 * of the gpon_init replay, which finishes as soon as its request is
 * released, so the wait reads PONMAC_RESET once.
 */
static void golden_write_hook(uint32_t off, uint32_t val)
{
	if (off == ODI_GPON_PONMAC_RESET_OFF && !ODI_GPON_PONMAC_RESET_RESET_REQ_GET(val))
		odi_mock.regs[odi_mock_slot(off)] = val | ODI_GPON_PONMAC_RESET_RESET_COMPLETE;
}

int main(int argc, char **argv)
{
	static const char *const main_loop[] = {
		"intr", "irq", "switch", "svlan", "stp", "oam", "acl", "qos", "sec",
		"rate", "classify", "stat", "trunk", "l2", "vlan", "port", "mirror",
		"cpu", "rldp", "trap", "gpio", "time", "ponmac",
	};
	struct odi_replay_fw fw;
	unsigned int i;
	int isp;

	if (argc < 3 || (strcmp(argv[1], "isp1") && strcmp(argv[1], "isp2"))) {
		fprintf(stderr, "usage: %s isp1|isp2 <out-file>\n", argv[0]);
		return 2;
	}
	isp = argv[1][3] - '0';
	out = fopen(argv[2], "w");
	if (!out) {
		fprintf(stderr, "boot_golden_test: cannot open %s\n", argv[2]);
		return 1;
	}

	odi_mock_reset();
	odi_mock_port_isolation_reset_state();
	odi_mock_dsq_reset_state();
	odi_switch_cmd_reset_state();
	odi_mock_log_reads = 1;
	odi_mock_write_hook = golden_write_hook;

	fprintf(out, "# boot golden, ISP%d\n# phase: board\n", isp);
	odi_board_init();
	flush();

	fprintf(out, "# phase: odi_init main loop\n");
	for (i = 0; i < sizeof(main_loop) / sizeof(main_loop[0]); i++)
		verb(main_loop[i], NULL);

	fprintf(out, "# phase: pon steps to gpondev\n");
	verb("optics", NULL);
	verb("i2c", "1");
	verb("i2cen", "1");
	verb("laser", NULL);
	verb("gpon", NULL);
	verb("rxsd", NULL);
	verb("gpondrv", NULL);
	verb("gpondev", NULL);

	fprintf(out, "# phase: init_platform\n");
	odi_switch_init_platform();
	flush();

	fprintf(out, "# phase: init_modload\n");
	if (odi_replay_fw_load(ODI_REPLAY_TABLE_MODLOAD, &fw)) {
		fprintf(stderr, "boot_golden_test: no modload.bin\n");
		return 1;
	}
	odi_switch_init_modload(&fw.blob);
	odi_replay_fw_release(&fw);
	flush();

	fprintf(out, "# phase: gponsn, gponact\n");
	verb("gponsn", GOLDEN_SN);
	verb("gponact", NULL);

	fprintf(out, "# phase: ISP%d driver commands\n", isp);
	if (isp == 1)
		isp1_boot5_replay(call);
	else
		isp2_cmds();

	if (!odi_mock_locks_idle()) {
		fprintf(stderr, "boot_golden_test: a lock is still held\n");
		return 1;
	}
	fclose(out);
	return 0;
}
