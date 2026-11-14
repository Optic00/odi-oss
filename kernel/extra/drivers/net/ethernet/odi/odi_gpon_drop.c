// SPDX-FileCopyrightText: 2026 Andre Ziviani
// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * odi_gpon_drop.c -- the record around every drop out of O5
 * (odi_gpon_drop.h has the why). Pure: no locking, no clock of its own.
 */
#ifdef __KERNEL__
#include <linux/kernel.h>
#include <linux/string.h>
#else
#include <stdio.h>
#include <string.h>
#endif

#include "odi_gpon_drop.h"

#define ODI_GPON_DROP_O5	5U

/* b is at or after a on a wrapping millisecond clock, by less than half of it. */
static int odi_gpon_drop_after(uint32_t a, uint32_t b)
{
	return (int32_t)(b - a) >= 0;
}

void odi_gpon_drop_reset(struct odi_gpon_drop *d)
{
	memset(d, 0, sizeof(*d));
	d->open = -1;
	d->next = 1;
}

static void odi_gpon_drop_close(struct odi_gpon_drop *d, uint32_t now_ms)
{
	struct odi_gpon_drop_rec *r;

	if (d->open < 0)
		return;
	r = &d->rec[d->open];
	if (!odi_gpon_drop_after(r->until_ms, now_ms))
		return;
	r->collecting = 0;
	d->open = -1;
}

static void odi_gpon_drop_add(struct odi_gpon_drop_rec *r, const struct odi_gpon_drop_ev *e)
{
	if (r->n >= ODI_GPON_DROP_EVENTS) {
		r->lost++;
		return;
	}
	r->ev[r->n++] = *e;
}

void odi_gpon_drop_note(struct odi_gpon_drop *d, uint32_t now_ms,
			const struct odi_gpon_drop_ev *e)
{
	struct odi_gpon_drop_ev ev = *e;

	odi_gpon_drop_close(d, now_ms);
	ev.ms = now_ms;
	d->ring[d->head] = ev;
	d->head = (d->head + 1U) % ODI_GPON_DROP_RING;
	if (d->count < ODI_GPON_DROP_RING)
		d->count++;
	if (d->open >= 0)
		odi_gpon_drop_add(&d->rec[d->open], &ev);
}

/* A new record for the drop at now_ms: the first one of the boot goes to
 * slot 0 and stays; the later ones take slots 1.. in turn. The ring's last
 * ODI_GPON_DROP_PRE_MS goes in first, oldest first.
 */
static void odi_gpon_drop_freeze(struct odi_gpon_drop *d, uint32_t now_ms, unsigned int to,
				 const char *cause, const char *side)
{
	struct odi_gpon_drop_rec *r;
	uint32_t first, i;
	int slot;

	d->drops++;
	if (d->open >= 0) {
		d->rec[d->open].more++;
		return;
	}
	if (!d->rec[0].used) {
		slot = 0;
	} else {
		slot = (int)d->next;
		d->next = d->next + 1U >= ODI_GPON_DROP_SLOTS ? 1U : d->next + 1U;
	}
	r = &d->rec[slot];
	memset(r, 0, sizeof(*r));
	r->used = 1;
	r->collecting = 1;
	r->at_ms = now_ms;
	r->until_ms = now_ms + ODI_GPON_DROP_POST_MS;
	r->seq = d->drops;
	r->to = (uint8_t)to;
	r->cause = cause;
	r->side = side;
	first = (d->head + ODI_GPON_DROP_RING - d->count) % ODI_GPON_DROP_RING;
	for (i = 0; i < d->count; i++) {
		const struct odi_gpon_drop_ev *e = &d->ring[(first + i) % ODI_GPON_DROP_RING];

		if ((uint32_t)(now_ms - e->ms) > ODI_GPON_DROP_PRE_MS)
			continue;	/* older than the window */
		odi_gpon_drop_add(r, e);
	}
	d->open = slot;
}

