/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */

/* omcid's event lines: what the OLT did to this ONU's management channel, and
 * what omcid itself did, one line per event.
 *
 * The OMCI log (omcid.log) has every frame; that is too much to forward and
 * too little to read after an outage. These are the lines that answer "who
 * started it": a MIB reset and who sent it, a MIB upload, the first and last
 * config write of a provisioning burst and what it added up to, a reboot or a
 * software download the OLT asked for, and omcid starting -- resumed from its
 * snapshot or back to an empty MIB. docs/TOOLS.md, "Link and provisioning
 * events", has the table, next to the kernel's own event=onu_state lines.
 *
 * Every line is "event=<name>" and then key=value pairs, written twice: to
 * stdout, so omcid.log has it in order with the frames around it, and to
 * syslog as "omcid[pid]: event=...", facility daemon.
 *
 * syslog(3) is what this would call, and omcid has no libc. What syslog(3)
 * does is send one datagram to /dev/log, and that is all this does: the same
 * socket, the same "<PRI>tag[pid]: message" framing busybox syslogd parses,
 * which stamps the time and, with SYSLOG_SERVER, forwards it. Nothing of ours
 * sits between this and the collector. MSG_DONTWAIT, because a wedged syslogd
 * with a full socket queue must cost a dropped line, never a stalled OMCI
 * loop; a missing /dev/log (qemu, a shell with syslogd stopped) costs the
 * syslog copy and nothing else.
 *
 * The reload lines (below) are the exception: an operator's SIGHUP is what
 * makes one, apply.sh reads the outcome elsewhere, and a reload that
 * finishes must always be logged as finished.
 *
 * Rate limited, since a misbehaving OLT can repeat any of these: at most
 * EV_BURST lines per EV_WINDOW_S, and the first line of the next window says
 * how many were dropped (event=suppressed), as the kernel's ratelimit does.
 */
#include "omcid.h"

#define EV_LINE_MAX      240
#define EV_WINDOW_S      60
#define EV_BURST         20
/* A provisioning burst ends after this long with no Create, Set or Delete.
 * Gets and Tests do not count: ISP2's OLT interleaves many of both. ISP1
 * sends its 83 creates and 102 sets back to back, well inside this. */
#define EV_PROV_QUIET_S  10

#define EV_AF_UNIX       1
#define EV_SOCK_DGRAM    1       /* MIPS: DGRAM is 1 and STREAM 2, the
				  * other way round from x86 */
#define EV_MSG_DONTWAIT  0x40
#define EV_LOG_PATH      "/dev/log"
#define EV_FAC_DAEMON    (3 << 3)
#define EV_SEV_WARNING   4
#define EV_SEV_NOTICE    5
#define EV_SEV_INFO      6

struct evline {
	char b[EV_LINE_MAX];
	int n;
};

struct ev_sockaddr_un {
	uint16_t family;
	char path[108];
};

static long ev_fd = -2;          /* -2: not tried yet; -1: no socket */
static long ev_window_start = -1;
static int ev_in_window;
static unsigned long ev_dropped;

/* A monotonic time as seconds and milliseconds, kept apart: long is 32 bits
 * here, so seconds times 1000 would wrap after 24 days up, and a 64-bit
 * divide is a libgcc call this -nostdlib link does not have. */
struct ev_time {
	long s, ms;
};

static struct ev_time ev_now(void)
{
	struct ev_time t = { 0, 0 };
	long ns = 0;

	if (sys_clock_gettime(CLOCK_MONOTONIC, &t.s, &ns) == 0)
		t.ms = ns / 1000000;
	return t;
}

/* b - a in milliseconds, for the short spans these lines report; capped at
 * a day, far past any upload or provisioning burst. */
static long ev_ms_since(struct ev_time a, struct ev_time b)
{
	long ds = b.s - a.s;

	if (ds > 86400)
		return 86400L * 1000;
	return ds * 1000 + (b.ms - a.ms);
}

static void ev_put(struct evline *e, const char *s)
{
	while (*s && e->n < EV_LINE_MAX - 1)
		e->b[e->n++] = *s++;
}

static void ev_putu(struct evline *e, unsigned long v)
{
	char t[12];
	int i = 0;

	do {
		t[i++] = (char)('0' + v % 10);
		v /= 10;
	} while (v && i < (int)sizeof t);
	while (i && e->n < EV_LINE_MAX - 1)
		e->b[e->n++] = t[--i];
}

static void ev_begin(struct evline *e, const char *name)
{
	e->n = 0;
	ev_put(e, "event=");
	ev_put(e, name);
}

static void ev_str(struct evline *e, const char *key, const char *val)
{
	ev_put(e, " ");
	ev_put(e, key);
	ev_put(e, "=");
	ev_put(e, val);
}

static void ev_num(struct evline *e, const char *key, unsigned long v)
{
	ev_put(e, " ");
	ev_put(e, key);
	ev_put(e, "=");
	ev_putu(e, v);
}

/* Milliseconds as seconds with three decimals, the kernel lines' shape. */
static void ev_secs(struct evline *e, const char *key, long ms)
{
	static const char d[] = "0123456789";

	if (ms < 0)
		ms = 0;
	ev_num(e, key, (unsigned long)(ms / 1000));
	ev_put(e, ".");
	ms %= 1000;
	if (e->n < EV_LINE_MAX - 4) {
		e->b[e->n++] = d[ms / 100];
		e->b[e->n++] = d[ms / 10 % 10];
		e->b[e->n++] = d[ms % 10];
	}
}

