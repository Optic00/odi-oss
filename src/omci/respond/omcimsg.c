/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */

/* OMCI frames: the OLT's questions and our answers.
 *
 * Split out of main.c; see omcid.h.
 */
#include "omcid.h"

uint8_t rxbuf[2048];

static uint8_t txbuf[2048];

uint8_t frame[OMCI_FRAME_LEN];

/* The netlink socket this process answers on, and the transaction id it
 * registered with. Set once, before the main loop.
 *
 * volatile because the signal handler and the main loop both touch them. Not
 * sig_atomic_t: <signal.h> is not one of the freestanding headers, so the type
 * is not available here. Both are word-sized, which on MIPS is what the
 * guarantee amounts to anyway.
 *
 * They are file-scope rather than main's locals because a frame can arrive by
 * two routes -- off the line, and injected on the message queue -- and the
 * queue path has no fd of its own to answer through. */
volatile int nl_fd = -1;

volatile uint32_t nl_tid;

/* A retransmission cache for the messages that change the MIB.
 *
 * The OLT repeats a request, with the same transaction id, when it has not
 * seen the answer in time. Run again, a Set would count a second MIB data
 * sync and a Create would answer "instance exists" to the very request that
 * made the instance, so a repeat of a request already answered is answered
 * with the same frame and does nothing else. The last few answers are kept,
 * each for RETX_AGE_S seconds, which is longer than an OLT keeps retrying;
 * a MIB reset empties it, as the OLT restarts its transaction ids there.
 * Only an identical frame counts as a repeat: the same id carrying other
 * contents is a new request. */
#define RETX_SLOTS 4
#define RETX_AGE_S 10

static struct {
	uint8_t used;
	long at;                       /* CLOCK_MONOTONIC seconds when answered */
	uint8_t req[40];               /* the request, header and contents */
	uint8_t rsp[OMCI_FRAME_LEN];
} retx[RETX_SLOTS];

static unsigned retx_next;

static long now_s(void)
{
	long s = 0, ns = 0;

	if (sys_clock_gettime(CLOCK_MONOTONIC, &s, &ns) != 0)
		return 0;
	return s;
}

static int retx_kind(uint8_t mt)
{
	return mt == OMCI_MT_CREATE || mt == OMCI_MT_SET || mt == OMCI_MT_DELETE;
}

void retx_clear(void)
{
	for (unsigned i = 0; i < RETX_SLOTS; i++)
		retx[i].used = 0;
}

static void emit_frame(int fd, uint32_t tid)
{
	uint16_t i;

	if (fd < 0) {
		/* No line side -- an injected frame on a bench, or qemu. The
		 * answer is the thing being tested, so print it rather than
		 * writing it to a socket that does not exist. */
		out("-> ");
		for (i = 0; i < OMCI_FRAME_LEN; i++)
			out_hex(frame[i], 2);
		out_char('\n');
		return;
	}
	nl_send_frame(fd, tid, txbuf, frame, OMCI_FRAME_LEN);
}

/* Answer a repeated request with the stored answer. Returns 1 when it did. */
static int retx_replay(int fd, uint32_t tid, const uint8_t *f)
{
	long t = now_s();

	if (!retx_kind(OMCI_MT(f[2])))
		return 0;
	for (unsigned i = 0; i < RETX_SLOTS; i++) {
		unsigned j;

		if (!retx[i].used || t - retx[i].at > RETX_AGE_S)
			continue;
		for (j = 0; j < sizeof retx[i].req; j++)
			if (retx[i].req[j] != f[j])
				break;
		if (j < sizeof retx[i].req)
			continue;
		for (j = 0; j < OMCI_FRAME_LEN; j++)
			frame[j] = retx[i].rsp[j];
		emit_frame(fd, tid);
		return 1;
	}
	return 0;
}

void send_resp(int fd, uint32_t tid, const uint8_t *req,
	       const uint8_t *contents, uint16_t clen)
{
	uint16_t i;

	for (i = 0; i < OMCI_FRAME_LEN; i++)
		frame[i] = 0;
	frame[0] = req[0];                       /* echo the transaction id */
	frame[1] = req[1];
	frame[2] = (uint8_t)(OMCI_MT(req[2]) | OMCI_AK);
	frame[3] = OMCI_DEVICE_ID;
	frame[4] = req[4];                       /* echo class and instance */
	frame[5] = req[5];
	frame[6] = req[6];
	frame[7] = req[7];
	for (i = 0; i < clen && 8 + i < 40; i++)
		frame[8 + i] = contents[i];
	nl_put32(frame + 40, OMCI_TRAILER);
	nl_put32(frame + 44, omci_crc(frame, 44));
	if (retx_kind(OMCI_MT(req[2]))) {
		/* Kept before sending, so the stored answer is the one the
		 * OLT was given. */
		unsigned s = retx_next++ % RETX_SLOTS;

		retx[s].used = 1;
		retx[s].at = now_s();
		for (i = 0; i < sizeof retx[s].req; i++)
			retx[s].req[i] = req[i];
		for (i = 0; i < OMCI_FRAME_LEN; i++)
			retx[s].rsp[i] = frame[i];
	}
	emit_frame(fd, tid);
}


/* 10*log10(x) in units of 0.002 dB, integer only (this program has no libm):
 * log2 from the top bit plus a 33-point table for the mantissa, then the
 * 3.0103 dB per octave. Good to about 0.05 dB, far below what an OLT shows. */
int32_t db_x500_of(uint32_t x)
{
	static const uint16_t frac[33] = {   /* 500*10*log10(1 + k/32) */
		0, 67, 133, 197, 260, 320, 380, 438, 494, 549, 603, 656, 707,
		758, 807, 855, 902, 949, 994, 1038, 1082, 1124, 1166, 1207,
		1248, 1287, 1326, 1365, 1402, 1439, 1476, 1512, 1547 };
	int e = 0;
	uint32_t m;

	if (x == 0)
		return -30000;                   /* -60 dB: nothing measurable */
	while (x >> (e + 1))
		e++;
	m = e >= 5 ? (x >> (e - 5)) & 31 : (x << (5 - e)) & 31;
	return e * 1505 + frac[m];               /* 3.0103 dB * 500 per octave */
}

