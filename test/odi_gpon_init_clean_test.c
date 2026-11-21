/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * odi_gpon_init_clean_test.c -- the GPON MAC reset wait and table clean of
 * odi_gpon_init.c, against a model of the two CAMs and the reset register
 * hung on the mock's write and read hooks (test/odi_switch_mock.h):
 *
 *   - a CAM answers a request (REQ set) at once: OP 3 clears the row, OP 2
 *     sets ROW_VALID when the row holds an entry and puts the entry in
 *     RDATA, OP 1 stores WDATA; DONE is set either way;
 *   - when RESET_REQ goes back to 0 the block stays in reset for a number
 *     of bus accesses (reads of PONMAC_RESET and writes alike), and drops
 *     every CAM request and flow type write that reaches it meanwhile;
 *     then PONMAC_RESET reads RESET_COMPLETE. That is the cold-boot
 *     failure: the replay used to go on writing at once, and its own
 *     clean of the tables was lost.
 *
 * The downstream multicast filter is a third CAM of the same shape (256
 * rows, DS_MC_FILTER_CTL), and its self-test words read 0x8000 (done, no
 * failure) unless a case says otherwise.
 *
 * Each case starts from the state a power-up left on a stick (most rows
 * valid with random entries, most flow type words not zero) and checks the
 * report and the tables afterwards: a plain power-up, clears lost in the
 * first pass for a reason the reset wait does not see, a row that never
 * clears, a flow type word that never clears, a CAM that never answers,
 * and the whole boot table, replayed with and without the wait.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "odi_switch_unity.h"
#include "odi_replay_fw_host.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_gpon_init.c"

static int failures;

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
		failures++; \
	} \
} while (0)

/* ---- the model ----------------------------------------------------------- */

struct cam_model {
	int valid[ODI_GPON_INIT_MC_ROWS];
	uint32_t entry[ODI_GPON_INIT_MC_ROWS];
	int sticky[ODI_GPON_INIT_MC_ROWS];	/* a clear of this row is ignored */
};

static struct {
	struct cam_model gem, alloc, mc;
	unsigned int drop_clears;	/* the first N clear requests are ignored */
	unsigned int reset_accesses;	/* bus accesses the reset lasts after its release */
	int reset_never_completes;
	unsigned int busy;		/* accesses left before the reset completes */
	uint32_t flow_type[ODI_GPON_INIT_GEM_ROWS];	/* what the flow type words hold */
	int flow_type_stuck_row;	/* this flow type word keeps 0x1f; -1 none */
	unsigned int seq;		/* writes seen */
	unsigned int reset_released_at;	/* seq of the RESET_REQ 1 -> 0 write, 0 none */
	unsigned int first_clear_at;	/* seq of the first clear request, 0 none */
	int reset_req;
} m;

static uint32_t *reg(uint32_t off)
{
	return &odi_mock.regs[odi_mock_slot(off)];
}

static void cam_request(struct cam_model *c, uint32_t ctl_off, uint32_t rdata_off,
			uint32_t wdata_off, uint32_t row_mask, uint32_t val)
{
	unsigned int row = val & row_mask, op = (val >> 8) & 3U;
	uint32_t out = val | (1U << 14);

	switch (op) {
	case 1:
		c->valid[row] = 1;
		c->entry[row] = *reg(wdata_off) & (c == &m.mc ? 0xffffffffU : 0xfffU);
		break;
	case 2:
		if (c->valid[row])
			out |= 1U << 13;
		*reg(rdata_off) = c->entry[row];
		break;
	case 3:
		if (!m.first_clear_at)
			m.first_clear_at = m.seq;
		if (m.drop_clears) {
			m.drop_clears--;
			break;
		}
		if (!c->sticky[row])
			c->valid[row] = 0;
		break;
	}
	*reg(ctl_off) = out;
}

/* One bus access of time while the block is in reset. Returns whether
 * the block was still in reset when the access reached it.
 */
