/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */

/* Rendering: our own dump and the vendor's.
 *
 * Split out of main.c; see omcid.h.
 */
#include "omcid.h"
#include "../extvlan.h"

/* A secret attribute shows as ** per byte, the same width as its hex, so a
 * parser that splits the line sees the same fields; one that was never set
 * (all zero) has nothing to hide and shows its zeros. */
static void row_hex(const struct mib_row *r, int off, uint16_t w, int mask)
{
	if (mask) {
		mask = 0;
		for (uint16_t b = 0; b < w; b++)
			mask |= row_get(r, (unsigned)off + b) != 0;
	}
	for (uint16_t b = 0; b < w; b++) {
		if (mask)
			out("**");
		else
			out_hex(row_get(r, (unsigned)off + b), 2);
	}
}

void cli_row(const struct mib_row *r, int secrets)
{
	const struct omci_class *c = find_class(r->classId);

	out_fmt("%d %s %d%s\n", (long)r->classId, c ? c->name : "?",
		(long)r->inst, r->truncated ? "  (row truncated)" : "");
	for (unsigned k = 1; c && k < c->nattr && k <= 16; k++) {
		uint16_t w = attr_width(c, k);
		int off = attr_offset(c, k);

		if (!w || off < 0 || off + w > row_cap(r))
			continue;
		out_fmt("    %-24s ", c->attrs[k].name);
		row_hex(r, off, w, !secrets && me_attr_secret(r->classId, k));
		out_char('\n');
	}
	/* Class 171 in words. The vendor dump prints PRI 15, VID 4096 and
	 * nobody can read that; the same table can say what it does to a frame.
	 *
	 * Only in OUR dump, never in --vendor: that one is asserted byte for
	 * byte against a capture from a real device, and an extra line there
	 * would be a difference from the thing it exists to reproduce.
	 */
	if (c && c->classId == OMCI_ME_EXT_VLAN_TAGGING_OP_CFG_DATA && r->tbl_head != MIB_TBL_NONE) {
		uint16_t in_tpid = (uint16_t)row_u32(r, c, 3);
		uint16_t out_tpid = (uint16_t)row_u32(r, c, 4);
		char line[160];
		int n = 0;

		out("    tagging\n");
		for (uint8_t i = r->tbl_head; i != MIB_TBL_NONE;
		     i = tblpool[i].next) {
			struct evtocd_entry t;

			evtocd_decode(tblpool[i].data, &t);
			evtocd_describe(&t, in_tpid, out_tpid, line,
					sizeof line);
			out_fmt("      %2d  %s\n", (long)n++, line);
		}
	}
}

/* -------------------------------------------------- the vendor's dump format
 *
 * `omcicli mib get <class>` is parsed by shell scripts on the stick --
 * /etc/scripts/get_mib256.sh and get_olt.sh feed the web UI -- and they match
 * on the key at the start of a line, not on columns. So what has to be right
 * is the key spelling and the `Key: Value` shape.
 *
 * The keys cost nothing: generated/omci_mib.c took the attribute names from
 * the vendor's own plugins, so they are already the vendor's, **including its
 * inconsistencies**. Class 256 and 84 say `EntityID` and 131 and 171 say
 * `EntityId`, in the model and in the dump alike. Do not tidy that.
 *
 * The values are the work, because the formatting is per attribute and not
 * derivable from the type: an entity id is two hex digits, a U8 is decimal, a
 * vendor id is eight hex digits, and FwdOp has TWO spaces after its colon.
 * Captured from ISP1.
 *
 * Frame, per class:
 *
 *     XXXXXXXX...   33 of them
 *     <class name>  omci_classes[].name, which already matches
 *     XXXXXXXX...
 *
 * then, per instance:
 *
 *     ========...   33 of them
 *     Key: Value
 *     ========...
 *
 * so consecutive instances show two separator lines between them.
 */
#define V_RULE "=================================\n"
#define V_BANNER "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX\n"

static void v_rule(void) { out(V_RULE); }

void v_banner(const struct omci_class *c)
{
	out(V_BANNER);
	out(c->name);
	out_char('\n');
	out(V_BANNER);
}

/* A string attribute, printed as text. The stored size is one more than the
 * wire size -- the model records the NUL -- so stop at the first NUL or at
 * size-1, whichever comes first. */
static void v_str(const char *name, const struct mib_row *r,
		  const struct omci_class *c, unsigned k)
{
	int off = attr_offset(c, k);
	uint16_t w = attr_width(c, k);

	out(name);
	out(": ");
	if (off >= 0 && w) {
		/* w is already the width on the wire, without the vendor's
		 * NUL (attr_width), so every byte of it is printable: a
		 * four-character vendor id printed as three before. */
		for (uint16_t i = 0; i < w && off + i < MIB_ROW_MAX; i++) {
			uint8_t ch = r->data[off + i];

			if (!ch)
				break;
			out_char((char)ch);
		}
	}
	out_char('\n');
}

