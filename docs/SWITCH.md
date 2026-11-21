# The switch core

Evidence behind the switch-core code in `kernel/extra/drivers/net/ethernet/odi/`:
what each register write was checked against, and what was left out and
why. The code comments say what a write does; this file says how it was
established. Register names are those of `odi_switch_hw.h`.

## Register window

The switch-core register file is at physical 0x1B000000. The stock
driver never ioremaps it: it dereferences the KSEG1 alias 0xBB000000
(0xBB000000 & 0x1FFFFFFF is 0x1B000000), and every register offset in a
capture (`CLASSIFY_SETUP`, `ACL_PORT_ENABLE`, `TABLE_CMD`, the GPON and
PON queue ranges) is a byte offset from it. Single reads on a live stick
confirmed it: `CLASSIFY_SETUP` at 0x1b01600c, `ACL_PORT_ENABLE` at
0x1b015040, `FLOOD_BCAST_PORTS` at 0x1b01c020 and `TABLE_CMD` at
0x1b012000 all read plausible values. `odi_switch.c` ioremaps the
physical base and maps only the part in use (`ODI_SWITCH_MMIO_SIZE`, up to
the PON queue block at +0xF00000).

0xB8000000 is a real window on this chip too, the SoC chip-control
registers, and much smaller. A switch-core offset added to it lands in
address space no device claims, and the first such write
(`CLASSIFY_SETUP`) stalled the whole SoC with no timeout and no clean
reset. The CPU-port NIC at 0xB8012000 (`odi_nic.c`) is a separate window
with its own mapping and never goes through the switch-core accessors.

## Register replays

Four captured write sequences are replayed rather than derived, all
through one loop, `odi_replay_run()` (`odi_replay.c`, with the record type
in `odi_replay.h`):

| sequence | source | register record | also |
|---|---|---|---|
| board init | `odi_board_data.c`, compiled in | write | SoC |
| sdkinit, one verb at a time | `sdkinit.bin` | read, then write | SoC, table rows, table fills |
| module load | `modload.bin` | read, then write | table rows, table fills |
| GPON boot init | `gpon_init.bin` | write, serial-number words substituted | table rows |

The read before a register write is the full-mask read-modify-write the
capture shows (`reg = (reg & 0) | value`). It is kept as a real read: it
changes nothing a register ends up holding, but without it the bus
traffic differs, and a clear-on-read register in a replay would see one
read fewer. Dropping it would need its own trial.

A SoC record carries the KSEG1 address of a SoC-window register and goes
through `odi_soc_write()` (`odi_soc.c`), which accepts only the offsets on
its one allowlist and logs any other. The replays write four of them
(clock/reset enable, IP enable, two GPIO registers).

A table fill (a call record) stands for one uniform sweep of the capture:
a range of rows of one or more tables, every row written cleared. The
driver writes the rows itself (`odi_switch_replay_call()` in
`odi_switch_cf.c`, through the `call` hook of the replay options), in
the order the capture wrote them. The record carries the fill, the first
row, the row count and, for the CF fills, the first row of pattern 1, so
what it writes does not depend on an earlier replayed write. There are
three:

| fill | rows | each row | where |
|---|---|---|---|
| `VLAN_MEMBERS` | 1 .. 0xfff | `VLAN_MEMBERS` to the sentinel `0xf0`, as cmd 51 sweeps it | sdkinit `vlan` |
| `CF_RULES` | 0 .. 255 | rule and mask cleared, pattern 0 (`CLS_RULE_A`, `CLS_MASK_A`) | sdkinit `classify` |
| `CF_RULES_ACTIONS` | 0 .. 255 | rule and mask cleared, the A tables below the first pattern-1 row and the B tables from it, then both action rows | sdkinit `classify` (pattern 1 from row 1), module load (from row 32) |

These sweeps were 5,631 of the 7,429 sdkinit records and 1,024 of the
module-load records. The generators still write every row; `replayblob.py
compact` turns each sweep of 16 rows or more into a fill, and `replayblob.py
expand` writes the rows back, which for the shipped files gives the
generator output byte for byte (`test/replayblob_fill_test.sh`). The rest
of the sdkinit `vlan` verb stays as captured: row 0 is written before nine
register writes and keeps its own record, and row 1 is written again after
the fill. The replay log counts a fill as one record (`vlan` 157, not
4,251).

### Board init

The stock board code sets up the switch core, the LED controller and the
I2C master before the first `/proc/odi_init` verb. `odi_board_data.c` is
that window of a clean ISP1 stock boot capture: 88 writes, write-only,
none dropped by the tracer, transcribed in capture order, with the three
SoC writes (0xb8000044, then 0xb8003324 and 0xb8003328) in position
between the switch-core writes around them. It is compiled in because it
runs at board init, before any filesystem, and is small and fixed enough
not to need a generator. `test/odi_board_test.sh` diffs the replay, write
for write, against the same window in
`test/fixtures/isp1-260923-g4-board-init-filtered.txt`.

## Platform init

`odi_switch_init_platform()` (`odi_switch_platform.c`) makes the switch-core
settings the stock OMCI kernel modules made when they loaded. rcS runs it
once through `/proc/odi_omci`, after the odi_init main loop (whose `switch`
verb writes the baseline these steps narrow) and before `omcid` starts,
which is where the stock image ran it. Each step is a read-modify-write of
the fields it owns. Board ports: UNI 0, PON 2, CPU 3; port 1 is unused.

- **classify**: `CLASSIFY_SETUP.US_NO_MATCH_ACTION = 1` (0 normal, 1 no PON
  match required, 2 drop). A running stock stick reads `0x1c09`: the field
  is already 1 there, `PON_SELECT` is 1 and `PATTERN1_COUNT` is `0xe0`. A
  fresh boot of ours reads `PATTERN1_COUNT = 0x80`, the value the `switch`
  verb writes. `PATTERN1_COUNT` is how many rows at the top of the 256-row
  table follow the second CF rule layout (pattern 1): row `i` is pattern 1
  when `i >= 256 - PATTERN1_COUNT`, the rows below follow pattern 0. It is
  counted from the top, so 128 is the one value where it is also the first
  row of the second layout; at the stock 224 pattern 1 starts at row 32. The
  stock module-load capture supports this: with `0x1c09` in place it sweeps
  rows 0-31 of the first rule and mask tables and rows 32-255 of the second.
  What is not settled: whether the stock value is a boot constant or moves with
  provisioning (the module-load capture writes `0x1c09` three times and never
  another value, and the replay overwrites it with 0x1009), and what pattern 1
  does to the S-tag template (above). It is not written here.
- **acl clean**: every row of the three ACL tables (96 each: the rule
  pattern, kind 2 rows 128-223; the rule mask, kind 2 rows 0-95; the
  action, kind 3) is read back, written all-zero and read back again,
  before the next step turns ACL ingress on. The table RAM survives a
  reset and a reboot: after a power cut it holds random words, and after
  a warm boot from the stock firmware it holds that firmware's rules (a
  stick back on this image after a stock boot showed a stock rule 3,
  matching 192.168.0.1, next to the module-load replay's rule 1). A rule
  is valid when bit 144 of its pattern row is set (bit 16 of word 0 in
  `odi_switch_table_write()` order); the module-load replay writes rule 1
  with it set. The step logs what it found and what it left:

      odi_switch: ACL rows before the clean: 2/96 rules valid (first 1), 4/96 rows not empty
      odi_switch: ACL rows clean after 1 pass(es): 0/96 rules valid, 0/96 rows not empty

  A row still not empty after a second pass, or a read that timed out,
  gives `ACL rows NOT clean after 2 passes` with the counts; the boot
  goes on. The module-load replay then writes rule 1 (pattern, mask and
  action), the only ACL row this image uses.

  The L2 table is emptied after the platform steps, from
  `odi_switch_boot_init()` (`odi_switch_l2_clean()`): the table, the CAM
  included, survives a warm restart, so the static groups of the stock
  firmware, an earlier pin of a MAC that changed or a row in the CAM would
  still be there. The CAM is turned on first (the **l2** step below),
  because a row in the CAM is out of reach with it off. Every row the
  engine reports in use is deleted by its own words (below), learned and
  static, in up to two passes, and the log says what was found:

      odi_switch: L2 table clean: 3 row(s) (1 static) left by the previous run removed

  Learned rows come back as traffic flows, and the stick's own pins are
  written again at the end of the boot (`odi_switch_pin_sync()`). The flush
  (`L2_TBL_FLUSH_CTRL`, `0x017024`) is not used: it keeps static rows.
  A row left after the second pass is logged as a warning and the boot goes
  on.

  A delete is written with the words the row itself reads back, VALID
  cleared, not with the key alone. Another open implementation measured
  that a hashed delete removes a row only when the rest of the word agrees,
  and that a key-only delete left a row behind the CPU port in place; the
  stick's pin is such a row. Multicast deletes (igmpd) still send the key
  alone, which was checked on a stick.
- **acl**: `ACL_PORT_ENABLE`, one bit per port. The `switch` verb clears
  every port; the stock stick reads `0x5` (UNI and PON). Two writes, one per
  port, as the stock module load makes them.
- **vlan**: `VLAN_SETUP.FILTER_ON = 0`, `PORT_VLAN_INDEX` words 0 and 1
  written 0 (a plain write of the whole word, no read-modify-write; each
  word holds two ports, 12 bits each, so this is the PVID of all four
  ports), the port-1 bit of `VLAN_INGRESS_CHECK` off, every port an SVLAN uplink,
  `SVLAN_SETUP.FILTER_ON = 0` and `UNTAGGED_ACTION = 2` (port-based SVID).
  Before this step a fresh boot reads `VLAN_SETUP 0x1`, `SVLAN_SETUP 0`,
  `PORT_VLAN_INDEX 0x1001`; after it `0x18`, `0x09` and `0`, the values of
  the stock stick. `VLAN_INGRESS_CHECK` reads `0x0f` on a fresh boot and
  `0x0d` on the stock stick; no capture shows the write, but port 1 carries
  no traffic on this board, so turning its check off cannot change
  forwarding and matches the stock state.
  `PORT_VLAN_INDEX` is two ports per word, not one: word 0 (`0x01300c`)
  holds ports 0 and 1, word 1 (`0x013010`) ports 2 and 3, and the
  `0x1001` of a fresh boot is two 12-bit fields. Indexing it as base + 4 *
  port would put port 2 on `0x013014`, which is why the platform step
  writes words 0 and 1, not ports 0 to 3. The cmd 51 sequence writes the
  same two words whole, so it also sets the PVID of ports 2 and 3.
  Not written, because its stride is not decoded: the per-port SVID
  (base + 4n lands port 2 of `PORT_SVLAN_INDEX` on `SVLAN_SETUP`).
- **l3 off**: `L3_GLOBAL_CTRL` (0x01106c) bit 0, the L3 engine's enable,
  cleared when set. It reads `0x4` (off) after a power-up and on a stick
  running this image; it survives a warm restart, so a firmware that
  enabled the engine would leave it routing. Nothing is written when the
  bit is clear. The L3 tables are not touched: with the engine off they are
  not read.
- **l2**: `L2_LOOKUP_SETUP.CAM_OFF = 0` (a disable bit). The ageing fields
  of the same register belong to cmd 62. No stock reading exists.
- **cf sweep**: every `CLS_RULE_B`, `CLS_DS_ACTION` and `CLS_US_ACTION` row
  (256 each) written all-zero. Bit 16 of word 0 is the valid flag in every
  CF row decoded, so an all-zero row is an invalid one. `CLS_MASK_B` is left
  alone, as a delete leaves it. The downstream default-forward entry at row
  255 is not written here: its action word is not decoded (the module-load
  replay carries the captured row).
- **reserved vid**: `VLAN_SETUP.VID0_MODE = VID4095_MODE = 1` (tag), as the
  stock stick reads, and `SVLAN_SETUP.PRIO_SOURCE = 1` (C-TAG priority).
