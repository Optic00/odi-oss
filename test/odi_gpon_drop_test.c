/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * odi_gpon_drop_test.c -- host test for odi_gpon_drop.c, the record frozen
 * around every drop out of O5.
 *
 * Covers: only the last PRE_MS of history goes into a record, oldest first,
 * with the state change itself at offset 0; events after the drop are added
 * until POST_MS has passed and not after; a second drop while a record is
 * collecting is counted in it; the first record of the boot is kept while
 * the later ones rotate through the other slots; a record that is full
 * counts what it lost; a wrapping millisecond clock; and the
 * /proc/odi_gpon/drops lines.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../kernel/extra/drivers/net/ethernet/odi/odi_gpon_drop.c"

static int fails;

#define CHECK(c, what) do { \
	if (c) { printf("ok    %s\n", what); } \
	else { printf("FAIL  %s\n", what); fails++; } \
} while (0)

static const char *names[] = { "O0", "O1", "O2", "O3", "O4", "O5", "O6", "O7" };
static const char *sname(unsigned int s) { return s < 8 ? names[s] : "?"; }

static char out[1 << 20];
static size_t outn;
static void put(void *ctx, const char *line)
{
	size_t n = strlen(line);

	(void)ctx;
	if (outn + n < sizeof(out)) {
		memcpy(out + outn, line, n);
		outn += n;
		out[outn] = 0;
	}
}

static void show(const struct odi_gpon_drop *d, uint32_t now)
{
	outn = 0;
	out[0] = 0;
	odi_gpon_drop_show(d, now, sname, put, NULL);
}

static void ploam(struct odi_gpon_drop *d, uint32_t ms, int us, uint8_t type)
{
	struct odi_gpon_drop_ev e;

	memset(&e, 0, sizeof(e));
	e.kind = ODI_GPON_DROP_PLOAM;
	e.a = (uint8_t)us;
	e.b = type;
	e.c = 1;
	odi_gpon_drop_note(d, ms, &e);
}

static void sample(struct odi_gpon_drop *d, uint32_t ms, uint8_t st)
{
	struct odi_gpon_drop_ev e;

	memset(&e, 0, sizeof(e));
	e.kind = ODI_GPON_DROP_SAMPLE;
	e.a = st;
	odi_gpon_drop_note(d, ms, &e);
}

static void alarm(struct odi_gpon_drop *d, uint32_t ms, int lof, int on, uint8_t st)
{
	struct odi_gpon_drop_ev e;

	memset(&e, 0, sizeof(e));
	e.kind = ODI_GPON_DROP_ALARM;
	e.a = (uint8_t)lof;
	e.b = (uint8_t)on;
	e.c = st;
	odi_gpon_drop_note(d, ms, &e);
}

static struct odi_gpon_drop d;