static void ev_state(struct evline *e, uint32_t st)
{
	char s[3] = { 'O', (char)('0' + (st <= 9 ? st : 0)), 0 };

	ev_str(e, "onu_state", st >= 1 && st <= 7 ? s : "unknown");
}

static void ev_syslog(const struct evline *e, int sev)
{
	static struct ev_sockaddr_un to;
	struct evline f;
	struct iovec iov;
	struct msghdr msg;

	if (ev_fd == -2) {
		ev_fd = sys_socket(EV_AF_UNIX, EV_SOCK_DGRAM, 0);
		if (ev_fd < 0)
			ev_fd = -1;
		to.family = EV_AF_UNIX;
		str_copy(to.path, EV_LOG_PATH, (int)sizeof to.path);
	}
	if (ev_fd < 0)
		return;
	f.n = 0;
	ev_put(&f, "<");
	ev_putu(&f, (unsigned long)(EV_FAC_DAEMON | sev));
	ev_put(&f, ">omcid[");
	ev_putu(&f, (unsigned long)sys_getpid());
	ev_put(&f, "]: ");
	for (int i = 0; i < e->n && f.n < EV_LINE_MAX - 1; i++)
		f.b[f.n++] = e->b[i];
	iov.base = f.b;
	iov.len = (uint32_t)f.n;
	msg.name = &to;
	msg.namelen = (uint32_t)(2 + str_len(EV_LOG_PATH) + 1);
	msg.iov = &iov;
	msg.iovlen = 1;
	msg.control = 0;
	msg.controllen = 0;
	msg.flags = 0;
	/* Unconnected: every send names /dev/log, so a syslogd that restarted
	 * (apply.sh syslog) is found again on the next line without reopening
	 * anything. A failure is a lost copy of a line omcid.log still has. */
	(void)sys_sendmsg((int)ev_fd, &msg, EV_MSG_DONTWAIT);
}

static void ev_write(const struct evline *e, int sev)
{
	/* Anything half-built in the output buffer goes first, so the line
	 * lands between whole lines of omcid.log. Straight to fd 1 rather
	 * than through out(): an event can fire inside a CLI command (a MIB
	 * reset from omcicli), whose output belongs to the client. */
	out_flush();
	sys_write(1, e->b, (unsigned long)e->n);
	sys_write(1, "\n", 1);
	ev_syslog(e, sev);
}

static void ev_emit(struct evline *e, int sev)
{
	long now = ev_now().s;

	if (ev_window_start < 0 || now - ev_window_start >= EV_WINDOW_S) {
		ev_window_start = now;
		ev_in_window = 0;
		if (ev_dropped) {
			struct evline s;

			ev_begin(&s, "suppressed");
			ev_num(&s, "count", ev_dropped);
			ev_num(&s, "window_s", EV_WINDOW_S);
			ev_write(&s, EV_SEV_NOTICE);
			ev_in_window++;
			ev_dropped = 0;
		}
	}
	if (ev_in_window >= EV_BURST) {
		ev_dropped++;
		return;
	}
	ev_in_window++;
	ev_write(e, sev);
}

/* ------------------------------------------------------------ the events */

static unsigned ev_services(void)
{
	unsigned n = 0;

	for (int i = 0; i < SERV_MAX; i++)
		n += servtab[i].in_use ? 1u : 0u;
	return n;
}

void ev_start(int restart, int resumed, const char *why, uint32_t onu_state)
{
	struct evline e;

	ev_begin(&e, "start");
	ev_str(&e, "run", restart ? "restart" : "boot");
	ev_str(&e, "mode", resumed ? "resume" : "reregister");
	if (!resumed)
		ev_str(&e, "reason", why);
	ev_state(&e, onu_state);
	ev_num(&e, "rows", (unsigned long)mib_count());
	ev_emit(&e, restart ? EV_SEV_NOTICE : EV_SEV_INFO);
}

/* A config reload on SIGHUP (reload.c). Three lines, never rate limited
 * (a reload is an operator's act):
 *
 *   event=reload changed=<none|vlan|identity> action=<none|rebuild|reregister|
 *                held> keys=<store key names>
 *   event=reload_step verb=<gpondeact|gponsn|gponpw|gponact> rc=<errno>
 *   event=reload_done changed=.. action=.. result=<ok|failed|timeout|
 *                no_services|sn_not_applied|refused|held> duration_s=..
 *                [o5_s=..] services=.. [refusals=..]
 *
 * refusals: the Deactivate_ONU-IDs the OLT sent while a re-registration
 * waited for O5 (rereg.c).
 */
void ev_reload(const char *changed, const char *action, const char *keys)
{
	struct evline e;

	ev_begin(&e, "reload");
	ev_str(&e, "changed", changed);
	ev_str(&e, "action", action);
	if (keys && keys[0])
		ev_str(&e, "keys", keys);
	ev_write(&e, EV_SEV_NOTICE);
}