/* The SFF-8472 word the driver holds for a DDM type (omcidrv command 10,
 * {type, data[32]}, big-endian word at data[0]). Returns -1 when it cannot
 * be read. */
static int ddm_read(unsigned type, uint32_t *word)
{
	uint8_t buf[36];

	for (unsigned i = 0; i < sizeof buf; i++)
		buf[i] = 0;
	buf[3] = (uint8_t)type;
	if (omci_getTransceiverStatus(buf) != 0)
		return -1;
	*word = ((uint32_t)buf[4] << 8) | buf[5];
	return 0;
}

/* The same word, 0 when it cannot be read (the Test result reports 0). */
static uint32_t ddm_word(unsigned type)
{
	uint32_t w;

	return ddm_read(type, &w) == 0 ? w : 0;
}

/* ANI-G attributes 10 (optical signal level) and 14 (transmit optical
 * level): the DDM power word of `type` (5 Tx, 6 Rx) as dBm in 0.002 dB
 * steps, a signed 16-bit value with a 0 dBm reference. The word is in 0.1 uW
 * units, so dBm is 10 log10(word) minus 40 dB. No light reads as -65.5 dBm
 * (the lowest value the field holds). Returns -1 when the driver cannot be
 * read; the caller then answers 0. */
int ddm_dbm_x500(unsigned type, int16_t *out)
{
	uint32_t w;
	int32_t v;

	if (ddm_read(type, &w) != 0)
		return -1;
	v = db_x500_of(w) - 20000;
	*out = (int16_t)(v < -32768 ? -32768 : v);
	return 0;
}

/* Test (18) on ANI-G: the OLT asks for the optical measurements and shows
 * them as the ONU's line values. The vendor answers, and this OLT stopped its
 * provisioning at the unanswered Test. Reply
 * with a Test response, then the autonomous Test Result (27, same TCI) in
 * the ANI-G layout of G.988 I.1: type/value pairs, 1 power feed voltage in
 * 20 mV, 3 received optical power and 5 mean launch power in 0.002 dB(uW),
 * 9 laser bias in 2 uA, 12 temperature in 1/256 C. */
static void handle_test(int fd, uint32_t tid, const uint8_t *f, uint16_t cls,
			const struct omci_class *c)
{
	uint8_t body[16];
	uint32_t volt = ddm_word(3), rx = ddm_word(6), tx = ddm_word(5);
	uint32_t bias = ddm_word(4), temp = ddm_word(2);
	int32_t rxdb, txdb;
	uint16_t i;

	/* The ANI-G is the one class omcid measures anything for. The model
	 * lists Test for the IP host too (a ping), but nothing here sends one,
	 * so acknowledging it would leave the OLT waiting for a result that
	 * never comes: result 2, and no Test Result, as for a class with no
	 * Test at all. A class the model does not hold keeps the old answer
	 * (see OMCI_UNKNOWN_ME_OK). */
	if (c && !(cls == OMCI_ME_ANI_G && (c->actions & (1u << OMCI_MT_TEST)))) {
		body[0] = OMCI_ERR_NOT_SUPPORTED;
		send_resp(fd, tid, f, body, 1);
		out("-> not supported (no test for this class)\n");
		return;
	}
	body[0] = OMCI_OK;
	send_resp(fd, tid, f, body, 1);
	if (cls != OMCI_ME_ANI_G) {
		out("-> test accepted (no result for this class)\n");
		return;
	}
	/* SFF-8472 power words are 0.1 uW; dB(uW) needs uW, so 10 log10 of
	 * the word minus 10 dB (5000 in these units). */
	rxdb = db_x500_of(rx) - 5000;
	txdb = db_x500_of(tx) - 5000;
	for (i = 0; i < OMCI_FRAME_LEN; i++)
		frame[i] = 0;
	frame[0] = f[0];
	frame[1] = f[1];
	frame[2] = OMCI_MT_TEST_RESULT;          /* autonomous: no AR, no AK */
	frame[3] = OMCI_DEVICE_ID;
	frame[4] = f[4]; frame[5] = f[5]; frame[6] = f[6]; frame[7] = f[7];
	frame[8] = 1;  frame[9]  = (uint8_t)((volt / 200) >> 8); frame[10] = (uint8_t)(volt / 200);
	frame[11] = 3; frame[12] = (uint8_t)((uint16_t)rxdb >> 8); frame[13] = (uint8_t)rxdb;
	frame[14] = 5; frame[15] = (uint8_t)((uint16_t)txdb >> 8); frame[16] = (uint8_t)txdb;
	frame[17] = 9; frame[18] = (uint8_t)(bias >> 8); frame[19] = (uint8_t)bias;
	frame[20] = 12; frame[21] = (uint8_t)(temp >> 8); frame[22] = (uint8_t)temp;
	nl_put32(frame + 40, OMCI_TRAILER);
	nl_put32(frame + 44, omci_crc(frame, 44));
	if (fd >= 0)
		nl_send_frame(fd, tid, txbuf, frame, OMCI_FRAME_LEN);
	out_fmt("-> test result: vcc %d mV, rx %d tx %d (0.002 dBuW), bias %d uA, temp %d/256 C\n",
		(long)(volt / 10), (long)rxdb, (long)txdb, (long)(bias * 2), (long)temp);
}