static void v_hex(const char *name, uint32_t v, int digits)
{
	out(name);
	out(": 0x");
	out_hex(v, digits);
	out_char('\n');
}

static void v_dec(const char *name, uint32_t v)
{
	out_fmt("%s: %d\n", name, (long)v);
}

/* Class 256, ONU-G. Fifteen attributes, printed in model order, which is the
 * order the vendor prints them in. */
static void v_ontg(const struct mib_row *r, const struct omci_class *c)
{
	static const char *dec[] = { "TraffMgtOpt", "AtmCCOpt", "BatteryBack",
				     "AdminState", "OpState", "OnuSurvivalTime",
				     "CredentialsStatus", "OntState" };

	v_hex("EntityID", r->inst, 2);
	v_str("VID", r, c, 1);
	v_str("Version", r, c, 2);
	/* SerialNum is four ASCII characters of vendor id followed by four
	 * binary bytes, and the vendor prints the second half as hex without a
	 * separator: ABCD01234567 (example value). */
	{
		int off = attr_offset(c, 3);

		out("SerialNum: ");
		if (off >= 0 && off + 8 <= MIB_ROW_MAX) {
			for (int i = 0; i < 4; i++)
				out_char((char)r->data[off + i]);
			for (int i = 4; i < 8; i++)
				out_hex(r->data[off + i], 2);
		}
		out_char('\n');
	}
	for (unsigned k = 4; k < c->nattr; k++) {
		const char *n = c->attrs[k].name;
		int is_dec = 0;

		for (unsigned i = 0; i < sizeof dec / sizeof dec[0]; i++)
			if (str_eq(dec[i], n))
				is_dec = 1;
		if (is_dec)
			v_dec(n, row_u32(r, c, k));
		else if (c->attrs[k].type == OMCI_STRING)
			v_str(n, r, c, k);
		else
			v_hex(n, row_u32(r, c, k), 1);
	}
}

/* Class 131, OLT-G. ToDInfo is stored as bytes but printed as a two-line
 * block, tab-indented: a four-byte superframe counter then a 64-bit seconds
 * and 32-bit nanoseconds timestamp. */
static void v_oltg(const struct mib_row *r, const struct omci_class *c)
{
	int off;

	v_hex("EntityId", r->inst, 2);
	v_hex("OltVendorId", row_u32(r, c, 1), 8);
	v_str("EquipId", r, c, 2);
	v_str("Version", r, c, 3);
	out("ToDInfo:\n");
	off = attr_offset(c, 4);
	if (off >= 0 && off + 14 <= MIB_ROW_MAX) {
		const uint8_t *p = r->data + off;
		uint32_t seq = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
			     | ((uint32_t)p[2] << 8) | p[3];
		uint32_t secs = ((uint32_t)p[8] << 24) | ((uint32_t)p[9] << 16)
			      | ((uint32_t)p[10] << 8) | p[11];
		uint32_t ns = ((uint32_t)p[12] << 8) | p[13];

		out("\tSequence number of GEM superframe: 0x");
		out_hex(seq, 1);
		out_char('\n');
		out_fmt("\tTimestamp: secs %d, nanosecs %d\n",
			(long)secs, (long)ns);
	}
}

/* Class 84, VLAN tagging filter data. The filter table is twelve two-byte TCI
 * entries and the vendor prints only the first NumOfEntries of them. Note the
 * two spaces after FwdOp's colon -- that is the vendor's, not a typo here. */
static void v_vlanfilter(const struct mib_row *r, const struct omci_class *c)
{
	int off = attr_offset(c, 1);
	uint32_t n = row_u32(r, c, 3);

	v_hex("EntityID", r->inst, 2);
	if (n > 12)
		n = 12;
	for (uint32_t i = 0; i < n; i++) {
		uint16_t tci;

		if (off < 0 || off + (int)(i * 2 + 2) > MIB_ROW_MAX)
			break;
		tci = (uint16_t)((r->data[off + i * 2] << 8)
				 | r->data[off + i * 2 + 1]);
		out_fmt("FilterTbl[%d]: PRI %d,CFI %d, VID %d\n", (long)i,
			(long)(tci >> 13), (long)((tci >> 12) & 1),
			(long)(tci & 0xfff));
	}
	out("FwdOp:  0x");
	out_hex(row_u32(r, c, 2), 2);
	out_char('\n');
	v_dec("NumOfEntries", row_u32(r, c, 3));
}

