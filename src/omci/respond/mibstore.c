/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */

/* The MIB store: what the OLT has created and set.
 *
 * Split out of main.c; see omcid.h.
 */
#include "omcid.h"
#include "omci_defaults.h"
#include "../procparse.h"

uint8_t serial[9];      /* 4 ASCII vendor id + 4 binary */

/* Classes whose contents may be a secret of whoever provisions them, in one
 * place so the log, the dumps and anything added later cannot disagree.
 * `first` is the first attribute index that carries the secret (masked in
 * a dump, up to `last`); 0xff means the class is kept out of the log but has
 * no secret attribute to mask.
 *
 *   148  authentication security method: username, password, realm
 *   157  large string: URLs, which may carry user:password@ (the ACS URL)
 *   307  octet string: the octet-sized twin of 157 -- opaque bytes up to 375
 *        long with no meaning of its own, so what an OLT puts in one (a
 *        credential, a certificate) cannot be ruled out
 *   340  TR-069 management server and 134 IP host config data: their own
 *        attributes are addresses and pointers, but the OLT sets the
 *        credentials around them, and the log never showed their contents
 *   65530 LOID authentication: the LOID and its password (attributes 2 and
 *        3), the line owner's from the config store; the operation id and
 *        the status the OLT sets stay readable
 *   256  ONU-G: the logical ONU id and logical password (attributes 10 and
 *        11) are the same LOID and password. Masked in a dump only: the
 *        OLT's Sets of ONU-G carry nothing secret and stay in the log
 *   153  SIP user data: the display name (attribute 3), the subscriber's
 *        own; the rest are pointers. The SIP credentials themselves are an
 *        authentication method (148) and the number a large string (157),
 *        both above
 *
 * `last` is the last attribute masked; 0xff is every one from `first` on.
 * `log` keeps the contents of a Create, Set or Delete out of the log.
 */
static const struct { uint16_t cls; uint8_t first, last, log; } secret_me[] = {
	{ OMCI_ME_AUTH_SECURITY_METHOD, 2, 0xff, 1 },
	{ OMCI_ME_LARGE_STRING, 2, 0xff, 1 },
	{ OMCI_ME_OCTET_STRING, 2, 0xff, 1 },
	{ OMCI_ME_TR069_MGMT_SERVER, 0xff, 0xff, 1 },
	{ OMCI_ME_IP_HOST_CONFIG_DATA, 0xff, 0xff, 1 },
	{ OMCI_ME_CTC_LOID_AUTH, 2, 3, 1 },
	{ OMCI_ME_ONU_G, 10, 11, 0 },
	{ OMCI_ME_SIP_USER_DATA, 3, 3, 1 },
};

int me_is_secret(uint16_t cls)
{
	for (unsigned i = 0; i < sizeof secret_me / sizeof secret_me[0]; i++)
		if (secret_me[i].cls == cls)
			return secret_me[i].log;
	return 0;
}

int me_attr_secret(uint16_t cls, unsigned k)
{
	for (unsigned i = 0; i < sizeof secret_me / sizeof secret_me[0]; i++)
		if (secret_me[i].cls == cls)
			return k >= secret_me[i].first && k <= secret_me[i].last;
	return 0;
}

/* Where serial[] came from: 0 nowhere yet (all zeroes), 1 the config store,
 * 2 the kernel. Anything below 2 is retried by serial_refresh(). */
static int serial_src;

/* The serial number, and why it is not simply read once at startup.
 *
 * rcS starts omcid at the "omcimods" pon step and programs the serial at the
 * "gponsn" step right after it, so the one read omcid used to make at startup
 * raced the step that sets it and, when it lost, answered 00000000 for the rest
 * of the run: ONU-G serial, `omcicli get sn`, `omcli state`, every MIB dump.
 *
 * So this is asked again until the kernel has a real one, from the places that
 * know it, in order of authority:
 *
 *   1. the driver, over the same command the OMCI stack uses for everything
 *      else (DRV_GET_SN, answered by odi_switch from the GPON block);
 *   2. /proc/odi_gpon/status, whose "sn" line is the same GPON block value, for when
 *      the command path is not answering;
 *   3. GPON_SN in the config store, which is what the gponsn step programs --
 *      used only until the kernel has one, and never over it.
 *
 * All zeroes means "not set yet" from every source. With none of the three
 * available the serial stays zero, which is what it was before, and omcid
 * runs regardless. */
static int serial_from_kernel(uint8_t sn[8])
{
	uint8_t buf[9];
	char proc[128];
	long fd, n;

	if (omci_drv_call(DRV_GET_SN, buf, sizeof buf) == 0 && !pp_sn_is_zero(buf)) {
		for (int i = 0; i < 8; i++)
			sn[i] = buf[i];
		return 0;
	}
	fd = sys_open(gpon_proc_path, O_RDONLY);
	if (fd < 0)
		return -1;
	n = sys_read((int)fd, proc, sizeof proc);
	sys_close((int)fd);
	if (n <= 0 || pp_gpon_sn(proc, (unsigned)n, sn) != 0 || pp_sn_is_zero(sn))
		return -1;
	return 0;
}

static unsigned serial_asked;

void serial_forget(void)
{
	serial_src = 0;
	serial_asked = 0;
}

int serial_refresh(int log)
{
	uint8_t sn[8];

	if (serial_src == 2)
		return 0;
	/* Once a second from the main loop; a kernel that still has no serial
	 * after fifteen minutes is not going to get one from waiting. */
	if (serial_asked >= 900)
		return serial_src ? 0 : -1;
	serial_asked++;
	if (serial_from_kernel(sn) == 0) {
		for (int i = 0; i < 8; i++)
			serial[i] = sn[i];
		serial[8] = 0;
		serial_src = 2;
		if (log)
			out("serial number: from the kernel\n");
		return 0;
	}
	if (serial_src == 0) {
		char label[16];
		int len = cfg_get("/var/config/lastgood_hs.xml", "GPON_SN",
				  label, sizeof label);

		if (len > 0 && pp_label_sn(label, (unsigned)len, sn) == 0 &&
		    !pp_sn_is_zero(sn)) {
			for (int i = 0; i < 8; i++)
				serial[i] = sn[i];
			serial[8] = 0;
			serial_src = 1;
			if (log)
				out("serial number: from the config store "
				    "until the kernel has one\n");
		}
	}
	return serial_src ? 0 : -1;
}

uint8_t devid[40];      /* "RTL9602C", then the revision */

uint8_t mib_data_sync;

/* Counts one more change of the MIB. 0 is reserved for a MIB just
 * instantiated or reset, so the counter goes from 255 to 1, never to 0. */
void mib_data_sync_advance(void)
{
	mib_data_sync = (uint8_t)(mib_data_sync + 1);
	if (mib_data_sync == 0)
		mib_data_sync = 1;
}

struct omci_instance aut[AUT_MAX];
unsigned aut_count;

/* The entities that live in the VEIP slot: the cardholder and circuit pack
 * (entity id 0x0100 | slot) and the VEIP, its UNI-G and the TR-069 server
 * (slot << 8 | 1). The generated tables hold them for slot 6. */
#define SLOT6_CARD   0x0106
#define SLOT6_PORT   0x0601

static int slot_card_class(uint16_t cls) { return cls == 5 || cls == 6; }
static int slot_port_class(uint16_t cls)
{
	return cls == 264 || cls == OMCI_ME_VEIP || cls == OMCI_ME_TR069_MGMT_SERVER;
}
static int private_class(uint16_t cls) { return cls == 350 || cls == 370 || cls == 373; }

/* The cardholder and circuit pack id of the phone ports' slot. */
static int pots_card(uint16_t inst)
{
	return pots_ports && inst == (uint16_t)(0x0100 | pots_slot);
}

static void aut_push(unsigned *n, uint16_t cls, uint16_t inst)
{
	if (*n < AUT_MAX) {
		aut[*n].classId = cls;
		aut[*n].inst = inst;
		(*n)++;
	}
}

void aut_rebuild(void)
{
	unsigned n = 0;

	for (unsigned i = 0; i < omci_autonomous_count && n < AUT_MAX; i++) {
		uint16_t cls = omci_autonomous[i].classId;
		uint16_t inst = omci_autonomous[i].inst;

		if (!private_me && private_class(cls))
			continue;
		if (cls == OMCI_ME_ONU_CAPABILITY &&
		    !(custom_me & CUSTOM_ME_ONU_CAPABILITY))
			continue;
		if (!veip_on && ((slot_card_class(cls) && inst == SLOT6_CARD) ||
				 (slot_port_class(cls) && inst == SLOT6_PORT)))
			continue;
		if (slot_card_class(cls) && inst == SLOT6_CARD)
			inst = (uint16_t)(0x0100 | veip_slot);
		else if (slot_port_class(cls) && inst == SLOT6_PORT)
			inst = (uint16_t)((veip_slot << 8) | 1);
		aut[n].classId = cls;
		aut[n].inst = inst;
		n++;
	}
	/* The phone ports OMCI_POTS_PORTS asks for (G.988 9.1.5, 9.9.1,
	 * 9.9.11, 9.9.18): a cardholder and a circuit pack of type POTS in
	 * their own slot, one PPTP POTS UNI per port, with the UNI-G and the
	 * VoIP line status the ONU creates beside each, and the one VoIP
	 * config data. After everything else, so every entity before them
	 * keeps its place in the MIB upload. */
	if (pots_ports) {
		aut_push(&n, OMCI_ME_CARDHOLDER, (uint16_t)(0x0100 | pots_slot));
		aut_push(&n, OMCI_ME_CIRCUIT_PACK, (uint16_t)(0x0100 | pots_slot));
		for (int p = 1; p <= pots_ports; p++) {
			uint16_t id = (uint16_t)(pots_slot << 8 | p);

			aut_push(&n, OMCI_ME_PPTP_POTS_UNI, id);
			aut_push(&n, OMCI_ME_UNI_G, id);
			aut_push(&n, OMCI_ME_VOIP_LINE_STATUS, id);
		}
		aut_push(&n, OMCI_ME_VOIP_CONFIG_DATA, 0);
	}
	aut_count = n;
	/* The entities that raise alarms and AVCs are these. */
	alarm_init();
}

