/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */

/* What the modules of omcid share.
 *
 * omcid was one 3122-line main.c. The split is by what a piece TALKS TO,
 * not by what it is about: the MIB store answers questions about itself,
 * drv.c is the only thing that reaches the driver, apply*.c turn the MIB
 * into driver commands, and the three servers each own one transport.
 * Everything crossing those lines is declared here and nowhere else, and
 * `static` is dropped only where another module uses a symbol.
 */
#ifndef OMCID_H
#define OMCID_H

#include "sys.h"
#include "io.h"
#include "../nl.h"
#include "../omci.h"
#include "omci_mib.h"
#include "../omci_tmpfile.h"
#include "omci_autonomous.h"
#include "omci_drv.h"
#include "omci_gemflow.h"
#include "omci_caps.h"
#include "omci_bdgconn.h"
#include "omci_bridgeport.h"
#include "../omci_cli_proto.h"
#include "../omci_msgq.h"

#define NL_POLL_US     2000    /* see nl_open: this paces the CLI too */
/* The longest a dirty rebuild waits, in seconds, from the first change to the
 * QoS or the bridge connections, when OMCI frames keep arriving less than a
 * second apart (respond/main.c). A multicast change alone is not timed. */
#define REBUILD_MAX_S  5
#define CLI_RETRIES    100
#define CLI_RETRY_NS   (5 * 1000 * 1000)
#define DRV_OPT        0x310au
#define DRV_GET_SN     15
#define DRV_GET_DEVID  4
#define OLT_ACC_CREATE 4
#define OLT_ACC_WRITE  2
#define PRIQ_DEFAULT_WEIGHT 1
#define FLOW_MAX 64                      /* what this device reports, and the
					  * ceiling on what is honoured */
#define SERV_MAX 256                     /* the service-id limit, and the number of
					  * rows omcicli dump srvflow prints */
#define MBPCD_TP_PPTP_ETH_UNI   1
#define MBPCD_TP_IP_HOST        4
#define MBPCD_TP_GEM_IWTP       5
#define MBPCD_TP_VEIP          11
/* Class 130's UnmarkFrmOpt: 0 takes an unmarked frame's P-bit from its
 * DSCP, 1 uses the profile's default P-bit. Only 0 sends the map. */
#define MAP8021P_UNMARKED_DSCP_TO_PBIT 0
#define TCONT_MAX 32
#define V_RULE "=================================\n"
#define V_BANNER "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX\n"

/* ---------------------------------------------------------------- MIB store
 *
 * What the OLT creates and sets has to be remembered, or a get after a set
 * lies and the next MIB upload is empty again. The layout here is this
 * program's own -- attributes packed at their wire widths, in order -- because
 * nothing outside reads it; matching the vendor's row padding would buy
 * nothing.
 *
 * Rows are capped: the entities the line provisions are all 96 bytes or less
 * (the multicast operations profile is the largest at 90, ExtVlanTagOperCfgData
 * the next at 64), and the ones that are not -- LargeString at 400 -- hold
 * the ACS URL and credentials, which this has no reason to keep. A truncated
 * row is marked rather than silently short.
 *
 * The classes that do hold more -- LargeString, whose parts carry the SIP
 * and ACS addresses, and the authentication method with its second username
 * -- are the exception: a bounded side pool gives each such row its full
 * width (see class_wide() in mibstore.c). Only when the pool is full does a
 * row fall back to the 96 bytes and the `truncated` mark.
 *
 * The counts are sized for a data service and two SIP lines at once. ISP1's
 * whole provisioning holds 185 rows; two phone lines add about sixty (per
 * line its voice CTP, SIP user, authentication method, the large strings and
 * network addresses of its servers and number, four PM entities and their
 * thresholds, the voice flow's GEM port and bridge port with their three
 * tables; once, the profiles): about 245, which 256 would leave about ten
 * from full and 320 leaves some seventy rows of margin, at 208 bytes a row.
 * Past it a Create is answered "processing error" and logged ("no room in
 * the MIB"), as it always was. The wide rows: with a SIP agent per line, as
 * some OLTs provision it, each line points at four large strings (proxy,
 * outbound proxy, host part, registrar address) besides its number,
 * voicemail address and authentication method -- seven a line -- and the
 * TR-069 server's two, the IP hosts the OLT sets (134 is wide too) and an
 * extended multicast profile come on top: about nineteen, past the 16 slots
 * the pool started with.
 */
#define MIB_ROWS      320
#define MIB_ROW_MAX   96
#define MIB_WIDE_MAX  384                /* covers the widest entity of the
					  * model, a large string at 376 */
#define MIB_WIDE_ROWS 32

/* ------------------------------------------------------- table attributes
 *
 * Some attributes are not a value but a table of rows, which the OLT sets one
 * row per Set and which accumulate: class 171's VLAN tagging operations, the
 * multicast operations profile's ACLs (309, and 65531, its extended form),
 * the subscriber config's service packages and preview groups (310), the
 * multicast GEM IW TP's address tables (281), the MAC bridge port filter
 * table (49). A flat copy -- whatever was written last -- is one row of it.
 *
 * Each table keeps its rows in a list off the entity's row, every node tagged
 * with its attribute. How a Set adds, replaces or deletes a row is the
 * table's own rule (G.988, and what the stock plugin does; mibstore.c,
 * tbl_defs[]). The flat copy is still written, as the vendor's generic store
 * does; a Get of the attribute answers the whole list (tbl_serialise).
 *
 * New rows go at the head of their list, as every stock plugin does, so row
 * 0 of a Get-Next walk -- and INDEX 0 of class 171's vendor dump -- is the
 * most recently added.
 *
 * Nothing here allocates: a fixed pool, shared by every row, with rows linked
 * through indices. ISP1's two class 171 instances hold three and two rows.
 * Voice is what fills it: one voice service profile carries four tables
 * (tone patterns, tone events, ringing patterns and events), commonly 20 to
 * 40 rows between them, and a dial plan and a SIP response table add tens
 * more, per profile -- past the 128 the pool held. It is now as large as a
 * one-byte index allows (MIB_TBL_NONE ends a list).
 */