static int reset_tick(void)
{
	if (!m.busy)
		return 0;
	if (!m.reset_never_completes && --m.busy == 0)
		*reg(ODI_GPON_PONMAC_RESET_OFF) |= ODI_GPON_PONMAC_RESET_RESET_COMPLETE;
	return 1;
}

static void model_read(uint32_t off)
{
	if (off == ODI_GPON_PONMAC_RESET_OFF)
		(void)reset_tick();
}

static void model_write(uint32_t off, uint32_t val)
{
	m.seq++;
	if (off >= ODI_SW_DSF_GEM_FLOW_TYPE(0) &&
	    off <= ODI_SW_DSF_GEM_FLOW_TYPE(ODI_GPON_INIT_GEM_ROWS - 1U)) {
		unsigned int row = (off - ODI_SW_DSF_GEM_FLOW_TYPE(0)) / 4U;

		if (reset_tick() || (int)row == m.flow_type_stuck_row)
			*reg(off) = m.flow_type[row];
		else
			m.flow_type[row] = val;
		return;
	}
	if (off != ODI_GPON_PONMAC_RESET_OFF && reset_tick())
		return;
	if (off == ODI_SW_DSF_GEM_CAM_CTL_OFF && (val & (1U << 15)))
		cam_request(&m.gem, off, ODI_SW_DSF_GEM_CAM_RDATA_OFF, ODI_SW_DSF_GEM_CAM_WDATA_OFF,
			    0x7fU, val);
	else if (off == ODI_SW_DSF_ALLOC_CAM_CTL_OFF && (val & (1U << 15)))
		cam_request(&m.alloc, off, ODI_SW_DSF_ALLOC_CAM_RDATA_OFF,
			    ODI_SW_DSF_ALLOC_CAM_WDATA_OFF, 0x1fU, val);
	else if (off == ODI_GPON_DS_MC_FILTER_CTL_OFF && (val & (1U << 15)))
		cam_request(&m.mc, off, ODI_GPON_DS_MC_FILTER_RDATA_OFF,
			    0x704088U /* its write data, unused by the driver */, 0xffU, val);
	else if (off == ODI_GPON_PONMAC_RESET_OFF) {
		if (m.reset_req && !(val & 1U)) {
			m.reset_released_at = m.seq;
			m.busy = m.reset_accesses;
			if (!m.busy && !m.reset_never_completes)
				*reg(off) = val | ODI_GPON_PONMAC_RESET_RESET_COMPLETE;
			if (!m.busy && m.reset_never_completes)
				m.busy = 1U;
		}
		m.reset_req = (int)(val & 1U);
	}
}

/* What ISP2 read back after a power cut: GEM-port rows 0..119 valid with
 * mostly-ones port ids, rows 120..127 clear, every Alloc-ID row valid,
 * flow type words 1..119 not zero. Nobody has read the multicast filter
 * after a power cut: here every third row holds a random word, and its
 * self-test words read done.
 */
static void power_up(void)
{
	unsigned int r;

	odi_mock_reset();
	memset(&m, 0, sizeof(m));
	m.flow_type_stuck_row = -1;
	odi_mock_write_hook = model_write;
	odi_mock_read_hook = model_read;
	for (r = 0; r < ODI_GPON_INIT_GEM_ROWS; r++) {
		m.gem.valid[r] = r < 120U;
		m.gem.entry[r] = 0xf00U | ((r * 37U) & 0xffU);
		m.flow_type[r] = (r >= 1U && r < 120U) ? 1U + r % 0x1fU : 0U;
		*reg(ODI_SW_DSF_GEM_FLOW_TYPE(r)) = m.flow_type[r];
	}
	for (r = 0; r < ODI_GPON_INIT_ALLOC_ROWS; r++) {
		m.alloc.valid[r] = 1;
		m.alloc.entry[r] = 0xe00U | r;
	}
	for (r = 0; r < ODI_GPON_INIT_MC_ROWS; r++) {
		m.mc.valid[r] = r % 3U == 1U;
		m.mc.entry[r] = 0x5e000000U | (r * 0x10101U);
	}
	for (r = 0; r < ODI_GPON_INIT_MC_BIST_WORDS; r++)
		*reg(ODI_GPON_DS_MC_FILTER_BIST(r)) = ODI_GPON_DS_MC_FILTER_BIST_ALL_BIST_DONE;
}

