/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */

/* The identity the line authenticates with, out of the jffs2 config store.
 *
 * `/etc/config` is a symlink to `/var/config`, mtd3 mounted jffs2, and the
 * store is two ordinary XML files -- not an opaque blob and not something that
 * needs `libmib.so.0`. The vendor's own
 * scripts read it by shelling out to `mib get`, which a freestanding daemon
 * cannot do and does not need to: the format is three elements deep.
 *
 *     <Config Name="ROOT">
 *       <Dir Name="MIB_TABLE">            <!-- HW_MIB_TABLE in the hs file -->
 *         <Value Name="KEY" Value="VALUE"/>
 *       </Dir>
 *     </Config>
 *
 * Which file a key is in comes from xmlconfig's own 102-row descriptor table,
 * not from guessing: ten keys are HS and the rest CS. The split is not what the
 * names suggest -- LOID, LOID_PASSWD and LOID_OLD are service config while
 * LOID_PASSWD_OLD alone is hardware identity.
 *
 * This does not parse XML. It looks for the exact byte sequence a Value element
 * starts with, which is all these files ever contain, and refuses anything it
 * does not recognise rather than guessing. A store written by something other
 * than xmlconfig would read as absent, which is the safe direction.
 */
#include "omcid.h"

/* Big enough for the whole store: isp1's is a few KB and the partition is
 * 240 KB with 204 KB free, so a file that does not fit here is not a config
 * store. Static, because there is no allocator. */
static char cfgbuf[16384];

/* The five entities xmlconfig carries, in two tables in its rodata beside the
 * `<Value Name="%s" Value=` writer format. So values in the store ARE escaped,
 * and a reader that does not undo it is wrong.
 *
 * It does not bite on GPON_SN, which is [A-Z0-9]. It bites on a PLOAM or LOID
 * password, which are the two fields that decide whether a line authenticates
 * -- and it bites silently, because nothing can tell a wrong password from a
 * wrong password. The first version of this file had that bug.
 *
 * Longest first is not needed here (no entity is a prefix of another) but the
 * table is ordered that way anyway so adding one cannot introduce the problem.
 */
static const struct { const char *ent; int len; char ch; } xml_ent[] = {
	{ "&quot;", 6, '"' },
	{ "&apos;", 6, '\'' },
	{ "&amp;",  5, '&'  },
	{ "&lt;",   4, '<'  },
	{ "&gt;",   4, '>'  },
};

#define XML_ENT_N ((int)(sizeof xml_ent / sizeof xml_ent[0]))

/* Decode `n` bytes into `out`, NUL-terminated. Returns the DECODED length,
 * which is what the caller reports -- an entity is five or six bytes on disk
 * and one in the value, and the difference is exactly what tells a present-
 * but-empty key from a missing one. */
static int xml_unescape(const char *in, int n, char *out, int max)
{
	int i = 0, o = 0;

	while (i < n) {
		int k;

		for (k = 0; k < XML_ENT_N; k++) {
			int l = xml_ent[k].len;
			int m;

			if (in[i] != '&' || i + l > n)
				continue;
			for (m = 1; m < l; m++)
				if (in[i + m] != xml_ent[k].ent[m])
					break;
			if (m == l)
				break;
		}
		if (k < XML_ENT_N) {
			if (o < max - 1)
				out[o] = xml_ent[k].ch;
			i += xml_ent[k].len;
		} else {
			if (o < max - 1)
				out[o] = in[i];
			i++;
		}
		o++;
	}
	out[o < max - 1 ? o : max - 1] = 0;
	return o;
}

/* The inverse, for the writer. Returns the encoded length, or -1 if it would
 * not fit -- silently truncating a password is the one failure mode this must
 * not have. */
static int xml_escape(const char *in, char *out, int max)
{
	int o = 0;

	for (int i = 0; in[i]; i++) {
		int k;

		for (k = 0; k < XML_ENT_N; k++)
			if (in[i] == xml_ent[k].ch)
				break;
		if (k < XML_ENT_N) {
			if (o + xml_ent[k].len > max)
				return -1;
			for (int m = 0; m < xml_ent[k].len; m++)
				out[o + m] = xml_ent[k].ent[m];
			o += xml_ent[k].len;
		} else {
			if (o + 1 > max)
				return -1;
			out[o++] = in[i];
		}
	}
	return o;
}

/* One key out of one file. Returns the value's length, or -1 when the file or
 * the key is not there -- which is NOT the same as a key present and empty, and
 * the caller has to tell them apart: an empty LOID means "do not send -l" and a
 * missing one means the store was never written. */
int cfg_get(const char *path, const char *key, char *out, int max)
{
	long fd = sys_open(path, 0);
	int n = 0, klen = str_len(key);

	if (fd < 0)
		return -1;
	for (;;) {
		long r = sys_read((int)fd, cfgbuf + n, sizeof cfgbuf - 1 - n);

		if (r <= 0)
			break;
		n += (int)r;
		if (n >= (int)sizeof cfgbuf - 1)
			break;
	}
	sys_close((int)fd);
	cfgbuf[n] = 0;

	for (int i = 0; i + 13 < n; i++) {
		int j;

		/* `<Value Name="` -- 13 bytes, and the only place a key name
		 * appears. Matching on the key alone would also hit the Dir
		 * element and any description text. */
		if (cfgbuf[i] != '<' || cfgbuf[i + 1] != 'V'
		    || cfgbuf[i + 2] != 'a' || cfgbuf[i + 3] != 'l'
		    || cfgbuf[i + 4] != 'u' || cfgbuf[i + 5] != 'e'
		    || cfgbuf[i + 6] != ' ' || cfgbuf[i + 7] != 'N'
		    || cfgbuf[i + 8] != 'a' || cfgbuf[i + 9] != 'm'
		    || cfgbuf[i + 10] != 'e' || cfgbuf[i + 11] != '='
		    || cfgbuf[i + 12] != '"')
			continue;
		j = i + 13;
		/* The key must match to its closing quote, so LOID does not
		 * answer for LOID_OLD. */
		for (int k = 0; k < klen; k++)
			if (j + k >= n || cfgbuf[j + k] != key[k])
				goto next;
		if (j + klen >= n || cfgbuf[j + klen] != '"')
			goto next;
		j += klen + 1;
		/* ` Value="` -- the attribute order xmlconfig writes. Anything
		 * else is not a file it wrote. */
		if (j + 8 > n || cfgbuf[j] != ' ' || cfgbuf[j + 1] != 'V'
		    || cfgbuf[j + 2] != 'a' || cfgbuf[j + 3] != 'l'
		    || cfgbuf[j + 4] != 'u' || cfgbuf[j + 5] != 'e'
		    || cfgbuf[j + 6] != '=' || cfgbuf[j + 7] != '"')
			goto next;
		j += 8;
		{
			int end = j;

			while (end < n && cfgbuf[end] != '"')
				end++;
			if (end >= n)
				return -1;       /* unterminated */
			return xml_unescape(cfgbuf + j, end - j, out, max);
		}
next:
		;
	}
	return -1;
}

/* One odi-only key out of odi.conf, the plain KEY=value file `flash` keeps
 * beside the XML store (rootfs/skeleton/etc/scripts/flash, ODI_KEYS). Same
 * contract as cfg_get: the value length, 0 for present and empty, -1 for a
 * missing file or key. The first line for the key wins, as `flash get` reads
 * it; a line that is not KEY=value is skipped. One bounded read per call: the
 * file holds a handful of short lines, and anything past the buffer is
 * treated as absent rather than read in pieces. */
static char odibuf[4096];