uint16_t canon_inst(uint16_t cls, uint16_t inst)
{
	if (slot_card_class(cls) && inst == (uint16_t)(0x0100 | veip_slot))
		return SLOT6_CARD;
	if (slot_port_class(cls) && inst == (uint16_t)((veip_slot << 8) | 1))
		return SLOT6_PORT;
	return inst;
}

/* The VoIP classes of G.988 9.9 the model carries for SIP over OMCI
 * (model/profile.json, not stock-backed), which handle() answers only while
 * OMCI_POTS_PORTS is on. The large string, network address, authentication
 * method, TCP/UDP config data and IP host they point at are not among them:
 * the stock plugins carry those, and the OLT has always been answered for
 * them (the TR-069 server uses the same ones). */
int voip_class(uint16_t cls)
{
	switch (cls) {
	case OMCI_ME_PPTP_POTS_UNI:
	case OMCI_ME_VOICE_SERVICE_PROFILE:
	case OMCI_ME_VOIP_CONFIG_DATA:
	case OMCI_ME_VOIP_VOICE_CTP:
	case OMCI_ME_CALL_CONTROL_PMHD:
	case OMCI_ME_VOIP_LINE_STATUS:
	case OMCI_ME_VOIP_MEDIA_PROFILE:
	case OMCI_ME_RTP_PROFILE_DATA:
	case OMCI_ME_RTP_PMHD:
	case OMCI_ME_NETWORK_DIAL_PLAN_TABLE:
	case OMCI_ME_VOIP_APP_SERVICE_PROFILE:
	case OMCI_ME_VOIP_FEATURE_ACCESS_CODES:
	case OMCI_ME_SIP_AGENT_CFG_DATA:
	case OMCI_ME_SIP_AGENT_PMHD:
	case OMCI_ME_SIP_CALL_INIT_PMHD:
	case OMCI_ME_SIP_USER_DATA:
		return 1;
	default:
		return 0;
	}
}

const struct omci_class *find_class(uint16_t id)
{
	for (unsigned i = 0; i < omci_class_count; i++)
		if (omci_classes[i].classId == id)
			return &omci_classes[i];
	return 0;
}

/* Power-on values the vendor stack initialises these entities to, which an
 * OLT that never writes them leaves in place: the TR-069 management server
 * and the virtual Ethernet interface point start locked (1), and the
 * interface point is then disabled (1). Class 65535 (private VLAN
 * configuration) starts as auto detection with the unset sentinels. */
#define ADMIN_LOCKED     1
#define OPER_DISABLED    1

/* The administrative state of VEIP instance `inst`: what the OLT wrote, else
 * locked. The operational state follows it: an unlocked interface point is
 * enabled (0), a locked one disabled (1). */
static uint8_t veip_admin(uint16_t inst)
{
	const struct omci_class *c = find_class(OMCI_ME_VEIP);
	const struct mib_row *r = mib_find(OMCI_ME_VEIP, inst);

	if (c && r && (r->written & (1u << (16 - 1))))
		return r->data[attr_offset(c, 1)];
	return ADMIN_LOCKED;
}

/* PPTP Ethernet UNI: the card type of the 10/100/1000 BASE-T slot, and the
 * configuration indication for 1000 Mbit/s full duplex. */
#define UNI_SENSED_TYPE   0x2f
#define UNI_CFG_IND_1G_FULL 0x03

/* ONU2-G attribute 2: baseline OMCC only, what omcid has always answered. */
#define OMCC_VER_BASELINE 0x80

static void put_str(uint8_t *out, uint16_t n, const char *s)
{
	uint16_t i = 0;

	for (; s[i] && i < n; i++)
		out[i] = (uint8_t)s[i];
	for (; i < n; i++)
		out[i] = 0;
}

/* Attribute k (1-based, as the mask counts) of one managed entity. Returns its
 * width from the model, having written that many bytes. Anything this does not
 * know about answers as zeroes of the right size, which is a valid answer for
 * every attribute the OLT asks about here. */
/* The model's size is what the vendor stores, and it keeps strings
 * NUL-terminated -- ONU-G's vendor id is 5 bytes there and 4 on the wire. Every
 * field after a string would be shifted by one without this. */
uint16_t attr_width(const struct omci_class *c, unsigned k)
{
	uint16_t n;

	if (k >= c->nattr)
		return 0;
	n = c->attrs[k].size;
	if (c->attrs[k].type == OMCI_STRING && n)
		n--;
	return n;
}

struct mib_tblent tblpool[MIB_TBL_NODES];

struct mib_row mib[MIB_ROWS];

/* The side pool: bytes MIB_ROW_MAX and up of the rows of the classes that
 * need them. A slot is free when no row in use points at it, so freeing a
 * row, resetting the MIB and loading a snapshot need no bookkeeping of their
 * own. */
static uint8_t wide_pool[MIB_WIDE_ROWS][MIB_WIDE_MAX - MIB_ROW_MAX];

/* Whether the model packs more than MIB_ROW_MAX bytes of attributes for the
 * class, computed from the model rather than listed: a class a later model
 * adds, or a wider one, is covered without anyone remembering this. Today it
 * is the large strings (157, 373, 307), the authentication method (148), the
 * IP host (134), the extended multicast profile (65531) and 65534. */
static int class_wide(const struct omci_class *c)
{
	unsigned off = 0;

	for (unsigned k = 1; c && k < c->nattr && k <= 16; k++)
		off += attr_width(c, k);
	return off > MIB_ROW_MAX;
}

static uint8_t *wide_alloc(void)
{
	static int warned;

	for (int s = 0; s < MIB_WIDE_ROWS; s++) {
		int used = 0;

		for (int i = 0; i < MIB_ROWS; i++)
			if (mib[i].used && mib[i].tail == wide_pool[s])
				used = 1;
		if (!used) {
			for (unsigned j = 0; j < sizeof wide_pool[s]; j++)
				wide_pool[s][j] = 0;
			return wide_pool[s];
		}
	}
	/* Exhausted: the row keeps the 96 bytes it always had and says so
	 * (`truncated`, once an attribute does not fit). */
	if (!warned) {
		warned = 1;
		out_fmt("   wide-row pool of %d is full; later wide rows are truncated\n",
			(long)MIB_WIDE_ROWS);
	}
	return 0;
}

/* Offset of attribute k in our packed row, or -1 if it does not fit. */
int attr_offset(const struct omci_class *c, unsigned k)
{
	int off = 0;

	for (unsigned i = 1; i < k; i++) {
		off += attr_width(c, i);
		if (off > MIB_WIDE_MAX)
			return -1;
	}
	return off;
}

/* The attributes a create carries: oltAcc bit 2 is set-by-create, bit 1 write,
 * bit 0 read. EntityId reads 5, an ordinary writable attribute 7, and a
 * read-only one like GemPortCtp's UNI counter reads 1. */
#define OLT_ACC_CREATE 4

uint16_t create_mask(const struct omci_class *c)
{
	uint16_t m = 0;

	for (unsigned k = 1; k < c->nattr && k <= 16; k++)
		if (c->attrs[k].oltAcc & OLT_ACC_CREATE)
			m |= (uint16_t)(1u << (16 - k));
	return m;
}

/* The attributes of `mask` a Set must refuse: the ones the OLT may not write
 * (oltAcc bit 1 clear -- read-only, or set-by-create, which only a Create
 * carries) and the ones the class does not have at all. G.988 answers a Set
 * that names any of them with result 9 and these bits in the failed-
 * attributes mask. */
uint16_t set_reject_mask(const struct omci_class *c, uint16_t mask)
{
	uint16_t m = 0;

	for (unsigned k = 1; k <= 16; k++) {
		if (!(mask & (1u << (16 - k))))
			continue;
		if (k >= c->nattr || !(c->attrs[k].oltAcc & OLT_ACC_WRITE))
			m |= (uint16_t)(1u << (16 - k));
	}
	return m;
}

/* The table attributes, and the rule each one's rows follow.
 *
 * The model cannot say which attributes are tables: it records them as
 * OMCI_OCTETS of one row's width, like any fixed-width octet string (class
 * 84's 24-byte VLAN list is one of those and is NOT a table). So they are
 * named here, each with the rule a Set follows. The rules are G.988's,
 * checked against the stock plugin of the class:
 *
 *   TBL_EVTOCD   171/6, 16 bytes. Keyed by the filter half, bytes 0..7; a row
 *                whose treatment half (8..15) is all ones deletes the row
 *                with that key, anything else adds or replaces it.
 *                (ExtVlanTagOperCfgDataDrvCfg.)
 *   TBL_CTRL     a two-byte table control first: bits 15-14 the set control
 *                (01 write, 10 delete, 11 clear the table, 00 ignored), bits
 *                9-0 the row key. 310/6. (MopEntryOper.)
 *   TBL_CTRL_PART the same, plus a row part in bits 13-11: a row is the pair
 *                (part, key); a write replaces that pair, a delete removes
 *                every part of the key. 309/7 and /8, 65531/7 and /8, 310/7.
 *                Like the stock stack (omci_adjust_tbl_ctrl_by_omcc_ver), the
 *                part bits count only when the reported OMCC version is 0xA1
 *                or later: before that G.988 had one-part rows and the bits
 *                were reserved. (AclTableEntryOper.)
 *   TBL_MCADDR   281/9 (12 bytes) and /10 (24): keyed by the GEM port and the
 *                secondary key, bytes 0..3; a row whose address bytes (the
 *                rest) are all zero deletes, anything else adds or replaces.
 *                (MultiGemIwTpDrvCfg; stock tests only the 12 high bytes of
 *                an IPv6 row, this tests them all.)
 *   TBL_MACFILT  49/1, 8 bytes: byte 0 the index, which is the key; byte 1
 *                bits 7-6 10 add or replace, 00 remove, 01 clear the table.
 *                (MacFilterTableOper removes by MAC address instead; G.988
 *                keys the table by the index.)
 *
 * The voice tables have no stock plugin to check against; their rules are
 * G.988's alone (9.9.6, 9.9.3, 9.9.10):
 *
 *   TBL_INDEX    58/10 to /13, the tone pattern and tone event tables and
 *                their ringing twins: byte 0 is the index (or the event) and
 *                the key; a row with 0 there clears the table, any other adds
 *                or replaces the row with that key. G.988 gives no way to
 *                remove one row.
 *   TBL_INDEX16  150/12, the SIP response table: the same, keyed by the
 *                two-byte response code.
 *   TBL_DIALPLAN 145/6, the dial plan table: byte 0 the dial plan id (the
 *                key), byte 1 the action, 1 adds or replaces, 0 removes.
 *
 * The control bits of a stored row read back as 00, as the stock stack
 * stores them.
 */
