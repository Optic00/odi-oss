// SPDX-FileCopyrightText: 2026 Andre Ziviani
// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * odi_gpon_init.c -- odi_gpon_init_apply(), the replay of gpon_init.bin
 * through odi_replay_run() with the serial number substituted, and the
 * GPON MAC reset wait and table clean that run inside it
 * (odi_gpon_init.h has the rules).
 */
#include "odi_gpon_init.h"
#include "odi_gpon_hw.h"
#include "odi_replay.h"
#include "odi_replay_blob.h"
#include "odi_switch_hw.h"
#include "odi_switch_reg.h"
#include "odi_switch_tbl.h"

#ifdef __KERNEL__
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/printk.h>
#include <linux/ratelimit.h>
#include <linux/sprintf.h>
#include <linux/string.h>
#define ODI_GPON_INIT_UDELAY(us)	udelay(us)
#define ODI_GPON_INIT_INFO(fmt, ...)	pr_info("odi_gpon: " fmt, ##__VA_ARGS__)
#define ODI_GPON_INIT_ERR(fmt, ...)	pr_err_ratelimited("odi_gpon: " fmt, ##__VA_ARGS__)
#else
#include <errno.h>
#include <stdio.h>
#include <string.h>
/* The host tests check the report, not the log: the format is still
 * compiled against its arguments, nothing is printed.
 */
#define ODI_GPON_INIT_UDELAY(us)	do { } while (0)
#define ODI_GPON_INIT_INFO(fmt, ...)	do { if (0) printf(fmt, ##__VA_ARGS__); } while (0)
#define ODI_GPON_INIT_ERR(fmt, ...)	do { if (0) printf(fmt, ##__VA_ARGS__); } while (0)
#endif

/* DSF_GEM_CAM_CTL.OP and DSF_ALLOC_CAM_CTL.OP: 1 writes WDATA into the
 * row, 2 reads it (ROW_VALID, RDATA), 3 clears it.
 */
#define ODI_GPON_CAM_OP_READ	2U
#define ODI_GPON_CAM_OP_CLEAR	3U

/* USF_PLOAM_TX_CTL (odi_gpon_hw.h), the register whose write opens and arms a
 * PLOAM transmit slot; the serial-number slot of the table is bracketed by it. */
#define GPON_INIT_SLOT_CTL_OFF 0x7050c0U

/* sn_word 1..4 is serial-number bytes 0-1, 2-3, 4-5, 6-7. */
static uint32_t odi_gpon_init_value(const struct odi_replay_event *e, const void *ctx)
{
	const uint8_t *serial_number = ctx;
	unsigned int b;

	if (!e->sn_word)
		return e->value;
	b = (e->sn_word - 1U) * 2U;
	return ((uint32_t)serial_number[b] << 8) | (uint32_t)serial_number[b + 1U];
}

/* ---- the reset wait ---------------------------------------------------- */

/* A counted loop rather than odi_poll_reg(): the number of reads is the
 * one measurement of how long the reset takes, warm and cold, and the
 * kernel poll helper does not return it. Bounded the same way, in delay
 * steps, since this kernel has no running clock with interrupts off.
 */
int odi_gpon_init_reset_wait(unsigned int *polls)
{
	unsigned int n;

	for (n = 0; n < ODI_GPON_INIT_RESET_POLLS; n++) {
		if (odi_reg_read(ODI_GPON_PONMAC_RESET_OFF) & ODI_GPON_PONMAC_RESET_RESET_COMPLETE) {
			*polls = n;
			return 0;
		}
		ODI_GPON_INIT_UDELAY(1);
	}
	*polls = n;
	return -ETIMEDOUT;
}

/* ---- the table clean ----------------------------------------------------- */

/* The two CAMs share one handshake and differ in their registers and the
 * width of CAM_ROW.
 */
struct odi_gpon_cam {
	uint32_t ctl;
	uint32_t rdata;
	unsigned int rows;
	int gem;
};

static const struct odi_gpon_cam odi_gpon_cam_gem = {
	ODI_SW_DSF_GEM_CAM_CTL_OFF, ODI_SW_DSF_GEM_CAM_RDATA_OFF, ODI_GPON_INIT_GEM_ROWS, 1,
};