static void handle_upload_next(int fd, uint32_t tid, const uint8_t *f)
{
	uint8_t body[32];
	uint16_t seq = nl_get16(f + 8), len = 0;

	/* A Next with no upload before it reads the MIB as it is now. */
	if (!upl_total)
		mib_upload_build();
	if (mib_upload_record(seq, body, &len) != 0) {
		body[0] = OMCI_ERR_BAD_PARAM;
		send_resp(fd, tid, f, body, 1);
		out_fmt("-> sequence %d is past the end\n", (long)seq);
		return;
	}
	send_resp(fd, tid, f, body, len);
	if (seq < 3 || (unsigned)seq + 1 == upl_total)
		out_fmt("-> [%d] class %d inst %04x mask %04x\n", (long)seq,
			(long)nl_get16(body), (long)nl_get16(body + 2),
			(long)nl_get16(body + 4));
}

struct getnext_ctx getnext;

/* End of the attribute values in a Get response's contents: three bytes of
 * result and mask, then 25 of values (G.988, the Get response layout).
 * The two masks of result 9 follow at 28 and 30. */
#define GET_VALUES_END 28u

/* How many alarm records the last Get All Alarms promised (alarm.c). Not
 * static: snapshot.c persists it across a respawn, alongside the MIB data
 * sync counter. */
uint16_t alarm_snapshot;

/* A Get of a big attribute: report the byte length and remember the value.
 *
 * The length goes back as four bytes in the attribute's place, which is the
 * one part of this exchange NOT read off the vendor. The Get-Next half
 * below is measured instruction by instruction; this half is G.988's own
 * shape, and it is the first thing to doubt if an OLT refuses the
 * exchange.
 *
 * Only one attribute is answered, and the mask says which: the vendor stops
 * looking at the mask the moment it finds a big attribute, and mixing four
 * bytes of length in among inline attribute values would leave the OLT parsing
 * everything after it at the wrong offset. */
static void handle_get_big(int fd, uint32_t tid, const uint8_t *f,
			   const struct omci_class *c, uint16_t inst,
			   unsigned k)
{
	uint8_t body[32];

	getnext.len = tbl_serialise(c, inst, k, getnext.data, GETNEXT_MAX);
	getnext.classId = c->classId;
	getnext.inst = inst;
	getnext.attr = (uint16_t)k;
	getnext.chunks = (uint16_t)((getnext.len + GETNEXT_CHUNK - 1)
				    / GETNEXT_CHUNK);
	getnext.primed = 1;

	body[0] = OMCI_OK;
	nl_put16(body + 1, (uint16_t)(1u << (16 - k)));
	nl_put32(body + 3, getnext.len);
	send_resp(fd, tid, f, body, 7);
	out_fmt("-> table get, attribute %d, %d bytes in %d chunks\n",
		(long)k, (long)getnext.len, (long)getnext.chunks);
}

/* Get-Next, from the responder at 0x413400 in bin/omci_app.
 *
 * The request carries the attribute mask at contents+0 and the sequence number
 * at contents+2. The vendor checks the class id, the instance AND the attribute
 * index against what the Get cached, and refuses rather than answering another
 * entity's bytes -- an OLT that interleaves two table reads would otherwise get
 * a silently wrong answer. */
static void handle_get_next(int fd, uint32_t tid, const uint8_t *f,
			    const struct omci_class *c, uint16_t cls,
			    uint16_t inst)
{
	uint8_t body[32];
	uint16_t mask = nl_get16(f + 8);
	uint16_t seq = nl_get16(f + 10);
	uint16_t k = 0, off, n;

	for (unsigned i = 1; i <= 16; i++)
		if (mask & (1u << (16 - i))) {
			k = (uint16_t)i;
			break;
		}
	if (!c || !getnext.primed || getnext.classId != cls
	    || getnext.inst != inst || getnext.attr != k) {
		body[0] = OMCI_ERR_BAD_PARAM;
		send_resp(fd, tid, f, body, 1);
		out_fmt("-> get-next for class %d inst %04x attr %d, "
			"which is not what the last get primed\n",
			(long)cls, (long)inst, (long)k);
		return;
	}
	if (seq >= getnext.chunks) {
		body[0] = OMCI_ERR_BAD_PARAM;
		send_resp(fd, tid, f, body, 1);
		out_fmt("-> get-next %d is past the last chunk (%d)\n",
			(long)seq, (long)getnext.chunks);
		return;
	}
	off = (uint16_t)(seq * GETNEXT_CHUNK);
	/* The last chunk is the remainder, not a padded full one: the vendor
	 * computes len - 29*seq only on the final sequence number. */
	n = (uint16_t)(seq + 1 == getnext.chunks ? getnext.len - off
						 : GETNEXT_CHUNK);
	body[0] = OMCI_OK;
	nl_put16(body + 1, mask);
	for (uint16_t i = 0; i < n; i++)
		body[3 + i] = getnext.data[off + i];
	send_resp(fd, tid, f, body, (uint16_t)(3 + n));
	out_fmt("-> get-next %d of %d, %d bytes\n",
		(long)seq, (long)getnext.chunks, (long)n);
}

unsigned long omci_frames_handled;

/* Frames dropped before handle(), per reason (RX_REJ_*). */
unsigned long omci_rx_reject[RX_REJ_N];

/* What omcid relies on in a received baseline frame, checked before handle()
 * reads or changes anything, for a frame off the line and an injected one
 * alike. The MAC may already have dropped a frame with a bad CRC; this does
 * not rely on it. A frame that fails is dropped with no answer, counted per
 * reason and logged (ev_rx_reject()), so the OLT retransmits or falls back
 * as it does for any lost frame.
 *
 * len is the number of bytes delivered. Dropped, in this order:
 *   length   fewer than the 48 bytes of a baseline frame;
 *   devid    byte 3 is not 0x0a. The extended format (0x0b) is not
 *            implemented here: dropping it, rather than reading it as a
 *            baseline frame, is what lets the OLT fall back to baseline;
 *   crc      the CRC-32 in bytes 44-47 does not match bytes 0-43.
 * Not dropped: bytes 40-43 other than 00 00 00 28, which is what our OLT
 * sends. They are inside the CRC, so with a valid CRC the OLT sent them on
 * purpose; the frame is answered and only counted and logged (action=accepted),
 * so an OLT that fills those fields differently is not silenced. */
