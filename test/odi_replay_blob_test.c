/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * odi_replay_blob_test.c -- the register-replay firmware blobs
 * (odi_replay_blob.h): the three files the image ships under
 * /lib/firmware/odi/, as built into out/firmware/odi/ from
 * tools/regtrace/tables/, parse with the record counts the replay tests
 * pin, odi_replay_blob_parse() refuses every defect it is meant to
 * (header fields, size, CRC, per-record rules, each checked with a CRC
 * that is otherwise valid so the rule itself is what refuses it), a
 * missing file fails the load, and odi_gpon_init_apply() replays
 * gpon_init.bin with the serial-number words substituted. The other two
 * tables are replayed against their captures by
 * odi_switch_sdkinit_replay_test.sh and odi_switch_modload_replay_test.sh.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "odi_switch_unity.h"
#include "odi_replay_fw_host.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_gpon_init.c"

/* The record counts of the shipped tables. */
#define SDKINIT_RECORDS		1801U
#define MODLOAD_RECORDS		725U
#define GPON_INIT_RECORDS	800U

/* gponsn: the four serial-number words, USF_PLOAM_TX_WORD[1..4]. */
#define GPON_SN_WORD1_OFF	0x007050e4U
#define GPON_SN_WORDS		4U

static int failures;

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
		failures++; \
	} \
} while (0)

static void put_be16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static void put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

/* Recomputes the header CRC after a test edit, so the edit is refused by
 * the rule it breaks and not by the CRC.
 */
static void reseal(uint8_t *data, size_t size)
{
	uint32_t crc = odi_replay_crc32_update(~0U, data, ODI_REPLAY_HDR_CRC);

	crc = odi_replay_crc32_update(crc, data + ODI_REPLAY_BLOB_HEADER_SIZE,
				      size - ODI_REPLAY_BLOB_HEADER_SIZE);
	put_be32(data + ODI_REPLAY_HDR_CRC, crc ^ ~0U);
}

/* A private, writable copy of one shipped table. */
static uint8_t *copy_of(enum odi_replay_table table, size_t *size)
{
	struct odi_replay_fw fw;
	uint8_t *copy;

	if (odi_replay_fw_load(table, &fw))
		return NULL;
	*size = ODI_REPLAY_BLOB_HEADER_SIZE + (size_t)fw.blob.count * ODI_REPLAY_BLOB_RECORD_SIZE;
	copy = malloc(*size);
	if (copy)
		memcpy(copy, fw.blob.records - ODI_REPLAY_BLOB_HEADER_SIZE, *size);
	odi_replay_fw_release(&fw);
	return copy;
}

static int parse(const uint8_t *data, size_t size, enum odi_replay_table table, const char **why)
{
	struct odi_replay_blob blob;

	*why = "";
	return odi_replay_blob_parse(data, size, table, &blob, why);
}

static uint8_t *record(uint8_t *data, uint32_t i)
{
	return data + ODI_REPLAY_BLOB_HEADER_SIZE + (size_t)i * ODI_REPLAY_BLOB_RECORD_SIZE;
}

static void test_shipped_tables_parse(void)
{
	struct odi_replay_fw fw;

	CHECK(odi_replay_fw_load(ODI_REPLAY_TABLE_SDKINIT, &fw) == 0 &&
	      fw.blob.count == SDKINIT_RECORDS, "sdkinit.bin parses, 1801 records");
	odi_replay_fw_release(&fw);
	CHECK(odi_replay_fw_load(ODI_REPLAY_TABLE_MODLOAD, &fw) == 0 &&
	      fw.blob.count == MODLOAD_RECORDS, "modload.bin parses, 725 records");
	odi_replay_fw_release(&fw);
	CHECK(odi_replay_fw_load(ODI_REPLAY_TABLE_GPON_INIT, &fw) == 0 &&
	      fw.blob.count == GPON_INIT_RECORDS, "gpon_init.bin parses, 800 records");
	odi_replay_fw_release(&fw);
	CHECK(odi_replay_fw_host_loads == odi_replay_fw_host_releases, "every load was released");
}