#define TBL_EVTOCD    1
#define TBL_CTRL      2
#define TBL_CTRL_PART 3
#define TBL_MCADDR    4
#define TBL_MACFILT   5
#define TBL_INDEX     6
#define TBL_INDEX16   7
#define TBL_DIALPLAN  8

/* The extended multicast operations profile, 65531 once the model has its
 * true class id. */
#define OMCI_ME_EXT_MCAST_OPER_PROFILE 65531

static const struct { uint16_t cls; uint8_t attr, kind; } tbl_defs[] = {
	{ OMCI_ME_EXT_VLAN_TAGGING_OP_CFG_DATA, 6, TBL_EVTOCD },
	{ 309, 7, TBL_CTRL_PART },               /* dynamic access control list */
	{ 309, 8, TBL_CTRL_PART },               /* static access control list */
	{ OMCI_ME_EXT_MCAST_OPER_PROFILE, 7, TBL_CTRL_PART },
	{ OMCI_ME_EXT_MCAST_OPER_PROFILE, 8, TBL_CTRL_PART },
	{ 310, 6, TBL_CTRL },                    /* multicast service package */
	{ 310, 7, TBL_CTRL_PART },               /* allowed preview groups */
	{ OMCI_ME_MCAST_GEM_IW_TP, 9, TBL_MCADDR },  /* IPv4 address table */
	{ OMCI_ME_MCAST_GEM_IW_TP, 10, TBL_MCADDR }, /* IPv6 address table */
	{ 49, 1, TBL_MACFILT },                  /* MAC filter table */
	{ OMCI_ME_VOICE_SERVICE_PROFILE, 10, TBL_INDEX },  /* tone pattern */
	{ OMCI_ME_VOICE_SERVICE_PROFILE, 11, TBL_INDEX },  /* tone event */
	{ OMCI_ME_VOICE_SERVICE_PROFILE, 12, TBL_INDEX },  /* ringing pattern */
	{ OMCI_ME_VOICE_SERVICE_PROFILE, 13, TBL_INDEX },  /* ringing event */
	{ OMCI_ME_SIP_AGENT_CFG_DATA, 12, TBL_INDEX16 },   /* SIP response table */
	{ OMCI_ME_NETWORK_DIAL_PLAN_TABLE, 6, TBL_DIALPLAN }, /* dial plan table */
};

static unsigned tbl_kind(const struct omci_class *c, unsigned k)
{
	if (!c)
		return 0;
	for (unsigned i = 0; i < sizeof tbl_defs / sizeof tbl_defs[0]; i++)
		if (tbl_defs[i].cls == c->classId && tbl_defs[i].attr == k)
			return attr_width(c, k) <= MIB_TBL_ENTRY ? tbl_defs[i].kind : 0;
	return 0;
}

int tbl_is_rows(const struct omci_class *c, unsigned k)
{
	return tbl_kind(c, k) != 0;
}

unsigned tbl_rows(const struct mib_row *r, unsigned k)
{
	unsigned n = 0;

	if (!r)
		return 0;
	for (uint8_t i = r->tbl_head; i != MIB_TBL_NONE; i = tblpool[i].next)
		n += tblpool[i].attr == k;
	return n;
}

/* Whether a Get of this attribute must go through Get-Next instead of being
 * answered inline. This is the vendor's own test, read out of OMCI_OnGetMsg in
 * bin/omci_app at 0x413abc:
 *
 *     dt = MIB_GetAttrDataType(tblIdx, k)
 *     if (dt == 5) goto big;                        // OCTETS
 *     if (MIB_GetAttrLen(tblIdx, k) < 26) continue;  // sltiu v0,v0,26
 *     goto big;
 *
 * 26 rather than 29 because a baseline Get response body is one result byte,
 * two mask bytes and 25 attribute bytes.
 *
 * Deliberately NOT the same question as tbl_is_rows(). That one asks which
 * attribute accumulates rows; this one asks which attribute is too big to
 * send inline, which is every OCTETS attribute in the model. Class 84's
 * 24-byte VLAN filter list is big by this test and is not a table by that
 * one. */
int attr_is_big(const struct omci_class *c, unsigned k)
{
	if (!c || k >= c->nattr)
		return 0;
	return c->attrs[k].type == OMCI_OCTETS || attr_width(c, k) >= 26;
}

/* The whole of a big attribute, laid out as the OLT will read it back.
 *
 * For a table that is every row of it, head to tail -- newest first, which is
 * also the order the vendor's class 171 dump prints. For any other big
 * attribute the row holds the value flat and there is exactly one "entry".
 *
 * Returns the byte length, which is what the Get response reports and what the
 * chunk count is computed from. */
uint16_t tbl_serialise(const struct omci_class *c, uint16_t inst, unsigned k,
		       uint8_t *out, uint16_t max)
{
	struct mib_row *r = mib_find(c->classId, inst);
	uint16_t w = attr_width(c, k);
	uint16_t n = 0;

	if (!r || !w)
		return 0;
	/* The bridge table of a port is the switch's, not the store's. */
	if (c->classId == 50 && k == 1)
		return bridge_table_read(inst, out, (uint16_t)(max - max % w));
	/* The active group list is the snooper's, and so is the profile's
	 * list of lost groups. */
	if (c->classId == CLS_MCAST_MONITOR && k == 5)
		return mcast_active_groups(out, (uint16_t)(max - max % w));
	if (c->classId == CLS_MCAST_OPER_PROFILE && k == 9)
		return mcast_lost_groups(inst, out, (uint16_t)(max - max % w));
	if (tbl_kind(c, k)) {
		for (uint8_t i = r->tbl_head; i != MIB_TBL_NONE;
		     i = tblpool[i].next) {
			if (tblpool[i].attr != k)
				continue;
			if (n + w > max)
				break;
			for (uint16_t b = 0; b < w; b++)
				out[n + b] = tblpool[i].data[b];
			n = (uint16_t)(n + w);
		}
		return n;
	}
	/* Not written is not zero: an attribute the OLT never set has no table
	 * to read back, and answering with a zero-filled entry would have the
	 * OLT walk chunks of nothing. */
	if (!(r->written & (1u << (16 - k))))
		return 0;
	if (w > max)
		return 0;
	n = attr_value(c, inst, k, out);
	return n;
}

void tbl_free(struct mib_row *r)
{
	uint8_t n = r->tbl_head;

	while (n != MIB_TBL_NONE) {
		uint8_t nx = tblpool[n].next;

		tblpool[n].used = 0;
		n = nx;
	}
	r->tbl_head = MIB_TBL_NONE;
	r->tbl_count = 0;
}

static int tbl_bytes_eq(const uint8_t *a, const uint8_t *b, int n)
{
	for (int i = 0; i < n; i++)
		if (a[i] != b[i])
			return 0;
	return 1;
}

/* Unlink every row of attribute k that `match` selects (all of them when it
 * is 0), returning their nodes to the pool. */
static void tbl_remove(struct mib_row *r, unsigned k,
		       int (*match)(const uint8_t *row, const uint8_t *e, uint16_t w),
		       const uint8_t *e, uint16_t w)
{
	uint8_t *link = &r->tbl_head;

	while (*link != MIB_TBL_NONE) {
		struct mib_tblent *t = &tblpool[*link];

		if (t->attr == k && (!match || match(t->data, e, w))) {
			*link = t->next;
			t->used = 0;
			r->tbl_count--;
			continue;
		}
		link = &t->next;
	}
}

/* Rows that share a key, per rule. */
static int key_evtocd(const uint8_t *row, const uint8_t *e, uint16_t w)
{
	(void)w;
	return tbl_bytes_eq(row, e, 8);
}

static int key_ctrl(const uint8_t *row, const uint8_t *e, uint16_t w)
{
	(void)w;
	return ((row[0] << 8 | row[1]) & 0x3ff) == ((e[0] << 8 | e[1]) & 0x3ff);
}

static int key_ctrl_part(const uint8_t *row, const uint8_t *e, uint16_t w)
{
	(void)w;
	return ((row[0] << 8 | row[1]) & 0x3bff) == ((e[0] << 8 | e[1]) & 0x3bff);
}

static int key_mcaddr(const uint8_t *row, const uint8_t *e, uint16_t w)
{
	(void)w;
	return tbl_bytes_eq(row, e, 4);
}

static int key_index(const uint8_t *row, const uint8_t *e, uint16_t w)
{
	(void)w;
	return row[0] == e[0];
}

static int key_index16(const uint8_t *row, const uint8_t *e, uint16_t w)
{
	(void)w;
	return row[0] == e[0] && row[1] == e[1];
}