#define MIB_TBL_ENTRY 32                 /* the widest row: 65531's ACL, 30 */
#define MIB_TBL_NODES 255
#define MIB_TBL_NONE  0xff

struct mib_tblent {
	uint8_t next;                        /* MIB_TBL_NONE ends the list */
	uint8_t used;
	uint8_t attr;                        /* the table attribute, 1-based */
	uint8_t data[MIB_TBL_ENTRY];
};

struct mib_row {
	uint16_t classId;
	uint16_t inst;
	/* Which attributes the OLT has actually written, same bit order as an
	 * OMCI attribute mask: bit (16 - k) for attribute k.
	 *
	 * A row exists as soon as ONE attribute is set, and the rest of it is
	 * zero -- which is not the same thing as "the value is zero". Without
	 * this mask a single set of ONU-G's AdminState (writable, oltAcc 3)
	 * created the row and every later get of SerialNum, VID or Version
	 * answered zeros: the identity the OLT ranged on, overwritten by an
	 * absence. apply_entity's own comment already states the rule -- an
	 * attribute that was never set is not a value, it is an absence -- and
	 * this is what makes the store obey it. */
	uint16_t written;
	uint8_t used;
	uint8_t truncated;
	uint8_t tbl_head;                    /* MIB_TBL_NONE when empty */
	uint8_t tbl_count;
	uint8_t data[MIB_ROW_MAX];
	/* The row as it was before the Set currently being applied.
	 *
	 * Several of the vendor's handlers act only when an attribute has
	 * CHANGED, comparing the new row against the one their first argument
	 * points at -- which is the pre-Set copy. Class 45's port-bridging and
	 * learning arms are gated that way, and so is class 298's whole rate
	 * limiter. Without this the guard cannot be expressed at all, and the
	 * choice was to send on every Set: more than the vendor sends, which is
	 * harmless but is not what the vendor does.
	 *
	 * `mib_write` snapshots the whole row before writing anything, so it is
	 * valid for exactly as long as the apply that follows. On a create the
	 * row is fresh and this is zeroes, which reads as "everything changed"
	 * -- also what the vendor sees. */
	uint8_t prev[MIB_ROW_MAX];
	/* Bytes MIB_ROW_MAX and up of a wide row, a slot of the side pool;
	 * 0 for every other row. `prev` stays 96 bytes: no wide class has a
	 * handler that compares before and after. */
	uint8_t *tail;
};

/* The width of a row's storage, and its bytes by offset. The caller checks
 * the offset against row_cap() first, as it did against MIB_ROW_MAX. */
static inline unsigned row_cap(const struct mib_row *r)
{
	return r->tail ? MIB_WIDE_MAX : MIB_ROW_MAX;
}

static inline uint8_t row_get(const struct mib_row *r, unsigned off)
{
	return off < MIB_ROW_MAX ? r->data[off] : r->tail[off - MIB_ROW_MAX];
}

static inline void row_put(struct mib_row *r, unsigned off, uint8_t v)
{
	if (off < MIB_ROW_MAX)
		r->data[off] = v;
	else
		r->tail[off - MIB_ROW_MAX] = v;
}

/* ---------------------------------------------------------------- GEM flows
 *
 * The driver's flow table is indexed by the flow id in word 0 of the
 * descriptor, and the id is the caller's to choose. The vendor keeps one table
 * per direction and picks the lowest free slot; so does this. Getting it wrong
 * is not a soft failure -- every flow sent with id 0 lands on top of the last
 * one, which is why a second downstream flow came back refused.
 */
struct flow_slot { uint16_t port; uint8_t used; uint8_t mcast; };

/* createTcont hands back the driver's own T-CONT index, which is not the ME id
 * the OLT uses. A GEM flow names its T-CONT by that index -- sending the ME id
 * (0x8000 for the first one) is what made the upstream leg fail where the
 * downstream leg, which does not name a T-CONT, succeeded. The vendor keeps the
 * same mapping; this is why createTcont's caller is given the entity id it never
 * sends to the driver. */
#define TCONT_MAX 32
struct tcont_slot { uint16_t meId; uint16_t index; uint8_t used; };

/* Where a T-CONT's Alloc-ID comes from (apply_qos.c, "the T-CONT
 * Alloc-IDs"): set by the OLT over OMCI, bound from the PLOAM assignments,
 * or none (0x00FF). */
#define TCONT_ALLOC_NONE  0
#define TCONT_ALLOC_OLT   1
#define TCONT_ALLOC_PLOAM 2
/* The file the kernel lists the PLOAM Alloc-IDs in (and the serial);
 * omcid -g points it elsewhere for a test. */
extern const char *gpon_proc_path;
/* The file the UNI link bit is read from (-l, test only), and its state. */
extern const char *uni_link_path;
int uni_link_state(void);
/* The file the bridge MAC address of class 46 is read from: the sysfs
 * address of eth0, the stick's own MAC (-m, test only). */
extern const char *bridge_mac_path;
/* The PON state number from gpon_proc_path, -1 when unreadable. */
int onu_state_now(void);
uint32_t uptime_cs(void);
/* ANI-G attribute 10/14 value of the DDM power word `type` (5 Tx, 6 Rx),
 * dBm in 0.002 dB steps; -1 when the driver cannot be read. */
int ddm_dbm_x500(unsigned type, int16_t *out);
int alloc_ids_refresh(void);
int gem_port_encrypted(uint16_t port);
int alloc_ids_known(void);
unsigned alloc_ids_assigned(const uint16_t **ids);
uint16_t tcont_alloc_id(uint16_t meId, int *src);
uint16_t tcont_bound_alloc_id(uint16_t meId, int *src);
int tcont_alloc_unset_mode(void);

/* Everything that crosses a module boundary. */
extern uint8_t serial[9];
/* Forget where serial[] came from, so the next serial_refresh() reads it
 * again: after the serial number was set in the driver. */
void serial_forget(void);
/* Fill serial[] from the kernel, or the config store until the kernel has
 * one; cheap once the kernel has answered. 0 when serial[] is set. `log`
 * says where it came from on stdout -- only from the main loop, never while
 * a CLI reply owns the output sink. */