static void test_missing_file(void)
{
	struct odi_replay_fw fw;

	odi_replay_fw_host_dir = "/nonexistent-odi-replay-fw-dir";
	CHECK(odi_replay_fw_load(ODI_REPLAY_TABLE_MODLOAD, &fw) == -ENOENT,
	      "a missing modload.bin fails the load with -ENOENT");
	CHECK(fw.priv == NULL && fw.blob.count == 0, "a failed load leaves nothing to release");
	odi_replay_fw_host_dir = NULL;
}

static void test_header_defects(void)
{
	size_t size;
	uint8_t *d = copy_of(ODI_REPLAY_TABLE_MODLOAD, &size);
	const char *why;

	CHECK(d != NULL, "test setup: a copy of modload.bin");
	if (!d)
		return;
	CHECK(parse(d, size, ODI_REPLAY_TABLE_MODLOAD, &why) == 0, "the untouched copy parses");

	CHECK(parse(d, size, ODI_REPLAY_TABLE_SDKINIT, &why) == -EINVAL,
	      "modload.bin is refused as the sdkinit table");
	CHECK(parse(d, size - 1U, ODI_REPLAY_TABLE_MODLOAD, &why) == -EINVAL,
	      "a truncated file is refused");
	CHECK(parse(d, ODI_REPLAY_BLOB_HEADER_SIZE - 1U, ODI_REPLAY_TABLE_MODLOAD, &why) == -EINVAL,
	      "a file shorter than the header is refused");

	d[ODI_REPLAY_BLOB_HEADER_SIZE + 9U] ^= 0x01U;
	CHECK(parse(d, size, ODI_REPLAY_TABLE_MODLOAD, &why) == -EBADMSG,
	      "one flipped record bit fails the crc32");
	d[ODI_REPLAY_BLOB_HEADER_SIZE + 9U] ^= 0x01U;

	d[ODI_REPLAY_HDR_MAGIC] ^= 0x01U;
	reseal(d, size);
	CHECK(parse(d, size, ODI_REPLAY_TABLE_MODLOAD, &why) == -EINVAL, "a bad magic is refused");
	d[ODI_REPLAY_HDR_MAGIC] ^= 0x01U;

	put_be16(d + ODI_REPLAY_HDR_VERSION, ODI_REPLAY_BLOB_VERSION + 1U);
	reseal(d, size);
	CHECK(parse(d, size, ODI_REPLAY_TABLE_MODLOAD, &why) == -EINVAL,
	      "a newer version is refused");
	put_be16(d + ODI_REPLAY_HDR_VERSION, ODI_REPLAY_BLOB_VERSION);

	put_be16(d + ODI_REPLAY_HDR_RECORD_SIZE, ODI_REPLAY_BLOB_RECORD_SIZE + 4U);
	reseal(d, size);
	CHECK(parse(d, size, ODI_REPLAY_TABLE_MODLOAD, &why) == -EINVAL,
	      "a different record size is refused");
	put_be16(d + ODI_REPLAY_HDR_RECORD_SIZE, ODI_REPLAY_BLOB_RECORD_SIZE);

	put_be32(d + ODI_REPLAY_HDR_COUNT, MODLOAD_RECORDS - 1U);
	reseal(d, size);
	CHECK(parse(d, size, ODI_REPLAY_TABLE_MODLOAD, &why) == -EINVAL,
	      "a count that disagrees with the size is refused");
	put_be32(d + ODI_REPLAY_HDR_COUNT, 0xffffffffU);
	reseal(d, size);
	CHECK(parse(d, size, ODI_REPLAY_TABLE_MODLOAD, &why) == -EINVAL,
	      "a huge count is refused before it is multiplied");
	put_be32(d + ODI_REPLAY_HDR_COUNT, MODLOAD_RECORDS);

	put_be32(d + ODI_REPLAY_HDR_RESERVED, 1U);
	reseal(d, size);
	CHECK(parse(d, size, ODI_REPLAY_TABLE_MODLOAD, &why) == -EINVAL,
	      "a nonzero reserved header field is refused");
	put_be32(d + ODI_REPLAY_HDR_RESERVED, 0U);

	reseal(d, size);
	CHECK(parse(d, size, ODI_REPLAY_TABLE_MODLOAD, &why) == 0, "every edit above was undone");
	free(d);
}

