/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */

/* Re-registrations: how often the identity changed, and how the OLT answered.
 *
 * Every re-registration a reload starts (reload.c, "anything the OLT must see
 * again") is one line in REREG_PATH on the config partition, so the count
 * survives a reboot and a respawn:
 *
 *     <time> <kind> <result> <refusals> <keys>
 *
 * time is the wall clock in seconds, 0 when the clock was not set (no NTP
 * yet: anything before 2024 is taken as not set); kind is c for a change and
 * r for a revert (the identity trial going back to what worked,
 * identity-trial.sh); result is reload.c's; refusals are the Deactivate_ONU-ID
 * PLOAMs that came while omcid waited for O5;
 * keys are the store key names, never values. The newest REREG_MAX lines are
 * kept, oldest first. A line that does not parse is skipped.
 *
 * The OLT refused a change when it deactivated or disabled the ONU
 * REREG_REFUSALS times, or disabled it once, before O5 (result refused), or
 * when O5 never came (result timeout: an unknown serial number is never
 * ranged, and the OLT logs every discovery of it). One Deactivate_ONU-ID is
 * not a refusal: an OLT may reject a correct credential for a few discovery
 * windows before it admits it.
 *
 * The guard, from what is recorded:
 *
 *   streak  the refused changes in a row, newest first, within the last
 *           24 h when the clock is set (any age when it is not). A change the
 *           OLT accepted ends the run; reverts and failed local steps neither
 *           count nor end it.
 *   today   the changes the OLT accepted in the last 24 h, -1 without a clock.
 *
 * With a streak of REREG_HOLD or more, reload.c does not re-register for a
 * changed identity unless the reload was confirmed: apply.sh omci --confirm
 * (or --revert) writes REREG_CONFIRM_PATH before its signal, and every reload
 * takes the file away, used or not, so a confirmation never outlives the
 * reload it was given for. apply.sh warns from one refusal on, and when
 * today reaches REREG_DAY_WARN.
 *
 * Some operators are said to block a line after a few refused
 * registrations; nothing here knows any operator's real limit. The numbers
 * are starting values, in omcid.h.
 */
#include "omcid.h"
#include "../procparse.h"

/* The start of 2024: a wall clock below it was never set (no NTP yet). */
#define RR_CLOCK_VALID 1704067200L
#define RR_DAY_S       86400L

const char *ploam_proc_path = PLOAM_PROC_PATH;

static struct rr_ent {
	long t;
	char kind;
	char result[16];
	unsigned refusals;
	char keys[64];
} rr[REREG_MAX];
static int rr_n;

static long wall_now(void)
{
	long s = 0, ns = 0;

	if (sys_clock_gettime(CLOCK_REALTIME, &s, &ns) != 0 || s < RR_CLOCK_VALID)
		return 0;
	return s;
}

/* ------------------------------------------------------------ the file */

static int is_space(char c) { return c == ' ' || c == '\t'; }

/* One field of a line: skips blanks, copies up to the next blank. Returns
 * the position after it, or -1 when there is no field. */
static int field(const char *l, int n, int i, char *out, int max)
{
	int o = 0;

	while (i < n && is_space(l[i]))
		i++;
	if (i >= n)
		return -1;
	while (i < n && !is_space(l[i])) {
		if (o < max - 1)
			out[o++] = l[i];
		i++;
	}
	out[o] = 0;
	return i;
}

static int num(const char *s, unsigned long *v)
{
	*v = 0;
	if (!*s)
		return -1;
	for (; *s; s++) {
		if (*s < '0' || *s > '9' || *v > 400000000UL)
			return -1;
		*v = *v * 10 + (unsigned long)(*s - '0');
	}
	return 0;
}

static void rr_push(const struct rr_ent *e)
{
	if (rr_n == REREG_MAX) {
		for (int i = 1; i < REREG_MAX; i++)
			rr[i - 1] = rr[i];
		rr_n--;
	}
	rr[rr_n++] = *e;
}

static void parse_line(const char *l, int n)
{
	struct rr_ent e;
	char t[16], k[4], r[16], f[16];
	unsigned long tv, fv;
	int i = 0;

	if ((i = field(l, n, i, t, sizeof t)) < 0 ||
	    (i = field(l, n, i, k, sizeof k)) < 0 ||
	    (i = field(l, n, i, r, sizeof r)) < 0 ||
	    (i = field(l, n, i, f, sizeof f)) < 0)
		return;
	if (num(t, &tv) || num(f, &fv) || k[1] || (k[0] != 'c' && k[0] != 'r'))
		return;
	e.t = (long)tv;
	e.kind = k[0];
	str_copy(e.result, r, sizeof e.result);
	e.refusals = (unsigned)fv;
	if (field(l, n, i, e.keys, sizeof e.keys) < 0)
		e.keys[0] = 0;
	rr_push(&e);
}

void rereg_load(void)
{
	static char buf[REREG_MAX * 112];
	long fd = sys_open(REREG_PATH, O_RDONLY);
	int n = 0;

	rr_n = 0;
	if (fd < 0)
		return;
	for (;;) {
		long r = sys_read((int)fd, buf + n, sizeof buf - (unsigned long)n);

		if (r <= 0)
			break;
		n += (int)r;
		if (n >= (int)sizeof buf)
			break;
	}
	sys_close((int)fd);
	for (int i = 0; i < n;) {
		int end = i;

		while (end < n && buf[end] != '\n')
			end++;
		if (end < n)              /* a line cut off by the buffer is dropped */
			parse_line(buf + i, end - i);
		i = end + 1;
	}
}