int serial_refresh(int log);
extern uint8_t devid[40];
extern uint8_t mib_data_sync;
void mib_data_sync_advance(void);
const struct omci_class *find_class(uint16_t id);
uint16_t attr_width(const struct omci_class *c, unsigned k);
extern struct mib_tblent tblpool[MIB_TBL_NODES];
extern struct mib_row mib[MIB_ROWS];
int attr_offset(const struct omci_class *c, unsigned k);
uint16_t create_mask(const struct omci_class *c);
uint16_t set_reject_mask(const struct omci_class *c, uint16_t mask);
void retx_clear(void);
void tbl_free(struct mib_row *r);
int attr_is_big(const struct omci_class *c, unsigned k);
uint16_t tbl_serialise(const struct omci_class *c, uint16_t inst, unsigned k,
		       uint8_t *out, uint16_t max);
/* Whether attribute k of the class is a table of rows (tbl_defs[]), and how
 * many rows attribute k of a row holds. */
int tbl_is_rows(const struct omci_class *c, unsigned k);
unsigned tbl_rows(const struct mib_row *r, unsigned k);

/* The Get / Get-Next staging area.
 *
 * A Get of a big attribute cannot answer inline, so it reports the byte length
 * and the OLT then walks the value 29 bytes at a time. Everything between the
 * two lives here.
 *
 * The vendor keeps two of these (gOmciMulGetData is 2 x 0x80204 and every path
 * indexes it by a context id checked against 2). One is enough for us: that
 * index comes off the message struct's first halfword, which is the OMCC
 * instance, and this device has one. */
#define GETNEXT_CHUNK  29                /* baseline: 32 - result - mask */
#define GETNEXT_MAX    (MIB_TBL_NODES * MIB_TBL_ENTRY)

struct getnext_ctx {
	uint16_t classId;
	uint16_t inst;
	uint16_t attr;                       /* 1-based, as the mask counts */
	uint16_t len;
	uint16_t chunks;
	uint8_t  primed;
	uint8_t  data[GETNEXT_MAX];
};

extern struct getnext_ctx getnext;
struct mib_row *mib_find(uint16_t cls, uint16_t inst);
struct mib_row *mib_add(uint16_t cls, uint16_t inst);
void mib_del(uint16_t cls, uint16_t inst);
int mib_count(void);
struct mib_row *mib_row_at(int i);
/* Every entity, autonomous and OLT-created, and one entity's effective
 * values: see mibstore.c. The CLI dumps go through these, never the rows
 * alone. */
struct mib_ent { uint16_t cls, inst; };
#define MIB_ENTS_MAX (AUT_MAX + MIB_ROWS) /* every autonomous one and every row */
int mib_entities(uint16_t cls, int want_inst, uint16_t inst,
		 const struct mib_ent **out);
const struct mib_row *mib_view(const struct omci_class *c, uint16_t inst);
/* show.c: the dump behind `omcli mib` (vendor 0) and `omcicli mib get`
 * (vendor 1). Always ends with an "N rows" line; returns N. */
int mib_dump(uint16_t cls, int want_inst, uint16_t inst, int vendor, int secrets);
/* mibstore.c: the one list of classes whose contents are an owner's secret.
 * me_is_secret(): omcid keeps the contents of a Create/Set/Delete of this
 * class out of its log. me_attr_secret(): attribute k of the class is masked
 * in every dump unless the caller asked for secrets. */
int me_is_secret(uint16_t cls);
int me_attr_secret(uint16_t cls, unsigned k);
extern int conn_dirty;
extern int qos_dirty;
/* mcast.c: the multicast policy file igmpd reads. mcast_dirty is set by
 * whatever changes a class that feeds it, and the file is rewritten a quiet
 * second later (main.c), like the bridge connections. */
extern int mcast_dirty;
void mcast_policy_export(void);
int mcast_policy_class(uint16_t cls);
/* The attributes of a class 309 Create or Set that carry a value G.988 does
 * not define (version, function, immediate leave, tag control, unauthorized
 * join behaviour), as a mask: refused with result 9. */
uint16_t mcast_value_reject(const struct omci_class *c, uint16_t mask,
			    const uint8_t *data, uint16_t len);
/* ME 311, the multicast subscriber monitor: what igmpd keeps in
 * /var/run/odi-mcast.state. A counter, 0 when there is no file; and the IPv4
 * active group list as 24-byte table rows (G.988 9.3.29), returning the
 * byte length written: from igmpd's meter file when there is one (client,
 * time since the join, bandwidth), else from the state file. */
#define CLS_MCAST_MONITOR 311
uint32_t mcast_state_get(const char *key);
uint16_t mcast_active_groups(uint8_t *out, uint16_t max);
/* What igmpd meters (/var/run/odi-mcast.meter, src/igmp/meter.h), read only
 * while the policy asks for snooping: a header number ("bps", "lost"), 0
 * when there is none; ME 309 attribute 9 of profile `inst`, 10-byte rows of
 * the lost groups (only the subscriber's profile has any); and alarm 0: -1
 * when the subscriber has no profile, else whether any group is lost, with
 * the profile's instance in *inst. */
#define CLS_MCAST_OPER_PROFILE 309
uint32_t mcast_meter_get(const char *key);
uint16_t mcast_lost_groups(uint16_t inst, uint8_t *out, uint16_t max);
int mcast_lost_alarm(uint16_t *inst);
/* What the bridge connection's multicast rule takes from the subscriber's
 * profile (../mcastvlan.h): attribute 16, the downstream IGMP and multicast
 * TCI, and the VLAN (ANI) of each part 0 row of both access control lists,
 * each value once, up to max. Returns 0 when the subscriber has no profile. */
int mcast_ds_tag_input(uint8_t tci[3], uint16_t *ani, int max, int *nani);
/* The next us_qos_rebuild() tears everything down first: set when the
 * PLOAM Alloc-ID list changed, since the T-CONT indexes (the CAM rows) may
 * have moved. */
extern int qos_full;
void bdgconn_rebuild(void);
void us_qos_rebuild(void);
/* The upstream state the driver holds as omcid programmed it, one block,
 * for the resume snapshot. */