int rx_accept(const uint8_t *f, uint32_t len)
{
	int why = -1;

	if (len < OMCI_FRAME_LEN)
		why = RX_REJ_LENGTH;
	else if (f[3] != OMCI_DEVICE_ID)
		why = RX_REJ_DEVID;
	else if (nl_get32(f + 44) != omci_crc(f, 44))
		why = RX_REJ_CRC;
	else if (nl_get32(f + 40) != OMCI_TRAILER)
		why = RX_REJ_TRAILER;
	if (why < 0)
		return 1;
	omci_rx_reject[why]++;
	ev_rx_reject(why, len > 3 ? f[3] : 0, len);
	return why == RX_REJ_TRAILER;
}

int unknown_me_ok;

/* The operations that name one managed entity: an unknown class on any of
 * them is event=unknown_me. MIB reset, upload and the alarm walks address
 * ONT data whatever the class field says, so they never are. */
static int addresses_entity(uint8_t mt)
{
	return mt == OMCI_MT_CREATE || mt == OMCI_MT_DELETE ||
	       mt == OMCI_MT_SET || mt == OMCI_MT_GET ||
	       mt == OMCI_MT_GET_NEXT || mt == OMCI_MT_TEST ||
	       mt == OMCI_MT_GET_CURRENT_DATA;
}

/* An instance omcid does not hold.
 *
 * G.988 and the stock stack answer a Get, Set or Delete of an instance that
 * does not exist with result 5, unknown instance (AUDIT B8). omcid answers
 * them as if the instance were there: the built-in values for a Get, a new
 * row for a Set, success for a Delete. Making that strict could break an OLT
 * that addresses an instance omcid fails to create on its own (the
 * companions of a MAC bridge port, say), so first the data: each (class,
 * instance) such a request names is logged once per boot (ev_no_instance),
 * and nothing about the answer changes. A Create is not one of these: it
 * makes the instance. */
static int targets_held_instance(uint8_t mt)
{
	return mt == OMCI_MT_GET || mt == OMCI_MT_GET_CURRENT_DATA ||
	       mt == OMCI_MT_SET || mt == OMCI_MT_DELETE;
}

/* OMCI_UNKNOWN_ME_OK: success for a Create, Set or Get of a class omcid has
 * no model for, the answer the stock stack gives with OMCI_FAKE_OK. Nothing
 * is stored, so a Get answers an empty attribute mask and a MIB upload never
 * lists the entity; MIB data sync still counts a faked Create or Set, as the
 * OLT counts every write it was told succeeded. docs/SETTINGS.md has the
 * risk. Returns 1 when it answered. */
static int fake_unknown(int fd, uint32_t tid, const uint8_t *f, uint8_t mt)
{
	uint8_t body[3] = { OMCI_OK, 0, 0 };

	if (!unknown_me_ok ||
	    (mt != OMCI_MT_CREATE && mt != OMCI_MT_SET && mt != OMCI_MT_GET &&
	     mt != OMCI_MT_GET_CURRENT_DATA))
		return 0;
	/* Create: result and attribute execution mask. Get: result and the
	 * answered mask, none. Set: the result alone. */
	send_resp(fd, tid, f, body, mt == OMCI_MT_SET ? 1 : 3);
	if (mt != OMCI_MT_GET && mt != OMCI_MT_GET_CURRENT_DATA)
		mib_data_sync_advance();
	out("-> ok (unknown entity, answered by OMCI_UNKNOWN_ME_OK)\n");
	return 1;
}

long reboot_pid = 1;

/* A Reboot of the ONU-G, when the owner allowed it. The stick answers first
 * and reboots a second later: SIGTERM to init is exactly what busybox reboot
 * sends, so the shutdown entries of inittab run as for any reboot, and an
 * alarm defers it without a child process or a blocked main loop. The
 * handler makes one system call and nothing else. */
static void reboot_on_alarm(int sig)
{
	(void)sig;
	/* Never 0 or below: those address a whole process group. */
	if (reboot_pid > 0)
		sys_kill(reboot_pid, 15);
}

/* OMCI_ACCEPT_REBOOT: 1 answers success and reboots, 2 answers success and
 * does not reboot, anything else, or absent, refuses. */
#define REBOOT_REFUSE 0
#define REBOOT_ACCEPT 1
#define REBOOT_ACK    2

static int accept_reboot(void)
{
	char v[4];

	if (cfg_odi_get(CFG_ODI_PATH, "OMCI_ACCEPT_REBOOT", v, sizeof v) != 1)
		return REBOOT_REFUSE;
	if (v[0] == '1')
		return REBOOT_ACCEPT;
	if (v[0] == '2')
		return REBOOT_ACK;
	return REBOOT_REFUSE;
}

/* Reboot (message type 25) of the ONU-G: the only entity G.988 defines it
 * for. Returns 0 for any other class or instance, which keeps the answer it
 * always had.
 *
 * Refused, the result is 2, command not supported (G.988 11.2.2): the
 * message is well formed and addressed to an entity that exists, so 3,
 * parameter error, would send the OLT hunting for a wrong field, and 1,
 * processing error, reads as a transient failure worth retrying. 2 says the
 * ONU never does this, which is the policy.
 *
 * The request carries a one-byte reboot condition: 0 unconditional, 1 and 2
 * only if no voice call or no emergency call is up. The stick has no voice
 * service, so no call can be up and every condition is unconditional.
 *
 * Mode 2 is for an OLT that insists on a successful answer: the Reboot is
 * answered success and nothing else happens. */