/* Breaks one field of record i, checks the table refuses it, restores it. */
static void expect_record_refused(uint8_t *d, size_t size, enum odi_replay_table table,
				  uint32_t i, unsigned int byte, uint8_t value, const char *msg)
{
	uint8_t *rec = record(d, i);
	uint8_t old = rec[byte];
	const char *why;

	rec[byte] = value;
	reseal(d, size);
	CHECK(parse(d, size, table, &why) == -EINVAL, msg);
	rec[byte] = old;
	reseal(d, size);
}

/* The index of the first record of a kind, or count when there is none. */
static uint32_t first_of_kind(uint8_t *d, uint32_t count, uint8_t kind)
{
	uint32_t i;

	for (i = 0; i < count; i++)
		if (record(d, i)[ODI_REPLAY_REC_KIND] == kind)
			break;
	return i;
}

static void test_record_rules(void)
{
	size_t size;
	uint8_t *d;
	uint32_t reg, tbl;

	d = copy_of(ODI_REPLAY_TABLE_MODLOAD, &size);
	CHECK(d != NULL, "test setup: a copy of modload.bin");
	if (d) {
		reg = first_of_kind(d, MODLOAD_RECORDS, ODI_REPLAY_REG);
		tbl = first_of_kind(d, MODLOAD_RECORDS, ODI_REPLAY_TABLE);
		CHECK(reg < MODLOAD_RECORDS && tbl < MODLOAD_RECORDS,
		      "test setup: modload.bin has register and table records");
		expect_record_refused(d, size, ODI_REPLAY_TABLE_MODLOAD, reg, ODI_REPLAY_REC_KIND,
				      ODI_REPLAY_SOC, "modload: a SoC-window record is refused");
		expect_record_refused(d, size, ODI_REPLAY_TABLE_MODLOAD, reg, ODI_REPLAY_REC_REG_GROUP,
				      ODI_REPLAY_BLOB_MODLOAD_MAX_REG_GROUP + 1U,
				      "modload: a reg_group past bit 31 is refused");
		expect_record_refused(d, size, ODI_REPLAY_TABLE_MODLOAD, reg, ODI_REPLAY_REC_VERB, 1U,
				      "modload: a record with a verb is refused");
		expect_record_refused(d, size, ODI_REPLAY_TABLE_MODLOAD, reg, ODI_REPLAY_REC_WORDS + 3U, 1U,
				      "modload: a register record with words is refused");
		expect_record_refused(d, size, ODI_REPLAY_TABLE_MODLOAD, tbl, ODI_REPLAY_REC_CATEGORY, 0U,
				      "modload: a table record in category 0 is refused");
		expect_record_refused(d, size, ODI_REPLAY_TABLE_MODLOAD, tbl, ODI_REPLAY_REC_N_WORDS + 1U,
				      ODI_REPLAY_BLOB_MAX_WORDS + 1U,
				      "modload: a table row wider than five words is refused");
		expect_record_refused(d, size, ODI_REPLAY_TABLE_MODLOAD, tbl, ODI_REPLAY_REC_N_WORDS + 1U, 0U,
				      "modload: a table row with no words is refused");
		free(d);
	}

	d = copy_of(ODI_REPLAY_TABLE_SDKINIT, &size);
	CHECK(d != NULL, "test setup: a copy of sdkinit.bin");
	if (d) {
		expect_record_refused(d, size, ODI_REPLAY_TABLE_SDKINIT, SDKINIT_RECORDS - 1U,
				      ODI_REPLAY_REC_VERB, ODI_SDKINIT_VERB_ID_COUNT,
				      "sdkinit: a verb past the enum is refused");
		expect_record_refused(d, size, ODI_REPLAY_TABLE_SDKINIT, SDKINIT_RECORDS - 1U,
				      ODI_REPLAY_REC_VERB, 0U,
				      "sdkinit: a record out of verb order is refused");
		expect_record_refused(d, size, ODI_REPLAY_TABLE_SDKINIT, 0U, ODI_REPLAY_REC_CATEGORY, 1U,
				      "sdkinit: a record with a category is refused");
		expect_record_refused(d, size, ODI_REPLAY_TABLE_SDKINIT, 0U, ODI_REPLAY_REC_KIND, 4U,
				      "sdkinit: an unknown kind is refused");
		free(d);
	}

	d = copy_of(ODI_REPLAY_TABLE_GPON_INIT, &size);
	CHECK(d != NULL, "test setup: a copy of gpon_init.bin");
	if (d) {
		expect_record_refused(d, size, ODI_REPLAY_TABLE_GPON_INIT, 0U, ODI_REPLAY_REC_SN_WORD,
				      ODI_REPLAY_BLOB_GPON_SN_WORD_MAX + 1U,
				      "gpon init: an sn_word past word 4 is refused");
		expect_record_refused(d, size, ODI_REPLAY_TABLE_GPON_INIT, 0U, ODI_REPLAY_REC_GPON_RESERVED, 1U,
				      "gpon init: a nonzero reserved field is refused");
		expect_record_refused(d, size, ODI_REPLAY_TABLE_GPON_INIT, 0U, ODI_REPLAY_REC_KIND,
				      ODI_REPLAY_SOC, "gpon init: a SoC-window record is refused");
		expect_record_refused(d, size, ODI_REPLAY_TABLE_GPON_INIT, 0U, ODI_REPLAY_REC_KIND,
				      ODI_REPLAY_CALL, "gpon init: a call record is refused");
		free(d);
	}
}