void *qos_prog_state(uint32_t *len);
uint16_t mib_write(struct mib_row *r, const struct omci_class *c, uint16_t mask, uint16_t skip, const uint8_t *src, uint16_t srclen);
/* Whether attribute k differs from what the row held before this Set. */
int mib_changed(const struct mib_row *r, const struct omci_class *c, unsigned k);
uint16_t attr_value(const struct omci_class *c, uint16_t inst, unsigned k, uint8_t *out);
uint32_t row_u32(const struct mib_row *r, const struct omci_class *c, unsigned k);
extern int apply_hw;
int omci_drv_call(uint32_t cmd, void *buf, uint32_t len);
extern uint8_t pairbuf[8];
void *pair(uint32_t port, uint32_t value);
extern uint8_t caps[OMCI_CAPS_LEN];
extern int caps_ok;
extern int caps_from_arg;
int hex_nib(char c);
uint32_t caps_u32(unsigned off);
uint32_t caps_flows(void);
uint32_t priq_related_port(uint16_t meId);
extern struct flow_slot flow_us[FLOW_MAX], flow_ds[FLOW_MAX];
extern int bc_flow;
extern struct omci_bdgconn servtab[SERV_MAX];
int bdgconn_add(int ingress, uint16_t gemPort, uint32_t dir, const struct omci_vlan_oper *vr);
void gen_no_vlan_filter_rule(struct omci_vlan_oper *vr);
void gen_manual_vlan_rule(struct omci_vlan_oper *vr, int vid, int pri, int isMc, int any_pri);
int manual_any_pri(void);
extern struct tcont_slot tcont_map[TCONT_MAX];
void mib_reset_all(void);
extern uint16_t alarm_snapshot;

/* ---------------------------------------------------------------- snapshot
 *
 * omcid's own MIB state (the rows, the table-attribute pool, the flow and
 * T-CONT bookkeeping, the service table -- everything that maps a managed
 * entity to switch programming), persisted to tmpfs so a respawned process
 * can resume answering the OLT without re-registration. See snapshot.c and
 * docs/BOOT.md, "Resume without re-registration".
 */
#define SNAPSHOT_PATH          "/var/run/omcid-mib.snap"
#define SNAPSHOT_TMP_PATH      "/var/run/omcid-mib.snap.tmp"
#define RESUME_DECISION_PATH   "/var/run/omcid-resume-decision"
#define RESUME_DECISION_PREV   RESUME_DECISION_PATH ".prev"
int snapshot_save(void);
/* Write `buf` to `tmp`, then rename it over `path`: a reader never sees a
 * half-written file. 0 on success. */
int atomic_write(const char *path, const char *tmp, const uint8_t *buf,
		 uint32_t n);
void snapshot_invalidate(void);
/* onu_state: the driver's ONU state (5 == O5). Returns 1 and repopulates the
 * MIB and its bookkeeping when a valid, matching snapshot was loaded; 0
 * otherwise, leaving everything exactly as it was (empty, for a fresh
 * process) -- the caller then falls back to the ordinary re-registration
 * path.
 * *why names the outcome: "resumed", or why not -- "not_o5", "no_snapshot",
 * "bad_snapshot" (short, wrong magic or version, bad crc) or "other_device"
 * (another device id or serial number). */
int snapshot_try_resume(uint32_t onu_state, const char **why);
void snapshot_write_decision(int resumed);
/* `creating` separates a Create from a Set. Class 47 needs it: the stock stack
 * drives the MAC learning limit and the flooding mask from the create arm
 * unconditionally, and the traffic descriptors only from the set arm, gated
 * on the attribute being in the mask. */
void apply_entity(const struct omci_class *c, uint16_t inst, const struct mib_row *r, uint16_t mask, int creating);
void apply_delete(const struct omci_class *c, uint16_t inst);
int instance_is_autonomous(uint16_t cls, uint16_t inst);
/* The instances the ONU creates for itself, as this configuration has them:
 * generated/omci_autonomous.c with the VEIP slot applied to the ids of the
 * slot's entities and the vendor-private classes left out when OMCI_PRIVATE_ME
 * is 0. Everything that walks the ONU-created entities reads this, not the
 * generated array. aut_rebuild() after any change of the two keys. */
#define AUT_MAX 512
extern struct omci_instance aut[AUT_MAX];
extern unsigned aut_count;
void aut_rebuild(void);
/* The id an entity has in the generated tables (slot 6) for an id of the
 * configured slot, for looking up generated default values. */
uint16_t canon_inst(uint16_t cls, uint16_t inst);
/* drv.c: the switch port of a UNI entity from the capability slots, or -1. */
int uni_switch_port(uint16_t meId, uint8_t slot_type);
/* apply_qos.c */
int us_queue_referenced(uint16_t pqMe);
/* Does an upstream GEM port CTP name this traffic descriptor (class 280)
 * as its upstream one? Its rates are then the rates of that CTP's queue. */
int us_td_referenced(uint16_t td);
/* Does an upstream queue in the plan name this traffic scheduler (class
 * 278)? Only then can its Policy change what is programmed. */
int us_scheduler_referenced(uint16_t ts);
int flow_find(int ds, uint16_t port);
void bc_gem_update(void);
void bc_gem_withdraw(uint16_t going);
void apply_priq(uint16_t inst);
void apply_gem_ctp(const struct omci_class *c, const struct mib_row *r);
/* A class 281 changed (`skip`: the instance being deleted, else -1): resend
 * the downstream flows whose multicast flag no longer matches. */
void mcast_flag_refresh(int skip);
/* The extra multicast GEM ports' downstream flows, sent or withdrawn to
 * match the live class 281 and 309 rows (`skip*`: the instance of that
 * class being deleted, else -1). */
void mcast_extra_refresh(int skip281, int skip309, int skip268);
void qos_reset(void);
/* apply_uni.c */
uint32_t bridge_uni_port_mask(uint16_t bridge_id);
/* Class 50: the table of a bridge port read from the switch (apply_uni.c);
 * the bytes written to out, never more than max, 0 without a driver. */
uint16_t bridge_table_read(uint16_t inst, uint8_t *out, uint16_t max);
void dot1rl_apply(const struct omci_class *c, uint16_t inst,
		  const struct mib_row *r, int drop, int all);
void apply_pptp_uni(const struct omci_class *c, uint16_t inst,
		    const struct mib_row *r, uint16_t mask, int creating);
