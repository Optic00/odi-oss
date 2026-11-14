/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */

/* Reload on SIGHUP: reread the config store and act on what changed.
 *
 * SIGHUP is the Unix convention for "reread your configuration", and it is
 * what /etc/scripts/apply.sh omci sends. omcid compares what it reads with
 * what it is running and picks one of three things:
 *
 *   nothing changed      log it, do nothing;
 *   only VLAN handling   tear the bridge connections down and build them
 *                        again from the MIB it already holds, by the same
 *                        bdgconn_rebuild() the initial build uses. The ONU
 *                        stays in O5, nothing is re-ranged;
 *   anything the OLT     deactivate the ONU, clear the MIB as a MIB reset
 *   must see again       does, hand the driver the new serial number and
 *                        PLOAM password, activate, and let the OLT
 *                        provision from scratch. The process stays up.
 *
 * The handler sets a flag and returns; everything else runs from the main
 * loop (reload_poll), and the re-registration is a state machine advanced
 * by that loop, so no step sleeps: the watchdog ping, the netlink receive
 * and both command queues keep being served while the ONU is down.
 *
 * A SIGHUP while a re-registration runs (or several in a row) leaves the
 * flag set once; the next reload starts after the current one ends and
 * finds, normally, nothing changed.
 *
 * A changed identity is held back, not re-registered, after refusals the
 * OLT gave the last changes, until apply.sh confirms the reload (rereg.c).
 *
 * The outcome is written to RELOAD_STATUS_PATH for apply.sh, one key=value
 * per line, replaced whole by a rename: id (pid.sequence, so a respawned
 * omcid is not mistaken for the run that was asked), state (running or
 * done), changed, action, result, keys, duration_ms, o5_ms, services.
 */
#include "omcid.h"
#include "../procparse.h"

volatile int reload_pending;

/* The driver verbs go through /proc/odi_init, as rcS and apply.sh always
 * did; -i points it at a file for the test. */
const char *odi_init_path = ODI_INIT_PATH;

/* How the ONU is held down, and how long it may take to come back. The
 * three seconds are the measured forced re-activation (O1 to O5 in about
 * five), the wait covers the OLT provisioning the services again. -j sets
 * the wait for the test. */
#define RL_DEACT_HOLD_S 3
#define RL_O5_WAIT_S    150
long reload_o5_wait_s = RL_O5_WAIT_S;

enum { RL_IDLE, RL_HOLD, RL_WAIT_O5 };

static struct {
	int phase;
	unsigned seq;
	unsigned mask;
	char keys[200];
	const char *changed, *action;
	long start_s, start_ms;          /* the reload began */
	long hold_s, hold_ms;            /* the ONU went down */
	long act_s, act_ms;              /* gponact was written */
	long deadline_s;                 /* RL_WAIT_O5 gives up */
	long last_check_s;
	int sn_failed;
	int confirm;                     /* rereg_take_confirm() of this reload */
	long deact_base;                 /* Deactivate_ONU-IDs before gponact, -1 unknown */
	unsigned refusals;               /* and since */
	struct cfg_snap base;            /* what the running daemon holds */
} rl;

void reload_on_hup(int sig)
{
	(void)sig;
	reload_pending = 1;
}

void reload_init(void)
{
	cfg_snap_take(&rl.base);
	rereg_load();
	/* A status file of an earlier omcid says nothing about this one. */
	sys_unlink(RELOAD_STATUS_PATH);
}

static void now_ms(long *s, long *ms)
{
	long ns = 0;

	*s = 0;
	*ms = 0;
	if (sys_clock_gettime(CLOCK_MONOTONIC, s, &ns) == 0)
		*ms = ns / 1000000;
}

static long since_ms(long s, long ms)
{
	long ns, nms;

	now_ms(&ns, &nms);
	return (ns - s) * 1000 + (nms - ms);
}

/* ---------------------------------------------------------- the status file */

struct sbuf {
	char b[512];
	int n;
};

static void sput(struct sbuf *s, const char *t)
{
	while (*t && s->n < (int)sizeof s->b - 1)
		s->b[s->n++] = *t++;
}