/* Class 171, extended VLAN tagging operation configuration data.
 *
 * The entry decode lives in extvlan.h, because the rule generator needs the
 * same one and two copies of a layout whose two halves differ is how one of
 * them ends up wrong.
 */
static void v_extvlan(const struct mib_row *r, const struct omci_class *c)
{
	int off;
	uint32_t idx = 0;

	v_hex("EntityId", r->inst, 2);
	v_dec("AssociationType", row_u32(r, c, 1));
	v_dec("ReceivedFrameVlanTagOperTableMaxSize", row_u32(r, c, 2));
	v_hex("InputTPID", row_u32(r, c, 3), 2);
	v_hex("OutputTPID", row_u32(r, c, 4), 2);
	v_dec("DsMode", row_u32(r, c, 5));
	out("ReceivedFrameVlanTaggingOperTable\n");
	for (uint8_t n = r->tbl_head; n != MIB_TBL_NONE; n = tblpool[n].next) {
		struct evtocd_entry t;

		evtocd_decode(tblpool[n].data, &t);
		out_fmt("INDEX %d\n", (long)idx++);
		out_fmt("Filter Outer   : PRI %d,VID %d, TPID %d\n",
			(long)t.f_out_pri, (long)t.f_out_vid, (long)t.f_out_tpid);
		out_fmt("Filter Inner   : PRI %d,VID %d, TPID %d, EthType 0x",
			(long)t.f_in_pri, (long)t.f_in_vid, (long)t.f_in_tpid);
		out_hex(t.f_ethertype, 2);
		out_char('\n');
		out_fmt("Treatment Outer   : PRI %d,VID %d, TPID %d, RemoveTags %d\n",
			(long)t.t_out_pri, (long)t.t_out_vid, (long)t.t_out_tpid,
			(long)t.remove_tags);
		out_fmt("Treatment Inner   : PRI %d,VID %d, TPID %d\n",
			(long)t.t_in_pri, (long)t.t_in_vid, (long)t.t_in_tpid);
	}
	v_hex("AssociatedMePoint", row_u32(r, c, 7), 2);
	/* DscpToPbitMapping is 24 bytes printed as eight tab-indented six-digit
	 * words, and then -- only when it is not all zero -- as 64 three-bit
	 * values. ISP1's map is all zero, which is why the second block never
	 * appeared in the capture. */
	out("DscpToPbitMapping:\n");
	off = attr_offset(c, 8);
	if (off >= 0 && off + 24 <= MIB_ROW_MAX) {
		const uint8_t *p = r->data + off;
		int any = 0;

		for (int i = 0; i < 8; i++) {
			out_char('\t');
			out("0x");
			out_hex(p[i * 3], 2);
			out_hex(p[i * 3 + 1], 2);
			out_hex(p[i * 3 + 2], 2);
			out_char('\n');
		}
		for (int i = 0; i < 24; i++)
			if (p[i])
				any = 1;
		if (any) {
			out("Parsed DscpToPbitMapping:\n");
			for (int i = 0; i < 64; i++) {
				uint32_t w = ((uint32_t)p[(i / 8) * 3] << 16)
					   | ((uint32_t)p[(i / 8) * 3 + 1] << 8)
					   | p[(i / 8) * 3 + 2];
				uint32_t sh = (uint32_t)(21 - (i % 8) * 3);

				out_fmt("dscp: %u => pbit: %u\n", (unsigned long)i,
					(unsigned long)((w >> sh) & 7));
			}
		}
	}
}

/* Renders `r` the way the vendor does, or returns 0 if this class has no
 * vendor renderer yet and the caller should fall back. */
int vendor_row_exists(const struct omci_class *c)
{
	return c && (c->classId == OMCI_ME_ONU_G ||
		     c->classId == OMCI_ME_OLT_G ||
		     c->classId == OMCI_ME_VLAN_TAGGING_FILTER_DATA ||
		     c->classId == OMCI_ME_EXT_VLAN_TAGGING_OP_CFG_DATA);
}

int vendor_row(const struct mib_row *r, const struct omci_class *c)
{
	if (!c)
		return 0;
	switch (c->classId) {
	case OMCI_ME_ONU_G:
		v_rule(); v_ontg(r, c);       v_rule(); return 1;
	case OMCI_ME_OLT_G:
		v_rule(); v_oltg(r, c);       v_rule(); return 1;
	case OMCI_ME_VLAN_TAGGING_FILTER_DATA:
		v_rule(); v_vlanfilter(r, c); v_rule(); return 1;
	case OMCI_ME_EXT_VLAN_TAGGING_OP_CFG_DATA:
		v_rule(); v_extvlan(r, c);    v_rule(); return 1;
	default:
		return 0;
	}
}