/* The OMCC version this ONU reports (ONU2-G attribute 2). */
static uint8_t omcc_version(void)
{
	const struct omci_class *c = find_class(OMCI_ME_ONU2_G);
	uint8_t v[4] = { 0 };

	if (!c || attr_width(c, 2) != 1)
		return 0;
	attr_value(c, 0, 2, v);
	return v[0];
}

/* Add `e` as a new head row of attribute k, after removing the row with the
 * same `key` (none to remove when `key` is 0). */
static void tbl_put(struct mib_row *r, unsigned k, const uint8_t *e, uint16_t w,
		    int (*key)(const uint8_t *, const uint8_t *, uint16_t))
{
	if (key)
		tbl_remove(r, k, key, e, w);
	for (uint8_t n = 0; n < MIB_TBL_NODES; n++) {
		if (tblpool[n].used)
			continue;
		tblpool[n].used = 1;
		tblpool[n].attr = (uint8_t)k;
		for (int i = 0; i < MIB_TBL_ENTRY; i++)
			tblpool[n].data[i] = i < w ? e[i] : 0;
		tblpool[n].next = r->tbl_head;
		r->tbl_head = n;
		r->tbl_count++;
		return;
	}
	r->truncated = 1;                    /* pool exhausted; say so */
}

/* One OLT Set of table attribute k: the row in `src`, applied by the rule of
 * tbl_defs[]. */
static void tbl_set(struct mib_row *r, const struct omci_class *c, unsigned k,
		    const uint8_t *src)
{
	uint16_t w = attr_width(c, k);
	uint8_t e[MIB_TBL_ENTRY];
	unsigned kind = tbl_kind(c, k);
	int zero = 1;

	for (uint16_t i = 0; i < MIB_TBL_ENTRY; i++)
		e[i] = i < w ? src[i] : 0;
	switch (kind) {
	case TBL_EVTOCD: {
		int del = 1;

		for (int i = 8; i < 16; i++)
			if (e[i] != 0xff)
				del = 0;
		if (del) {
			tbl_remove(r, k, key_evtocd, e, w);
			return;
		}
		/* Replaced in place, not moved to the head: the vendor
		 * reassigns words 1..3 of the node it found. */
		for (uint8_t n = r->tbl_head; n != MIB_TBL_NONE; n = tblpool[n].next)
			if (tblpool[n].attr == k && key_evtocd(tblpool[n].data, e, w)) {
				for (int i = 8; i < 16; i++)
					tblpool[n].data[i] = e[i];
				return;
			}
		tbl_put(r, k, e, w, 0);
		return;
	}
	case TBL_CTRL:
	case TBL_CTRL_PART: {
		unsigned set = e[0] >> 6;

		e[0] &= 0x3f;                    /* stored rows read back 00 */
		if (kind == TBL_CTRL || omcc_version() < 0xa1)
			e[0] &= (uint8_t)~0x38;  /* no row part */
		if (set == 1)
			tbl_put(r, k, e, w, kind == TBL_CTRL ? key_ctrl : key_ctrl_part);
		else if (set == 2)
			tbl_remove(r, k, key_ctrl, e, w);
		else if (set == 3)
			tbl_remove(r, k, 0, e, w);
		return;
	}
	case TBL_MCADDR:
		for (uint16_t i = 4; i < w; i++)
			if (e[i])
				zero = 0;
		if (zero)
			tbl_remove(r, k, key_mcaddr, e, w);
		else
			tbl_put(r, k, e, w, key_mcaddr);
		return;
	case TBL_MACFILT:
		switch (e[1] >> 6) {
		case 2:
			tbl_put(r, k, e, w, key_index);
			return;
		case 0:
			tbl_remove(r, k, key_index, e, w);
			return;
		case 1:
			tbl_remove(r, k, 0, e, w);
			return;
		default:
			return;
		}
	case TBL_INDEX:
		if (e[0])
			tbl_put(r, k, e, w, key_index);
		else
			tbl_remove(r, k, 0, e, w);
		return;
	case TBL_INDEX16:
		if (e[0] || e[1])
			tbl_put(r, k, e, w, key_index16);
		else
			tbl_remove(r, k, 0, e, w);
		return;
	case TBL_DIALPLAN:
		if (e[1] == 1)
			tbl_put(r, k, e, w, key_index);
		else if (e[1] == 0)
			tbl_remove(r, k, key_index, e, w);
		return;
	default:
		return;
	}
}

struct mib_row *mib_find(uint16_t cls, uint16_t inst)
{
	for (int i = 0; i < MIB_ROWS; i++)
		if (mib[i].used && mib[i].classId == cls && mib[i].inst == inst)
			return &mib[i];
	return 0;
}

struct mib_row *mib_add(uint16_t cls, uint16_t inst)
{
	struct mib_row *r = mib_find(cls, inst);

	if (r)
		return r;
	for (int i = 0; i < MIB_ROWS; i++)
		if (!mib[i].used) {
			r = &mib[i];
			r->classId = cls;
			r->inst = inst;
			r->used = 1;
			r->truncated = 0;
			r->written = 0;
			r->tbl_head = MIB_TBL_NONE;
			r->tbl_count = 0;
			r->tail = 0;
			for (int j = 0; j < MIB_ROW_MAX; j++)
				r->data[j] = r->prev[j] = 0;
			if (class_wide(find_class(cls)))
				r->tail = wide_alloc();
			return r;
		}
	return 0;
}

void mib_del(uint16_t cls, uint16_t inst)
{
	struct mib_row *r = mib_find(cls, inst);

	if (r) {
		tbl_free(r);
		r->used = 0;
	}
}

/* The i-th slot of the store, for callers that walk a class: 0 for a slot
 * that holds no row. A free slot keeps the class and data of the row it
 * last held, so handing it out made a deleted entity -- or, after a MIB
 * reset, the whole previous MIB -- part of every rebuild again. */
struct mib_row *mib_row_at(int i)
{
	return (i >= 0 && i < MIB_ROWS && mib[i].used) ? &mib[i] : 0;
}

/* Every managed entity this ONU holds, for the CLI: the ones it creates for
 * itself (omci_autonomous[], which a MIB upload reports) and the ones the OLT
 * created, one entry each, sorted by class and then instance.
 *
 * The store alone is not the MIB. An autonomous entity has a row only once
 * the OLT sets one of its attributes, and most never are -- SWImage, ONT2-G,
 * the T-CONTs of an OLT that assigns Alloc-IDs by PLOAM -- so a dump that
 * walked the rows answered "0 rows" for exactly the entities a user looks
 * up first. `cls` 0 is every class; `want_inst` limits it to one instance.
 * Returns the count; the entries stay valid until the next call. */
static struct mib_ent ents[MIB_ENTS_MAX];

static void ent_add(int *n, uint16_t cls, uint16_t inst)
{
	int i;

	for (i = 0; i < *n; i++)
		if (ents[i].cls == cls && ents[i].inst == inst)
			return;
	if (*n >= MIB_ENTS_MAX)
		return;
	/* Insertion sort: a few hundred entries, once per CLI request. */
	for (i = *n; i > 0 && (ents[i - 1].cls > cls ||
			       (ents[i - 1].cls == cls && ents[i - 1].inst > inst)); i--)
		ents[i] = ents[i - 1];
	ents[i].cls = cls;
	ents[i].inst = inst;
	(*n)++;
}

int mib_entities(uint16_t cls, int want_inst, uint16_t inst,
		 const struct mib_ent **out)
{
	int n = 0;

	for (unsigned i = 0; i < aut_count; i++) {
		const struct omci_instance *e = &aut[i];

		if ((cls && e->classId != cls) || (want_inst && e->inst != inst))
			continue;
		ent_add(&n, e->classId, e->inst);
	}
	for (int i = 0; i < MIB_ROWS; i++) {
		if (!mib[i].used || (cls && mib[i].classId != cls) ||
		    (want_inst && mib[i].inst != inst))
			continue;
		ent_add(&n, mib[i].classId, mib[i].inst);
	}
	*out = ents;
	return n;
}

/* One entity as the OLT would read it: every attribute through attr_value(),
 * so what the OLT wrote wins and everything else is the built-in answer (the
 * defaults, the identity, ONT data's MIB data sync) -- the same bytes a Get
 * returns, where the row itself holds zeros for every attribute nobody
 * wrote. The table attribute, `written` and `truncated` come from the row
 * when there is one. The result is one static row, valid until the next
 * call; the renderers of show.c take it exactly as they take a stored row. */
const struct mib_row *mib_view(const struct omci_class *c, uint16_t inst)
{
	static struct mib_row v;
	static uint8_t view_tail[MIB_WIDE_MAX - MIB_ROW_MAX];
	const struct mib_row *r = mib_find(c->classId, inst);
	uint8_t tmp[MIB_WIDE_MAX];

	for (unsigned i = 0; i < sizeof v; i++)
		((uint8_t *)&v)[i] = 0;
	v.classId = c->classId;
	v.inst = inst;
	v.used = 1;
	v.tbl_head = r ? r->tbl_head : MIB_TBL_NONE;
	v.tbl_count = r ? r->tbl_count : 0;
	v.truncated = r ? r->truncated : 0;
	v.written = r ? r->written : 0;
	/* The view of a wide class is whole even when the stored row is not
	 * (pool full): what the OLT can read is what the dump shows. */
	if (class_wide(c)) {
		for (unsigned i = 0; i < sizeof view_tail; i++)
			view_tail[i] = 0;
		v.tail = view_tail;
	}
	for (unsigned k = 1; k < c->nattr && k <= 16; k++) {
		uint16_t w = attr_width(c, k);
		int off = attr_offset(c, k);

		if (!w || off < 0 || off + w > row_cap(&v) || w > sizeof tmp)
			continue;
		attr_value(c, inst, k, tmp);
		for (uint16_t b = 0; b < w; b++)
			row_put(&v, (unsigned)off + b, tmp[b]);
	}
	return &v;
}