void apply_mbpcd(const struct omci_class *c, uint16_t inst,
		 const struct mib_row *r, uint16_t mask, int creating);
void delete_mbpcd(const struct omci_class *c, uint16_t inst);
void apply_ext_vlan_dscp(const struct omci_class *c, uint16_t inst,
			 const struct mib_row *r, uint16_t mask);
void apply_mapper_dscp(const struct omci_class *c, uint16_t inst,
		       const struct mib_row *r, uint16_t mask);
void apply_mbsp(const struct omci_class *c, uint16_t inst,
		const struct mib_row *r, uint16_t mask, int creating);
void delete_mbsp(uint16_t inst);
void uni_reset(void);
void uni_start(void);
/* apply_bridge.c */
uint32_t veip_uni_port_mask(uint16_t veipId);
uint32_t all_eth_uni_mask(void);
int iphost_is_voice(uint16_t id);
int iphost_switch_port(uint16_t id);
void cli_row(const struct mib_row *r, int secrets);
void v_banner(const struct omci_class *c);
int vendor_row_exists(const struct omci_class *c);
int vendor_row(const struct mib_row *r, const struct omci_class *c);
uint32_t cli_mib(void);
uint32_t cli_flows(void);
uint32_t cli_caps(void);
uint32_t cli_tcont(void);
uint32_t cli_state(void);
uint32_t cli_provision(void);
uint32_t cli_voip(void);
uint32_t cli_conn(void);
void qmap_dump(void);
uint32_t cli_bridge(void);
uint32_t cli_ident(void);
uint32_t cli_vlan(void);
uint32_t cli_cfgset(void);
uint32_t cli_help(void);
void onu_state_sample(void);
extern long cliq;
extern long reply_q;
extern struct omcli_req clireq;
extern struct omcli_rep clirep;
extern uint32_t cli_seq;
extern int cli_failed;
extern uint32_t cli_used;
void cli_sink(const char *s, int n);
const char *cli_arg(unsigned i);
int cli_num(unsigned i, uint32_t *out);
int cli_id(unsigned i, uint32_t *out);
int cli_poll(void);
extern long vq;
extern int vq_is_ours;
extern long vfile;
void vq_ensure(void);
int vq_poll(void);
extern uint8_t rxbuf[2048];
extern uint8_t frame[OMCI_FRAME_LEN];
extern volatile int nl_fd;
extern volatile uint32_t nl_tid;
void handle(int fd, uint32_t tid, const uint8_t *f);
/* OMCI frames handle() has taken, from the line or injected: the main loop
 * times the quiet second before its rebuilds on this, not on CLI traffic. */
extern unsigned long omci_frames_handled;
/* Received frames the check in rx_accept() flagged, per reason: the index is
 * one of RX_REJ_*, for the line and the injection queue (omcimsg.c). A frame
 * it refuses is never answered; RX_REJ_TRAILER is only counted, the frame is
 * answered. */
#define RX_REJ_LENGTH   0
#define RX_REJ_DEVID    1
#define RX_REJ_TRAILER  2
#define RX_REJ_CRC      3
#define RX_REJ_N        4
extern unsigned long omci_rx_reject[RX_REJ_N];
int rx_accept(const uint8_t *f, uint32_t len);

/* ------------------------------------------------------------- event lines
 *
 * One "event=..." line per link or provisioning event, to omcid.log and to
 * syslog (facility daemon). See events.c and docs/TOOLS.md, "Link and
 * provisioning events". `side` is "olt" or "local". */
void ev_start(int restart, int resumed, const char *why, uint32_t onu_state);
void ev_mib_reset(const char *side);
/* Every request handled, before its answer: the last few go on the next OLT
 * MIB reset line (before=). Not the upload Nexts or download sections. */
void ev_rx_msg(uint8_t mt, uint16_t cls, uint16_t inst);
/* MIB resets since omcid started, the OLT resets within the loop window, and
 * whether they are a loop now (events.c, "mib_reset_loop"). */
#define MIBLOOP_RESETS    3
#define MIBLOOP_WINDOW_S  600
void ev_mib_reset_counts(unsigned long *olt, unsigned long *local,
			 unsigned *recent, int *loop, unsigned long *loops);
void ev_mib_upload(uint16_t total);
void ev_mib_upload_next(uint16_t seq);
void ev_config_write(uint8_t mt, uint16_t cls, uint16_t inst);
void ev_olt_command(uint8_t mt, uint16_t cls, uint16_t inst, const char *result);
void ev_snapshot_fail(unsigned rows);
void ev_alloc_ids(const uint16_t *ids, unsigned n);
/* A config reload (reload.c): the decision, a step that failed, and the
 * outcome. `keys` is store key names only, never values. */
void ev_reload(const char *changed, const char *action, const char *keys);
void ev_reload_step(const char *verb, long rc);
void ev_reload_done(const char *changed, const char *action, const char *result,
		    long duration_ms, long o5_ms, unsigned services);
/* One software image step: `sections` < 0 and `crc` 0 leave those keys out,
 * `size` < 0 leaves out size and window. */
void ev_sw_image(const char *op, uint16_t inst, long size, unsigned window,
		 long sections, const char *crc, const char *result);
/* A class 84 forward operation no bridge rule can express, built as
 * `used` instead: logged once per code for the life of the process. */
void ev_vlan_fwdop(uint16_t inst, uint8_t code, uint8_t used);
void ev_vlan_fwdop_override(uint16_t inst, uint8_t code, uint8_t used);
/* An OLT lock or unlock of an Ethernet UNI, or a lock a MIB reset or start
 * undid (apply_uni.c): event=uni_lock, once per change of a port's state. */
void ev_uni_lock(uint16_t inst, int port, const char *action);
void ev_tick(void);

/* ------------------------------------------- alarms and AVCs (alarm.c)
 *
 * The autonomous messages: alarm notifications and attribute value changes,
 * evaluated once a second in O5 (alarm_tick), and the Get All Alarms
 * snapshot. See alarm.c. */
