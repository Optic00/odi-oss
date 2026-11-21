/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * odi_gpon_init.h -- the GPON boot-init replay: the writes the
 * gpondrv/gpondev/gponsn/gponpw/gponact PON steps make, generated from a
 * boot capture by tools/regtrace/mkgponinit.py into
 * /lib/firmware/odi/gpon_init.bin (odi_replay_blob.h has the format;
 * regenerate rather than hand-edit). The table covers the whole
 * switch-core address space, not only the GPON MAC window
 * 0x700000-0x706fff: the steps also write plain switch-core registers such
 * as MAX_FRAME_LEN_1 and PORT_MAX_FRAME_SEL.
 *
 * A register record is a plain write, with no read before it; a table
 * record is one switch-core table row.
 */
#ifndef ODI_GPON_INIT_H
#define ODI_GPON_INIT_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stddef.h>
#include <stdint.h>
#endif

struct odi_replay_blob;	/* odi_replay_blob.h */

/* Replays a parsed gpon_init.bin in capture order. A register record with
 * an sn_word (1..4) writes that word of this board serial number instead
 * of the captured value: the Serial_Number_ONU content the gponsn step
 * writes is six words, word 0 the fixed 0xff01 ONU id and type header,
 * words 1..4 serial_number[0..7] two bytes each, big-endian, and word 5
 * the fixed 0x0005 trailer. password is unused: the capture had an empty
 * password, so its gponpw step wrote nothing.
 *
 * The table pulses PONMAC_RESET.RESET_REQ early on. Right after the
 * record that releases it, the replay stops, waits for RESET_COMPLETE
 * (odi_gpon_init_reset_wait()) and cleans the GPON MAC tables
 * (odi_gpon_init_clean_tables()), then goes on with the rest of the
 * table, which programs this ONU and activates it. A table without that
 * pulse gets the clean before its first record. Both steps log one line.
 *
 * Returns 0 once the whole table is replayed. When the clean fails, the
 * rest of the table is not replayed and the return is what the clean
 * returned (-EIO, -ETIMEDOUT): the ONU is not activated, since a row left
 * in the Alloc-ID CAM makes it answer grants meant for other ONUs on the
 * same PON. The records up to the reset release have run by then; none of
 * them is in the GPON MAC window but the reset pulse itself, and a later
 * call replays them again from the first. One of them sets MAX_FRAME_LEN_1,
 * the frame length the PON port accepts; it is written back to 0 (every
 * frame dropped, as on a deactivated ONU) before the return.
 *
 * No allocation, no sleeping: odi_gpon.c calls this with its spinlock
 * held, on a table it loaded before taking the lock.
 */
int odi_gpon_init_apply(const struct odi_replay_blob *table,
			const uint8_t serial_number[8], const uint8_t password[10]);

/* Writes a new serial number into the PLOAM slot the boot replay armed,
 * after boot: only the run of records that arms that slot, not the whole
 * table. The hardware answers the OLT ranging with what this slot holds, so
 * a new serial number reaches the line only through it. Returns the
 * records applied, 0 when the table has no such run. Same locking rules as
 * odi_gpon_init_apply().
 */
unsigned int odi_gpon_init_apply_serial(const struct odi_replay_blob *table,
					const uint8_t serial_number[8]);

/* ---- the GPON MAC tables at boot -------------------------------------
 *
 * The GEM-port CAM, the Alloc-ID CAM and the DSF_GEM_FLOW_TYPE words are
 * on-chip RAM. A warm reset keeps whatever the last session left in them;
 * a power-up fills them with whatever the RAM came up with, which on this
 * SoC reads as nearly every row valid with a random port or Alloc-ID. A
 * row like that makes the ONU answer bandwidth grants meant for other
 * ONUs, and the OLT deactivates it. So every boot clears every row of the
 * three and reads every row back, before the first row of ours is
 * programmed and before activation.
 *
 * The downstream multicast MAC filter is the same kind of RAM, and the
 * boot replay sets it up to be consulted for downstream multicast (no
 * IPv4 or IPv6 force pass), so a leftover row could change which
 * downstream multicast groups get through. The boot table
 * clears its 256 rows itself, back to back with no wait; the clean clears
 * them too, one request at a time and waited for, and reads them back.
 * Its self-test words are read for the log and never written.
 */