static const struct odi_gpon_cam odi_gpon_cam_alloc = {
	ODI_SW_DSF_ALLOC_CAM_CTL_OFF, ODI_SW_DSF_ALLOC_CAM_RDATA_OFF, ODI_GPON_INIT_ALLOC_ROWS, 0,
};

/* One CAM request: the control word with REQ clear, the same with REQ
 * set, then DONE polled with the bound of every CAM poll
 * (odi_switch_tbl.h). Returns 0 with the control word the poll last read
 * in *ctl, or -ETIMEDOUT. The caller holds odi_switch_dsf_lock.
 */
static int odi_gpon_cam_request(const struct odi_gpon_cam *cam, unsigned int row, uint32_t op,
				uint32_t *ctl)
{
	uint32_t ind;

	if (cam->gem) {
		ind = ODI_SW_DSF_GEM_CAM_CTL_OP_SET(0, op);
		ind = ODI_SW_DSF_GEM_CAM_CTL_CAM_ROW_SET(ind, row);
		odi_reg_write(cam->ctl, ind);
		odi_reg_write(cam->ctl, ODI_SW_DSF_GEM_CAM_CTL_REQ_SET(ind, 1));
		return odi_poll_reg(cam->ctl, ODI_SW_DSF_GEM_CAM_CTL_DONE, ODI_SW_DSF_GEM_CAM_CTL_DONE,
				    ODI_SW_TABLE_POLL_DELAY_US, ODI_SW_TABLE_POLL_US, ctl);
	}
	ind = ODI_SW_DSF_ALLOC_CAM_CTL_OP_SET(0, op);
	ind = ODI_SW_DSF_ALLOC_CAM_CTL_CAM_ROW_SET(ind, row);
	odi_reg_write(cam->ctl, ind);
	odi_reg_write(cam->ctl, ODI_SW_DSF_ALLOC_CAM_CTL_REQ_SET(ind, 1));
	return odi_poll_reg(cam->ctl, ODI_SW_DSF_ALLOC_CAM_CTL_DONE, ODI_SW_DSF_ALLOC_CAM_CTL_DONE,
			    ODI_SW_TABLE_POLL_DELAY_US, ODI_SW_TABLE_POLL_US, ctl);
}

/* Reads every row of one CAM. Returns the rows found valid; *row and *val
 * get the first of them (row -1 when none), seen[] the values of the first
 * ODI_GPON_INIT_SEEN_MAX. A request that never finishes ends the walk: the
 * block is not answering, and every further row would cost a whole poll
 * bound with interrupts off.
 */
static unsigned int odi_gpon_cam_count(const struct odi_gpon_cam *cam, int *row, uint32_t *val,
				       uint32_t *seen, int *timed_out)
{
	/* ROW_VALID is bit 13 in both control words. */
	const uint32_t valid = cam->gem ? ODI_SW_DSF_GEM_CAM_CTL_ROW_VALID :
					  ODI_SW_DSF_ALLOC_CAM_CTL_ROW_VALID;
	unsigned int r, n = 0;
	uint32_t ctl;

	*row = -1;
	*val = 0;
	for (r = 0; r < cam->rows; r++) {
		if (odi_gpon_cam_request(cam, r, ODI_GPON_CAM_OP_READ, &ctl)) {
			*timed_out = 1;
			break;
		}
		if (!(ctl & valid))
			continue;
		if (n < ODI_GPON_INIT_SEEN_MAX) {
			uint32_t d = odi_reg_read(cam->rdata);

			seen[n] = cam->gem ? ODI_SW_DSF_GEM_CAM_RDATA_GEM_PORT_GET(d) :
					     ODI_SW_DSF_ALLOC_CAM_RDATA_ALLOC_ID_GET(d);
			if (*row < 0) {
				*row = (int)r;
				*val = seen[n];
			}
		}
		n++;
	}
	return n;
}

/* One multicast filter request, the CAM handshake on DS_MC_FILTER_CTL.
 * Same returns as odi_gpon_cam_request().
 */