void ev_reload_step(const char *verb, long rc)
{
	struct evline e;

	ev_begin(&e, "reload_step");
	ev_str(&e, "verb", verb);
	ev_num(&e, "rc", (unsigned long)(rc < 0 ? -rc : rc));
	ev_write(&e, rc ? EV_SEV_NOTICE : EV_SEV_INFO);
}

void ev_reload_done(const char *changed, const char *action, const char *result,
		    long duration_ms, long o5_ms, unsigned services,
		    long refusals)
{
	struct evline e;

	ev_begin(&e, "reload_done");
	ev_str(&e, "changed", changed);
	ev_str(&e, "action", action);
	ev_str(&e, "result", result);
	ev_secs(&e, "duration_s", duration_ms);
	if (o5_ms >= 0)
		ev_secs(&e, "o5_s", o5_ms);
	ev_num(&e, "services", services);
	if (refusals >= 0)
		ev_num(&e, "refusals", (unsigned long)refusals);
	ev_write(&e, EV_SEV_NOTICE);
}

/* The provisioning burst: the first Create/Set/Delete opens it, EV_PROV_QUIET_S
 * without one closes it (ev_tick()). */
static struct {
	int open;
	int after_reset;         /* a MIB reset came first: a full reprovisioning */
	struct ev_time first, last;
	unsigned long creates, sets, deletes;
} prov;

static int reset_pending;        /* a MIB reset, and no burst since */

static void prov_close(void)
{
	struct evline e;

	if (!prov.open)
		return;
	ev_begin(&e, "provision_end");
	ev_num(&e, "creates", prov.creates);
	ev_num(&e, "sets", prov.sets);
	ev_num(&e, "deletes", prov.deletes);
	ev_secs(&e, "duration_s", ev_ms_since(prov.first, prov.last));
	ev_num(&e, "rows", (unsigned long)mib_count());
	ev_num(&e, "services", ev_services());
	ev_num(&e, "after_mib_reset", (unsigned long)prov.after_reset);
	ev_emit(&e, EV_SEV_INFO);
	prov.open = 0;
}

/* The last few requests before a MIB reset from the OLT, and whether the
 * resets have become a loop.
 *
 * An OLT that does not accept what it reads (the identity, the software
 * version, an answer it did not expect) typically resets the MIB, uploads it,
 * provisions, and resets again a minute or two later. Each reset is one
 * event=mib_reset line, so the loop shows only to someone who reads the log
 * and counts. Here it is counted: MIBLOOP_RESETS or more resets from the OLT
 * within MIBLOOP_WINDOW_S are a loop, logged once as event=mib_reset_loop
 * state=start, and once as state=end after a whole window without one. A
 * provisioning that settles never resets twice in a row, so three in ten
 * minutes is a loop by any reading, and short enough to show while someone
 * is still looking.
 *
 * The requests before each reset (before=op:class.inst, oldest first) are
 * what the OLT read or wrote last, which is where to look for what it checks.
 * The MIB upload Nexts and software download sections are not kept: they are
 * hundreds of the same, and would push out the requests that matter. A
 * retransmission answered from the cache is not kept either (omcimsg.c
 * records a request after that check).
 *
 * Three OLT resets in ten minutes also follow from a line that keeps
 * dropping out of O5 (each re-activation resets the MIB), so the
 * event=onu_state lines around them say which kind of loop it is. */
#define MIB_BEFORE_N      6
/* The OLT reset times kept: enough for recent= to count a fast loop. */
#define MIBLOOP_KEEP      16

static struct {
	uint8_t mt;
	uint16_t cls, inst;
} mib_before[MIB_BEFORE_N];
static int mib_before_n, mib_before_head;

static struct {
	unsigned long olt, local;     /* since omcid started */
	long at[MIBLOOP_KEEP];        /* the last OLT resets, seconds */
	int n;                        /* how many of at[] are set */
	int head;
	int loop;                     /* in a loop now */
	long loop_since;
	unsigned long loop_resets;    /* OLT resets since the loop started */
	unsigned long loops;          /* loops since omcid started */
} mres;

static const char *mib_before_op(uint8_t mt)
{
	switch (mt) {
	case OMCI_MT_CREATE:            return "create";
	case OMCI_MT_DELETE:            return "delete";
	case OMCI_MT_SET:               return "set";
	case OMCI_MT_GET:               return "get";
	case OMCI_MT_GET_NEXT:          return "get_next";
	case OMCI_MT_GET_CURRENT_DATA:  return "get_cur";
	case OMCI_MT_MIB_UPLOAD:        return "upload";
	case OMCI_MT_MIB_UPLOAD_NEXT:   return "upload_next";
	case OMCI_MT_GET_ALL_ALARMS:    return "alarms";
	case OMCI_MT_GET_ALL_ALARMS_NEXT: return "alarms_next";
	case OMCI_MT_TEST:              return "test";
	case OMCI_MT_REBOOT:            return "reboot";
	case OMCI_MT_SYNC_TIME:         return "sync_time";
	default:                        return 0;
	}
}