void alarm_init(void);
void alarm_tick(void);
/* Performance monitoring history data (pm.c): the 15-minute intervals and
 * ME 312 on them. pm_attr answers a PM attribute from them, 1 when it is
 * one; pm_view(1) makes the calls that follow answer the interval in
 * progress (Get current data), pm_view(0) the completed one. */
extern const char *pm_counters_path;
extern const char *pm_clock_path;
int pm_class(uint16_t cls);
void pm_create(uint16_t cls, uint16_t inst);
void pm_delete(uint16_t cls, uint16_t inst);
void pm_mib_reset(void);
void pm_sync_time(unsigned hh, unsigned mm, unsigned ss);
void pm_tick(void);
void pm_view(int current);
int pm_attr(uint16_t cls, uint16_t inst, unsigned k, uint8_t *out, uint16_t n);
void alarm_mib_reset(void);
uint16_t alarm_get_all(void);
int alarm_get_next(uint16_t seq, uint8_t *body);
void anig_signal_thresholds(int sf, int sd);
extern unsigned long notify_sent;
/* 10 log10(x) in 0.002 dB (omcimsg.c). */
int32_t db_x500_of(uint32_t x);
/* omci_drv_call, except that the driver-call trace does not log it: for the
 * once-a-second polls, whose count depends on the clock (drv.c). */
int omci_drv_call_quiet(uint32_t cmd, void *buf, uint32_t len);
/* What the OLT sent that omcid does not model: one event line and one
 * summary line per unknown class and operation, or unknown message type,
 * per boot (events.c, UNKNOWN_PATH). ev_msg_known() is whether omcid knows
 * the message type at all, answered or deliberately refused. */
#define UNKNOWN_PATH     "/var/log/omcid-unknown.txt"
#define UNKNOWN_TMP_PATH "/var/log/omcid-unknown.txt.tmp"
int ev_msg_known(uint8_t mt);
/* A received frame dropped by rx_accept(): event=rx_reject, at most one line
 * per reason per minute, carrying the count since the last one. */
void ev_rx_reject(int reason, uint8_t devid, uint32_t len);
void ev_unknown_me(uint16_t cls, uint8_t mt);
void ev_unknown_msg(uint8_t mt, uint16_t cls);
/* A Get, Get current data, Set or Delete of a modelled class, addressed to
 * an instance omcid holds no row for and does not create itself: one event
 * line and one summary line per (class, instance) per boot. Diagnostic only;
 * the answer is unchanged (omcimsg.c, "an instance omcid does not hold"). */
void ev_no_instance(uint16_t cls, uint16_t inst, uint8_t mt);
/* OMCI_UNKNOWN_ME_OK=1 in CFG_ODI_PATH: answer a Create, Set or Get of a
 * class omcid does not model with success instead of "unknown entity". */
extern int unknown_me_ok;
/* OMCI_ACCEPT_REBOOT=1 in CFG_ODI_PATH: an ONU-G Reboot from the OLT is
 * answered with success and followed by a reboot; 2 answers success and does
 * not reboot. Read when the Reboot
 * arrives (omcimsg.c), not held, so a change needs no reload. reboot_pid is
 * the process that gets the shutdown signal: init, unless -k says otherwise
 * (a test harness has no init to reboot). */
extern long reboot_pid;

/* ------------------------------------------------ reload on SIGHUP (reload.c)
 *
 * The handler only sets a flag; reload_poll(), called from the main loop,
 * does the work and drives the re-registration as a state machine, so the
 * loop (and its watchdog ping) never blocks for it. */
#define RELOAD_STATUS_PATH     "/var/run/omcid-reload"
#define RELOAD_STATUS_TMP_PATH "/var/run/omcid-reload.tmp"
#define ODI_INIT_PATH          "/proc/odi_init"
extern volatile int reload_pending;
extern const char *odi_init_path;
extern long reload_o5_wait_s;
void reload_init(void);
void reload_on_hup(int sig);
void reload_poll(void);

/* ------------------------------------------------ software download (swimage.c)
 *
 * Start / Download section / End software download, Activate and Commit
 * image, from the OLT. OLT_SW_DOWNLOAD (odi.conf) picks accept (the default:
 * answered with success, the image counted, CRC-checked and discarded) or
 * reject (not supported). Nothing here writes flash, the U-Boot environment,
 * or reboots. */
void send_resp(int fd, uint32_t tid, const uint8_t *req,
	       const uint8_t *contents, uint16_t clen);
void sw_handle(int fd, uint32_t tid, const uint8_t *f, const struct omci_class *c);
/* The class 7 flags a Get reports: attribute 2 is_committed, 3 is_active,
 * 4 is_valid. */
uint8_t sw_flag(uint16_t inst, unsigned attr);
/* The class 7 instance (0 or 1) whose is_active flag is set. */
uint16_t sw_active_inst(void);
#define SWIMAGE_PATH     "/var/run/omcid-swimage"
#define SWIMAGE_TMP_PATH "/var/run/omcid-swimage.tmp"
int atomic_write(const char *path, const char *tmp, const uint8_t *buf,
		 uint32_t n);

/* ------------------------------------------------------------ config store
 *
 * The identity the line authenticates with, out of the two XML files on the
 * jffs2 partition. See cfgstore.c: which key lives in which file comes from
 * xmlconfig's own descriptor table, and the LOID selection rule comes from
 * /etc/runomci.sh. */
struct onu_identity {
	char sn[32];
	char ploam[32];
	char loid[64];
	char loidPwd[64];
	int  snLen;                          /* -1 absent, 0 present and empty */
	int  ploamLen;
	uint8_t loaded;
};

#define FWDOP_OVR_MAX 12

/* The manual VLAN, which is where the service's tag actually comes from. */
struct onu_vlan_cfg {
	int mode;                            /* VLAN_MANU_MODE, 0 = off */
	int type;                            /* VLAN_CFG_TYPE */
	int vid;                             /* VLAN_MANU_TAG_VID, -1 absent */
	int pri;                             /* VLAN_MANU_TAG_PRI, 0 when absent */
	uint8_t manual;                      /* type 1, mode 1, vid and pri present */
	uint8_t loaded;
	uint8_t eapol;                       /* VLAN_EAPOL_UNTAGGED=1 in odi.conf */
	/* RTK_DEVINFO_SPECVER, the stock FwdOp override (cfgstore.c): class 84
	 * instance and the code to build it with. nfwdop -1: malformed, off. */
	int nfwdop;
	struct { uint16_t inst; uint8_t code; } fwdop[FWDOP_OVR_MAX];
};