int mib_count(void)
{
	int n = 0;

	for (int i = 0; i < MIB_ROWS; i++)
		n += mib[i].used;
	return n;
}

/* Write the attributes named by `mask` from `src` into the row, except the
 * ones also in `skip`: those still take their bytes from the frame (the
 * values of the attributes after them sit behind), but are not stored.
 * Returns the number of bytes consumed, so a caller can tell a short frame
 * from a full one. */
uint16_t mib_write(struct mib_row *r, const struct omci_class *c,
			  uint16_t mask, uint16_t skip, const uint8_t *src,
			  uint16_t srclen)
{
	uint16_t used = 0;

	/* Snapshot first: the handlers that run after this need the row as it
	 * was, and taking the copy per attribute would miss the ones this Set
	 * does not touch. */
	for (unsigned j = 0; j < MIB_ROW_MAX; j++)
		r->prev[j] = r->data[j];

	for (unsigned k = 1; k <= 16; k++) {
		uint16_t w;
		int off;

		if (!(mask & (1u << (16 - k))))
			continue;
		w = attr_width(c, k);
		if (!w || used + w > srclen)
			break;
		if (skip & (1u << (16 - k))) {
			used = (uint16_t)(used + w);
			continue;
		}
		off = attr_offset(c, k);
		if (off < 0 || off + w > row_cap(r)) {
			r->truncated = 1;
		} else {
			for (uint16_t i = 0; i < w; i++)
				row_put(r, (unsigned)off + i, src[used + i]);
			r->written |= (uint16_t)(1u << (16 - k));
		}
		/* A table attribute also accumulates. The flat copy above is
		 * still made, as the vendor's generic store keeps it beside
		 * the plugin's list; a Get answers the list. */
		if (tbl_kind(c, k))
			tbl_set(r, c, k, src + used);
		used = (uint16_t)(used + w);
	}
	return used;
}

int mib_changed(const struct mib_row *r, const struct omci_class *c, unsigned k)
{
	uint16_t w = attr_width(c, k);
	int off = attr_offset(c, k);

	if (!r || !w || off < 0 || off + w > MIB_ROW_MAX)
		return 0;
	for (uint16_t i = 0; i < w; i++)
		if (r->data[off + i] != r->prev[off + i])
			return 1;
	return 0;
}

/* ONU2-G SysUpTime (attribute 10): the time since boot in 10 ms units, as
 * G.988 defines it, from the monotonic clock. The 32-bit counter wraps after
 * about 497 days, which is the rollover the entity describes. */
uint32_t uptime_cs(void)
{
	long s = 0, ns = 0;

	if (sys_clock_gettime(CLOCK_MONOTONIC, &s, &ns) != 0)
		return 0;
	return (uint32_t)s * 100u + (uint32_t)(ns / 10000000L);
}

/* Where the UNI link state comes from: the read-only odi_switch parameter
 * that holds the link bit of the SFP edge port (-l file replaces it in a
 * test). */
const char *uni_link_path = "/sys/module/odi_switch/parameters/host_link";

/* 1 link up, 0 link down, -1 not readable. */
int uni_link_state(void)
{
	char b[8];
	long fd = sys_open(uni_link_path, O_RDONLY), n;

	if (fd < 0)
		return -1;
	n = sys_read((int)fd, b, sizeof b);
	sys_close((int)fd);
	if (n < 1 || (b[0] != '0' && b[0] != '1'))
		return -1;
	return b[0] == '1';
}

/* The stick's own MAC address, for the bridge MAC of class 46: eth0 carries
 * it (ELAN_MAC_ADDR sets eth0, eth0.2 and br0 to the same one). Zeros when
 * the file cannot be read or does not hold "aa:bb:cc:dd:ee:ff". */
const char *bridge_mac_path = "/sys/class/net/eth0/address";

static int hexdig(char ch)
{
	if (ch >= '0' && ch <= '9')
		return ch - '0';
	if (ch >= 'a' && ch <= 'f')
		return ch - 'a' + 10;
	if (ch >= 'A' && ch <= 'F')
		return ch - 'A' + 10;
	return -1;
}

static void bridge_mac(uint8_t mac[6])
{
	char b[24];
	long fd = sys_open(bridge_mac_path, O_RDONLY), n;

	for (int i = 0; i < 6; i++)
		mac[i] = 0;
	if (fd < 0)
		return;
	n = sys_read((int)fd, b, sizeof b);
	sys_close((int)fd);
	if (n < 17)
		return;
	for (int i = 0; i < 6; i++) {
		int hi = hexdig(b[i * 3]), lo = hexdig(b[i * 3 + 1]);

		if (hi < 0 || lo < 0 || (i < 5 && b[i * 3 + 2] != ':')) {
			for (int j = 0; j < 6; j++)
				mac[j] = 0;
			return;
		}
		mac[i] = (uint8_t)(hi << 4 | lo);
	}
}

/* The PON state O1..O7 as a number: the driver status file first (the same
 * one the Alloc-IDs come from), then the driver command, and O1 when neither
 * answers -- the state the ONU starts in. */
static uint8_t pon_state_now(void)
{
	uint32_t st = 0;
	int n = onu_state_now();

	if (n >= 1 && n <= 7)
		return (uint8_t)n;
	if (omci_getOnuState(&st) == 0 && st >= 1 && st <= 7)
		return (uint8_t)st;
	return 1;
}

/* G.988's values at instantiation for the attributes of an entity that no
 * Create carries, where they are not zero: the pointers that are null until
 * set, and the timers G.988 gives a default. An OLT that never Sets them
 * reads these, with or without a row. Class 290 (Dot1X port extension
 * package, G.988 9.3.14) is not a voice entity but is served the same way:
 * its port status starts at 1, authorized (G.988 allows 1 or 2, and the
 * stick never blocks the port), and its server timeout at 30 s. */
static const struct { uint16_t cls; uint8_t attr; uint16_t v; } voice_dfl[] = {
	{ OMCI_ME_SIP_AGENT_CFG_DATA, 5, 0xffff },      /* TCP/UDP pointer */
	{ OMCI_ME_SIP_AGENT_CFG_DATA, 6, 3600 },        /* registration expiry, s */
	{ OMCI_ME_SIP_AGENT_CFG_DATA, 7, 360 },         /* re-registration head start, s */
	{ OMCI_ME_SIP_USER_DATA, 11, 10 },              /* release timer, s */
	{ OMCI_ME_SIP_USER_DATA, 12, 15 },              /* receiver off hook timer, s */
	{ OMCI_ME_RTP_PROFILE_DATA, 8, 0xffff },        /* IP host config pointer */
	{ OMCI_ME_VOIP_APP_SERVICE_PROFILE, 10, 0xffff }, /* IP host pointer */
	{ OMCI_ME_VOIP_CONFIG_DATA, 5, 0xffff },        /* configuration address pointer */
	{ OMCI_ME_DOT1X_PORT_EXT, 7, 1 },                                  /* Dot1X controlled port status: authorized */
	{ OMCI_ME_DOT1X_PORT_EXT, 9, 30 },                                 /* Dot1X server timeout, s */
};

/* The value in the attribute's own width, big-endian like every attribute. */
static void voice_default(uint16_t cls, unsigned k, uint8_t *out, uint16_t n)
{
	for (unsigned i = 0; i < sizeof voice_dfl / sizeof voice_dfl[0]; i++)
		if (voice_dfl[i].cls == cls && voice_dfl[i].attr == k) {
			for (uint16_t b = 0; b < n; b++)
				out[n - 1 - b] = b < 2 ? (uint8_t)(voice_dfl[i].v >> (8 * b)) : 0;
			return;
		}
}

/* Whether the OLT has provisioned a SIP line on this PPTP POTS UNI: a voice
 * CTP that points at it and at a SIP user whose SIP agent the ONU holds. */
static int voip_line_configured(uint16_t pptp)
{
	const struct omci_class *ctp = find_class(OMCI_ME_VOIP_VOICE_CTP);
	const struct omci_class *usr = find_class(OMCI_ME_SIP_USER_DATA);

	for (int i = 0; ctp && usr && i < MIB_ROWS; i++) {
		const struct mib_row *u;

		if (!mib[i].used || mib[i].classId != OMCI_ME_VOIP_VOICE_CTP ||
		    row_u32(&mib[i], ctp, 2) != pptp)
			continue;
		u = mib_find(OMCI_ME_SIP_USER_DATA, (uint16_t)row_u32(&mib[i], ctp, 1));
		if (u && mib_find(OMCI_ME_SIP_AGENT_CFG_DATA, (uint16_t)row_u32(u, usr, 1)))
			return 1;
	}
	return 0;
}

/* OMCI_VOIP_LINE_REPORT=1 (odi.conf): the router behind the stick says what
 * its SIP client really does, and the line status of a provisioned line
 * follows it. The report is the file VOIP_REPORT_PATH, written by the web UI
 * daemon (confd) when the router posts it: a line LINE<port>=<state> <s>,
 * <s> being the seconds since boot (the /proc/uptime clock) when it was
 * received. Only a fact that is fresh counts: a report older than
 * VOIP_REPORT_TTL_S, from the future, unreadable or not one of the words
 * below is not a report, and the line reads "config done" as it always did.
 * The words are G.988 9.9.11 voice server status values 0 to 12: none,
 * registered, in session, the registration failures (3 to 7) and the invite
 * failures (8 to 12). Read at every Get, so a change is live. Returns the status, or
 * -1 for no report. */
#define VOIP_REPORT_PATH   "/var/run/voip-line-status"
#define VOIP_REPORT_TTL_S  300
#define CLOCK_BOOTTIME_ID  7