void ev_rx_msg(uint8_t mt, uint16_t cls, uint16_t inst)
{
	if (mt == OMCI_MT_MIB_RESET || mt == OMCI_MT_MIB_UPLOAD_NEXT)
		return;
	mib_before[mib_before_head].mt = mt;
	mib_before[mib_before_head].cls = cls;
	mib_before[mib_before_head].inst = inst;
	mib_before_head = (mib_before_head + 1) % MIB_BEFORE_N;
	if (mib_before_n < MIB_BEFORE_N)
		mib_before_n++;
}

/* before=get:256.0,set:171.257,... -- oldest first. The last key of the
 * line, so a line cut at EV_LINE_MAX loses its newest entries, never the
 * fields before it; six entries of the longest names with five-digit
 * classes and instances still fit. */
static void ev_before(struct evline *e)
{
	int first = (mib_before_head - mib_before_n + MIB_BEFORE_N) % MIB_BEFORE_N;

	if (!mib_before_n)
		return;
	ev_put(e, " before=");
	for (int i = 0; i < mib_before_n; i++) {
		int k = (first + i) % MIB_BEFORE_N;
		const char *op = mib_before_op(mib_before[k].mt);

		if (i)
			ev_put(e, ",");
		if (op) {
			ev_put(e, op);
		} else {
			ev_put(e, "mt");
			ev_putu(e, mib_before[k].mt);
		}
		ev_put(e, ":");
		ev_putu(e, mib_before[k].cls);
		ev_put(e, ".");
		ev_putu(e, mib_before[k].inst);
	}
}

/* The OLT resets within the last MIBLOOP_WINDOW_S. */
static int mres_recent(long now)
{
	int n = 0;

	for (int i = 0; i < mres.n; i++)
		if (now - mres.at[i] < MIBLOOP_WINDOW_S)
			n++;
	return n;
}

static long mres_last(void)
{
	return mres.n ? mres.at[(mres.head + MIBLOOP_KEEP - 1) % MIBLOOP_KEEP] : -1;
}

static void mres_loop_line(const char *state, long now)
{
	struct evline e;

	ev_begin(&e, "mib_reset_loop");
	ev_str(&e, "state", state);
	ev_num(&e, "resets", mres.loop_resets);
	ev_num(&e, "window_s", MIBLOOP_WINDOW_S);
	if (state[0] == 'e')
		ev_num(&e, "duration_s", (unsigned long)(mres_last() - mres.loop_since));
	ev_num(&e, "services", ev_services());
	/* Past the rate limit, as the reload lines are: each is logged once
	 * and never again, and a reset storm is exactly when the shared burst
	 * runs out. At most one start and one end per MIBLOOP_WINDOW_S. */
	ev_write(&e, state[0] == 's' ? EV_SEV_WARNING : EV_SEV_NOTICE);
}

static void mres_olt(void)
{
	long now = ev_now().s;

	mres.olt++;
	mres.at[mres.head] = now;
	mres.head = (mres.head + 1) % MIBLOOP_KEEP;
	if (mres.n < MIBLOOP_KEEP)
		mres.n++;
	if (mres.loop) {
		mres.loop_resets++;
		return;
	}
	if (mres_recent(now) >= MIBLOOP_RESETS) {
		/* From the first reset of the window: that is when it began. */
		mres.loop_since = now;
		for (int i = 0; i < mres.n; i++)
			if (now - mres.at[i] < MIBLOOP_WINDOW_S &&
			    mres.at[i] < mres.loop_since)
				mres.loop_since = mres.at[i];
		mres.loop = 1;
		mres.loops++;
		mres.loop_resets = (unsigned long)mres_recent(now);
		mres_loop_line("start", now);
	}
}

/* For `omcli state` (show.c). */
void ev_mib_reset_counts(unsigned long *olt, unsigned long *local,
			 unsigned *recent, int *loop, unsigned long *loops)
{
	*olt = mres.olt;
	*local = mres.local;
	*recent = (unsigned)mres_recent(ev_now().s);
	*loop = mres.loop;
	*loops = mres.loops;
}

void ev_mib_reset(const char *side)
{
	struct evline e;
	int olt = side[0] == 'o';

	/* A reset in the middle of a burst ends it: what came before is
	 * about to be thrown away, and the next write starts a new one. */
	prov_close();
	ev_begin(&e, "mib_reset");
	ev_str(&e, "side", side);
	ev_num(&e, "rows", (unsigned long)mib_count());
	ev_num(&e, "mib_data_sync", mib_data_sync);
	ev_num(&e, "services", ev_services());
	if (olt) {
		int recent = mres_recent(ev_now().s);

		/* This one included, and counted as omcli state counts: at most
		 * the MIBLOOP_KEEP times kept. */
		ev_num(&e, "recent", (unsigned long)(recent < (int)MIBLOOP_KEEP ? recent + 1 : recent));
		ev_before(&e);
	}
	ev_emit(&e, EV_SEV_NOTICE);
	if (olt)
		mres_olt();
	else
		mres.local++;
	mib_before_n = 0;
	reset_pending = 1;
}

