# omci — replacing omci_app

Our own OMCI stack for the ODI DFP-34X-2C2: it terminates the OLT's
management channel, holds the MIB, and turns what the OLT provisions into
switch and GPON configuration. This directory builds:

- `omcid` (`respond/`) -- the daemon; answers the OLT and programs the switch
- `omcli` / `omcicli` (`cli/`) -- the client, one binary, symlinked twice
- `omciprobe` (`probe/`) -- one raw driver command, bypassing omcid
- `omcicap` (`capture/`) -- captures the OMCI channel

User-facing flags, config-store keys and log format are in
`docs/TOOLS.md`; this file is the architecture and the design decisions
behind it.

## The three interfaces

    OLT  --OMCI frames--  [netlink redirect, type 1]  --+
                                                         |
                                omcid  --netlink ODI_OMCI_OP_CMD, cmd 1..73--  odi_switch
                                     |
                      SysV msgq 0x800 (mtype 4)
                                     |
                                 omcli / omcicli

1. **Southbound control**: driver commands over the `odi_omci` netlink
   transport (`ODI_OMCI_OP_CMD`, on a socket of its own), answered by
   `odi_switch_cmd()` in the kernel; `respond/drv.c` is the one place that
   sends them. The argument structs are the kernel ABI headers in
   `kernel/extra/drivers/net/ethernet/odi/uapi/`.
2. **Southbound data**: `odi_omci` netlink redirect, type 1. **The redirect
   has one receiver per type** -- registering takes it from whoever holds
   it, there is no passive tap. That is why every one of these binaries
   deregisters on every exit path, signals included (`redirect_guard.h`),
   and why `omciprobe` never registers at all (it goes through the sockopt
   path only) while `omcicap` refuses to start against a live `omcid`
   unless forced.
3. **Northbound**: the CLI, over the vendor's own System V message queue
   (key 0x800, 20-byte header, 240-byte payload, command id in word 0).
   `omcid` answers on this queue too, in the stock output format, because
   `confd`'s MIB/Services pages and the exporter's `omcicli dump srvflow`
   already depend on it.

## The MIB model is data

`tools/mib-gen.py` emits `generated/omci_mib.[ch]` and `omci_mib.json`
-- 99 managed entities, 941 attributes, each with name, type, width, OLT
access and index -- from two checked-in sources, without a stock root
filesystem:

- `model/g988.json`, the factual table of ITU-T G.988 (11/2022) with
  Amendments 1 and 2: per class the clause, creation model and actions,
  per attribute its width, access, requirement, AVC, table and PM flags.
  It holds numbers and flags with clause references, no specification
  text. `tools/g988-extract.py` produces it from the PDF on a developer
  machine; it never runs in CI and the PDF is not stored here.
- `model/profile.json`, the device profile: which classes the device
  implements and in which order, the names the CLI, `confd` and the
  exporter depend on, local row layout and class flags, the 23 private
  classes, obsolete class 267 and the private attributes inside standard
  classes. Where the device deviates from the derived value, an override
  names the field, a classification (`REPRESENTATION`, `SUPPORT`,
  `SPEC_NEWER`, `VENDOR_DEVIATION` or `SUSPECT`) and a reason. An override
  that changes nothing, lacks a reason or names an unknown attribute is an
  error.

`tools/mib-compare.py` compares the pure derivation with a reference model
and reports every difference with its classification. About 95% of the
attribute fields of the standard classes come from G.988 without an
override. The names come from the earlier model, not from G.988. ME 46,
ME 290 (Dot1X port extension package: an OLT that runs 802.1X on a UNI
Sets it, and omcid keeps what the OLT writes without acting on it) and the
sixteen VoIP classes of G.988 clause 9.9 (SIP over OMCI, answered only
while `OMCI_POTS_PORTS` is on, docs/SETTINGS.md) have no stock counterpart;
their profile entries are not marked `stockBacked`.

Edit the sources, never `generated/`: `test/omci_mib_gen_test.sh` (part of
`make test-host`) fails when the generated files differ from what the
sources produce, and rejects invalid profiles. `tools/mib-tables.py`, which
recovered the original model from a stock root filesystem, stays as an
optional cross-check. `tools/me-drv-map.py` shows only 21 of the 81
stock-backed managed entities reach the driver at all (T-CONT, GEM port,
bridge port and friends); the other 60 are pure MIB and need no driver
call.

The rootfs tools read the stock rootfs from `$STOCK_ROOTFS` (default
`stock-rootfs` in the current directory); most also take the path as
their first argument.

## Where this differs from the vendor daemon, and why

`omcid` drives the same wire protocols (southbound sockopt, northbound
message queue) so it is a drop-in for `confd`, the exporter and the stock
`/bin/omcicli` client, but fixes several problems observed in the vendor
daemon along the way:

- **Dumps are waited for, not slept through.** The vendor client sleeps a
  fixed 12 ms after a dump request and reads whatever landed in the temp
  file, which truncates long dumps. `omcid`'s handler loop is single
  threaded and CLI messages are FIFO with respect to each other, so
  `omcli` treats the next queue-answering reply as the completion signal
  instead of a timer.
- **The reply queue is per process and `IPC_PRIVATE`.** The vendor client
  hardcodes one reply key, so two concurrent runs read each other's
  answers; a killed client's queue also cannot outlive it and leak.
- **`get tables` no longer deadlocks.** The vendor handler takes a table
  mutex twice; `omcid` has no such mutex and answers it directly, though
  `omcli` still refuses to send it to a real vendor daemon (`--vendor`).