- **host pause**: `PORT_FORCE_ABILITY(0).TX_PAUSE = 0`, then
  `PORT_FORCE_SELECT(0).TX_PAUSE = 1`: the host port never sends PAUSE
  frames to the host. Ours, not the stock image's; see
  [Host port PAUSE](#host-port-pause).
- **pon isolate**: the CPU port and the extension ports cleared from the
  PON port's isolation mask (`PORT_ISOLATION`, port 2), so a frame from
  the fibre is never forwarded to the CPU. Ours, not the stock image's;
  see [PON port isolation](#pon-port-isolation).

Left out altogether:

- ACL template index 1 as the stock module load shapes it (DMAC, SMAC,
  C-TAG, GEM index). `SW_0x015008` is addressed by a template index and a
  field slot, and how the two combine into an offset is not decoded. The
  `switch` verb leaves index 1 with a different field set, so an ACL rule
  that assumed the module-load shape of index 1 would match the wrong
  fields.
- The number of ACL rows the stock stack reserves for its own entries. It
  is read from state there is no way to read, and nothing here allocates
  ACL rows yet.

## Module-load replay

`modload.bin` is the switch-core writes the stock OMCI kernel modules
made while loading, in capture order: 716 register writes and 1032 table
rows, 1024 of them the CF sweep, which the file holds as one table fill
(see [Register replays](#register-replays)). The capture has 1,871 more
register writes, in the GPON MAC window (0x700000-0x706fff): a GPON MAC
bring-up with its own `PONMAC_RESET` pulse, the same sequence
`gpon_init.bin` starts with. `gpon_init.bin` resets the GPON MAC again at
the first `gponact` and writes every one of those registers after the
reset (itself, or through the driver's table clean), so they are not in
the file: `tools/regtrace/replayblob.py trim-modload-gpon` takes them out,
and refuses to drop a register the GPON init does not write again.
`tools/regtrace/mkmodload.py` generates it from that capture and
tags each record with its family (`category`: registers, the CF sweep, the
CF rules written over the sweep, the VLAN row, the L2 unicast row, the ACL
rows; `reg_group` for a register: its register family). The families were
the selection keys of a boot-time mask while the replay was bisected. The
production selection is now applied to the file itself, with
`tools/regtrace/replayblob.py filter` and the mask that every boot used
(`PRODUCTION_MODLOAD_MASK`), and the kernel replays every record:

- every register and every table row as captured, except
- the three LUT flood masks (`FLOOD_BCAST_PORTS`, `FLOOD_UNKN_MCAST_PORTS`,
  `FLOOD_UNKN_UCAST_PORTS`), which carry `0xf`, the CPU port included,
  where the capture has `0x7` (three ports, CPU excluded). Replayed with the
  captured value, broadcast from the LAN stopped reaching the CPU while
  unicast still passed. Right after the replay `odi_switch_init_modload()`
  puts the unknown-multicast mask back to `0x7`: the CPU needs broadcast
  (ARP) but no unknown multicast (no IPv6 in the kernel, no group joined on
  the stick, the reserved 01:80:C2 groups have their own actions), and with
  IGMP snooping off every IPTV group is unknown, so a stream would be copied
  to the CPU at stream rate. The unknown-unicast mask is cmd 64's.

The families stay in the records for review; nothing selects by them.

The table-engine writes of the capture (`0x012000`-`0x01202c`) are not in
the file: they are the handshake of the rows the capture also records as
table ops, which `odi_switch_table_write()` performs itself. Records are
not grouped by table because the stock sweep interleaves four CF tables per
row, and the order is kept as the hardware saw it.

The downstream default-forward rule is in this file: `CLS_RULE_B[255]`,
`CLS_MASK_B[255]` and `CLS_DS_ACTION[255]`, a wildcard downstream rule
whose action is force-forward (`UNI_ACT` 1).

## GPON boot init

`gpon_init.bin` is the GPON MAC bring-up the PON steps `gpondrv` to
`gponact` made in the capture, replayed once per boot by the first
`gponact` (`odi_gpon_init.c`), after `gponsn` has given it the serial
number. Early in it the table pulses `PONMAC_RESET.RESET_REQ` (0x70000c
bit 0). The replay stops after the record that releases it and, before
it goes on:

1. **waits for the reset.** It reads `PONMAC_RESET` until `RESET_COMPLETE`
   (bit 8) is set, at most 20000 reads 1 us apart, and logs the number of
   reads (`odi_gpon: GPON MAC reset complete after N reads`), or an error
   with the register value when the bound runs out. The stock driver
   waits for the same bit; a register capture of its bring-up shows about
   a hundred reads.
2. **cleans the GPON tables.** Every `DSF_GEM_FLOW_TYPE` word (128) is
   written to 0, and every row of the GEM-port CAM (128 rows,
   `DSF_GEM_CAM_CTL` 0x701100), of the Alloc-ID CAM (32 rows,
   `DSF_ALLOC_CAM_CTL` 0x7010c0) and of the downstream multicast MAC
   filter (256 rows, `DS_MC_FILTER_CTL` 0x704084, the same control word
   layout) is cleared with a request of `OP` 3, each polled for `DONE`
   with the bound of every CAM poll.
3. **reads every row back.** A request of `OP` 2 per row sets
   `ROW_VALID` (bit 13) when the row still holds an entry, with the entry
   in `DSF_GEM_CAM_RDATA` / `DSF_ALLOC_CAM_RDATA` / `DS_MC_FILTER_RDATA`
   (0x704090); the flow type words are read directly. The same read runs
   before the clean, so the log shows what the boot found and what it
   left:

       odi_gpon: GPON tables before the clean: GEM-port CAM 120/128 valid, Alloc-ID CAM 32/32 valid, flow type 119/128 set
       odi_gpon: GPON multicast filter before the clean: 0/256 rows valid (row -1 0x00000000); self-test words 00008000 00008000 00008000 00008000 00008000 00008000 00008000 00008000
       odi_gpon: GPON tables clean after 1 pass(es): GEM-port CAM 0/128, Alloc-ID CAM 0/32, flow type 0/128, multicast filter 0/256

   The multicast filter line also carries the eight words of
   `DS_MC_FILTER_BIST` (0x70428c), read and never written. They are not
   filter content: their fields are the built-in self-test status of the
   filter RAM, the same layout as the words beside them for the other
   downstream GEM RAMs, and 0x8000 is `ALL_BIST_DONE` alone, the test run
   and passed. A word with `BIST_FAIL` (bit 7) or `DRF_BIST_FAIL` (bit 9)
   set, or `ALL_BIST_DONE` clear, adds "(a self-test failed or never
   ran)". The register list carries no reset values, so what a word
   reads straight after a power-up is not known yet. The other fields
   start a test, which is why nothing writes them.

   A row still set after the clean is logged as an error, with the first
   such row and its entry, and the whole clean runs once more. If rows
   survive that too (or a CAM request never finishes), the replay stops
   there and the ONU is not activated:

       odi_gpon: GPON tables NOT clean after 2 passes (-5): GEM-port CAM ... multicast filter 0/256 valid (row -1); the PON is NOT activated, a reboot or another gponact tries again
       odi_gpon: gponact: the GPON tables are not clean (-5), not activating

   The `gponact` verb fails with what the clean returned (`-EIO` for
   rows that survived, `-ETIMEDOUT` for a request that never finished),
   so the boot crumb reads `pon step gponact: ret -5` for `-EIO` and
   `ret -110` for `-ETIMEDOUT`. `/proc/odi_gpon/status` shows state O1,
   which is the driver's own state machine and says nothing of why: the
   reason is the two lines above, in the kernel log. A row left in the
   Alloc-ID CAM is another ONU's upstream timeslot answered by this one,
   so a stick that stays off the line is the smaller harm: it costs this
   subscriber the service, the other costs the neighbours theirs. Up to
   the reset release the table writes nothing in the GPON MAC window but
   the reset pulse itself (records 0 to 361: the PON queues,
   `PON_SERDES_MODE`, the frame length and other switch-core registers),
   so the MAC is left as the reset left it: nothing has written an ONU
   state, enabled its interrupts or put a serial number in its PLOAM
   slot. One of those records opens the frame length check of the PON
   port (`MAX_FRAME_LEN_1` 0x7ef, record 351; the rest of the table
   closes it again at record 386, until O5): the failed replay writes it
   back to 0, so the port drops every frame, as on a deactivated ONU.
   The driver does not retry. The next `gponact` runs the whole replay
   again from its first record, reset and clean included: by hand
   (`echo gponact > /proc/odi_init`), at the next boot, after an omcid
   respawn (`omci-respawn-reprovision.sh`) or in a re-registration omcid
   makes for a changed identity (`apply.sh omci`). A `gpondeact` before
   that one (both of the last two write it first) does nothing while no
   boot replay has completed: there is nothing to tear down, and it
   writes nothing into the GPON MAC.

Then, on clean tables only, the rest of the table runs: it programs this
ONU and activates it (`DSF_ONU_STATE` leaves O1 in its last records, after
the serial number is in the PLOAM slot).
Every row of ours (the ONU-ID row, the OMCC GEM port, the T-CONTs and
flows) is written later, as the OLT assigns it, so all of them land in
clean tables.

Why: these tables are on-chip RAM. A warm reset keeps whatever the
last session left in them, which is this ONU's own, still valid entries.
A power-up leaves whatever the RAM came up with: read back on a stick after
a power cut, 120 of the 128 GEM-port rows and all 32 Alloc-ID rows were
valid with random entries, and about 120 flow type words were not zero.
With 30 foreign Alloc-IDs in its CAM the ONU answers bandwidth grants meant
for other ONUs; that stick accepted about nine times the bandwidth map
entries of a warm boot, ranged to O5 and was deactivated by the OLT 21
times in 150 s, and OMCI never started. Before this clean the replay
carried its own clean of the same rows, issued right after the reset
release with no wait: on that stick the clears of GEM-port rows 0 to 119
and of every Alloc-ID row were lost, and only rows 120 to 127 came out
clear, so a block still in reset drops what reaches it. A warm reboot of
the same image kept the garbage; one boot of the stock firmware, which
waits for the reset, cleaned it.

The file no longer carries the replay's own clean of these tables (the
flow type words, the GEM-port and Alloc-ID CAMs, and the 256-entry
multicast MAC filter, issued back to back with no wait for `DONE`): it ran
again after the clean above, on tables that were already clear, and the
clean does the same with the wait, the `DONE` poll and a read-back. Nor
does it carry the stock driver's sweep of the per-flow counter index
registers (128 flows, the four indirect pairs of `odi_gpon_hw.h`): the
capture records writes only, so the read that clears a counter was never
replayed, and `odi_gpon_flow.c` writes the index itself before every
read. `tools/regtrace/replayblob.py trim-gpon` takes these 1,728 records
out of the `mkgponinit.py` output (`GPON_INIT_TRIMMED` has the exact
shapes, each matched whole and once), which leaves 800.
The boot table sets the filter up to be consulted for downstream
multicast (`GPON_GEM_DS_MC_CFG` 0x704080 ends at 0x59: no IPv4 or IPv6
force pass or drop), so a row left behind could change which groups
get through; the clean above reads it back and clears it, but what a
stick holds there after a power cut has not been seen yet.
`test/odi_gpon_init_clean_test.sh` runs
the clean against a model of the two CAMs whose reset drops writes for a
while after its release, and checks that the plain replay leaves the
garbage and this one does not.

## Command leaves

`odi_switch_dal.h` declares one leaf per group of switch-core writes an
OMCI driver command makes; `odi_switch_cmd.c` calls them. Each leaf is the
register sequence of the stock image for that command, captured on ISP1
during provisioning (`test/fixtures/isp1-260922-boot5.txt`), with the
fields that vary between instances as parameters.
`test/odi_switch_dal_test.sh` replays each leaf into a bracket and
compares it with the matching `test/fixtures/dal-cmd*.txt`, whose header
names the capture lines it came from. The stock driver names of the
commands, from the command table of the shipped binary:

| cmd | stock name | leaf | file |
|---|---|---|---|
| 10 | getTransceiverStatus | `odi_ddm_get` | `odi_ddm.c` |
| 23 | setPriQueue | `odi_sw_ponmac_queue_add` (downstream), `odi_sw_ponq_queue_add` (upstream) | `odi_switch_qos.c` |
| 24 | clearPriQueue | `odi_sw_ponq_queue_del` | `odi_switch_qos.c` |
| 25 | cfgGemFlow | `odi_sw_gpon_usflow_set` (downstream), `odi_sw_ponq_us_gem_set`, `_sidvalid_set`, `_glb_th_set`, `_sid2qid_set` (upstream) | `odi_switch_ds_gem.c`, `odi_switch_qos.c` |
| 11 | setSignalParameter | `odi_gpon_signal_threshold_set` (the SF/SD thresholds) | `odi_gpon.c`, `odi_gpon_ctr.c` |
| 30 | setPortAutoNegoAbility | `odi_sw_port_autoneg_get`, `_set` | `odi_switch_port.c` |
| 32 | setPortState | `odi_sw_port_admin_set` | `odi_switch_port.c` |
| 51 | activeBdgConn | `odi_sw_cf_add` | `odi_switch_cf.c` |
| 53 | setMacLearnLimit | `odi_sw_l2_learn_limit_set` | `odi_switch_port.c` |
| 62 | setAgeingTime | `odi_sw_l2_aging_set` | `odi_switch_port.c` |
| 67 | setUniPortRate | `odi_sw_uni_police_set`, `odi_sw_uni_shape_set` | `odi_switch_port.c` |
| 64 | setFloodingPortMask | `odi_sw_l2_flood_mask_set` | `odi_switch_port.c` |

- **cmd 62**: one write of `L2_LOOKUP_SETUP`, AGE_TICKS 3000 and
  AGE_ON_LINK_DOWN 1. The leaf is a read-modify-write, because the
  register also holds CAM_FULL_ACTION, ARP_AS_KNOWN, the two multicast
  hash-mode bits and CAM_OFF; the capture shows one write because the
  register reads 0 at that point.
- **cmd 30**: the UNI PHY registers at indirect PHY addresses 0xa400,
  0xa408 and 0xa412 through `PHY_ACCESS_CMD`/`PHY_ACCESS_DATA`: the read
  commands, each address twice, then the three values written back, with
  one more read command on 0xa400 before its write. The fields are not
  decoded, so the parameters are named by address.
- **cmd 32**: the admin state of an Ethernet UNI, `{port, state}` (ME 11
  AdministrativeState; omcid sends state 0 for a lock). It is the stock
  SDK's admin enable: a lock writes LINK (bit 4) of `PORT_FORCE_ABILITY` to
  0 and then sets LINK of `PORT_FORCE_SELECT`, so the port's link is forced
  down; an unlock clears that select bit. Bit 4 only, both ways, so the
  `TX_PAUSE` force on port 0 (the word is 0x40 unless
  `odi_switch.host_pause` is set, [Host port PAUSE](#host-port-pause)) is
  kept. The capture only ever shows the unlock, a read and a write of 0 to
  `PORT_FORCE_SELECT(0)`: both ISPs leave the UNI unlocked. On this SFP the
  UNI is the host port, so a lock takes the host link down, the stick's own
  management path included, until the OLT unlocks; OMCI does not cross that
  port. omcid sends a lock only with `OMCI_UNI_LOCK=1`; by default it
  answers the OLT's lock and keeps the port up. A lock of the PON or CPU port is refused (it would cut OMCI itself).
  The host tests compare the unlock bracket with the capture under the
  stock policy (`test/odi_host_pause_test.c` covers the lock).
- **cmd 64**: `{sel, enable, portMask}`, a lookup-miss flood mask, as the
  stock OMCI layer does it: `sel` picks unknown unicast
  (`FLOOD_UNKN_UCAST_PORTS`, 0x01c028), broadcast (0x01c020) or unknown
  multicast (0x01c024); `enable` 1 adds portMask's ports, 0 removes them,
  every other port keeps its bit; 2 (trap) is refused. omcid names one
  port per class 47 create or delete, so a delete or ME 45
  `DiscardUnknown` = 1 takes that port out, not every port. One exception
  is kept from the old fixed write: cmd 64 on the unknown-unicast mask
  clears the CPU bit, which the module-load replay sets (0xf, above) and
  the stock image never has; both ISPs run with it clear after the first
  provisioning. The capture's call (the PON port added) gives 0x7, written
  four times: the SDK writes the word once per port, and the host replay
  compares brackets write for write.
- **cmd 23** has two shapes, and the direction in its argument picks one.
  A downstream queue writes `PORT_QUEUE_MAP` (0x01c0c0; the word at
  0x01c0a0 is never written) with 0xd4, identical in the eight instances
  ISP1 sends, all before any T-CONT exists. The register holds a 2-bit
  group index per port, four ports: 0xd4 is the UNI on group 0, the other
  ports as the init left them, the result of the stock read-modify-write of
  port 0 for a queue with no drop precedence marking. That word is all the
  stock driver writes for a downstream queue; the schedule of the queue
  (strict or weighted, the weight) is ours, on top of it:
  [Downstream queues](#downstream-queues). An upstream queue,
  and cmd 21, 24 and the upstream side of cmd 25, are the upstream PON
  queues, below.
- **cmd 25** downstream is the DS GEM port CAM row and its
  `DSF_GEM_FLOW_TYPE` word (FLAGS 2, Ethernet, plus bit 0, multicast, when
  omcid sets `mcast_filter`: the GEM port of a multicast GEM interworking TP,
  as the stock OMCI layer flags it; ISP1's is 0xfff, ISP2's 0xffe), plus the slot record the AES path reads (below). `ena` 0 is
  a delete, as the stock driver reads it: the flow type word goes to 0,
  the CAM row is cleared (MODE 3) and read back (MODE 2), and only a row
  that reads back clear forgets the flow (its slot record, its readback
  row, its downstream priority, its counters); otherwise the command
  fails and the flow stays recorded for omcid to retry. omcid sends it
  for a multicast GEM port no row names any more, and cmd 0, the MIB
  reset, deletes every downstream flow the same way.
  `/proc/odi_gpon/cam` reads the rows back (`gem_cam`). The upstream side
  is below.

- **cmd 10**: one SFF-8472 DDM word, for the ANI-G Test result. omcid
  sends `{type, data[32]}` with the type (2 temperature, 3 supply voltage,
  4 bias current, 5 Tx power, 6 Rx power, the `odi_ddm.h` selectors) as a
  big-endian word at bytes 0-3, and reads the A2h word back, big-endian,
  at `data[0..1]`. The command is a DDM read through `odi_ddm_get()`,
  both bytes under `odi_i2c_lock`. Its writes are the capture's: the
  A2h device select (0x234413a in `I2C_MASTER_SETUP`, port 1), the byte
  address in `I2C_BYTE_ADDR` (0xbc) and START in `I2C_CMD` (0xc4), once
  per byte; the five captured instances are A2h 98, 104, 102, 100 and 96,
  the order omcid asks for voltage, Rx, Tx, bias and temperature.

  The driver used to replay those writes with no completion poll and no
  read of `I2C_READ_DATA`, taking 0xbc and 0xc4 for GPIO pin selects: the
  command returned 0 with nothing in `data`, so every ANI-G Test result
  reported 0 V, no light, 0 mA and 0 C, and the transaction it started
  was still running when it released `odi_i2c_lock`.

### UNI limits

Commands 53 and 67, and what became of the rest of the commands that used
to be refused. omcid sends them only when the OLT provisions the matching
attribute, so a port nobody set keeps the boot replay's state (every port
may learn 1088 addresses, no rate). Registers read on a stick and
checked against another open implementation's measurements of the same
chip:

| what | register | boot state read |
|---|---|---|
| learning limit of port p | `0x017008 + 4 * (p / 2)`, 11 bits at `11 * (p % 2)` | `0x00220440`: 1088 for ports 0 and 1 |
| addresses port p has learned | `0x017010 + 4 * (p / 2)`, the same packing | `0x2` and `0x1` with two on the UNI, one on the PON port |
| action past the limit | `0x01c00c`, 2 bits a port | 0, forward the frame without learning it |
| ingress meter of port p | `0x020010 + 0x400 * p`, bits 20..2, 8 kbit/s units | `0x001ffffe`: 0x7ffff, none; bit 1 is flow control |
| egress shaper of port p | `0x02d804 + 4 * p`, bits 19..1, 8 kbit/s units | `0x000ffffe`: 0x7ffff, none |
| second shaper register, port 0 only | `0x02d8b8`, bits 18..0 half the rate in 16 kbit/s units, bit 19 the 2.5G override | `0x0003ffff` |

- **cmd 53**, `{port, limit}`: the limit field of the port, a
  read-modify-write (the other port of the word is kept). A limit at or past
  the table size is "none". 0 is refused, not taken as "learn nothing": it
  is learning off, which nobody asked for. The action register is never
  written, so a frame from an address past the limit is still forwarded,
  only not learned; a drop there would make the limit an outage. The PON and
  CPU ports are refused: a limit on either would cut OMCI or the stick's
  management.
- **cmd 67**, `{port, dir, kbit/s}`: direction 1 (the ME 47 inbound
  descriptor, traffic entering the bridge from the UNI) is the ingress
  meter, direction 2 the egress shaper. The shaper of port 0 only enforces
  its rate when `0x02d8b8` holds half of it (six rates from 8 kbit/s to
  20 Mbit/s all ran at line rate with the first register alone, on the other
  implementation's board), so both are written, the override bit left as it
  is. "No limit" is exactly the reset words. A rate below 1 Mbit/s is
  refused and the port keeps its previous one: such a descriptor is not a
  service plan, and a limit that low would cut the host and the stick's own
  management off, so a refusal fails open. The rate unit of the driver
  (8 kbit/s) is the register's, so the kbit/s are divided by 8 and a rate
  that is not a multiple is rounded down.
- A MIB reset (cmd 0) puts back, on the ports whose limits were set, the boot
  state; the stock stack puts the learning limit back to a global default on
  a bridge port delete, which omcid does not recover, so the reset is where
  the limits end.
- **cmd 61 and ME 50.** `getBridgeTableByPort` returns the number of unicast
  rows the L2 table holds for a port. The stock driver hands the rows
  themselves through a mapped buffer of its character device, which a
  netlink command cannot do, so they come through a command of ours,
  `OMCI_BRIDGE_TBL_CMD` (0x101, `uapi/omci_bridgeport.h`): a page of 30
  entries at a time, each the 8 bytes ME 50 carries (an information word of
  age in seconds, learned or static, blocking; then the MAC). The age is the
  row's 3-bit counter times the ageing time over 7, as the stock driver
  scales it. omcid reads it on every Get of ME 50's table attribute, so the
  OLT sees what is learned then; which port is asked follows the bridge
  port's TP: the UNI, the CPU port for an IP host, the PON port for anything
  else. The OLT thereby learns the MAC addresses behind the stick, which is
  what ME 50 is for.
- **cmd 63** is the stock driver's "the UNIs bridge to one another": it
  rewrites the isolation masks of the UNIs, the very masks
  [PON port isolation](#pon-port-isolation) owns. 1, which is what is
  already true, succeeds without a write; 0 is refused.
- **cmd 40** with a pause time of 0 (flow control off) succeeds, the host
  port already never sends PAUSE; anything else is refused, that is the
  operator's switch (`odi_switch.host_pause`), not the OLT's.
- **Still refused**, each with its reason in the log line of the command:
  34 (max frame size: the switch has two frame-length profiles, profile 1
  is the PON port's at 2031 bytes and the host, UNI 1 and the CPU port
  share profile 0 at 16368, so a per-UNI limit would move the CPU's), 52
  (DSCP remap; the path from the priority to the P-bit of an upstream frame
  is not understood), 54 and 55 (MAC filters, ACL rules; omcid does not send
  them), 56 (S-VLAN TPID, not sent), 59 and 60 (the dot1 rate limiters, an
  ACL rule and a meter per GEM port, not built; omcid sends them for class
  298) and 71 (UNI QoS, not sent).

### Downstream queues

The queues of the UNI port, toward the host: ME 277 rows whose RelatedPort
names the UNI (entities 0..7 on a stick, priority 7 down to 0; the next
eight, the VEIP's, are not scheduled). The OLT provisions them with Weight,
and omcid sends each row as cmd 23, direction DS, when the OLT sets it.
The stock driver writes `PORT_QUEUE_MAP` for it and nothing else (the same
word for ISP1's eight rows, whatever their weights), so before this the UNI
had all eight queues strict, weight 1, whatever the OLT asked. Registers,
read on a stick and checked against another open implementation's
measurements of the same chip, for port p and queue q:

| what | register | boot state read |
|---|---|---|
| priority to queue | `0x01c0b0 + 4p`, eight 3-bit entries, priority i at bits 3i+2..3i | port 0 `0x00fac688`, the identity (the stock init writes it; the chip's reset is 0, every priority to queue 0) |
| queue type | `0x02d85c + 4p`, bit q set = weighted | 0, all strict |
| weight of queue 0 | `0x02d824 + 4(p/2)`, 16 bits, an odd port in bits 31..16 | 1 |
| weight of queues 1..7 | `0x02d82c + 12p + 4((q-1)/3)`, 10 bits, three to a word | 1 |
| weight unit | `0x02d800` bit 17 clear: bytes (set: packets) | `0x00003fff`, bytes |

The scheduler serves every strict queue before any weighted one, the
highest queue number first among the strict ones, and shares what is left
among the weighted queues in the ratio of their weights. Two weighted queues
split the bytes as their weights, with a granularity (fields 1 and 3 gave
29/71 %, 4 and 12 gave 25/75 %), which is why the driver writes four times
the OMCI weight: 255, the largest, is 1020, inside the 10 bits.

What cmd 23 does with a downstream queue, after the stock write:

- the queue is number `index`, the low 16 bits of RelatedPort, of the UNI
  port `owner`. `index` of 8 or more, an owner that is the PON or CPU port
  (omcid names the PON port for a VEIP queue, as its flow does, so that it
  cannot overwrite a PPTP queue of the same priority) or a port that does
  not exist: nothing is scheduled, and the call succeeds as before.
- Weight 2 or more is weighted, at four times the weight; Weight 0 or 1 is
  strict at the reset weight. That is the stock driver's own reading
  (`wrr = Weight >= 2` in the descriptor it builds) and the only one the
  hardware can express: it has no weighted queue above a strict one.
- a register is written only when it changes, so rows at the default write
  nothing and an OLT that provisions nothing leaves the boot state alone.
- the port's priority map is made the identity if it is not, once, so that
  a queue's number is the priority of the frames that reach it.
- a MIB reset (cmd 0) puts every queue it scheduled back to strict at weight
  1. The priority map stays as it is.

Which number is served first is read from the OLT, not from a spec: the
OLT of ISP1 (and the one on the stick this was read on) gives Weight 0 to entities 0 and 1 (priorities 7 and 6) and
30, 24, 19, 14, 9, 4 to entities 2 to 7 (priorities 5 down to 0). Read with
the queue number as the priority, that is two strict queues on top and then
weighted queues whose share falls with the number, which is what such a
template is for. The other reading (OMCI priority 0 first, so queue 7 - p)
puts the two strict queues below the weighted ones, and the hardware serves
strict first, so it would starve the queues the OLT gave the weights. The
flow descriptor's `ds_prio` (cmd 25) is that other reading, the stock
number, and the switch driver does not use it.

**Which queue a frame takes.** The queue is the one the frame's internal
priority maps to (the identity above), and a frame from the PON port gets its
priority from the sources the switch weighs (`0x01c0f4`, table 0 of the two
that `0x01c0fc` bit 0 selects). On a stick table 0 reads `0x00000e03`: the
port default (`0x01c0f0`, 0) has weight 3, the PCP and the DSCP weight 0 (not
consulted) and the fourth source, the one a classifier or ACL action assigns,
weight 7. So, with nothing assigning, every downstream frame has priority 0
and takes queue 0 whatever GEM port or PCP it carries.

G.988 names the queue by the GEM port, not by the frame's PCP: ME 268
attribute 7 (DsPriQPtr) points at the ME 277 row, whose RelatedPort gives the
UNI and the priority. A downstream CF row can assign the priority (action
word bits 59..57, enabled by bit 56; the per-row action class that gates it,
`CLS_ACT_EN` bits 4:0, reads `0x3f`), and that is how a frame is steered.
Making the PCP a source instead (weight of source 1 above the port's) would
queue by the frame's PCP, which is not what the OLT provisioned, and it would
change the upstream and host-side priority of every frame as well: it is
not done.

- cmd 25 (downstream) carries the queue of the flow: `ds_pq_pri`, the low half
  of RelatedPort, and `ds_port`, the UNI. A flow whose queue is a UNI's, with
  a priority of 0..7, has that priority; one whose queue is the PON port's (a
  VEIP's) or past the eight queues has none. The same call makes the UNI's
  priority map the identity if it is not (as cmd 23 does), so priority n is
  queue n.
- cmd 51 (a bridge connection) names the flow of its GEM port (`ds_flow`).
  The downstream CF row of the connection carries the priority of that flow's
  queue in its action word, whichever of the two arrives first: a cmd 25 that
  changes the priority of a flow rewrites the action word of the rows on it
  (the action table only, nothing else of the row), and one that does not
  change it writes nothing. A flow with no priority leaves its row as it was.
- omcid sends a flow again when the OLT writes the RelatedPort of the queue
  its CTP points at after the CTP was created, since the priority is in that
  attribute.
- a MIB reset (cmd 0) takes the priority off every row. The OLT's next
  provisioning puts it back.
- `odi_switch.ds_steer=N` leaves every row without a priority, the stock
  behaviour (all downstream traffic in queue 0).

What was measured on a stick (register reads and writes of CF rows on a
running line, each restored by the script that wrote it):

- a downstream row with the priority assigned moves its frames to that queue:
  the per-queue page counters (`0x02d038 + 4(8p + q)`, peak in bits 25..16;
  they only grow, a write does not clear them) of port 0 showed a queue the
  line had never used take pages as soon as a row assigned it. All the
  downstream rows given priority 3 took queue 3 (peak 2 pages, from 0); rows
  given distinct priorities took 1 for the data row (peak 9 pages) and 6 and
  7 for two other services' rows (1 page each), the rows of the priorities
  nothing matched staying at 0.
- a download of about 300 Mbit/s (what that line gives) ran
  at the same speed with the data row on priority 7 as with nothing assigned,
  and the queue-7 page peak matched the one queue 0 had reached before.
- the field of the classifier rule that was read as "TOS / GEM index"
  (rule bits 30..23) is not the GEM flow of a downstream frame: rows that
  matched flow 5 (the data flow) or 2 on those bits matched nothing, and
  single-bit rows showed the bits to read `0x7f` for the frames of that
  flow. So a row cannot be told apart by GEM port, and a steered row follows
  the GEM port of its bridge connection, not the one a frame arrived on. The
  two are the same wherever the connection maps one GEM port to one VLAN
  match, which is what the OLTs seen provision. Two GEM ports feeding one
  match would need an ACL rule on the GEM flow index (field `0x0a`, which
  another implementation measured to match one flow), with an action that is
  not decoded.
- a downstream row that forwards to an empty port mask drops the frame, which
  is how those rows were probed.

`odi_switch.ds_sched=N` turns the schedule off (the stock driver's
behaviour); it is read at each cmd 23, so a write to
`/sys/module/odi_switch/parameters/ds_sched` covers the next provisioning
and does not undo what is programmed. `odi_switch.ds_steer` is read at each
cmd 25 in the same way. `test/odi_switch_dsq_test.c` covers the schedule's
registers, `test/odi_switch_dssteer_test.c` the priority of the rows, and the
boot golden shows the stream.

### Upstream PON queues

cmd 21, 23 (upstream), 24 and 25 (upstream) program the upstream scheduler
from their arguments. Nothing in them is replayed. The register meanings
match the stock driver's queue add, queue delete and upstream flow add and
delete, with the register addresses and fields of the chip's register map.
The capture checks the add side: given the arguments omcid sends on ISP1
(five T-CONTs, one queue each, weights 1, 30, 24, 4, 4), the code writes
every word of the ten upstream cmd 23 and cmd 25 brackets of the stock
capture (`test/odi_switch_cmd_test.sh`), which were replayed word for word
before. The capture has no queue or flow delete.

The window `PONQ_COUNT_MASK` (0xf020a8 + 4 n) holds these registers:

| n | register | what |
|---|---|---|
| 15 | `DRN_CMD` | drain one queue: queue << 3, QUEUE_MODE (bit 2), start (bit 1); bit 0 reads 1 while it runs |
| 20.. | `PON_SID2QID` | the queue of each upstream flow, 7 bits, 4 flows a word |
| 37.. | `PON_SIDVALID` | one bit per upstream flow |
| 60+q | `PON_QID_CIR_RATE` | CIR of queue q, 18 bits |
| 125+q | `PON_QID_PIR_RATE` | PIR of queue q, 18 bits, 0x3ffff no limit |
| 190+t | `PON_SCH_QMAP` | the queue mask of T-CONT t |
| 207 | `PON_TCONT_EN` | one bit per T-CONT |
| 208.. | `PON_WFQ_TYPE` | one bit per queue: 0 strict, 1 WFQ |
| 212.. | `PON_WFQ_WEIGHT` | 10 bits per queue, 3 a word |
| 235 | `PON_SID_GLB_TH` | global PBO thresholds, ON 28:16, OFF 12:0 |
| 236.. | `PON_SID_RPV_TH` | per-flow PBO thresholds, same fields |

There are 65 queues. T-CONTs 0-7 share queues 0-31 and T-CONTs 8-15
share 32-63; the queue mask holds the queue's number within its group of
32 (logical), so physical = 32 * (t / 8) + logical. T-CONT 16 is the OMCC,
on the default Alloc-ID, with queue 64: the module-load replay sets its
mask (+206 = 1), its enable bit (+207 = 0x10000) and its flow (64, valid,
on queue 64) before any OMCI.

- **cmd 21** answers with the T-CONT index of the Alloc-ID it is given,
  which is the row of the Alloc-ID CAM that holds it: the hardware's T-CONT
  n is the scheduler that serves the grants of the Alloc-ID in CAM row n.
  The row is looked up, never written: an Alloc-ID that no Assign_Alloc-ID
  (type 1) of the current activation placed is refused, and omcid binds the
  T-CONT once the assignment shows in /proc/odi_gpon/status. Only PLOAM
  arms the CAM, because a row decides which grants the ONU answers (G.984.3
  5.5.4: a T-CONT maps to an Alloc-ID "previously assigned to the given ONU
  via the PLOAM messaging channel"). The stock driver writes a row here,
  but only on a T-CONT Set; omcid also sends cmd 21 after every change of
  the PLOAM list, where a write would bring back an Alloc-ID the OLT had
  just taken away. Rows 0-15 are data T-CONTs; the OMCC's row 16 and rows
  17-31 (no scheduler) are refused. Word 0 goes in as the number of queues
  omcid will put on the T-CONT, and the driver reserves a block of that
  many consecutive logical queues in the T-CONT's group, first fit (the
  stock driver also first-fits; it would move the T-CONT to the other
  group when its own is full, where this refuses). Asking again for the
  same or fewer keeps the block, so the same MIB provisioned again lands
  on the same queues.
- **cmd 23**, an upstream queue, is ordinal `index` of T-CONT `owner`:
  logical queue = block start + ordinal. The first queue of a T-CONT sets
  its `PON_TCONT_EN` bit; a queue not yet in the mask is drained (a bounded
  poll of `DRN_CMD` bit 0; the driver warns and goes on if it never
  clears) and added to `PON_SCH_QMAP`. A strict queue gets type 0, then
  weight 1; a WFQ queue (`wrr`) its weight, then type 1. That order is the
  "+208 before or after +212" order the replay used to track. Then
  `PON_QID_CIR_RATE` and `PON_QID_PIR_RATE` from `cir` and `pir`, written
  one less except 0, 1 and the maximum, and a PIR of 0 with a CIR of 0 as
  no limit. Each write is a read-modify-write of the queue's field. The
  flows already mapped to the queue are made valid again.
- **cmd 24** deletes the queue: its flows out of `PON_SIDVALID`, a drain,
  its bit out of the mask, and the T-CONT disabled with its last queue,
  whose block is then free for cmd 21 to reserve again.
- **cmd 25** upstream with `ena` 1 maps flow `flow_id` to its GEM port
  (`US_GEM_PORT_MAP`), sets its `PON_SIDVALID` bit, recomputes the global
  PBO thresholds when the flow is new, and puts it on its queue
  (`PON_SID2QID`): ordinal `queue` of T-CONT `tcont`, refused when that
  T-CONT has no such queue. With `ena` 0 it is the stock delete: GEM port
  0 in the slot, the valid bit cleared, the thresholds recomputed.
  The thresholds for n upstream data flows are ON = 7904 - (n - 1) * s
  and OFF = 7744 - (n - 1) * s, with s the ON threshold of flow 0
  (`PON_SID_RPV_TH[0]`, 150 after the sdkinit replay) and ON no lower than
  2000 (OFF 1840). The five ISP1 pairs are its first five values.
- **cmd 0** (the stock resetMib, the first of the stock command enum)
  empties the upstream state: every upstream flow deleted as cmd 25 `ena`
  0 does, every queue of a data T-CONT deleted as cmd 24 does, every
  T-CONT block freed. Every downstream flow goes as well, as cmd 25 `ena`
  0 deletes one. The Alloc-ID CAM (PLOAM's), the OMCC (T-CONT 16, flow
  64, GEM row 64) and the bridge connections are left.
  omcid sends it on a MIB reset (the OLT's, the CLI's, a re-registration)
  and when it starts again in the same boot without resuming, so the next
  provisioning never meets the last one's queue blocks or flows.

omcid changes only what differs from what it programmed: a T-CONT whose
Alloc-ID and queue at every ordinal are unchanged keeps its index and its
queues, and a queue whose weight, type, marking or rates moved is sent again
in place (cmd 23 on a queue already in the mask writes no drain); a flow is
sent only when its GEM port, T-CONT or queue changed. A T-CONT that changed
is torn down (its flows, then its queues) and built again alone. Only a
change of the PLOAM Alloc-ID list tears everything down, since the CAM
rows, and so the T-CONT indexes, may have moved.

`/proc/odi_gpon/ponq` reads these registers back (every one above but
`DRN_CMD`, plus `US_GEM_PORT_MAP`) next to what cmd 21 and 25 recorded,
and `omcli check` compares them, with `/proc/odi_gpon/cam`, against what
the MIB asks for (docs/TOOLS.md). Reading the window while the line is up
is what the read-modify-write of every field above already does (#412).

The driver used to replay ISP1's words for these registers by T-CONT and
flow ordinal, which gave every other OLT ISP1's weights, drained T-CONT
4's queue when it set up T-CONTs 5 and up, put T-CONTs 8-15 on queues
their scheduler does not serve, refused a second queue on a T-CONT, took
omcid's flow delete for a create of GEM port 0, and numbered T-CONTs in
call order rather than by CAM row. Before that, the flow word was replayed
by flow id modulo 5, which unmapped flows 1-3 on an ISP with six upstream
flows (issue #42).

### Bridge connections

`odi_sw_cf_add()` is the whole sequence of one cmd 51: the CF rows it is
handed, a VLAN table sweep and a register template. Which rows, and in
which order, is decided by the command layer (`odi_switch_cmd.c`), which
derives them from the OMCI bridge rule; the leaf writes what it is given.
The plain-register part is the same, bit for bit, in all twelve ISP1
instances and in the tail of the ISP2 stock bracket
(`test/fixtures/isp2-260921-cmd51-tail.txt`), so it is replayed as a
fixed template: it is idempotent.

- Each CF row is written after the `CLASSIFY_PATTERN_SEL` word that holds
  its template bit (32 rows per word, row 254 in word 7); every row is on
  template 0, so the word is 0.
- The VLAN table is written twice. The first pass sets all 4096 rows to
  0xf0. Rows 1 and 0 are then rewritten (row 1 0x3f8ff then 0x3f800, row
  0 0xff), and the second pass writes rows 2 to 4094 row by row, 0 except
  the service rows. Row 4095 is not rewritten in any of the twelve
  instances.
- Around them: `VLAN_SETUP` stepped through a fixed series of values,
  four `VLAN_ACCEPT_FRAMES`/`VLAN_INGRESS_CHECK`/`PORT_VLAN_INDEX`
  triplets, the egress tag group, two passes of the reserved-multicast
  (`LINK_MCAST`) block with the DSCP remark and IPMC leak writes between
  them, `PROTO_VLAN_GROUP(0..3)` twice each and the 16 `PORT_PROTO_VLAN`
  slots. The capture addresses 0x013008 through the macro for
  `VLAN_ACCEPT_FRAMES(2)`; the register there is `VLAN_SETUP`, whose four
  fields decode the values written.

#### Priorities an 802.1p mapper discards

G.988 9.3.10 gives each priority of an 802.1p mapper (class 130) an
interworking pointer, and the null pointer 0xffff discards the frames of that
priority. omcid follows it only where the mapper sends its priorities to more
than one GEM IW TP: the rules of each target then carry just the priorities
the mapper gives it (`mapper_splits()` in `src/omci/respond/apply_bridge.c`).
A bridge port on a mapper (TP type 3) whose mapped priorities all go to one target
keeps the single rule it always had, so tagged frames of a priority the
mapper sets to 0xffff still go up that GEM port. (A bridge port on the GEM IW
TP itself, TP type 5, whose mapper gives it exactly one priority, filters on
that priority, so the others do not pass there.)

This is a deliberate deviation. No OLT is known to rely on the discard, and
following it could only cost tagged traffic that works today (#306, #315,
#421). The `vlan-test.sh` case "one target with a priority unmapped" holds
it. If an OLT ever needs the discard, it can be added behind a key.

### CF rows

A CF (classification) row is three tables at one index: `CLS_RULE_B` and
`CLS_MASK_B` (two words each) hold the match, `CLS_US_ACTION` or
`CLS_DS_ACTION` (three words) the treatment, and the direction bit of the
match selects the action table. Word 0 is the most significant word, the
order `odi_switch_table_write()` takes and a capture lists. Row 255 is the
downstream catch-all of the module-load replay (force forward, no tag
change). The captured allocation puts the more specific rows at lower
indexes, which implies the lowest matching index wins; that is inferred
from the ordering, not measured.

The match is a TCAM data/care pair, not value and mask: for every bit,
RULE = data AND care and MASK = care AND NOT data, so a bit set in either
word is cared about and the two never overlap. Every service row of both
captures has this property, and the stock diag CLI prints the rows back
as databit/carebit with care = RULE | MASK. Bits over the 64-bit pair:

| bits | field |
|---|---|
| 48 | VALID (RULE only) |
| 47..32 | ethertype / inner-tag word (never cared in any capture) |
| 31 | direction: 1 downstream (from the PON), 0 upstream |
| 30..23 | TOS / GEM index (never cared). Not the GEM flow of a downstream frame, as the name had it: those bits read 0x7f for the frames of the data flow, and a row matching flow 5 on them matched nothing |
| 22..11 | VID of the outer tag |
| 10..8 | priority of the outer tag |
| 7..5 | internal priority (never cared) |
| 4, 3 | the frame carries an S-tag, a C-tag |
| 2..0 | source port (upstream rows: the UNI) |

Confirmed by value across the twelve ISP1 instances: direction, VID (10
to 14), priority (4, 5 and 0 cared, 8 not), both tag flags and the UNI
change in step with the OMCI input; the rest is layout only and never
seen non-zero. Bit 16 of `mask_w0` is not a match field: the stock driver
writes it 0 on the row an insert adds and 1 on every other row write (the
rows an insert shifts, and a rewrite in place). No effect on matching
shows in either capture; ours writes it the same way so the host replay
stays identical.

The action, 67 bits over three words (word 0 is bits 95..64):

| bits | field |
|---|---|
| 66..61, 60 | DSCP value, DSCP remark enable (0 so far) |
| 59..57, 56 | CF priority, its enable: the internal priority the frame takes, which picks its queue on the egress port. 0 in the captures; a downstream row takes the priority of its flow's queue ([Downstream queues](#downstream-queues)) |
| 55..53, 52..50 | C-tag priority source, C-tag VID source (1: the values below, the only value used) |
| 49..47, 46..35 | C-tag priority, C-tag VID |
| 34..33 | C-tag action: 0 none, 1 add, 2 delete, 3 transparent |
| 32..31 | upstream: drop/trap (0 forward); downstream: UNI action (3 forward to the port mask, every service row; 1 the row-255 force forward) |
| 30..24 | upstream: the flow id the frame is queued on (the us_flow of the bridge rule, 0..4 on ISP1) |
| 30..27 | downstream: egress port mask (bit n = switch port n) |
| 23 | upstream: take the flow id (1 on every service row) |
| 22..21, 20..18 | upstream: S-tag VID source, S-tag priority source (1) |
| 23..21, 20..18 | downstream: S-tag VID source, S-tag priority source (1) |
| 17..15, 14..3 | S-tag priority, S-tag VID (0 in every row) |
| 2..0 | S-tag action: 3 delete, 4 transparent (the two seen), 0 none |

A VLAN table row is one word by VID: bits 3..0 the member ports (bit n =
switch port n: 0 UNI, 2 PON, 3 CPU), 7..4 the members that send the frame
untagged, 8 FID/MSTI, 9 S-VLAN check IVL/SVL, 10 IVL/SVL, 17..11 the
extension port mask. The stock default row words (for example
0x0003f8ff) are not decoded beyond the member and untag nibbles our code
uses, and a later chip of the family uses a different, wider layout. A service VLAN is 0x15 when the UNI sees it untagged
(members UNI and PON, untagged on the UNI: ISP1 VID 11, ISP2 stock VID
10) and 0x5 when the UNI sees it tagged (ISP1 VIDs 10, 12 to 14). With
VLAN filtering on, a frame whose VID row does not list the ingress port is
dropped at ingress: the likely reason ISP2 discarded every downstream
frame at the PON port while this table was replayed from the ISP1
capture, where VID 10 had no row until the ninth call.

A rule that passes tagged frames of any VID -- no VID in its filter, not
untagged-only, both tag actions transparent: forward-all, tagged-only or a
priority filter, all from the class 84 forward operation -- has no VID for a
row of its own, so while it is active every row 2..4094 carries its members
(UNI and PON, tagged, 0x5), ORed into the service rows; releasing it
rewrites every row without it (`odi_switch_bdgconn.c`,
`rule_passes_any_vid()`, pinned by `test/odi_switch_bdgconn_vlan_test.c`).
No capture has such a rule; whether the hardware drops a tagged frame on a
0 row with the final `VLAN_SETUP` of cmd 51 (`FILTER_ON` 0) is not
measured, and this keeps such a frame from depending on it.

A CF row delete (`odi_sw_cf_del()`) writes an all-zero rule row, VALID
clear, and an all-zero action row. No capture shows a delete; this is the
encoding the layout implies and the one the platform sweep uses.

### Extended VLAN rules

The extended VLAN tagging operation of class 171 reaches the switch as
cmd 51 descriptors of `rule_gen` `OMCI_VLAN_OPER_EXTVLAN_GEN` (7,
`uapi/omci_bdgconn.h`). Each carries **one direction**: omcid works out the
downstream rule of an entry (the "inverse" of G.988, or what DsMode says)
itself, from `src/omci/extvlan.c`, and sends it as a second descriptor, so the
driver derives nothing here. The two halves of the rule are the two tag
positions the classifier can act on: the "outer" half is the S-tag (TPID
0x88a8) and the "inner" half the C-tag (0x8100). A descriptor that names a VID
or priority on both, that is not one direction, or that asks an S-tag action
without the S-tag priority, is refused (`-EOPNOTSUPP`, nothing written). Rows
of these rules never use the bottom region: the ordering class is how much the
row names (VID and priority 0, one of them 1, neither 2), so the more
specific row has the lower index, which is the one that wins.

What the rows rely on, and where each fact comes from. "Read" is a register
read on a stick on the current image; "other" is another open implementation's
measurement on the same chip, with its own lab OLT, which agrees with our
stock register table where the table says anything.

| fact | source |
|---|---|
| `CLASSIFY_TEMPLATE(t)`, 0x016030 + 4t, t 0..1: the tag a rule takes VID and priority from, per direction (US_VID 1:0, US_PRI 3:2, DS_VID 5:4, DS_PRI 7:6; 0 the C-tag, also the one behind an S-tag, 1 the S-tag). Both words read 0, so every rule compares the C-tag until template 1 is written | the stock register table (two words, four 2-bit fields); read; the meaning of the values: other |
| `CLASSIFY_PATTERN_SEL`: the row's template bit, 32 rows to a word. Written for every row, and now carries the bits of the 31 rows beside it | table; other. All words read 0 |
| template 1 is written 0x55 (S-tag, both directions) when the first row that compares the S-tag goes in, and never otherwise, so a line with no such rule sees the register writes it always saw | ours |
| per-row action classes `CLS_ACT_EN(i)`, 0x016038 + 4i: bit 5 stream, bits 4:0 the rest. Read 0x3f on every row looked at (0..15, 64..87, 240..255) | table; read |
| C-tag action 1 ("tag") adds a tag to an untagged frame and replaces the VID of one that has it; the VID is the assigned one when the VID source (bit 18) is set, the priority the assigned one when its source (bit 21) is, **else the one the frame had**; 2 removes the tag; 0 and 3 leave it. A translation of C-VID 410 to 1410 reads back as action word 1 0x00042c12 there and is written the same here | other |
| S-tag action 1 puts an S-tag (TPID `SVLAN_TPID_CFG` primary, 0x88a8) on a frame without one and replaces the S-tag of one with one; 2 rewrites the S-tag into a 0x8100 tag; 3 removes it; 0, 4 leave it (5 acts as 1). VID and priority are assigned under their source bits (21, 18). Rows here use 1, 3 and 4 | other |
| the S-tag is only recognised on a port in S-VLAN uplink mode: `SVLAN_UPLINK_PORTS` (0x0230c4) reads 0xf, every port | read |
| `SVLAN_TPID_CFG`: primary 0x88a8, second 0x8100 disabled (0x0230c8 reads 0, 0x0230cc reads 0x810088a8). So an added or compared S-tag has TPID 0x88a8 and nothing else (0x9100 is refused) | read |
| the classifier acts on the lowest-numbered matching row | other (an operator OLT's pass-through at a lower index shadowed a translation) |
| a frame forwarded to the PON port with no stream is dropped silently in the PON MAC, so a rule up the PON names its stream (`us_flow`) like every other upstream row, and a frame no row matches has no stream | other |
| `PON_EN` (`CLASSIFY_SETUP` bit 3) must be set for the C-tag actions to apply to frames crossing the PON port. Reads set (0x1009) | other; read |
| the last `CLASSIFY_SETUP` 12:5 (`PATTERN1_COUNT`, reads 128) rows of the table, counted from the top, are matched with a second rule layout (row `i` when `i >= 256 - count`); the fields our rows use sit in the same place in both, the S-tag template bit is not known to work there, so such a row is not placed at or past the first of them (`cf_insert()` refuses with "classification table full"). The direction is the stock sweep's split at 32 with count 224; the layout difference itself is not measured here | other; read; the placement is ours |
| in a row of the second layout on template 0, rule and mask word 0 (our `rule_w0`, `mask_w0`, "bits 47..32" with the valid bit at 48) bits 15:0 are the frame's EtherType: the bits that must be 1 in the rule row, the bits that must be 0 in the mask row, the valid bit beside them. For 0x88b5 the words are 0x000188b5 and 0x0000774a. The first layout holds other keys there, so a row writes them only when it names an EtherType. Measured on untagged frames only: 100 of 100 untagged frames of 0x88b5 pushed a tag downstream and 0x88b6 untouched, and pushes on 0x8863 and on 0x8864 upstream each took their own frames; the EtherType behind a tag is not known | other |
| a row that names an EtherType must sit in the second layout (row `256 - count` up, row 128 as programmed, from the register), and the lowest matching row still wins across both layouts | other; the placement is ours |
| ACL (96 ternary rules, by ingress port): a rule compares up to eight 16-bit slots of a template, and the C-tag field (code 0x09) holds the VID in bits 11:0 and the priority in bits 15:13, 0 for a frame with no C-tag, so VIDs 1..4094 never match an untagged one; a forwarding decision of 1 (redirect) with an empty port mask drops the frame, with the class word at 0x10; a rule acts on the frames of its own ingress ports only; with the lookup on for a port the permit for unmatched frames must be on first; the lowest matching entry decides, and a zero class word chains an entry to the one before it | other (count-only rules on the field matched 50 of 50 frames of VLAN 410, also with priority 0 named, none with priority 5, none untagged; drops on a C-tag kept the target frames and passed the controls; the same field behind an S-tag is not known) |
| the ACL decides for a frame before the classifier applies its tag operation | other, inferred: no measurement set a drop and a translation of one VID against each other. Nothing here depends on it: omcid leaves out of a discard every priority another entry of the VID names, so the two rows never match the same frame |
| `CLASSIFY_SETUP` bits 1:0 apply to downstream frames too (value 2 drops those no row matches). We write 1 | other |

A frame with two C-tags cannot be told from one with a single C-tag, so a rule
on a double-C filter is refused by omcid before it gets here.

#### Shapes

Every shape of a class 171 entry (G.988 9.3.13), and what the switch does with
it. "Now" is what this firmware does; the unproven ones use only registers
and fields known from the table above, but in a combination nobody we have
read has measured.

| shape | now | why |
|---|---|---|
| untagged in, add one C-tag, VID and priority assigned, TPID 0x8100 | done, as before (the manual-VLAN rule; a table of only this and plain defaults keeps the old path byte for byte) | |
| untagged in, add one S-tag (Output TPID 0x88a8), VID and priority assigned | done | S-tag action 1 |
| untagged in, add an S-tag outside a C-tag | done, one row for both planes: measured by another implementation (an entry writing C-VID 410 priority 5 and S-VID 2002 priority 3: 100 of 100 untagged frames left with S 2002/3 outside C 410/5) | the host test pins the action words |
| single C-tag in, VID named (priority named or not), replace the VID (or priority) with an assigned one, priority kept or assigned | done | C-tag action 1 |
| the same with the priority named in the filter and copied: one VID, several priorities, one rule each (the operator vector) | done | each rule is a row, the more specific lower |
| single C-tag in, VID named, remove it | done; the way back tags untagged frames with that VID | C-tag action 2 |
| single C-tag in, push an S-tag in front | done | S-tag action 1 |
| single C-tag in, replace it with an S-tag, or an S-tag and a C-tag | done, **unproven**: the planes are written together as above, but this mix of actions (C-tag removed, S-tag added) nobody has measured | |
| single S-tagged in (Input TPID 0x88a8), replace, pop, or replace with a C-tag | replace and pop done; S-to-C (action 2) **not done**, the register is known | |
| S-tag over a C-tag in, filter on either tag (not both), pop, replace or push | done | |
| both tags removed | done, measured by another implementation (S-VID 200 in front of C-VID 410, two pops on the S-VID, template 1: 100 of 100 frames arrived untagged); C-tag action 2 and S-tag action 3 in one row | |
| one tag replaced, or one or both pushed | done; both pushed on an untagged frame is the measured row above, a replace together with a push of the other tag in the same row is **unproven** | |
| pop and push of the same tag (a replace with the priority assigned, 0 included) | done, one row that rewrites the tag with the assigned priority (C-VID 410 priority 5 to 2002 priority 0: 100 of 100 left as 2002 priority 0); the row writes priority 0 as an assignment | measured by another implementation |
| filter on the S-VID and the C-VID of one frame | refused: the classifier compares one tag per row | needs a second compare nobody has measured |
| double C-tag (both 0x8100) | refused: a frame with two C-tags cannot be told from one with one | |
| filter or treatment TPID 0x9100, 0x9200 | refused | the second S-VLAN TPID register (0x0230c8) reads disabled; nothing says it can be used |
| filter on the DEI bit, or an added tag with DEI 1 | refused | no DEI field in the classifier is known |
| EtherType filter on untagged frames (IPoE 0x0800, PPPoE 0x8863 and 0x8864, ARP 0x0806, IPv6 0x86dd, EAPOL 0x888e) | done upstream: one row a value (PPPoE two) in the second layout, on template 0, no tag named; the way back is the inverse by VID. The untagged frames of every other EtherType (an entry with no EtherType, in a table that has some) are one row in the bottom region, after them | the measured EtherType slot, above |
| EtherType filter on untagged frames, downstream (only the EAPOL rule of `VLAN_EAPOL_UNTAGGED`) | done: the same row in the second layout with the direction bit set, forwarded to the UNI, no tag change; the bottom-region REST rule stays upstream only. Like every downstream row it matches the frames of every GEM port. **Not run on this image**: another implementation measured a downstream EtherType row on its own driver (the slot, above); this driver's row has not been run on a stick | the measured EtherType slot, above, with the direction bit |
| EtherType filter on tagged frames | refused | the EtherType behind a tag is not known |
| VID copied from a tag whose VID the filter does not name (any-VID priority remap) | refused | whether C-tag action 1 with no VID source keeps the VID is not known |
| priority copied between the two tags, or from the DSCP | refused unless the source is named in the filter, then done with its value | the source codes other than keep and assign are not known; DSCP needs a DSCP match |
| entry that discards the C-tagged frames of one VID (RemoveTags 3, not a default rule), the priority named or not | done, one ACL row on the UNI ports (a row for each priority when the table has an entry of that VID naming some: those priorities are left out, since the ACL decides first); nothing downstream | the ACL drop above |
| discard of untagged frames, of every VID, of S-tagged or double-tagged frames, one that names an EtherType, or in a table that acts on S-tags, or with a rule of the same filter or of a priority only beside it | refused | VID 0 reads like no tag; the C-tag field behind an S-tag is not known; nothing orders two rules of one filter |
| default rules: single-tag discard | done: tagged frames the table does not name are not passed | no row matches them, so they have no stream and are dropped |
| default rules: double-tag discard, or a default that adds or removes a tag | discard ignored (not enforced); the other refused | |
| downstream, DsMode 0 | done, the inverse: a tag the rule added is removed, one it removed is put back with the filter VID and priority, one it replaced is replaced back; a rule that merged frames (a filter that did not name the VID) has no downstream rule | |
| DsMode 1 | no downstream rule (forwarded unchanged) | |
| DsMode 2, 3, 4 | done: the inverse on VID and priority, VID, or priority only | |
| DsMode 5, 6, 7 | as 2, 3, 4, and logged that what they discard is forwarded | no drop action |
| DsMode 8 | no downstream rule, logged | no drop action |

The downstream half of the manual add-tag rule matches the manual VID on a
C-tag and no S-tag, **whatever the priority**, and removes the tag, where
that cannot lose to another row. The stock stack matches the VID and the
priority of the pushed tag, so an OLT that sends a frame at a priority other
than `VLAN_MANU_TAG_PRI` (control traffic at 7 and data at 0 on one VID is
seen) loses those frames: measured on a stick, with the tag priority 7 and
the OLT at 0 every downstream frame was discarded at the PON port. This is a
deliberate improvement over stock, and always on; the idea and the guards
follow odi-oss #370. `VLAN_MANU_TAG_PRI` then only sets the priority of the
upstream tag. omcid says it with the ignore sentinel (`OMCI_PRI_ANY`, 8) in
the `out_tag` priority of that rule, which the both-ways derive reads as
"priority not cared about" (the VID filters of class 84 do the same); the
descriptor is otherwise the stock one. The row names a VID and the tag flags,
so it is in class 1 of the ordered region: ahead of the VID-only rows of the
class 84 filters and of the "tagged" pass row of a FwdOp 0x00 override
(bottom region), behind a row that names VID and priority. Where such a row
could take the frames of the manual VID first and pass them tagged, the rule
is the stock one (VID and `VLAN_MANU_TAG_PRI`) and omcid logs why, once and
again when the reason changes: the manual VID is 0 (the row would sort behind
the multicast VID 0 priority 0 row), a class 84 filter of the line goes by
priority alone (FwdOp 0x07, 0x08, 0x11, 0x12, 0x1e, 0x1f, after the
`RTK_DEVINFO_SPECVER` override), or an IP host lane has rules from its own
class 171 table. The class 171 tag (`tag_from_olt`) is never changed: its
downstream half follows DsMode. The multicast rows (`manual-remove`,
`manual-pass`) are not changed either: they are downstream-only rules built
from a filter of VID 0 priority 0 that does not depend on
`VLAN_MANU_TAG_PRI`, as on the stock stack.
`test/odi_switch_bdgconn_vlan_test.c` pins the row and the order.

`VLAN_EAPOL_UNTAGGED` (docs/SETTINGS.md) puts three such rules beside the
manual add-tag rule: `eapol-up` (untagged, EtherType 0x888e, upstream, no tag
change, on the service's stream), `eapol-down` (the same downstream, to the
UNI) and `eapol-down-vid0` (a C-tag of VID 0, any priority, no EtherType,
downstream, to the UNI, C-tag action DEL, so every priority-tagged frame
reaches the router untagged). It names the VID and no priority, so it is in
class 1 of the ordered region, behind a row that names VID 0 and a priority:
the multicast VID 0 priority 0 row keeps its own action, and with the manual
VID 0 so does the add-tag rule's downstream row at `VLAN_MANU_TAG_PRI`
(`test/odi_switch_bdgconn_vlan_test.c` pins the action, and the order beside
the multicast row). The add-tag rule is the
one it always was: its upstream row names no VID, so it sits in the bottom
region, behind the EtherType row (`test/odi_switch_bdgconn_vlan_test.c` pins
the order for both install orders). The bottom region fills down and the
second layout up, so of two add-tag rules the later one takes the untagged
frames while of two EtherType rows the earlier one wins: omcid installs the
EAPOL rules once, after every unicast port, on the GEM port of the add-tag
rule that went in last. The two downstream rows name no GEM port and match
frames from all of them. The downstream VID 0 row cares both tag flags, a
C-tag and no S-tag; that a priority-tagged frame sets the C-tag flag is not
measured.

A table is applied whole: one entry in the refused rows leaves every frame as
it comes and the log names the entry and the reason once.

The discards and the EtherType rows have their own descriptors (rule_gen
`OMCI_VLAN_OPER_EXTVLAN_DROP`, and `ethertype` of a rule_gen 7 descriptor
holding `OMCI_ETHTYPE_LITERAL` with the value, or `OMCI_ETHTYPE_REST`, in
`uapi/omci_bdgconn.h`). The ACL rows are entries 16 to 47 on template 5, whose
slot 0 reads the C-tag; the snooping copy keeps entry 8 and template 6. The
rule is written with the lookup and the permit turned on for its ports first
(never off), the valid bit last, and released valid bit first. A discard
programmed from the OLT's table, and one split by priority, has not been run
through an OLT by anyone yet; only the drop on a C-tag has.

#### What is not known

- a classifier drop action (the discards go through the ACL, on the UNI ports
  and upstream only; a downstream discard of DsMode 5 to 8 is not carried out);
- the EtherType of a frame behind a tag, and what the ACL's C-tag field holds
  for the C-tag behind an S-tag;
- the VID and priority source codes other than "keep" and "assign" (so no
  priority copied between tags, and none from the DSCP);
- whether a C-tag action 1 with no VID source keeps the VID (so a priority
  remap whose filter does not name the VID is refused);
- what `SVLAN_POLICY` does to an S-tagged frame whose S-VID has no VLAN row
  (rows for the VIDs a rule matches are made, tagged on both ports).

### MIB counters

`odi_sw_mib_get()` (`odi_switch_mib.c`) answers a port counter by the
index of `src/diag/src/mib.h` `mib_names[]`, the counter order of the
stock diag, from the `PORT_TX_COUNTERS`, `PORT_RX_COUNTERS` and
`PORT_OAM_COUNTERS` blocks. The two 64-bit counters, ifInOctets and
ifOutOctets, are two consecutive words, the low half at the lower
address: a live ISP1 counter that had not yet passed 32 bits read back
nonzero high and zero low with the halves the other way round, and
`test/odi_reg_test.c` pins the order. The two words are read high, low,
high, and again (three tries at most) when the two highs differ: nothing
shows the hardware latching one half when the other is read, and a
low-then-high read across a wrap of the low word, about every 34 s at
line rate for the octet counters, came back 4 GiB too high. Should all
three tries see a wrap, the top bit of the low word tells which high it
goes with. Left unmapped, and refused: the
stock duplicates (a direction-specific name such as etherStatsTx... next
to a direction-less etherStats... that no register tells apart:
etherStatsOctets, the broadcast and multicast counts, the undersize and
oversize counts, the frame-size buckets, TxFragments, TxJabbers,
TxCRCAlignErrors, RxUndersizeDropPkts), and the counters no register row
names at all (dot1dPortDelayExceedDiscards, dot1dTpHcPortInDiscards,
dot3StatsAlignmentErrors, dot3StatsFrameTooLongs, dot3OutPauseOnFrames).

## Downstream AES

The OLT encrypts the data GEM ports: downstream frames on them are AES-CTR
ciphertext, and the ONU decrypts a port only with bit 4 of its
`DSF_GEM_FLOW_TYPE` FLAGS set. A full-stream capture of the stock image
shows FLAGS `0x12` on the five ISP1 data ports where ours had `0x02`, and
the bit set right after the OLT sends the G.984.3 Encrypted_Port-ID PLOAM
(which names a GEM port and an enable bit), from the GPON interrupt and
outside any OMCI command. The other FLAGS bits are multicast (0), Ethernet
(1) and OMCI (2).

The GPON core decodes that PLOAM and calls
`odi_switch_gpon_encrypt_port()`, which turns the GEM port id into its DS
slot (recorded by cmd 25 when it creates the port) and sets or clears the
one bit with a read-modify-write. It also keeps the flag per GEM port, and
cmd 25 applies it when it writes the slot: one OLT sends Encrypted_Port-ID
right after O5, before any OMCI has created the port, and a flow cmd 25
writes again (every QoS or bridge rebuild, an OMCI MIB reset) gets no new
PLOAM. cmd 25 writes FLAGS once with the bit already in it, as the stock
flow add does; written bare and set after, the row would decrypt nothing in
between. The kept flags are forgotten when the ONU-ID is discarded; the next
activation configures the ports again. The table covers DS slots 0..63,
every flow cmd 25 accepts. Not reproduced from the capture: the CAM
rewrite around the bit (it writes the same GEM id back), and the interrupt
mask save and restore the stock handler makes around it.

The key itself is new for every Request_Key: the driver draws 16 bytes
from the kernel RNG (`hw_new_key()` in `odi_gpon_hw.c`), as G.984.3 clause
12.3 asks ("cryptographically unpredictable"). A request that arrives before
the RNG is initialised still gets a key, with a one-time warning, because
no key would make the OLT ask again and then deactivate the ONU.
`test/odi_gpon_aes_key_test.sh` runs it against the register model.

The OMCC port may be encrypted too (G.984.3 9.2.3.8 names any Port-ID): an
Encrypted_Port-ID for the OMCI port sets the bit on its row (FLAGS `0x14`,
`odi_gpon_hw_gem_port_encrypted()`), as the stock GPON module does, and a
Configure_Port-ID writes the row back unencrypted (`0x04`). Without it, an
OLT that encrypts the OMCC would leave every downstream OMCI frame
unreadable. No OLT seen so far encrypts it, so this path is host-tested
only. Like the stock module, the message's `b` bit is not read.

What the OLT left on is readable: `odi_switch_gpon_aes_ports()` lists every
DS slot whose FLAGS word has the AES bit set in the hardware, plus any port
the OLT asked to decrypt that has no slot yet, and `/proc/odi_gpon/status`
prints it as `enc_ports <n> <port>...`. omcid answers the GEM port network
CTP's Encryption state (ME 268, attribute 8) from that line, 1 for a port on
it and 0 for any other. G.988 has no AVC for the attribute, so it shows in a
Get and in a MIB upload only. The OMCC row is not a GEM port network CTP and
is not on the line.

Setting the bit on every data port at creation was rejected: a port the
OLT does not encrypt would then be decrypted into garbage, and nothing here
could tell. Every OLT seen so far encrypts every data port.

## Host port PAUSE

Port 0 is the host port: the SerDes link to the host, in Fiber 1G
(1000BASE-X) with auto-negotiation on (`network.sh` sets the mode). The
stock image leaves the port's abilities to that negotiation, and the host
NICs seen so far advertise PAUSE both ways, so the port ends up able to
send PAUSE frames to the host. A live register dump of a stick on the stock
modules reads `PORT_ABILITY(0) = 0xfe`: 1000 Mb/s, full duplex, fibre, link
up, PAUSE received and sent, negotiated. This image forces the sending side
off and leaves the rest alone.

Why: when the upstream stops draining (no grants from the OLT, or garbage
in the Alloc-ID CAM, see [GPON boot init](#gpon-boot-init)), frames from
the host pile up in the switch, and the port sends XOFF after XOFF for as
long as the stall lasts. A host NIC that honours them stalls its transmit
queue; Linux declares a transmit timeout after 5 s and resets the adapter,
over and over (seen on an Intel 10G NIC behind both test sticks: about
100,000 and 5.9 million XOFF frames received, and adapter resets for as
long as the stick stayed stuck). An ONU drops at its own queue when its
upstream cannot drain, and TCP adapts; that is what the switch does when
the port does not send PAUSE.

The registers, one word per port, with the bit layout of the stock
register table:

| register | offset | role |
|---|---|---|
| `PORT_FORCE_ABILITY` | 0x000180 | the value a port runs with for each field forced |
| `PORT_ABILITY` | 0x000198 | read-only: the ability the port runs with |
| `PORT_FORCE_SELECT` | 0x0001b4 | one bit per field: 1 forced value, 0 negotiated |

In all three, bit 6 is `TX_PAUSE` (the port sends PAUSE), bit 5 `RX_PAUSE`
(it obeys PAUSE it receives), bit 4 the link, bits 1:0 the speed. The
select works field by field: the stock image forces the CPU port whole
(`PORT_FORCE_SELECT(3) = 0xfff`, `PORT_FORCE_ABILITY(3) = 0x16`: 1000 Mb/s,
full duplex, link, no PAUSE), and the host port not at all.

The platform settings write `PORT_FORCE_ABILITY(0).TX_PAUSE = 0`, then
`PORT_FORCE_SELECT(0).TX_PAUSE = 1`, value before select so the port never
runs on a stale forced value, each a read-modify-write of the one bit, and
log the ability the port then reads (read right after the write; the
line below is the one expected on a stick, not yet seen):

    odi_switch: host port PAUSE toward the host off: ability 0x0be (link 1, tx pause 0, rx pause 1)

cmd 32 writes `PORT_FORCE_SELECT(0)` at every provisioning; its leaf changes only the LINK bit
([Command leaves](#command-leaves)). `test/odi_host_pause_test.c` checks
both words after the platform settings, after the whole ISP1 provisioning
twice over, and through the parameter below.

What is left alone:

- **Receiving PAUSE.** `RX_PAUSE` stays negotiated, so a host that sends
  PAUSE is still obeyed. In practice the host sends none.
- **The advertisement.** A 1000BASE-X link negotiates PAUSE with two bits
  each way, and against a partner that advertises both, no advertisement
  of ours resolves to "receive only": clearing ours would turn PAUSE off in
  both directions and needs a renegotiation, a link flap. So the host still
  resolves PAUSE as on in both directions (`ethtool -a`); it is the port's
  own transmitter that never sends.
- **The buffer thresholds.** The switch has a second set of ingress
  thresholds for ports that do not send PAUSE, where it drops instead of
  pausing. On a live stick they hold the same values as the PAUSE ones
  (0x0230dc and 0x0230e4, the global pair, both on 0x200 / off 0x1f4
  pages; 0x0230f4 and 0x0230ec, the per-port pair, both on 0xf0 / off 0xea),
  so the host port now drops frames where it used to pause the host. No
  drop policy of our own is added.

`odi_switch.host_pause` is the escape hatch, a kernel parameter (default
`N`). Set to `Y` it clears the select bit and the port goes back to the
negotiated PAUSE, as the stock image runs; on the command line it takes
effect with the platform settings, and written to
`/sys/module/odi_switch/parameters/host_pause` on a running stick it is
applied at once (both directions, idempotent).

To see it on a stick: `register get 0x198 1` in `diag` reads
`PORT_ABILITY(0)` (bit 6 clear: the port does not send PAUSE); the
exporter's `gpon_port_pause_frames_total{port="0",direction="transmit"}`
(`dot3OutPauseFrames`) counts PAUSE frames the port sent, and stays flat;
the host's `ethtool -S` `rx_flow_control_xoff` stays flat. The meaning of
bit 6 as the sending side (bit 5 receiving) is the one thing here not yet
seen on hardware: that is what these three readings check.


## Host port auto-negotiation

The host port's SerDes runs in Fiber 1G (1000BASE-X), and its PCS runs the
clause-37 auto-negotiation with the host: `HOST_FIBER_CTRL` (0x021c00) reads
`0x1140` on every stick seen, stock or ours, and nothing in this image
writes its `AUTONEG` bit unless asked. Most hosts negotiate. A host whose SFP
port has auto-negotiation off (some routers offer it per port) sees no link
from a negotiating stick, so `HOST_AUTONEG=off` forces the PCS to 1000BASE-X
full duplex instead (`docs/SETTINGS.md`, "Host port auto-negotiation").
OMCI does not reach this word: the PPTP auto-detection an OLT sets (class 11)
is a UNI PHY ability for cmd 30, and the host SerDes has no such PHY.

`HOST_FIBER_CTRL` is laid out like a clause-22 control register:

| bit | field | 0x1140 |
|---|---|---|
| 15 | `RESET` | 0 |
| 14 | `LOOPBACK` | 0 |
| 13 | `SPEED_LSB` | 0 |
| 12 | `AUTONEG` | 1 |
| 11 | `POWER_DOWN` (the one bit `network.sh` writes, to 0) | 0 |
| 10 | `ISOLATE` | 0 |
| 9 | `AUTONEG_RESTART` (self-clearing) | 0 |
| 8 | `FULL_DUPLEX` | 1 |
| 6 | `SPEED_MSB` (with bit 13: 10 = 1000 Mb/s) | 1 |

The bit layout is the one of the stock register table for this block. The
same table has a second candidate, a two-bit "force auto-negotiation" field
(bits 9:8, one bit per lane, bit 8 the host lane) of another host SerDes
word, `SDS_AN_CTRL` 0x021808. `HOST_AUTONEG` uses bit 12 of the PCS
control word, whose meaning is the standard clause-22 one, and does not
touch 0x021808. Only the host SerDes mode does: bit 8 set in HiSGMII,
clear in Fiber 1G and 2500BASE-X ("Host port SerDes mode" below). On a stick it
gives a working forced link: with the bit cleared (0x1140 becomes 0x0140)
the port keeps reporting the link of a forced PCS, and against a host whose
SFP port has auto-negotiation off a forced stick carries traffic where a
negotiating one gets a link and none. For `HOST_AUTONEG` the other
candidate was never needed.

`odi_sw_host_autoneg_apply()` (`odi_switch_port.c`) is one read-modify-write
of bit 12: cleared for off (`0x1140` becomes `0x0140`: no negotiation, 1000
Mb/s full duplex, the speed and duplex bits as the mode set left them);
set, with `AUTONEG_RESTART`, for on (`0x0140` is written `0x1340` and reads
back `0x1140`), so the PCS negotiates at once. Nothing is written when the
bit already holds the wanted value, since a restart flaps a working link. It
logs the word before and after:

    odi_switch: host port auto-negotiation off (forced 1000BASE-X full duplex): control 0x1140 -> 0x0140, ability 0x0fe

The port side (`PORT_FORCE_SELECT(0)`, [Host port PAUSE](#host-port-pause))
is left as it is: the port keeps taking its link, speed and duplex from the
PCS. Whether `PORT_ABILITY(0)` reports the link of a forced PCS the way it
reports a negotiated one is the first thing a stick trial checks; if it does
not, the port side needs forcing too (link, 1000, full duplex in
`PORT_FORCE_ABILITY(0)` and their select bits, as the stock image forces the
CPU port).

Two kernel parameters, under `/sys/module/odi_switch/parameters/`:

- `host_autoneg` (bool, default `Y`, 0644): written, it applies at once, one
  register write, under `odi_switch_lock`. On the command line it is stored
  and the platform settings apply it (a step that reads and writes nothing
  while it is `Y`, so the boot of a stick that never sets it is unchanged:
  the boot goldens do not move). Not persistent: userland decides what each
  boot runs, from `HOST_AUTONEG` (`host-autoneg.sh`, `docs/TOOLS.md`).
- `host_link` (read-only): `1` while `PORT_ABILITY(0).LINK_UP` is set, `0`
  otherwise, read when asked. What `host-autoneg.sh` and the web UI read to
  decide whether a mode works. On a forced port (`host_forced`, below) it
  reads the forced value, `1`, whatever the host does.

`test/odi_host_autoneg_test.c` checks the writes in both directions, that a
second apply writes nothing, that the other bits survive, that the default
platform settings neither read nor write the word, and the link read.

To see it on a stick: `register get 0x21c00 1` in `diag` (bit 12), `cat
/sys/module/odi_switch/parameters/host_link`, and on the host `ethtool
<if>` (`Auto-negotiation: on|off`, `Link detected`).

## Host port SerDes mode

`HOST_SERDES` (`docs/SETTINGS.md`, "Host port SerDes mode") picks the mode
of the host SerDes: Fiber 1G (1000BASE-X, the default), 2500BASE-X or
HiSGMII (3.125 Gbaud). `host-serdes.sh` runs the mode's register sequence
through `diag`, then sets the port side and the PCS auto-negotiation bit
through two kernel parameters. The sequences and the forced port values are the reporter's,
read off a stick running its stock HSGQ firmware in each mode; the Fiber 1G
one is the sequence this image always ran.

The sequence: the same thirteen steps in every mode, in this order (the
mode parked, the analog block, the mode selected). The two 3.125 Gbaud
modes, 2500BASE-X and HiSGMII, add the 3.125G analog page before the mode
is selected and a receive pulse after it. Every mode ends with `SDS_AN_CTRL`
0x21808 bit 8, in-band negotiation forced off on the host lane: set in
HiSGMII, as the stock HSGQ kernel does in its LAN SerDes mode 4 (HiSGMII
PHY; 0x70e0 becomes 0x71e0), clear in the other two (mode 6, 2500BaseX,
leaves it at its reset value), so no change out of HiSGMII leaves it set.
RMW is a read-modify-write of the bits named; the rest are plain writes.

| step | register | Fiber 1G | 2500BASE-X | HiSGMII |
|---|---|---|---|---|
| 1 | `HOST_SERDES_MODE` 0x214 `HOLD_CLK_GLITCH` bit 13 := 1 (RMW) | same | same | same |
| 2 | 0x214 `PCIE_SELECT` bit 15 := 0, `LINK_MODE` [4:0] := 0x1f, parked (RMW) | same | same | same |
| 3 | `HOST_FIBER_CTRL` 0x21c00 `POWER_DOWN` bit 11 (RMW) | 0 | 0 | 1 |
| 4 | `HOST_FIBER_TX` 0x22e4c `TX_MODE` [15:14] (RMW) | 0 | 1 | 1 |
| 5-8 | 0x21680 := 0, 0x2168c := 0x3001, 0x21698 := 0x68, 0x215b8 := 0x98c5 | same | same | same |
| 9 | 0x215b4, the TX amplitude word | 0x1264 | 0x1764 | 0x1564 |
| 10-11 | 0x215bc := 0x400f, 0x215e4 := 0xfc00 | same | same | same |
| -- | the 3.125G analog page: 0x21700 := 0x1800, 0x2170c := 0x7b01, 0x21718 := 0x168 | -- | written | written |
| 12 | 0x214 bits 15, 13, 8 and [4:0] := the mode (RMW, mask 0xa11f) | 0x4 | 0x16 | 0x112 (`PHY_SIDE_SEL` bit 8 and 0x12) |
| 13 | `PHY_PATCH_STATUS` 0x110 `PATCH_DONE` bit 0 := 1 (RMW) | same | same | same |
| -- | 0x21500 := 0x10, then 0x30, then 0: a receive pulse | -- | written | written |
| -- | `SDS_AN_CTRL` 0x21808 bit 8 (RMW; 0x70e0 out of reset) | 0, written only when it reads 1 | 0, written only when it reads 1 | 1 |

What the boot compares, before writing anything and after: the words of
steps 2 to 13 under their masks (0x214 under 0xa11f, 0x21c00 under 0x800,
0x22e4c under 0xc000, 0x21808 under 0x100, the rest whole), plus the
3.125G page in 2500BASE-X and HiSGMII. A Fiber 1G stick that never ran
HiSGMII reads bit 8 clear, so its sequence is the stock thirteen writes.
0x21500 is not compared: it reads 0 after its pulse. A stick that already
holds the mode is not written; a live change (a trial, its revert, the boot
watch, `reapply`) writes the whole sequence whatever the registers hold.
Read back from the HSGQ stick in 2500BASE-X: 0x214 = 0x00260016, 0x21c00 =
0x0140, 0x215b4 = 0x1764, 0x21500 = 0, the 3.125G page as written (and
0x21600, 0x2160c, 0x21618 the same, written by nothing here), 0x2166c =
0x2176c = 1. In HiSGMII 0x214 = 0x00260112, 0x21c00 = 0x1940 and 0x21808
= 0x71e0. With the sequence before it (no 3.125G page, no receive pulse,
0x21808 bit 8 clear) the HiSGMII SerDes is in sync and the host link up,
and no frame is received from the host; which of the three the newer
hosts need is not isolated (docs/QUALIFICATION.md).

HiSGMII is not an IEEE 802.3 mode: SGMII signalling at 3.125 Gbaud, no
standard to define the line or its negotiation. The host port is set to
it; nothing on the line selects it.

After the sequence, in 2500BASE-X and HiSGMII, the port side and the PCS:

- `odi_switch.host_forced` = `Y`: `PORT_FORCE_ABILITY(0)` = 0x16 (link up,
  full duplex, 1000 Mb/s) and `PORT_FORCE_SELECT(0)` = 0xfff, the values of
  the stock HSGQ firmware (read: 0x180 = 0x198 = 0x16, 0x1b4 = 0xfff). The
  1000BASE-X PCS reports no link to the port in these modes, so without the
  force the port has none.
- `odi_switch.host_autoneg`: `N` in 2500BASE-X (0x21c00 0x1140 becomes
  0x0140; the only setting seen to link to the 2.5G router port it was
  measured on, while another open implementation's measurements linked
  2500BASE-X with it on and off against a matching host, see
  `docs/SETTINGS.md`), `Y` in HiSGMII (0x1940, the stock value; the PCS is
  powered down).

Back in Fiber 1G during a boot, `host_forced` goes back to `N` (only when
it was `Y`) and `host_autoneg` to what `host-autoneg.sh` last ran. A boot
in Fiber 1G writes neither: the kernel defaults stand.

**The link a mode can see.** `host_link` is `PORT_ABILITY(0).LINK_UP`, which
a forced port reports as forced, `1`. So in the two forced modes it is
blind. In 2500BASE-X bit 17 of 0x214 is set while the SerDes is in sync
with the host: 0x00260016 on a stick that linked, 0x00240016 on one that
did not (auto-negotiation on, against the same port). `host-serdes.sh`
reads it through `diag` for its boot watch and its `status`; it is not a
kernel parameter, because it is an observation of one mode on one stick.
The same bit is set on a DFP-34X-2C2 running Fiber 1G (0x00260004). HiSGMII
has no signal known: no boot watch there, only the trial revert.

`odi_switch.host_forced` (bool, default `N`, 0644), `odi_sw_host_forced_apply()`
in `odi_switch_port.c`: written `Y`, it writes the forced value, then the
select, then the PAUSE policy again (which owns the `TX_PAUSE` bit of both
words: with `host_pause=N` the select stays 0xfff, with `Y` it is 0xfbf);
written `Y` again, it writes the same values again. Forcing saves both
words first, and `N` after `Y` puts them back, then the PAUSE policy, so
the port follows its SerDes again with the registers a boot leaves (0x180
= 0x0, 0x1b4 = 0x40 on a DFP-34X-2C2 after a trial of another mode, as
before it). Written `N` while already `N`, it reads and writes nothing, so a
stick that never forces the port keeps its registers exactly as before.
On the command line it is stored, and the platform settings apply it (a
step that reads and writes nothing while it is `N`: the boot goldens do
not move). An OLT admin lock (cmd 32, `OMCI_UNI_LOCK`) survives it both
ways: a locked port forced keeps its forced `LINK` value at 0, and a
locked port unforced keeps its `LINK` select bit and value 0; the
`TX_PAUSE` bits are the PAUSE policy's, not taken from the saved words.

cmd 32 on a forced port. The stock admin enable clears the `LINK` select
bit on an unlock, handing the link back to the PHY or SerDes; on a forced
port that would leave no link at all, and omcid unlocks the UNI at every
provisioning. So while `host_forced` is set, cmd 32 on port 0 moves the
forced `LINK` value instead (0 to lock, 1 to unlock, the value first) and
keeps the select bit set. Other ports, and port 0 unforced, are as before.

`test/odi_host_forced_test.c` checks the forced values both ways and with
either PAUSE policy, that off puts both words back as the force found them
and that `N` while `N` touches nothing, the order of the writes, cmd 32 on a forced port, the
ISP1 provisioning (cmd 32 among it) leaving the force in place, an admin
lock across the force, and that the default platform settings write port
0 for the PAUSE policy only. `test/host_serdes_test.sh` checks the write
sequence of each mode, value for value and in order.

To see it on a stick: `register get 0x214 1` in `diag` (bits 4:0 the mode,
bit 17 the sync), `register get 0x180 1` and `register get 0x1b4 1`, `cat
/sys/module/odi_switch/parameters/host_forced`, and `host-serdes.sh
status`.

## Multicast

How a group the OLT sends on the multicast GEM port reaches the host, and
what IGMP snooping adds. The data path needs no programming: a frame on the
multicast GEM port is flagged by the downstream flow's type (below), the
GPON MAC passes it, and the switch floods it to the host unless a row or a
drop says otherwise. Snooping is opt-in (`IGMP_SNOOPING`, `docs/TOOLS.md`,
`igmpd`), and its pruning of the PON side is a second opt-in (`IGMP_PON_DROP`).

What was read on a stick (ISP1, read-only) and what is taken from another
open implementation's measurements on the same chip. Nothing below was
exercised with a multicast stream: neither of our lines carries IPTV.

- **The downstream flow's multicast flag.** omcid sets bit 0 of the flow's
  `DSF_GEM_FLOW_TYPE` word when a multicast GEM interworking TP (ME 281)
  that points at its CTP holds an address row, whatever the port number. ISP1
  reads `0x3` (multicast and Ethernet) on its flow for GEM port 4095. The
  GPON MAC applies its multicast filter only to flows with that bit.
- **The GPON multicast filter passes everything while its table is empty.**
  `GPON_GEM_DS_MC_CFG` (0x704080) reads `0x59` on ISP1 (the boot table writes the same word on both lines): bit 0 set, the
  table lists the multicast to drop, and it is empty; bits 3, 4 and 6 are
  FCS check, unicast pass and broadcast pass; no IPv4 or IPv6 override.
  Another implementation measured exactly this configuration passing 224.0.0.1,
  239.2.2.1 and ff02::1 on a multicast-flagged flow, and the opposite
  configuration (an including filter with an empty table) dropping all of
  them. So the stock stack's rows (ME 281 range to the filter table) are
  not needed for the stream to arrive, and nothing writes the 256-row table
  (it is emptied at boot).
- **Flooding.** A group no row names is a lookup miss and goes by
  `FLOOD_UNKN_MCAST_PORTS`: `0x7` on ISP1 (the module-load replay
  writes `0xf` and `odi_switch_init_modload()` takes the CPU bit out on
  purpose; see above). So the stream reaches the UNI and port 1, never the
  CPU, which is what the CPU-RX guards want from a stream of thousands of
  frames a second. The PON port's isolation mask (`0x7`, never the CPU)
  decides where it may go from there.
- **IGMP is data to the switch.** `IGMP_PORT_ACTION` (0x01105c, a word a
  port) reads `0x400` on ports 0 to 2: every IGMP and MLD type is forwarded.
  Reports and queries cross the stick as plain multicast, so with no
  snooping the OLT sees every report and the host every query.

### Downstream tag control

The multicast profile (ME 309) says what happens to the tag of downstream
multicast in attribute 16: a control type (0 pass, 1 strip the outer tag, 2
add a tag, 3 replace it, 4 replace its VID only, 5 to 7 the same with the
service package VID) and the TCI to write. omcid builds it into the bridge
rules of the multicast bridge port (`src/omci/mcastvlan.c`,
`respond/apply_bridge.c`).

A downstream classifier row compares the tags of a frame and nothing that
says it is multicast: not its GEM port, not its destination address ([CF
rows](#cf-rows)). So the treatment is put on the VLANs the profile names as
carrying its groups downstream, the VLAN (ANI) of the part 0 rows of its two
access control lists: one rule per VLAN, rule_gen
`OMCI_VLAN_OPER_EXTVLAN_GEN`, downstream only, matching a C-tag of that VID
with no S-tag in front ([Extended VLAN rules](#extended-vlan-rules) for the
row it becomes). The stock OMCI stack builds its multicast rules per ACL
VLAN the same way, and another open implementation's measurements on this
chip apply the profile's treatment on the ACL VLANs too.

| type | rule |
|---|---|
| 0 | none: the rules of the line apply to multicast as to the rest, as G.988 says for a zero first byte |
| 1 | C-tag removed |
| 3 | C-tag rewritten with the TCI's VID and priority |
| 4 | C-tag VID rewritten, its own priority (and DEI) kept |
| 2 | the C-tag kept and an S-tag (TPID 0x88a8) with the TCI pushed in front: the classifier replaces a C-tag, it does not stack a second one |
| 5, 6, 7 | none, logged: the service package table does not select by VID here (`docs/TOOLS.md`) |

Left out, each logged once: VLAN (ANI) 0 (untagged groups) and 4097 (any
tagged frame), since a rule on them would take unicast frames too, and
0xffff (unspecified); a VID that a unicast rule of the line names (the
manual VID, a class 84 filter, a class 171 entry), which is left to that
rule; a TCI with DEI 1 for types 2 and 3 (no DEI field in the classifier),
and VID 4095. `OMCI_CUSTOM_MCAST` bit 1 reads the type as 1 whatever the
OLT set, as its stock plugin (`cf_force_ds_mcast_untagged`) does by writing
1 over the first byte; omcid keeps the attribute as the OLT set it.

The rules go in front of the port's own rules, which stay: the manual tag's
removal (`manual-remove`, the stock rule: C-tag VID 0 priority 0, both tags
removed) or forward-all. That VID 0 row, for the priority-tagged and untagged
groups, follows the profile too: type 0 keeps the tag as received
(`manual-pass`, the same match with the tag left alone), type 1 and
`OMCI_CUSTOM_MCAST` bit 1 strip it. Types 2 to 4 write a tag on a VLAN the
lists name and a VID 0 row is never one of those, so there the row keeps its
strip, as do types 5 to 7; with no multicast profile at all it strips as it
always did. With no ACL VLAN named, as on both our lines (ISP1 provisions
type 1, ISP2 type 0, neither an ACL row), no `mcast-*` rule is added;
`src/omci/drv-test.sh` holds the driver calls of both to their goldens with
the attribute set as each OLT sets it.

Not run on a stick: no line here carries a multicast stream, so none of these
rules has met a frame. What is not known: whether a frame whose VID a rule
replaced (types 3, 4) needs a VLAN row of the new VID to leave the UNI port
(the rule writes the row of the VID it matches only), and whether the host
side takes an S-tagged stream (type 2).

### Snooping

With the lease held (`odi_switch_mcast.c`, `ODI_SW_IOC_MC_SNOOP`):

- **The copy.** ACL entry 8, template 6 (EtherType in slot 0, IPv4 protocol
  in slot 1; the platform step empties the tables and the replayed rule is
  entry 1 on template 3, so both are free), ingress port 0 only, EtherType
  0x0800 and protocol 2, action "mirror to the CPU port", action class
  enable `0x10` (the forwarding decision alone). A rule acts on the frames of
  its own port and on no other (another implementation measured a drop on
  each of the two ports leaving the other's frames alone), so a frame from
  the PON never matches it. The copy arrives on the CPU port with source port
  0; `odi_omci.c` (an RX hook registered for the UNI port when the lease is
  first taken) recognises it by that and by IPv4 protocol 2 and hands it
  to igmpd, and drops it if nobody is registered. It does not reach the IP
  stack. The original frame is forwarded as if there were no rule.
- **Rows.** One static L2 multicast row a group (SVL, filtering id 0, the
  keys `docs/KERNEL.md` describes), the UNI and the PON as members. The PON
  has to be one: a report to a group is forwarded to the row's members, and
  another implementation measured that without the PON in the row the report
  "goes nowhere but back" and the OLT never sees it. The CPU is never one.
- **The drop.** `MC_UNKNOWN_IP4` (0x01c014), two bits a port, set to drop
  (1) for port 2: IPv4 multicast from the PON that no row names is dropped
  instead of flooded. `MC_UNKNOWN_CFG` (0x01c01c) bits 4:3 are set to 1 with
  it, which exempts 224.0.0.0/24: ISP1 reads `0x20` there (the reset value
  is `0x28`), without which the OLT's general queries to 224.0.0.1 would be
  dropped as unregistered, the host would never report, and the stream would
  lapse. Both are put back as found when the drop is released. IPv6 is the
  MLD drop's (below). igmpd turns the drop on only after one membership
  interval, so every group the host already watches has reported again.
- **MLD** (`ODI_SW_MC_SNOOP_MLD`, igmpd `-6`, only with the copy). Three
  more rules, ACL entries 9 to 11 on template 2, which no rule here names
  (the discards are on 5, the probes on 4, the replayed rule on 3; entries 9
  to 15 are kept for snooping): slot 0 the EtherType, slot 1 field code
  0x15, the next header behind any IPv6 extension headers (its low octet;
  the traffic class above it is not compared), slot 2 field code 0x40, the
  window of user field selector 0 (`FIELDSEL_CFG` 0x023144, written 6:
  origin the start of the IP payload, offset 0), whose high octet is an
  ICMPv6 message's type. Each rule: ingress port 0 only, EtherType 0x86dd,
  next header 58, and the type under a mask: 130 and 131 (query, v1 report;
  `0x8200` under `0xfe00`), 132 (v1 done), 143 (v2 report). Neighbour and
  router discovery, echo and the error messages match none of them, so they
  are never copied and never taken. The action is the IGMP rule's (mirror to
  the CPU, or trap with the take), and a change of mode rewrites the four
  action entries. The field codes, the selector's origin (behind the
  hop-by-hop header an MLD message carries) and the trap of MLDv1 and MLDv2
  reports from the host side are another open implementation's measurements
  on this chip; nothing here or in the replays points selector 0 anywhere,
  and the lease saves its word and template 2 and puts both back. The RX
  hook takes a frame from port 0 that is an MLD message (the four types,
  behind at most two tags and four extension headers) while MLD is held,
  and hands it to igmpd like the IGMP copies. The tag rule is one contract
  for the hook and igmpd: up to two VLAN tags, each 802.1Q (0x8100) or
  802.1ad (0x88a8), so a C-tag, an S-tag or an S-tag over a C-tag all reach
  a report; a third tag is refused by the hook and not parsed by igmpd. The
  VID igmpd keys a report by is that of the outermost tag, 0 when untagged
  (`eth_l2_parse()` in `src/igmp/igmpd.h`, shared by the IGMP and MLD
  parsers).
- **The IPv6 drop** (`ODI_SW_MC_SNOOP_PON_DROP6`, igmpd `-6 -d`, only with
  MLD). `MC_UNKNOWN_IP6` (0x01c018), set to drop for port 2. Before it,
  `MC_UNKNOWN_CFG` bits 6:5 are set to 1 (the reserved IPv6 groups flooded
  whatever the drop says), bit 8 (the link-scope groups ff02::/16 are
  reserved) and bit 12 (the solicited-node groups ff02::1:ff00:0/104 are);
  out of reset no IPv6 class is selected, so the drop alone would prune
  them. ISP1 reads `0x20` there: bits 6:5 already 1, no class. Router
  advertisements (ff02::1), DHCPv6 (ff02::1:2), MLDv2 (ff02::16) and
  neighbour discovery ride those groups; another open implementation saw
  router advertisements and DHCPv6 stop when they followed a pruned flood
  mask. The register meanings are that implementation's measurements on the
  PON port; the drop goes off before the reserved classes are put back as
  found, so no frame of theirs ever meets the drop alone
  (`test/odi_switch_mcast_test.c` checks the order).

- **The take.** When the OLT's profile asks for what the switch cannot do on
  its own (an upstream IGMP tag, a rate, the discard of joins outside its
  lists: `docs/TOOLS.md`, `igmpd`), the lease holds `ODI_SW_MC_SNOOP_TRAP`
  as well, and the same rule's action is "trap" (3 at bits 42:41, no port)
  instead of "mirror": the report reaches the CPU and nothing leaves by the
  PON, as measured with `diag acl-probe` (below). The match is the copy's,
  so the take, like the copy, never sees a frame from the PON. A change of
  mode rewrites the action entry alone. The RX hook marks a frame received
  while the take is on (bit 7 of the second prefix byte), and igmpd sends
  only marked frames up; the mark goes on before the take and off after
  it, so a report on the edge of a change may go up twice, never not at all.
- **The way up.** igmpd sends a taken report up through two `/dev/odi_sw`
  ioctls. `ODI_SW_IOC_MC_ROUTE` reads back the classifier rows the kernel
  programmed (`odi_switch_bdgconn.c`): for the report's own tags, the row
  the classifier would pick (the extended VLAN discards first, then the
  lowest matching upstream row, a row matching when its cared bits equal
  its rule bits, [CF rows](#cf-rows)) with its tag treatment and flow; for a
  report igmpd re-tagged, the lowest upstream row whose frames leave with
  that one C-tag (VID, and priority unless the row passes the frame's own).
  `ODI_SW_IOC_MC_SEND` sends it: only for the holder of the lease, only
  while it holds the take, only an IPv4 IGMP frame (behind at most two tags)
  to an IPv4 multicast MAC, or, while MLD is held, an MLD report or done (no
  query) to a 33:33 MAC, of 60 bytes or more, and only on a flow an
  upstream row queues on (`odi_switch_mcast_send()`). The descriptor is the
  OMCI reply's (`odi_igmp_tx.h`): `PORT_SEL` with the PON port alone and
  the flow as the stream, a directed transmit and not a lookup, and
  `NO_LEARN`, which matters here: the frame carries the host's source MAC,
  and a learn from the CPU port would move the host's address there.
  `/proc/odi_omci` counts `igmp_sent_to_pon` and `igmp_send_fail`.

The lease is held by the open file that takes it: closing it, or the daemon
dying, releases the copy (or the take), MLD and the drops and deletes the rows
that file wrote (up to 256). After the take goes, the switch forwards the host's
IGMP and MLD by itself again. A second holder is refused with EBUSY. `odi_switch_init_modload()`
and the isolation masks are not touched, and `test/odi_switch_mcast_test.c`
checks each rule by what it matches (the UNI port and IGMP, or the four MLD
types, nothing else, the PON least of all).

### ACL probes

`diag acl-probe` (`odi_switch_acl_probe.c`, `ODI_SW_IOC_ACL_PROBE`) installs
rules by hand to measure what this section otherwise takes from another
implementation: eight slots, ACL entries 88 to 95 on template 4 (slot 0 the
EtherType, 1 the IPv4 protocol, 2 and 3 the low and high halves of the IPv4
destination; the snooping copy keeps entry 8 and template 6, the extended
VLAN discards 16 to 47 and template 5). Slot n counts in log counter
16 + 2n (`LOG_CNT`, 0x032600 + 4i), and its pair's bit of `LOG_TYPE`
(0x034008) says frames or bytes. Those two layouts, the action fields (log
class type at bits 36:35 and counter at 34:30 of the action entry, class
enable bit 3; trap 3 and mirror 2 at bits 42:41) and the receive reason of a
frame an ACL rule sends the CPU (128 plus the entry) are another
implementation's measurements; the probes are how they get checked here.
`LOG_TYPE` is never read (no read of it has been made on a stick): it has
two owners, the probes (pairs 8 to 15) and the multicast group counters
below (pairs 0 to 7), so it is kept as a shadow (`odi_switch_log.c`) in which
each changes only the bits of its own pairs, and the shadow is written
whole; the last probe out puts its pairs back to frames and leaves the
others. The counter itself is read, and a probe reports what it counted as
the difference from the moment it went in.

What a probe may do is narrow. A counting rule applies the log class alone
(class word 0x08), which leaves the forwarding of the frames to the switch.
A copy (`mirror`, to the CPU port) or a take (`trap`, no port) matches IGMP
entering the UNI and nothing else; a rule on the PON port can only count. A
frame a probe sends the CPU is counted by `odi_omci.c` (an RX hook below the
IGMP one, registered when the first probe goes in, which counts by receive
reason and takes nothing), and then goes where it would have gone: from the
UNI to the net device, and the NIC still drops anything from the PON no hook
took, so the PON isolation below is unchanged. The probes belong to the open
file of `/dev/odi_sw` that installed the first one: its close removes them
all, and another file is refused (EBUSY) until then. Probes and the snooping
lease exclude each other, since the copy rule and a probe of the same frames
would decide for each other, and so do probes and the group counters below
(of two rules that match a frame only the lower entry counts it, and the
counters sit below the probes). `test/odi_switch_acl_probe_test.c` checks each
rule by what it matches and decodes its action by bit.

`tools/mcast-probe/mcast-probe.sh` runs the measurements below on a stick as
one nohup script: counting windows on both ports, the same in bytes, two
identical rules, a copy and a take of the UNI IGMP, and a static row for a
test group naming port 1 alone. The UNI half needs IGMP and multicast from
the host; the script header has the loop to run on the host beside it. It
writes no flash and nothing of the boot slots, and every way out (end, a
signal, the holder of the probes ending) leaves the switch as found;
`test/mcast_probe_test.sh` runs it against stubs and kills it both ways.

#### What is not known

The copy rule follows another implementation's decoded layout and the one
rule the stock replay writes; it has not been run here, and neither has a
frame been matched against a static multicast row (the row layout was
checked by reading it back, `docs/KERNEL.md`). Whether the lookup is keyed
on the MAC or on the group address follows `L2_LOOKUP_SETUP` bit 23, which
reads 0 here (MAC); another implementation measured the MAC-keyed row
steering the frames. The probe run answers these, window by window:

- **Does the OLT's IGMP reach the ONU, and how often?** `count`, PON in,
  IGMP: the general queries a receive-only path to the CPU would see.
  (The VLAN they arrive in is not counted yet.)
- **How much other IPv4 multicast enters by the PON?** `count`, PON in,
  other IPv4 multicast; against `MIB port 2 in multicast`.
- **Does a counting rule leave forwarding alone?** `count`: the UNI IGMP and
  multicast are counted while `MIB port 2 out multicast` keeps moving, the
  PON's while `MIB port 0 out multicast` does.
- **Is a log counter free-running, not cleared by a read?** Every count is
  read again a second after the window: a cleared counter would read low.
- **Does `LOG_TYPE` switch a pair to bytes, and what does a byte count
  include?** `bytes`, against the frame rates of `count`; and `trap`, whose
  rule counts bytes while the CPU sums the length of each frame it gets
  (the difference a frame says whether the switch counts the FCS, and a
  tag the NIC takes off).
- **When two rules match the same frame, does only the lower entry count?**
  `overlap`: entry 88 should count, entry 89 not.
- **Does a copy of the UNI IGMP reach the CPU once a frame, with reason 128
  plus the entry and source port 0, and still reach the PON?** `mirror`:
  `cpu` equal to the count, `cpu_ports` 0x1, nothing by another reason, and
  `MIB port 2 out multicast` at the rate of `count`. This is the mechanism
  of the snooping copy, without igmpd.
- **Does a take reach the CPU and nothing else?** `trap`: `cpu` frames at
  the IGMP rate, and `MIB port 2 out multicast` lower than in `count` by
  that rate.
- **Does a static row keyed on the group MAC (SVL, filtering id 0) steer an
  untagged frame from the UNI?** `row`: with the row naming port 1 alone,
  the frames to the group stop reaching the PON (`MIB port 2 out
  multicast` lower than in `count` by their rate). A tagged frame is the
  same run with the host loop on a VLAN interface.
- **Does anything from the PON reach the CPU meanwhile?** `isolation`:
  `rx_pon_dropped` stays 0.

Not answered by a run on a line without IPTV: the OLT's stream on its
multicast GEM port, the PON drop with a real stream, the tags on the fibre,
and igmpd's own delivery path (the run does not start it). Those are what a
first run with a real stream has to check, with `diag l2-table get all`, the
port counters (`diag mib dump counter port all`, ports 0 and 2) and
`/proc/odi_omci` (`igmp_delivered`, `igmp_dropped`).

#### Measured on a stick

ISP1, one run of `tools/mcast-probe/mcast-probe.sh` (848 s, windows as in
the script), on an image with this tree's probes. The line carries no IPTV,
and the OLT sent no IGMP during the run. The host sent its IGMP (80 reports
in the 300 s count window); the multicast it was meant to send to the test
group never left it, so no frame to the group entered the UNI. The run
ended with every probe and the test row removed.

What is now known:

- **A counting rule counts and leaves forwarding alone.** The 80 reports
  from the host were counted on the UNI, and the PON sent out 80
  multicast frames in the same window (`MIB port 2 out multicast`).
- **A log counter is free-running.** Every count read the same a second
  later, with no traffic between.
- **`LOG_TYPE` switches a pair to bytes, and a byte count includes the
  FCS.** The same reports counted 2048 bytes for 32 frames, 64 bytes a
  frame, while the CPU received them as 1920 bytes, 60 a frame: 4 bytes a
  frame more on the switch, the FCS (untagged frames).
- **A copy (`mirror`) of the UNI IGMP reaches the CPU once a frame and still
  goes on.** 32 counted, 32 at the CPU, all with the receive reason 128 plus
  the entry and source port 0, none by another reason, and 32 multicast
  frames out of the PON. This is the mechanism the snooping copy uses.
- **A trap takes the UNI IGMP to the CPU and nowhere else.** 32 frames at
  the CPU by the rule's reason, and no multicast frame out of the PON in that
  window. Upstream tag control by trap and re-send can rest on this.
- **Nothing from the PON reached the CPU.** Over the run `rx_pon` (55)
  equals the OMCI frames delivered (55), and `rx_pon_dropped` stayed 0.

What is still not known, and why:

- **Whether the OLT's IGMP reaches the ONU, and in which VLAN.** None
  arrived: this OLT does not query a line without IPTV.
- **Whether a counting rule counts on the PON ingress.** The two PON rules
  counted 0 while the PON received 237 multicast frames
  (`MIB port 2 in multicast`, all flooded to the UNI). With no IPv4
  multicast counted on the UNI either, those were most likely IPv6 or
  non-IP multicast, which the rules do not match; a PON rule that counts
  nothing at all would read the same. A line with IPTV, or any IPv4
  multicast from the OLT, settles it.
- **Whether only the lower of two matching entries counts.** `overlap` ran
  on the PON ingress and saw no frame.
- **Whether a static row keyed on the group MAC steers a frame.** The row
  went in (`01:00:5E:7F:4D:01`, SVL, port 1 alone, read back) but no frame
  to the group came from the host. A second run with a sender that works,
  untagged and then tagged, answers it.
- Everything a real stream decides (the stream on its multicast GEM port,
  the PON drop, tags on the fibre), and igmpd's own delivery path, as above.

### Multicast group counters

`odi_switch_mc_count.c` (`ODI_SW_IOC_MC_COUNT`; `diag mc-count` by hand)
counts what enters a port to one destination: igmpd meters each group it
holds with one, on the PON ingress, in bytes, for the lost groups of ME 309,
its alarm 0 and the bandwidth of ME 311 (`docs/TOOLS.md`, `igmpd`).

| | |
|---|---|
| entries | 48 to 63, one rule a slot (64 to 79 kept for more); below the probes (88 to 95), above the extended VLAN discards (16 to 47) |
| template | 1: slots 0 to 2 the destination MAC (codes 0x01, 0x02, 0x03, bits 15:0 first), 3 the EtherType, 4 and 5 the IPv4 destination (0x12, 0x13); 6 and 7 nothing. The module-load replay writes codes into template 1, but no rule uses it (the tables are emptied at boot and the replayed rule is entry 1 on template 3); its words are saved with the first rule and put back with the last |
| counters | slot n counts in log counter n, so the slots own pairs 0 to 7 of `LOG_TYPE`; slots 2k and 2k + 1 share a unit |
| action | the log class alone (class word 0x08): a frame is forwarded exactly as without the rule |
| match | a MAC under a mask (igmpd: the group MAC, whole), or EtherType 0x0800 and an IPv4 prefix |
| the count | 32 bits in the switch, folded into 64 by the kernel on every read (a byte counter wraps in 13.8 s at the full downstream rate; igmpd reads each second) |
| hold | the open file of `/dev/odi_sw` that installed the first: its close removes them all; the probes and these exclude each other |

The ACL map, then: entry 1 and template 3 the module-load replay, entry 8
and template 6 the snooping copy, entries 16 to 47 and template 5 the
extended VLAN discards, entries 48 to 63 and template 1 these counters,
entries 88 to 95 and template 4 the probes; log counters 0 to 15 these, 16
to 31 the probes. `test/odi_switch_mc_count_test.c` checks each rule by what
it matches, its action by bit, the folded total and the shared `LOG_TYPE`.

What is not known, and how to find out. Two facts these rest on have not
been measured on a stick: that a rule on the PON ingress counts at all (the
probe run saw the two PON rules count nothing while the port received 237
multicast frames, most likely not IPv4), and the order of the three MAC
codes (taken from another open implementation's reading, by analogy with the
IPv4 halves). Until both are, igmpd treats a counter that never grew as
unknown, never as a lost group, so a stick on which neither holds tells the
OLT nothing wrong. `tools/mcast-probe/count-proof.sh` settles both, on a line
without IPTV, in one nohup script with its own restore (about 18 minutes; it
refuses to run beside igmpd):

- **pon**: every group-addressed frame entering the PON in three rules that
  cannot overlap (33:33/16, first octet 01, broadcast), against `MIB port 2
  in multicast` plus `in broadcast`. PASS is the counted sum within 5 % (or
  3 frames) of the MIB, over at least 10 frames.
- **ra**: 33:33:00:00:00:01 alone (router advertisements and MLD queries)
  and 224.0.0.x, the multicast every line carries by itself.
- **bytes**: the pon rules counting bytes; bytes a frame between 64 and
  1522 says the unit igmpd uses works on the PON.
- **ip**: an IPv4 rule for 224.0.0.0/4, the kind the probes ran.
- **uni** and **uni-ip**: the test group the host sends to (the loop is in
  the script header), by its exact MAC and then by its IPv4 address. Equal
  rates are the MAC field order proved; the IPv4 rule counting and the MAC
  rule not is the order wrong.

## PON port isolation

Nothing that arrives from the fibre reaches the stick's own IP stack: not
the web UI, ssh, the exporter, nor an ARP or ping answer. Two independent
layers hold this, each enough on its own, and neither depends on what an
OLT provisions, on VLANs or on tags: both key on the port a frame entered
by. OMCI is the one exception, and it is not Ethernet forwarding: the OLT's
management channel arrives as a trapped frame (reason 246, the OMCC stream)
that `odi_omci.c` hands to omcid, never to a network interface.

### Why two layers

What stood between the fibre and the CPU before: the switch forwards a
frame for the stick's MAC (learned on the CPU port) to the CPU, and floods
broadcast to the CPU (`FLOOD_BCAST_PORTS` 0xf, see
[Module-load replay](#module-load-replay)); the port isolation masks were
all open, as on the stock image; and the kernel handed any frame from the
PON port to `eth0`, up but outside `br0`, where the IP stack still answered
for `br0`'s address (a Linux host accepts a frame for any of its addresses
on any interface, and answers ARP for it). What remained was the
classifier: every downstream service row of
[Bridge connections](#bridge-connections) filters the egress by a port
mask that never holds the CPU, and row 255 forwards whatever no service row
matches to the port mask its action carries, which is empty. That this
removes the CPU from a lookup result is read from the field layout, not
measured, and it is one layer only.

### The switch: the PON port's isolation mask

`PORT_ISOLATION` holds, per ingress port, the ports a frame from it may be
forwarded to (bits 3:0, bit n = switch port n) and the CPU-side extension
ports (bits 10:4). Two 11-bit items share a word: port n is in the word at
0x027000 + 4 * (n / 2), at bit 11 * (n % 2). The register table of the
stock binary gives the width and the port range (11 bits, ports 0-3), and
puts the extension-port table at 0x027008; a word per port would put ports
2 and 3 on it. A stick reads `0x003fffff` in both words (two fields of
0x7ff, every port open) and `0x000007ff` in the last word of the extension
table (one field alone), on the stock modules and on this image before the
step below.

The platform step "pon isolate" (`odi_sw_pon_isolate_apply()`,
`odi_switch_port.c`) clears the CPU bit and bits 10:4 of port 2's field,
and the PON bit (2) of port 3's field ([Frames the stick
sends](#frames-the-stick-sends)), in one read-modify-write of the word the
two ports share: `0x003fffff` becomes `0x003fd807` (port 2 0x007, port 3
0x7fb), and logs

    odi_switch: PON port isolation on: the PON port forwards to 0x007 (never the CPU), the CPU port to 0x7fb (never the PON by lookup)

It runs before omcid starts, so before any service exists. The host still
reaches the CPU and the fibre, the CPU still reaches the host, and a frame
from the fibre still reaches the UNI. Isolation filters the
egress whatever chose it, a flood or an L2 hit; that it filters a trap to
the CPU as well is suggested, not measured, by the chip's per-protocol
isolation "leaky" controls (reserved multicast, IP multicast), which
exist to let trapped control frames through it.

Open: OMCI is itself a frame from the PON port trapped to the CPU. Whether
isolation applies to that trap is not known; if it did, OMCI would stop
with the step on. The trial checks it first (below), and
`odi_switch.pon_isolate` is the way back:

- `pon_isolate` (bool, default `Y`, 0644): `N` on the command line leaves
  both masks open; written to `/sys/module/odi_switch/parameters/pon_isolate`
  after the platform settings it is applied at once, one register write,
  in either direction. The NIC layer below holds either way.

### The kernel: frames from the PON port are dropped

The CPU tag of every received frame carries the port it entered the switch
by (`odi_nic_hw.h`, the source port of RX word 3). That the field is right
is shown by function, on both ISPs: OMCI replies are sent to the source
port of the request and reach the OLT, and host frames are delivered to
`eth0.2` by source port 0.

`odi_poll()` (`odi_nic.c`) hands a frame to the RX hooks first, as before;
`odi_omci.c` takes the OMCI frames. A frame from the PON port that no hook
took is dropped there, before any net_device sees it, whatever its type.
`/proc/odi_nic` counts both, one line each (`rx_pon <n>`,
`rx_pon_dropped <n>`).
`rx_pon` is every frame from the PON port (OMCI included), `rx_pon_dropped`
the ones dropped; with the switch layer in place it stays 0, and `rx_pon`
equals the OMCI frames of `/proc/odi_omci` (`frames_delivered` +
`dropped_unregistered` + `unicast_fail`). A frame dropped here also counts
as `rx_dropped` of `eth0`. There is no switch to turn it off: nothing on
the stick wants a frame from the PON other than OMCI. The one other RX hook,
the copy of the host's IGMP to igmpd (`odi_omci.c`), is registered for the
UNI port alone, and only once igmpd first takes its lease, so it never sees a
frame from the PON and costs nothing on an image that never snoops; an IGMP daemon that
wanted the OLT's queries would need a hook for the PON port, which is a
decision about this rule and not a detail of a daemon. There is no
netfilter in this kernel, and only the driver knows the ingress port.

### Frames the stick sends

The rule is that the only frames the CPU may send toward the fibre are the
OMCI replies and, while igmpd takes the host's IGMP ([Snooping](#snooping)),
the host's own reports, re-sent in their place. Neither answers anything
that came from the fibre. The NIC transmits two kinds of descriptor:

- the OMCI replies (`odi_omci.c`, through `odi_nic_tx_words()`):
  `PORT_SEL`, `NO_LEARN` and bit 25 in word 0, the PON port's bit and the
  OMCC stream in word 3. igmpd's re-sent reports use the same words with an
  upstream flow as the stream (`odi_igmp_tx.h`), and only for what
  `odi_switch_mcast_send()` lets through. `PORT_SEL` is what puts a frame onto a
  PON stream (the vendor SDK's debug CLI calls it "force pon forward" and
  takes the stream id with it); a reply without it never reached the OLT;
- everything from the IP stack (`odi_start_xmit()`): no `PORT_SEL`, and the
  port mask of the device it leaves on, `odi_nic_stack_tx_mask()`
  (`odi_nic_hw.h`): 0x1 for `eth0.2`, 0x2 for `eth0.3`, 0x3 for `eth0`,
  nothing for `nas0` and `pon0`. Never the PON port, never the CPU port,
  and never 0, which would be a hardware lookup.

Whether the switch honours a port mask without `PORT_SEL` is not measured.
The vendor's own NIC driver sends every protocol-stack frame that way and
calls a non-zero mask a direct transmit (a zero mask is the lookup); read
so, the stick's ARP, broadcasts and replies already leave by the host port
alone. Read the other way (the mask ignored, the frame forwarded by
lookup), a broadcast from the stick (`FLOOD_BCAST_PORTS` 0xf) or an unknown
unicast would also reach the PON port. Setting `PORT_SEL` on those frames
is not the cure: it is the "onto the PON" bit.

So the switch layer covers the second reading: the CPU port's isolation
mask loses the PON port (above), and a frame the CPU sends that the switch
forwards by lookup can reach the host only. The OMCI replies are a
directed transmit, not a lookup, and the vendor forwarding engine applies
the isolation masks to directed transmits in software, which says the
hardware does not; that is a reading too, so the first trial checks that
OMCI keeps working with the mask in place. It is a property of the chip,
not of the OLT: if OMCI works with it on one OLT, it does on any.
`pon_isolate=N` is the way back here as well. The OMCI transmit path
itself (`odi_omci.c`, `odi_nic_tx_words()`, `odi_nic_xmit_raw()`) is not
changed by any of this.

### The stick's MAC

The switch learns a frame's source MAC on the port it entered by, whatever
the isolation masks let it reach afterwards. A frame from the fibre
carrying the stick's MAC as its source would move the stick's learned row
from the CPU port to the PON port, and frames from the host to the stick
would then be switched toward the fibre until the stick sent again.

The stick's MACs are therefore static rows on the CPU port
(`odi_switch_pin.c`, `odi_switch_l2.c`): bit 62 of an L2 row, the SDK's
"no SA learning", is never learned over. A frame whose source hits a static
row from another port is an "illegal port move", whose action is
`SA_MOVED` (0x01c000, two bits a port: 0 moves a learned row to the port
the frame came in by, 1 drops the frame and leaves the row). The boot replay
writes 0 on every port, and `odi_sw_sa_moved_apply()` sets the PON port and
the UNI (port 0) to 1 afterwards (`0x00000011`), leaving the CPU port and
port 1: whichever side learned a MAC first keeps it. A frame from the fibre
whose source is a host's learned row, such as one an OLT reflects, is dropped
instead of moving the host to the PON port, which would switch traffic for
that host toward the fibre. The other way round, a router that floods an OLT
frame back out of its SFP port (an IGMP query, #369) would move the OLT MAC to
the UNI, and the PON field would then drop every frame of that MAC from the
fibre until the row aged out (about 250 s, measured by the reporter); the
reflected frame is dropped instead. A MAC whose row has already aged out of
the PON is still learned from such a frame. A static row, as the stick's pin, is not moved
either way (another open implementation's reading). The row is the one the hardware
writes when it learns the stick's MAC from the CPU port (SVL, filtering id
0, port 3, see [the L2 table readout](KERNEL.md)) plus the static bit, with
the age non-zero as the SDK writes a static unicast row; written through
the hash, it lands on the learned row and replaces it.

Which MACs: the address of every `odi_nic` device that is up and
transmits (`eth0`, `eth0.2`, `eth0.3`), and of the bridge it is a port of
(`br0`, which takes `eth0.2`'s). A netdevice notifier (up, down, a new
address, a new bridge) recomputes the set and brings the table in step, so
`network.sh` setting `ELAN_MAC_ADDR` (link down, address, link up) moves
the pin with it; `switch_init` syncs once more before omcid starts. Each
change is logged:

    odi_switch: 02:00:00:00:00:04 pinned to the CPU port (static row 912)

The table is emptied once at the platform step, before the pins are
written. Nothing removes a static row after that except a MAC that is no
longer the stick's (the delete is by the row's own words, looked up again
afterwards, and logs a warning when the row stayed): no replay writes the
L2 table, `L2_TBL_FLUSH_CTRL` (0x017024) is set to flush dynamic rows only
and is never started (0x017028 is read as the flush port mask, not "the
flush itself"; claro reads 0 there), the bridge connections of cmd 51
do not touch the table, and ageing (cmd 62) and the learning limits (the
MAC bridge port MEs) apply to learned rows. Learning on the PON port stays
on: in a bridge, the OLT side's MACs are learned there.

What cannot be checked without the OLT side: a frame from the fibre with
the stick's MAC as its source. The static semantics rest on the field and
the port-move register, not on a measurement.

### Checking it on a stick

- `register get 0x27000 2` in `diag`: `0x003fffff 0x003fd807`.
- `l2-table get all` in `diag`: the stick's MAC `Static`, port 3, after
  the boot, after the OLT's provisioning and after an `apply.sh`.
- `/proc/odi_nic`: `rx_pon_dropped` 0 over a day, and `rx_pon` equal to the
  OMCI frame count; `diag mib dump counter port 2` shows the OLT's
  broadcasts arriving (`ifInBroadcastPkts` growing) all the while.
- OMCI: the ONU in O5 with its services, and `/proc/odi_omci`
  `frames_delivered` and `replies_sent_to_pon` growing together.
- The host still reaches the web UI, ssh and the exporter.

`test/odi_pon_isolation_test.c` starts from the words a stick reads,
checks the words the platform step leaves, asks a forwarding model where
each kind of frame from the fibre, and each frame the CPU sends by
lookup, can go, runs the NIC's PON test on RX descriptor words logged on a
stick (an OMCI frame and host frames), and checks the port mask and
`PORT_SEL` of every device's transmit descriptor.
`test/odi_switch_l2_test.c` checks the static row against the row the
stick learned for its own MAC and against the static bit as igmpd's row
showed it, and pins, moves and unpins MACs through its model of the table.