void ev_config_write(uint8_t mt, uint16_t cls, uint16_t inst)
{
	struct ev_time now = ev_now();

	if (!prov.open) {
		struct evline e;

		prov.open = 1;
		prov.after_reset = reset_pending;
		reset_pending = 0;
		prov.first = now;
		prov.creates = prov.sets = prov.deletes = 0;
		ev_begin(&e, "provision_begin");
		ev_str(&e, "op", mt == OMCI_MT_CREATE ? "create" :
				 mt == OMCI_MT_SET ? "set" : "delete");
		ev_num(&e, "class", cls);
		ev_num(&e, "inst", inst);
		ev_num(&e, "after_mib_reset", (unsigned long)prov.after_reset);
		ev_emit(&e, EV_SEV_INFO);
	}
	prov.last = now;
	if (mt == OMCI_MT_CREATE)
		prov.creates++;
	else if (mt == OMCI_MT_SET)
		prov.sets++;
	else
		prov.deletes++;
}

/* The MIB upload: its count, and the Next that answers its last entity. An
 * upload the OLT abandons half way has a begin and no end. */
static struct {
	int open;
	uint16_t total;
	struct ev_time first;
} upl;

void ev_mib_upload(uint16_t total)
{
	struct evline e;

	upl.open = 1;
	upl.total = total;
	upl.first = ev_now();
	ev_begin(&e, "mib_upload_begin");
	ev_num(&e, "entities", total);
	ev_emit(&e, EV_SEV_INFO);
}

void ev_mib_upload_next(uint16_t seq)
{
	struct evline e;

	if (!upl.open || (unsigned)seq + 1 != upl.total)
		return;
	upl.open = 0;
	ev_begin(&e, "mib_upload_end");
	ev_num(&e, "entities", upl.total);
	ev_secs(&e, "duration_s", ev_ms_since(upl.first, ev_now()));
	ev_emit(&e, EV_SEV_INFO);
}

/* A reboot the OLT asked for, and what omcid did with it: "accepted" (the
 * stick reboots), "acknowledged" (answered success, no reboot), "refused"
 * (OMCI_ACCEPT_REBOOT is off) or "not_supported"
 * (any class but ONU-G). Each request is worth a line: an OLT that follows a
 * refused reboot with a deactivation is an outage the ISP started. */
void ev_olt_command(uint8_t mt, uint16_t cls, uint16_t inst, const char *result)
{
	struct evline e;

	if (mt != OMCI_MT_REBOOT)
		return;
	ev_begin(&e, "olt_reboot");
	ev_num(&e, "class", cls);
	ev_num(&e, "inst", inst);
	ev_str(&e, "result", result);
	ev_emit(&e, EV_SEV_NOTICE);
}

/* The snapshot could not be written (full tmpfs, a failed write or rename).
 * The previous snapshot is still on disk, so a respawn resumes from an older
 * MIB than the one the OLT was told about. */
void ev_snapshot_fail(unsigned rows)
{
	struct evline e;

	ev_begin(&e, "snapshot_fail");
	ev_num(&e, "rows", rows);
	ev_emit(&e, EV_SEV_NOTICE);
}

/* One step of a software download, activate or commit (swimage.c). Download
 * sections are counted there, not logged: an image is thousands of them. */
void ev_sw_image(const char *op, uint16_t inst, long size, unsigned window,
		 long sections, const char *crc, const char *result)
{
	struct evline e;

	ev_begin(&e, "sw_image");
	ev_str(&e, "op", op);
	ev_num(&e, "inst", inst);
	if (size >= 0) {
		ev_num(&e, "size", (unsigned long)size);
		ev_num(&e, "window", window);
	}
	if (sections >= 0)
		ev_num(&e, "sections", (unsigned long)sections);
	if (crc)
		ev_str(&e, "crc", crc);
	ev_str(&e, "result", result);
	ev_emit(&e, EV_SEV_NOTICE);
}

/* The Alloc-IDs the OLT assigned by PLOAM, whenever the kernel list
 * changes (apply_qos.c binds the T-CONTs the OLT does not set to them):
 * during ranging, on a deallocation, and when a re-ranging clears them. */
void ev_alloc_ids(const uint16_t *ids, unsigned n)
{
	struct evline e;

	ev_begin(&e, "alloc_ids");
	ev_num(&e, "count", n);
	ev_put(&e, " ids=");
	for (unsigned i = 0; i < n; i++) {
		if (i)
			ev_put(&e, ",");
		ev_putu(&e, ids[i]);
	}
	if (!n)
		ev_put(&e, "none");
	ev_emit(&e, EV_SEV_INFO);
}

/* ------------------------------------------- what omcid does not model
 *
 * An OLT of another vendor can send a managed entity class omcid has no
 * model for, or a message type it does not handle. Each is worth one line,
 * not one per frame: an OLT that stalls on an answer retries the same
 * request for as long as it waits. So each unknown (class, operation) pair
 * and each unknown message type is logged once per boot, as
 * event=unknown_me or event=unknown_msg, and kept with a count in
 * UNKNOWN_PATH, which diag-bundle.sh collects with the rest of /var/log.
 *
 * "Per boot", not per process: /var/log is tmpfs, so the file itself is the
 * record of this boot, and a respawned omcid reads it back before it logs
 * anything, rather than logging every unknown again. Counts are rewritten
 * at most once a second (ev_tick()); a new entry is written at once. */
#define UNK_MAX      64
#define UNK_BUF      8192

/* What an entry records: a class omcid has no model for, a message type it
 * does not handle, or an instance of a modelled class that it holds no row
 * for (see ev_no_instance()). */
#define UNK_ME       0
#define UNK_MSG      1
#define UNK_NO_INST  2