uint32_t cli_mib(void)
{
	const char *sel = cli_arg(1);
	uint32_t cls = 0, inst = 0;
	int want_inst = cli_num(2, &inst), secrets = 0;

	/* A selector is a class id, a table name, or "all". Taking a name and
	 * quietly ignoring it -- which is what this did first -- turns
	 * `mib GemPortCtp` into `mib all`, which is the wrong answer delivered
	 * confidently. */
	if (sel && !str_eq(sel, "all") && !cli_num(1, &cls)) {
		for (unsigned i = 0; i < omci_class_count; i++)
			if (str_eq(omci_classes[i].name, sel)) {
				cls = omci_classes[i].classId;
				break;
			}
		if (!cls) {
			out_fmt("no managed entity called %s\n", sel);
			return OMCLI_ENOCMD;
		}
	}
	/* The client lifts --secrets out of its own command line and sends it
	 * last, so it never shifts the selector or the entity id. */
	for (unsigned i = 1; cli_arg(i); i++)
		if (str_eq(cli_arg(i), "--secrets"))
			secrets = 1;
	mib_dump((uint16_t)cls, want_inst, (uint16_t)inst, 0, secrets);
	return OMCLI_OK;
}

/* Both MIB dumps, ours and the vendor-shaped one, over the same entities:
 * every autonomous one and every one the OLT created (mib_entities()), each
 * with the values a Get would return (mib_view()), not the raw row.
 *
 * One shape for every class and every outcome. A class with a vendor
 * renderer prints its banner once and one block per instance; any other
 * class prints our own block; and every dump, empty or not, vendor-shaped
 * or not, ends with one "N rows" line. The vendor printed no count for its
 * rendered classes, which left an empty answer and a missing class looking
 * the same as a truncated one; the scripts that parse this output match on
 * the key at the start of a line, and "N rows" has no key. */
int mib_dump(uint16_t cls, int want_inst, uint16_t inst, int vendor,
	     int secrets)
{
	const struct mib_ent *e;
	int count = mib_entities(cls, want_inst, inst, &e), n = 0;
	uint16_t banner = 0;
	int bannered = 0;

	for (int i = 0; i < count; i++) {
		const struct omci_class *c = find_class(e[i].cls);
		const struct mib_row *v;

		if (!c) {
			/* No model: nothing to render beyond what was stored. */
			const struct mib_row *r = mib_find(e[i].cls, e[i].inst);

			if (r) {
				cli_row(r, secrets);
				n++;
			}
			continue;
		}
		v = mib_view(c, e[i].inst);
		if (vendor && vendor_row_exists(c)) {
			if (!bannered || banner != c->classId) {
				v_banner(c);
				banner = c->classId;
				bannered = 1;
			}
			vendor_row(v, c);
		} else {
			cli_row(v, secrets);
		}
		n++;
	}
	out_fmt("%d row%s\n", (long)n, n == 1 ? "" : "s");
	return n;
}

uint32_t cli_flows(void)
{
	uint32_t max = caps_flows();

	int n = 0;

	out_fmt("flow table, %d entries per direction\n", (long)max);
	out("  flow  upstream  downstream\n");
	for (uint32_t i = 0; i < max; i++) {
		if (!flow_us[i].used && !flow_ds[i].used)
			continue;
		out_fmt("  %4d", (long)i);
		if (flow_us[i].used)
			out_fmt("  gem %-5d", (long)flow_us[i].port);
		else
			out("  -        ");
		if (flow_ds[i].used)
			out_fmt(" gem %-5d", (long)flow_ds[i].port);
		else
			out(" -");
		out_char('\n');
		n++;
	}
	if (!n)
		out("  (none programmed)\n");
	if (bc_flow >= 0)
		out_fmt("downstream broadcast flow: %d\n", (long)bc_flow);
	else
		out("downstream broadcast flow: not designated\n");
	return OMCLI_OK;
}