static const struct { const char *word; uint8_t status; } voip_report_words[] = {
	{ "none",           0 },
	{ "registered",     1 },
	{ "in_session",     2 },
	{ "failed_icmp",    3 },
	{ "failed_tcp",     4 },
	{ "failed_auth",    5 },
	{ "failed_timeout", 6 },
	{ "failed_server",  7 },
	{ "failed_invite_icmp",    8 },
	{ "failed_invite_tcp",     9 },
	{ "failed_invite_auth",   10 },
	{ "failed_invite_timeout", 11 },
	{ "failed_invite_server", 12 },
};

static int voip_line_reported(uint16_t pptp)
{
	char key[8] = "LINE", v[40];
	long now = 0, ns = 0, stamp = 0;
	int len, i = 0;

	if (cfg_odi_get(CFG_ODI_PATH, "OMCI_VOIP_LINE_REPORT", v, sizeof v) != 1 ||
	    v[0] != '1' || (pptp & 0xff) < 1 || (pptp & 0xff) > POTS_PORTS_MAX)
		return -1;
	key[4] = (char)('0' + (pptp & 0xff));
	len = cfg_odi_get(VOIP_REPORT_PATH, key, v, sizeof v);
	if (len < 3 || len >= (int)sizeof v ||
	    sys_clock_gettime(CLOCK_BOOTTIME_ID, &now, &ns) != 0)
		return -1;
	while (v[i] && v[i] != ' ')
		i++;
	if (!v[i])
		return -1;
	v[i++] = 0;
	if (!v[i])
		return -1;
	for (; v[i]; i++) {
		if (v[i] < '0' || v[i] > '9' || stamp > 100000000L)
			return -1;
		stamp = stamp * 10 + (v[i] - '0');
	}
	if (stamp > now || now - stamp > VOIP_REPORT_TTL_S)
		return -1;
	for (unsigned w = 0; w < sizeof voip_report_words / sizeof voip_report_words[0]; w++)
		if (str_eq(v, voip_report_words[w].word))
			return voip_report_words[w].status;
	return -1;
}

/* The voice attributes the ONU answers itself, then G.988's defaults.
 *
 * What the stick holds and can observe, never more: it has no phone socket,
 * places no call and registers nothing; the router behind it does, out of
 * sight. So the POTS UNI is enabled while the OLT leaves it unlocked (the
 * line is provisioned, not working), its hook is on (no handset), the line
 * status says "port not configured" until a SIP line names the port and
 * "config done" once one does -- never "registered" or "in session" -- and
 * the rest of it (codec, session, packet periods, called addresses, line
 * state, emergency call) stays idle and empty. The VoIP config data offers
 * SIP over OMCI only: the other methods fetch a file or talk to a server
 * toward the fibre, which nothing on the stick may do; an OLT that picks one
 * anyway reads the configuration state as failed. */
#define CARD_TYPE_POTS       32
#define LINE_NOT_CONFIGURED  13
#define LINE_CONFIG_DONE     14
#define VCD_PROTOCOL_SIP     0x01          /* available protocols, bit 1 */
#define VCD_METHOD_OMCI      0x01          /* available methods, bit 1 */
#define VCD_METHOD_FIRST_NON_OMCI 2        /* method used: 2 file, 3 TR-069, ... */
#define VCD_STATE_FAULT      3

static void voice_value(const struct omci_class *c, const struct mib_row *r,
			uint16_t inst, unsigned k, uint8_t *out, uint16_t n)
{
	uint8_t v[1];

	switch (c->classId) {
	case OMCI_ME_NETWORK_DIAL_PLAN_TABLE:
		if (k == 1 && n >= 2) {            /* dial plan number: rows held */
			unsigned rows = tbl_rows(r, 6);

			out[0] = (uint8_t)(rows >> 8);
			out[1] = (uint8_t)rows;
		}
		break;
	case OMCI_ME_PPTP_POTS_UNI:
		if (k == 9) {                      /* operational state */
			attr_value(c, inst, 1, v);
			out[0] = v[0] ? OPER_DISABLED : 0;
		}
		break;
	case OMCI_ME_VOIP_LINE_STATUS:
		if (k == 2) {                      /* voice server status */
			int rep = voip_line_configured(inst) ? voip_line_reported(inst) : -1;

			out[0] = rep >= 0 ? (uint8_t)rep :
				 voip_line_configured(inst) ? LINE_CONFIG_DONE
							    : LINE_NOT_CONFIGURED;
		}
		break;
	case OMCI_ME_VOIP_CONFIG_DATA:
		if (k == 1)
			out[0] = VCD_PROTOCOL_SIP;
		else if (k == 3 && n >= 4)
			out[3] = VCD_METHOD_OMCI;
		else if (k == 6) {                 /* configuration state */
			attr_value(c, inst, 4, v);
			out[0] = v[0] >= VCD_METHOD_FIRST_NON_OMCI ? VCD_STATE_FAULT : 0;
		}
		break;
	default:
		break;
	}
	voice_default(c->classId, k, out, n);
}

/* The Weight mib_PriQ.so's own default row carries. See the longer note beside
 * ds_queue_words: isp1's live weights are not all 1, and which queues get a
 * non-default one is provisioning rather than derivation. */
#define PRIQ_DEFAULT_WEIGHT 1