struct unk_entry {
	uint16_t cls;            /* unknown_me: the class; unknown_msg: the
				  * class of the first frame of that type */
	uint16_t inst;           /* no_instance: the instance */
	uint8_t mt;              /* no_instance: of the first request */
	uint8_t msg;             /* UNK_ME, UNK_MSG or UNK_NO_INST */
	unsigned long count;
	unsigned long first_s;   /* monotonic seconds, i.e. uptime */
};

static struct unk_entry unk[UNK_MAX];
static int unk_n, unk_loaded, unk_dirty;
static unsigned long unk_overflow;
static char unkbuf[UNK_BUF];

int ev_msg_known(uint8_t mt)
{
	return mt == OMCI_MT_CREATE || mt == OMCI_MT_DELETE ||
	       mt == OMCI_MT_SET || mt == OMCI_MT_GET ||
	       mt == OMCI_MT_GET_ALL_ALARMS || mt == OMCI_MT_GET_ALL_ALARMS_NEXT ||
	       mt == OMCI_MT_MIB_UPLOAD || mt == OMCI_MT_MIB_UPLOAD_NEXT ||
	       mt == OMCI_MT_MIB_RESET || mt == OMCI_MT_TEST ||
	       (mt >= OMCI_MT_START_SW_DOWNLOAD && mt <= OMCI_MT_REBOOT) ||
	       mt == OMCI_MT_GET_NEXT || mt == OMCI_MT_GET_CURRENT_DATA;
}

static const char *unk_op(uint8_t mt)
{
	switch (mt) {
	case OMCI_MT_CREATE:   return "create";
	case OMCI_MT_DELETE:   return "delete";
	case OMCI_MT_SET:      return "set";
	case OMCI_MT_GET:      return "get";
	case OMCI_MT_GET_NEXT: return "get_next";
	case OMCI_MT_GET_CURRENT_DATA: return "get_current_data";
	case OMCI_MT_TEST:     return "test";
	default:               return 0;
	}
}

/* The number after `key` on one line, or -1. */
static long unk_field(const char *l, int n, const char *key)
{
	int k = str_len(key);

	for (int i = 0; i + k <= n; i++) {
		int j = 0;
		long v = 0;

		if (i && l[i - 1] != ' ')
			continue;
		while (j < k && l[i + j] == key[j])
			j++;
		if (j < k)
			continue;
		if (i + k >= n || l[i + k] < '0' || l[i + k] > '9')
			return -1;
		for (j = i + k; j < n && l[j] >= '0' && l[j] <= '9'; j++)
			v = v * 10 + (l[j] - '0');
		return v;
	}
	return -1;
}

static int unk_prefix(const char *l, int n, const char *p)
{
	int k = str_len(p);

	if (n < k)
		return 0;
	for (int i = 0; i < k; i++)
		if (l[i] != p[i])
			return 0;
	return 1;
}

/* The entries an earlier omcid of this boot wrote. A line this does not
 * recognise is dropped at the next rewrite. */
static void unk_load(void)
{
	long fd, r;
	int n = 0;

	unk_loaded = 1;
	fd = sys_open(UNKNOWN_PATH, O_RDONLY);
	if (fd < 0)
		return;
	while (n < UNK_BUF - 1 && (r = sys_read((int)fd, unkbuf + n,
						 UNK_BUF - 1 - n)) > 0)
		n += (int)r;
	sys_close((int)fd);
	for (int i = 0; i < n && unk_n < UNK_MAX; ) {
		int e = i;
		const char *l = unkbuf + i;
		long cls, mt, cnt, first, inst = 0;
		int msg = -1;

		while (e < n && unkbuf[e] != '\n')
			e++;
		if (unk_prefix(l, e - i, "unknown_me "))
			msg = 0;
		else if (unk_prefix(l, e - i, "unknown_msg "))
			msg = 1;
		else if (unk_prefix(l, e - i, "no_instance "))
			msg = 2;
		cls = unk_field(l, e - i, "class=");
		mt = unk_field(l, e - i, msg == UNK_MSG ? "type=" : "mt=");
		if (msg == UNK_NO_INST)
			inst = unk_field(l, e - i, "inst=");
		cnt = unk_field(l, e - i, "count=");
		first = unk_field(l, e - i, "first_uptime_s=");
		if (msg >= 0 && cls >= 0 && cls <= 0xffff && mt >= 0 && mt <= 0xff &&
		    cnt >= 0 && first >= 0 && inst >= 0 && inst <= 0xffff) {
			struct unk_entry *u = &unk[unk_n++];

			u->cls = (uint16_t)cls;
			u->inst = (uint16_t)inst;
			u->mt = (uint8_t)mt;
			u->msg = (uint8_t)msg;
			u->count = (unsigned long)cnt;
			u->first_s = (unsigned long)first;
		} else if (unk_prefix(l, e - i, "overflow ") &&
			   (cnt = unk_field(l, e - i, "count=")) >= 0) {
			unk_overflow = (unsigned long)cnt;
		}
		i = e + 1;
	}
}