uint32_t cli_caps(void)
{
	if (!caps_ok) {
		out("capabilities were not read (the driver refused, or -c gave none)\n");
		return OMCLI_EFAIL;
	}
	out_fmt("gem flows per direction : %d\n", (long)caps_u32(OMCI_CAPS_OFF_FLOWS));
	out_fmt("t-conts                 : %d\n", (long)caps_u32(OMCI_CAPS_OFF_TCONTS));
	out_fmt("priority queues         : %d\n", (long)caps_u32(OMCI_CAPS_OFF_PRIQ));
	out_fmt("pon port / cpu port     : %d / %d\n",
		(long)caps_u32(OMCI_CAPS_OFF_PONPORT),
		(long)caps_u32(OMCI_CAPS_OFF_CPUPORT));
	out("uni slots (index is the switch port):\n");
	for (unsigned i = 0; i < OMCI_CAPS_UNI_SLOTS; i++)
		if (caps[i * 2 + 1] != 0xff)
			out_fmt("  port %-2d  type %d  uni index %d\n",
				(long)i, (long)caps[i * 2], (long)caps[i * 2 + 1]);
	out("raw:\n");
	for (unsigned i = 0; i < OMCI_CAPS_LEN; i += 16) {
		out("  ");
		out_hex(i, 3);
		out_char(' ');
		for (unsigned j = 0; j < 16 && i + j < OMCI_CAPS_LEN; j++) {
			out_hex(caps[i + j], 2);
			out_char(' ');
		}
		out_char('\n');
	}
	return OMCLI_OK;
}

uint32_t cli_tcont(void)
{
	int n = 0;

	out("t-cont map: the OLT's entity id against the driver's index\n");
	for (int i = 0; i < TCONT_MAX; i++)
		if (tcont_map[i].used) {
			out_fmt("  me %04x -> index %d\n", (long)tcont_map[i].meId,
				(long)tcont_map[i].index);
			n++;
		}
	if (!n)
		out("  (none allocated)\n");
	{
		const uint16_t *ids;
		unsigned na = alloc_ids_assigned(&ids), unassigned = 0;
		static const char *const from[] = { "unassigned", "olt", "ploam" };

		out_fmt("alloc-ids the olt assigned (ploam): %d", (long)na);
		for (unsigned i = 0; i < na; i++)
			out_fmt(" %d", (long)ids[i]);
		out("\nt-cont alloc-ids (olt: set over omci; ploam: bound in "
		    "assignment order):\n");
		for (unsigned i = 0; i < aut_count; i++) {
			const struct omci_instance *e = &aut[i];
			int src;
			uint16_t a;

			if (e->classId != OMCI_ME_TCONT)
				continue;
			a = tcont_alloc_id(e->inst, &src);
			if (src == TCONT_ALLOC_NONE && a == 0x00ff) {
				unassigned++;
				continue;
			}
			out_fmt("  me %04x alloc %d (%s)\n", (long)e->inst,
				(long)a, from[src]);
		}
		out_fmt("  %d more unassigned (255)\n", (long)unassigned);
	}
	return OMCLI_OK;
}

uint32_t cli_state(void)
{
	uint32_t st = 0;

	/* Four ASCII characters of vendor id then four binary bytes, which is
	 * why printing it as a string stops after the vendor id (for a serial
	 * such as ABCD0000002A, after "ABCD"). */
	serial_refresh(0);
	out("serial     : ");
	out_n((const char *)serial, 4);
	for (int i = 4; i < 8; i++)
		out_hex(serial[i], 2);
	out_char('\n');
	out_fmt("device     : %s\n", (const char *)devid);
	if (omci_getOnuState(&st) == 0)
		out_fmt("onu state  : O%d\n", (long)st);
	else
		out("onu state  : unreadable\n");
	out_fmt("mib sync   : %d\n", (long)mib_data_sync);
	out_fmt("programming: %s\n", apply_hw ? "on" : "off");
	out_fmt("rx dropped : length %d  devid %d  crc %d  (trailer odd, answered: %d)\n",
		(long)omci_rx_reject[RX_REJ_LENGTH],
		(long)omci_rx_reject[RX_REJ_DEVID],
		(long)omci_rx_reject[RX_REJ_CRC],
		(long)omci_rx_reject[RX_REJ_TRAILER]);
	{
		unsigned long olt, local, loops;
		unsigned recent;
		int loop;

		ev_mib_reset_counts(&olt, &local, &recent, &loop, &loops);
		out_fmt("mib resets : olt %d  local %d  olt in last %d s %d  "
			"loop %s  (loops %d)\n", (long)olt, (long)local,
			(long)MIBLOOP_WINDOW_S, (long)recent,
			loop ? "yes" : "no", (long)loops);
	}
	return OMCLI_OK;
}

/* `conn` prints our service table in the shape omcicli dump srvflow prints the
 * vendor's, so the two can be diffed line for line against a stick that is
 * still running the vendor daemon. Only the used rows: 256 rows of zeroes is
 * what the vendor prints and it is not useful here. */