static void put(char *b, int *o, int max, const char *s)
{
	while (*s && *o < max - 1)
		b[(*o)++] = *s++;
}

static void putu(char *b, int *o, int max, unsigned long v)
{
	char t[12];
	int i = 0;

	do {
		t[i++] = (char)('0' + v % 10);
		v /= 10;
	} while (v && i < (int)sizeof t);
	while (i && *o < max - 1)
		b[(*o)++] = t[--i];
}

static void rereg_save(void)
{
	static char b[REREG_MAX * 112];
	int o = 0;

	for (int i = 0; i < rr_n; i++) {
		char k[2] = { rr[i].kind, 0 };

		putu(b, &o, sizeof b, (unsigned long)rr[i].t);
		put(b, &o, sizeof b, " ");
		put(b, &o, sizeof b, k);
		put(b, &o, sizeof b, " ");
		put(b, &o, sizeof b, rr[i].result);
		put(b, &o, sizeof b, " ");
		putu(b, &o, sizeof b, rr[i].refusals);
		put(b, &o, sizeof b, " ");
		put(b, &o, sizeof b, rr[i].keys[0] ? rr[i].keys : "-");
		put(b, &o, sizeof b, "\n");
	}
	/* One small rename on the config partition per re-registration, the
	 * same write cfg_set makes for one key. */
	(void)atomic_write(REREG_PATH, REREG_TMP_PATH, (const uint8_t *)b,
			   (uint32_t)o);
}

void rereg_record(int revert, const char *result, unsigned refusals,
		  const char *keys)
{
	struct rr_ent e;
	int i = 0;

	e.t = wall_now();
	e.kind = revert ? 'r' : 'c';
	str_copy(e.result, result, sizeof e.result);
	e.refusals = refusals;
	/* Names only, and no blank: it is the last field of the line. */
	for (; keys && keys[i] && i < (int)sizeof e.keys - 1; i++)
		e.keys[i] = is_space(keys[i]) ? ',' : keys[i];
	e.keys[i] = 0;
	rr_push(&e);
	rereg_save();
}

/* ------------------------------------------------------------ the guard */

static int refused(const char *r)
{
	return str_eq(r, "refused") || str_eq(r, "timeout");
}

static int accepted(const char *r)
{
	return str_eq(r, "ok") || str_eq(r, "no_services") ||
	       str_eq(r, "sn_not_applied");
}

static int recent(long now, long t)
{
	return !now || !t || now - t < RR_DAY_S;
}

unsigned rereg_streak(void)
{
	long now = wall_now();
	unsigned n = 0;

	for (int i = rr_n - 1; i >= 0; i--) {
		if (rr[i].kind != 'c')
			continue;
		if (!recent(now, rr[i].t))
			break;
		if (refused(rr[i].result))
			n++;
		else if (accepted(rr[i].result))
			break;
	}
	return n;
}

int rereg_today(void)
{
	long now = wall_now();
	int n = 0;

	if (!now)
		return -1;
	for (int i = 0; i < rr_n; i++)
		if (rr[i].kind == 'c' && rr[i].t && now - rr[i].t < RR_DAY_S &&
		    accepted(rr[i].result))
			n++;
	return n;
}

/* The confirmation apply.sh leaves for this reload: 0 none, 1 --confirm, 2
 * --revert. Taken away whatever it said. */
int rereg_take_confirm(void)
{
	char b[16];
	long fd = sys_open(REREG_CONFIRM_PATH, O_RDONLY);
	long n;

	if (fd < 0)
		return 0;
	n = sys_read((int)fd, b, sizeof b - 1);
	sys_close((int)fd);
	sys_unlink(REREG_CONFIRM_PATH);
	if (n <= 0)
		return 1;
	b[n] = 0;
	return str_has_prefix(b, "revert") ? 2 : 1;
}

/* Deactivate_ONU-ID PLOAMs (type 0x05) the driver has counted since it
 * loaded, from its ploam file: the type counts come before the message ring,
 * so the head of the file is enough, and a type with no line was never seen.
 * -1 when the file cannot be read. A disable needs no count: it leaves the
 * ONU in O7, which onu_state_now() shows. */
long rereg_ploam_deacts(void)
{
	static char buf[2048];
	static const char key[] = "ploam_type ds 0x05";
	long fd = sys_open(ploam_proc_path, O_RDONLY), n, v = 0;
	int at;

	if (fd < 0)
		return -1;
	n = sys_read((int)fd, buf, sizeof buf);
	sys_close((int)fd);
	if (n < 0)
		return -1;
	at = pp_find_line(buf, (unsigned)n, key);
	if (at < 0)
		return 0;
	for (int i = at + (int)sizeof key; i < n && buf[i] >= '0' && buf[i] <= '9'; i++)
		v = v * 10 + (buf[i] - '0');
	return v;
}

/* For omcli state. */
void rereg_show(void)
{
	unsigned streak = rereg_streak();
	int today = rereg_today();

	out_fmt("reregister : %d recorded  refused in a row %d  accepted in 24 h ",
		(long)rr_n, (long)streak);
	if (today < 0)
		out("unknown (no clock)");
	else
		out_fmt("%d", (long)today);
	if (rr_n)
		out_fmt("  last %s%s", rr[rr_n - 1].result,
			rr[rr_n - 1].kind == 'r' ? " (revert)" : "");
	if (streak >= REREG_HOLD)
		out("  held: apply.sh omci --confirm");
	out_char('\n');
}