uint16_t attr_value(const struct omci_class *c, uint16_t inst,
			   unsigned k, uint8_t *out)
{
	uint16_t n = attr_width(c, k);
	struct mib_row *r = mib_find(c->classId, inst);
	int off = attr_offset(c, k);

	for (uint16_t i = 0; i < n; i++)
		out[i] = 0;
	/* A PM entity's counts are never the OLT's to write: they are the
	 * intervals of pm.c. */
	if (pm_class(c->classId) && pm_attr(c->classId, inst, k, out, n))
		return n;
	/* An attribute the OLT has written wins: a get after a set has to return
	 * what was set. `written`, not merely `r`, because a row exists as soon
	 * as any one attribute is set and the rest of it is still zero -- the
	 * built-in answers below have to keep serving the attributes nobody
	 * wrote. */
	if (r && (r->written & (1u << (16 - k))) && off >= 0
	    && off + n <= row_cap(r)) {
		for (uint16_t i = 0; i < n; i++)
			out[i] = row_get(r, (unsigned)off + i);
		return n;
	}
	/* What the vendor stack answers for the same instance on this device
	 * (generated/omci_defaults.c, from its `omcicli mib get all`): the
	 * capability picture -- ONT2-G with 128 queues, 64 GEM ports,
	 * connectivity 0x7f; ANI-G with 16 T-CONTs -- that an OLT builds its
	 * service model from. Zeros here read as an ONU with nothing to
	 * provision. The identity cases below still
	 * override. */
	for (unsigned d = 0; d < omci_defaults_count; d++) {
		const struct omci_default *df = &omci_defaults[d];
		if (df->cls == c->classId && df->inst == canon_inst(c->classId, inst) &&
		    df->attr == k) {
			for (uint16_t i = 0; i < n && i < df->width; i++)
				out[i] = df->data[i];
			break;
		}
	}
	if (c->classId == OMCI_ME_CIRCUIT_PACK) { /* circuit pack */
		/* The ONU identity and software, not a capture: the serial and
		 * its vendor id are the configured serial, the version is the
		 * active software image's. The operational state (7) is the
		 * administrative state (6) the OLT set, as the stock
		 * circuit_pack_check_cb copies it; the AVC follows (alarm.c). */
		serial_refresh(0);
		if (k == 7 && r && (r->written & (1u << (16 - 6))) &&
		    attr_offset(c, 6) >= 0)
			out[0] = r->data[attr_offset(c, 6)];
		if (k == 3) for (int i = 0; i < 8 && i < n; i++) out[i] = serial[i];
		else if (k == 4) put_str(out, n, report_sw_ver(sw_active_inst()));
		else if (k == 5) for (int i = 0; i < 4 && i < n; i++) out[i] = serial[i];
		/* The phone ports' pack: type POTS, the ports; no T-CONT
		 * buffer, queue or scheduler of its own (the zeros). */
		else if (pots_card(inst) && k == 1) out[0] = CARD_TYPE_POTS;
		else if (pots_card(inst) && k == 2) out[0] = (uint8_t)pots_ports;
	} else if (c->classId == OMCI_ME_CARDHOLDER && pots_card(inst)) {
		/* Its cardholder: a POTS card expected and present, with the
		 * ports. */
		if (k == 1 || k == 2) out[0] = CARD_TYPE_POTS;
		else if (k == 3) out[0] = (uint8_t)pots_ports;
	} else if (c->classId == OMCI_ME_ONU_G) { /* ONU-G */
		serial_refresh(0);
		if (k == 1) { out[0] = serial[0]; out[1] = serial[1];
			      out[2] = serial[2]; out[3] = serial[3]; }
		else if (k == 2) put_str(out, n, report_hw_ver());
		else if (k == 3) for (int i = 0; i < 8; i++) out[i] = serial[i];
		/* Attribute 14, the ONT PON state: the O-state number. */
		else if (k == 14) out[0] = pon_state_now();
		/* Logical ONU ID and password (10, 11): the LOID the stock MIB
		 * reset copies in from its -l/-w arguments, which are the
		 * config store's LOID keys -- the same values the CTC LOID
		 * entity (65530) reports. An OLT that authenticates by the
		 * G.988 ONU-G LOID read an empty one here. Credentials status
		 * (12) stays 0 until the OLT writes it, as on the stock ONU. */
		else if (k == 10) put_str(out, n, ident.loid);
		else if (k == 11) put_str(out, n, ident.loidPwd);
	} else if (c->classId == OMCI_ME_ONU2_G) { /* ONU2-G */
		/* Attribute 2 is the OMCC version, per the vendor's own table
		 * and G.988 -- not attribute 3, which is the vendor product
		 * code. The three identity keys override only when reported
		 * (cfgstore.c: report); otherwise the answer is what it always
		 * was, the device id, 0x80 and product code 15, the stock
		 * firmware's built-in default for that key (cfgstore.c). */
		if (!report.loaded)
			cfg_load_report();
		if (k == 1)
			put_str(out, n, report.on && report.modelLen > 0
				? report.model : (const char *)devid);
		else if (k == 2)
			out[0] = report.on && report.omccVer >= 0
				? (uint8_t)report.omccVer : OMCC_VER_BASELINE;
		else if (k == 3 && report.on && report.productCode >= 0 && n >= 2) {
			out[0] = (uint8_t)(report.productCode >> 8);
			out[1] = (uint8_t)report.productCode;
		} else if (k == 10 && n >= 4) {         /* SysUpTime, 10 ms */
			uint32_t t = uptime_cs();

			out[0] = (uint8_t)(t >> 24); out[1] = (uint8_t)(t >> 16);
			out[2] = (uint8_t)(t >> 8);  out[3] = (uint8_t)t;
		}
	} else if (c->classId == OMCI_ME_ANI_G && (k == 10 || k == 14) && n >= 2) {
		/* Optical signal level (10, Rx) and transmit optical level
		 * (14, Tx), live from the transceiver; zeros when it cannot
		 * be read. */
		int16_t v;

		if (ddm_dbm_x500(k == 10 ? 6 : 5, &v) == 0) {
			out[0] = (uint8_t)((uint16_t)v >> 8);
			out[1] = (uint8_t)v;
		}
	} else if (c->classId == OMCI_ME_VEIP) { /* virtual Ethernet interface point */
		/* CustomizedSlotId: the slot the key names, as the stock
		 * stack reports it, even when the slot itself is unchanged. */
		if (k == 6 && veip_slot_key >= 0) out[0] = (uint8_t)veip_slot_key;
		else if (k == 1) out[0] = veip_admin(inst);
		else if (k == 2) out[0] = veip_admin(inst) ? OPER_DISABLED : 0;
	} else if (c->classId == OMCI_ME_TR069_MGMT_SERVER) { /* TR-069 server */
		/* The default row of mib_TR069ManageServer.so, which the stock
		 * MIB reset leaves as it is: locked, and both pointers
		 * (ACS network address, associated tag) unset, 0xffff. Both
		 * stock lines report 0xffff until their OLT writes one. */
		if (k == 1) out[0] = ADMIN_LOCKED;
		else if ((k == 2 || k == 3) && n >= 2) out[0] = out[1] = 0xff;
	} else if (c->classId == OMCI_ME_PRIVATE_VLAN_CFG) { /* private VLAN config */
		/* What the stock firmware stores here is what /etc/runomci.sh
		 * passes omci_app (-iot_vt/-iot_vm/-iot_vid/-iot_pri), not the
		 * keys as stored: type 1 and mode 1 with a VID and a priority
		 * give all four; type 1 with another mode gives that mode and
		 * the unset sentinels 65535 / 255; any other type is reported
		 * as type 0, the mode, and the sentinels. */
		int tag;

		if (!vlanCfg.loaded)
			cfg_load_vlan();
		tag = vlanCfg.type == 1 && vlanCfg.mode == 1 && vlanCfg.vid >= 0;
		if (k == 1) out[0] = vlanCfg.type == 1 ? 1 : 0;
		else if (k == 2) out[0] = (uint8_t)vlanCfg.mode;
		else if (k == 3 && n >= 2) {
			uint16_t v = tag ? (uint16_t)vlanCfg.vid : 0xffff;

			out[0] = (uint8_t)(v >> 8); out[1] = (uint8_t)v;
		} else if (k == 4) out[0] = tag ? (uint8_t)vlanCfg.pri : 0xff;
	} else if (c->classId == CLS_MCAST_MONITOR) { /* multicast subscriber monitor */
		/* The snooper's counters (mcast.c): the bandwidth its group
		 * counters measure (2, zero while nothing is metered), the joins
		 * the subscriber sent (3), and the joins past the bandwidth of
		 * ME 310 (4). */
		if ((k == 2 || k == 3 || k == 4) && n >= 4) {
			uint32_t v = k == 2 ? mcast_meter_get("bps") :
				     k == 3 ? mcast_state_get("joins") :
				     mcast_state_get("bw_exceeded");

			out[0] = (uint8_t)(v >> 24); out[1] = (uint8_t)(v >> 16);
			out[2] = (uint8_t)(v >> 8);  out[3] = (uint8_t)v;
		}
	} else if (c->classId == OMCI_ME_PPTP_ETH_UNI) { /* PPTP Ethernet UNI */
		/* Sensed type is the slot card type, which the ONU never
		 * changes; the configuration indication follows the link:
		 * unknown (0) with no link, otherwise the speed and duplex of
		 * the port, which is 1000BASE-X full duplex here. */
		if (k == 2) out[0] = UNI_SENSED_TYPE;
		else if (k == 7) out[0] = uni_link_state() == 1 ? UNI_CFG_IND_1G_FULL : 0;
		/* The operational state follows the link too: the stock LAN-LOS
		 * handler (pptp_eth_uni_alarm_handler) writes 1 (disabled) when
		 * the link goes and 0 when it comes back, and the AVC of it
		 * follows (alarm.c). An unreadable link keeps the default. */
		else if (k == 6 && uni_link_state() >= 0)
			out[0] = uni_link_state() == 1 ? 0 : OPER_DISABLED;
	} else if (c->classId == OMCI_ME_SOFTWARE_IMAGE) { /* software image */
		if (k == 1) put_str(out, n, report_sw_ver(inst));
		/* is committed, is active, is valid: image 0 all three and
		 * image 1 valid, until an accepted download, activate or
		 * commit from the OLT moves them (swimage.c). Reported only:
		 * the slots themselves never change. */
		else if (k >= 2 && k <= 4) out[0] = sw_flag(inst, k);
	} else if (c->classId == OMCI_ME_CTC_LOID_AUTH) { /* CTC LOID authentication */
		/* As the stock MIB reset (FUN_0040dc44 of omci_app) fills it:
		 * OperationId "CTC " (with the space, four bytes), the LOID and
		 * its password from the config store (LOID and LOID_PASSWD,
		 * the LOID_OLD rule of cfgstore.c), AuthStatus 0 until the OLT
		 * sets it. */
		if (k == 1 && n >= 4) {
			out[0] = 'C'; out[1] = 'T'; out[2] = 'C'; out[3] = ' ';
		} else if (k == 2) put_str(out, n, ident.loid);
		else if (k == 3) put_str(out, n, ident.loidPwd);
	} else if (c->classId == OMCI_ME_MAC_BRIDGE_CFG_DATA) { /* MAC bridge configuration */
		/* G.988 9.3.2. omcid runs no spanning tree, so the ONU is the
		 * root of every bridge: the designated root is its own bridge
		 * id (the priority of the service profile, then the bridge MAC
		 * address), the path cost and the root port are 0, and the
		 * timers are the profile's. The bridge MAC is the stick's own;
		 * the port count is the class 47 ports that point at this
		 * bridge, counted now. */
		const struct omci_class *sp = find_class(OMCI_ME_MAC_BRIDGE_SERVICE_PROFILE);
		const struct omci_class *bp = find_class(OMCI_ME_MAC_BRIDGE_PORT_CFG_DATA);
		uint8_t mac[6], pri[2], t[2];
		unsigned ports = 0;

		bridge_mac(mac);
		if (sp)
			attr_value(sp, inst, 4, pri);       /* Priority */
		else
			pri[0] = pri[1] = 0;
		if (k == 1) {
			for (int i = 0; i < 6; i++)
				out[i] = mac[i];
		} else if (k == 2) {
			out[0] = pri[0]; out[1] = pri[1];
		} else if (k == 3) {
			out[0] = pri[0]; out[1] = pri[1];
			for (int i = 0; i < 6; i++)
				out[2 + i] = mac[i];
		} else if (k == 5 && bp) {
			for (int i = 0; i < MIB_ROWS; i++)
				ports += mib[i].used &&
					 mib[i].classId == OMCI_ME_MAC_BRIDGE_PORT_CFG_DATA &&
					 row_u32(&mib[i], bp, 1) == inst;
			out[0] = (uint8_t)ports;
		} else if ((k == 7 || k == 8) && sp) {
			attr_value(sp, inst, k - 1, t);     /* HelloTime, ForwardDelay */
			out[0] = t[0]; out[1] = t[1];
		}
	} else if (c->classId == OMCI_ME_ONT_DATA) { /* ONT data */
		if (k == 1) out[0] = mib_data_sync;
	} else if (c->classId == OMCI_ME_PRIORITY_QUEUE) { /* priority queue */
		/* All 208 of these are in omci_autonomous[], so a MIB upload
		 * has always reported them -- with every attribute zero, which
		 * tells an OLT that this ONU has 208 identical queues. The
		 * values are known: RelatedPort is derivable from the entity
		 * id (checked against all 208 of isp1's rows; FUN_0040c28c of
		 * omci_app computes it the same way) and the rest is
		 * mib_PriQ.so's own default row, plus the two queue sizes,
		 * 3276 (0x0ccc), which that same reset function sets on every
		 * queue. */
		uint32_t rp = priq_related_port(inst);

		if (k == 1) out[0] = 1;                /* QCfgOpt */
		else if (k == 2 || k == 3) {           /* Max/AllocQSize */
			out[0] = 3276 >> 8; out[1] = 3276 & 0xff;
		} else if (k == 6) {                   /* RelatedPort */
			out[0] = (uint8_t)(rp >> 24); out[1] = (uint8_t)(rp >> 16);
			out[2] = (uint8_t)(rp >> 8);  out[3] = (uint8_t)rp;
		} else if (k == 8) out[0] = PRIQ_DEFAULT_WEIGHT;
		else if (k == 14) { out[0] = 0; out[1] = 0xff; } /* PktDropMaxP */
		else if (k == 15) out[0] = 9;          /* QueueDropWQ */
	} else if (c->classId == OMCI_ME_TCONT && k == 1 && n >= 2) { /* T-CONT */
		/* AllocID, when the OLT has not set it: the PLOAM assignment
		 * bound to this T-CONT, or 0x00FF (apply_qos.c). */
		uint16_t a = tcont_alloc_id(inst, 0);

		out[0] = (uint8_t)(a >> 8);
		out[1] = (uint8_t)a;
	} else if (c->classId == OMCI_ME_GEM_PORT_CTP && k == 8 && r) {
		/* Encryption state (G.988 9.2.3, read only): whether the
		 * downstream path decrypts this port, from the driver, which
		 * applies the OLT's Encrypted_Port-ID. There is no AVC for it. */
		out[0] = (uint8_t)gem_port_encrypted((uint16_t)row_u32(r, c, 1));
	} else if (c->classId == OMCI_ME_TRAFFIC_SCHEDULER) { /* traffic scheduler */
		/* ISP1's sixteen are identical apart from their identity:
		 * TcontPtr is the entity id, SchedulerPtr 0, Policy 2,
		 * PriWeight 0. */
		if (k == 1) { out[0] = (uint8_t)(inst >> 8); out[1] = (uint8_t)inst; }
		else if (k == 3) out[0] = 2;           /* Policy */
	} else if (voip_class(c->classId)) {
		voice_value(c, r, inst, k, out, n);
	} else if (c->classId == OMCI_ME_DOT1X_PORT_EXT) {
		voice_default(c->classId, k, out, n);
	}
	return n;
}