int cfg_odi_get(const char *path, const char *key, char *out, int max)
{
	long fd = sys_open(path, 0);
	int n = 0, klen = str_len(key);

	if (fd < 0)
		return -1;
	for (;;) {
		long r = sys_read((int)fd, odibuf + n, sizeof odibuf - 1 - n);

		if (r <= 0)
			break;
		n += (int)r;
		if (n >= (int)sizeof odibuf - 1)
			break;
	}
	sys_close((int)fd);
	for (int i = 0; i < n;) {
		int end = i, k = 0;

		while (end < n && odibuf[end] != '\n')
			end++;
		/* A line cut off by the buffer is not a whole value. */
		if (end >= n && n >= (int)sizeof odibuf - 1)
			break;
		while (k < klen && i + k < end && odibuf[i + k] == key[k])
			k++;
		if (k == klen && i + k < end && odibuf[i + k] == '=') {
			int v = i + k + 1, len = end - v, o;

			if (len > 0 && odibuf[end - 1] == '\r')
				len--;
			for (o = 0; o < len && o < max - 1; o++)
				out[o] = odibuf[v + o];
			out[o] = 0;
			return len;
		}
		i = end + 1;
	}
	return -1;
}

/* Replace one key's value, or insert the key, and write the file back.
 *
 * Deliberately NOT what `flash set` does. That runs `xmlconfig -s` to update
 * libmib's shared state and then `xmlconfig -of` to dump the WHOLE state over
 * the file -- so every key libmib holds is rewritten, and the store has 102 of
 * them against the six we understand. This edits one attribute in place and
 * leaves every byte around it alone, which cannot lose a key we do not model.
 *
 * The other divergence is atomicity. `flash set` overwrites lastgood.xml
 * directly -- the script has a ${LASTGOOD_FILE}_bak name but only ever removes
 * it -- and on jffs2 a power cut mid-write is how a stick comes back without
 * its serial. This writes a sibling temp file and renames over the target. The
 * bytes that end up in the file are identical either way; only the failure mode
 * differs, so this is not a behaviour change.
 *
 * Returns 0, or -1 and writes nothing. `dir` is the element the key belongs
 * under when it has to be inserted -- "MIB_TABLE" or "HW_MIB_TABLE" -- and
 * getting it wrong creates a duplicate the vendor's own reader will not see.
 */