/* Sets the u32 at byte off of record i, checks the table refuses the
 * blob, restores it.
 */
static void expect_call_refused(uint8_t *d, size_t size, enum odi_replay_table table,
				uint32_t i, unsigned int off, uint32_t value, const char *msg)
{
	uint8_t *rec = record(d, i);
	uint32_t old = odi_replay_be32(rec + off);
	const char *why;

	put_be32(rec + off, value);
	reseal(d, size);
	CHECK(parse(d, size, table, &why) == -EINVAL, msg);
	put_be32(rec + off, old);
	reseal(d, size);
}

/* The call records of the shipped tables and the rules the parser holds
 * every call record to (odi_replay_check_call()).
 */
static void test_call_rules(void)
{
	size_t size;
	uint8_t *d;
	uint32_t call;
	const char *why;

	d = copy_of(ODI_REPLAY_TABLE_SDKINIT, &size);
	CHECK(d != NULL, "test setup: a copy of sdkinit.bin");
	if (d) {
		call = first_of_kind(d, SDKINIT_RECORDS, ODI_REPLAY_CALL);
		CHECK(call < SDKINIT_RECORDS, "sdkinit.bin has call records");
		if (call < SDKINIT_RECORDS) {
			/* The table field is the fill id, the offset the first
			 * row, the value the row count.
			 */
			expect_call_refused(d, size, ODI_REPLAY_TABLE_SDKINIT, call,
					    ODI_REPLAY_REC_TABLE, 0x00000000U, /* fill 0, 0 words */
					    "sdkinit: a call to fill 0 is refused");
			expect_call_refused(d, size, ODI_REPLAY_TABLE_SDKINIT, call,
					    ODI_REPLAY_REC_TABLE, 0x00040001U,
					    "sdkinit: a call to an unknown fill is refused");
			expect_call_refused(d, size, ODI_REPLAY_TABLE_SDKINIT, call,
					    ODI_REPLAY_REC_TABLE, 0x00010001U,
					    "sdkinit: a VLAN fill with an argument word is refused");
			expect_call_refused(d, size, ODI_REPLAY_TABLE_SDKINIT, call,
					    ODI_REPLAY_REC_TABLE, 0x00020000U,
					    "sdkinit: a CF fill without its pattern-1 row is refused");
			expect_call_refused(d, size, ODI_REPLAY_TABLE_SDKINIT, call,
					    ODI_REPLAY_REC_VALUE, 0U,
					    "sdkinit: a call over no rows is refused");
			expect_call_refused(d, size, ODI_REPLAY_TABLE_SDKINIT, call,
					    ODI_REPLAY_REC_VALUE, ODI_REPLAY_FILL_CF_ROW_COUNT + 1U,
					    "sdkinit: a call past the last row is refused");
			expect_call_refused(d, size, ODI_REPLAY_TABLE_SDKINIT, call,
					    ODI_REPLAY_REC_OFFSET, ODI_REPLAY_FILL_CF_ROW_COUNT,
					    "sdkinit: a call starting past the table is refused");
			expect_call_refused(d, size, ODI_REPLAY_TABLE_SDKINIT, call,
					    ODI_REPLAY_REC_OFFSET, 0xffffffffU,
					    "sdkinit: a first row that would wrap is refused");
			expect_call_refused(d, size, ODI_REPLAY_TABLE_SDKINIT, call,
					    ODI_REPLAY_REC_WORDS, ODI_REPLAY_FILL_CF_ROW_COUNT + 1U,
					    "sdkinit: a pattern-1 row past the table is refused");
			expect_call_refused(d, size, ODI_REPLAY_TABLE_SDKINIT, call,
					    ODI_REPLAY_REC_WORDS + 4U, 1U,
					    "sdkinit: a call with padding past its words is refused");
			expect_record_refused(d, size, ODI_REPLAY_TABLE_SDKINIT, call,
					      ODI_REPLAY_REC_CATEGORY, 1U,
					      "sdkinit: a call record with a category is refused");
		}
		CHECK(parse(d, size, ODI_REPLAY_TABLE_SDKINIT, &why) == 0, "sdkinit: every edit above was undone");
		free(d);
	}

	d = copy_of(ODI_REPLAY_TABLE_MODLOAD, &size);
	CHECK(d != NULL, "test setup: a copy of modload.bin");
	if (d) {
		call = first_of_kind(d, MODLOAD_RECORDS, ODI_REPLAY_CALL);
		CHECK(call < MODLOAD_RECORDS && record(d, call)[ODI_REPLAY_REC_CATEGORY] == 1U,
		      "modload.bin has a call record, in category 1");
		if (call < MODLOAD_RECORDS) {
			expect_record_refused(d, size, ODI_REPLAY_TABLE_MODLOAD, call,
					      ODI_REPLAY_REC_CATEGORY, 0U,
					      "modload: a call record in category 0 is refused");
			expect_record_refused(d, size, ODI_REPLAY_TABLE_MODLOAD, call,
					      ODI_REPLAY_REC_REG_GROUP, 1U,
					      "modload: a call record with a reg_group is refused");
			expect_call_refused(d, size, ODI_REPLAY_TABLE_MODLOAD, call,
					    ODI_REPLAY_REC_VALUE, 0U,
					    "modload: a call over no rows is refused");
		}
		CHECK(parse(d, size, ODI_REPLAY_TABLE_MODLOAD, &why) == 0, "modload: every edit above was undone");
		free(d);
	}
}