- **Every blocking request gets a reply**, even a bare acknowledgement,
  so a caller can never hang waiting for one that was silently dropped.
- **The reply buffer is zeroed before it is filled**, not after, so a
  field the handler forgets to set never leaks whatever the buffer held
  before.
- **The message queue is recreated, not attached to, at startup**, since
  its id changes across every restart of whichever daemon owns key 0x800.

One accepted difference from the vendor, worth stating because a test
depends on it: **a Set that changes only one field of a multi-pointer table
row re-sends every pointer in that row**, not only the one that changed
(the vendor gates re-send on which pointer changed against the
previously-stored row; our store keeps one row and overwrites it, so the
guard is dropped). This is strictly more traffic to the switch driver than
the vendor sends, never less, and is exercised by `qemu-test.sh`.

## What respond/apply*.c reproduces

`apply.c` dispatches a Create, Set or Delete by class; `apply_qos.c`
(queues, T-CONTs, GEM flows, the broadcast flow), `apply_uni.c` (the UNI,
its bridge ports and service profile, rate limits, flooding, DSCP) and
`apply_bridge.c` (the bridge connections) hold the work. Each handler
reproduces what one handler of the stock daemon (`omci_app` and its
`mib_*.so` plugins) was seen to send; the names below are the symbols of
those shipped binaries, kept here as provenance rather than in the code.

| ours | stock | notes |
|---|---|---|
| `apply_priq()` | `PriQDrvCfg` (`mib_PriQ.so`), `omci_wrapper_setPriQueue` | sent on a class 277 Set; the stock setPriQueue returns before the driver for an upstream queue (bit 15) |
| queue weights | `mibTable_init` of `mib_PriQ.so` | the initial values the stock queue table stages |
| `tcont_apply()` | `TcontDrvCfg` | Alloc-IDs it refuses: 0xff and 4096 or more |
| `tcont_alloc_id()` | the T-CONT AllocID values of the stock MIB on ISP1 | a T-CONT the OLT never set takes the next PLOAM Alloc-ID (`/proc/odi_gpon/status`, `alloc_ids`) in assignment order, as the stock MIB shows it doing; an OMCI-set one keeps its value (G.988 9.2.2). `tcont_bound_alloc_id()` is that value and is what the datapath uses; `tcont_alloc_id()` is what is reported, and with `OMCI_TCONT_ALLOC_UNSET=unassigned` reads 0x00FF for a T-CONT the OLT never set |
| `bc_gem_update()`, `bc_gem_withdraw()` | `MacBriPortCfgDataDrvCfg` | the broadcast flow lookup and its delete arm |
| `apply_mbpcd()`, `delete_mbpcd()` | `MacBriPortCfgDataDrvCfg` | its create, set and delete arms; the global default learning limit is in `gInfo[0xe4]`, runtime data that no store in the binary writes, so it is not recovered |
| `mbpcd_uni_rate()` | `omci_apply_traffic_descriptor_to_uni_port` | whole |
| `uni_switch_port()` | `pptp_eth_uni_me_id_to_switch_port` | the stock one reaches the slot type through the entity id and its own data; ours takes it as an argument |
| `bridge_uni_port_mask()`, `bridge_veip_port_mask()`, `all_eth_uni_mask()` | `omci_get_pptp_eth_uni_port_mask_in_bridge`, `omci_get_eth_uni_port_mask_behind_veip`, `omci_get_all_eth_uni_port_mask` | the stock daemon picks between the first two on `gInfo[0]`; we try the PPTP shape and fall back |
| `apply_ext_vlan_dscp()` | the tail of `ExtVlanTagOperCfgDataDrvCfg` | the DSCP map only; the VLAN rules travel in the bridge connection |
| `apply_mapper_dscp()` | `Map8021pServProfConnCfg` | the one arm that reaches the driver; `Map8021pServProfDrvCfg` needs a class 280 traffic descriptor or is the command 65 bug of `uapi/omci_gemflow.h` |
| `apply_olt_g()` | `OltGDrvCfg` | ToDInfo to command 70 |
| `apply_mbsp()`, `delete_mbsp()` | `MacBriServProfDrvCfg` | the delete sends ageing 300 s and port bridging 1; its fourth arm, `feature_api(46, ...)`, dispatches to `me_00004000.so`, which this image does not have, so it does nothing; the one-port test is `omci_is_one_pptp_eth_uni_number_in_bridge`; the zero-ageing default is the `li v0,300` / `movz` pair |
| `bdgconn_add()` | `omci_wrapper_activeBdgConn` | `us_dp_flow` starts as `us_flow`, as `omci_UpdateUsDpFlowId` does |
| `bdgconn_rebuild()` | `MIB_TreeConnUpdate` | read off `omcicli dump conn` on ISP1 |

## Status

Every phase of the original build-out (southbound probe, MIB extraction,
identification, service programming including bridge connections, and the
CLI) is complete and covered by `qemu-test.sh` / `ontest.sh` / `bdgtest.sh`.
`drv-test.sh` pins what omcid asks of the switch driver for a whole
provisioning session of each ISP (test/fixtures/omci-session-isp*.txt,
rebuilt from omcid logs by `tools/session-from-log.py`), call for call, against
a test build that logs every driver call; a change to `respond/drv.c` or
`respond/apply*.c` that is meant to be a refactor must leave both goldens
identical.
There is no outstanding design work tracked outside this file and the
per-binary source comments.