/* ------------------------------------------------------------ MIB upload
 *
 * What the stock omci_app uploads, read off the binary (the record count in
 * FUN_0040ff70, the walk of the snapshot at 0x415e24 and OMCI_OnMibUploadMsg):
 *
 *  - every entity of the MIB, the ONU's own and the OLT's, but no entity of a
 *    class whose type is private or "not uploaded" (`(std & 0x110) == 0`) and
 *    no instance added with the no-upload flag (the `!` rows of
 *    /etc/omci_mib.cfg: priority queues 0xff00 to 0xff3f);
 *  - of each entity, the attributes whose option type has none of
 *    OMCI_OPT_NOT_UPLOADED (not supported, private, PM, table) and that are
 *    at most 26 bytes, in attribute order;
 *  - 26 bytes of them per record: an attribute that would overflow starts the
 *    next record, under its own mask. An entity with no such attribute is one
 *    record with an empty mask.
 *
 * The stock firmware's own upload on ISP2 (a decoded session of the stock
 * image) is exactly this: 205 entities in 399 records, ONU2-G in two, ONU-G
 * in four, each priority queue in two. omcid used to send one record per
 * entity and drop whatever did not fit in it -- the ONU2-G queue, scheduler
 * and GEM port counts among them. */
struct upl_rec { uint16_t cls, inst, mask; };
static struct upl_rec upl[UPL_REC_MAX];
static unsigned upl_n;
unsigned upl_total;

/* The reserved queues of /etc/omci_mib.cfg, which the stock firmware creates
 * with the no-upload flag. */
#define PRIQ_NO_UPLOAD_FIRST 0xff00

static int upl_skipped(const struct omci_class *c, uint16_t inst)
{
	return !c || (c->std & OMCI_STD_NOT_UPLOADED) ||
	       (c->classId == OMCI_ME_PRIORITY_QUEUE && inst >= PRIQ_NO_UPLOAD_FIRST);
}

static void upl_push(uint16_t cls, uint16_t inst, uint16_t mask)
{
	if (upl_n < UPL_REC_MAX) {
		upl[upl_n].cls = cls;
		upl[upl_n].inst = inst;
		upl[upl_n].mask = mask;
		upl_n++;
	}
}

static void upl_entity(const struct omci_class *c, uint16_t inst)
{
	uint16_t used = 0, mask = 0;
	int recs = 0;

	if (upl_skipped(c, inst))
		return;
	for (unsigned k = 1; k < c->nattr && k <= 16; k++) {
		uint16_t w = attr_width(c, k);

		if ((c->attrs[k].opt & OMCI_OPT_NOT_UPLOADED) || !w ||
		    w > UPL_REC_BYTES)
			continue;
		if (used + w > UPL_REC_BYTES) {
			upl_push(c->classId, inst, mask);
			recs++;
			used = 0;
			mask = 0;
		}
		used = (uint16_t)(used + w);
		mask |= (uint16_t)(1u << (16 - k));
	}
	if (mask || !recs)
		upl_push(c->classId, inst, mask);
}

static int is_autonomous(uint16_t cls, uint16_t inst)
{
	for (unsigned i = 0; i < aut_count; i++)
		if (aut[i].classId == cls && aut[i].inst == inst)
			return 1;
	return 0;
}

unsigned mib_upload_build(void)
{
	upl_n = 0;
	for (unsigned i = 0; i < aut_count; i++)
		upl_entity(find_class(aut[i].classId), aut[i].inst);
	/* The OLT's entities, and an autonomous one only once: a Set gives it
	 * a row, which is not a second entity. */
	for (int i = 0; i < MIB_ROWS; i++)
		if (mib[i].used && !is_autonomous(mib[i].classId, mib[i].inst))
			upl_entity(find_class(mib[i].classId), mib[i].inst);
	upl_total = upl_n + (unsigned)fixed_me_upload(-1, 0, 0);
	return upl_total;
}

int mib_upload_record(unsigned seq, uint8_t *body, uint16_t *len)
{
	const struct omci_class *c;
	uint16_t n = 6;

	if (seq >= upl_n) {
		int idx = (int)(seq - upl_n);

		if (idx < fixed_me_upload(-1, 0, 0)) {
			fixed_me_upload(idx, body, len);
			return 0;
		}
		return -1;
	}
	c = find_class(upl[seq].cls);
	nl_put16(body + 0, upl[seq].cls);
	nl_put16(body + 2, upl[seq].inst);
	nl_put16(body + 4, upl[seq].mask);
	for (unsigned k = 1; c && k <= 16; k++) {
		if (!(upl[seq].mask & (1u << (16 - k))))
			continue;
		attr_value(c, upl[seq].inst, k, body + n);
		n = (uint16_t)(n + attr_width(c, k));
	}
	*len = n;
	return 0;
}

/* What the stock omci_app does when the OLT creates a MAC bridge port config
 * data (class 47), read in mib_MacBriPortCfgData.so of V1.0-220923
 * (MacBriPortCfgDataDrvCfg, the add arm): it creates, in this order, a MAC
 * bridge port filter table data (49), a filter preassign table (79) and a
 * bridge table data (50), each with the entity id of the new port and all
 * attributes zero or an empty table. Both stock dumps agree: every class 47
 * instance of ISP1 and ISP2 has a 49, a 50 and a 79 under its own id. The
 * stock MIB reset creates none of them, so they are not autonomous, and a
 * decoded stock upload taken after a reset has none. The stock does it only
 * when the port's UNI-G is not "non-OMCI only", which omcid never reports.
 *
 * G.988 has the ONU create and delete them with the port. The stock delete arm
 * (MacBriPortCfgDataConnCfg) removes 49 and 50 but never 79, which stays in
 * its MIB: a leftover this does not copy. */
void mbpcd_tables_add(uint16_t inst)
{
	static const uint16_t cls[3] = { 49, 79, 50 };

	for (int i = 0; i < 3; i++)
		if (find_class(cls[i]) && !mib_add(cls[i], inst))
			out_fmt("   no room in the MIB for class %d of port %04x\n",
				(long)cls[i], (long)inst);
}

void mbpcd_tables_del(uint16_t inst)
{
	mib_del(49, inst);
	mib_del(79, inst);
	mib_del(50, inst);
}

/* The class 46 the ONU creates with each MAC bridge service profile (45), at
 * the profile's entity id. G.988 has the ONU create and delete it; the stock
 * firmware has no such entity at all (no plugin for class 46), so this is
 * from the standard alone. The row holds nothing: every attribute is derived
 * when read (attr_value), so a Set of the profile is seen at once, and the
 * row exists to put the entity in the MIB upload and the resume snapshot,
 * as for the port tables above. */
void mbcd_add(uint16_t inst)
{
	if (find_class(OMCI_ME_MAC_BRIDGE_CFG_DATA) &&
	    !mib_add(OMCI_ME_MAC_BRIDGE_CFG_DATA, inst))
		out_fmt("   no room in the MIB for class 46 of bridge %04x\n",
			(long)inst);
}

void mbcd_del(uint16_t inst)
{
	mib_del(OMCI_ME_MAC_BRIDGE_CFG_DATA, inst);
}

/* `contents` is everything from byte 8 on -- for a get that is the result code,
 * the attribute mask and the values, but a MIB upload response carries a count
 * and no result code at all, so this does not impose one. */
/* ------------------------------------------------------------------- apply
 *
 * Only 21 of the 81 managed entities reach the driver at all, and each does so
 * through wrappers that tools/omci-drv-abi.py has reduced to one command and a
 * payload. This is that map, for the entities whose payload came back whole.
 *
 * It is off unless -a is given. Everything above this point only answers
 * questions; from here the program reconfigures the switch, and it does so on
 * a device whose real configuration belongs to omci_app.
 */
uint32_t row_u32(const struct mib_row *r, const struct omci_class *c, unsigned k)
{
	int off = attr_offset(c, k);
	uint16_t w = attr_width(c, k);
	uint32_t v = 0;

	if (off < 0 || !w || off + w > MIB_ROW_MAX)
		return 0;
	for (uint16_t i = 0; i < w && i < 4; i++)
		v = (v << 8) | r->data[off + i];
	return v;
}