uint32_t cli_conn(void)
{
	int n = 0;

	for (int i = 0; i < SERV_MAX; i++) {
		const struct omci_bdgconn *r = &servtab[i];

		if (!r->in_use)
			continue;
		out_fmt("SERVID %d: Used: 1, (DIR=%d, USFID=%d, DSFID=%d, "
			"SERVID=%d, UNIMASK=%d)\n", (long)i, (long)r->dir,
			(long)r->us_flow, (long)r->ds_flow, (long)r->service_id,
			(long)r->uni_mask);
		out_fmt("    rule_gen %d  stagMode %d  ctagMode %d  "
			"tag_count %d  out_tag (pri %d, vid %d)\n",
			(long)r->vlan_op.rule_gen,
			(long)r->vlan_op.filter.outer_mode,
			(long)r->vlan_op.filter.inner_mode,
			(long)r->vlan_op.out.tag_count,
			(long)r->vlan_op.out.out_tag.pri,
			(long)r->vlan_op.out.out_tag.vid);
		n++;
	}
	out_fmt("%d service%s\n", (long)n, n == 1 ? "" : "s");
	return OMCLI_OK;
}

/* `bridge <ingressMeId> <gemPort> <dir>` -- build one N:1 all-pass rule.
 *
 * Explicit rather than automatic on purpose: the five-way choice of generator
 * needs the MIB tree the vendor maintains, and this is the one branch whose
 * construction is known end to end.
 *
 * Without -a it is a DRY RUN: the descriptor is built and printed and nothing
 * is sent. That is the useful mode for anything but a stick you are willing to
 * take off the air -- these 160 bytes are what make forwarding work or stop,
 * and until now the only way to inspect one was to send it.
 *
 * The ingress is the entity id the OLT used -- a PPTP Ethernet UNI, a VEIP, or
 * an IP host config data -- or the literal `any` for the all-UNIs mask. */
uint32_t cli_bridge(void)
{
	struct omci_vlan_oper vr;
	const char *a0 = cli_arg(1);
	uint32_t gem = 0, dir = 0;
	int ingress, service_id;

	if (!a0 || !cli_num(2, &gem) || !cli_num(3, &dir)) {
		out("usage: bridge <ingressMeId|any> <gemPort> <dir 1|2|3> "
		    "[vid|transparent]\n");
		return OMCLI_EARGS;
	}
	if (dir != OMCI_DIR_US && dir != OMCI_DIR_DS && dir != OMCI_DIR_BI) {
		out("direction must be 1 (us), 2 (ds) or 3 (both)\n");
		return OMCLI_EARGS;
	}
	if (str_eq(a0, "any")) {
		ingress = -1;
	} else {
		uint32_t v;

		if (!cli_id(1, &v) || v > 0xffff) {
			out("ingress must be an entity id (decimal or 0x...), "
			    "or `any`\n");
			return OMCLI_EARGS;
		}
		ingress = (int)v;
	}

	/* The rule the stick actually needs. In manual VLAN mode -- which both
	 * our sticks are in -- the service tag comes from the config store, not
	 * from class 171, whose treatment fields carry the "no VID" sentinel.
	 * Without this the connection forwards and the OLT drops what it
	 * forwards, which is exactly what S2's experiment measured. An explicit
	 * fourth argument overrides, and `transparent` asks for the old rule. */
	{
		const char *a3 = cli_arg(4);
		int vid = cfg_manual_vid(), pri = vlanCfg.pri;
		uint32_t v;

		if (a3 && str_eq(a3, "transparent")) {
			vid = -1;
		} else if (a3 && cli_num(4, &v)) {
			vid = (int)v;
		}
		if (vid >= 0) {
			gen_manual_vlan_rule(&vr, vid, pri, dir == OMCI_DIR_DS, manual_any_pri());
			out_fmt("rule: c-tag %s vid %d pri %d\n",
				dir == OMCI_DIR_DS ? "remove" : "add",
				(long)vid, (long)pri);
		} else {
			gen_no_vlan_filter_rule(&vr);
			out("rule: transparent (no vlan operation)\n");
		}
	}
	service_id = bdgconn_add(ingress, (uint16_t)gem, dir, &vr);
	if (service_id < 0) {
		out("refused\n");
		return OMCLI_EFAIL;
	}
	out_fmt("service %d\n", (long)service_id);
	return OMCLI_OK;
}

/* The manual VLAN, which is where a service's tag comes from on both our
 * sticks. Takes an optional path so a test can point it at a fixture. */
uint32_t cli_vlan(void)
{
	const char *cs = cli_arg(1);

	if (cs)
		cfg_load_vlan_from(cs);
	else
		cfg_load_vlan();
	cfg_show_vlan();
	return OMCLI_OK;
}

/* The identity the line authenticates with. Takes the two file paths so the
 * test can point it at fixtures; with no arguments it reads the store. The
 * passwords are never printed -- see cfg_show_identity. */