int cfg_set(const char *path, const char *dir, const char *key, const char *value)
{
	static char outbuf[sizeof cfgbuf + 256];
	char esc[192];
	char tmp[128];
	int n, elen, o = 0, klen = str_len(key), dlen = str_len(dir);
	int at = -1, vstart = -1, vend = -1, dirend = -1;
	long fd;

	elen = xml_escape(value, esc, (int)sizeof esc);
	if (elen < 0)
		return -1;                       /* would truncate: refuse */
	esc[elen] = 0;

	fd = sys_open(path, 0);
	if (fd < 0)
		return -1;
	n = 0;
	for (;;) {
		long r = sys_read((int)fd, cfgbuf + n, sizeof cfgbuf - 1 - n);

		if (r <= 0)
			break;
		n += (int)r;
		if (n >= (int)sizeof cfgbuf - 1)
			break;
	}
	sys_close((int)fd);
	cfgbuf[n] = 0;

	/* Find the key's value span, and -- in case it is absent -- the end of
	 * the Dir it belongs in. Both in one pass over a file of a few KB. */
	for (int i = 0; i + 5 < n; i++) {
		if (cfgbuf[i] == '<' && cfgbuf[i + 1] == '/'
		    && cfgbuf[i + 2] == 'D' && cfgbuf[i + 3] == 'i'
		    && cfgbuf[i + 4] == 'r' && dirend < 0 && at < 0) {
			/* The first </Dir> after the Dir whose name matched. */
			if (dirend == -2)
				dirend = i;
			continue;
		}
		if (cfgbuf[i] == '<' && cfgbuf[i + 1] == 'D'
		    && cfgbuf[i + 2] == 'i' && cfgbuf[i + 3] == 'r') {
			int j = i + 4;

			while (j + 6 < n && cfgbuf[j] != '"')
				j++;
			j++;
			{
				int m = 0;

				while (m < dlen && j + m < n
				       && cfgbuf[j + m] == dir[m])
					m++;
				if (m == dlen && j + m < n && cfgbuf[j + m] == '"')
					dirend = -2;     /* inside our Dir now */
			}
			continue;
		}
		if (i + 13 >= n || cfgbuf[i] != '<' || cfgbuf[i + 1] != 'V'
		    || cfgbuf[i + 2] != 'a' || cfgbuf[i + 3] != 'l'
		    || cfgbuf[i + 4] != 'u' || cfgbuf[i + 5] != 'e'
		    || cfgbuf[i + 6] != ' ' || cfgbuf[i + 7] != 'N'
		    || cfgbuf[i + 8] != 'a' || cfgbuf[i + 9] != 'm'
		    || cfgbuf[i + 10] != 'e' || cfgbuf[i + 11] != '='
		    || cfgbuf[i + 12] != '"')
			continue;
		{
			int j = i + 13, k;

			for (k = 0; k < klen; k++)
				if (j + k >= n || cfgbuf[j + k] != key[k])
					break;
			if (k != klen || j + klen >= n || cfgbuf[j + klen] != '"')
				continue;
			j += klen + 1;
			if (j + 8 > n || cfgbuf[j] != ' ' || cfgbuf[j + 1] != 'V'
			    || cfgbuf[j + 2] != 'a' || cfgbuf[j + 3] != 'l'
			    || cfgbuf[j + 4] != 'u' || cfgbuf[j + 5] != 'e'
			    || cfgbuf[j + 6] != '=' || cfgbuf[j + 7] != '"')
				continue;
			at = i;
			vstart = j + 8;
			vend = vstart;
			while (vend < n && cfgbuf[vend] != '"')
				vend++;
			if (vend >= n)
				return -1;
			break;
		}
	}
	/* dirend is -2 while inside the matching Dir and a byte offset once its
	 * closing tag is seen. The loop above covers every byte of the file, so
	 * a -2 here means the Dir was opened and never closed -- a malformed
	 * store, which is refused rather than repaired. */
	if (at < 0 && dirend < 0)
		return -1;                       /* no such Dir: refuse */

	if (at >= 0) {
		for (int i = 0; i < vstart; i++)
			outbuf[o++] = cfgbuf[i];
		for (int i = 0; i < elen; i++)
			outbuf[o++] = esc[i];
		for (int i = vend; i < n; i++)
			outbuf[o++] = cfgbuf[i];
	} else {
		static const char pre[] = "\t\t<Value Name=\"";
		static const char mid[] = "\" Value=\"";
		static const char post[] = "\"/>\n";

		for (int i = 0; i < dirend; i++)
			outbuf[o++] = cfgbuf[i];
		for (int i = 0; i < (int)sizeof pre - 1; i++)
			outbuf[o++] = pre[i];
		for (int i = 0; i < klen; i++)
			outbuf[o++] = key[i];
		for (int i = 0; i < (int)sizeof mid - 1; i++)
			outbuf[o++] = mid[i];
		for (int i = 0; i < elen; i++)
			outbuf[o++] = esc[i];
		for (int i = 0; i < (int)sizeof post - 1; i++)
			outbuf[o++] = post[i];
		for (int i = dirend; i < n; i++)
			outbuf[o++] = cfgbuf[i];
	}

	/* Same directory, so the rename is within one filesystem. */
	str_copy(tmp, path, (int)sizeof tmp - 5);
	{
		int t = str_len(tmp);

		tmp[t] = '.'; tmp[t + 1] = 'n'; tmp[t + 2] = 'e';
		tmp[t + 3] = 'w'; tmp[t + 4] = 0;
	}
	fd = sys_create(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return -1;
	for (int done = 0; done < o; ) {
		long w = sys_write((int)fd, outbuf + done, (unsigned long)(o - done));

		if (w <= 0) {
			sys_close((int)fd);
			sys_unlink(tmp);
			return -1;
		}
		done += (int)w;
	}
	sys_close((int)fd);
	if (sys_rename(tmp, path) < 0) {
		sys_unlink(tmp);
		return -1;
	}
	return 0;
}

#define CFG_CS "/var/config/lastgood.xml"
const char *const CFG_CS_PATH = CFG_CS;
#define CFG_HS "/var/config/lastgood_hs.xml"

struct onu_identity ident;
struct onu_vlan_cfg vlanCfg;

/* `omci_app`'s command line is built by /etc/runomci.sh, and the LOID rule
 * there is not "use LOID":
 *
 *     if (LOID_OLD == LOID)  use LOID;  else  use LOID_OLD;
 *
 * which is to say the OLD value wins whenever the two disagree -- including
 * when LOID_OLD is empty and LOID is not, in which case the vendor passes no
 * -l at all and the new value is silently ignored. A key that is absent
 * counts as empty, as an unset shell variable does.
 *
 * The password follows the LOID, not itself: runomci.sh tests
 * LOID_OLD == LOID again for -w, so with the same LOID the current password
 * is sent, changed or not, and with a different LOID the old one is. Its OLD
 * half lives in the hs file while its current half is in cs, so a
 * `flash default cs` that changes the LOID leaves the old password in force.
 *
 * Reproduced exactly. It is surprising enough that doing the sensible thing
 * instead would make us behave differently from the stick beside us. */
static void pick_loid_value(int same, const char *cur, int curLen,
			    const char *old, int oldLen, char *out, int max)
{
	const char *src = same ? cur : old;
	int len = same ? curLen : oldLen;

	if (len < 0) {
		out[0] = 0;
		return;
	}
	str_copy(out, src, max);
}

/* The paths are an argument so the test can point this at fixtures. Note which
 * file each key comes from: LOID, LOID_PASSWD and LOID_OLD are cs, while
 * LOID_PASSWD_OLD is hs. That is xmlconfig's split, not a typo. */
/* One key from whichever of the two files carries it, the file xmlconfig
 * assigns it to first.
 *
 * xmlconfig's split is where a stick that has only ever run the vendor
 * firmware keeps a key, but it is not where every stick keeps it: isp1 holds
 * GPON_PLOAM_PASSWD and LOID_PASSWD_OLD in the cs file, and a reader that
 * looked in hs alone reported both as missing. flash get and rcS already look
 * in cs; this is the same answer from the other side. A key in neither file is
 * still -1, absent. */
static int cfg_get_either(const char *first, const char *second,
			  const char *key, char *out, int max)
{
	int n = cfg_get(first, key, out, max);

	return n >= 0 ? n : cfg_get(second, key, out, max);
}

/* The identity trial (identity-trial.sh): while CFG_TRIAL_PATH exists, the
 * values it holds are the ones answered, over the store. KEY=value lines,
 * read as odi.conf is; an empty value is an empty value here, not an absent
 * key. LOID and LOID_PASSWD are the values answered, after the LOID_OLD rule
 * above: the trial says what the OLT is to see. The file is on a tmpfs, so a
 * reboot drops the trial and the boot reads the store alone. */
const char *cfg_trial_path = CFG_TRIAL_PATH;

static void trial_overlay(void)
{
	int n;

	n = cfg_odi_get(cfg_trial_path, "GPON_SN", ident.sn, sizeof ident.sn);
	if (n >= 0)
		ident.snLen = n < (int)sizeof ident.sn ? n : (int)sizeof ident.sn - 1;
	n = cfg_odi_get(cfg_trial_path, "GPON_PLOAM_PASSWD", ident.ploam,
			sizeof ident.ploam);
	if (n >= 0)
		ident.ploamLen = n < (int)sizeof ident.ploam ? n : (int)sizeof ident.ploam - 1;
	(void)cfg_odi_get(cfg_trial_path, "LOID", ident.loid, sizeof ident.loid);
	(void)cfg_odi_get(cfg_trial_path, "LOID_PASSWD", ident.loidPwd,
			  sizeof ident.loidPwd);
}

void cfg_load_identity_from(const char *cs, const char *hs)
{
	char loid[64], loidOld[64], pwd[64], pwdOld[64];
	int a, b, same;

	ident.snLen   = cfg_get_either(hs, cs, "GPON_SN", ident.sn, sizeof ident.sn);
	ident.ploamLen = cfg_get_either(hs, cs, "GPON_PLOAM_PASSWD",
					ident.ploam, sizeof ident.ploam);
	a = cfg_get_either(cs, hs, "LOID", loid, sizeof loid);
	b = cfg_get_either(cs, hs, "LOID_OLD", loidOld, sizeof loidOld);
	if (a < 0)
		loid[0] = 0;
	if (b < 0)
		loidOld[0] = 0;
	same = str_eq(loid, loidOld);
	pick_loid_value(same, loid, a, loidOld, b, ident.loid, sizeof ident.loid);
	a = cfg_get_either(cs, hs, "LOID_PASSWD", pwd, sizeof pwd);
	b = cfg_get_either(hs, cs, "LOID_PASSWD_OLD", pwdOld, sizeof pwdOld);
	pick_loid_value(same, pwd, a, pwdOld, b, ident.loidPwd,
			sizeof ident.loidPwd);
	trial_overlay();
	ident.loaded = 1;
}

void cfg_load_identity(void)
{
	cfg_load_identity_from(CFG_CS, CFG_HS);
}

/* -------------------------------------------------------- the manual VLAN
 *
 * The VLAN a service carries is NOT in class 171. Both our sticks keep it in
 * the config store, and the extended-VLAN table the OLT sets carries the "no
 * VID" sentinel (4096) in every treatment field:
 *
 *     VLAN_CFG_TYPE      1
 *     VLAN_MANU_MODE     1
 *     VLAN_MANU_TAG_VID  10 on isp2, 11 on isp1
 *     VLAN_MANU_TAG_PRI  0
 *
 * and each stick's live bridge connection adds exactly its own VID. So the ONU
 * is tagging locally -- `OMCI_GenTrafficRule` reaches a feature plugin twice,
 * and isp2's OMCI_CUSTOM_BDP selects `cf_sfu_report_veip` -- and reproducing
 * the service is a matter of reading four keys rather than of rebuilding the
 * generator.
 *
 * `mode` is reported separately from the VID so a caller can tell "manual mode
 * is off" from "manual mode is on and the VID happens to be 0".
 */
static int cfg_num(const char *path, const char *key, int missing)
{
	char buf[16];
	int n = cfg_get(path, key, buf, sizeof buf);
	int v = 0, i = 0;

	if (n <= 0)
		return missing;
	for (; buf[i] >= '0' && buf[i] <= '9'; i++)
		v = v * 10 + (buf[i] - '0');
	return i ? v : missing;
}

/* The manual tag is applied only in the one combination the vendor stack
 * applies it in. /etc/runomci.sh passes `-iot_vt 1 -iot_vm 1 -iot_vid V
 * -iot_pri P` only when VLAN_CFG_TYPE is 1, VLAN_MANU_MODE is 1, and both the
 * VID and the priority are present; every other combination hands omci_app the
 * 65535/255 sentinels, which is no manual tag at all. ISP1 and ISP2 both run
 * 1/1 with a VID and a priority, so the applied path never triggers on
 * either.
 *
 * A priority past 7 (255 is what a factory store holds) does NOT switch the
 * tag off: the stock stack assigns the VID whenever it is handed one (the
 * manual-tagging branch of its connection builder), and only the pair VID
 * 65535 / priority 255, which runomci.sh builds for the other combinations,
 * never matches a service. v1.4.2 to v1.8.0 read 255 as "no tag", and a line
 * whose OLT wants untagged frames tagged with the manual VID (VLAN 30, six
 * upstream flows) then had no way for the router's DHCP to reach the PON.
 * No 802.1p tag carries a priority past 7 and the switch driver already takes
 * one as 0, so it is 0 here, which is also what an OLT's own untagged-add
 * rule asks for. */
#define VLAN_CFG_TYPE_MANUAL  1
#define VLAN_MANU_MODE_TAG    1

static void fwdop_load(const char *cs);
static int eapol_load(void);

void cfg_load_vlan_from(const char *cs)
{
	int pri;

	vlanCfg.mode = cfg_num(cs, "VLAN_MANU_MODE", 0);
	vlanCfg.type = cfg_num(cs, "VLAN_CFG_TYPE", 0);
	vlanCfg.vid  = cfg_num(cs, "VLAN_MANU_TAG_VID", -1);
	pri          = cfg_num(cs, "VLAN_MANU_TAG_PRI", -1);
	vlanCfg.pri  = pri < 0 || pri > 7 ? 0 : pri;
	vlanCfg.manual = vlanCfg.type == VLAN_CFG_TYPE_MANUAL
		&& vlanCfg.mode == VLAN_MANU_MODE_TAG
		&& vlanCfg.vid >= 0 && pri >= 0;
	fwdop_load(cs);
	vlanCfg.eapol = (uint8_t)eapol_load();
	vlanCfg.loaded = 1;
}

/* The VID the connection builder tags with, or -1 for none. The one place the
 * gate above is applied, so the builder and `omcli bridge` cannot disagree. */
int cfg_manual_vid(void)
{
	if (!vlanCfg.loaded)
		cfg_load_vlan();
	return vlanCfg.manual ? vlanCfg.vid : -1;
}

/* Whether the OLT's class 171 may drive the tagging: the stock firmware does
 * it with every VLAN_CFG_TYPE but 1 (type 1 is the manual tag, and with mode
 * 0 the router tags: neither looks at class 171). The one place this gate is
 * applied, beside cfg_manual_vid(). */
int cfg_olt_tagging(void)
{
	if (!vlanCfg.loaded)
		cfg_load_vlan();
	return vlanCfg.type != VLAN_CFG_TYPE_MANUAL;
}

/* VLAN_EAPOL_UNTAGGED in CFG_ODI_PATH, odi-only: 1 lets 802.1X EAPOL frames
 * (EtherType 0x888e) cross the stick untagged while it adds the manual tag to
 * everything else. Anything else, or absent, is off, and then no rule is built
 * that differs from the manual tag alone. Read with the VLAN keys
 * (cfg_load_vlan_from()) and held in vlanCfg with them, so a rebuild the OLT
 * triggers keeps the value omcid runs with; an edited file takes effect on
 * the reload (SIGHUP, `apply.sh omci`), which sees the change in
 * cfg_snap_diff() and puts the old value back with the rest when it cannot
 * re-register. */
static int eapol_load(void)
{
	char v[4];

	return cfg_odi_get(CFG_ODI_PATH, "VLAN_EAPOL_UNTAGGED", v, sizeof v) == 1 &&
	       v[0] == '1';
}

int cfg_eapol_untagged(void)
{
	if (!vlanCfg.loaded)
		cfg_load_vlan();
	return vlanCfg.eapol;
}

void cfg_load_vlan(void)
{
	cfg_load_vlan_from(CFG_CS);
}

void cfg_show_vlan(void)
{
	if (!vlanCfg.loaded)
		cfg_load_vlan();
	out_fmt("vlan mode   %d (manual tagging %s)\n", (long)vlanCfg.mode,
		vlanCfg.mode ? "on" : "off");
	out_fmt("vlan type   %d\n", (long)vlanCfg.type);
	if (vlanCfg.vid < 0)
		out("vlan vid    (not in the store)\n");
	else
		out_fmt("vlan vid    %d\n", (long)vlanCfg.vid);
	out_fmt("vlan pri    %d\n", (long)vlanCfg.pri);
	if (vlanCfg.manual)
		out_fmt("manual tag  applied: vid %d pri %d\n",
			(long)vlanCfg.vid, (long)vlanCfg.pri);
	else
		out("manual tag  not applied (needs type 1, mode 1, a vid and a pri)\n");
	/* The two ways a service can carry its VLAN (docs/SETTINGS.md,
	 * "VLAN handling"): the stick adds the manual tag to untagged frames,
	 * or it adds none and the router tags, every tag passing through as
	 * the OLT provisioned it (FwdOp of class 84 still applies). */
	if (vlanCfg.manual)
		out_fmt("handling    stick tags: vid %d added to untagged frames\n",
			(long)vlanCfg.vid);
	else
		out("handling    transparent: no tag added or removed, the router tags\n");
	if (vlanCfg.nfwdop < 0)
		out("fwdop       RTK_DEVINFO_SPECVER malformed, ignored\n");
	for (int k = 0; k < vlanCfg.nfwdop; k++)
		out_fmt("fwdop       84/%04x built as 0x%02x (RTK_DEVINFO_SPECVER)\n",
			(long)vlanCfg.fwdop[k].inst, (long)vlanCfg.fwdop[k].code);
	if (vlanCfg.eapol)
		out(vlanCfg.manual ?
		    "eapol       untagged both ways (VLAN_EAPOL_UNTAGGED)\n" :
		    "eapol       VLAN_EAPOL_UNTAGGED set, no effect without the manual tag\n");
}

/* What is safe to print. The serial is on the label and the OLT ranges on it,
 * so it is not a secret; the PLOAM password and the LOID password are the
 * line's credentials and this is the same discipline the frame dump already
 * applies to classes 148, 157, 340 and 134 -- say whether it is set and how
 * long, never what it is. A log gets pasted. */
void cfg_show_identity(void)
{
	if (!ident.loaded)
		cfg_load_identity();
	out_fmt("serial      %s\n", ident.snLen >= 0 ? ident.sn : "(not in the store)");
	out_fmt("ploam       %s", ident.ploamLen > 0 ? "set, " : "");
	if (ident.ploamLen > 0)
		out_fmt("%d bytes (withheld)", (long)ident.ploamLen);
	else
		out(ident.ploamLen == 0 ? "empty" : "(not in the store)");
	out_char('\n');
	out_fmt("loid        %s\n", ident.loid[0] ? ident.loid : "(none -- nothing would be sent)");
	if (ident.loidPwd[0]) {
		int n = 0;

		while (ident.loidPwd[n])
			n++;
		out_fmt("loid pw     set, %d bytes (withheld)\n", (long)n);
	} else {
		out("loid pw     (none)\n");
	}
	if (!report.loaded)
		cfg_load_report();
	out_fmt("report      %s (" CFG_REPORT_SWITCH ")\n",
		report.on ? "on" : "off -- the keys below are stored, not reported");
	for (int i = 0; i < 2; i++) {
		out_fmt("sw ver %d    ", (long)i);
		if (report.swVerLen[i] > 0)
			out_fmt("%s%s\n", report.swVer[i], report.on ? "" : " (not reported)");
		else
			out("(not in the store: " REPORT_DEFAULT_SW_VER ")\n");
	}
	if (report.modelLen > 0)
		out_fmt("onu model   %s%s\n", report.model, report.on ? "" : " (not reported)");
	else
		out("onu model   (not in the store: the device id)\n");
	if (report.hwVerLen > 0)
		out_fmt("hw version  %s%s\n", report.hwVer, report.on ? "" : " (not reported)");
	else if (report.hwVerLen == -2)
		out("hw version  (ONU_HW_VERSION ignored: more than 14 characters or "
		    "not printable; the device id)\n");
	else
		out("hw version  (not in odi.conf: the device id)\n");
	if (report.omccVer >= 0)
		out_fmt("omcc ver    %d%s\n", (long)report.omccVer, report.on ? "" : " (not reported)");
	else
		out("omcc ver    (not in the store: 128)\n");
	if (report.productCode >= 0)
		out_fmt("prod code   %d%s\n", (long)report.productCode, report.on ? "" : " (not reported)");
	else
		out("prod code   (not in the store: the captured default)\n");
}

/* ------------------------------------------------ what the OLT is told
 *
 * Six keys describe this ONU to the OLT, in three managed entities:
 *
 *     OMCI_SW_VER1, OMCI_SW_VER2   software image (class 7) instance 0 and 1,
 *                                  attribute 1 Version, 14 bytes
 *     GPON_ONU_MODEL               ONU2-G (class 257) attribute 1, Equipment
 *                                  id, 20 bytes -- "IGD" on the vendor stack,
 *                                  the same string both sticks store
 *     ONU_HW_VERSION               ONU-G (class 256) attribute 2, Version, 14
 *                                  bytes: the hardware version some OLTs
 *                                  whitelist. An odi-only key, in odi.conf,
 *                                  not the XML store
 *     OMCC_VER                     ONU2-G attribute 2, one byte
 *     OMCI_VENDOR_PRODUCT_CODE     ONU2-G attribute 3, two bytes
 *
 * An empty or absent key keeps what omcid has always answered: "0.0.0", the
 * device id (both for ONU2-G and ONU-G), 0x80 and product code 15 -- the
 * stock firmware's own default for OMCI_VENDOR_PRODUCT_CODE (the built-in
 * MIB table of bin/configd), so every stick of that firmware reports it.
 * So does an ONU_HW_VERSION that does not fit the attribute: more than 14
 * characters, or anything outside printable ASCII, is ignored whole rather
 * than cut, since a truncated version is a different version to a whitelist.
 *
 * Reported only while CFG_REPORT_SWITCH exists, and that is not caution for
 * its own sake. Both our sticks already carry the five XML keys, written by the vendor
 * firmware (it rewrites OMCI_SW_VER1/2 itself as it boots and flashes), so
 * honouring them unconditionally would change what both OLTs see today --
 * the software version from "0.0.0" to the vendor string and the equipment id
 * from the device id to "IGD" -- on an image whose point is that nothing
 * changes by default. With the switch absent the values are loaded and shown
 * by `omcli ident`, and nothing else. */
struct onu_report report;

static int cfg_num_str(const char *s, int n, int max)
{
	int v = 0;

	if (n <= 0)
		return -1;
	for (int i = 0; i < n; i++) {
		if (s[i] < '0' || s[i] > '9')
			return -1;
		v = v * 10 + (s[i] - '0');
		if (v > max)
			return -1;
	}
	return v;
}

void cfg_load_report_from(const char *cs, const char *hs, const char *sw,
			  const char *odi)
{
	char buf[32];
	int n;

	for (int i = 0; i < 2; i++) {
		n = cfg_get_either(cs, hs, i ? "OMCI_SW_VER2" : "OMCI_SW_VER1",
				   report.swVer[i], sizeof report.swVer[i]);
		report.swVerLen[i] = n;
	}
	report.modelLen = cfg_get_either(hs, cs, "GPON_ONU_MODEL",
					 report.model, sizeof report.model);
	n = cfg_get_either(cs, hs, "OMCC_VER", buf, sizeof buf);
	report.omccVer = cfg_num_str(buf, n, REPORT_OMCC_VER_MAX);
	n = cfg_get_either(cs, hs, "OMCI_VENDOR_PRODUCT_CODE", buf, sizeof buf);
	report.productCode = cfg_num_str(buf, n, REPORT_PRODUCT_CODE_MAX);
	n = cfg_odi_get(odi, "ONU_HW_VERSION", buf, sizeof buf);
	report.hwVerLen = n > 0 ? n : -1;
	if (n > REPORT_HW_VER_LEN)
		report.hwVerLen = -2;
	for (int i = 0; report.hwVerLen > 0 && i < n; i++)
		if (buf[i] < 0x20 || buf[i] > 0x7e)
			report.hwVerLen = -2;
	for (int i = 0; i <= REPORT_HW_VER_LEN; i++)
		report.hwVer[i] = (report.hwVerLen > 0 && i < n) ? buf[i] : 0;
	{
		long fd = sys_open(sw, 0);

		report.on = fd >= 0;
		if (fd >= 0)
			sys_close((int)fd);
	}
	report.loaded = 1;
}

void cfg_load_report(void)
{
	cfg_load_report_from(CFG_CS, CFG_HS, CFG_REPORT_SWITCH, CFG_ODI_PATH);
}

/* The software-image version to answer for instance `inst`: the key, or the
 * "0.0.0" omcid has always answered. */
const char *report_sw_ver(uint16_t inst)
{
	if (!report.loaded)
		cfg_load_report();
	if (report.on && inst < 2 && report.swVerLen[inst] > 0)
		return report.swVer[inst];
	return REPORT_DEFAULT_SW_VER;
}

/* ONU-G Version: ONU_HW_VERSION when reported, else the device id omcid has
 * always answered. */
const char *report_hw_ver(void)
{
	if (!report.loaded)
		cfg_load_report();
	if (report.on && report.hwVerLen > 0)
		return report.hwVer;
	return (const char *)devid;
}

/* ------------------------------------------------ OMCI_VEIP_SLOT_ID
 *
 * The stock key, read from the config store as the stock firmware reads it:
 * a decimal slot number, one byte, which the stock OMCI stack applies only
 * when bit 0x100 of OMCI_CUSTOM_ME (apply customized VEIP slot id) is set.
 * Unset, the VEIP lives in slot 6, which is the stock default. A slot that
 * would collide with another slot of this ONU (1, the PPTP Ethernet UNI, and
 * 128, the ANI) or is not a slot number (0, 255) is not used. */
int veip_slot_key = -1;
int veip_slot = VEIP_SLOT_DEFAULT;

void cfg_load_veip_slot(void)
{
	char v[16];
	int n = cfg_get(CFG_CS_PATH, "OMCI_VEIP_SLOT_ID", v, sizeof v);
	int slot = 0, i = 0;

	veip_slot_key = -1;
	veip_slot = VEIP_SLOT_DEFAULT;
	if (n <= 0)
		return;
	for (; v[i] >= '0' && v[i] <= '9' && slot < 1000; i++)
		slot = slot * 10 + (v[i] - '0');
	/* 255 is the stock "not set": the factory value of the key on both our
	 * lines, and the value omci_app's main() starts the slot at before it
	 * reads -slot_veip. Unset, so nothing to say about it. */
	if (i > 0 && !v[i] && slot == 255)
		return;
	if (i == 0 || v[i] || slot < 2 || slot > 254 || slot == 128) {
		out("OMCI_VEIP_SLOT_ID ignored: not a usable slot (2 to 254, not 128)\n");
		return;
	}
	veip_slot_key = slot;
	cfg_load_custom_me();
	if (custom_me & CUSTOM_ME_VEIP_SLOT_ID)
		veip_slot = slot;
}

/* ------------------------------------------------ OMCI_PRIVATE_ME
 *
 * odi.conf, odi-only: 0 leaves out the three vendor-private entities the ONU
 * creates for itself (350, 370 and 373, which the stock firmware creates
 * too); anything else, an absent key included, keeps them. */
int private_me = 1;

/* ------------------------------------------------ OMCI_VEIP
 *
 * odi.conf, odi-only: 0 creates no VEIP (and nothing of its slot); anything
 * else, an absent key included, creates it. A separate key, not a value of
 * the stock OMCI_VEIP_SLOT_ID: the stock firmware reads that key as a slot
 * number only (0 would be slot 0 there), and the stick must stay valid when it
 * falls back to the stock slot. */
int veip_on = 1;

/* The stock key, decimal, as runomci.sh passes it to omci_app (-cme). */
int custom_me;

void cfg_load_custom_me(void)
{
	char v[16];
	int n = cfg_get(CFG_CS_PATH, "OMCI_CUSTOM_ME", v, sizeof v), me = 0;

	for (int i = 0; n > 0 && v[i] >= '0' && v[i] <= '9' && me < 100000000; i++)
		me = me * 10 + (v[i] - '0');
	custom_me = me;
}

/* Likewise OMCI_CUSTOM_MCAST (runomci.sh -cmc); omcid.h says which bit acts. */
int custom_mcast;

void cfg_load_custom_mcast(void)
{
	char v[16];
	int n = cfg_get(CFG_CS_PATH, "OMCI_CUSTOM_MCAST", v, sizeof v), mc = 0;

	for (int i = 0; n > 0 && v[i] >= '0' && v[i] <= '9' && mc < 100000000; i++)
		mc = mc * 10 + (v[i] - '0');
	custom_mcast = mc;
}

void cfg_load_veip_on(void)
{
	char v[4];

	veip_on = !(cfg_odi_get(CFG_ODI_PATH, "OMCI_VEIP", v, sizeof v) == 1
		    && v[0] == '0');
}

void cfg_load_private_me(void)
{
	char v[4];

	private_me = !(cfg_odi_get(CFG_ODI_PATH, "OMCI_PRIVATE_ME", v, sizeof v) == 1
		       && v[0] == '0');
}

/* ------------------------------------------------ OMCI_POTS_PORTS
 *
 * odi.conf, odi-only: the phone (POTS) ports, 1 or 2; anything else, an
 * absent key included, is 0, no phone service. The stick has no phone
 * socket: the ports are the SIP lines an OLT may provision for the router
 * behind it to register. A change re-registers: the entities the ONU
 * creates, and the classes it accepts, change with it.
 *
 * The ports live in a slot of their own, a circuit pack of type POTS. Not
 * a fixed slot: the VEIP's moves with OMCI_VEIP_SLOT_ID, so a fixed one
 * could collide with it. The first slot from 2 up that the ONU does not
 * already use (1 the Ethernet UNI, 128 the ANI, the VEIP's, the slot the
 * VEIP key names even while the key is not in force, and 6, the stock VEIP
 * slot, whose ids the generated defaults carry), unless OMCI_POTS_SLOT_ID
 * names a free one. */
int pots_ports;
int pots_slot_key = -1;
int pots_slot = 2;

static int slot_taken(int s)
{
	return s == 1 || s == 128 || s == VEIP_SLOT_DEFAULT || s == veip_slot ||
	       s == veip_slot_key;
}

void cfg_load_pots_ports(void)
{
	char v[16];
	int n, slot = 0, i = 0;

	pots_ports = 0;
	if (cfg_odi_get(CFG_ODI_PATH, "OMCI_POTS_PORTS", v, sizeof v) == 1 &&
	    v[0] >= '1' && v[0] <= '0' + POTS_PORTS_MAX)
		pots_ports = v[0] - '0';

	pots_slot_key = -1;
	n = cfg_odi_get(CFG_ODI_PATH, "OMCI_POTS_SLOT_ID", v, sizeof v);
	if (n > 0) {
		for (; v[i] >= '0' && v[i] <= '9' && slot < 1000; i++)
			slot = slot * 10 + (v[i] - '0');
		if (i == 0 || v[i] || slot < 2 || slot > 254 || slot_taken(slot))
			out("OMCI_POTS_SLOT_ID ignored: not a free slot (2 to 254, not 6, 128 or the VEIP's)\n");
		else
			pots_slot_key = slot;
	}
	if (pots_slot_key >= 0) {
		pots_slot = pots_slot_key;
	} else {
		for (pots_slot = 2; slot_taken(pots_slot); pots_slot++)
			;
	}
}

/* ------------------------------------------------ OMCI_UNKNOWN_ME_OK
 *
 * Only the value 1 turns it on: anything else, an absent key included, keeps
 * the "unknown entity" answer. */
void cfg_load_unknown_me(void)
{
	char v[4];

	unknown_me_ok = cfg_odi_get(CFG_ODI_PATH, "OMCI_UNKNOWN_ME_OK",
				    v, sizeof v) == 1 && v[0] == '1';
}

/* ------------------------------------------------ OMCI_FIXED_ME
 *
 * Classes omcid has no model for, answered with bytes the owner chooses: an
 * OLT that reads vendor-specific entities of its own ONUs gets the values it
 * expects, which the owner copies from a native ONU. The key holds up to
 * FIXED_ME_ENTRIES entries separated by spaces or commas, in two forms.
 *
 * CLASS:HEX answers a Get (and a Get current data) of ANY instance of the
 * class with the bytes, under the mask the OLT asked for. The entity is not
 * in the MIB upload. The class is decimal (1..65535), the bytes hex (an
 * optional 0x, 1 to 25 bytes, what fits a baseline Get response).
 *
 * CLASS:INSTANCE:ATTR:HEX is one attribute of one entity: the instance is
 * decimal or 0x hex (0..65535), the attribute 1..16, the value hex as above.
 * Entries that share CLASS:INSTANCE make one entity with several attributes.
 * That entity is listed in the MIB upload, a Get of it answers the
 * attributes asked for that it has, and a Get of any other instance of the
 * class answers unknown instance:
 *
 *     OMCI_FIXED_ME=200:0x0011aa,201:0x8001:1:01020304
 *
 * In odi.conf, so the values live on the stick only. Anything malformed --
 * a bad entry, the same class twice in the first form, the same
 * class/instance/attribute twice, a class in both forms, too many entries --
 * turns the whole key off, so a typo cannot answer half of what the owner
 * meant. */
struct fixed_me_set fixed_me;

/* Hex bytes in v[0..n), an optional 0x first: 1 to FIXED_ME_MAX of them. */
static int fixed_me_hex(const char *v, int n, struct fixed_me *e)
{
	int i = 0;

	if (n >= 2 && v[0] == '0' && (v[1] == 'x' || v[1] == 'X'))
		i = 2;
	if (n <= i || (n - i) % 2 || (n - i) / 2 > FIXED_ME_MAX)
		return -1;
	e->len = 0;
	for (; i < n; i += 2) {
		int hi = hex_nib(v[i]), lo = hex_nib(v[i + 1]);

		if (hi < 0 || lo < 0)
			return -1;
		e->data[e->len++] = (uint8_t)(hi << 4 | lo);
	}
	return 0;
}

/* A number in v[0..n): decimal of up to maxdig digits, or, when hex_ok, 0x
 * and up to 4 hex digits. -1 when it is neither or above 65535. */
static int fixed_me_num(const char *v, int n, int maxdig, int hex_ok)
{
	int x = 0, i = 0;

	if (hex_ok && n >= 2 && v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) {
		if (n < 3 || n > 6)
			return -1;
		for (i = 2; i < n; i++) {
			int d = hex_nib(v[i]);

			if (d < 0)
				return -1;
			x = x * 16 + d;
		}
		return x;
	}
	if (n < 1 || n > maxdig)
		return -1;
	for (; i < n; i++) {
		if (v[i] < '0' || v[i] > '9')
			return -1;
		x = x * 10 + (v[i] - '0');
	}
	return x > 65535 ? -1 : x;
}

static int fixed_me_entry(const char *v, int n, struct fixed_me *e)
{
	int colon[3], nc = 0, cls, inst = 0, attr = 0, hexat;

	for (int i = 0; i < n; i++)
		if (v[i] == ':') {
			if (nc == 3)
				return -1;
			colon[nc++] = i;
		}
	if (nc != 1 && nc != 3)
		return -1;
	/* Class and attribute are decimal; 0x is for the instance and the bytes. */
	cls = fixed_me_num(v, colon[0], 5, 0);
	if (cls < 1)
		return -1;
	if (nc == 3) {
		inst = fixed_me_num(v + colon[0] + 1, colon[1] - colon[0] - 1, 5, 1);
		attr = fixed_me_num(v + colon[1] + 1, colon[2] - colon[1] - 1, 2, 0);
		if (inst < 0 || attr < 1 || attr > 16)
			return -1;
	}
	hexat = colon[nc - 1] + 1;
	if (fixed_me_hex(v + hexat, n - hexat, e) < 0)
		return -1;
	e->cls = (uint16_t)cls;
	e->inst = (uint16_t)inst;
	e->attr = (uint8_t)attr;
	return 0;
}

void cfg_load_fixed_me(void)
{
	static char v[FIXED_ME_ENTRIES * 80];
	struct fixed_me_set s;
	int n, i = 0;

	fixed_me.n = 0;
	s.n = 0;
	n = cfg_odi_get(CFG_ODI_PATH, "OMCI_FIXED_ME", v, sizeof v);
	if (n <= 0 || n >= (int)sizeof v - 1)
		return;
	while (i < n) {
		int start, k;
		const struct fixed_me *x;

		while (i < n && (v[i] == ' ' || v[i] == ','))
			i++;
		if (i >= n)
			break;
		start = i;
		while (i < n && v[i] != ' ' && v[i] != ',')
			i++;
		if (s.n >= FIXED_ME_ENTRIES ||
		    fixed_me_entry(v + start, i - start, &s.e[s.n]) < 0)
			return;
		x = &s.e[s.n];
		for (k = 0; k < s.n; k++) {
			const struct fixed_me *y = &s.e[k];

			if (y->cls != x->cls)
				continue;
			/* Both forms of one class, or one slot twice. */
			if (!y->attr || !x->attr ||
			    (y->inst == x->inst && y->attr == x->attr))
				return;
		}
		s.n++;
	}
	fixed_me = s;
}

/* ------------------------------------------------ RTK_DEVINFO_SPECVER
 *
 * The override of the class 84 forward operation (FwdOp) on the stock firmware:
 * its "VLAN Forward Operation" page stores EntityId[:FwdOp] entries,
 * separated by commas, in this otherwise unused device-info key, and
 * fix_vlan_fwdop.sh writes each code over the attribute once the OLT has
 * provisioned the port. An entry without a code gets 0x02, as there:
 *
 *     RTK_DEVINFO_SPECVER=0x1102:0x00,0x110b
 *
 * Some lines need it. On one (odi-oss #301) the OLT filters every GEM-side
 * port with FwdOp 0x10 and the service VID, while its EAPOL frames come down
 * priority-tagged (VID 0): the filter drops every one, and 802.1X never
 * completes. 0x00 lets them through.
 *
 * Here the attribute keeps what the OLT wrote, so a Get answers the OLT with
 * its own value; only the bridge rules are built from the override
 * (cfg_fwdop()). The same key in both slots means the same thing. Instances
 * are decimal or 0x hex (0..65535), codes 0x hex or decimal (0..255).
 * Anything malformed -- a bad entry, the same instance twice, more than
 * FWDOP_OVR_MAX entries -- turns the whole key off, as OMCI_FIXED_ME does. */
static void fwdop_load(const char *cs)
{
	char v[FWDOP_OVR_MAX * 16];
	int len = cfg_get(cs, "RTK_DEVINFO_SPECVER", v, sizeof v);
	int i = 0, n = 0;

	vlanCfg.nfwdop = 0;
	if (len <= 0)
		return;
	if (len >= (int)sizeof v - 1) {
		vlanCfg.nfwdop = -1;
		return;
	}
	while (i < len) {
		int s = i, colon = -1, inst, code = 0x02;

		if (v[i] == ',' || v[i] == ' ') {
			i++;
			continue;
		}
		for (; i < len && v[i] != ',' && v[i] != ' '; i++)
			if (v[i] == ':' && colon < 0)
				colon = i;
		if (colon < 0) {
			inst = fixed_me_num(v + s, i - s, 5, 1);
		} else {
			inst = fixed_me_num(v + s, colon - s, 5, 1);
			code = fixed_me_num(v + colon + 1, i - colon - 1, 3, 1);
		}
		if (inst < 0 || code < 0 || code > 0xff || n == FWDOP_OVR_MAX) {
			vlanCfg.nfwdop = -1;
			return;
		}
		for (int k = 0; k < n; k++)
			if (vlanCfg.fwdop[k].inst == inst) {
				vlanCfg.nfwdop = -1;
				return;
			}
		vlanCfg.fwdop[n].inst = (uint16_t)inst;
		vlanCfg.fwdop[n].code = (uint8_t)code;
		n++;
	}
	vlanCfg.nfwdop = n;
}

/* The forward operation the bridge rules of class 84 instance `inst` are
 * built with: the override when the key names the instance, else the code
 * the OLT wrote. */
uint32_t cfg_fwdop(uint16_t inst, uint32_t olt_code)
{
	if (!vlanCfg.loaded)
		cfg_load_vlan();
	for (int k = 0; k < vlanCfg.nfwdop; k++)
		if (vlanCfg.fwdop[k].inst == inst)
			return vlanCfg.fwdop[k].code;
	return olt_code;
}

/* The any-instance entry of a class, if the class is in the first form. */
const struct fixed_me *fixed_me_find(uint16_t cls)
{
	for (int i = 0; i < fixed_me.n; i++)
		if (fixed_me.e[i].cls == cls && !fixed_me.e[i].attr)
			return &fixed_me.e[i];
	return 0;
}

/* Whether the class is configured per instance. */
int fixed_me_per_inst(uint16_t cls)
{
	for (int i = 0; i < fixed_me.n; i++)
		if (fixed_me.e[i].cls == cls && fixed_me.e[i].attr)
			return 1;
	return 0;
}

/* Whether the entity at this instance of the class is configured. */
int fixed_me_has_inst(uint16_t cls, uint16_t inst)
{
	for (int i = 0; i < fixed_me.n; i++)
		if (fixed_me.e[i].cls == cls && fixed_me.e[i].attr &&
		    fixed_me.e[i].inst == inst)
			return 1;
	return 0;
}

/* One attribute of one per-instance entity, if configured. */
const struct fixed_me *fixed_me_attr(uint16_t cls, uint16_t inst, unsigned attr)
{
	for (int i = 0; i < fixed_me.n; i++)
		if (fixed_me.e[i].cls == cls && fixed_me.e[i].attr == attr &&
		    fixed_me.e[i].inst == inst)
			return &fixed_me.e[i];
	return 0;
}

/* The upload records of the per-instance entities, one entity after another
 * in the order its first attribute appears in the key. A record holds the
 * attributes of one entity in attribute order, as many as fit in the 26
 * bytes after the class, instance and mask; the next record continues with
 * the rest, each with its own mask. Returns how many records there are
 * (want -1 only counts); a want below that fills body with the record's
 * class, instance, mask and values, and *len with their length. */
int fixed_me_upload(int want, uint8_t *body, uint16_t *len)
{
	int rec = 0;

	for (int i = 0; i < fixed_me.n; i++) {
		const struct fixed_me *g = &fixed_me.e[i];
		uint16_t n = 6, mask = 0;
		int seen = 0, open = 0;

		if (!g->attr)
			continue;
		for (int k = 0; k < i; k++)
			if (fixed_me.e[k].attr && fixed_me.e[k].cls == g->cls &&
			    fixed_me.e[k].inst == g->inst)
				seen = 1;
		if (seen)
			continue;
		for (unsigned a = 1; a <= 16; a++) {
			const struct fixed_me *x = fixed_me_attr(g->cls, g->inst, a);

			if (!x)
				continue;
			if (open && n + x->len > FIXED_ME_UPLOAD) {
				if (rec == want) {
					nl_put16(body + 4, mask);
					*len = n;
					return rec + 1;
				}
				rec++;
				open = 0;
			}
			if (!open) {
				n = 6;
				mask = 0;
				open = 1;
				if (rec == want) {
					nl_put16(body + 0, g->cls);
					nl_put16(body + 2, g->inst);
				}
			}
			if (rec == want)
				for (int b = 0; b < x->len; b++)
					body[n + b] = x->data[b];
			n = (uint16_t)(n + x->len);
			mask |= (uint16_t)(1u << (16 - a));
		}
		if (open) {
			if (rec == want) {
				nl_put16(body + 4, mask);
				*len = n;
				return rec + 1;
			}
			rec++;
		}
	}
	return rec;
}

/* ------------------------------------------------------- config reload
 *
 * Every key omcid holds, read again, and the comparison that decides what a
 * SIGHUP does (reload.c). What is compared is what the OLT or a connection
 * would see, not the bytes on disk: a key that is absent and one that is
 * empty are the same to the OLT, the report keys count only while the
 * identity switch is on (with it off they are stored and shown, never sent),
 * and a LOID is the one the vendor rule picks (LOID_OLD wins). */
void cfg_load_all(void)
{
	cfg_load_identity();
	cfg_load_vlan();
	cfg_load_report();
	cfg_load_unknown_me();
	cfg_load_fixed_me();
	cfg_load_veip_slot();
	cfg_load_private_me();
	cfg_load_veip_on();
	cfg_load_custom_me();
	cfg_load_custom_mcast();
	cfg_load_pots_ports();
	aut_rebuild();
}

void cfg_snap_take(struct cfg_snap *s)
{
	s->ident = ident;
	s->vlan = vlanCfg;
	s->report = report;
	s->unknown_me_ok = unknown_me_ok;
	s->fixed = fixed_me;
	s->veip_slot_key = veip_slot_key;
	s->veip_slot = veip_slot;
	s->private_me = private_me;
	s->veip_on = veip_on;
	s->custom_me = custom_me;
	s->custom_mcast = custom_mcast;
	s->pots_ports = pots_ports;
	s->pots_slot_key = pots_slot_key;
	s->pots_slot = pots_slot;
}

void cfg_snap_restore(const struct cfg_snap *s)
{
	ident = s->ident;
	vlanCfg = s->vlan;
	report = s->report;
	unknown_me_ok = s->unknown_me_ok;
	fixed_me = s->fixed;
	veip_slot_key = s->veip_slot_key;
	veip_slot = s->veip_slot;
	private_me = s->private_me;
	veip_on = s->veip_on;
	custom_me = s->custom_me;
	custom_mcast = s->custom_mcast;
	pots_ports = s->pots_ports;
	pots_slot_key = s->pots_slot_key;
	pots_slot = s->pots_slot;
	aut_rebuild();
}

/* Two optional strings are the same when both are unset or empty, or both
 * hold the same text. */
static int opt_same(const char *a, int alen, const char *b, int blen)
{
	int ea = alen > 0, eb = blen > 0;

	if (!ea || !eb)
		return ea == eb;
	return str_eq(a, b);
}

unsigned cfg_snap_diff(const struct cfg_snap *o, const struct cfg_snap *n)
{
	unsigned m = 0;

	if (!opt_same(o->ident.sn, o->ident.snLen, n->ident.sn, n->ident.snLen))
		m |= CFGD_SN;
	if (!opt_same(o->ident.ploam, o->ident.ploamLen, n->ident.ploam,
		      n->ident.ploamLen))
		m |= CFGD_PLOAM;
	if (!str_eq(o->ident.loid, n->ident.loid))
		m |= CFGD_LOID;
	if (!str_eq(o->ident.loidPwd, n->ident.loidPwd))
		m |= CFGD_LOID_PWD;
	if (o->report.on != n->report.on)
		m |= CFGD_SWITCH;
	if (o->report.on || n->report.on) {
		if (!opt_same(o->report.swVer[0], o->report.swVerLen[0],
			      n->report.swVer[0], n->report.swVerLen[0]))
			m |= CFGD_SW_VER1;
		if (!opt_same(o->report.swVer[1], o->report.swVerLen[1],
			      n->report.swVer[1], n->report.swVerLen[1]))
			m |= CFGD_SW_VER2;
		if (!opt_same(o->report.model, o->report.modelLen,
			      n->report.model, n->report.modelLen))
			m |= CFGD_MODEL;
		if (o->report.omccVer != n->report.omccVer)
			m |= CFGD_OMCC_VER;
		if (o->report.productCode != n->report.productCode)
			m |= CFGD_PRODUCT;
		if (!opt_same(o->report.hwVer, o->report.hwVerLen,
			      n->report.hwVer, n->report.hwVerLen))
			m |= CFGD_HW_VER;
	}
	if (o->unknown_me_ok != n->unknown_me_ok)
		m |= CFGD_UNKNOWN_ME;
	if (o->veip_slot_key != n->veip_slot_key || o->veip_slot != n->veip_slot)
		m |= CFGD_VEIP_SLOT;
	if (o->private_me != n->private_me)
		m |= CFGD_PRIVATE_ME;
	if (o->veip_on != n->veip_on)
		m |= CFGD_VEIP_ON;
	if ((o->custom_me ^ n->custom_me) & CUSTOM_ME_ONU_CAPABILITY)
		m |= CFGD_CUSTOM_ME;
	if (o->pots_ports != n->pots_ports)
		m |= CFGD_POTS_PORTS;
	/* The slot matters only while there are ports in it, before and after:
	 * ports coming or going are the change above, wherever they sit. */
	if (o->pots_ports && n->pots_ports &&
	    (o->pots_slot_key != n->pots_slot_key || o->pots_slot != n->pots_slot))
		m |= CFGD_POTS_SLOT;
	if (o->fixed.n != n->fixed.n)
		m |= CFGD_FIXED_ME;
	else
		for (int i = 0; i < n->fixed.n && !(m & CFGD_FIXED_ME); i++) {
			const struct fixed_me *a = &o->fixed.e[i], *b = &n->fixed.e[i];

			if (a->cls != b->cls || a->len != b->len ||
			    a->inst != b->inst || a->attr != b->attr)
				m |= CFGD_FIXED_ME;
			for (int k = 0; k < b->len && !(m & CFGD_FIXED_ME); k++)
				if (a->data[k] != b->data[k])
					m |= CFGD_FIXED_ME;
		}
	if ((o->custom_mcast ^ n->custom_mcast) & CUSTOM_MCAST_DS_UNTAGGED)
		m |= CFGD_CUSTOM_MCAST;
	if (o->vlan.eapol != n->vlan.eapol)
		m |= CFGD_EAPOL;
	if (o->vlan.nfwdop != n->vlan.nfwdop)
		m |= CFGD_FWDOP;
	for (int k = 0; k < n->vlan.nfwdop && k < o->vlan.nfwdop; k++)
		if (o->vlan.fwdop[k].inst != n->vlan.fwdop[k].inst ||
		    o->vlan.fwdop[k].code != n->vlan.fwdop[k].code)
			m |= CFGD_FWDOP;
	if (o->vlan.type != n->vlan.type)
		m |= CFGD_VLAN_TYPE;
	if (o->vlan.mode != n->vlan.mode)
		m |= CFGD_VLAN_MODE;
	if (o->vlan.vid != n->vlan.vid)
		m |= CFGD_VLAN_VID;
	/* An absent priority reads as 0, so its presence shows only in whether
	 * the manual tag applies at all (type 1, mode 1, VID and priority). */
	if (o->vlan.pri != n->vlan.pri ||
	    (o->vlan.manual != n->vlan.manual && !(m & CFGD_VLAN)))
		m |= CFGD_VLAN_PRI;
	return m;
}

void cfg_diff_names(unsigned mask, char *out, int max)
{
	static const struct { unsigned bit; const char *name; } names[] = {
		{ CFGD_SN, "GPON_SN" },
		{ CFGD_PLOAM, "GPON_PLOAM_PASSWD" },
		{ CFGD_LOID, "LOID" },
		{ CFGD_LOID_PWD, "LOID_PASSWD" },
		{ CFGD_SW_VER1, "OMCI_SW_VER1" },
		{ CFGD_SW_VER2, "OMCI_SW_VER2" },
		{ CFGD_MODEL, "GPON_ONU_MODEL" },
		{ CFGD_OMCC_VER, "OMCC_VER" },
		{ CFGD_PRODUCT, "OMCI_VENDOR_PRODUCT_CODE" },
		{ CFGD_HW_VER, "ONU_HW_VERSION" },
		{ CFGD_SWITCH, "omci-identity.on" },
		{ CFGD_UNKNOWN_ME, "OMCI_UNKNOWN_ME_OK" },
		{ CFGD_FIXED_ME, "OMCI_FIXED_ME" },
		{ CFGD_VEIP_SLOT, "OMCI_VEIP_SLOT_ID" },
		{ CFGD_PRIVATE_ME, "OMCI_PRIVATE_ME" },
		{ CFGD_VEIP_ON, "OMCI_VEIP" },
		{ CFGD_CUSTOM_ME, "OMCI_CUSTOM_ME" },
		{ CFGD_POTS_PORTS, "OMCI_POTS_PORTS" },
		{ CFGD_POTS_SLOT, "OMCI_POTS_SLOT_ID" },
		{ CFGD_VLAN_TYPE, "VLAN_CFG_TYPE" },
		{ CFGD_VLAN_MODE, "VLAN_MANU_MODE" },
		{ CFGD_VLAN_VID, "VLAN_MANU_TAG_VID" },
		{ CFGD_VLAN_PRI, "VLAN_MANU_TAG_PRI" },
		{ CFGD_CUSTOM_MCAST, "OMCI_CUSTOM_MCAST" },
		{ CFGD_FWDOP, "RTK_DEVINFO_SPECVER" },
		{ CFGD_EAPOL, "VLAN_EAPOL_UNTAGGED" },
	};
	int o = 0;

	for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) {
		if (!(mask & names[i].bit))
			continue;
		if (o && o < max - 1)
			out[o++] = ',';
		for (const char *p = names[i].name; *p && o < max - 1; p++)
			out[o++] = *p;
	}
	out[o < max ? o : max - 1] = 0;
}

/* The serial number as "gponsn" takes it: four ASCII vendor characters and
 * eight hex digits, twelve in all. Anything else is not sent, the driver
 * would refuse it. NULL when the store holds no usable one. */
const char *cfg_sn_label(const struct onu_identity *id)
{
	if (id->snLen != 12)
		return 0;
	for (int i = 4; i < 12; i++)
		if (hex_nib(id->sn[i]) < 0)
			return 0;
	return id->sn;
}