static void sputu(struct sbuf *s, unsigned long v)
{
	char t[12];
	int i = 0;

	do {
		t[i++] = (char)('0' + v % 10);
		v /= 10;
	} while (v && i < (int)sizeof t);
	while (i && s->n < (int)sizeof s->b - 1)
		s->b[s->n++] = t[--i];
}

static void sline(struct sbuf *s, const char *key, const char *val)
{
	sput(s, key);
	sput(s, "=");
	sput(s, val);
	sput(s, "\n");
}

static void sline_u(struct sbuf *s, const char *key, unsigned long v)
{
	sput(s, key);
	sput(s, "=");
	sputu(s, v);
	sput(s, "\n");
}

static unsigned services_in_use(void)
{
	unsigned n = 0;

	for (int i = 0; i < SERV_MAX; i++)
		n += servtab[i].in_use ? 1u : 0u;
	return n;
}

static void status_write(int done, const char *result, long o5_ms)
{
	struct sbuf s;

	s.n = 0;
	sput(&s, "id=");
	sputu(&s, (unsigned long)sys_getpid());
	sput(&s, ".");
	sputu(&s, rl.seq);
	sput(&s, "\n");
	sline(&s, "state", done ? "done" : "running");
	sline(&s, "changed", rl.changed);
	sline(&s, "action", rl.action);
	sline(&s, "result", result);
	sline(&s, "keys", rl.keys);
	sline_u(&s, "duration_ms", (unsigned long)since_ms(rl.start_s, rl.start_ms));
	if (o5_ms >= 0)
		sline_u(&s, "o5_ms", (unsigned long)o5_ms);
	sline_u(&s, "services", services_in_use());
	if (str_eq(rl.changed, "identity")) {
		int today = rereg_today();

		sline_u(&s, "refusals", rl.refusals);
		sline_u(&s, "streak", rereg_streak());
		if (today >= 0)
			sline_u(&s, "today", (unsigned long)today);
	}
	(void)atomic_write(RELOAD_STATUS_PATH, RELOAD_STATUS_TMP_PATH,
			   (const uint8_t *)s.b, (uint32_t)s.n);
}

/* ------------------------------------------------------------ the driver */

/* One verb to /proc/odi_init, the write `echo verb > /proc/odi_init` makes.
 * It is one kernel call that returns when the verb has run (register
 * writes and a timer sync, no waiting on another process), so nothing here
 * can be bounded further from userspace; a kernel that never returned would
 * stop the watchdog ping and the board would reset, as for any hang in this
 * loop. Returns 0, or the negative errno. */
static long odi_verb(const char *verb, const char *arg)
{
	char line[64];
	int n = 0;
	long fd, rc;

	for (const char *p = verb; *p && n < (int)sizeof line - 2; p++)
		line[n++] = *p;
	if (arg && arg[0]) {
		line[n++] = ' ';
		for (const char *p = arg; *p && n < (int)sizeof line - 2; p++)
			line[n++] = *p;
	}
	line[n++] = '\n';
	fd = sys_open(odi_init_path, O_WRONLY | O_APPEND);
	if (fd < 0)
		return fd;
	rc = sys_write((int)fd, line, (unsigned long)n);
	sys_close((int)fd);
	return rc == n ? 0 : (rc < 0 ? rc : -5);
}

/* Read the ONU state number from the driver's /proc file: 5 is O5, -1 when
 * it cannot be read. */
int onu_state_now(void)
{
	char buf[128];
	long fd = sys_open(gpon_proc_path, O_RDONLY);
	long n;

	if (fd < 0)
		return -1;
	n = sys_read((int)fd, buf, sizeof buf);
	sys_close((int)fd);
	return n > 0 ? pp_gpon_state(buf, (unsigned)n) : -1;
}

/* 1 when the downstream path decrypts GEM port `port`: it is on the status
 * file's enc_ports line, which lists what the OLT asked to have decrypted
 * (the Encrypted_Port-ID PLOAM) as the driver applied it. 0 for a port that
 * is not on it, and for a file or line that is not there (no kernel of ours,
 * or one that does not publish it): unencrypted is what was always reported.
 * Read on every call, so a Get shows the state of that moment. */