#define ODI_GPON_INIT_GEM_ROWS		128U	/* GEM-port CAM rows, and DSF_GEM_FLOW_TYPE words */
#define ODI_GPON_INIT_ALLOC_ROWS	32U	/* Alloc-ID CAM rows */
#define ODI_GPON_INIT_SEEN_MAX		8U	/* row values kept per CAM for the log */
#define ODI_GPON_INIT_MC_ROWS		256U	/* downstream multicast filter rows */
#define ODI_GPON_INIT_MC_BIST_WORDS	8U	/* DS_MC_FILTER_BIST words */

/* What a read of every row found. */
struct odi_gpon_table_count {
	unsigned int gem_valid;		/* GEM-port CAM rows a read finds valid */
	unsigned int alloc_valid;	/* Alloc-ID CAM rows a read finds valid */
	unsigned int flow_type_set;	/* DSF_GEM_FLOW_TYPE words that are not zero */
	int gem_row;			/* the first valid GEM-port row, -1 if none */
	uint32_t gem_port;		/* the port that row holds */
	int alloc_row;			/* the first valid Alloc-ID row, -1 if none */
	uint32_t alloc_id;		/* the Alloc-ID that row holds */
	int timed_out;			/* a CAM request never finished: the counts stop there */
	/* The first ODI_GPON_INIT_SEEN_MAX values of the valid rows, in row
	 * order: what tells a previous session's own entries from power-up
	 * garbage in the log.
	 */
	uint32_t gem_seen[ODI_GPON_INIT_SEEN_MAX];
	uint32_t alloc_seen[ODI_GPON_INIT_SEEN_MAX];
	unsigned int mc_valid;		/* multicast filter rows a read finds valid */
	int mc_row;			/* the first of them, -1 if none */
	uint32_t mc_data;		/* what that row holds (DS_MC_FILTER_RDATA) */
};

struct odi_gpon_clean_report {
	struct odi_gpon_table_count before;	/* as found, before the first clean */
	struct odi_gpon_table_count after;	/* after the last clean */
	unsigned int passes;			/* clean passes run, 1 or 2 */
	int rc;					/* odi_gpon_init_clean_tables() */
	/* Filled by odi_gpon_init_apply() only: */
	int reset_rc;				/* odi_gpon_init_reset_wait(), or 1 when the table has no reset */
	unsigned int reset_polls;		/* the reads it took */
	/* DS_MC_FILTER_BIST as found before the clean, and the words with a
	 * FAIL bit set or ALL_BIST_DONE clear.
	 */
	uint32_t mc_bist[ODI_GPON_INIT_MC_BIST_WORDS];
	unsigned int mc_bist_bad;
};

/* Waits for PONMAC_RESET.RESET_COMPLETE after a RESET_REQ pulse, at most
 * ODI_GPON_INIT_RESET_POLLS reads 1 us apart. Returns 0 and the reads it
 * took in *polls, or -ETIMEDOUT. The stock driver, in a register capture
 * of its bring-up, read the register about a hundred times before the bit
 * was set: the bound is two orders of magnitude above that, and costs
 * about 20 ms with interrupts off only when the reset never finishes.
 */
#define ODI_GPON_INIT_RESET_POLLS	20000U
int odi_gpon_init_reset_wait(unsigned int *polls);

/* Clears every row of the GEM-port CAM, the Alloc-ID CAM and the
 * multicast filter (each a request with OP 3, polled for DONE) and writes
 * every DSF_GEM_FLOW_TYPE word to 0, then reads every row back (OP 2,
 * ROW_VALID) and every word.
 * A row still set is logged and the whole clean runs once more. Fills
 * *r and returns 0 when every row reads back clear, -EIO when some row
 * survived both passes, -ETIMEDOUT when a CAM request never finished.
 * Takes odi_switch_dsf_lock around each pass and logs only outside it.
 */
int odi_gpon_init_clean_tables(struct odi_gpon_clean_report *r);

/* The report of the last odi_gpon_init_apply(), all zero before the first.
 * Read under the same lock the apply ran under (odi_gpon_lock).
 */
const struct odi_gpon_clean_report *odi_gpon_init_last_clean(void);

/* Formats the values a census kept ("63 319 1087", "+n" for rows past
 * ODI_GPON_INIT_SEEN_MAX, "none" for no valid row) into buf; returns the
 * length written. For the boot log.
 */
size_t odi_gpon_init_seen_str(char *buf, size_t len, const uint32_t *seen, unsigned int valid);

#endif /* ODI_GPON_INIT_H */