/* The table op entries of the mock log, from entry *i on: the next 'T'
 * (table, row), skipping its 'D' words. 0 when there is none.
 */
static int next_table_op(unsigned int *i, uint32_t *table, uint32_t *row)
{
	for (; *i < odi_mock.log_n; (*i)++) {
		if (odi_mock.log[*i].kind != 'T')
			continue;
		*table = odi_mock.log[*i].addr >> 16;
		*row = odi_mock.log[*i].addr & 0xffffU;
		(*i)++;
		return 1;
	}
	return 0;
}

static struct odi_replay_event call_event(uint16_t fill, uint32_t first, uint32_t count,
					  uint32_t pattern1)
{
	struct odi_replay_event e;

	memset(&e, 0, sizeof(e));
	e.kind = ODI_REPLAY_CALL;
	e.table = fill;
	e.offset = first;
	e.value = count;
	e.n_words = fill == ODI_REPLAY_FILL_VLAN_MEMBERS ? 0 : 1;
	e.words[0] = fill == ODI_REPLAY_FILL_VLAN_MEMBERS ? 0 : pattern1;
	return e;
}

/* What each fill writes, row by row: the replay tests fold a run of equal
 * rows into one entry, so the order inside a fill is pinned here.
 */
static void test_call_fills(void)
{
	static const uint32_t cf_rules[] = {
		ODI_SW_TBL_CLS_RULE_A, ODI_SW_TBL_CLS_MASK_A,	/* row 0 */
		ODI_SW_TBL_CLS_RULE_A, ODI_SW_TBL_CLS_MASK_A,	/* row 1 */
		ODI_SW_TBL_CLS_RULE_B, ODI_SW_TBL_CLS_MASK_B,	/* row 2, pattern 1 */
		ODI_SW_TBL_CLS_RULE_B, ODI_SW_TBL_CLS_MASK_B,	/* row 3 */
	};
	static const uint32_t cf_actions[] = {
		ODI_SW_TBL_CLS_RULE_A, ODI_SW_TBL_CLS_MASK_A,
		ODI_SW_TBL_CLS_DS_ACTION, ODI_SW_TBL_CLS_US_ACTION,	/* row 254 */
		ODI_SW_TBL_CLS_RULE_A, ODI_SW_TBL_CLS_MASK_A,
		ODI_SW_TBL_CLS_DS_ACTION, ODI_SW_TBL_CLS_US_ACTION,	/* row 255 */
	};
	struct odi_replay_event e;
	unsigned int i, k, ok;
	uint32_t table, row;

	odi_mock_reset();
	e = call_event(ODI_REPLAY_FILL_VLAN_MEMBERS, 1U, 4095U, 0);
	odi_switch_replay_call(&e, NULL);
	odi_mock_table_flush();
	/* One run: its first row and word, then the run length, which only
	 * grows while each row is the next one with the same word.
	 */
	CHECK(odi_mock.log_n == 3U && odi_mock.log[0].kind == 'T' &&
	      odi_mock.log[0].addr == ((ODI_SW_TBL_VLAN_MEMBERS << 16) | 1U) &&
	      odi_mock.log[1].kind == 'D' && odi_mock.log[1].val == 0xf0U &&
	      odi_mock.log[2].kind == 'R' && odi_mock.log[2].val == 4095U,
	      "vlan fill: rows 1 .. 0xfff to 0xf0, in order, one write each");

	odi_mock_reset();
	e = call_event(ODI_REPLAY_FILL_CF_RULES, 0, 4U, 2U);
	odi_switch_replay_call(&e, NULL);
	odi_mock_table_flush();
	ok = 1;
	i = 0;
	for (k = 0; k < sizeof(cf_rules) / sizeof(cf_rules[0]); k++)
		if (!next_table_op(&i, &table, &row) || table != cf_rules[k] || row != k / 2U)
			ok = 0;
	CHECK(ok && !next_table_op(&i, &table, &row),
	      "cf rules fill: rule then mask per row, A below the pattern-1 row, B from it");

	odi_mock_reset();
	e = call_event(ODI_REPLAY_FILL_CF_RULES_ACTIONS, 254U, 2U, ODI_REPLAY_FILL_CF_ROW_COUNT);
	odi_switch_replay_call(&e, NULL);
	odi_mock_table_flush();
	ok = 1;
	i = 0;
	for (k = 0; k < sizeof(cf_actions) / sizeof(cf_actions[0]); k++)
		if (!next_table_op(&i, &table, &row) || table != cf_actions[k] || row != 254U + k / 4U)
			ok = 0;
	CHECK(ok && !next_table_op(&i, &table, &row),
	      "cf rules and actions fill: rule, mask, DS and US action per row");

	odi_mock_reset();
	e = call_event(ODI_REPLAY_FILL_VLAN_MEMBERS, 4095U, 2U, 0);
	odi_switch_replay_call(&e, NULL);
	e = call_event(ODI_REPLAY_FILL_CF_RULES, 255U, 2U, 0);
	odi_switch_replay_call(&e, NULL);
	e = call_event(7U, 0, 1U, 0);
	odi_switch_replay_call(&e, NULL);
	odi_mock_table_flush();
	CHECK(odi_mock.log_n == 0U, "a fill past its table, or an unknown one, writes nothing");

	/* The engine without a call hook: the record counts, nothing is
	 * written (the board and GPON replays never see one; their parsers
	 * refuse it).
	 */
	{
		const struct odi_replay_event ev[1] = {
			call_event(ODI_REPLAY_FILL_VLAN_MEMBERS, 0, 16U, 0),
		};
		const struct odi_replay_blob blob = { .events = ev, .count = 1 };
		const struct odi_replay_opts opts = { .verb = ODI_REPLAY_ALL_VERBS };

		odi_mock_reset();
		CHECK(odi_replay_run(&blob, &opts) == 1U, "a call record counts as one applied record");
		odi_mock_table_flush();
		CHECK(odi_mock.log_n == 0U, "a call record with no hook writes nothing");
	}
}