static unsigned int mc_rows_valid(void)
{
	unsigned int r, n = 0;

	for (r = 0; r < ODI_GPON_INIT_MC_ROWS; r++)
		n += m.mc.valid[r] ? 1U : 0U;
	return n;
}

static int tables_clear(void)
{
	unsigned int r;

	for (r = 0; r < ODI_GPON_INIT_GEM_ROWS; r++)
		if (m.gem.valid[r] || *reg(ODI_SW_DSF_GEM_FLOW_TYPE(r)))
			return 0;
	for (r = 0; r < ODI_GPON_INIT_ALLOC_ROWS; r++)
		if (m.alloc.valid[r])
			return 0;
	return mc_rows_valid() == 0U;
}

/* ---- the cases ----------------------------------------------------------- */

static void test_power_up(void)
{
	struct odi_gpon_clean_report r;

	power_up();
	CHECK(odi_gpon_init_clean_tables(&r) == 0, "power-up: clean");
	CHECK(r.before.gem_valid == 120U && r.before.alloc_valid == 32U &&
	      r.before.flow_type_set == 119U, "power-up: the census before counts the garbage");
	CHECK(r.before.gem_row == 0 && r.before.gem_port == 0xf00U,
	      "power-up: the first valid GEM-port row and its port");
	CHECK(r.before.alloc_row == 0 && r.before.alloc_id == 0xe00U,
	      "power-up: the first valid Alloc-ID row and its id");
	CHECK(r.after.gem_valid == 0U && r.after.alloc_valid == 0U && r.after.flow_type_set == 0U &&
	      r.after.gem_row == -1 && r.after.alloc_row == -1 && !r.after.timed_out,
	      "power-up: every row reads back clear");
	CHECK(r.passes == 1U, "power-up: one pass");
	CHECK(r.before.alloc_seen[0] == 0xe00U && r.before.alloc_seen[7] == 0xe07U &&
	      r.before.gem_seen[1] == (0xf00U | 37U),
	      "power-up: the first values of the valid rows are kept for the log");
	CHECK(r.before.mc_valid == 85U && r.before.mc_row == 1 &&
	      r.before.mc_data == (0x5e000000U | 0x10101U),
	      "power-up: the multicast filter rows found, the first with its word");
	CHECK(r.after.mc_valid == 0U && r.after.mc_row == -1,
	      "power-up: every multicast filter row reads back clear");
	CHECK(r.mc_bist[0] == 0x8000U && r.mc_bist[7] == 0x8000U && r.mc_bist_bad == 0U,
	      "power-up: the self-test words are reported, none bad");
	CHECK(tables_clear(), "power-up: the model agrees");
	CHECK(odi_mock_locks_idle(), "power-up: odi_switch_dsf_lock released");
}

/* The cold-boot failure: the block drops what reaches it too early, so the
 * first pass leaves rows behind; the read-back sees them and the second
 * pass clears them.
 */
static void test_first_pass_lost(void)
{
	struct odi_gpon_clean_report r;

	power_up();
	m.drop_clears = 140U;
	CHECK(odi_gpon_init_clean_tables(&r) == 0, "lost pass: clean in the end");
	CHECK(r.passes == 2U, "lost pass: a second pass");
	CHECK(tables_clear(), "lost pass: the model agrees");
}