uint32_t cli_ident(void)
{
	const char *cs = cli_arg(1), *hs = cli_arg(2), *odi = cli_arg(3);

	if (cs && hs) {
		cfg_load_identity_from(cs, hs);
		cfg_load_report_from(cs, hs, CFG_REPORT_SWITCH,
				     odi ? odi : CFG_ODI_PATH);
	} else {
		cfg_load_identity();
		cfg_load_report();
	}
	cfg_show_identity();
	return OMCLI_OK;
}

/* `cfgset <file> <dir> <key> <value>` -- one attribute, in place.
 *
 * A tool, not a provisioning command: it names the file and the Dir explicitly
 * rather than knowing which key belongs where, because the caller that does
 * know is the test. Writing a key into the wrong file makes a duplicate the
 * vendor's own reader will not see, so this refuses to guess. */
uint32_t cli_cfgset(void)
{
	const char *path = cli_arg(1), *dir = cli_arg(2);
	const char *key = cli_arg(3), *value = cli_arg(4);

	if (!path || !dir || !key)
		return OMCLI_EARGS;
	if (cfg_set(path, dir, key, value ? value : "") != 0) {
		out("write failed\n");
		return OMCLI_EARGS;
	}
	out("ok\n");
	return OMCLI_OK;
}

/* `provision` -- what the OLT provisioned, one line per item, for metricsd
 * (gpon_provision_*, odi-sfp-exporter) and for a person comparing two days:
 *
 *   tcont me=<id> alloc_id=<n> index=<n>     a T-CONT omcid programmed
 *   gem me=<id> port=<n> direction=<1|2|3> tcont_me=<id> us_td=<id> ds_td=<id>
 *   vlan vid=<n> source=<vlan_filter|ext_vlan_filter|ext_vlan_treatment>
 *   td me=<id> cir=<B/s> pir=<B/s> cbs=<B> pbs=<B>
 *   summary rows=<n> tconts=<n> gem_ports=<n> vlans=<n> traffic_descriptors=<n>
 *           services=<n> mib_data_sync=<n>
 *
 * All numbers decimal. A VLAN is listed once per (vid, source), whichever
 * entities name it: an ISP plan change moves a VID, not an entity count.
 * The T-CONT lines are the ones omcid programmed into the switch; the
 * Alloc-IDs the OLT assigned by PLOAM are the kernel's (/proc/odi_gpon/status,
 * alloc_ids), which is the list to trust where the two differ. */
#define PROV_VLANS 32

static struct { uint16_t vid; uint8_t src; } prov_vlan[PROV_VLANS];
static int prov_nvlan;
static const char *const prov_vlan_src[] = {
	"vlan_filter", "ext_vlan_filter", "ext_vlan_treatment" };

static void prov_vlan_add(uint16_t vid, uint8_t src)
{
	if (vid > EVTOCD_VID_MAX)
		return;
	int i;

	for (i = 0; i < prov_nvlan; i++)
		if (prov_vlan[i].vid == vid && prov_vlan[i].src == src)
			return;
	if (prov_nvlan >= PROV_VLANS)
		return;
	/* Sorted by VID, then source: the store slot order the rows arrive
	 * in is not stable across sessions, and a diff of two days should
	 * show a changed VID, not a reshuffle. */
	for (i = prov_nvlan; i > 0 && (prov_vlan[i - 1].vid > vid ||
			(prov_vlan[i - 1].vid == vid && prov_vlan[i - 1].src > src)); i--)
		prov_vlan[i] = prov_vlan[i - 1];
	prov_vlan[i].vid = vid;
	prov_vlan[i].src = src;
	prov_nvlan++;
}