/* gpon_init.bin through odi_gpon_init_apply(): one register write per
 * record (no table rows in this capture), in order, with the four
 * serial-number words carrying this box serial number, never the zeros
 * the public table holds. Right after the record that releases the GPON
 * MAC reset, the table clean (odi_gpon_init.h) adds its own writes: on
 * the plain mock register file every CAM row reads back clear, so one
 * pass, a read of every row before it and after it.
 */
#define GPON_INIT_RESET_RELEASE	361U	/* PONMAC_RESET = 0, the record before the clean */
#define GPON_CAM_ROWS		(ODI_GPON_INIT_GEM_ROWS + ODI_GPON_INIT_ALLOC_ROWS + \
				 ODI_GPON_INIT_MC_ROWS)
#define GPON_CLEAN_WRITES	(2U * GPON_CAM_ROWS + ODI_GPON_INIT_GEM_ROWS + 2U * GPON_CAM_ROWS + \
				 2U * GPON_CAM_ROWS)

static void test_gpon_init_apply(void)
{
	static const uint8_t sn[8] = { 'O', 'D', 'I', 'X', 0x12, 0x34, 0x56, 0x78 };
	static const uint8_t pw[10];
	struct odi_replay_fw fw;
	unsigned int i, w, rec, sn_seen = 0;

	if (odi_replay_fw_load(ODI_REPLAY_TABLE_GPON_INIT, &fw)) {
		CHECK(0, "test setup: gpon_init.bin loads");
		return;
	}
	odi_mock_reset();
	CHECK(odi_gpon_init_apply(&fw.blob, sn, pw) == 0, "gpon init: the apply reports success");
	odi_mock_table_flush();
	CHECK(odi_mock_locks_idle(), "gpon init: no lock left held");

	CHECK(odi_mock.log_n == GPON_INIT_RECORDS + GPON_CLEAN_WRITES,
	      "gpon init: one register write per record, plus the table clean");
	CHECK(odi_mock.log[GPON_INIT_RESET_RELEASE].addr == ODI_GPON_PONMAC_RESET_OFF &&
	      odi_mock.log[GPON_INIT_RESET_RELEASE].val == 0U,
	      "gpon init: the clean follows the reset release");
	for (i = GPON_INIT_RESET_RELEASE + 1U;
	     i < GPON_INIT_RESET_RELEASE + 1U + GPON_CLEAN_WRITES && i < odi_mock.log_n; i++) {
		uint32_t a = odi_mock.log[i].addr;

		CHECK(a == ODI_SW_DSF_GEM_CAM_CTL_OFF || a == ODI_SW_DSF_ALLOC_CAM_CTL_OFF ||
		      a == ODI_GPON_DS_MC_FILTER_CTL_OFF ||
		      (a >= ODI_SW_DSF_GEM_FLOW_TYPE(0) &&
		       a <= ODI_SW_DSF_GEM_FLOW_TYPE(ODI_GPON_INIT_GEM_ROWS - 1U)),
		      "gpon init: the clean writes only the request controls and the flow types");
	}
	for (i = 0; i < odi_mock.log_n; i++) {
		const struct odi_mock_write *e = &odi_mock.log[i];
		struct odi_replay_event ev;

		if (i > GPON_INIT_RESET_RELEASE && i <= GPON_INIT_RESET_RELEASE + GPON_CLEAN_WRITES)
			continue;
		rec = i > GPON_INIT_RESET_RELEASE ? i - GPON_CLEAN_WRITES : i;
		if (rec >= fw.blob.count)
			break;
		odi_replay_blob_event(&fw.blob, rec, &ev);
		CHECK(e->kind == 'W' && e->addr == ev.offset, "gpon init: writes follow the table order");
		if (ev.sn_word == 0) {
			CHECK(e->val == ev.value, "gpon init: a literal record writes its own value");
			continue;
		}
		w = ev.sn_word - 1U;
		CHECK(e->addr == GPON_SN_WORD1_OFF + 4U * w, "gpon init: sn words sit in USF_PLOAM_TX_WORD[1..4]");
		CHECK(e->val == (((uint32_t)sn[2U * w] << 8) | sn[2U * w + 1U]),
		      "gpon init: an sn word carries this box serial number");
		sn_seen++;
	}
	CHECK(sn_seen == GPON_SN_WORDS, "gpon init: all four serial-number words substituted");
	odi_replay_fw_release(&fw);
}