void odi_gpon_drop_state(struct odi_gpon_drop *d, uint32_t now_ms, unsigned int from,
			 unsigned int to, const char *cause, const char *side)
{
	struct odi_gpon_drop_ev e;

	memset(&e, 0, sizeof(e));
	e.kind = ODI_GPON_DROP_STATE;
	e.a = (uint8_t)from;
	e.b = (uint8_t)to;
	e.s = cause;
	e.s2 = side;
	/* Frozen before the change is added, so the record's history ends
	 * with what led up to it, and the change itself is its first event
	 * at offset 0.
	 */
	odi_gpon_drop_close(d, now_ms);
	if (from == ODI_GPON_DROP_O5 && to != ODI_GPON_DROP_O5)
		odi_gpon_drop_freeze(d, now_ms, to, cause, side);
	odi_gpon_drop_note(d, now_ms, &e);
}

/* Milliseconds from the drop, signed, as "-12.345" or "+0.020". */
static void odi_gpon_drop_rel(char *b, size_t n, uint32_t at, uint32_t ms)
{
	int32_t d = (int32_t)(ms - at);
	uint32_t a = d < 0 ? (uint32_t)-d : (uint32_t)d;

	snprintf(b, n, "%c%u.%03u", d < 0 ? '-' : '+', a / 1000U, a % 1000U);
}

static const char *odi_gpon_drop_str(const char *s)
{
	return s ? s : "-";
}

void odi_gpon_drop_show(const struct odi_gpon_drop *d, uint32_t now_ms,
			const char *(*state_name)(unsigned int state),
			odi_gpon_drop_put_fn put, void *ctx)
{
	char line[160], rel[16];
	unsigned int kept = 0, s, i, k;
	unsigned int order[ODI_GPON_DROP_SLOTS];

	for (s = 0; s < ODI_GPON_DROP_SLOTS; s++)
		if (d->rec[s].used)
			order[kept++] = s;
	/* Oldest first: by drop number. */
	for (i = 1; i < kept; i++)
		for (k = i; k > 0 && d->rec[order[k]].seq < d->rec[order[k - 1]].seq; k--) {
			unsigned int t = order[k];

			order[k] = order[k - 1];
			order[k - 1] = t;
		}
	snprintf(line, sizeof(line), "drops %u kept %u pre_ms %u post_ms %u\n",
		 d->drops, kept, ODI_GPON_DROP_PRE_MS, ODI_GPON_DROP_POST_MS);
	put(ctx, line);
	for (k = 0; k < kept; k++) {
		const struct odi_gpon_drop_rec *r = &d->rec[order[k]];

		snprintf(line, sizeof(line),
			 "drop %u at_ms %u age_s %u to %s cause %s side %s events %u lost %u more %u %s\n",
			 r->seq, r->at_ms, (now_ms - r->at_ms) / 1000U, state_name(r->to),
			 odi_gpon_drop_str(r->cause), odi_gpon_drop_str(r->side), r->n, r->lost,
			 r->more, r->collecting ? "collecting" : "done");
		put(ctx, line);
		for (i = 0; i < r->n; i++) {
			const struct odi_gpon_drop_ev *e = &r->ev[i];

			odi_gpon_drop_rel(rel, sizeof(rel), r->at_ms, e->ms);
			switch (e->kind) {
			case ODI_GPON_DROP_PLOAM:
				snprintf(line, sizeof(line), "  %s ploam %s type 0x%02x onu_id 0x%02x\n",
					 rel, e->a ? "us" : "ds", e->b, e->c);
				break;
			case ODI_GPON_DROP_STATE:
				snprintf(line, sizeof(line), "  %s state %s %s cause %s side %s\n",
					 rel, state_name(e->a), state_name(e->b),
					 odi_gpon_drop_str(e->s), odi_gpon_drop_str(e->s2));
				break;
			case ODI_GPON_DROP_ALARM:
				snprintf(line, sizeof(line), "  %s %s %s state %s\n", rel,
					 e->a ? "lof" : "los", e->b ? "on" : "off", state_name(e->c));
				break;
			case ODI_GPON_DROP_SAMPLE:
				snprintf(line, sizeof(line), "  %s sample state %s alarms 0x%08x bip_blocks %u\n",
					 rel, state_name(e->a), e->v, e->v2);
				break;
			default:
				snprintf(line, sizeof(line), "  %s kind %u\n", rel, e->kind);
				break;
			}
			put(ctx, line);
		}
	}
}