static int odi_gpon_mc_request(unsigned int row, uint32_t op, uint32_t *ctl)
{
	uint32_t ind;

	ind = ODI_GPON_DS_MC_FILTER_CTL_OP_SET(0, op);
	ind = ODI_GPON_DS_MC_FILTER_CTL_ROW_SET(ind, row);
	odi_reg_write(ODI_GPON_DS_MC_FILTER_CTL_OFF, ind);
	odi_reg_write(ODI_GPON_DS_MC_FILTER_CTL_OFF, ind | ODI_GPON_DS_MC_FILTER_CTL_REQ);
	return odi_poll_reg(ODI_GPON_DS_MC_FILTER_CTL_OFF, ODI_GPON_DS_MC_FILTER_CTL_DONE,
			    ODI_GPON_DS_MC_FILTER_CTL_DONE, ODI_SW_TABLE_POLL_DELAY_US,
			    ODI_SW_TABLE_POLL_US, ctl);
}

/* Reads every multicast filter row, as odi_gpon_cam_count() does a CAM. */
static void odi_gpon_mc_count(struct odi_gpon_table_count *c)
{
	unsigned int r;
	uint32_t ctl;

	c->mc_valid = 0;
	c->mc_row = -1;
	c->mc_data = 0;
	for (r = 0; r < ODI_GPON_INIT_MC_ROWS; r++) {
		if (odi_gpon_mc_request(r, ODI_GPON_CAM_OP_READ, &ctl)) {
			c->timed_out = 1;
			break;
		}
		if (!(ctl & ODI_GPON_DS_MC_FILTER_CTL_ROW_VALID))
			continue;
		if (c->mc_row < 0) {
			c->mc_row = (int)r;
			c->mc_data = odi_reg_read(ODI_GPON_DS_MC_FILTER_RDATA_OFF);
		}
		c->mc_valid++;
	}
}

/* Reads back every row of the four tables. The caller holds
 * odi_switch_dsf_lock.
 */
static void odi_gpon_tables_count(struct odi_gpon_table_count *c)
{
	unsigned int r;

	c->timed_out = 0;
	c->gem_valid = odi_gpon_cam_count(&odi_gpon_cam_gem, &c->gem_row, &c->gem_port,
					  c->gem_seen, &c->timed_out);
	c->alloc_valid = odi_gpon_cam_count(&odi_gpon_cam_alloc, &c->alloc_row, &c->alloc_id,
					    c->alloc_seen, &c->timed_out);
	c->flow_type_set = 0;
	for (r = 0; r < ODI_GPON_INIT_GEM_ROWS; r++)
		if (odi_reg_read(ODI_SW_DSF_GEM_FLOW_TYPE(r)))
			c->flow_type_set++;
	odi_gpon_mc_count(c);
}

/* One clean of the four tables. Returns 0, or -ETIMEDOUT at the first
 * request that never finished. The caller holds odi_switch_dsf_lock.
 */
static int odi_gpon_tables_clear(void)
{
	uint32_t ctl;
	unsigned int r;

	for (r = 0; r < ODI_GPON_INIT_GEM_ROWS; r++)
		odi_reg_write(ODI_SW_DSF_GEM_FLOW_TYPE(r), 0U);
	for (r = 0; r < ODI_GPON_INIT_GEM_ROWS; r++)
		if (odi_gpon_cam_request(&odi_gpon_cam_gem, r, ODI_GPON_CAM_OP_CLEAR, &ctl))
			return -ETIMEDOUT;
	for (r = 0; r < ODI_GPON_INIT_ALLOC_ROWS; r++)
		if (odi_gpon_cam_request(&odi_gpon_cam_alloc, r, ODI_GPON_CAM_OP_CLEAR, &ctl))
			return -ETIMEDOUT;
	for (r = 0; r < ODI_GPON_INIT_MC_ROWS; r++)
		if (odi_gpon_mc_request(r, ODI_GPON_CAM_OP_CLEAR, &ctl))
			return -ETIMEDOUT;
	return 0;
}

static int odi_gpon_tables_are_clear(const struct odi_gpon_table_count *c)
{
	return !c->timed_out && !c->gem_valid && !c->alloc_valid && !c->flow_type_set &&
	       !c->mc_valid;
}