int gem_port_encrypted(uint16_t port)
{
	static char buf[2048];
	uint16_t ids[64];
	long fd, n;
	int got;

	fd = sys_open(gpon_proc_path, O_RDONLY);
	if (fd < 0)
		return 0;
	n = sys_read((int)fd, buf, sizeof buf);
	sys_close((int)fd);
	if (n <= 0)
		return 0;
	got = pp_gpon_enc_ports(buf, (unsigned)n, ids, 64);
	for (int i = 0; i < got; i++)
		if (ids[i] == port)
			return 1;
	return 0;
}

/* ------------------------------------------------------------- the reload */

static void finish(const char *result, long o5_ms)
{
	long dur = since_ms(rl.start_s, rl.start_ms);
	int rereg = str_eq(rl.action, "reregister");

	/* Recorded before the status file, so the streak apply.sh reads
	 * there already counts this one. */
	if (rereg)
		rereg_record(rl.confirm == 2, result, rl.refusals, rl.keys);
	status_write(1, result, o5_ms);
	ev_reload_done(rl.changed, rl.action, result, dur, o5_ms,
		       services_in_use(), rereg ? (long)rl.refusals : -1);
	out_flush();
	rl.phase = RL_IDLE;
}

/* The VLAN keys only: tear down and build again from the MIB. */
static void do_rebuild(void)
{
	rl.changed = "vlan";
	rl.action = "rebuild";
	ev_reload(rl.changed, rl.action, rl.keys);
	status_write(0, "pending", -1);
	/* The same builder the quiet-second rebuild and the first build use,
	 * so the rules are the ones a fresh daemon builds from this MIB and
	 * this store. Whatever was pending is now moot. */
	conn_dirty = 0;
	bdgconn_rebuild();
	snapshot_save();
	finish("ok", -1);
}

/* An identity key changed: the first half, up to the hold. */
static void do_reregister(const struct cfg_snap *old)
{
	rl.changed = "identity";
	rl.action = "reregister";
	ev_reload(rl.changed, rl.action, rl.keys);
	status_write(0, "pending", -1);
	rl.sn_failed = 0;
	rl.refusals = 0;
	rl.deact_base = -1;

	if (apply_hw) {
		long rc = odi_verb("gpondeact", 0);

		ev_reload_step("gpondeact", rc);
		if (rc) {
			/* Nothing was touched: go back to the values the
			 * daemon was running with, so what it answers still
			 * matches what the OLT was told, and a next SIGHUP
			 * tries again. */
			cfg_snap_restore(old);
			finish("failed", -1);
			return;
		}
	}
	/* What the OLT's own MIB reset does, then the connections that MIB
	 * had built are taken out of the switch (the rebuild of an empty MIB
	 * deactivates every one it made). The snapshot goes with the reset:
	 * a respawn now must not resume a MIB the OLT is about to replace. */
	ev_mib_reset("local");
	mib_reset_all();
	conn_dirty = 0;
	qos_dirty = 0;
	bdgconn_rebuild();
	for (int i = 0; i < SERV_MAX; i++)
		servtab[i].in_use = 0;
	serial_forget();
	status_write(0, "deactivated", -1);
	rl.phase = RL_HOLD;
	now_ms(&rl.hold_s, &rl.hold_ms);
	out_flush();
}

/* The second half: the values into the driver, activation, then wait. */
static void activate(void)
{
	long rc;

	if (apply_hw) {
		const char *sn = cfg_sn_label(&ident);

		if ((rl.mask & CFGD_SN) && sn) {
			rc = odi_verb("gponsn", sn);
			ev_reload_step("gponsn", rc);
			if (rc)
				rl.sn_failed = 1;
		} else if (rl.mask & CFGD_SN) {
			/* Not a serial number the driver takes (twelve
			 * characters, the last eight hex). It keeps the old
			 * one, and the OLT keeps ranging on that. */
			ev_reload_step("gponsn", -22);
			rl.sn_failed = 1;
		}
		/* The password is written whenever there is one, as
		 * omci_reactivate does, and cleared with the bare verb when
		 * the key was just emptied: the driver then sends none. */
		if (ident.ploamLen > 0) {
			rc = odi_verb("gponpw", ident.ploam);
			ev_reload_step("gponpw", rc);
		} else if (rl.mask & CFGD_PLOAM) {
			rc = odi_verb("gponpw", 0);
			ev_reload_step("gponpw", rc);
		}
		/* The count the refusals are measured from: whatever the
		 * OLT sends after the activation is its answer to it. */
		rl.deact_base = rereg_ploam_deacts();
		rc = odi_verb("gponact", 0);
		ev_reload_step("gponact", rc);
		if (rc) {
			/* Left deactivated: better said loudly than retried
			 * blind. rcS and the old apply.sh said the same. */
			finish("failed", -1);
			return;
		}
	}
	now_ms(&rl.act_s, &rl.act_ms);
	rl.deadline_s = rl.act_s + reload_o5_wait_s;
	rl.last_check_s = 0;
	rl.phase = RL_WAIT_O5;
	status_write(0, "activated", -1);
	if (!apply_hw)
		finish("ok", -1);
}