/* A serial number set after boot rewrites the armed slot and nothing else:
 * the six message words between the two USF_PLOAM_TX_CTL writes, the four
 * serial words carrying the new number. */
static void test_gpon_init_apply_serial(void)
{
	static const uint8_t sn[8] = { 'O', 'D', 'I', 'Y', 0xa1, 0xb2, 0xc3, 0xd4 };
	struct odi_replay_fw fw;
	unsigned int i;

	if (odi_replay_fw_load(ODI_REPLAY_TABLE_GPON_INIT, &fw)) {
		CHECK(0, "test setup: gpon_init.bin loads");
		return;
	}
	odi_mock_reset();
	CHECK(odi_gpon_init_apply_serial(&fw.blob, sn) == 8U,
	      "serial rearm: eight records, the CTL open, six words, the CTL arm");
	odi_mock_table_flush();
	CHECK(odi_mock.log_n == 8U, "serial rearm: eight register writes");
	if (odi_mock.log_n == 8U) {
		CHECK(odi_mock.log[0].addr == 0x7050c0U && odi_mock.log[0].val == 0x600U,
		      "serial rearm: opens the slot");
		CHECK(odi_mock.log[7].addr == 0x7050c0U && odi_mock.log[7].val == 0x601U,
		      "serial rearm: arms it");
		for (i = 0; i < 4U; i++)
			CHECK(odi_mock.log[2U + i].addr == GPON_SN_WORD1_OFF + 4U * i &&
			      odi_mock.log[2U + i].val ==
				      (((uint32_t)sn[2U * i] << 8) | sn[2U * i + 1U]),
			      "serial rearm: a serial word carries the new number");
	}
	odi_replay_fw_release(&fw);
}

int main(void)
{
	test_gpon_init_apply_serial();
	test_shipped_tables_parse();
	test_missing_file();
	test_header_defects();
	test_record_rules();
	test_call_rules();
	test_call_fills();
	test_gpon_init_apply();
	CHECK(odi_replay_fw_host_loads == odi_replay_fw_host_releases, "every load was released");

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("odi_replay_blob_test: ok\n");
	return 0;
}