static void unk_line(struct evline *e, const struct unk_entry *u)
{
	const char *op = unk_op(u->mt);

	e->n = 0;
	if (u->msg == UNK_MSG) {
		ev_put(e, "unknown_msg");
		ev_num(e, "type", u->mt);
		ev_num(e, "class", u->cls);
	} else if (u->msg == UNK_NO_INST) {
		ev_put(e, "no_instance");
		ev_num(e, "class", u->cls);
		ev_num(e, "inst", u->inst);
		if (op)
			ev_str(e, "op", op);
		else
			ev_num(e, "op", u->mt);
		ev_num(e, "mt", u->mt);
	} else {
		ev_put(e, "unknown_me");
		ev_num(e, "class", u->cls);
		if (op)
			ev_str(e, "op", op);
		else
			ev_num(e, "op", u->mt);
		ev_num(e, "mt", u->mt);
	}
}

static void unk_save(void)
{
	static const char head[] =
		"# omcid: what the OLT sent that omcid does not model, this boot.\n"
		"# One line per class and operation (unknown_me), message type\n"
		"# (unknown_msg) or instance omcid holds no row for (no_instance);\n"
		"# docs/TOOLS.md, \"Link and provisioning events\".\n";
	int n = 0;

	for (int i = 0; head[i] && n < UNK_BUF - 1; i++)
		unkbuf[n++] = head[i];
	for (int i = 0; i < unk_n; i++) {
		struct evline e;

		unk_line(&e, &unk[i]);
		ev_num(&e, "count", unk[i].count);
		ev_num(&e, "first_uptime_s", unk[i].first_s);
		for (int k = 0; k < e.n && n < UNK_BUF - 1; k++)
			unkbuf[n++] = e.b[k];
		if (n < UNK_BUF - 1)
			unkbuf[n++] = '\n';
	}
	if (unk_overflow) {
		struct evline e;

		e.n = 0;
		ev_put(&e, "overflow");
		ev_num(&e, "count", unk_overflow);
		ev_put(&e, " (more distinct unknowns than the table holds)\n");
		for (int k = 0; k < e.n && n < UNK_BUF - 1; k++)
			unkbuf[n++] = e.b[k];
	}
	/* A failed write (a full /var) costs this copy; the next change
	 * tries again. */
	(void)atomic_write(UNKNOWN_PATH, UNKNOWN_TMP_PATH,
			   (const uint8_t *)unkbuf, (uint32_t)n);
	unk_dirty = 0;
}

static void unk_note(uint8_t msg, uint16_t cls, uint16_t inst, uint8_t mt)
{
	struct evline e;
	struct unk_entry *u;
	const char *name = msg == UNK_MSG ? "unknown_msg" :
			   msg == UNK_NO_INST ? "no_instance" : "unknown_me";

	if (!unk_loaded)
		unk_load();
	for (int i = 0; i < unk_n; i++) {
		u = &unk[i];
		/* no_instance is one entry per (class, instance), whatever
		 * the operation: the question it answers is whether the OLT
		 * addresses that instance at all. */
		if (u->msg == msg &&
		    (msg == UNK_NO_INST ? u->cls == cls && u->inst == inst
					: u->mt == mt && (msg == UNK_MSG || u->cls == cls))) {
			u->count++;
			unk_dirty = 1;
			return;
		}
	}
	if (unk_n >= UNK_MAX) {
		unk_overflow++;
		unk_dirty = 1;
		return;
	}
	u = &unk[unk_n++];
	u->msg = msg;
	u->cls = cls;
	u->inst = inst;
	u->mt = mt;
	u->count = 1;
	u->first_s = (unsigned long)ev_now().s;
	unk_save();
	/* The summary line is the event line minus the counters; the rate
	 * limit may drop the event, never the file entry. */
	unk_line(&e, u);
	{
		struct evline ev;

		ev_begin(&ev, name);
		for (int k = str_len(name); k < e.n && ev.n < EV_LINE_MAX - 1; k++)
			ev.b[ev.n++] = e.b[k];
		ev_emit(&ev, EV_SEV_NOTICE);
	}
}

void ev_unknown_me(uint16_t cls, uint8_t mt)
{
	unk_note(UNK_ME, cls, 0, mt);
}

void ev_unknown_msg(uint8_t mt, uint16_t cls)
{
	unk_note(UNK_MSG, cls, 0, mt);
}

void ev_no_instance(uint16_t cls, uint16_t inst, uint8_t mt)
{
	unk_note(UNK_NO_INST, cls, inst, mt);
}

/* A VLAN filter forward operation (class 84 FwdOp) omcid cannot build and
 * replaces (apply_bridge.c, bp_rules()). Once per code for the life of the
 * process: the rebuild runs after every provisioning burst and would say it
 * again each time. The code is two hex digits, as G.988 prints it. */
void ev_vlan_fwdop(uint16_t inst, uint8_t code, uint8_t used)
{
	static uint8_t seen[256 / 8];
	static const char hex[] = "0123456789abcdef";
	char c[5] = { '0', 'x', hex[code >> 4], hex[code & 15], 0 };
	char u[5] = { '0', 'x', hex[used >> 4], hex[used & 15], 0 };
	struct evline e;

	if (seen[code >> 3] & (1u << (code & 7)))
		return;
	seen[code >> 3] |= (uint8_t)(1u << (code & 7));
	ev_begin(&e, "vlan_fwdop");
	ev_str(&e, "code", c);
	ev_num(&e, "inst", inst);
	ev_str(&e, "result", "unsupported");
	ev_str(&e, "built_as", u);
	ev_emit(&e, EV_SEV_NOTICE);
}