/* The multicast filter self-test words, read once; nothing here writes
 * them.
 */
static void odi_gpon_mc_bist_read(struct odi_gpon_clean_report *r)
{
	const uint32_t fail = ODI_GPON_DS_MC_FILTER_BIST_BIST_FAIL |
			      ODI_GPON_DS_MC_FILTER_BIST_DRF_BIST_FAIL;
	unsigned int i;

	r->mc_bist_bad = 0;
	for (i = 0; i < ODI_GPON_INIT_MC_BIST_WORDS; i++) {
		r->mc_bist[i] = odi_reg_read(ODI_GPON_DS_MC_FILTER_BIST(i));
		if ((r->mc_bist[i] & fail) ||
		    !(r->mc_bist[i] & ODI_GPON_DS_MC_FILTER_BIST_ALL_BIST_DONE))
			r->mc_bist_bad++;
	}
}

int odi_gpon_init_clean_tables(struct odi_gpon_clean_report *r)
{
	unsigned long flags;
	int rc = 0;

	memset(r, 0, sizeof(*r));
	odi_gpon_mc_bist_read(r);
	spin_lock_irqsave(&odi_switch_dsf_lock, flags);
	odi_gpon_tables_count(&r->before);
	spin_unlock_irqrestore(&odi_switch_dsf_lock, flags);

	for (r->passes = 1; r->passes <= 2U; r->passes++) {
		spin_lock_irqsave(&odi_switch_dsf_lock, flags);
		rc = odi_gpon_tables_clear();
		odi_gpon_tables_count(&r->after);
		spin_unlock_irqrestore(&odi_switch_dsf_lock, flags);
		if (rc == 0 && r->after.timed_out)
			rc = -ETIMEDOUT;
		if (rc == 0 && !odi_gpon_tables_are_clear(&r->after))
			rc = -EIO;
		if (rc == 0 || r->passes == 2U)
			break;
		ODI_GPON_INIT_ERR("GPON tables not clean after pass 1 (%d): GEM-port CAM %u/%u valid (row %d port %u), Alloc-ID CAM %u/%u valid (row %d id %u), flow type %u/%u set, multicast filter %u/%u valid (row %d); cleaning again\n",
				  rc, r->after.gem_valid, ODI_GPON_INIT_GEM_ROWS, r->after.gem_row,
				  (unsigned int)r->after.gem_port, r->after.alloc_valid,
				  ODI_GPON_INIT_ALLOC_ROWS, r->after.alloc_row,
				  (unsigned int)r->after.alloc_id, r->after.flow_type_set,
				  ODI_GPON_INIT_GEM_ROWS, r->after.mc_valid, ODI_GPON_INIT_MC_ROWS,
				  r->after.mc_row);
	}
	r->rc = rc;
	return rc;
}

/* ---- the replay ---------------------------------------------------------- */

/* Records [from, from + n) of a table, as a table of their own. */
static struct odi_replay_blob odi_gpon_init_slice(const struct odi_replay_blob *table,
						  uint32_t from, uint32_t n)
{
	struct odi_replay_blob s = *table;

	if (s.events)
		s.events += from;
	else
		s.records += (size_t)from * ODI_REPLAY_BLOB_RECORD_SIZE;
	s.count = n;
	return s;
}

/* The record that ends the RESET_REQ pulse: the first PONMAC_RESET write
 * with RESET_REQ clear after one with it set. table->count when there is
 * none.
 */
static uint32_t odi_gpon_init_reset_release(const struct odi_replay_blob *table)
{
	struct odi_replay_event e;
	int pulsed = 0;
	uint32_t i;

	for (i = 0; i < table->count; i++) {
		odi_replay_blob_event(table, i, &e);
		if (e.kind != ODI_REPLAY_REG || e.offset != ODI_GPON_PONMAC_RESET_OFF)
			continue;
		if (ODI_GPON_PONMAC_RESET_RESET_REQ_GET(e.value))
			pulsed = 1;
		else if (pulsed)
			return i;
	}
	return table->count;
}

/* "a b c" for the first values of a census, "+n" for the rest, "none"
 * when there are none.
 */