static void test_sticky_row(void)
{
	struct odi_gpon_clean_report r;

	power_up();
	m.gem.sticky[37] = 1;
	CHECK(odi_gpon_init_clean_tables(&r) == -EIO, "sticky row: -EIO");
	CHECK(r.passes == 2U, "sticky row: both passes ran");
	CHECK(r.after.gem_valid == 1U && r.after.gem_row == 37 &&
	      r.after.gem_port == m.gem.entry[37], "sticky row: named in the report, with its port");
	CHECK(r.after.alloc_valid == 0U && r.after.flow_type_set == 0U,
	      "sticky row: the rest is clear");
	CHECK(odi_mock_locks_idle(), "sticky row: odi_switch_dsf_lock released");
}

/* A multicast filter row that never clears: the clean fails and names
 * it, like a CAM row; a self-test word with a FAIL bit is counted and
 * never written.
 */
static void test_mc_filter(void)
{
	struct odi_gpon_clean_report r;
	unsigned int i, bist_writes = 0;

	power_up();
	CHECK(odi_gpon_init_clean_tables(&r) == 0 && mc_rows_valid() == 0U,
	      "multicast filter: every row cleared");

	power_up();
	m.mc.sticky[200] = 1;
	m.mc.valid[200] = 1;
	*reg(ODI_GPON_DS_MC_FILTER_BIST(3)) = ODI_GPON_DS_MC_FILTER_BIST_ALL_BIST_DONE |
					      ODI_GPON_DS_MC_FILTER_BIST_BIST_FAIL;
	*reg(ODI_GPON_DS_MC_FILTER_BIST(5)) = 0;
	CHECK(odi_gpon_init_clean_tables(&r) == -EIO, "multicast filter: a sticky row is -EIO");
	CHECK(r.passes == 2U && r.after.mc_valid == 1U && r.after.mc_row == 200 &&
	      r.after.gem_valid == 0U && r.after.alloc_valid == 0U,
	      "multicast filter: the sticky row named, the CAMs clear");
	CHECK(r.mc_bist_bad == 2U && r.mc_bist[3] == 0x8080U && r.mc_bist[5] == 0U,
	      "multicast filter: a failed and a never-run self-test word counted");
	for (i = 0; i < odi_mock.log_n; i++)
		if (odi_mock.log[i].kind == 'W' &&
		    odi_mock.log[i].addr >= ODI_GPON_DS_MC_FILTER_BIST(0) &&
		    odi_mock.log[i].addr <= ODI_GPON_DS_MC_FILTER_BIST(ODI_GPON_INIT_MC_BIST_WORDS - 1U))
			bist_writes++;
	CHECK(bist_writes == 0U, "multicast filter: the self-test words are never written");
	CHECK(odi_mock_locks_idle(), "multicast filter: odi_switch_dsf_lock released");
}

static void test_stuck_flow_type(void)
{
	struct odi_gpon_clean_report r;

	power_up();
	m.flow_type_stuck_row = 64;
	CHECK(odi_gpon_init_clean_tables(&r) == -EIO, "stuck flow type: -EIO");
	CHECK(r.after.flow_type_set == 1U && r.after.gem_valid == 0U,
	      "stuck flow type: counted, CAMs clear");
}

/* A CAM that never answers: each walk stops at its first timeout, so the
 * whole clean stays a handful of requests rather than 160 poll bounds per
 * walk with interrupts off.
 */
static void test_cam_never_answers(void)
{
	struct odi_gpon_clean_report r;

	power_up();
	odi_mock_write_hook = NULL;
	odi_mock_read_hook = NULL;
	odi_mock.poll_stuck = 1;
	CHECK(odi_gpon_init_clean_tables(&r) == -ETIMEDOUT, "no answer: -ETIMEDOUT");
	CHECK(r.before.timed_out && r.after.timed_out, "no answer: both reads say so");
	CHECK(r.passes == 2U, "no answer: retried once");
	CHECK(odi_mock.log_n < 2U * (3U * 2U * 2U + ODI_GPON_INIT_GEM_ROWS) + 8U,
	      "no answer: every walk gives up at its first timeout");
	CHECK(odi_mock_locks_idle(), "no answer: odi_switch_dsf_lock released");
	odi_mock.poll_stuck = 0;
}