/* A class 84 port built with the owner override of its FwdOp in place of
 * the code the OLT wrote (RTK_DEVINFO_SPECVER, cfgstore.c). Once per port
 * and pair of codes, for the same reason as above. */
void ev_vlan_fwdop_override(uint16_t inst, uint8_t code, uint8_t used)
{
	static struct { uint16_t inst; uint8_t code, used; } seen[FWDOP_OVR_MAX];
	static int nseen;
	static const char hex[] = "0123456789abcdef";
	char c[5] = { '0', 'x', hex[code >> 4], hex[code & 15], 0 };
	char u[5] = { '0', 'x', hex[used >> 4], hex[used & 15], 0 };
	struct evline e;
	int k;

	for (k = 0; k < nseen && seen[k].inst != inst; k++)
		;
	if (k < nseen && seen[k].code == code && seen[k].used == used)
		return;
	if (k == nseen && nseen < FWDOP_OVR_MAX)
		nseen++;
	if (k < nseen) {
		seen[k].inst = inst;
		seen[k].code = code;
		seen[k].used = used;
	}
	ev_begin(&e, "vlan_fwdop");
	ev_str(&e, "code", c);
	ev_num(&e, "inst", inst);
	ev_str(&e, "result", "override");
	ev_str(&e, "built_as", u);
	ev_emit(&e, EV_SEV_NOTICE);
}

/* The OLT locked or unlocked an Ethernet UNI (apply_uni.c). action is
 * applied (OMCI_UNI_LOCK=1: the host link goes down now, and with it the
 * web UI and ssh, so the line is a warning and is sent before the port is
 * touched), ignored (the default: answered, the port stays up), unlocked
 * (the OLT's unlock of a port it had locked) or cleared (a MIB reset or an
 * omcid start unlocked a port a lock had taken down; inst is 0). One line
 * per change of state of the port, so an OLT that repeats the same Set says
 * it once. */
void ev_uni_lock(uint16_t inst, int port, const char *action)
{
	/* 0 never locked, 1 unlocked, 2 lock ignored, 3 lock applied */
	static uint8_t st[32];
	uint8_t to = action[0] == 'a' ? 3 : action[0] == 'i' ? 2 : 1;
	struct evline e;

	if (port < 0 || port >= 32)
		return;
	if (action[0] == 'c') {
		to = st[port];
		st[port] = 0;
		if (to != 3)
			return;
	} else {
		if (st[port] == to || (to == 1 && st[port] <= 1))
			return;
		st[port] = to;
	}
	ev_begin(&e, "uni_lock");
	ev_num(&e, "inst", inst);
	ev_num(&e, "port", (unsigned long)port);
	ev_str(&e, "action", action);
	if (action[0] == 'a')
		ev_str(&e, "effect", "host_link_down_until_unlock");
	ev_emit(&e, action[0] == 'a' ? EV_SEV_WARNING : EV_SEV_NOTICE);
}

/* Once a second, from the main loop. */
void ev_tick(void)
{
	if (prov.open && ev_ms_since(prov.last, ev_now()) >= EV_PROV_QUIET_S * 1000L)
		prov_close();
	if (unk_dirty)
		unk_save();
	if (mres.loop) {
		long now = ev_now().s;

		if (now - mres_last() >= MIBLOOP_WINDOW_S) {
			mres.loop = 0;
			mres_loop_line("end", now);
		}
	}
}

/* A frame rx_accept() flagged: dropped, or for reason trailer answered anyway
 * (action=accepted). An OLT speaking the extended format retries
 * every request, and that must not spend the shared burst of ev_emit() on one
 * fault, so each reason has a window of its own: the first frame is logged at
 * once, the next line comes EV_WINDOW_S later and says how many frames were
 * dropped since (count) and in all (total, which `omcli state` shows too).
 * devid and len are those of the frame that triggered the line. */
void ev_rx_reject(int reason, uint8_t devid, uint32_t len)
{
	static const char *const name[RX_REJ_N] = {
		"length", "devid", "trailer", "crc"
	};
	static long win_start[RX_REJ_N];
	static unsigned long last_total[RX_REJ_N];
	static int seen[RX_REJ_N];
	long now = ev_now().s;
	struct evline e;

	if (reason < 0 || reason >= RX_REJ_N)
		return;
	if (seen[reason] && now - win_start[reason] < EV_WINDOW_S)
		return;
	seen[reason] = 1;
	win_start[reason] = now;
	ev_begin(&e, "rx_reject");
	ev_str(&e, "reason", name[reason]);
	ev_num(&e, "count", omci_rx_reject[reason] - last_total[reason]);
	ev_num(&e, "total", omci_rx_reject[reason]);
	ev_str(&e, "action", reason == RX_REJ_TRAILER ? "accepted" : "dropped");
	if (reason == RX_REJ_DEVID)
		ev_num(&e, "devid", devid);
	ev_num(&e, "len", len);
	last_total[reason] = omci_rx_reject[reason];
	ev_emit(&e, EV_SEV_NOTICE);
}
