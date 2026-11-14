/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * odi_gpon_drop.h -- a short record of the PON side around every drop out
 * of O5, frozen when it happens, for /proc/odi_gpon/drops.
 *
 * When the ONU leaves O5 the kernel log gets one event=onu_state line with
 * its cause and side. That line does not say what happened in the minute
 * before, and the log rotates. This keeps a small ring of what the driver
 * already sees -- PLOAM messages both ways (type and ONU-ID, never the
 * contents), ONU state changes with their cause, LOS and LOF edges, and the
 * 1 s counter sample -- and on every exit from O5 copies the last
 * ODI_GPON_DROP_PRE_MS of it into a record of its own, then keeps adding to
 * that record for ODI_GPON_DROP_POST_MS more. That answers "who dropped
 * first" (an OLT deactivate against a LOS or LOF) after the fact.
 *
 * ODI_GPON_DROP_SLOTS records: the first of the boot is kept, the others
 * hold the latest, so a later drop never overwrites the first one. A drop
 * while a record is still collecting is counted in it, not given a new one.
 *
 * Pure: time is passed in, no locking (odi_gpon.c calls every function with
 * odi_gpon_lock held), so test/odi_gpon_drop_test.c runs it on the host.
 */
#ifndef ODI_GPON_DROP_H
#define ODI_GPON_DROP_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

#define ODI_GPON_DROP_RING	256U	/* events kept before a drop */
#define ODI_GPON_DROP_EVENTS	384U	/* events per record */
#define ODI_GPON_DROP_SLOTS	4U	/* records: the first and the latest three */
#define ODI_GPON_DROP_PRE_MS	60000U	/* history copied in at the drop */
#define ODI_GPON_DROP_POST_MS	120000U	/* and kept adding to after it */

enum odi_gpon_drop_kind {
	ODI_GPON_DROP_PLOAM = 1,	/* a: 0 ds, 1 us; b: type; c: ONU-ID */
	ODI_GPON_DROP_STATE,		/* a: from; b: to; s: cause; s2: side */
	ODI_GPON_DROP_ALARM,		/* a: 0 LOS, 1 LOF; b: 1 on, 0 off; c: state */
	ODI_GPON_DROP_SAMPLE,		/* a: state; v: alarm bits; v2: BIP error blocks since the last */
};

struct odi_gpon_drop_ev {
	uint32_t ms;
	uint32_t v, v2;
	uint8_t kind, a, b, c;
	const char *s, *s2;	/* static strings only: the cause and side names */
};

struct odi_gpon_drop_rec {
	uint32_t at_ms;		/* the drop: the exit from O5 */
	uint32_t until_ms;	/* collecting until then */
	uint32_t seq;		/* drops since boot, this one included */
	uint32_t more;		/* further exits from O5 while it collected */
	uint32_t lost;		/* events that did not fit */
	uint8_t to;		/* the state it dropped to */
	uint8_t collecting;
	uint8_t used;
	const char *cause, *side;
	uint32_t n;
	struct odi_gpon_drop_ev ev[ODI_GPON_DROP_EVENTS];
};

struct odi_gpon_drop {
	struct odi_gpon_drop_ev ring[ODI_GPON_DROP_RING];
	uint32_t head, count;
	uint32_t drops;		/* exits from O5 since boot */
	uint32_t next;		/* the slot the next latest record goes to, 1.. */
	int open;		/* the slot collecting now, or -1 */
	struct odi_gpon_drop_rec rec[ODI_GPON_DROP_SLOTS];
};

void odi_gpon_drop_reset(struct odi_gpon_drop *d);

/* One event, at now_ms (any millisecond clock; only differences are used,
 * modulo 2^32). Ends a collecting record whose window has passed first.
 */
void odi_gpon_drop_note(struct odi_gpon_drop *d, uint32_t now_ms,
			const struct odi_gpon_drop_ev *e);

/* An ONU state change, noted as an event; from O5 to anything else also
 * freezes a record. cause and side are static strings.
 */
void odi_gpon_drop_state(struct odi_gpon_drop *d, uint32_t now_ms, unsigned int from,
			 unsigned int to, const char *cause, const char *side);

/* The lines of /proc/odi_gpon/drops, each passed to put() whole, newline
 * included; state_name() names an ONU state. docs/TOOLS.md has the format.
 */
typedef void (*odi_gpon_drop_put_fn)(void *ctx, const char *line);
void odi_gpon_drop_show(const struct odi_gpon_drop *d, uint32_t now_ms,
			const char *(*state_name)(unsigned int state),
			odi_gpon_drop_put_fn put, void *ctx);

#endif /* ODI_GPON_DROP_H */