static void test_reset_wait(void)
{
	unsigned int polls = 99U;

	power_up();
	odi_reg_write(ODI_GPON_PONMAC_RESET_OFF, 1U);
	odi_reg_write(ODI_GPON_PONMAC_RESET_OFF, 0U);
	CHECK(odi_gpon_init_reset_wait(&polls) == 0 && polls == 0U, "reset wait: complete");

	power_up();
	m.reset_never_completes = 1;
	odi_reg_write(ODI_GPON_PONMAC_RESET_OFF, 1U);
	odi_reg_write(ODI_GPON_PONMAC_RESET_OFF, 0U);
	CHECK(odi_gpon_init_reset_wait(&polls) == -ETIMEDOUT &&
	      polls == ODI_GPON_INIT_RESET_POLLS, "reset wait: bounded, then -ETIMEDOUT");
}

/* The shipped boot table on a cold stick: the block stays in reset for
 * about as long as ISP2 showed (its cleans of GEM-port rows 0..119 were
 * lost, about 460 records of the replay), after the release.
 */
#define COLD_RESET_ACCESSES	460U

static int boot_table_load(struct odi_replay_fw *fw)
{
	if (odi_replay_fw_load(ODI_REPLAY_TABLE_GPON_INIT, fw)) {
		CHECK(0, "test setup: gpon_init.bin loads");
		return -1;
	}
	return 0;
}

static unsigned int gem_rows_valid(void)
{
	unsigned int r, n = 0;

	for (r = 0; r < ODI_GPON_INIT_GEM_ROWS; r++)
		n += m.gem.valid[r] ? 1U : 0U;
	return n;
}

/* The table replayed straight through, as before this clean existed: the
 * garbage stays, which is what a power cut did (the table then carried its
 * own clean of the tables, which landed inside the reset; it no longer
 * does). Checks that the model reproduces the
 * failure, so the next case means something.
 */
static void test_boot_table_without_wait(void)
{
	static const struct odi_replay_opts opts = { .verb = ODI_REPLAY_ALL_VERBS };
	struct odi_replay_fw fw;

	if (boot_table_load(&fw))
		return;
	power_up();
	m.reset_accesses = COLD_RESET_ACCESSES;
	(void)odi_replay_run(&fw.blob, &opts);
	CHECK(gem_rows_valid() > 100U, "no wait: the GEM-port garbage survives the replay");
	CHECK(m.alloc.valid[5] && m.alloc.valid[31], "no wait: the Alloc-ID garbage survives too");
	odi_replay_fw_release(&fw);
}

/* Whether the part of the boot table after the clean was replayed. Two
 * registers only that part writes: DSF_ONU_STATE, which its last records
 * move out of O1 (the activation), and the serial-number word of the PLOAM
 * slot it arms, which holds the first two bytes of the serial number
 * passed in ("OD"). The mock register file keeps what was written whether
 * or not the model let it reach the block.
 */
static int rest_replayed(void)
{
	return ODI_GPON_DSF_ONU_STATE_ACTIVATION_STATE_GET(*reg(ODI_GPON_DSF_ONU_STATE_OFF)) == 3U &&
	       *reg(ODI_GPON_USF_PLOAM_TX_WORD(1)) == 0x4f44U;
}

/* odi_gpon_init_apply() on the same cold stick: it waits the reset out,
 * then the clean lands, every row reads back clear, and the rest of the
 * table follows. The table itself programs no CAM row and no flow type
 * (the OMCC row comes with Configure_Port-ID, after ranging), so every row
 * is clear at the end.
 */