size_t odi_gpon_init_seen_str(char *buf, size_t len, const uint32_t *seen, unsigned int valid)
{
	unsigned int i, shown = valid < ODI_GPON_INIT_SEEN_MAX ? valid : ODI_GPON_INIT_SEEN_MAX;
	size_t used = 0;
	int n;

	buf[0] = '\0';
	if (!valid)
		return (size_t)snprintf(buf, len, "none");
	for (i = 0; i < shown && used < len; i++) {
		n = snprintf(buf + used, len - used, "%s%u", i ? " " : "", (unsigned int)seen[i]);
		if (n < 0)
			break;
		used += (size_t)n;
	}
	if (valid > shown && used < len) {
		n = snprintf(buf + used, len - used, " +%u", valid - shown);
		if (n > 0)
			used += (size_t)n;
	}
	return used < len ? used : len - 1;
}

static void odi_gpon_init_clean_log(const struct odi_gpon_clean_report *r, int rc)
{
	const struct odi_gpon_table_count *b = &r->before, *a = &r->after;
	char gem[96], alloc[96];

	ODI_GPON_INIT_INFO("GPON tables before the clean: GEM-port CAM %u/%u valid, Alloc-ID CAM %u/%u valid, flow type %u/%u set%s\n",
			   b->gem_valid, ODI_GPON_INIT_GEM_ROWS, b->alloc_valid,
			   ODI_GPON_INIT_ALLOC_ROWS, b->flow_type_set, ODI_GPON_INIT_GEM_ROWS,
			   b->timed_out ? " (a CAM read timed out)" : "");
	if (b->gem_valid || b->alloc_valid) {
		odi_gpon_init_seen_str(gem, sizeof(gem), b->gem_seen, b->gem_valid);
		odi_gpon_init_seen_str(alloc, sizeof(alloc), b->alloc_seen, b->alloc_valid);
		ODI_GPON_INIT_INFO("GPON tables before the clean held GEM ports %s; Alloc-IDs %s\n",
				   gem, alloc);
	}
	ODI_GPON_INIT_INFO("GPON multicast filter before the clean: %u/%u rows valid (row %d 0x%08x); self-test words %08x %08x %08x %08x %08x %08x %08x %08x%s\n",
			   b->mc_valid, ODI_GPON_INIT_MC_ROWS, b->mc_row, (unsigned int)b->mc_data,
			   (unsigned int)r->mc_bist[0], (unsigned int)r->mc_bist[1],
			   (unsigned int)r->mc_bist[2], (unsigned int)r->mc_bist[3],
			   (unsigned int)r->mc_bist[4], (unsigned int)r->mc_bist[5],
			   (unsigned int)r->mc_bist[6], (unsigned int)r->mc_bist[7],
			   r->mc_bist_bad ? " (a self-test failed or never ran)" : "");
	if (rc == 0) {
		ODI_GPON_INIT_INFO("GPON tables clean after %u pass(es): GEM-port CAM 0/%u, Alloc-ID CAM 0/%u, flow type 0/%u, multicast filter 0/%u\n",
				   r->passes, ODI_GPON_INIT_GEM_ROWS, ODI_GPON_INIT_ALLOC_ROWS,
				   ODI_GPON_INIT_GEM_ROWS, ODI_GPON_INIT_MC_ROWS);
		return;
	}
	ODI_GPON_INIT_ERR("GPON tables NOT clean after %u passes (%d): GEM-port CAM %u/%u valid (row %d port %u), Alloc-ID CAM %u/%u valid (row %d id %u), flow type %u/%u set, multicast filter %u/%u valid (row %d); the PON is NOT activated, a reboot or another gponact tries again\n",
			  r->passes, rc, a->gem_valid, ODI_GPON_INIT_GEM_ROWS, a->gem_row,
			  (unsigned int)a->gem_port, a->alloc_valid, ODI_GPON_INIT_ALLOC_ROWS,
			  a->alloc_row, (unsigned int)a->alloc_id, a->flow_type_set,
			  ODI_GPON_INIT_GEM_ROWS, a->mc_valid, ODI_GPON_INIT_MC_ROWS, a->mc_row);
}

static struct odi_gpon_clean_report odi_gpon_init_report;