/* What the OLT is told about this ONU, from the store. See cfgstore.c. */
#define CFG_REPORT_SWITCH       "/var/config/omci-identity.on"
#define REPORT_DEFAULT_SW_VER   "0.0.0"
#define REPORT_SW_VER_LEN       14       /* class 7 attribute 1 */
#define REPORT_MODEL_LEN        20       /* class 257 attribute 1 */
#define REPORT_HW_VER_LEN       14       /* class 256 attribute 2 */
#define REPORT_OMCC_VER_MAX     255      /* class 257 attribute 2, one byte */
#define REPORT_PRODUCT_CODE_MAX 65535    /* class 257 attribute 3, two bytes */
struct onu_report {
	char swVer[2][REPORT_SW_VER_LEN + 1];
	int  swVerLen[2];                    /* -1 absent, 0 empty */
	char model[REPORT_MODEL_LEN + 1];
	int  modelLen;
	/* ONU_HW_VERSION, odi.conf: -1 absent or empty, -2 set but not
	 * reportable (longer than 14 or not printable ASCII). */
	char hwVer[REPORT_HW_VER_LEN + 1];
	int  hwVerLen;
	int  omccVer;                        /* -1: absent, empty or not a number */
	int  productCode;                    /* likewise */
	uint8_t on;                          /* CFG_REPORT_SWITCH exists */
	uint8_t loaded;
};

extern struct onu_identity ident;
extern struct onu_vlan_cfg vlanCfg;
extern struct onu_report report;
/* OMCI_FIXED_ME (odi.conf): classes omcid has no model for, answered with
 * fixed bytes. attr 0 is the any-instance form (CLASS:HEX); otherwise the
 * entry is attribute attr of the entity at inst. n 0 is off. */
#define FIXED_ME_MAX     25
#define FIXED_ME_ENTRIES 16
#define FIXED_ME_UPLOAD  32     /* contents of one upload record: class, instance, mask, values */
struct fixed_me {
	uint16_t cls;
	uint16_t inst;
	uint8_t attr;
	uint8_t len;
	uint8_t data[FIXED_ME_MAX];
};
struct fixed_me_set {
	uint8_t n;
	struct fixed_me e[FIXED_ME_ENTRIES];
};
extern struct fixed_me_set fixed_me;
void cfg_load_fixed_me(void);
const struct fixed_me *fixed_me_find(uint16_t cls);
int fixed_me_per_inst(uint16_t cls);
int fixed_me_has_inst(uint16_t cls, uint16_t inst);
const struct fixed_me *fixed_me_attr(uint16_t cls, uint16_t inst, unsigned attr);
int fixed_me_upload(int want, uint8_t *body, uint16_t *len);

/* The MIB upload, built as the stock omci_app builds it (mibstore.c). A MIB
 * upload takes a snapshot -- every entity the ONU holds, each split over as
 * many records as its attributes need -- and the Next commands read records
 * out of it by sequence number. mib_upload_build() returns the record count
 * the upload response announces, mib_upload_record() fills one record's
 * contents (class, instance, mask, values) and returns 0, or -1 past the
 * end. */
#define UPL_REC_MAX   1024              /* 399 on the stock ONU, plus the OLT's */
#define UPL_REC_BYTES 26                /* attribute bytes in one baseline record */
unsigned mib_upload_build(void);
int mib_upload_record(unsigned seq, uint8_t *body, uint16_t *len);
/* The tables the ONU keeps for each MAC bridge port the OLT creates (class
 * 47): the filter table data (49), the bridge table data (50) and the filter
 * preassign table (79), each at the port's own entity id, empty or all
 * zeros. Added on a Create of 47, removed on its Delete. */
void mbpcd_tables_add(uint16_t inst);
void mbpcd_tables_del(uint16_t inst);
/* The MAC bridge configuration data (class 46) the ONU keeps for each MAC
 * bridge service profile (45) the OLT creates, at the profile's own entity
 * id. Added on a Create of 45, removed on its Delete; the values are
 * derived, see attr_value(). */
void mbcd_add(uint16_t inst);
void mbcd_del(uint16_t inst);
/* Records announced by the last MIB upload, 0 before the first. */
extern unsigned upl_total;

/* ------------------------------------------------------ config reload
 *
 * SIGHUP rereads the store and acts on what changed (reload.c). The keys
 * fall in two classes: those that change only how a service is tagged
 * (rebuilt in place), and those the OLT has to see again (the ONU
 * re-registers). cfg_snap_diff() answers which, from the values omcid holds
 * and the ones just read. */
