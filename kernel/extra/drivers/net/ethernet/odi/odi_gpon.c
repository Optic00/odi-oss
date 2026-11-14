// SPDX-FileCopyrightText: 2026 Andre Ziviani
// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * odi_gpon.c -- odi_gpon kernel-integration glue: the real binding between
 * the host-tested FSM/PLOAM/register core (odi_gpon_drv.h and its .c
 * files, replayed host-side against a captured re-activation trace,
 * test/fixtures/gpon-react-260922-excerpt.txt)
 * and the kernel boot path (the seven /proc/odi_init verbs, the
 * odi_switch_cmd.c cmd 13/15 and odi_switch_ds_gem.c encrypt-port call
 * sites).
 *
 * Three independent pieces live in this one file:
 *
 *  - The one struct odi_gpon_fsm this driver runs, one spinlock
 *    serialising every touch of it (the hard-IRQ handler, the four kernel
 *    timers below, and every /proc/odi_init verb), and the switch
 *    interrupt line: the GPON block is its one consumer, so this file
 *    requests it at boot, with every source masked, and gpondrv unmasks the
 *    GPON source (odi_gpon_chip_irq_*(), odi_gpon_isr.c).
 *
 *  - Four kernel timers (TO1, TO2, the BER-interval/REI send, the line
 *    alarm poll) armed and disarmed reactively: after every call that can
 *    move the FSM (the ISR, gponact/gpondeact, and the timers' own expiry
 *    handlers), this file re-evaluates the current state and arms exactly
 *    the timer(s) that state implies, rather than threading an arm/disarm
 *    call through struct odi_gpon_fsm_ops (whose own start_to1/stop_to1/
 *    etc leaves are deliberate no-ops, odi_gpon_hw.c's own comment --
 *    arming/disarming a real kernel timer is this file's own concern, not
 *    a register leaf).
 *    The BER-interval period itself
 *    (frames, from the OLT's own BER_Interval PLOAM) and the REI send
 *    (odi_gpon_hw_send_us_ploam(), the BIP-error-count register read) are
 *    the two things odi_gpon_hw.c could not own itself without either
 *    depending on the kernel clock or losing its own host-testability --
 *    see odi_gpon_drv.h's own comment on the seam.
 *
 *  - /proc/odi_gpon/: three read-only files. status: state, ONU-ID, serial
 *    number, EqD, Alloc-IDs, the REI count, TO1/TO2 expiry counts, the
 *    interrupt count and the ISR-attach result (irq_attached/isr_rc/imr_rc,
 *    see odi_gpon_irq_attach()), the laser, the last DS LOS edge seen and
 *    the sampler's alarm and ONU state times. counters: the sampled
 *    totals. ploam: PLOAM rx/tx totals AND per-type counts, and the last
 *    32 PLOAM messages in either direction with an uptime timestamp --
 *    filled from odi_gpon_drv.h getters plus this file's own ring buffer
 *    (odi_gpon_hw_set_ploam_log(), since the core stays
 *    kernel-clock-independent, see above).
 *
 * odi_gpon_init() -- the one-time bring-up (FSM/PLOAM state, the four
 * timers) -- runs from this file's module_init(), at kernel boot, before
 * rcS writes the first PON-step verb; so does the line request. The GPON
 * source is unmasked later, by gpondrv, where a stock boot capture shows
 * the stock firmware setting bit 10 of CHIP_IRQ_ENABLE (0x80 -> 0x480).
 */
#ifdef __KERNEL__
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/printk.h>
#include <linux/bitmap.h>
#include <linux/ratelimit.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/hex.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/reboot.h>
#include <linux/umh.h>

#endif

#include "odi_board.h"
#include "odi_gpon.h"
#include "odi_gpon_ctr.h"
#include "odi_gpon_drop.h"
#include "odi_gpon_dlog.h"
#include "odi_gpon_drv.h"
#include "odi_gpon_flow.h"
#include "odi_gpon_hw.h"
#include "odi_gpon_init.h"
#include "odi_gpon_pw.h"
#include "odi_replay_blob.h"
#include "odi_switch_cmd.h"
#include "odi_switch_dal.h"
#include "odi_switch_hw.h"
#include "odi_switch_reg.h"

#ifdef __KERNEL__

/* The link event lines (docs/TOOLS.md, "Link and provisioning events"): one
 * "odi_gpon: event=..." line of key=value pairs per ONU state transition and
 * per LOS or LOF edge, KERN_NOTICE when the link goes (leaving O5, entering
 * O7, LOS or LOF asserted) and KERN_INFO otherwise. klogd hands them to
 * syslogd, and SYSLOG_SERVER sends them off the stick: nothing of our own
 * between the printk and the collector. Their own ratelimit rather than the shared
 * printk one: 30 a minute is three times what a flapping ONU produces
 * (O2 -> O3 -> O2 on every 10 s TO1) and still bounds a PLOAM storm, and
 * ___ratelimit() itself prints how many it dropped.
 */
static DEFINE_RATELIMIT_STATE(odi_gpon_event_rs, 60 * HZ, 30);

/* Log lines of the interrupt path. odi_gpon_isr_entry() takes the lock with
 * interrupts off and every PLOAM message of an interrupt is dispatched
 * under it; a console line at 115200 baud costs about 10 ms, and a message
 * queued behind it would be acted on that much later (the O3 -> O4 answer
 * to Assign_ONU-ID is the one with a window). So while the interrupt holds
 * the lock the lines are formatted into odi_gpon_dlog_inst (microseconds)
 * and printed once the lock is released. Outside the interrupt (verbs,
 * timers) there is no such pressure and a line is printed where it is
 * logged. Called with odi_gpon_lock held.
 */
static struct odi_gpon_dlog odi_gpon_dlog_inst;

static void odi_gpon_dlog_print(enum odi_gpon_dlog_level level, const char *text)
{
	if (level == ODI_GPON_DLOG_NOTICE)
		pr_notice("%s", text);
	else
		pr_info("%s", text);
}

static __printf(2, 3) void odi_gpon_logf(enum odi_gpon_dlog_level level, const char *fmt, ...)
{
	va_list ap;

	if (odi_gpon_dlog_inst.active) {
		va_start(ap, fmt);
		odi_gpon_dlog_vadd(&odi_gpon_dlog_inst, level, fmt, ap);
		va_end(ap);
	} else {
		char text[ODI_GPON_DLOG_LEN];

		va_start(ap, fmt);
		vscnprintf(text, sizeof(text), fmt, ap);
		va_end(ap);
		odi_gpon_dlog_print(level, text);
	}
}
static unsigned long odi_gpon_state_since;	/* jiffies of the last transition */
static unsigned long odi_gpon_los_since;	/* jiffies of the last LOS rising edge */
static unsigned long odi_gpon_lof_since;	/* jiffies of the last LOF rising edge */

/* The switch interrupt, "apl_sw": the board irqchip numbers its sources
 * from RTL8686_IRQ_BASE, so switch bit 8 is irq 16. Not shared: nothing
 * else requests it.
 */
#include <linux/interrupt.h>
#include <asm/mach-rtl8686/irq.h>
#include <asm/mach-rtl8686/rtl8686regs.h>
#define ODI_GPON_IRQ	RTL8686_VIRQ(RTL8686_IRQ_SWITCH)

/* ---- Driver state: one FSM, one lock, four timers --------------------- */

static struct odi_gpon_fsm odi_gpon_fsm_inst;
static DEFINE_SPINLOCK(odi_gpon_lock);

/* True once the boot-time gponact call has replayed gpon_init.bin
 * (odi_gpon_init.h) -- every LATER gponact (an interactive re-activation,
 * after an explicit gpondeact) goes through odi_gpon_verb_activate()
 * instead, the sequence that function's own header documents. The two are
 * genuinely different register preconditions (a cold block vs. one
 * gpondeact just tore down), so replaying the fixed boot capture a second
 * time would not match either a stock re-activation or this driver's own
 * tested re-activation scenario.
 */
static bool odi_gpon_booted;

/* Dying_Gasp on a reboot (GPON_DYING_GASP, off by default, experimental).
 * odi_gpon_dg_enabled is set by the gpondg verb at boot; odi_gpon_dg_done
 * makes the send happen once per reboot. Both under odi_gpon_lock.
 */
static bool odi_gpon_dg_enabled;
static bool odi_gpon_dg_done;

static struct timer_list odi_gpon_to1_timer;
static struct timer_list odi_gpon_to2_timer;
static struct timer_list odi_gpon_ber_timer;
static struct timer_list odi_gpon_line_timer;

#define ODI_GPON_TO1_MS	10000U	/* G.984.3 clause 10.2.1 recommended initial value */
#define ODI_GPON_TO2_MS	100U	/* G.984.3 clause 10.2.1 recommended initial value */

/* The line alarm poll (odi_gpon_line_check()). The interrupt samples the
 * alarm too, and LOS_CHG/LOF_CHG are unmasked, but nothing shows those
 * interrupts arrive for every loss, and a pulled fibre stops the PLOAM
 * traffic that raises the others; the BER timer, the one periodic timer
 * this file had, runs only in O5, only once the OLT sent BER_Interval, and
 * at the period the OLT picks (10 s on both ISPs), so a loss shorter than
 * that, ended between two of its samples, was never seen at all, and O1
 * has no timer to see the light come back. So this timer runs in every
 * state while the ONU is activated (odi_gpon_line_watch). 100 ms is TO2,
 * the time G.984.3 gives an ONU to come back from an intermittent loss: a
 * loss the poll misses is shorter than the window the standard itself
 * allows for one. Its cost is one register read under the lock per tick.
 */
#define ODI_GPON_LINE_POLL_MS	100U

/* Whether the line drives the FSM: set by a gponact that activated the ONU,
 * cleared by gpondeact. Until then, and after it, the alarm is only read
 * and logged: an ONU this side stopped stays in O1 when the light comes
 * back, until the next gponact.
 */
static bool odi_gpon_line_watch;

/* 0 = BER timer not armed (no BER_Interval received yet this activation, or
 * not in O5) -- distinct from odi_gpon_hw_get_ber_interval_frames()'s own
 * "0 = none yet" so a change in the OLT-configured interval re-arms at the
 * new period instead of leaving the old one running.
 */
static uint32_t odi_gpon_ber_period_ms;
static uint8_t odi_gpon_rei_seq;

/* The link counters (odi_gpon_ctr.h): sampled every second from the boot
 * gponact on, under odi_gpon_lock, and by the BER timer before each REI.
 * One second keeps every 32-bit counter far from full at line rate and
 * bounds the state times to a second; the line check feeds the alarm
 * accounting every 100 ms besides (odi_gpon_line_check()).
 */
static struct odi_gpon_ctr odi_gpon_ctr_inst;
/* The record around every drop out of O5, /proc/odi_gpon/drops
 * (odi_gpon_drop.h). Fed under odi_gpon_lock from the PLOAM log, the
 * transition note, the alarm edges and the 1 s sample.
 */
static struct odi_gpon_drop odi_gpon_drop_inst;
static uint64_t odi_gpon_drop_last_bip;

static uint32_t odi_gpon_now_ms(void)
{
	return (uint32_t)jiffies_to_msecs(jiffies - INITIAL_JIFFIES);
}
static struct timer_list odi_gpon_ctr_timer;
#define ODI_GPON_CTR_PERIOD_MS	1000U

/* The per-flow GEM counters (odi_gpon_flow.h): read in the same cycle and
 * under the same lock as the link counters, for the flows OMCI created.
 * odi_switch_cmd.c says when one is created or the flow state emptied.
 */
static struct odi_gpon_flows odi_gpon_flows_inst;

/* ---- Counters and the PLOAM ring for /proc/odi_gpon/ -------------------
 *
 * odi_gpon_hw.c stays kernel-clock-independent (its own header contract),
 * so every field below that needs a clock or is purely observational is
 * kept here instead, fed by odi_gpon_hw_set_ploam_log() (PLOAM traffic),
 * the ISR wrapper (interrupt count, LOS status), and the timer callbacks
 * (TO1/TO2 expiry counts) -- all under odi_gpon_lock.
 */
static unsigned int odi_gpon_irq_count;
static unsigned int odi_gpon_to1_expiries;
static unsigned int odi_gpon_to2_expiries;
static unsigned int odi_gpon_rei_count;
static unsigned long odi_gpon_last_los_ms;	/* 0 = never observed since init */
static int odi_gpon_last_los_state;		/* the DS LOS status bit, last read */
static int odi_gpon_last_lof_state;		/* the DS LOF status bit, last read */

/* Spurious-interrupt and per-sub-block last-nonzero-STS bookkeeping,
 * odi_gpon_isr_poll()'s own struct odi_gpon_isr_poll_status out-parameter
 * fed straight into /proc/odi_gpon/status -- IRQ-storm diagnosis (is the switch
 * core raising this interrupt with nothing actually pending, and if not,
 * which sub-block was busy last).
 */
static unsigned int odi_gpon_irq_spurious_count;	/* top_sts == 0 on this poll */
static uint32_t odi_gpon_last_top_sts_nonzero;
static uint32_t odi_gpon_last_ds_dlt_nonzero;
static uint32_t odi_gpon_last_us_sts_nonzero;

/* For /proc/odi_gpon/status: irq_attached once gpondrv has unmasked the GPON
 * source; isr_rc the request_irq() result at boot, imr_rc the unmask (0),
 * so a failed attach is visible without the kernel log.
 */
static bool odi_gpon_irq_attached;
static int odi_gpon_irq_isr_rc;
static int odi_gpon_irq_imr_rc;

/* Per-message-type counters, both directions -- one array wide enough for
 * every documented DS/US type byte (odi_gpon_ploam.h) plus the 0x81 vendor-
 * specific Disable_Serial_Number variant, indexed directly by the raw type byte.
 */
static unsigned int odi_gpon_ds_type_count[256];
static unsigned int odi_gpon_us_type_count[256];

static bool odi_gpon_ds_type_known(uint8_t type)
{
	switch (type) {
	case ODI_GPON_DS_UPSTREAM_OVERHEAD:
	case ODI_GPON_DS_SERIAL_NUMBER_MASK:
	case ODI_GPON_DS_ASSIGN_ONU_ID:
	case ODI_GPON_DS_RANGING_TIME:
	case ODI_GPON_DS_DEACTIVATE_ONU_ID:
	case ODI_GPON_DS_DISABLE_SERIAL_NUMBER:
	case ODI_GPON_DS_CONFIGURE_VP_VC:
	case ODI_GPON_DS_ENCRYPTED_PORT_ID:
	case ODI_GPON_DS_REQUEST_PASSWORD:
	case ODI_GPON_DS_ASSIGN_ALLOC_ID:
	case ODI_GPON_DS_NO_MESSAGE:
	case ODI_GPON_DS_POPUP:
	case ODI_GPON_DS_REQUEST_KEY:
	case ODI_GPON_DS_CONFIGURE_PORT_ID:
	case ODI_GPON_DS_PHYSICAL_EQUIPMENT_ERROR:
	case ODI_GPON_DS_CHANGE_POWER_LEVEL:
	case ODI_GPON_DS_PST:
	case ODI_GPON_DS_BER_INTERVAL:
	case ODI_GPON_DS_KEY_SWITCHING_TIME:
	case ODI_GPON_DS_EXT_BURST_LENGTH:
	case ODI_GPON_DS_PON_ID:
	case ODI_GPON_DS_DISABLE_SERIAL_NUMBER_VS:
		return true;
	default:
		return false;
	}
}

/* The last Upstream_Overhead and Ranging_Time received, decoded, for
 * /proc/odi_gpon/status and the ranging event line: what the OLT asked for, next to
 * what the driver wrote (odi_gpon_get_ranging_dbg()). Debug only.
 */
static struct odi_gpon_ds_upstream_overhead odi_gpon_last_uo;
static bool odi_gpon_last_uo_valid;
static struct odi_gpon_ds_ranging_time odi_gpon_last_rng;
static bool odi_gpon_last_rng_valid;
/* The last PON-ID (G.984.3 Annex C) and when it came, for the pon_id line of
 * /proc/odi_gpon/status. Forgotten at a loss of signal: the fibre that comes
 * back may lead to another OLT port.
 */
static struct odi_gpon_ds_pon_id odi_gpon_last_pon_id;
static bool odi_gpon_last_pon_id_valid;
static u32 odi_gpon_last_pon_id_ms;

static struct odi_gpon_ploam_entry odi_gpon_ring[ODI_GPON_PLOAM_RING_LEN];
static unsigned int odi_gpon_ring_head;
static unsigned int odi_gpon_ring_count;
/* Downstream PLOAM types outside odi_gpon_ds_type_known() already logged
 * this boot. Some OLTs send the optional G.984.3 annex messages every few
 * seconds (0x18 Sleep_Allow, Annex E); one line per
 * type is enough, odi_gpon_ds_type_count[] keeps the count.
 */
static DECLARE_BITMAP(odi_gpon_ds_unknown_logged, 256);

/* odi_gpon_hw_set_ploam_log()'s own callback -- called from inside
 * odi_gpon_hw.c/odi_gpon_isr.c while odi_gpon_lock is already held by this
 * file's own ISR wrapper or verb dispatch, so no locking of its own beyond
 * that (the "one spinlock serialises every touch" rule in the file header).
 */
static void odi_gpon_ploam_log(unsigned int dir, const struct odi_gpon_ploam *msg)
{
	struct odi_gpon_ploam_entry *e = &odi_gpon_ring[odi_gpon_ring_head];

	e->timestamp_ms = (u32)jiffies_to_msecs(jiffies - INITIAL_JIFFIES);
	e->direction = (u8)dir;
	e->onu_id = msg->onu_id;
	e->type = msg->type;
	memcpy(e->content, msg->content, sizeof(e->content));
	odi_gpon_ring_head = (odi_gpon_ring_head + 1U) % ODI_GPON_PLOAM_RING_LEN;
	if (odi_gpon_ring_count < ODI_GPON_PLOAM_RING_LEN)
		odi_gpon_ring_count++;
	{
		struct odi_gpon_drop_ev de = { .kind = ODI_GPON_DROP_PLOAM, .a = (u8)dir,
					       .b = msg->type, .c = msg->onu_id };

		odi_gpon_drop_note(&odi_gpon_drop_inst, e->timestamp_ms, &de);
	}

	if (dir == 0U) {
		odi_gpon_ds_type_count[msg->type]++;
		if (msg->type == ODI_GPON_DS_UPSTREAM_OVERHEAD) {
			odi_gpon_decode_upstream_overhead(msg, &odi_gpon_last_uo);
			odi_gpon_last_uo_valid = true;
		} else if (msg->type == ODI_GPON_DS_RANGING_TIME &&
			   odi_gpon_fsm_directed_to_us(&odi_gpon_fsm_inst, msg)) {
			/* Directed: only ours is kept, as the FSM takes the EqD
			 * only from ours (odi_gpon_fsm.c, O4). The PLOAM logs
			 * of both sticks have shown only their own ONU-ID and
			 * broadcast, so the receiver appears to filter, but a
			 * shared PON carries every ONU its own Ranging_Time. */
			odi_gpon_decode_ranging_time(msg, &odi_gpon_last_rng);
			odi_gpon_last_rng_valid = true;
		} else if (msg->type == ODI_GPON_DS_PON_ID && msg->onu_id == ODI_GPON_ONU_ID_BROADCAST) {
			odi_gpon_decode_pon_id(msg, &odi_gpon_last_pon_id);
			odi_gpon_last_pon_id_valid = true;
			odi_gpon_last_pon_id_ms = e->timestamp_ms;
		}
		if (!odi_gpon_ds_type_known(msg->type) &&
		    !__test_and_set_bit(msg->type, odi_gpon_ds_unknown_logged))
			odi_gpon_logf(ODI_GPON_DLOG_INFO,
				      "odi_gpon: unexpected downstream PLOAM type 0x%02x (logged once; ploam_type ds counts it)\n",
				      msg->type);
	} else {
		odi_gpon_us_type_count[msg->type]++;
		if (msg->type == ODI_GPON_US_REI)
			odi_gpon_rei_count++;
	}
}

/* ---- Timer (re)arming, called with odi_gpon_lock held ----------------- */

/* TO1 and TO2 are armed only when not already pending, so they run from the
 * state entry and nothing extends them: TO1 bounds O3 and O4 together
 * (G.984.3 clause 10.2.1, "the overall time an ONU can sojourn in states O3
 * and O4"), so it is left running across O3 -> O4, and TO2 bounds one stay
 * in O6. Re-arming them on every call let each interrupt, and now every
 * line poll, push the deadline back, and a TO the poll keeps moving never
 * expires. A state left and entered again always passes through a call
 * here in between (each entry point feeds at most one line event, and the
 * interrupt syncs after each PLOAM it drains, odi_gpon_isr_msg()), which
 * deletes the timer, so a new entry starts a fresh one.
 */
static void odi_gpon_timers_sync(void)
{
	enum odi_gpon_state st = odi_gpon_fsm_inst.state;

	if (st == ODI_GPON_STATE_O3 || st == ODI_GPON_STATE_O4) {
		if (!timer_pending(&odi_gpon_to1_timer))
			mod_timer(&odi_gpon_to1_timer,
				  jiffies + msecs_to_jiffies(ODI_GPON_TO1_MS));
	} else {
		timer_delete(&odi_gpon_to1_timer);
	}

	if (st == ODI_GPON_STATE_O6) {
		if (!timer_pending(&odi_gpon_to2_timer))
			mod_timer(&odi_gpon_to2_timer,
				  jiffies + msecs_to_jiffies(ODI_GPON_TO2_MS));
	} else {
		timer_delete(&odi_gpon_to2_timer);
	}

	if (odi_gpon_line_watch) {
		if (!timer_pending(&odi_gpon_line_timer))
			mod_timer(&odi_gpon_line_timer,
				  jiffies + msecs_to_jiffies(ODI_GPON_LINE_POLL_MS));
	} else {
		timer_delete(&odi_gpon_line_timer);
	}

	if (st == ODI_GPON_STATE_O5) {
		uint32_t frames = odi_gpon_hw_get_ber_interval_frames();

		if (frames) {
			/* One downstream frame is 125 us (G.984.3 clause 3.1
			 * frame period; cross-check against the react.txt
			 * capture: 80000 frames * 125 us = 10.000 s, matching
			 * the observed 10.02 s REI cadence exactly) -- frames *
			 * 125 us = frames / 8 ms exactly, no rounding needed for
			 * every interval this driver has seen (all multiples of
			 * 8 frames).
			 */
			uint32_t period_ms = frames / 8U;

			if (!period_ms)
				period_ms = 1U;
			if (period_ms != odi_gpon_ber_period_ms || !timer_pending(&odi_gpon_ber_timer)) {
				odi_gpon_ber_period_ms = period_ms;
				mod_timer(&odi_gpon_ber_timer, jiffies + msecs_to_jiffies(period_ms));
			}
		} else {
			odi_gpon_ber_period_ms = 0U;
			timer_delete(&odi_gpon_ber_timer);
		}
	} else {
		odi_gpon_ber_period_ms = 0U;
		timer_delete(&odi_gpon_ber_timer);
	}
}

static const char *odi_gpon_state_name(enum odi_gpon_state st)
{
	switch (st) {
	case ODI_GPON_STATE_O1: return "O1";
	case ODI_GPON_STATE_O2: return "O2";
	case ODI_GPON_STATE_O3: return "O3";
	case ODI_GPON_STATE_O4: return "O4";
	case ODI_GPON_STATE_O5: return "O5";
	case ODI_GPON_STATE_O6: return "O6";
	case ODI_GPON_STATE_O7: return "O7";
	default: return "?";
	}
}

/* Seconds and milliseconds since `since`, for the event lines: printed as
 * N.mmm, which does not wrap where a u32 millisecond count would, at 49 days
 * of an ONU happily sitting in O5.
 */
static void odi_gpon_elapsed(unsigned long since, unsigned long *s, unsigned int *ms)
{
	unsigned long d = jiffies - since;

	*s = d / HZ;
	*ms = jiffies_to_msecs(d % HZ);
}

/* The event=ranging line, once per O4 -> O5 transition: what the Ranging_Time
 * carried (EqD in bits, protection path bit), whether the Upstream_Overhead
 * before it announced a pre-assigned delay, and the registers written. Same
 * ratelimit as the onu_state line. Called under the lock, after the
 * transition.
 */
static void odi_gpon_note_ranging(enum odi_gpon_state prev, enum odi_gpon_state now)
{
	struct odi_gpon_ranging_dbg dbg;

	if (prev != ODI_GPON_STATE_O4 || now != ODI_GPON_STATE_O5 ||
	    !odi_gpon_last_rng_valid || !odi_gpon_last_uo_valid)
		return;
	if (!__ratelimit(&odi_gpon_event_rs))
		return;
	odi_gpon_get_ranging_dbg(&dbg);
	odi_gpon_logf(ODI_GPON_DLOG_INFO,
		      "odi_gpon: event=ranging eqd_bits=%u protection=%u preassigned=%u preassigned_delay=%u multframe=%u inframe=%u onu_id=%u\n",
		      odi_gpon_last_rng.eqd, odi_gpon_last_rng.protection_path,
		      odi_gpon_last_uo.preassigned_delay_en,
		      (unsigned int)odi_gpon_last_uo.preassigned_delay,
		      dbg.multframe, dbg.inframe, (unsigned int)odi_gpon_fsm_inst.onu_id);
}

/* GPON_KEEP_DISABLE: the kept Emergency Stop lives in the leaf
 * (odi_gpon_hw_hold_*()); what is here is the way out to the config
 * partition. The kernel cannot write the jffs2 files, so a change of the
 * hold runs the helper once, which makes the marker file follow what
 * /proc/odi_gpon/status says (rootfs/skeleton/etc/scripts/gpon-hold.sh).
 * The helper reads the state itself instead of being told it, so two runs
 * that overtake each other still end on the current state. Run from the
 * unbound workqueue, no waiting: this is called under the lock, from the
 * interrupt. A helper that cannot run (no rootfs, no script) leaves the
 * hold in force for this boot and loses it at the next.
 */
static char *odi_gpon_hold_argv[] = { "/etc/scripts/gpon-hold.sh", "sync", NULL };
static char *odi_gpon_hold_envp[] = {
	"HOME=/", "PATH=/etc/scripts:/sbin:/usr/sbin:/bin:/usr/bin", NULL,
};

static void odi_gpon_hold_sync(void)
{
	struct subprocess_info *info;

	info = call_usermodehelper_setup(odi_gpon_hold_argv[0], odi_gpon_hold_argv,
					 odi_gpon_hold_envp, GFP_ATOMIC, NULL, NULL, NULL);
	if (!info || call_usermodehelper_exec(info, UMH_NO_WAIT))
		odi_gpon_logf(ODI_GPON_DLOG_NOTICE,
			      "odi_gpon: event=o7_hold action=marker_not_synced cause=no_helper\n");
}

/* The event=o7_hold line: what happened to the kept Emergency Stop. Not
 * ratelimited: it is rare, and it is what says why the stick is dark.
 */
static void odi_gpon_hold_event(const char *action, const char *cause)
{
	odi_gpon_logf(ODI_GPON_DLOG_NOTICE, "odi_gpon: event=o7_hold action=%s cause=%s\n", action, cause);
}

/* The event=onu_state line, for every entry point below that can move the
 * FSM: called under the lock, after the call, with the state before it and
 * why it ran (odi_gpon_fsm_cause(), or a verb's own name).
 */
static void odi_gpon_note_transition(enum odi_gpon_state prev,
				     const struct odi_gpon_fsm_cause *cause)
{
	enum odi_gpon_state now = odi_gpon_fsm_inst.state;
	unsigned long secs;
	unsigned int ms;

	if (now == prev)
		return;
	odi_gpon_ctr_note_transition(&odi_gpon_ctr_inst, prev, now);
	odi_gpon_drop_state(&odi_gpon_drop_inst, odi_gpon_now_ms(), prev, now, cause->name,
			    cause->side);
	odi_gpon_note_ranging(prev, now);
	switch (odi_gpon_hw_hold_track(prev, now)) {
	case ODI_GPON_HOLD_RECORDED:
		odi_gpon_hold_event("recorded", cause->name);
		odi_gpon_hold_sync();
		break;
	case ODI_GPON_HOLD_RELEASED:
		odi_gpon_hold_event("released", cause->name);
		odi_gpon_hold_sync();
		break;
	case ODI_GPON_HOLD_UNCHANGED:
		break;
	}
	odi_gpon_elapsed(odi_gpon_state_since, &secs, &ms);
	odi_gpon_state_since = jiffies;
	if (!__ratelimit(&odi_gpon_event_rs))
		return;
	if (prev == ODI_GPON_STATE_O5 || now == ODI_GPON_STATE_O7)
		odi_gpon_logf(ODI_GPON_DLOG_NOTICE,
			      "odi_gpon: event=onu_state from=%s to=%s in_state_s=%lu.%03u cause=%s side=%s onu_id=%u\n",
			      odi_gpon_state_name(prev), odi_gpon_state_name(now), secs, ms,
			      cause->name, cause->side, (unsigned int)odi_gpon_fsm_inst.onu_id);
	else
		odi_gpon_logf(ODI_GPON_DLOG_INFO,
			      "odi_gpon: event=onu_state from=%s to=%s in_state_s=%lu.%03u cause=%s side=%s onu_id=%u\n",
			      odi_gpon_state_name(prev), odi_gpon_state_name(now), secs, ms,
			      cause->name, cause->side, (unsigned int)odi_gpon_fsm_inst.onu_id);
}

static void odi_gpon_note_event(enum odi_gpon_state prev, enum odi_gpon_event event,
				const struct odi_gpon_ploam *msg)
{
	struct odi_gpon_fsm_cause cause;

	odi_gpon_fsm_cause(event, msg, &cause);
	odi_gpon_note_transition(prev, &cause);
}

/* One alarm bit, its edges as event=<name> lines: state=on with the ONU
 * state the loss found, state=off with how long it lasted. *last and
 * *since are that bit own record.
 */
static void odi_gpon_alarm_edge(const char *name, int now, int *last, unsigned long *since,
				enum odi_gpon_state st)
{
	unsigned long secs;
	unsigned int ms;

	if (now != *last) {
		struct odi_gpon_drop_ev de = { .kind = ODI_GPON_DROP_ALARM,
					       .a = strcmp(name, "lof") ? 0 : 1, .b = now ? 1 : 0,
					       .c = (u8)st };

		odi_gpon_drop_note(&odi_gpon_drop_inst, odi_gpon_now_ms(), &de);
	}
	if (now && !*last) {
		*since = jiffies;
		if (__ratelimit(&odi_gpon_event_rs))
			odi_gpon_logf(ODI_GPON_DLOG_NOTICE,
				      "odi_gpon: event=%s state=on onu_state=%s\n",
				      name, odi_gpon_state_name(st));
	} else if (!now && *last) {
		odi_gpon_elapsed(*since, &secs, &ms);
		if (__ratelimit(&odi_gpon_event_rs))
			odi_gpon_logf(ODI_GPON_DLOG_INFO,
				      "odi_gpon: event=%s state=off lasted_s=%lu.%03u onu_state=%s\n",
				      name, secs, ms, odi_gpon_state_name(st));
	}
	*last = now;
}

/* The line alarm, sampled and fed to the FSM (odi_gpon_line_sample(),
 * odi_gpon_fsm_line_event()): from the interrupt, every timer callback and
 * the line poll. The LOS and LOF edges are logged with the state the
 * sample found, before the event=onu_state line of the transition they
 * cause (cause=los, lof or los_cleared, side=line). Called with the lock
 * held; the caller then runs odi_gpon_timers_sync(), which arms TO2 on an
 * O5 -> O6.
 */
static void odi_gpon_line_check(void)
{
	struct odi_gpon_line_sample smp;
	enum odi_gpon_state prev = odi_gpon_fsm_inst.state;
	int los;

	odi_gpon_line_sample(&odi_gpon_fsm_inst, odi_gpon_line_watch, &smp);
	if (smp.dark) {
		/* Not clocked: the alarm word is the bus's no-decode reply, so
		 * there is no LOS or LOF to count or log. Nothing was fed.
		 */
		pr_warn_once("odi_gpon: the GPON MAC answers 0x%08x (not clocked); line alarms ignored while it does\n",
			     smp.alarms);
		return;
	}
	odi_gpon_ctr_note_alarms(&odi_gpon_ctr_inst, smp.alarms,
				 jiffies_to_msecs(jiffies - INITIAL_JIFFIES));

	los = (smp.alarms & ODI_GPON_DSF_ALARM_STATE_LOS_NOW) ? 1 : 0;
	if (los && !odi_gpon_last_los_state) {
		odi_gpon_last_los_ms = jiffies_to_msecs(jiffies - INITIAL_JIFFIES);
		odi_gpon_last_pon_id_valid = false;
	}
	odi_gpon_alarm_edge("los", los, &odi_gpon_last_los_state, &odi_gpon_los_since, prev);
	odi_gpon_alarm_edge("lof", (smp.alarms & ODI_GPON_DSF_ALARM_STATE_LOF_NOW) ? 1 : 0,
			    &odi_gpon_last_lof_state, &odi_gpon_lof_since, prev);

	if (smp.fed) {
		struct odi_gpon_fsm_cause cause = { smp.cause, "line" };

		odi_gpon_note_transition(prev, &cause);
	}
}

/* ---- Timer callbacks --------------------------------------------------- */

/* One counter sample now; with the lock held. */
static void odi_gpon_ctr_sample_locked(void)
{
	odi_gpon_ctr_sample(&odi_gpon_ctr_inst, jiffies_to_msecs(jiffies - INITIAL_JIFFIES),
			    odi_gpon_fsm_inst.state);
	odi_gpon_flows_sample(&odi_gpon_flows_inst);
}

/* The 1 s sample into the drop record, from the periodic timer only: the
 * REI timer samples too, at whatever BER interval the OLT sets, and must
 * not crowd the ring. The BIP total is cumulative, so the difference is
 * right whoever sampled in between. With the lock held.
 */
static void odi_gpon_drop_sample_locked(void)
{
	uint64_t bip;
	struct odi_gpon_drop_ev de = { .kind = ODI_GPON_DROP_SAMPLE };

	bip = odi_gpon_ctr_inst.total[ODI_GPON_CTR_DS_BIP_ERR_BLOCKS];
	de.a = (u8)odi_gpon_fsm_inst.state;
	de.v = odi_gpon_ctr_inst.alarm_state;
	de.v2 = bip - odi_gpon_drop_last_bip > 0xffffffffULL ? 0xffffffffU :
		(uint32_t)(bip - odi_gpon_drop_last_bip);
	odi_gpon_drop_last_bip = bip;
	odi_gpon_drop_note(&odi_gpon_drop_inst, odi_gpon_now_ms(), &de);
}

/* The flow tables of odi_switch_cmd.c, as they change. */
static void odi_gpon_flow_set(int us, uint32_t idx, uint32_t gem_port)
{
	unsigned long flags;

	spin_lock_irqsave(&odi_gpon_lock, flags);
	odi_gpon_flows_set(&odi_gpon_flows_inst, us ? ODI_GPON_FLOW_DIR_US : ODI_GPON_FLOW_DIR_DS,
			   idx, gem_port);
	spin_unlock_irqrestore(&odi_gpon_lock, flags);
}

static void odi_gpon_flow_del(int us, uint32_t idx)
{
	unsigned long flags;

	spin_lock_irqsave(&odi_gpon_lock, flags);
	(void)odi_gpon_flows_del(&odi_gpon_flows_inst,
				 us ? ODI_GPON_FLOW_DIR_US : ODI_GPON_FLOW_DIR_DS, idx);
	spin_unlock_irqrestore(&odi_gpon_lock, flags);
}

static void odi_gpon_flow_clear(void)
{
	unsigned long flags;

	spin_lock_irqsave(&odi_gpon_lock, flags);
	odi_gpon_flows_clear(&odi_gpon_flows_inst);
	spin_unlock_irqrestore(&odi_gpon_lock, flags);
}

static const struct odi_switch_flow_ops odi_gpon_flow_ops = {
	.set = odi_gpon_flow_set,
	.del = odi_gpon_flow_del,
	.clear = odi_gpon_flow_clear,
};

static void odi_gpon_ctr_fn(struct timer_list *odi_timer_arg)
{
	unsigned long flags;

	(void)odi_timer_arg;
	spin_lock_irqsave(&odi_gpon_lock, flags);
	odi_gpon_ctr_sample_locked();
	odi_gpon_drop_sample_locked();
	mod_timer(&odi_gpon_ctr_timer, jiffies + msecs_to_jiffies(ODI_GPON_CTR_PERIOD_MS));
	spin_unlock_irqrestore(&odi_gpon_lock, flags);
}

static void odi_gpon_to1_fn(struct timer_list *odi_timer_arg)
{
	unsigned long flags;
	enum odi_gpon_state prev;

	(void)odi_timer_arg;
	spin_lock_irqsave(&odi_gpon_lock, flags);
	prev = odi_gpon_fsm_inst.state;
	odi_gpon_to1_expiries++;
	odi_gpon_fsm_handle_event(&odi_gpon_fsm_inst, odi_gpon_hw_ops(), NULL,
				   ODI_GPON_EVENT_TO1_EXPIRE, NULL);
	odi_gpon_note_event(prev, ODI_GPON_EVENT_TO1_EXPIRE, NULL);
	odi_gpon_line_check();	/* O2 with the light gone goes on to O1 */
	odi_gpon_timers_sync();
	spin_unlock_irqrestore(&odi_gpon_lock, flags);
}

static void odi_gpon_to2_fn(struct timer_list *odi_timer_arg)
{
	unsigned long flags;
	enum odi_gpon_state prev;

	(void)odi_timer_arg;
	spin_lock_irqsave(&odi_gpon_lock, flags);
	prev = odi_gpon_fsm_inst.state;
	odi_gpon_to2_expiries++;
	odi_gpon_fsm_handle_event(&odi_gpon_fsm_inst, odi_gpon_hw_ops(), NULL,
				   ODI_GPON_EVENT_TO2_EXPIRE, NULL);
	odi_gpon_note_event(prev, ODI_GPON_EVENT_TO2_EXPIRE, NULL);
	/* The light may have come back inside the O6 window, with no POPUP:
	 * O1 then goes on to O2 at once, since no LOS edge is left to wait
	 * for.
	 */
	odi_gpon_line_check();
	odi_gpon_timers_sync();
	spin_unlock_irqrestore(&odi_gpon_lock, flags);
}

static void odi_gpon_ber_fn(struct timer_list *odi_timer_arg)
{
	unsigned long flags;
	uint32_t bip_err;
	struct odi_gpon_ploam rei;

	(void)odi_timer_arg;
	spin_lock_irqsave(&odi_gpon_lock, flags);
	odi_gpon_line_check();
	if (odi_gpon_fsm_inst.state != ODI_GPON_STATE_O5) {
		odi_gpon_timers_sync();	/* a loss just found: TO2 for O6 */
		spin_unlock_irqrestore(&odi_gpon_lock, flags);
		return;
	}

	/* The REI error count is DSF_BIP_ERR_BLOCKS, the register the react
	 * capture reads before every REI send. It clears when read, so only
	 * the counter sampler reads it: brought up to date here, it hands
	 * over the blocks counted in O5 since the last REI.
	 */
	odi_gpon_ctr_sample_locked();
	bip_err = odi_gpon_ctr_rei_take(&odi_gpon_ctr_inst);
	odi_gpon_rei_seq = (odi_gpon_rei_seq + 1U) & 0x0fU;
	odi_gpon_encode_rei(odi_gpon_fsm_inst.onu_id, bip_err, odi_gpon_rei_seq, &rei);
	odi_gpon_hw_send_us_ploam(&rei);

	odi_gpon_timers_sync();	/* re-arms this same timer for the next interval */
	spin_unlock_irqrestore(&odi_gpon_lock, flags);
}

static void odi_gpon_line_fn(struct timer_list *odi_timer_arg)
{
	unsigned long flags;

	(void)odi_timer_arg;
	spin_lock_irqsave(&odi_gpon_lock, flags);
	odi_gpon_line_check();
	odi_gpon_timers_sync();	/* re-arms this timer while the line is watched */
	spin_unlock_irqrestore(&odi_gpon_lock, flags);
}

/* ---- ISR ---------------------------------------------------------------
 *
 * odi_gpon_isr_entry() services the GPON block, called by
 * odi_gpon_chip_irq_demux() from the line handler below when the GPON
 * source is pending; the demux acks the source afterwards. The GPON block
 * masks and unmasks its own PONMAC_IRQ_ENABLE inside odi_gpon_isr_poll().
 */

/* After each downstream message odi_gpon_isr_poll() dispatched, under the
 * lock: the event=onu_state line for the transition that message caused,
 * and the timers for the state it left, so a state left and entered again
 * within one interrupt still restarts its timer.
 */
static void odi_gpon_isr_msg(enum odi_gpon_state prev, const struct odi_gpon_ploam *msg)
{
	static bool ebl_logged;

	/* The burst overhead the first Extended_Burst_Length set, one line
	 * per boot; one that arrives outside O3 is not applied
	 * (odi_gpon_isr.c) and not logged.
	 */
	if (msg->type == ODI_GPON_DS_EXT_BURST_LENGTH && prev == ODI_GPON_STATE_O3 && !ebl_logged) {
		ebl_logged = true;
		odi_gpon_logf(ODI_GPON_DLOG_INFO,
			      "odi_gpon_hw: Ext_Burst_Length: pre-ranged %u, ranged %u type 3 preamble bytes\n",
			      (unsigned int)msg->content[0], (unsigned int)msg->content[1]);
	}
	odi_gpon_note_event(prev, ODI_GPON_EVENT_PLOAM_RX, msg);
	odi_gpon_timers_sync();
}

/* The lines taken out of odi_gpon_dlog_inst to be printed without the lock.
 * Static, not on the stack: the buffer is 1.5 KB and this runs in interrupt
 * context on a small kernel stack. Only odi_gpon_isr_entry() touches it, and
 * the kernel does not run one interrupt handler concurrently with itself,
 * so it needs no lock of its own.
 */
static struct odi_gpon_dlog odi_gpon_dlog_out;

static void odi_gpon_isr_entry(void)
{
	unsigned long flags;
	struct odi_gpon_isr_poll_status poll_status;
	struct odi_gpon_dlog *held = &odi_gpon_dlog_out;
	unsigned int i;

	spin_lock_irqsave(&odi_gpon_lock, flags);
	odi_gpon_irq_count++;
	odi_gpon_dlog_inst.active = 1;	/* lines are held until the lock is released, below */

	/* The line first, then the PLOAM: a loss found here moves the FSM
	 * before the message is dispatched, so the message meets the state
	 * the line put the ONU in (O6, where only POPUP, Deactivate_ONU-ID
	 * and Disable_Serial_Number act; O1, where nothing does), and one
	 * read as the light comes back meets O2.
	 */
	odi_gpon_line_check();

	/* Each message drained moves the FSM on its own and is logged and
	 * timed by odi_gpon_isr_msg(); the sync after covers an interrupt
	 * that drained none.
	 */
	odi_gpon_isr_poll(&odi_gpon_fsm_inst, &poll_status);
	odi_gpon_timers_sync();

	if (poll_status.top_sts == 0U)
		odi_gpon_irq_spurious_count++;
	else
		odi_gpon_last_top_sts_nonzero = poll_status.top_sts;
	if (poll_status.ds_dlt)
		odi_gpon_last_ds_dlt_nonzero = poll_status.ds_dlt;
	if (poll_status.us_sts)
		odi_gpon_last_us_sts_nonzero = poll_status.us_sts;

	odi_gpon_dlog_inst.active = 0;
	odi_gpon_dlog_take(&odi_gpon_dlog_inst, held);
	spin_unlock_irqrestore(&odi_gpon_lock, flags);

	for (i = 0; i < held->n; i++)
		odi_gpon_dlog_print(held->line[i].level, held->line[i].text);
	if (held->lost)
		pr_info("odi_gpon: %u log line(s) of one interrupt not kept\n", held->lost);
}

static int odi_gpon_irq_dev_id; /* request_irq() needs a unique non-NULL token */

static irqreturn_t odi_gpon_irq(int irq, void *dev_id)
{
	(void)irq;
	(void)dev_id;
	return odi_gpon_chip_irq_demux(odi_gpon_isr_entry) ? IRQ_HANDLED : IRQ_NONE;
}

/* ---- gponsn/gponpw argument parsing ------------------------------------
 *
 * odi_gpon owns this parsing now (odi_gpon.h's own header comment) -- the
 * /proc/odi_init dispatch hands this file the raw
 * verb argument text unmodified, the same text rcS already trims. The
 * formats: gponsn takes a 12-character argument (4 literal ASCII vendor-ID
 * bytes + 8 hex characters for the 4 specific bytes); gponpw takes the stored
 * password text, parsed by odi_gpon_pw.c (hex of any even length up to 20,
 * or 1-10 literal characters, zero-padded), or no argument at all.
 */
static int odi_gpon_parse_sn(const char *arg, uint8_t sn[8])
{
	if (!arg || strlen(arg) != 12U)
		return -EINVAL;
	memcpy(sn, arg, 4U);
	if (hex2bin(&sn[4U], &arg[4U], 4U))
		return -EINVAL;
	return 0;
}

/* ---- odi_gpon_drv.h-facing entry points -------------------------------- */

/* The chip words once, for a bug report: all three answer at any time. */
static void odi_gpon_log_chip(void)
{
	uint32_t test = odi_reg_read(ODI_GPON_GPON_0x700280_OFF);

	pr_info("odi_gpon: chip id 0x%08x, GPON MAC rev 0x%08x, memory self-test 0x%08x%s\n",
		odi_reg_read(ODI_SW_CHIP_ID_WORD_OFF), odi_reg_read(ODI_GPON_PONMAC_REVISION_OFF), test,
		(test & 0x18U) ? " (FAIL bit set)" : "");
}

int odi_gpon_init(void)
{
	odi_gpon_log_chip();
	odi_gpon_hw_reset();
	odi_gpon_fsm_init(&odi_gpon_fsm_inst);
	odi_gpon_hw_set_ploam_log(odi_gpon_ploam_log);
	odi_gpon_hw_set_tx_disable(odi_board_tx_disable);
	odi_gpon_isr_set_msg_hook(odi_gpon_isr_msg);
	odi_gpon_booted = false;
	odi_gpon_ber_period_ms = 0U;
	odi_gpon_state_since = jiffies;
	odi_gpon_ctr_reset(&odi_gpon_ctr_inst);
	odi_gpon_drop_reset(&odi_gpon_drop_inst);
	odi_gpon_drop_last_bip = 0;
	odi_gpon_flows_reset(&odi_gpon_flows_inst);
	odi_switch_cmd_set_flow_ops(&odi_gpon_flow_ops);
	timer_setup(&odi_gpon_ctr_timer, odi_gpon_ctr_fn, 0);

	timer_setup(&odi_gpon_to1_timer, odi_gpon_to1_fn, 0);
	timer_setup(&odi_gpon_to2_timer, odi_gpon_to2_fn, 0);
	timer_setup(&odi_gpon_ber_timer, odi_gpon_ber_fn, 0);
	timer_setup(&odi_gpon_line_timer, odi_gpon_line_fn, 0);
	odi_gpon_line_watch = false;

	/* The line, with every source masked; gpondrv unmasks GPON. */
	odi_gpon_chip_irq_reset();
	odi_gpon_irq_isr_rc = request_irq(ODI_GPON_IRQ, odi_gpon_irq, 0, "apl_sw",
					  &odi_gpon_irq_dev_id);
	if (odi_gpon_irq_isr_rc)
		pr_err("odi_gpon: request_irq(%d) failed, rc=%d\n",
		       ODI_GPON_IRQ, odi_gpon_irq_isr_rc);
	else
		pr_info("odi_gpon: IRQ %d (apl_sw) attached, every source masked\n",
			ODI_GPON_IRQ);
	return 0;
}

/* Unmasks the GPON source of the switch interrupt, once, from gpondrv.
 * The read-modify-write of CHIP_IRQ_ENABLE runs under odi_gpon_lock, the
 * lock the handler takes for the GPON block.
 */
static void odi_gpon_irq_attach(void)
{
	unsigned long flags;

	spin_lock_irqsave(&odi_gpon_lock, flags);
	if (!odi_gpon_irq_attached) {
		odi_gpon_chip_irq_enable();
		odi_gpon_irq_attached = true;
		odi_gpon_irq_imr_rc = 0;
	}
	spin_unlock_irqrestore(&odi_gpon_lock, flags);
	pr_info("odi_gpon: GPON interrupt source unmasked\n");
}

/* The gponhold verb, under the lock (GPON_KEEP_DISABLE, docs/SETTINGS.md):
 *   1    restore a kept Emergency Stop: the boot with the marker present. The
 *        line goes high now, and the boot gponact (or the next one) ends in
 *        O7. Refused (-EPERM) with the feature off, and (-EBUSY) once the
 *        ONU is past O1: it is for a PON that is not up.
 *   0    release by hand: what the OLT enable does, without the OLT. The
 *        hold ends, and an ONU in O7 is activated (to O2).
 *   sim  TEST HOOK: act as if the OLT had disabled this ONU. Sends nothing.
 *        Needs the feature on, and an ONU in O2 to O6 as the real message
 *        does (-EBUSY otherwise).
 * Called with odi_gpon_lock held; the event=o7_hold lines are here.
 */
static int odi_gpon_verb_hold(const char *arg)
{
	struct odi_gpon_fsm *fsm = &odi_gpon_fsm_inst;

	if (!arg)
		return -EINVAL;
	if (!strcmp(arg, "1")) {
		if (!odi_gpon_hw_hold_enabled())
			return -EPERM;
		if (fsm->state != ODI_GPON_STATE_O1 && fsm->state != ODI_GPON_STATE_O7)
			return -EBUSY;
		if (odi_gpon_hw_hold_get())
			return 0;
		odi_gpon_hw_hold_set(1);
		if (odi_gpon_booted)
			odi_gpon_hw_hold_enter(fsm);
		odi_gpon_hold_event("kept", "boot");
	} else if (!strcmp(arg, "0")) {
		if (!odi_gpon_hw_hold_get())
			return 0;
		odi_gpon_hw_hold_set(0);
		if (odi_gpon_booted && fsm->state == ODI_GPON_STATE_O7)
			odi_gpon_verb_activate(fsm);
		odi_gpon_hold_event("cleared", "local");
		odi_gpon_hold_sync();
	} else if (!strcmp(arg, "sim")) {
		if (!odi_gpon_hw_hold_enabled())
			return -EPERM;
		if (fsm->state < ODI_GPON_STATE_O2 || fsm->state > ODI_GPON_STATE_O6)
			return -EBUSY;
		odi_gpon_hw_hold_simulate_disable(fsm);
	} else {
		return -EINVAL;
	}
	return 0;
}

/* The verb as a static string: the transition it causes keeps its cause
 * in the drop record (odi_gpon_drop.h, static strings only), and verb
 * itself points into the caller's buffer, the /proc/odi_init write.
 */
static const char *odi_gpon_verb_name(const char *verb)
{
	static const char *const names[] = {
		"gpondrv", "gpondev", "gponsn", "gponpw", "gpondg", "gponkeep",
		"gponhold", "gponact", "gpondeact", "gponstat",
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(names); i++)
		if (!strcmp(verb, names[i]))
			return names[i];
	return "verb";
}

int odi_gpon_verb(const char *verb, const char *arg)
{
	struct odi_replay_fw init_fw = { };
	int init_fw_rc = -ENOENT;
	unsigned long flags;
	enum odi_gpon_state prev;
	struct odi_board_laser laser;
	int rc = 0;

	if (!verb)
		return -EINVAL;

	/* Before the lock below: odi_gpon_irq_attach() takes it itself. */
	if (!strcmp(verb, "gpondrv"))
		odi_gpon_irq_attach();

	/* The boot gponact replays gpon_init.bin, and loading it sleeps, so it
	 * is loaded here, in process context (the /proc/odi_init write),
	 * before the spinlock below; the replay under the lock only reads it.
	 * Only the first gponact needs it (odi_gpon_booted), and it is
	 * released again at the end of this call, so nothing stays resident.
	 * An unlocked read is enough to decide whether to load: every verb
	 * runs under odi_switch_lock (odi_init.c), and the flag is
	 * checked again under odi_gpon_lock.
	 */
	if ((!strcmp(verb, "gponact") && !READ_ONCE(odi_gpon_booted)) ||
	    (!strcmp(verb, "gponsn") && READ_ONCE(odi_gpon_booted)))
		init_fw_rc = odi_replay_fw_load(ODI_REPLAY_TABLE_GPON_INIT, &init_fw);

	odi_board_laser_get(&laser);
	spin_lock_irqsave(&odi_gpon_lock, flags);
	prev = odi_gpon_fsm_inst.state;

	if (!strcmp(verb, "gpondrv") || !strcmp(verb, "gpondev")) {
		/* No-op beyond the irq_attach() above: gpon_init.bin
		 * (odi_gpon_init.h) is one flat table covering
		 * gpondrv+gpondev+gponsn+gponact's own boot-time writes in the
		 * real capture's own order (odi_gpon_init.h's own header) --
		 * applied once, at "gponact" below, once the real serial
		 * number (from "gponsn") is known.
		 */
	} else if (!strcmp(verb, "gponsn")) {
		uint8_t sn[8];

		rc = odi_gpon_parse_sn(arg, sn);
		if (rc == 0 && odi_gpon_booted) {
			/* After boot the serial number that reaches the line is
			 * the one in the armed PLOAM slot, so a new one has to
			 * rewrite that slot, and only while the ONU is
			 * deactivated: the OLT assigns an ONU-ID to the serial
			 * it heard, and the driver compares the next
			 * assignment with the one it holds.
			 */
			if (odi_gpon_fsm_inst.state != ODI_GPON_STATE_O1 &&
			    !(odi_gpon_fsm_inst.state == ODI_GPON_STATE_O7 && odi_gpon_hw_hold_get()))
				rc = -EBUSY;
			else if (init_fw_rc)
				rc = init_fw_rc;
			else if (!odi_gpon_init_apply_serial(&init_fw.blob, sn))
				rc = -ENOENT;
		}
		if (rc == 0)
			odi_gpon_fsm_set_serial_number(&odi_gpon_fsm_inst, sn);
	} else if (!strcmp(verb, "gponpw")) {
		uint8_t pw[10];

		rc = odi_gpon_parse_pw(arg, pw);
		if (rc == 0)
			odi_gpon_fsm_set_password(&odi_gpon_fsm_inst, pw);
		else	/* the length only: the text is the secret */
			pr_warn("odi_gpon: gponpw: %zu characters is not a usable PLOAM password (hex up to 20 digits, or text up to 10), keeping the old one\n",
				arg ? strlen(arg) : (size_t)0);
	} else if (!strcmp(verb, "gponact") && !laser.laser_on) {
		/* The laser verb kept the laser off (odi_board.h): an
		 * unknown laser driver may light outside the ONU's timeslots
		 * and take down every other ONU on the OLT port, so the MAC
		 * is not started either.
		 */
		pr_err("odi_gpon: gponact refused: the laser is off (laser verb: laser driver id %d)\n",
		       laser.ldd_id);
		rc = -EPERM;
	} else if (!strcmp(verb, "gponact")) {
		if (!odi_gpon_booted && init_fw_rc) {
			/* No boot table (odi_replay_fw_load() logged why):
			 * nothing is written and the ONU is not activated. The
			 * flag stays clear, so the next gponact tries again.
			 */
			pr_err("odi_gpon: gponact: no boot init table (%d), not activating\n",
			       init_fw_rc);
			rc = init_fw_rc;
		} else if (!odi_gpon_booted) {
			odi_gpon_init_apply(&init_fw.blob, odi_gpon_fsm_inst.serial_number,
					    odi_gpon_fsm_inst.password);
			/* The captured boot sequence ends with the FSM's own
			 * O1->O2 self-transition inside this same odi_init
			 * call -- the registers are already written by the
			 * replay above; this poke brings this driver's own
			 * software state in line with them without re-issuing
			 * those writes through the ops table a second time
			 * (the same direct fsm->state poke
			 * odi_gpon_verb_deactivate() already uses).
			 */
			if (odi_gpon_hw_hold_get()) {
				/* GPON_KEEP_DISABLE: the OLT disabled this ONU
				 * and has not enabled it since; it starts in
				 * O7, the line already high (gponhold 1).
				 */
				odi_gpon_hw_hold_enter(&odi_gpon_fsm_inst);
			} else {
				odi_gpon_fsm_inst.state = ODI_GPON_STATE_O2;
				odi_gpon_fsm_inst.onu_id = 0U;
			}
			odi_gpon_booted = true;
			/* The block is set up: the counter sampler starts, with
			 * its baseline (the first sample only clears) taken
			 * here rather than a period later. The downstream
			 * framer runs from the replay above, and a PLOAM it
			 * accepts in that first second would reach ds_rx but
			 * be cleared out of ds_ploam_accepted by a late
			 * baseline: the two would differ for good.
			 */
			odi_gpon_ctr_sample_locked();
			mod_timer(&odi_gpon_ctr_timer, jiffies + msecs_to_jiffies(ODI_GPON_CTR_PERIOD_MS));
		} else {
			odi_gpon_verb_activate(&odi_gpon_fsm_inst);
		}
	} else if (!strcmp(verb, "gpondg")) {
		if (arg && (!strcmp(arg, "0") || !strcmp(arg, "1")))
			odi_gpon_dg_enabled = (arg[0] == '1');
		else
			rc = -EINVAL;
	} else if (!strcmp(verb, "gponkeep")) {
		/* GPON_KEEP_DISABLE=1, written at boot by rcs-lib.sh. */
		if (arg && (!strcmp(arg, "0") || !strcmp(arg, "1")))
			odi_gpon_hw_hold_enable(arg[0] == '1');
		else
			rc = -EINVAL;
	} else if (!strcmp(verb, "gponhold")) {
		rc = odi_gpon_verb_hold(arg);
	} else if (!strcmp(verb, "gpondeact")) {
		odi_gpon_verb_deactivate(&odi_gpon_fsm_inst);
	} else if (!strcmp(verb, "gponstat")) {
		/* A trigger only -- odi_gpon owns its own return convention
		 * now (odi_gpon.h's own header comment), not the stock
		 * error-code-then-state-code range /proc/odi_init used to read
		 * back. /proc/odi_gpon/status is the real status surface.
		 */
		rc = 0;
	} else {
		rc = -ENOENT;
	}

	/* An activated ONU follows the line from here on, from the first
	 * poll ODI_GPON_LINE_POLL_MS later or the first interrupt; a
	 * deactivated one is left in O1.
	 */
	if (!strcmp(verb, "gponact") && rc == 0)
		odi_gpon_line_watch = true;
	else if (!strcmp(verb, "gpondeact"))
		odi_gpon_line_watch = false;

	{
		/* A verb is this side acting, whatever it does to the state:
		 * the boot gponact, a respawn re-provisioning or `apply.sh
		 * omci` (gpondeact, then gponact), by hand at a shell. */
		struct odi_gpon_fsm_cause cause = { odi_gpon_verb_name(verb), "local" };

		odi_gpon_note_transition(prev, &cause);
	}
	odi_gpon_timers_sync();
	spin_unlock_irqrestore(&odi_gpon_lock, flags);
	if (!init_fw_rc)
		odi_replay_fw_release(&init_fw);
	return rc;
}

int odi_gpon_onu_state(void)
{
	unsigned long flags;
	int st;

	spin_lock_irqsave(&odi_gpon_lock, flags);
	st = (int)odi_gpon_fsm_inst.state;
	spin_unlock_irqrestore(&odi_gpon_lock, flags);
	return st;
}

int odi_gpon_onu_id_get(void)
{
	unsigned long flags;
	int id;

	spin_lock_irqsave(&odi_gpon_lock, flags);
	id = odi_gpon_fsm_inst.onu_id ? (int)odi_gpon_fsm_inst.onu_id : -1;
	spin_unlock_irqrestore(&odi_gpon_lock, flags);
	return id;
}

void odi_gpon_sn_get(u8 sn[8])
{
	unsigned long flags;

	spin_lock_irqsave(&odi_gpon_lock, flags);
	memcpy(sn, odi_gpon_fsm_inst.serial_number, 8);
	spin_unlock_irqrestore(&odi_gpon_lock, flags);
}

int odi_gpon_alloc_row(uint16_t alloc_id)
{
	unsigned long flags;
	int row;

	spin_lock_irqsave(&odi_gpon_lock, flags);
	row = odi_gpon_hw_alloc_id_find(alloc_id);
	spin_unlock_irqrestore(&odi_gpon_lock, flags);
	return row;
}

int odi_gpon_signal_threshold_set(unsigned int signal, unsigned int exp)
{
	unsigned long flags;
	int rc;

	spin_lock_irqsave(&odi_gpon_lock, flags);
	rc = odi_gpon_ctr_signal_threshold_set(&odi_gpon_ctr_inst, signal, exp);
	spin_unlock_irqrestore(&odi_gpon_lock, flags);
	return rc;
}

void odi_gpon_eqd_get(struct odi_gpon_eqd *eqd)
{
	uint32_t mf, inf;

	odi_gpon_get_eqd(&mf, &inf);
	eqd->multiframe = mf;
	eqd->inframe = inf;
}

void odi_gpon_ploam_counts_get(struct odi_gpon_ploam_counts *counts)
{
	unsigned int rx, tx;

	odi_gpon_get_ploam_counts(&rx, &tx);
	counts->ds_rx = rx;
	counts->us_tx = tx;
	counts->us_dropped = odi_gpon_get_ploam_us_dropped();
}

void odi_gpon_ploam_ring_get(struct odi_gpon_ploam_entry *ring, unsigned int *count)
{
	unsigned long flags;
	unsigned int i, start;

	spin_lock_irqsave(&odi_gpon_lock, flags);
	*count = odi_gpon_ring_count;
	start = (odi_gpon_ring_head + ODI_GPON_PLOAM_RING_LEN - odi_gpon_ring_count) % ODI_GPON_PLOAM_RING_LEN;
	for (i = 0; i < odi_gpon_ring_count; i++)
		ring[i] = odi_gpon_ring[(start + i) % ODI_GPON_PLOAM_RING_LEN];
	spin_unlock_irqrestore(&odi_gpon_lock, flags);
}

/* ---- /proc/odi_gpon/ ---------------------------------------------------
 *
 * One read-only file per topic, like /proc/odi_wdt/: status (the ONU state
 * and everything that describes the link now), counters (the sampled
 * totals), flows (the per-flow GEM totals) and ploam (the PLOAM traffic and
 * its ring).
 */

static void odi_gpon_proc_put(void *ctx, const char *line)
{
	seq_puts(ctx, line);
}

/* The sampler state, from a copy taken under the lock: the formatting runs
 * with interrupts on.
 */
static struct odi_gpon_ctr *odi_gpon_proc_ctr_snap(void)
{
	struct odi_gpon_ctr *snap = kmalloc(sizeof(*snap), GFP_KERNEL);
	unsigned long flags;

	if (!snap)
		return NULL;
	spin_lock_irqsave(&odi_gpon_lock, flags);
	*snap = odi_gpon_ctr_inst;
	spin_unlock_irqrestore(&odi_gpon_lock, flags);
	return snap;
}

/* The most ports the status file lists as decrypted: the 64 DS slots cmd 25
 * accepts, so every one of them fits. */
#define ENC_PORTS_MAX	64U

/* /proc/odi_gpon/status: the ONU state, ONU-ID, serial number, EqD, the
 * Alloc-IDs, the decrypted GEM ports, the REI count and the TO1/TO2 expiries, the interrupt
 * bookkeeping, the laser, the last LOS edge, the last Upstream_Overhead,
 * Ranging_Time and PON-ID, then the sampler's live bits, alarm and ONU state times and
 * state transitions.
 */
static int odi_gpon_proc_status_show(struct seq_file *seq, void *v)
{
	struct odi_gpon_eqd eqd;
	struct odi_gpon_ctr *snap;
	unsigned int i;
	u8 sn[8];
	unsigned long flags;
	unsigned int irq_count, to1_exp, to2_exp, rei_count;
	unsigned long last_los;
	int last_los_state;
	bool irq_attached;
	int irq_isr_rc, irq_imr_rc;
	unsigned int irq_spurious;
	uint32_t last_top_sts, last_ds_dlt, last_us_sts;
	uint16_t alloc_ids[32];
	unsigned int n_alloc;
	struct odi_gpon_ds_upstream_overhead uo;
	struct odi_gpon_ds_ranging_time rng;
	struct odi_gpon_ds_pon_id pid;
	u32 pid_ms;
	struct odi_gpon_ranging_dbg rdbg;
	bool uo_valid, rng_valid, pid_valid;
	int hold_keep, hold_held;

	(void)v;

	odi_gpon_eqd_get(&eqd);
	odi_gpon_sn_get(sn);

	spin_lock_irqsave(&odi_gpon_lock, flags);
	irq_count = odi_gpon_irq_count;
	to1_exp = odi_gpon_to1_expiries;
	to2_exp = odi_gpon_to2_expiries;
	rei_count = odi_gpon_rei_count;
	last_los = odi_gpon_last_los_ms;
	last_los_state = odi_gpon_last_los_state;
	irq_attached = odi_gpon_irq_attached;
	irq_isr_rc = odi_gpon_irq_isr_rc;
	irq_imr_rc = odi_gpon_irq_imr_rc;
	irq_spurious = odi_gpon_irq_spurious_count;
	last_top_sts = odi_gpon_last_top_sts_nonzero;
	last_ds_dlt = odi_gpon_last_ds_dlt_nonzero;
	last_us_sts = odi_gpon_last_us_sts_nonzero;
	n_alloc = odi_gpon_get_alloc_ids(alloc_ids, ARRAY_SIZE(alloc_ids));
	uo = odi_gpon_last_uo;
	uo_valid = odi_gpon_last_uo_valid;
	rng = odi_gpon_last_rng;
	rng_valid = odi_gpon_last_rng_valid;
	pid = odi_gpon_last_pon_id;
	pid_valid = odi_gpon_last_pon_id_valid;
	pid_ms = odi_gpon_last_pon_id_ms;
	odi_gpon_get_ranging_dbg(&rdbg);
	hold_keep = odi_gpon_hw_hold_enabled();
	hold_held = odi_gpon_hw_hold_get();
	spin_unlock_irqrestore(&odi_gpon_lock, flags);

	{
		int state = odi_gpon_onu_state();

		seq_printf(seq, "state %d (%s)\n", state, odi_gpon_state_name((enum odi_gpon_state)state));
	}
	/* GPON_KEEP_DISABLE: keep 1 = the feature is on; held 1 = Emergency
	 * Stop is kept (the OLT disabled this ONU and has not enabled it).
	 */
	seq_printf(seq, "o7_hold keep %d held %d\n", hold_keep, hold_held);
	seq_printf(seq, "onu_id %d\n", odi_gpon_onu_id_get());
	seq_printf(seq, "sn %02x%02x%02x%02x%02x%02x%02x%02x\n",
		   sn[0], sn[1], sn[2], sn[3], sn[4], sn[5], sn[6], sn[7]);
	seq_printf(seq, "eqd multiframe %u inframe %u\n", eqd.multiframe, eqd.inframe);
	/* The Alloc-IDs the OLT assigned (Assign_Alloc-ID), one line: the
	 * count, then each id. metricsd reads it for gpon_provision_tcont_info.
	 */
	seq_printf(seq, "alloc_ids %u", n_alloc);
	for (i = 0; i < n_alloc; i++)
		seq_printf(seq, " %u", (unsigned int)alloc_ids[i]);
	seq_putc(seq, '\n');
	/* The GEM ports the downstream path decrypts, the same shape: what
	 * the OLT's Encrypted_Port-IDs left on (odi_switch_gpon_aes_ports()).
	 * omcid answers the GEM port network CTP's Encryption state from it.
	 */
	{
		uint16_t enc[ENC_PORTS_MAX];
		unsigned int n_enc = odi_switch_gpon_aes_ports(enc, ARRAY_SIZE(enc));

		seq_printf(seq, "enc_ports %u", n_enc);
		for (i = 0; i < n_enc; i++)
			seq_printf(seq, " %u", (unsigned int)enc[i]);
		seq_putc(seq, '\n');
	}

	seq_printf(seq, "rei_count %u\n", rei_count);
	seq_printf(seq, "to1_expiries %u\n", to1_exp);
	seq_printf(seq, "to2_expiries %u\n", to2_exp);
	seq_printf(seq, "irq_count %u\n", irq_count);
	seq_printf(seq, "irq_spurious %u\n", irq_spurious);
	seq_printf(seq, "irq_last_top_sts 0x%08x irq_last_ds_dlt 0x%08x irq_last_us_sts 0x%08x\n",
		   last_top_sts, last_ds_dlt, last_us_sts);
	seq_printf(seq, "irq_attached %d isr_rc %d imr_rc %d\n",
		   irq_attached ? 1 : 0, irq_isr_rc, irq_imr_rc);
	{
		struct odi_board_laser laser;

		odi_board_laser_get(&laser);
		seq_printf(seq, "laser on %d ldd_id %d unverified %d\n",
			   laser.laser_on, laser.ldd_id, laser.unverified);
		seq_printf(seq, "laser_emergency_stop %d\n", laser.emergency_stop);
	}
	seq_printf(seq, "last_los_ms %lu state %d\n", last_los, last_los_state);

	/* The last Upstream_Overhead (G.984.3 9.2.3.1) and Ranging_Time
	 * (9.2.3.4) the OLT sent, decoded. The pre-assigned delay is in units
	 * of 32 bytes, so 32 * 8 bits each.
	 */
	if (uo_valid)
		seq_printf(seq, "upstream_overhead guard %u type1 %u type2 %u pattern 0x%02x delimiter %02x%02x%02x preassigned_delay_en %u sn_mask %u extra_sn_tx %u power_level_mode %u preassigned_delay %u bits %u\n",
			   uo.guard_bits, uo.type1_preamble_bits, uo.type2_preamble_bits,
			   uo.type3_pattern, uo.delimiter[0], uo.delimiter[1], uo.delimiter[2],
			   uo.preassigned_delay_en, uo.sn_mask_enabled, uo.extra_sn_tx,
			   uo.power_level_mode, (unsigned int)uo.preassigned_delay,
			   (unsigned int)uo.preassigned_delay * 32U * 8U);
	else
		seq_puts(seq, "upstream_overhead none\n");
	if (rng_valid)
		seq_printf(seq, "ranging_time eqd_bits %u protection_path %u min_resp_delay 0x%08x multframe %u inframe %u\n",
			   rng.eqd, rng.protection_path, rdbg.min_resp_delay,
			   rdbg.multframe, rdbg.inframe);
	else
		seq_puts(seq, "ranging_time none\n");
	/* The last PON-ID (Annex C.2.2) since the last loss of signal: the
	 * fields as sent, TOL raw (0.1 dB steps above -30 dBm, 65535 for not
	 * reported), and when it came.
	 */
	if (pid_valid)
		seq_printf(seq, "pon_id odn_class %u reach_extender %u tol %u id %*phN ms %u\n",
			   pid.odn_class, pid.reach_extender, pid.tol,
			   (int)ODI_GPON_PON_ID_LEN, pid.id, pid_ms);
	else
		seq_puts(seq, "pon_id none\n");

	snap = odi_gpon_proc_ctr_snap();
	if (snap) {
		odi_gpon_ctr_show_status(snap, odi_gpon_proc_put, seq);
		kfree(snap);
	}

	return 0;
}

/* /proc/odi_gpon/counters: the sampler's sample count and 64-bit totals. */
static int odi_gpon_proc_counters_show(struct seq_file *seq, void *v)
{
	struct odi_gpon_ctr *snap = odi_gpon_proc_ctr_snap();

	(void)v;
	if (snap) {
		odi_gpon_ctr_show_counters(snap, odi_gpon_proc_put, seq);
		kfree(snap);
	}
	return 0;
}

/* /proc/odi_gpon/flows: the per-flow GEM counters, from a copy taken under
 * the lock.
 */
static int odi_gpon_proc_flows_show(struct seq_file *seq, void *v)
{
	struct odi_gpon_flows *snap = kmalloc(sizeof(*snap), GFP_KERNEL);
	unsigned long flags;

	(void)v;
	if (!snap)
		return 0;
	spin_lock_irqsave(&odi_gpon_lock, flags);
	*snap = odi_gpon_flows_inst;
	spin_unlock_irqrestore(&odi_gpon_lock, flags);
	odi_gpon_flows_show(snap, odi_gpon_proc_put, seq);
	kfree(snap);
	return 0;
}

/* /proc/odi_gpon/ploam: the PLOAM message totals, the per-type counts and
 * the ring of the last messages.
 */
static int odi_gpon_proc_ploam_show(struct seq_file *seq, void *v)
{
	struct odi_gpon_ploam_counts counts;
	struct odi_gpon_ploam_entry ring[ODI_GPON_PLOAM_RING_LEN];
	unsigned int count = 0, i;

	(void)v;

	odi_gpon_ploam_counts_get(&counts);
	odi_gpon_ploam_ring_get(ring, &count);

	seq_printf(seq, "ploam ds_rx %u us_tx %u\n", counts.ds_rx, counts.us_tx);
	/* Replies the upstream queue had no room for within the bounded wait
	 * (odi_gpon_hw.c, hw_send_us_ploam()); not part of us_tx.
	 */
	seq_printf(seq, "ploam_us_dropped %u\n", counts.us_dropped);

	for (i = 0; i < 256U; i++) {
		if (odi_gpon_ds_type_count[i])
			seq_printf(seq, "ploam_type ds 0x%02x %u\n", i, odi_gpon_ds_type_count[i]);
	}
	for (i = 0; i < 256U; i++) {
		if (odi_gpon_us_type_count[i])
			seq_printf(seq, "ploam_type us 0x%02x %u\n", i, odi_gpon_us_type_count[i]);
	}

	seq_printf(seq, "ploam_ring %u entries\n", count);
	for (i = 0; i < count && i < ODI_GPON_PLOAM_RING_LEN; i++) {
		char content[ODI_GPON_PLOAM_RING_CONTENT_CHARS];

		/* The live AES key of an Encryption_Key is masked: this file
		 * is world-readable (odi_gpon_ploam_ring_content()).
		 */
		odi_gpon_ploam_ring_content(ring[i].direction == ODI_GPON_PLOAM_US,
					    ring[i].type, ring[i].content, content);
		seq_printf(seq, "  %u %s onu_id 0x%02x type 0x%02x content %s\n",
			   ring[i].timestamp_ms,
			   ring[i].direction == ODI_GPON_PLOAM_US ? "us" : "ds",
			   ring[i].onu_id, ring[i].type, content);
	}

	return 0;
}

/* odi_gpon_drop_show()'s state names: enum odi_gpon_state by number. */
static const char *odi_gpon_drop_state_name(unsigned int st)
{
	return odi_gpon_state_name((enum odi_gpon_state)st);
}

/* /proc/odi_gpon/drops: the records frozen around the drops out of O5,
 * from a copy taken under the lock (about 40 KB, so kvmalloc), formatted
 * with interrupts on.
 */
static int odi_gpon_proc_drops_show(struct seq_file *seq, void *v)
{
	struct odi_gpon_drop *snap = kvmalloc(sizeof(*snap), GFP_KERNEL);
	unsigned long flags;

	(void)v;
	if (!snap)
		return -ENOMEM;
	spin_lock_irqsave(&odi_gpon_lock, flags);
	*snap = odi_gpon_drop_inst;
	spin_unlock_irqrestore(&odi_gpon_lock, flags);
	odi_gpon_drop_show(snap, odi_gpon_now_ms(), odi_gpon_drop_state_name,
			   odi_gpon_proc_put, seq);
	kvfree(snap);
	return 0;
}

static int odi_gpon_proc_drops_open(struct inode *inode, struct file *file)
{
	return single_open_size(file, odi_gpon_proc_drops_show, NULL, 256 * 1024);
}

static const struct proc_ops odi_gpon_proc_drops_fops = {
	.proc_open = odi_gpon_proc_drops_open, .proc_read = seq_read,
	.proc_lseek = seq_lseek, .proc_release = single_release,
};

/* /proc/odi_gpon/cam: the Alloc-ID CAM read back from the hardware, one
 * line per row that is valid in the hardware or recorded by the driver:
 * "alloc_cam <row> hw <Alloc-ID|-> recorded <Alloc-ID|->". Row 16 is the
 * default Alloc-ID, the ONU-ID. Only these rows decide which upstream
 * grants the ONU answers; status's alloc_ids is the driver's record.
 * Then the downstream GEM port CAM, rows 0..64, one line per row valid in
 * the hardware or with a flow type word that is not zero:
 * "gem_cam <row> hw <GEM port|-> type 0x<FLAGS>". Rows 0..63 are cmd 25's
 * flows (row = flow id), 64 the OMCC's; a flow cmd 25 deleted leaves no
 * line.
 */
static int odi_gpon_proc_cam_show(struct seq_file *seq, void *v)
{
	int hw[32], rec[32], gem[ODI_GPON_HW_GEM_CAM_ROWS];
	uint32_t type[ODI_GPON_HW_GEM_CAM_ROWS];
	unsigned long flags;
	unsigned int row, valid = 0;

	(void)v;
	spin_lock_irqsave(&odi_gpon_lock, flags);
	odi_gpon_hw_alloc_cam_read(hw, rec);
	spin_unlock_irqrestore(&odi_gpon_lock, flags);
	for (row = 0; row < 32U; row++) {
		if (hw[row] == -1 && rec[row] < 0)
			continue;
		if (hw[row] >= 0)
			valid++;
		seq_printf(seq, "alloc_cam %u hw ", row);
		if (hw[row] >= 0)
			seq_printf(seq, "%d", hw[row]);
		else
			seq_puts(seq, hw[row] == -2 ? "timeout" : "-");
		if (rec[row] >= 0)
			seq_printf(seq, " recorded %d\n", rec[row]);
		else
			seq_puts(seq, " recorded -\n");
	}
	seq_printf(seq, "alloc_cam_valid %u\n", valid);

	spin_lock_irqsave(&odi_gpon_lock, flags);
	odi_gpon_hw_gem_cam_read(gem, type);
	spin_unlock_irqrestore(&odi_gpon_lock, flags);
	valid = 0;
	for (row = 0; row < ODI_GPON_HW_GEM_CAM_ROWS; row++) {
		if (gem[row] == -1 && !type[row])
			continue;
		if (gem[row] >= 0)
			valid++;
		seq_printf(seq, "gem_cam %u hw ", row);
		if (gem[row] >= 0)
			seq_printf(seq, "%d", gem[row]);
		else
			seq_puts(seq, gem[row] == -2 ? "timeout" : "-");
		seq_printf(seq, " type 0x%02x\n", (unsigned int)type[row]);
	}
	seq_printf(seq, "gem_cam_valid %u\n", valid);
	return 0;
}

static int odi_gpon_proc_cam_open(struct inode *inode, struct file *file)
{
	return single_open(file, odi_gpon_proc_cam_show, NULL);
}

static const struct proc_ops odi_gpon_proc_cam_fops = {
	.proc_open = odi_gpon_proc_cam_open, .proc_read = seq_read,
	.proc_lseek = seq_lseek, .proc_release = single_release,
};

static int odi_gpon_proc_status_open(struct inode *inode, struct file *file)
{
	return single_open(file, odi_gpon_proc_status_show, NULL);
}

static int odi_gpon_proc_counters_open(struct inode *inode, struct file *file)
{
	return single_open(file, odi_gpon_proc_counters_show, NULL);
}

static int odi_gpon_proc_flows_open(struct inode *inode, struct file *file)
{
	return single_open(file, odi_gpon_proc_flows_show, NULL);
}

static int odi_gpon_proc_ploam_open(struct inode *inode, struct file *file)
{
	return single_open(file, odi_gpon_proc_ploam_show, NULL);
}

static const struct proc_ops odi_gpon_proc_status_fops = {
	.proc_open = odi_gpon_proc_status_open, .proc_read = seq_read,
	.proc_lseek = seq_lseek, .proc_release = single_release,
};

static const struct proc_ops odi_gpon_proc_counters_fops = {
	.proc_open = odi_gpon_proc_counters_open, .proc_read = seq_read,
	.proc_lseek = seq_lseek, .proc_release = single_release,
};

static const struct proc_ops odi_gpon_proc_flows_fops = {
	.proc_open = odi_gpon_proc_flows_open, .proc_read = seq_read,
	.proc_lseek = seq_lseek, .proc_release = single_release,
};

static const struct proc_ops odi_gpon_proc_ploam_fops = {
	.proc_open = odi_gpon_proc_ploam_open, .proc_read = seq_read,
	.proc_lseek = seq_lseek, .proc_release = single_release,
};

static struct proc_dir_entry *odi_gpon_proc_dir;

/* Dying_Gasp (G.984.3 clause 9.2.4.3) before an orderly restart, halt or
 * power-off, only when GPON_DYING_GASP enabled it and the ONU is in O4 or
 * O5. Never from the PON-down path: another open implementation's
 * measurements on this chip found that a Dying_Gasp followed by no restart
 * made the OLT deactivate and re-activate the ONU about 4 s later. On a
 * reboot in O5 all three copies have been seen to leave, on our stick and
 * in their later measurements (G.984.3 tells the OLT not to stop
 * allocations on a Dying_Gasp alone), though their first test OLT granted
 * nothing after the first. So up to three copies (the standard says at
 * least three), each given 50 ms to leave the queue, stopping at the first
 * that does not. Runs in process context; the register poll is bounded.
 */
#define ODI_GPON_DG_COPIES	3U
#define ODI_GPON_DG_LEAVE_US	50000U

static int odi_gpon_reboot_notify(struct notifier_block *nb, unsigned long action, void *data)
{
	unsigned int sent = 0;
	unsigned long flags;

	(void)nb;
	(void)action;
	(void)data;

	while (sent < ODI_GPON_DG_COPIES) {
		struct odi_gpon_ploam dg;
		bool go = false;

		spin_lock_irqsave(&odi_gpon_lock, flags);
		if (odi_gpon_dg_enabled && (sent || !odi_gpon_dg_done) &&
		    (odi_gpon_fsm_inst.state == ODI_GPON_STATE_O4 ||
		     odi_gpon_fsm_inst.state == ODI_GPON_STATE_O5) &&
		    odi_gpon_fsm_inst.onu_id) {
			odi_gpon_dg_done = true;
			odi_gpon_encode_dying_gasp(odi_gpon_fsm_inst.onu_id, &dg);
			go = true;
		}
		spin_unlock_irqrestore(&odi_gpon_lock, flags);
		if (!go)
			break;
		if (odi_gpon_hw_send_us_ploam_wait(&dg, ODI_GPON_DG_LEAVE_US))
			break;
		sent++;
	}
	if (sent || odi_gpon_dg_done)
		pr_info("odi_gpon: Dying_Gasp sent %u of %u times before the restart\n",
			sent, ODI_GPON_DG_COPIES);
	return NOTIFY_DONE;
}

static struct notifier_block odi_gpon_reboot_nb = {
	.notifier_call = odi_gpon_reboot_notify,
};

static int __init odi_gpon_module_init(void)
{
	register_reboot_notifier(&odi_gpon_reboot_nb);

	int rc = odi_gpon_init();

	if (rc)
		pr_info("odi_gpon: init failed, rc=%d\n", rc);
	odi_gpon_proc_dir = proc_mkdir("odi_gpon", NULL);
	if (!odi_gpon_proc_dir) {
		pr_info("odi_gpon: /proc/odi_gpon not created\n");
		return 0;
	}
	if (!proc_create("status", 0444, odi_gpon_proc_dir, &odi_gpon_proc_status_fops))
		pr_info("odi_gpon: /proc/odi_gpon/status not created\n");
	if (!proc_create("counters", 0444, odi_gpon_proc_dir, &odi_gpon_proc_counters_fops))
		pr_info("odi_gpon: /proc/odi_gpon/counters not created\n");
	if (!proc_create("flows", 0444, odi_gpon_proc_dir, &odi_gpon_proc_flows_fops))
		pr_info("odi_gpon: /proc/odi_gpon/flows not created\n");
	if (!proc_create("ploam", 0444, odi_gpon_proc_dir, &odi_gpon_proc_ploam_fops))
		pr_info("odi_gpon: /proc/odi_gpon/ploam not created\n");
	if (!proc_create("cam", 0444, odi_gpon_proc_dir, &odi_gpon_proc_cam_fops))
		pr_info("odi_gpon: /proc/odi_gpon/cam not created\n");
	if (!proc_create("drops", 0444, odi_gpon_proc_dir, &odi_gpon_proc_drops_fops))
		pr_info("odi_gpon: /proc/odi_gpon/drops not created\n");
	return 0; /* never fail module_init over odi_gpon_init()'s own result */
}
module_init(odi_gpon_module_init);

#endif /* __KERNEL__ */