static int handle_reboot(int fd, uint32_t tid, const uint8_t *f, uint16_t cls,
			 uint16_t inst)
{
	uint8_t body[1];
	int mode;

	if (cls != OMCI_ME_ONU_G || inst != 0)
		return 0;
	mode = accept_reboot();
	ev_olt_command(OMCI_MT_REBOOT, cls, inst,
		       mode == REBOOT_ACCEPT ? "accepted" :
		       mode == REBOOT_ACK ? "acknowledged" : "refused");
	if (f[2] & OMCI_AR) {
		body[0] = mode != REBOOT_REFUSE ? OMCI_OK : OMCI_ERR_NOT_SUPPORTED;
		send_resp(fd, tid, f, body, 1);
	}
	if (mode == REBOOT_ACK) {
		out("reboot from the OLT acknowledged, not rebooting (OMCI_ACCEPT_REBOOT=2)\n");
	} else if (mode == REBOOT_ACCEPT) {
		out("reboot from the OLT accepted\n");
		out_flush();
		sys_signal(14, reboot_on_alarm);
		sys_alarm(1);
	} else {
		out("reboot from the OLT refused (OMCI_ACCEPT_REBOOT=0)\n");
	}
	return 1;
}

void handle(int fd, uint32_t tid, const uint8_t *f)
{
	uint8_t mt = OMCI_MT(f[2]);
	uint16_t cls = nl_get16(f + 4), inst = nl_get16(f + 6);
	const struct omci_class *c = find_class(cls);
	uint8_t body[32];

	/* The VoIP classes are in the model, but an ONU that tells the OLT of
	 * no phone port has no business holding them: with OMCI_POTS_PORTS at
	 * 0 every message about one is answered, logged and counted as for a
	 * class the model never had, exactly as before they were modelled. */
	if (c && !pots_ports && voip_class(cls))
		c = 0;
	omci_frames_handled++;
	if (mt == OMCI_MT_MIB_UPLOAD_NEXT)
		ev_mib_upload_next(nl_get16(f + 8));
	if (mt == OMCI_MT_MIB_UPLOAD_NEXT && nl_get16(f + 8) > 2 &&
	    (unsigned)nl_get16(f + 8) + 1 < upl_total) {
		handle_upload_next(fd, tid, f);   /* ~400 of these; do not log */
		return;
	}
	/* A software image is thousands of these: counted, not logged. */
	if (mt == OMCI_MT_DOWNLOAD_SECTION) {
		sw_handle(fd, tid, f, c);
		return;
	}
	out_fmt("<- %-16s tci %-5d class %-5d %-22s inst %d\n",
		mt == OMCI_MT_GET ? "get" :
		mt == OMCI_MT_MIB_RESET ? "mib-reset" :
		mt == OMCI_MT_MIB_UPLOAD ? "mib-upload" :
		mt == OMCI_MT_MIB_UPLOAD_NEXT ? "mib-upload-next" :
		mt == OMCI_MT_CREATE ? "create" :
		mt == OMCI_MT_DELETE ? "delete" :
		mt == OMCI_MT_SET ? "set" :
		mt == OMCI_MT_GET_NEXT ? "get-next" :
		mt == OMCI_MT_GET_CURRENT_DATA ? "get-current-data" :
		mt == OMCI_MT_GET_ALL_ALARMS ? "get-all-alarms" :
		mt == OMCI_MT_GET_ALL_ALARMS_NEXT ? "get-all-alarms-next" :
		mt == OMCI_MT_SYNC_TIME ? "sync-time" :
		mt == OMCI_MT_START_SW_DOWNLOAD ? "start-sw-download" :
		mt == OMCI_MT_END_SW_DOWNLOAD ? "end-sw-download" :
		mt == OMCI_MT_ACTIVATE_SW ? "activate-image" :
		mt == OMCI_MT_COMMIT_SW ? "commit-image" :
		mt == OMCI_MT_REBOOT ? "reboot" :
		mt == OMCI_MT_TEST ? "test" : "?",
		(long)nl_get16(f), (long)cls, c ? c->name : "(unknown)", (long)inst);
	/* Anything unnamed is what phase 4 has to learn, so show the type and
	 * enough of the contents to recognise it -- except where the contents
	 * are the line's own secrets. The tail of this OLT's provisioning sets
	 * the TR-069 ACS credentials into AuthSecMethod and the ACS URL into
	 * LargeString; dumping those puts them in a log file and, from there,
	 * anywhere the log is pasted. The shape is what matters here, not the
	 * value. */
	if (mt != OMCI_MT_GET && mt != OMCI_MT_GET_NEXT &&
	    mt != OMCI_MT_GET_CURRENT_DATA && mt != OMCI_MT_MIB_RESET && mt != OMCI_MT_MIB_UPLOAD &&
	    mt != OMCI_MT_MIB_UPLOAD_NEXT && mt != OMCI_MT_GET_ALL_ALARMS &&
	    mt != OMCI_MT_GET_ALL_ALARMS_NEXT && mt != OMCI_MT_SYNC_TIME) {
		if (me_is_secret((uint16_t)cls)) {
			out_fmt("   type %d contents withheld (credentials)\n", (long)mt);
		} else {
			out_fmt("   type %d contents", (long)mt);
			for (int i = 8; i < 24; i++) {
				out_char(' ');
				out_hex(f[i], 2);
			}
			out_char('\n');
		}
	}

	/* A repeat of a request already answered gets the same answer and
	 * changes nothing (see the cache above). */
	if ((f[2] & OMCI_AR) && retx_replay(fd, tid, f)) {
		out("-> retransmission, answer repeated\n");
		return;
	}
	ev_rx_msg(mt, cls, inst);
	/* Before the AR check, so a request that asks for no answer is
	 * recorded too. */
	if (!ev_msg_known(mt))
		ev_unknown_msg(mt, cls);
	else if (!c && addresses_entity(mt))
		ev_unknown_me(cls, mt);
	else if (c && targets_held_instance(mt) && !mib_find(cls, inst) &&
		 !instance_is_autonomous(cls, inst))
		ev_no_instance(cls, inst, mt);

	/* The software download steps answer for themselves (swimage.c). */
	if (mt >= OMCI_MT_START_SW_DOWNLOAD && mt <= OMCI_MT_COMMIT_SW) {
		sw_handle(fd, tid, f, c);
		return;
	}
	if (mt == OMCI_MT_REBOOT) {
		if (handle_reboot(fd, tid, f, cls, inst))
			return;
		ev_olt_command(mt, cls, inst, "not_supported");
	}

	if (!(f[2] & OMCI_AR)) {                 /* nothing to answer */
		out("   (no ack requested)\n");
		return;
	}
	if (mt == OMCI_MT_MIB_RESET) {
		ev_mib_reset("olt");
		mib_reset_all();
		body[0] = OMCI_OK;
		send_resp(fd, tid, f, body, 1);
		out("-> ok, mib data sync reset\n");
		return;
	}
	if (mt == OMCI_MT_GET_ALL_ALARMS) {
		/* Same shape as MIB upload: a count of the Next commands to
		 * follow, no result byte. The snapshot and the sequence number
		 * reset are alarm.c's (OMCI_OnGetAllAlarms, 0x412eac). */
		uint16_t n = alarm_get_all();

		nl_put16(body, n);
		send_resp(fd, tid, f, body, 2);
		out_fmt("-> %d entities with alarms\n", (long)n);
		return;
	}
	if (mt == OMCI_MT_GET_ALL_ALARMS_NEXT) {
		/* Class id, instance id and the 28-byte alarm bitmap -- the
		 * whole 32-byte contents field (0x412c58). Past the end of the
		 * snapshot the stock answer is a failure result, not a
		 * zero-filled record. */
		uint16_t seq = nl_get16(f + 8);

		if (alarm_get_next(seq, body) == 0) {
			send_resp(fd, tid, f, body, 32);
			out_fmt("-> get-all-alarms-next %d: class %d inst %04x\n",
				(long)seq, (long)nl_get16(body), (long)nl_get16(body + 2));
			return;
		}
		body[0] = OMCI_ERR_BAD_PARAM;
		send_resp(fd, tid, f, body, 1);
		out_fmt("-> get-all-alarms-next %d, but the snapshot holds %d\n",
			(long)seq, (long)alarm_snapshot);
		return;
	}
	if (mt == OMCI_MT_SYNC_TIME) {
		/* Year (2 bytes), month, day, hour, minute, second: only the
		 * time of day matters, it places the 15-minute PM boundaries
		 * (pm.c). An OLT that sends no date still sends the time. */
		pm_sync_time(f[12], f[13], f[14]);
		body[0] = OMCI_OK;
		body[1] = 0;                     /* success, no offset */
		send_resp(fd, tid, f, body, 2);
		out("-> time accepted\n");
		return;
	}
	if (mt == OMCI_MT_MIB_UPLOAD) {
		/* A count of the Next commands to follow, no result byte. After
		 * a MIB reset the ONU's MIB is not empty: it holds what the ONU
		 * built for itself, and that is how the OLT learns what
		 * hardware it has. Reporting zero left the OLT with nothing to
		 * work with, and it simply started the cycle again. */
		unsigned total = mib_upload_build();

		nl_put16(body, (uint16_t)total);
		send_resp(fd, tid, f, body, 2);
		ev_mib_upload((uint16_t)total);
		out_fmt("-> %d upload records (%d autonomous, %d held, %d fixed)\n",
			(long)total, (long)aut_count,
			(long)mib_count(), (long)fixed_me_upload(-1, 0, 0));
		return;
	}
	if (mt == OMCI_MT_MIB_UPLOAD_NEXT) {
		handle_upload_next(fd, tid, f);
		return;
	}
	if (mt == OMCI_MT_GET_NEXT) {
		handle_get_next(fd, tid, f, c, cls, inst);
		return;
	}
	/* Get current data (G.988, message type 28) has Get's layout both ways
	 * and asks a PM entity for the counters of the interval in progress
	 * rather than of the last one completed. omcid keeps one set of values
	 * per entity, so both answer from it. An OLT that polls with it got
	 * "not supported" once a minute (a reporter's bundle, 2026-10). */
	if ((mt == OMCI_MT_GET || mt == OMCI_MT_GET_CURRENT_DATA) && c) {
		uint16_t mask = nl_get16(f + 8);
		uint16_t got = 0, unsup = 0, failed = 0;
		uint16_t n = 0, vlen;

		/* A big attribute takes the whole message: the vendor breaks
		 * out of the mask walk at the first one it finds and hands the
		 * message to the Get-Next state machine. Doing this before the
		 * inline loop, not inside it, is what keeps the answer's shape
		 * single-valued. */
		for (unsigned k = 1; k <= 16; k++)
			if ((mask & (1u << (16 - k))) && attr_is_big(c, k)) {
				handle_get_big(fd, tid, f, c, inst, k);
				return;
			}

		/* G.988 lays a Get response out as the result, the mask
		 * answered, 25 bytes of attribute values (contents 3 to 27),
		 * then the optional-attribute mask (28-29) and the attribute
		 * execution mask (30-31), which only result 9 uses. Values stop
		 * at byte 27, so they never run into the two masks. */
		n = 3;
		/* A PM entity answers its completed interval to a Get and the
		 * one in progress to Get current data (pm.c). */
		pm_view(mt == OMCI_MT_GET_CURRENT_DATA);
		for (unsigned k = 1; k <= 16; k++) {
			uint16_t bit = (uint16_t)(1u << (16 - k));
			uint16_t w;

			if (!(mask & bit))
				continue;
			w = attr_width(c, k);
			if (!w) {
				unsup |= bit;   /* past nattr, or unsized */
				continue;
			}
			if (failed || n + w > GET_VALUES_END) {
				/* It does not fit; nor does anything after
				 * it, so the values stay in mask order and the
				 * OLT asks for these again on their own. */
				failed |= bit;
				continue;
			}
			attr_value(c, inst, k, body + n);
			got |= bit;
			n = (uint16_t)(n + w);
		}
		pm_view(0);
		/* The mask that goes back is the one actually answered, not the
		 * one asked for: echoing the request while leaving an
		 * attribute out tells the OLT bytes are present that are not,
		 * and it then parses every attribute after the gap at the
		 * wrong offset. handle_upload_next builds its mask the same
		 * way. Anything left out is result 9, attribute(s) failed or
		 * unknown, with the two masks saying which and why. */
		nl_put16(body + 1, got);
		vlen = (uint16_t)(n - 3);
		if (unsup || failed) {
			body[0] = OMCI_ERR_ATTR_FAILED;
			for (unsigned b = n; b < GET_VALUES_END; b++)
				body[b] = 0;
			nl_put16(body + GET_VALUES_END, unsup);
			nl_put16(body + GET_VALUES_END + 2, failed);
			n = GET_VALUES_END + 4;
		} else {
			body[0] = OMCI_OK;
		}
		send_resp(fd, tid, f, body, n);
		if (body[0] == OMCI_OK)
			out_fmt("-> ok, mask %04x", (long)mask);
		else
			out_fmt("-> attribute failed, mask %04x (answered %04x, unsupported %04x, failed %04x)",
				(long)mask, (long)got, (long)unsup, (long)failed);
		out_fmt(", %d bytes\n", (long)vlen);
		return;
	}
	/* A class OMCI_FIXED_ME names, when omcid has no model of its own for
	 * it: the configured bytes under the mask the OLT asked for. The bytes
	 * are not logged; they are the owner's (cfgstore.c). */
	if ((mt == OMCI_MT_GET || mt == OMCI_MT_GET_CURRENT_DATA) && !c &&
	    fixed_me_per_inst(cls)) {
		/* A per-instance entity: the attributes asked for that it has,
		 * in mask order, under the mask actually answered, as for a
		 * modelled entity above. Another instance of the class does
		 * not exist. */
		uint16_t mask = nl_get16(f + 8), got = 0, n = 3;

		if (!fixed_me_has_inst(cls, inst)) {
			body[0] = OMCI_ERR_UNKNOWN_INST;
			send_resp(fd, tid, f, body, 1);
			out("-> unknown instance (OMCI_FIXED_ME)\n");
			return;
		}
		body[0] = OMCI_OK;
		for (unsigned k = 1; k <= 16; k++) {
			const struct fixed_me *x;

			if (!(mask & (1u << (16 - k))))
				continue;
			x = fixed_me_attr(cls, inst, k);
			if (!x || n + x->len > GET_VALUES_END)
				continue;
			for (int i = 0; i < x->len; i++)
				body[n + i] = x->data[i];
			got |= (uint16_t)(1u << (16 - k));
			n = (uint16_t)(n + x->len);
		}
		nl_put16(body + 1, got);
		send_resp(fd, tid, f, body, n);
		out_fmt("-> ok, mask %04x", (long)mask);
		if (got != mask)
			out_fmt(" (answered %04x)", (long)got);
		out_fmt(", %d bytes (OMCI_FIXED_ME)\n", (long)(n - 3));
		return;
	}
	if ((mt == OMCI_MT_GET || mt == OMCI_MT_GET_CURRENT_DATA) && !c) {
		const struct fixed_me *fx = fixed_me_find(cls);

		if (fx) {
			uint16_t mask = nl_get16(f + 8);

			body[0] = OMCI_OK;
			nl_put16(body + 1, mask);
			for (int i = 0; i < fx->len; i++)
				body[3 + i] = fx->data[i];
			send_resp(fd, tid, f, body, (uint16_t)(3 + fx->len));
			out_fmt("-> ok, mask %04x, %d bytes (OMCI_FIXED_ME)\n",
				(long)mask, (long)fx->len);
			return;
		}
	}
	/* Accept what the OLT provisions without acting on it. Nothing here
	 * configures hardware or keeps a MIB: the point is to see the whole
	 * script the line runs, which only appears if each step is
	 * acknowledged. A refusal stops the conversation at that step. */
	/* Class 46 is the ONU's own, created and deleted with its profile and
	 * read-only: G.988 gives the OLT only a Get. Its model lists Get as the
	 * one message it takes, so that is what is checked, and a Create, Set
	 * or Delete is refused before it can add a row, write one or remove
	 * the entity from under the profile. The voice classes are checked the
	 * same way: the ONU creates the POTS UNI, its line status and the VoIP
	 * config data (the OLT may not create or delete them, nor Set the line
	 * status), and their models say so. Every other class keeps the
	 * answer-everything behaviour below. */
	if (c && (cls == OMCI_ME_MAC_BRIDGE_CFG_DATA || voip_class(cls)) &&
	    (mt == OMCI_MT_CREATE || mt == OMCI_MT_SET || mt == OMCI_MT_DELETE) &&
	    !(c->actions & (1u << mt))) {
		body[0] = OMCI_ERR_NOT_SUPPORTED;
		send_resp(fd, tid, f, body, 1);
		out(cls == OMCI_ME_MAC_BRIDGE_CFG_DATA
		    ? "-> not supported (the ONU creates it, the OLT can only get it)\n"
		    : "-> not supported (the ONU creates it, the OLT may not do this)\n");
		return;
	}
	if (mt == OMCI_MT_CREATE) {
		struct mib_row *r;
		uint16_t mask;

		if (!c && fake_unknown(fd, tid, f, mt))
			return;
		/* G.988: a Create of an instance that exists is refused with
		 * result 7 and changes nothing, MIB data sync included. A
		 * repeat of one request is answered before this point, from
		 * the retransmission cache. */
		if (c && mib_find(cls, inst)) {
			body[0] = OMCI_ERR_INST_EXISTS;
			send_resp(fd, tid, f, body, 1);
			out("-> instance exists\n");
			return;
		}
		/* A multicast profile naming a value G.988 does not define for
		 * a few-valued attribute is not created: result 9 and the
		 * attributes at fault (mcast.c says which are judged). */
		if (c && cls == 309) {
			uint16_t bad = mcast_value_reject(c, create_mask(c), f + 8, 32);

			if (bad) {
				body[0] = OMCI_ERR_ATTR_FAILED;
				nl_put16(body + 1, bad);
				send_resp(fd, tid, f, body, 3);
				out_fmt("-> refused: undefined value in %04x\n", (long)bad);
				return;
			}
		}
		r = c ? mib_add(cls, inst) : 0;
		if (r)
			pm_create(cls, inst);
		if (!r) {
			body[0] = c ? OMCI_ERR_CMD : OMCI_ERR_UNKNOWN_ME;
			send_resp(fd, tid, f, body, 1);
			out(c ? "-> no room in the MIB\n" : "-> unknown entity\n");
			return;
		}
		/* Create carries, in order and with no mask, only the
		 * attributes the OLT may set at creation -- which the model
		 * records as bit 2 of oltAcc. GemPortCtp's UNI counter is
		 * read-only and therefore absent, so taking every attribute in
		 * order shifts everything after it: the port id came out as
		 * 4095 before this. */
		mask = create_mask(c);
		mib_write(r, c, mask, 0, f + 8, 32);
		if (cls == OMCI_ME_GEM_PORT_CTP || cls == OMCI_ME_TCONT) {
			out_fmt("   create mask %04x raw", (long)mask);
			for (int i = 8; i < 28; i++) {
				out_char(' ');
				out_hex(f[i], 2);
			}
			out_char('\n');
			out_fmt("   parsed port %d tcont %d dir %d usTM %d dsPQ %d\n",
				(long)row_u32(r, c, 1), (long)row_u32(r, c, 2),
				(long)row_u32(r, c, 3), (long)row_u32(r, c, 4),
				(long)row_u32(r, c, 7));
		}
		body[0] = OMCI_OK;
		body[1] = 0;                     /* attribute execution mask */
		body[2] = 0;
		mib_data_sync_advance();
		if (cls == OMCI_ME_MAC_BRIDGE_PORT_CFG_DATA)
			mbpcd_tables_add(inst);
		if (cls == OMCI_ME_MAC_BRIDGE_SERVICE_PROFILE)
			mbcd_add(inst);
		/* Saved before the answer: once the OLT is told the entity
		 * exists, a respawn must find it. Saved again after the apply,
		 * which may add what it programmed. */
		snapshot_save();
		send_resp(fd, tid, f, body, 3);
		ev_config_write(mt, cls, inst);
		apply_entity(c, inst, r, mask, 1);
		snapshot_save();
		out_fmt("-> created (%d rows held)\n", (long)mib_count());
		return;
	}
	if (mt == OMCI_MT_SET) {
		struct mib_row *r = c ? mib_add(cls, inst) : 0;
		uint16_t mask = nl_get16(f + 8);
		uint16_t failed = r ? set_reject_mask(c, mask) : 0;
		int done;

		/* The same for a Set of a multicast profile: the attribute with
		 * the undefined value is refused, the others go through. */
		if (r && cls == 309)
			failed |= mcast_value_reject(c, mask, f + 10, 30);

		if (!c && fake_unknown(fd, tid, f, mt))
			return;
		/* The attributes the OLT may not write (read-only, or settable
		 * only at Create) are skipped, not stored, and named in the
		 * answer; the rest of the Set goes through. */
		if (r)
			mib_write(r, c, mask, failed, f + 10, 30);
		mask &= (uint16_t)~failed;
		/* Everything but a Set that stored nothing because every
		 * attribute it named was refused. */
		done = r && (!failed || mask);
		body[0] = !r ? OMCI_ERR_UNKNOWN_ME :
			  failed ? OMCI_ERR_ATTR_FAILED : OMCI_OK;
		/* Result 9 carries the unsupported-optional mask (none: the
		 * model has no verified notion of it) and the failed mask. */
		nl_put16(body + 1, 0);
		nl_put16(body + 3, failed);
		/* The MIB changed when any attribute was stored. The OLT
		 * counts the Sets it was told succeeded, so a partly failed
		 * one can leave the counts apart, which its audit finds and
		 * resolves; a refused one changed nothing and is not counted.
		 * Saved before the answer, as for a Create. */
		if (done) {
			mib_data_sync_advance();
			snapshot_save();
		}
		send_resp(fd, tid, f, body, failed ? 5 : 1);
		if (done)
			ev_config_write(mt, cls, inst);
		if (done) {
			apply_entity(c, inst, r, mask, 0);
			snapshot_save();
		}
		out_fmt("-> set mask %04x%s", (long)mask,
			r && r->truncated ? " (row truncated)" : "");
		if (failed)
			out_fmt(", refused %04x (read-only or unknown)", (long)failed);
		out_char('\n');
		return;
	}
	if (mt == OMCI_MT_DELETE) {
		/* Before mib_del, so the row is still there for anything that
		 * needs it. Class 45 is the only entity whose delete reaches
		 * the driver at all. */
		apply_delete(c, inst);
		mib_del(cls, inst);
		if (cls == OMCI_ME_GEM_PORT_CTP)
			bc_gem_update(); /* only surviving CTPs may be selected */
		pm_delete(cls, inst);
		if (cls == OMCI_ME_MAC_BRIDGE_PORT_CFG_DATA)
			mbpcd_tables_del(inst);
		if (cls == OMCI_ME_MAC_BRIDGE_SERVICE_PROFILE)
			mbcd_del(inst);
		body[0] = OMCI_OK;
		mib_data_sync_advance();
		snapshot_save();               /* before the answer, as above */
		send_resp(fd, tid, f, body, 1);
		ev_config_write(mt, cls, inst);
		out("-> deleted\n");
		return;
	}
	if (mt == OMCI_MT_TEST) {
		handle_test(fd, tid, f, cls, c);
		return;
	}
	if (!c && fake_unknown(fd, tid, f, mt))
		return;
	body[0] = c ? OMCI_ERR_CMD : OMCI_ERR_UNKNOWN_ME;
	send_resp(fd, tid, f, body, 1);
	out("-> not supported yet\n");
}