struct cfg_snap {
	struct onu_identity ident;
	struct onu_vlan_cfg vlan;
	struct onu_report report;
	int unknown_me_ok;
	struct fixed_me_set fixed;
	int veip_slot_key;
	int veip_slot;
	int private_me;
	int veip_on;
	int custom_me;
	int custom_mcast;
	int pots_ports;
	int pots_slot_key;
	int pots_slot;
};
/* Identity class: one bit per key the OLT reads. */
#define CFGD_SN         (1u << 0)
#define CFGD_PLOAM      (1u << 1)
#define CFGD_LOID       (1u << 2)
#define CFGD_LOID_PWD   (1u << 3)
#define CFGD_SW_VER1    (1u << 4)
#define CFGD_SW_VER2    (1u << 5)
#define CFGD_MODEL      (1u << 6)
#define CFGD_OMCC_VER   (1u << 7)
#define CFGD_PRODUCT    (1u << 8)
#define CFGD_HW_VER     (1u << 9)
#define CFGD_SWITCH     (1u << 10)
#define CFGD_UNKNOWN_ME (1u << 11)
#define CFGD_FIXED_ME   (1u << 16)
#define CFGD_VEIP_SLOT  (1u << 17)
#define CFGD_PRIVATE_ME (1u << 18)
#define CFGD_VEIP_ON    (1u << 19)
#define CFGD_CUSTOM_ME  (1u << 20)
#define CFGD_POTS_PORTS (1u << 21)
#define CFGD_POTS_SLOT  (1u << 22)
#define CFGD_IDENTITY   (0x0fffu | CFGD_FIXED_ME | CFGD_VEIP_SLOT | CFGD_PRIVATE_ME | CFGD_VEIP_ON | CFGD_CUSTOM_ME | CFGD_POTS_PORTS | CFGD_POTS_SLOT)
/* Rebuild class, like the VLAN keys: what the bridge rules are built from. */
#define CFGD_CUSTOM_MCAST (1u << 23)
#define CFGD_FWDOP        (1u << 24)
#define CFGD_EAPOL        (1u << 25)
/* VLAN class: the four keys that pick how a connection is tagged. */
#define CFGD_VLAN_TYPE  (1u << 12)
#define CFGD_VLAN_MODE  (1u << 13)
#define CFGD_VLAN_VID   (1u << 14)
#define CFGD_VLAN_PRI   (1u << 15)
#define CFGD_VLAN       0xf000u
void cfg_snap_take(struct cfg_snap *s);
void cfg_snap_restore(const struct cfg_snap *s);
/* Reread every key the daemon holds, the way startup reads them. */
void cfg_load_all(void);
void cfg_load_unknown_me(void);
/* OMCI_VEIP_SLOT_ID and OMCI_PRIVATE_ME: which instances the ONU creates for
 * itself. The VEIP key follows the stock firmware: a decimal slot number in
 * the config store, in effect only with bit 0x100 of OMCI_CUSTOM_ME. */
#define VEIP_SLOT_DEFAULT  6
#define CUSTOM_ME_VEIP_SLOT_ID 0x100
/* OMCI_CUSTOM_ME bit 0x40 loads me_00000040.so on the stock firmware, whose
 * cf_me_65529_is_supported is what makes the MIB reset create the CTC ONU
 * capability entity (class 65529, FUN_0040dc44 calls feature 0x23). Without
 * the bit the entity does not exist -- the factory config (0x10000) and the
 * ISP2 stick have no class 65529 instance, the ISP1 stick (0x9040) has one. */
#define CUSTOM_ME_ONU_CAPABILITY 0x40
/* OMCI_CUSTOM_ME as stored, 0 when absent or not a number. */
extern int custom_me;
void cfg_load_custom_me(void);
/* OMCI_CUSTOM_MCAST, the stock key, decimal. Bit 0x1 loads mc_00000001.so on
 * the stock firmware, whose cf_force_ds_mcast_untagged writes control type 1
 * (strip the outer tag) over byte 0 of class 309 attribute 16 before the
 * profile is applied; here the bit does the same to the bridge rules, and
 * the attribute keeps what the OLT set. */
#define CUSTOM_MCAST_DS_UNTAGGED 0x1
extern int custom_mcast;
void cfg_load_custom_mcast(void);
void cfg_load_veip_slot(void);
void cfg_load_private_me(void);
/* The slot key as stored (0..255), -1 when absent or not a valid slot; the
 * slot omcid builds the VEIP at, 6 unless the key is valid and enabled; and
 * whether the vendor-private entities 350, 370 and 373 are created. */
extern int veip_slot_key;
extern int veip_slot;
extern int private_me;
/* OMCI_VEIP (odi.conf): 0 creates no VEIP at all, so the ONU presents only
 * its PPTP Ethernet UNI; the VEIP, its UNI-G, the TR-069 server and the slot's
 * cardholder and circuit pack are then left out. */
extern int veip_on;
void cfg_load_veip_on(void);
/* OMCI_POTS_PORTS (odi.conf): the phone ports, 0 to POTS_PORTS_MAX; 0 when
 * absent or anything else. While it is 0 the VoIP classes of the model
 * (voip_class(), mibstore.c) are unknown to the OLT, exactly as before the
 * model had them, and the ONU creates none. With ports, it creates a POTS
 * circuit pack in slot pots_slot (aut_rebuild()): OMCI_POTS_SLOT_ID
 * (odi.conf) when that is a free slot, kept in pots_slot_key (-1 when
 * absent or not usable), else the first free slot from 2 up. Both keys are
 * read here, after the VEIP slot, which the POTS slot must avoid. */
#define POTS_PORTS_MAX 2
extern int pots_ports;
extern int pots_slot_key;
extern int pots_slot;
void cfg_load_pots_ports(void);
int voip_class(uint16_t cls);
unsigned cfg_snap_diff(const struct cfg_snap *old, const struct cfg_snap *cur);
/* The store key names of a diff mask, comma separated, into out. */
void cfg_diff_names(unsigned mask, char *out, int max);
/* The SN the driver takes in "gponsn": the 12-character label, or NULL. */
const char *cfg_sn_label(const struct onu_identity *id);
void cfg_load_vlan(void);
void cfg_load_vlan_from(const char *cs);
int cfg_manual_vid(void);
int cfg_olt_tagging(void);
/* VLAN_EAPOL_UNTAGGED (odi.conf), as loaded with the VLAN keys: 1 passes EAPOL
 * untagged under the manual tag. Whether it applies is up to the caller, which
 * needs cfg_manual_vid() >= 0. */
int cfg_eapol_untagged(void);
uint32_t cfg_fwdop(uint16_t inst, uint32_t olt_code);
void cfg_load_report(void);
void cfg_load_report_from(const char *cs, const char *hs, const char *sw,
			  const char *odi);
const char *report_sw_ver(uint16_t inst);
const char *report_hw_ver(void);
void cfg_show_vlan(void);
int cfg_get(const char *path, const char *key, char *out, int max);
/* The odi-only keys (docs/SETTINGS.md, "Odi-only keys"): KEY=value lines in
 * CFG_ODI_PATH, which the stock firmware never reads. Returns the value
 * length (0 present and empty), -1 absent or unreadable. */
#define CFG_ODI_PATH "/var/config/odi.conf"
int cfg_odi_get(const char *path, const char *key, char *out, int max);
extern const char *const CFG_CS_PATH;
void cfg_load_identity(void);
void cfg_load_identity_from(const char *cs, const char *hs);
int cfg_set(const char *path, const char *dir, const char *key, const char *value);
void cfg_show_identity(void);

#endif