static void test_boot_table_cold(void)
{
	static const uint8_t sn[8] = { 'O', 'D', 'I', 'X', 0x12, 0x34, 0x56, 0x78 };
	static const uint8_t pw[10];
	struct odi_replay_fw fw;
	unsigned int r;

	if (boot_table_load(&fw))
		return;
	power_up();
	m.reset_accesses = COLD_RESET_ACCESSES;
	CHECK(odi_gpon_init_apply(&fw.blob, sn, pw) == 0, "cold: the apply reports success");
	CHECK(m.reset_released_at && m.first_clear_at > m.reset_released_at,
	      "cold: the first clear lands after the reset release");
	CHECK(rest_replayed(), "cold: the rest of the table ran, the ONU is activated");
	CHECK(odi_gpon_init_last_clean()->reset_rc == 0 &&
	      odi_gpon_init_last_clean()->reset_polls > 0U, "cold: the reset was waited out");
	CHECK(odi_gpon_init_last_clean()->rc == 0 && odi_gpon_init_last_clean()->passes == 1U,
	      "cold: after the wait one clean pass is enough");
	CHECK(odi_gpon_init_last_clean()->before.gem_valid == 120U,
	      "cold: the report has the garbage the clean found");
	CHECK(gem_rows_valid() == 0U, "cold: no GEM-port row left valid");
	CHECK(odi_gpon_init_last_clean()->before.mc_valid == 85U &&
	      odi_gpon_init_last_clean()->after.mc_valid == 0U && mc_rows_valid() == 0U,
	      "cold: the multicast filter found dirty, read back clear");
	for (r = 0; r < ODI_GPON_INIT_ALLOC_ROWS; r++)
		CHECK(!m.alloc.valid[r], "cold: no Alloc-ID row left valid");
	for (r = 0; r < ODI_GPON_INIT_GEM_ROWS; r++)
		CHECK(*reg(ODI_SW_DSF_GEM_FLOW_TYPE(r)) == 0U, "cold: every flow type word 0");
	CHECK(odi_mock_locks_idle(), "cold: no lock left held");
	odi_replay_fw_release(&fw);
}

/* A reset that never completes: the wait gives up at its bound, the clean
 * still runs and fails (logged), and the call returns what the clean
 * returned without replaying the rest of the table: the ONU is not
 * activated on tables that may hold another ONU's Alloc-IDs.
 */
static void test_boot_table_reset_stuck(void)
{
	static const uint8_t sn[8] = { 'O', 'D', 'I', 'X', 0x12, 0x34, 0x56, 0x78 };
	static const uint8_t pw[10];
	struct odi_replay_fw fw;
	int rc;

	if (boot_table_load(&fw))
		return;
	power_up();
	m.reset_never_completes = 1;
	rc = odi_gpon_init_apply(&fw.blob, sn, pw);
	CHECK(rc == -EIO, "stuck reset: the apply returns the failure of the clean");
	CHECK(rc == odi_gpon_init_last_clean()->rc, "stuck reset: the same value as the report");
	CHECK(m.reset_released_at != 0U, "stuck reset: released");
	CHECK(odi_gpon_init_last_clean()->reset_rc == -ETIMEDOUT &&
	      odi_gpon_init_last_clean()->reset_polls == ODI_GPON_INIT_RESET_POLLS,
	      "stuck reset: the wait gave up at its bound");
	CHECK(odi_gpon_init_last_clean()->rc != 0 && odi_gpon_init_last_clean()->passes == 2U,
	      "stuck reset: the clean failed, after its retry");
	CHECK(gem_rows_valid() > 100U, "stuck reset: nothing reaches a block that stays in reset");
	CHECK(!rest_replayed(), "stuck reset: the rest of the table was not replayed");
	CHECK(odi_mock_locks_idle(), "stuck reset: no lock left held");
	odi_replay_fw_release(&fw);
}

/* A clean that fails once and works on the next call, which is what a
 * second gponact is: the first call stops after the clean, the second
 * starts again at the first record (the reset pulse included), finds the
 * tables as the first left them, cleans them and replays the rest.
 */