uint32_t cli_provision(void)
{
	int tconts = 0, gems = 0, tds = 0;
	unsigned services = 0;

	prov_nvlan = 0;
	for (int i = 0; i < TCONT_MAX; i++) {
		const struct omci_class *c = find_class(OMCI_ME_TCONT);
		uint8_t b[2] = { 0, 0 };

		if (!tcont_map[i].used || !c)
			continue;
		attr_value(c, tcont_map[i].meId, 1, b);
		out_fmt("tcont me=%u alloc_id=%u index=%u\n",
			(unsigned long)tcont_map[i].meId,
			(unsigned long)((b[0] << 8) | b[1]),
			(unsigned long)tcont_map[i].index);
		tconts++;
	}
	for (int i = 0; i < MIB_ROWS; i++) {
		const struct mib_row *r = &mib[i];
		const struct omci_class *c;

		if (!r->used || !(c = find_class(r->classId)))
			continue;
		if (r->classId == OMCI_ME_GEM_PORT_CTP) {
			out_fmt("gem me=%u port=%u direction=%u tcont_me=%u "
				"us_td=%u ds_td=%u\n", (unsigned long)r->inst,
				(unsigned long)row_u32(r, c, 1),
				(unsigned long)row_u32(r, c, 3),
				(unsigned long)row_u32(r, c, 2),
				(unsigned long)row_u32(r, c, 5),
				(unsigned long)row_u32(r, c, 9));
			gems++;
		} else if (r->classId == OMCI_ME_TRAFFIC_DESCRIPTOR) {
			out_fmt("td me=%u cir=%u pir=%u cbs=%u pbs=%u\n",
				(unsigned long)r->inst,
				(unsigned long)row_u32(r, c, 1),
				(unsigned long)row_u32(r, c, 2),
				(unsigned long)row_u32(r, c, 3),
				(unsigned long)row_u32(r, c, 4));
			tds++;
		} else if (r->classId == OMCI_ME_VLAN_TAGGING_FILTER_DATA) {
			int off = attr_offset(c, 1);
			uint32_t n = row_u32(r, c, 3);

			for (uint32_t k = 0; k < n && k < 12 && off >= 0 &&
			     off + (int)(k * 2 + 2) <= MIB_ROW_MAX; k++)
				prov_vlan_add((uint16_t)(((r->data[off + k * 2] << 8) |
					r->data[off + k * 2 + 1]) & 0xfff), 0);
		} else if (r->classId == OMCI_ME_EXT_VLAN_TAGGING_OP_CFG_DATA) {
			for (uint8_t t = r->tbl_head; t != MIB_TBL_NONE;
			     t = tblpool[t].next) {
				struct evtocd_entry e;

				evtocd_decode(tblpool[t].data, &e);
				/* A filter VID counts only where its tag is
				 * examined at all: priority 15 ignores the
				 * tag, 14 is a default rule, and VID 4096 is
				 * do-not-filter (above EVTOCD_VID_MAX). */
				if (e.f_out_pri < EVTOCD_F_PRI_DEFAULT_RULE)
					prov_vlan_add(e.f_out_vid, 1);
				if (e.f_in_pri < EVTOCD_F_PRI_DEFAULT_RULE)
					prov_vlan_add(e.f_in_vid, 1);
				/* A treatment VID counts only where a tag is
				 * added (priority 15 adds none) and the VID
				 * is assigned, not copied (4096/4097). */
				if (e.remove_tags != EVTOCD_T_DISCARD_FRAME) {
					if (e.t_out_pri != EVTOCD_T_PRI_DO_NOT_ADD)
						prov_vlan_add(e.t_out_vid, 2);
					if (e.t_in_pri != EVTOCD_T_PRI_DO_NOT_ADD)
						prov_vlan_add(e.t_in_vid, 2);
				}
			}
		}
	}
	for (int i = 0; i < prov_nvlan; i++)
		out_fmt("vlan vid=%u source=%s\n", (unsigned long)prov_vlan[i].vid,
			prov_vlan_src[prov_vlan[i].src]);
	for (int i = 0; i < SERV_MAX; i++)
		services += servtab[i].in_use ? 1u : 0u;
	out_fmt("summary rows=%u tconts=%u gem_ports=%u vlans=%u "
		"traffic_descriptors=%u services=%u mib_data_sync=%u\n",
		(unsigned long)mib_count(), (unsigned long)tconts,
		(unsigned long)gems, (unsigned long)prov_nvlan,
		(unsigned long)tds, (unsigned long)services,
		(unsigned long)mib_data_sync);
	return OMCLI_OK;
}

uint32_t cli_help(void)
{
	out("omcid commands\n"
	    "  mib [all|classId] [entityId]   the MIB this daemon has been told\n"
	    "  flows                          the gem flow tables, per direction\n"
	    "  caps                           the device capability blob, decoded\n"
	    "  tcont                          entity id to driver index\n"
	    "  conn                           the bridge connections we built\n"
	    "  bridge <ingress|any> <gem> <dir>  build one N:1 all-pass rule\n"
	    "  state                          serial, device, onu state\n"
	    "  provision                      what the OLT provisioned: T-CONTs,\n"
	    "                                 GEM ports, VLANs, traffic descriptors\n"
	    "  voip [--secrets|--secrets-list]  the voice lines the OLT provisioned,\n"
	    "                                 number and login masked, no password\n"
	    "  ident [cs hs [odi]]            identity from the config store\n"
	    "  cfgset <file> <dir> <key> <v>  write one key into a config store\n"
	    "  help\n");
	return OMCLI_OK;
}