/* The guard (rereg.c): the OLT refused the last changes, and nobody
 * confirmed this one. The daemon goes on with the identity it runs, the OLT
 * sees nothing, and a confirmed reload applies the store as it then is.
 * Nothing is recorded: a held change never reached the OLT. */
static void hold(const struct cfg_snap *old)
{
	cfg_snap_restore(old);
	rl.base = *old;
	rl.changed = "identity";
	rl.action = "held";
	ev_reload(rl.changed, rl.action, rl.keys);
	finish("held", -1);
}

static void do_reload(void)
{
	struct cfg_snap old, cur;
	unsigned mask;

	rl.seq++;
	now_ms(&rl.start_s, &rl.start_ms);
	/* Taken away by every reload, used or not: a confirmation is for the
	 * reload apply.sh asked for, never a later one. */
	rl.confirm = rereg_take_confirm();
	rl.refusals = 0;
	cfg_snap_take(&old);
	cfg_load_all();
	cfg_snap_take(&cur);
	mask = cfg_snap_diff(&rl.base, &cur);
	rl.mask = mask;
	cfg_diff_names(mask, rl.keys, sizeof rl.keys);

	if (!mask) {
		rl.changed = "none";
		rl.action = "none";
		ev_reload(rl.changed, rl.action, 0);
		finish("ok", -1);
		return;
	}
	/* The baseline moves with the decision: the daemon now runs the new
	 * values. do_reregister puts the old ones back if it cannot start. */
	rl.base = cur;
	if ((mask & CFGD_IDENTITY) && !rl.confirm &&
	    rereg_streak() >= REREG_HOLD) {
		hold(&old);
		return;
	}
	if (mask & CFGD_IDENTITY) {
		do_reregister(&old);
		if (rl.phase == RL_IDLE)
			rl.base = old;           /* gpondeact failed */
	} else {
		do_rebuild();
	}
}

void reload_poll(void)
{
	long s, ms;
	int st;

	if (rl.phase == RL_IDLE) {
		if (!reload_pending)
			return;
		reload_pending = 0;
		do_reload();
		return;
	}
	now_ms(&s, &ms);
	if (rl.phase == RL_HOLD) {
		if (!apply_hw || since_ms(rl.hold_s, rl.hold_ms) >= RL_DEACT_HOLD_S * 1000L)
			activate();
		return;
	}
	/* RL_WAIT_O5, looked at once a second: the state and the services. */
	if (s == rl.last_check_s)
		return;
	rl.last_check_s = s;
	st = onu_state_now();
	if (rl.deact_base >= 0) {
		long d = rereg_ploam_deacts();

		if (d >= rl.deact_base)
			rl.refusals = (unsigned)(d - rl.deact_base);
	}
	/* O7: the OLT disabled the serial number, which no wait undoes. A few
	 * deactivations before O5: the OLT turned the credential down more
	 * than an OLT that is still learning it does. */
	if (st == 7 || (st != 5 && rl.refusals >= REREG_REFUSALS)) {
		finish("refused", -1);
		return;
	}
	if (st == 5 && services_in_use() > 0) {
		finish(rl.sn_failed ? "sn_not_applied" : "ok",
		       since_ms(rl.act_s, rl.act_ms));
		return;
	}
	if (s >= rl.deadline_s) {
		finish(onu_state_now() == 5 ? "no_services" : "timeout", -1);
		return;
	}
	if (s % 5 == 0)
		status_write(0, "activated", -1);
}