static void test_boot_table_retry(void)
{
	static const uint8_t sn[8] = { 'O', 'D', 'I', 'X', 0x12, 0x34, 0x56, 0x78 };
	static const uint8_t pw[10];
	struct odi_replay_fw fw;
	unsigned int released;

	if (boot_table_load(&fw))
		return;
	power_up();
	m.reset_never_completes = 1;
	CHECK(odi_gpon_init_apply(&fw.blob, sn, pw) == -EIO, "retry: the first apply fails");
	CHECK(!rest_replayed(), "retry: and stops before the rest of the table");
	released = m.reset_released_at;

	m.reset_never_completes = 0;
	m.busy = 0U;
	m.reset_accesses = COLD_RESET_ACCESSES;
	CHECK(odi_gpon_init_apply(&fw.blob, sn, pw) == 0, "retry: the second apply succeeds");
	CHECK(m.reset_released_at > released, "retry: it pulsed the reset again");
	CHECK(odi_gpon_init_last_clean()->reset_rc == 0 && odi_gpon_init_last_clean()->rc == 0 &&
	      odi_gpon_init_last_clean()->passes == 1U,
	      "retry: the report is the one of the second apply");
	CHECK(odi_gpon_init_last_clean()->before.gem_valid == 120U,
	      "retry: it found the garbage the first apply left");
	CHECK(tables_clear(), "retry: every row clear");
	CHECK(rest_replayed(), "retry: the rest of the table ran, the ONU is activated");
	CHECK(odi_mock_locks_idle(), "retry: no lock left held");
	odi_replay_fw_release(&fw);
}

/* A warm boot after a session that had ONU-ID 63 and Alloc-IDs 319 and
 * 1087: the log names them, which is what tells them from garbage.
 */
static void test_warm_names_the_entries(void)
{
	struct odi_gpon_clean_report r;
	char buf[96];

	power_up();
	memset(&m.gem, 0, sizeof(m.gem));
	memset(&m.alloc, 0, sizeof(m.alloc));
	m.alloc.valid[0] = 1; m.alloc.entry[0] = 63U;
	m.alloc.valid[1] = 1; m.alloc.entry[1] = 319U;
	m.alloc.valid[5] = 1; m.alloc.entry[5] = 1087U;
	CHECK(odi_gpon_init_clean_tables(&r) == 0, "warm: clean");
	CHECK(r.before.alloc_valid == 3U, "warm: three Alloc-ID rows");
	odi_gpon_init_seen_str(buf, sizeof(buf), r.before.alloc_seen, r.before.alloc_valid);
	CHECK(strcmp(buf, "63 319 1087") == 0, "warm: the Alloc-IDs, in row order");
	odi_gpon_init_seen_str(buf, sizeof(buf), r.before.gem_seen, r.before.gem_valid);
	CHECK(strcmp(buf, "none") == 0, "warm: no GEM-port row reads none");

	power_up();
	CHECK(odi_gpon_init_clean_tables(&r) == 0, "power-up again: clean");
	odi_gpon_init_seen_str(buf, sizeof(buf), r.before.alloc_seen, r.before.alloc_valid);
	CHECK(strcmp(buf, "3584 3585 3586 3587 3588 3589 3590 3591 +24") == 0,
	      "power-up: eight values, then the count of the rest");
	odi_gpon_init_seen_str(buf, 8, r.before.alloc_seen, r.before.alloc_valid);
	CHECK(strlen(buf) < 8U, "a short buffer stays terminated");
}

int main(void)
{
	test_power_up();
	test_warm_names_the_entries();
	test_first_pass_lost();
	test_sticky_row();
	test_mc_filter();
	test_stuck_flow_type();
	test_cam_never_answers();
	test_reset_wait();
	test_boot_table_without_wait();
	test_boot_table_cold();
	test_boot_table_reset_stuck();
	test_boot_table_retry();
	odi_mock_write_hook = NULL;
	odi_mock_read_hook = NULL;
	CHECK(odi_replay_fw_host_loads == odi_replay_fw_host_releases, "every load was released");

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("odi_gpon_init_clean_test: ok\n");
	return 0;
}