int main(void)
{
	uint32_t t0 = 1000000U, ms;

	/* --- one drop: the window before, the change, the window after --- */
	odi_gpon_drop_reset(&d);
	for (ms = t0; ms < t0 + 100000U; ms += 1000U)
		sample(&d, ms, 5);
	alarm(&d, t0 + 99500U, 0, 1, 5);
	odi_gpon_drop_state(&d, t0 + 100000U, 5, 6, "los", "line");
	CHECK(d.drops == 1 && d.open == 0 && d.rec[0].collecting, "an exit from O5 opens a record");
	/* 60 s back from t0+100000: samples t0+40000 .. t0+99000 (60), the alarm, the change. */
	CHECK(d.rec[0].n == 62, "only the last 60 s of history go in, then the change");
	CHECK(d.rec[0].ev[0].ms == t0 + 40000U && d.rec[0].ev[60].kind == ODI_GPON_DROP_ALARM &&
	      d.rec[0].ev[61].kind == ODI_GPON_DROP_STATE, "oldest first, the change last");
	ploam(&d, t0 + 100020U, 0, 0x03);
	odi_gpon_drop_state(&d, t0 + 110000U, 6, 1, "to2", "timer");
	CHECK(d.rec[0].n == 64, "what follows the drop is added");
	odi_gpon_drop_state(&d, t0 + 150000U, 5, 2, "deactivate_onu_id", "olt");
	CHECK(d.drops == 2 && d.rec[0].more == 1 && !d.rec[1].used,
	      "a second drop while collecting is counted, not given a record");
	sample(&d, t0 + 220000U, 1);
	CHECK(!d.rec[0].collecting && d.open < 0, "the record closes once 120 s have passed");
	CHECK(d.rec[0].n == 65, "and the event that closed it is not in it");

	show(&d, t0 + 230000U);
	CHECK(strstr(out, "drops 2 kept 1 pre_ms 60000 post_ms 120000\n") != NULL, "show: the header");
	CHECK(strstr(out, "drop 1 at_ms 1100000 age_s 130 to O6 cause los side line events 65 lost 0 more 1 done\n") != NULL,
	      "show: the record line");
	CHECK(strstr(out, "  -0.500 los on state O5\n") != NULL, "show: the alarm half a second before");
	CHECK(strstr(out, "  +0.000 state O5 O6 cause los side line\n") != NULL, "show: the change at offset 0");
	CHECK(strstr(out, "  +0.020 ploam ds type 0x03 onu_id 0x01\n") != NULL, "show: a PLOAM after it");
	CHECK(strstr(out, "  -60.000 sample state O5 alarms 0x00000000 bip_blocks 0\n") != NULL,
	      "show: the oldest sample is 60 s before");

	/* --- the first record stays, the later ones rotate --- */
	for (unsigned int i = 0; i < 5; i++) {
		uint32_t at = t0 + 1000000U + i * 300000U;

		odi_gpon_drop_state(&d, at, 5, 2, "deactivate_onu_id", "olt");
		sample(&d, at + 130000U, 2);
	}
	CHECK(d.drops == 7, "every exit from O5 is counted");
	CHECK(d.rec[0].seq == 1, "the first record of the boot is kept");
	CHECK(d.rec[1].seq == 6 && d.rec[2].seq == 7 && d.rec[3].seq == 5,
	      "the others hold the latest three");
	show(&d, t0 + 3000000U);
	{
		const char *a = strstr(out, "drop 1 "), *b = strstr(out, "drop 5 "),
			   *c = strstr(out, "drop 6 "), *e = strstr(out, "drop 7 ");

		CHECK(a && b && c && e && a < b && b < c && c < e, "show: oldest first");
	}

	/* --- a full record counts what it lost --- */
	odi_gpon_drop_reset(&d);
	odi_gpon_drop_state(&d, 5000U, 5, 2, "deactivate_onu_id", "olt");
	for (unsigned int i = 0; i < ODI_GPON_DROP_EVENTS + 10U; i++)
		ploam(&d, 5001U + i, 0, 0x01);
	CHECK(d.rec[0].n == ODI_GPON_DROP_EVENTS && d.rec[0].lost == 11,
	      "a full record keeps its first events and counts the rest");

	/* --- the clock wraps --- */
	odi_gpon_drop_reset(&d);
	sample(&d, 0xfffe0000U, 5);	/* 2 min before: out */
	sample(&d, 0xffff8000U, 5);
	sample(&d, 0xfffff000U, 5);
	odi_gpon_drop_state(&d, 0x00001000U, 5, 1, "los", "line");
	CHECK(d.rec[0].n == 3 && d.rec[0].ev[0].ms == 0xffff8000U, "the window reaches back across the wrap");
	sample(&d, 0x00001000U + 119000U, 1);
	CHECK(d.rec[0].collecting, "and still collects just before its end");
	sample(&d, 0x00001000U + 120000U, 1);
	CHECK(!d.rec[0].collecting, "and closes at its end");

	/* --- other changes freeze nothing --- */
	odi_gpon_drop_reset(&d);
	odi_gpon_drop_state(&d, 1000U, 1, 2, "los_cleared", "line");
	odi_gpon_drop_state(&d, 2000U, 4, 5, "ranging_time", "olt");
	odi_gpon_drop_state(&d, 3000U, 5, 5, "x", "y");
	CHECK(d.drops == 0 && d.open < 0, "only an exit from O5 is a drop");

	if (fails) {
		printf("%d failed\n", fails);
		return 1;
	}
	printf("odi_gpon_drop_test: all checks passed\n");
	return 0;
}