const struct odi_gpon_clean_report *odi_gpon_init_last_clean(void)
{
	return &odi_gpon_init_report;
}

int odi_gpon_init_apply(const struct odi_replay_blob *table,
			const uint8_t serial_number[8], const uint8_t password[10])
{
	const struct odi_replay_opts opts = {
		.rmw = 0,
		.verb = ODI_REPLAY_ALL_VERBS,
		.value = odi_gpon_init_value,
		.ctx = serial_number,
	};
	struct odi_gpon_clean_report *report = &odi_gpon_init_report;
	struct odi_replay_blob part;
	uint32_t rel = odi_gpon_init_reset_release(table);
	uint32_t rest = 0;	/* the first record replayed after the clean */
	unsigned int polls = 0;
	int reset_rc = 1;

	(void)password;
	if (rel < table->count) {
		rest = rel + 1U;
		part = odi_gpon_init_slice(table, 0, rest);
		(void)odi_replay_run(&part, &opts);
		reset_rc = odi_gpon_init_reset_wait(&polls);
		if (reset_rc)
			ODI_GPON_INIT_ERR("GPON MAC reset not complete after %u reads (PONMAC_RESET 0x%08x); cleaning the tables anyway\n",
					  polls, (unsigned int)odi_reg_read(ODI_GPON_PONMAC_RESET_OFF));
		else
			ODI_GPON_INIT_INFO("GPON MAC reset complete after %u reads\n", polls);
	} else {
		ODI_GPON_INIT_INFO("boot init table has no GPON MAC reset; cleaning the tables before it\n");
	}
	(void)odi_gpon_init_clean_tables(report);
	report->reset_rc = reset_rc;
	report->reset_polls = polls;
	odi_gpon_init_clean_log(report, report->rc);
	/* A row the clean could not clear may hold the Alloc-ID of another
	 * ONU, and an activated ONU answers the grants of every Alloc-ID in
	 * its CAM. So the rest of the table, which moves the ONU out of O1
	 * and arms its serial-number answer, runs on clean tables only. What
	 * was replayed so far wrote nothing inside the GPON MAC but the reset
	 * pulse.
	 */
	if (report->rc)
		return report->rc;
	part = odi_gpon_init_slice(table, rest, table->count - rest);
	(void)odi_replay_run(&part, &opts);
	return 0;
}

/* The serial-number slot write of the boot table, replayed alone: the run
 * of records from the USF_PLOAM_TX_CTL write that opens the slot to the
 * one that arms it again, holding the six message words between them.
 * Found by the sn_word records inside it, so it follows the table if a
 * regeneration moves it. The other records of the table are not touched.
 */
unsigned int odi_gpon_init_apply_serial(const struct odi_replay_blob *table,
					const uint8_t serial_number[8])
{
	const struct odi_replay_opts opts = {
		.rmw = 0,
		.verb = ODI_REPLAY_ALL_VERBS,
		.value = odi_gpon_init_value,
		.ctx = serial_number,
	};
	struct odi_replay_event e;
	struct odi_replay_blob slot = *table;
	uint32_t i, first = table->count, last = 0, from, to;

	for (i = 0; i < table->count; i++) {
		odi_replay_blob_event(table, i, &e);
		if (!e.sn_word)
			continue;
		if (first == table->count)
			first = i;
		last = i;
	}
	if (first == table->count)
		return 0;
	for (from = first; from > 0; from--) {
		odi_replay_blob_event(table, from - 1U, &e);
		if (e.kind == ODI_REPLAY_REG && e.offset == GPON_INIT_SLOT_CTL_OFF)
			break;
	}
	if (from == 0)
		return 0;
	from--;
	for (to = last + 1U; to < table->count; to++) {
		odi_replay_blob_event(table, to, &e);
		if (e.kind == ODI_REPLAY_REG && e.offset == GPON_INIT_SLOT_CTL_OFF)
			break;
	}
	if (to == table->count)
		return 0;
	slot.records += (size_t)from * ODI_REPLAY_BLOB_RECORD_SIZE;
	slot.count = to - from + 1U;
	return odi_replay_run(&slot, &opts);
}
