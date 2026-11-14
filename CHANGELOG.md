# Changelog

Releases of the flashable image. Trial builds between releases are not
listed here.

## Unreleased

- Identity trial: `identity-trial.sh try KEY=VALUE...` changes `GPON_SN`,
  `GPON_PLOAM_PASSWD`, `LOID` or `LOID_PASSWD` as a trial. The values go to
  `/var/run/identity-trial.conf`, which omcid reads over the store, and omcid
  re-registers with them; a refused, timed-out or service-less
  re-registration, or a MIB reset loop that starts within 600 s on a line that
  had none, goes back to the stored identity by itself (`apply.sh omci
  --revert`). After 600 s, or `keep`, the values are stored. A reboot undoes a
  trial. `status` shows the ONU state, services and MIB resets before and
  after. The web UI does not use it yet. Refs #392.
- omcid: re-registrations are counted, and held back after refusals. Every
  re-registration a changed identity starts is one line in
  `/etc/config/odi-reregister` (the newest 16: time, change or revert, result,
  Deactivate_ONU-IDs, key names, never values). Three Deactivate_ONU-IDs
  before O5, or O7, end the wait at once with `result=refused`; one or two do
  not. After one refused change `apply.sh omci` warns; after two in a row
  (within 24 h) omcid holds a changed identity back (`result=held`, the ONU
  stays on the identity it runs) until `apply.sh omci --confirm`.
  `apply.sh omci --revert` is the confirmation for going back, recorded as a
  revert and skipped by the count. `omcli state` adds a `reregister` line,
  `event=reload_done` a `refusals` field, the status file `refusals`,
  `streak` and `today`. Refs #391.
- omcid: a MIB reset loop is named instead of left to be counted in the log.
  Three or more MIB resets from the OLT within 600 s log one
  `event=mib_reset_loop state=start` (warning), and `state=end` after 600 s
  without one. Each OLT `event=mib_reset` line adds `recent` (the OLT resets
  in the last 600 s) and `before`, the last six requests before it as
  `op:class.instance`, so the log shows what the OLT read or wrote just before
  it gave up. `omcli state` adds a `mib resets` line with the counts and
  whether a loop is on. Refs #393.
- Docs: `QUALIFICATION.md` adds a second HSGQ v08 report at 2.5G against a
  MikroTik RB5009 (`HOST_SERDES=2500basex`, v1.15.0 and v1.16.0, warm and cold
  boot, a live try and confirm). Refs #369.
- Slots: `nv slotcheck <slot>`, a partial check of a slot: the kernel
  partition holds a uImage whose magic, header CRC and data CRC are good and
  whose size fits the partition, and the rootfs partition starts with a
  version 4.0 squashfs superblock that fits it. Not proof that the slot
  boots; it catches a slot that cannot, such as a kernel left half-written
  by an interrupted flash. `slot-state.sh` runs it on the other slot at every
  boot, after the state file is first written (trial-guard.sh waits only 60 s
  for it), records `other_slot`, `other_slot_check` and `other_slot_detail`
  in `/var/run/odi-slot`, and adds `OTHER SLOT BROKEN` to the login banner
  and syslog when it fails. A successful `nv commit` warns, after its result
  line, when that recorded check failed (issue #379, point 3).

## v1.16.0 (2026-11-13)

- Watchdog: `odi_wdt` logs the control value it found at init
  (`found ctrl 0x...`), and the comments no longer say U-Boot arms the
  watchdog before every boot. It does so before a trial boot only; a
  committed boot reaches the kernel with the watchdog off (`found ctrl
  0x00000000` on a stick), so a committed image that hangs before `odi_wdt`
  starts is not reset. docs/KERNEL.md says so. Refs #379.
- `nv commit` also sets `sw_tryactive=2` in both environment copies, in the
  same writes as `sw_commit` and `mupgrade_en`. U-Boot clears it only in the
  copy it saves as it starts a trial, so the other copy kept the value that
  armed it, and a fall back to that copy would have started a one-shot trial
  nobody asked for. A `sw_tryactive` other than 2 in the primary copy is a
  trial queued since this boot, and `nv commit` now refuses it, nothing
  written, rather than drop it (`--force` does not override this). A stick
  committed earlier gets the fallback fixed by running `nv commit` again.
  Refs #379.
- Releases: confd v1.16.0. The web UI adds the static routes editor under
  Config, Network (`LAN_ROUTES`, empty by default, saved through the same Keep
  trial as the management address), the path loss as a meter of its own
  between Receive and Transmit with the window of the OLT fibre class, the
  equalisation delay under Registration and the fibre length a change of it
  equals, and both fibre rates on the stick diagram. The asset hashes in
  src/release-sha256.txt are the release SHA256SUMS, checked by
  `make releases`.
- Network: `LAN_ROUTES` (odi.conf), optional static routes for the management
  interface, so the stick can be reached from other subnets without NAT, e.g.
  `default via 192.168.1.253` behind a router that holds the stick subnet.
  Up to 8 entries, `DEST via GW`, `DEST` being `default` or an IPv4
  `a.b.c.d/len`, `GW` inside a management subnet; anything malformed turns the
  whole key off with one log line. Disabled by default. `network.sh` installs
  them after the addresses, at boot and on `apply.sh network`, on the device
  carrying the primary address only, with `ip route replace ... proto static`
  after flushing `proto static` there. `mgmt-addr.sh try LAN_ROUTES=...` makes
  a change a trial like the addresses, and a trial that changes only the
  routes is confirmed from any client. A safe-mode boot does not apply them.
  The routes change what the stick answers, not what reaches it: nothing is
  reachable from the fibre. Refs #367.
- GPON: the `ranging_time` line of `/proc/odi_gpon/status` (and the EqD the
  web UI and `gpon_ranging_eqd_bits` show) keeps only a Ranging_Time
  addressed to this ONU, as the activation already did for the EqD it
  applies. The PLOAM logs of our sticks have only shown their own ONU-ID
  and broadcast, but a shared PON carries every ONU its own Ranging_Time.
- Switch: the UNI drops a frame whose source MAC is learned on the PON
  port, as the PON port already did the other way round (`SA_MOVED`
  0x01c000 now `0x00000011`). A router that floods an OLT frame back out of
  its SFP port (an IGMP general query, say) made the stick learn the OLT
  MAC on the UNI and then drop every frame from that MAC on the fibre for
  about 250 s, which cut IPTV every few minutes. Refs #369.
- VLAN: in manual VLAN mode the downstream half of the add-tag rule matches
  the manual VID whatever the priority, and removes the tag. It matched the
  VID and `VLAN_MANU_TAG_PRI` (as the stock stack does), so a line whose OLT
  sends control traffic and data at different priorities on the VID lost
  the frames of the priority that was not configured, or with a FwdOp 0x00
  override got them tagged. `VLAN_MANU_TAG_PRI` now only sets the priority
  of the tag added upstream, always, except where another row could take the
  frames of the VID first and pass them tagged: the manual VID 0, a class 84
  filter that goes by priority alone, and an IP host with class 171 rules.
  There the rule is as before and `omcid` logs why. The idea and the guards
  follow #370. Refs #301.
- OMCI: ME 290, Dot1X port extension package, is modelled with all twelve
  G.988 attributes, as stored values (omcid runs no 802.1X authenticator
  and does not act on them; the state and status attributes stay zero, except the port status,
  authorized, and the server timeout, 30 s). An
  OLT that starts 802.1X on the UNI only after its Set of 290 is accepted
  had to be given `OMCI_UNKNOWN_ME_OK=1`; it is now answered with success
  by default. A Get returns the stored values, and the MIB upload lists
  the entity only after the OLT has Set it (the write-only action register
  included, as for the other write-only attributes of the model).
  Refs #301.
- Releases: confd v1.15.0 and metricsd v1.10.0. The web UI adds the
  "802.1X EAPOL without the service tag" toggle on Config, VLAN
  (`VLAN_EAPOL_UNTAGGED`), the OLT launch power and path loss under the
  receive meter (from the PON-ID), the shared PON load on Status, coloured
  zones on the optics meters, serial numbers typed as 16 hex digits, a
  warning beside Commit while `confirm-arp` or `breadcrumbs.on` exists, and
  refuses a reboot while a firmware write runs, a commit of a partition
  that is not running, a backup from a failed dump and a change to an
  `authorized_keys` it could not read whole. The exporter adds
  `gpon_cpu_seconds_total`, `gpon_olt_info`, `gpon_ranging_eqd_bits`, the
  PON-ID time, the counter sample superframe count and FEC bit, and the
  restart state of the respawned daemons; `OdiResetLoop` no longer fires on
  a power cut. The asset hashes in src/release-sha256.txt are the release
  SHA256SUMS, checked by `make releases`.
- OMCI: a respawned `omcid` runs the rebuild that was pending when the last
  one died (issue 357, found reading the code, not seen on a stick). A
  Create or Set is answered and saved at once, and the switch is programmed
  a quiet second later. An `omcid` that died in between was resumed with the
  new MIB and the sync counter the OLT expects, and the change stayed out of
  the switch until some later message caused a rebuild. The snapshot (format
  7) now carries the pending rebuilds, and the resume sets them again. A
  resume with nothing pending still makes no driver call.
- Watchdog: `odi_wdt` takes its uptime from the 64-bit tick count (issue
  356, found reading the code; no stick of mine has been up that long). The
  quotient of the 32-bit `jiffies` was back at 0 after 198.8 days of uptime.
  From then on the heartbeat line stayed away for another 198 days, and a
  client that pinged last before the wrap and hung before its next ping was
  not reset. A healthy stick was not reset by it.
- Management address: `mgmt-addr.sh confirm` reads back the roll-back after
  a key that does not stick (issue 360, found reading the code, not seen on
  a stick). When the roll-back fails too, the store holds new values next
  to old ones, a set that was never tried and that the next boot would
  use; the refusal said "nothing is kept" regardless. It now names each key
  that could not be put back with the value the store holds. The keys are
  still written one at a time, so a power cut between two of them can leave
  the same mix.
- Host link: a trial of `HOST_AUTONEG` or `HOST_SERDES` goes back at once
  when its state file cannot be written after the switch (issue 358, found
  reading the code, not seen on a stick). With `/var` full the file kept
  saying the mode before, so the expiry and `revert` skipped the hardware
  write and logged the port as back while it stayed in the unconfirmed
  trial mode until a power cycle.
- Logs: the log trim in `rcS` keeps the last 128 KB of a log when `/var` is
  full (issue 359). It wrote its copy next to the log. With `/var` full the
  copy came out empty or cut short, busybox `tail` exited 0 all the same,
  and the log was replaced by that: the space came back, the log was gone.
  The copy goes to `/var/tmp` now, a tmpfs of its own, and is judged by its
  size; a log whose copy cannot be made whole there either is emptied.
  Checked with the image busybox on a full tmpfs under qemu, not on a stick.
- Flash: `fwu.sh` no longer accepts a read-back that ended early. The
  read-back pipes the tar member into `cmp` and took "EOF on -" for success,
  which is what `cmp` says when the member ends before the partition does,
  and also what it says when tar dies halfway (the OOM killer, say) and only
  a prefix was compared. tar's exit status is now part of the check: a
  non-zero one is `READ-BACK INCOMPLETE` and exit 1, before the kernel is
  written. The write itself was already checked by tar's status, and both
  members by md5 before the erase, so this closes a gap in the extra check
  only. Found reading the code, not seen on a stick.
- Flash: `fwu_starter.sh` waits up to 10 s (`FWU_START_S`) for the
  background job to claim the state file, instead of one second. On a loaded
  stick a job that took longer was reported as "the write job did not start"
  and then ran and wrote the slot anyway. A job that starts after the
  starter gave up now exits without writing. Found reading the code.
- Safe mode: a safe-mode boot can now be reached and stays up in three cases
  where it could not (issues 351 and 352, found reading the code, not
  seen on a stick). rcS no longer registers `omcid` with the kernel watchdog
  in it, and `services start omcid` does not arm the deadline there: with
  an omcid that dies before its first ping at every start, the safe-mode
  boot was reset after 60 s like the three boots before it, and the stick
  looped. `svc-confd.sh` and `svc-dropbear.sh` ignore `confd.off` and
  `dropbear.off` in a safe-mode boot and leave the files in place: with both
  set, that boot had the rescue address and the default password but neither
  the web UI nor ssh, and no reflash removes the files. And with no ssh host
  key and no way to write one, a safe-mode boot runs dropbear on a key in
  RAM instead of parking it. A normal boot is unchanged.
- Network: `network.sh` picks the management device by where the address
  is, not by whether `br0` exists (issue 353, found reading the code). When
  the bridge path fails after `br0` was created, the boot puts the address
  on `eth0` and an empty `br0` stays behind; `network.sh configured` then
  looked at `br0`, found nothing and never confirmed that boot to the
  watchdog, and `addr` and `primary` acted on the empty bridge too. All
  three modes now use `eth0` when `br0` holds no address and `eth0` does.
- Build: `kernel/618/fetch.sh` checks the kernel signature with `gpgv`
  against the stable-release signing key pinned in the script, next to its
  fingerprint, instead of fetching that key from a keyserver on every run.
  The keyserver call had no time limit: it usually answers in 2 to 8 s, but
  on 2026-11-11 it hung for 3 to 15 minutes at a time, which held the CI
  `clean-build` job (normally under 1.5 minutes, cold cache included) for up
  to 15 minutes and once past its time limit. The second keyserver never
  returned a usable copy of the key. When neither answered, the check fell
  back to the pinned SHA-256 alone; now the signature is always checked,
  with no network, and a bad or missing signature, or one made by any other
  key, fails the fetch. `gpgv` (part of GnuPG) is now listed with the build
  prerequisites.

- `nv setenv` and `nv commit` no longer take an environment copy they could not
  read completely for an invalid one. A failed or short read of a copy that is
  in fact valid (a transient error on `/dev/mtdN`) used to send the repair
  down the invalid-copy path, which overwrote that copy with the other one and
  lost whatever was only in the newer copy. Now only a copy read in full whose
  CRC is wrong is repaired; if either copy cannot be read completely, nothing
  is written, `-c N` included, and the tool says `a copy could not be read
  completely, nothing written` and exits 1 (`setenv` says so also when the
  other copy is invalid or unreadable too). Running it again repairs as
  before; while a copy stays unreadable `commit` (`--force` too) and `setenv`
  stay refused. `nv getenv` and `nv fallback` are unchanged. Host tests in
  `src/nv/test/test_commit.c` fail one read on a valid copy for `commit` and
  for each form of `setenv`; the power-cut tests of the repair now reach the
  write they cut (with `env2` invalid the fixture's `env` named the wrong slot,
  so `commit` stopped before writing and the test passed without a cut) and
  assert the result code and the number of writes.

- `omcicli set logfile` and `set resetlauth` now say `omcid does not
  implement ...; nothing was sent` and exit 1 when omcid is the daemon, like
  the other stock setters: omcid accepts `set logfile` and does nothing, and
  answers `set resetlauth` with an empty reply that printed as if it had
  worked. New host test `test/omcli_table_test.sh` compares the command table
  of `omcli` with `vq_dispatch()` in `respond/vqsrv.c`, so a setter omcid
  gains or lacks cannot go unmarked. Cases in `src/omci/writes-test.sh`.
  Refs #301.

- `nv setenv` and `nv commit` now repair an invalid environment copy as part
  of the write instead of refusing, or erasing the only good copy. With one
  copy invalid (a write cut by a power loss), the new environment is built and
  checked in memory (it fits, the value is in it), written to the invalid copy
  and read back, and only then is the valid copy rewritten and read back, so a
  second power cut still leaves a valid copy. `commit` used to refuse with one
  invalid copy and `setenv -c N` refused to write it; a plain `setenv` used to
  erase the only valid copy with just a warning, which is gone. The repaired
  copy gets the flags byte that keeps the valid copy the one U-Boot picks
  until the sequence ends (the same byte when it is `env2`, one below when it
  is `env`; with `env2` at 0 the repaired `env` wins, which is safe because it
  already holds the whole new environment). Every other refusal of `commit`
  stays: a slot that is not the running one, not `sw_active` in the valid
  copy, no valid copy at all. Host tests: `src/nv/test/test_commit.c`, with a
  fake flash that cuts a write half way at each step. Refs #341.

- The diagnostics bundle now includes `pon/omcli_vlan.txt` (`omcli vlan`): the
  VLAN handling (the stick adds the manual tag, or it is transparent) and any
  `FwdOp` override read from `RTK_DEVINFO_SPECVER`, so the bundle alone shows
  whether the override is in force. The output is numbers and keywords: no
  serial, password or MAC. Host test: `src/omci/voip-test.sh`. Refs #301.

- `omcicli mib set|create|delete` and the other stock setters omcid does not
  implement (`set log|sn|devmode|dmmode|loid`, `debug simavc`) now say
  `omcid does not implement ...; nothing was sent` and exit 1 when omcid is the
  daemon, instead of sending a request omcid drops and printing nothing, which
  read as a change that had been made. Against the stock daemon they are sent as
  before. `docs/TOOLS.md` lists them. Host test: `src/omci/writes-test.sh`.
  Refs #301.

- omcid times the 5 s cap on a pending rebuild from the first change to the
  upstream QoS or the bridge connections, no longer from the start of the
  daemon. The multicast flag is set from the start, so on a busy channel the
  cap ran out 5 s after omcid started and could rebuild the bridge connections
  (which removes every service first) from a half-provisioned MIB when the
  OLT's burst began inside those seconds. A change that only touches the
  multicast policy (a Set of class 309 or 310 that leaves out the tag, the
  access lists and the profile pointer) is not capped and waits for the quiet
  second, as before the cap. The cap rebuilds all three as before. Host test:
  `src/omci/mcast-test.sh`. Refs #340.

- omcid no longer holds the rebuilds off for as long as OMCI frames keep
  coming. The upstream QoS, the multicast policy and the bridge connections
  were rebuilt one quiet second after the last OMCI frame, and every frame of
  the line (Gets and PM polls too) restarted that second, so an OLT sending
  more often than once a second would have kept the rules from ever being
  programmed. A pending rebuild now runs at the latest 5 s after the first
  change marked it, however busy the channel is; the quiet second is
  unchanged otherwise. Host test: `src/omci/mcast-test.sh`. Refs #340.

- `VLAN_EAPOL_UNTAGGED` follow-ups from a review. omcid now reads the key
  with the VLAN keys and holds it, so an edit of `odi.conf` takes effect on
  `apply.sh omci` only: before, a rebuild the OLT triggered applied the file
  as it stood, and the reload that followed (or the retry after a failed
  re-registration) could report no change while the switch kept rules the key
  no longer asked for. When the OLT gives the manual add-tag rule to more than
  one GEM port, the EAPOL rules now go once, on the GEM port that carries the
  untagged frames up (the last one installed), instead of on the first. With
  `VLAN_MANU_TAG_VID` 0 the VID 0 rule is no longer built, so the service's
  frames of other priorities are not passed with their tag. A downstream
  EtherType row with no room is now logged as such, not as a full table.
  `docs/SETTINGS.md` now says that the downstream rules match frames of every
  GEM port, and what would happen if the switch ignored the EtherType
  downstream. With the key off the rules are byte for byte what they were.
  Host tests: `src/omci/reload-test.sh`, `src/omci/vlan-test.sh`,
  `test/odi_switch_bdgconn_vlan_test.c`. Refs #338, #301.
- New odi-only key `VLAN_EAPOL_UNTAGGED` (`/etc/config/odi.conf`, off by
  default, live with `apply.sh omci`): with the manual tag on ("stick tags"),
  802.1X EAPOL frames (EtherType 0x888e) cross the stick untagged in both
  directions while every other frame keeps the manual tag, for an ISP that
  authenticates the line untagged or on VLAN 0 and carries the service on a
  VLAN. Three bridge rules go beside the add-tag rule on its GEM port
  (`eapol-up`, `eapol-down`, `eapol-down-vid0`, shown by `omcli bridge`); the
  add-tag rule itself is unchanged, and with the key off the rules are byte
  for byte what they were. The switch driver now also accepts an EtherType
  row for untagged frames downstream. Not yet run against a line that uses
  EAP, and the downstream EtherType row has not been run on a stick
  (`docs/SETTINGS.md`, `docs/SWITCH.md`). Refs #301.
- `confirm-arp` guard. The development file now holds the IPv4 address to
  ARP (one line, whitespace ignored) instead of the hard-coded `.2` of the
  `br0` subnet; an empty file, or content that is not a dotted IPv4 address,
  is no target: the boot logs it (stderr and a crumb) and confirms the
  watchdog as usual, so a misconfigured toggle cannot cause a reset loop. The
  other `arping` diagnostics in `rcS.dev` use the same address and are
  skipped, with a log line, when there is none. `nv commit` refuses while
  `/etc/config/confirm-arp` exists, naming the file and why, unless
  `--force` (or `-f`) is given; `breadcrumbs.on` only gets a one-line note.
  The ssh login banner names both files while they exist, committed slot or
  not (the confirm-arp address, or "no address: has no effect"). Host tests:
  `test/rcs_dev_test.sh`, `test/slot_state_test.sh`, `src/nv/test`; the rcS
  trace has a confirm-arp scenario with an address and one with an empty file,
  both also running rcS.pon, where the confirmation happens. Refs #334.
- Docs: new `docs/QUALIFICATION.md`, linked from the README's "Status and
  limitations" and "Documentation". It records, per board, laser driver, host,
  host SerDes mode and OLT line (named ISP1 and ISP2, never by make) and per
  feature path, whether it is maintainer-tested on a stick, community-reported
  (issue linked), host-tested (test named) or untested, with the release it was
  last checked on, so it changes when something is checked again and not per
  release. It also states the minimum qualification suite a hardware path must
  pass before it is called supported, and what a community report should
  carry, including a diagnostics bundle without `--full`. The README's
  statements on fibre-pull recovery, the watchdog reset, MLD snooping, the
  trial guard and the 2C3 are brought in line with it. Refs #268.

- Releases: the publishing job runs in a protected `release` environment,
  so a release is published only after the maintainer approves it, once
  all of CI has passed on the tag commit. The repository now also keeps
  release tags (`v*`) immutable and signed, lets only admins create them,
  and asks for a pull request with green CI on `main` (admins can still
  push release commits). Refs #259.

- Releases: confd v1.14.0 and metricsd v1.9.0. The web UI has the "VLAN
  forward operation override" field on Config, VLAN (`RTK_DEVINFO_SPECVER`,
  #301), a diagram of the stick on the Status tab, a faster page start
  (two requests instead of nine, cached image files), and makes the OLT
  lock of the Ethernet port opt-in to match `OMCI_UNI_LOCK`. The exporter
  adds `gpon_olt_launch_power_dbm`, `gpon_pon_id_odn_class` and
  `gpon_pon_id_reach_extender` from the PON-ID line (#321). The asset
  hashes in src/release-sha256.txt are the release SHA256SUMS; confd and
  metricsd were matched byte for byte by local builds of the tags, the
  assets tarball by its contents. `test/sbom_test.sh` takes the pinned
  metricsd tag from `src/fetch-releases.sh` instead of naming one.

- omcli: a request to omcid gives up after 0.5 s when omcid is not taking
  it, with "omcid did not take the request: its queue stayed full for
  0.5 s" and status 1. Its own commands (`omcli state`, `omcli mib`,
  `omcli provision`, which the exporter runs as `omcicli provision`) used
  to wait with no bound for room in the queue of an omcid that had
  stopped reading it; the exporter SIGKILLs omcicli at 2 s, which left
  the reply queue of omcli behind for good, and the kernel has few queues
  to spare. Now omcli ends first and removes its queue. The stock-format
  commands and `--inject` were already bounded (0.5 s, and 30 s for a
  frame). Tested in `qemu-test.sh` against an omcid stopped with SIGSTOP.

- Build: `toolchain/image.sh` bounds its docker calls. A pull of a
  toolchain image gets 120 s (`PULL_TIMEOUT`; on GitHub runners these
  pulls take 2 to 8 s) and is tried up to 3 times (`PULL_TRIES`), 10 s
  apart (`PULL_PAUSE`), so a registry error no longer fails a CI job at
  once, and a pull or a local image lookup (30 s) that hangs fails with a
  message instead of holding the job until its own time limit. On a host
  without `timeout` (stock macOS) the calls run unbounded, as before.
  `test/toolchain_image_test.sh`, in `make test-host`, checks each case
  against a stub docker.

- Tests: the OMCI suites could check a line omcid had not logged yet.
  `inject_wait` returned as soon as the answer to a frame was in the log,
  but omcid answers first and then applies the change and logs what it
  did (`-> created`, the rate limiter slots, a refused 802.1p mapper), so
  a check right after it could run ahead of those lines; `qemu-test.sh`
  failed "the row is stored" that way. Injecting Creates and checking at
  once missed the line 9 times in 200 on a loaded machine. `inject_wait`
  now also makes an `omcli help` round trip after the answer: omcid serves
  omcli only between two frames, so the reply means the frame is done
  (0 misses in 200). It costs about 13 ms per frame. `qemu-test.sh`, 16
  runs side by side: 3 failures in 128 runs before, none in 128 after.

- Tests: `make test-qemu` could fail with "dropbear did not print
  /etc/motd at an interactive login" now and then. The check typed `exit`
  into the login shell and closed its input at once; dropbear closes the
  session as soon as the client input ends, and a motd it had not passed
  back yet was dropped (against the same dropbear on a host, 158 of 200
  such logins came back without it). The check now keeps the input open
  until the motd line is back, up to 20 s, and only then types `exit`
  (all 397 logins that connected came back with it). The TRIAL BOOT
  banner check goes the same way. Nothing changes on the stick.

- `/etc/scripts/flash set` no longer installs a truncated `odi.conf` or
  XML store when a write fails, for example on a full config partition.
  The grep and printf that build the new `odi.conf` are checked on their
  own, and every rewrite (`odi.conf`, the XML store, `default cs`) is
  checked byte for byte against the same rewrite run again before it
  replaces the file, so a short file is refused even when the tool that
  wrote it reported success; the old file is left as it was and the set
  fails with a message. Two `flash set` (or a set and a `default cs`) at
  once no longer lose one change: writers take an flock on a lock file,
  the same mechanism as the firmware writer lock, waiting at most 5
  seconds before failing with "busy". Refs #326.

- Tests: two host tests no longer fail now and then under load. In
  `count_proof_test.sh` the stub `diag` rewrote its owner file in place, so
  a reader that opened it mid-write took the holder for dead and dropped
  every counter (the run then lacked a verdict); the owner and the counter
  files are now written whole and renamed into place. In
  `fwu_starter_test.sh` the four starters of the race check shared one
  `cmdline` file, which one starter rewrote while another read it empty and
  refused to write; each call now has its own file.

- OMCI respawn: the re-provisioning helper no longer acts on the previous
  omcid's resume decision. `svc-omcid.sh` started the helper before the new
  omcid existed (the supervisor can wait up to 2 s first), and a stale
  `resumed` or `reprovision` file still in `/var/run` answered for the new
  run. It now moves the old file to `omcid-resume-decision.prev` first, so
  the helper only ever reads the new omcid's decision; omcid treats either
  file as proof of a restart and removes both. Refs #324.

- OMCI: the MIB data sync counter now goes from 255 to 1 instead of to 0.
  G.988 keeps 0 for a MIB just instantiated or reset, so after 256
  successful Create, Set or Delete commands since the last MIB reset the
  ONU reported 0 where the OLT expected 1, which an OLT audit may answer
  with a MIB upload or a reset. One helper now advances the counter at
  all four places. Refs #325.

- Docs: the Luleey LL-XS2510 is listed as reported working, on one user
  report, with `HOST_SERDES=2500basex` set before the first boot. The
  README now records the first hardware runs of the host SerDes mode
  (`2500basex` linked, `hisgmii` passed no frames on that host) and of
  the trial guard (a trial whose host link passed no frames went back to
  stock by itself). Refs #321.

- GPON: the driver decodes the PON-ID message some OLTs send (G.984.3
  Annex C), which it used to log once as unexpected.
  `/proc/odi_gpon/status` has a `pon_id` line with the ODN class, whether
  the launch power is the OLT's or a reach extender's, the launch power
  (TOL, 0.1 dB steps above -30 dBm, so the path loss can be estimated
  from the stick's receive power), the PON identifier the operator set
  and when it came; `pon_id none` before one arrives and after a loss of
  signal. The diagnostics bundle masks the identifier, there and in the
  PLOAM ring, unless `--full`. Refs #321.

- `/proc/odi_gpon/counters` now also has the downstream superframe count
  and FEC bit of the sample its totals come from, as `ctr_ds_superframe`
  and `ctr_fec_now ds`. A reader that works out a share of the downstream
  frames, such as the idle GEM frames per frame, gets both from one read;
  `status` has the same values, but a read of a second file can land on
  the next sample. `status` keeps its lines. Refs #319.

- OMCI: with no class 84 filter, the fallback bridge rules of an 802.1p
  mapper that sends its priorities to more than one GEM port are now held
  to the priorities each port carries; every target used to get the same
  tagged rule. A tagged rule becomes one upstream rule per priority, the
  first keeps the downstream leg and the VLAN membership, and untagged
  upstream frames go to one target: the mapper default priority when its
  unmarked-frame option is 1, otherwise priority 0. A mapper with a
  single target, with or without unmapped priorities, keeps its one rule.
  A rule that cannot be installed is logged with its ingress and GEM
  port. Not run against an OLT that splits priorities. Refs #306.

- OMCI: a bridge rebuild in an omcid that resumed from a snapshot now
  withdraws the services the snapshot restored. It took the ids to withdraw
  from a list only the building process had, which is empty after a resume,
  so a VLAN change then left the old rules in the switch beside the new
  ones. The rebuild goes by the service table itself. `omcli bridge` rules
  are in that table too and are withdrawn by the next rebuild. The resume
  regression resets the resumed MIB and checks that every restored id is
  withdrawn; not run under qemu or on hardware yet. Refs #305.

- OMCI: deleting a downstream GEM CTP now withdraws its own flow when no
  surviving CTP or multicast table owns that port, so replacing CTPs on
  new GEM ports no longer uses up the 64 flow slots. Only a downstream or
  bidirectional CTP counts as the owner of a downstream flow: the port of
  an upstream-only CTP can be an extra multicast port, and a CTP that is
  Set to upstream-only gives its downstream flow back. A withdrawal the
  driver refuses keeps the slot, a broadcast selection is cleared before
  its flow is freed, and deleting the CTP that holds the broadcast flow
  moves it to a surviving one. Not run against an OLT. Refs #307.

- Firmware: `fwu_starter.sh` takes a file lock before it touches the
  shared extraction directory, in both modes, and the flasher inherits it,
  so the lock lasts until the write ends even if the starter is killed. A
  foreground write was not recorded anywhere and a second starter could
  run beside it; it now also claims the state file, so the upload guard
  sees it, and refuses to flash when it cannot. The web UI upload guard
  still reads the state file only. Host-tested with stub writers, four
  starters at once included; no flash written on a stick. Refs #308.

- Apply: when a disabled service has no daemon pid, wake all anonymous
  service placeholders so its own init entry rereads the setting. Signal
  errors are reported as failures. Host-tested with two placeholders;
  activation on a stick remains untested. Refs #309.

- Multicast: retain every potentially programmed MAC until its hardware
  deletion succeeds, including IPv4 and IPv6 groups already removed from
  membership state. Retries recompute current MAC owners, so an alias or
  rejoin is not deleted. A full tracking table defers new writes rather
  than losing failed deletions. Host fault-injection and sanitizer tests
  pass; real multicast hardware testing remains open. Refs #248.

- VLAN: the forward operation (FwdOp) of the OLT's VLAN filters can now be
  overridden, with the same setting and format as the stock firmware's "VLAN
  Forward Operation" page: `RTK_DEVINFO_SPECVER`, `EntityId[:FwdOp]` entries
  separated by commas (`0x1102:0x00,0x110b`; no FwdOp means 0x02). Some
  lines need it: an OLT that filters on the service VLAN with FwdOp 0x10
  while sending its 802.1X EAP frames on VLAN 0 has them all dropped, and
  the router never authenticates; 0x00 lets them through. Applied without a
  reboot, the ONU stays in O5. The filter keeps the value the OLT wrote, so
  the FwdOp page still shows 0x10; `omcli vlan` lists the overrides in force
  and omcid logs each one once. Refs #301.

- CI: four causes of failures that had nothing to do with the change under
  test, from the last two weeks of runs. The SIGTERM and SIGKILL cases of
  `test/mcast_probe_test.sh` gave the probe script 5 s to install its probes,
  which a runner busy with the other legs of `make test` sometimes exceeded
  ("the count window never started"); they now wait up to 30 s, with a
  window long enough to cover it. `src/omci/reload-test.sh` read the daemon
  log as soon as the status file said done, but omcid writes the status file
  first, so the line could still be on its way ("O5 never reached: logged");
  a log check now waits up to 5 s for its line. `tools/bump-deps.sh` moved
  the kernel and release pins but not the versions `README.md` and
  `docs/BUILDING.md` name, so every weekly bump failed its own `make lint`
  and no bump PR was opened; it now rewrites those too. And the most common
  one: `test-rcs` installed strace from a Debian snapshot mirror inside the
  test container on every run, and failed whenever the mirror timed out.
  The freestanding toolchain image is now pinned at v4, which carries
  strace, and the test installs it only when an image lacks it.
  `test/apply_test.sh` gave `apply.sh` 2 s to see the stub omcid take the
  signal and 4 s for the outcome, while the stub ran its handler only after
  its current one-second sleep, and gave the stub a fixed second to start;
  the stub now runs its handler at once, the test waits for it to start, and
  the bounds are generous except in the two cases that test them.

- Config: `flash all cs`, which the web UI runs every time it loads the
  config, reads `odi.conf` in one pass instead of starting a `sed` for each
  odi key, about 70 processes. On the stick it went from 0.36 s to 0.03 s,
  with the same output. `test/flash_test.sh` checks the order and the
  first-line-wins rule.

- VLAN: frames the router tags itself now pass when the OLT's extended VLAN
  table adds a tag to untagged frames and the line has no VLAN filter (class
  84). With `VLAN_CFG_TYPE` other than 1, omcid built only the add-tag rule
  in that case, so a router sending PPPoE on its own VLAN got nothing
  upstream, while the stock firmware passed it. Tagged frames now go by the
  table's single-tag default rule: forwarded unchanged when it passes them or
  the table has none, still dropped when it discards them. The manual tag
  and lines with a VLAN filter are unchanged. Refs #302.

- Licensing: `reuse lint` now fails on a source file without an SPDX
  header. `REUSE.toml` annotated the whole tree (`**`) as a fallback, so a
  new script or C file that forgot its header still passed; it now lists
  only the kinds of file that take no header (markdown, text, XML, goldens,
  the IGMP replay data, `LICENSE`, the kernel config and five rootfs config
  files). The nine files that relied on the fallback and do take a comment
  (`.gitignore`s, `dependabot.yml`, the issue template, `toolchain/images.env`,
  the busybox config fragment, `rootfs/devices.pseudo`,
  `src/diag/test/hw_fake_common.inc`) got their header. Refs #277.

- Releases: the SBOM is now complete and checkable. `sbom.cdx.json` tells
  runtime components from build tools (gcc and binutils are `excluded` and
  nothing at runtime depends on them), gives every runtime component a
  version, purl, license and `bom-ref`, adds a dependency graph, and lists
  every file the image carries with its SHA-256 and rootfs path: the busybox,
  dropbear and iproute2 binaries, our own daemons, `confd` and `metricsd`
  (hashes from `src/release-sha256.txt`), the `confd` web assets and the three
  register replay tables. Upstream source tarballs carry their pinned hash.
  The SBOM is in `SHA256SUMS` and a subject of the provenance attestation.
  `tools/verify-sbom.sh sbom.cdx.json --sums SHA256SUMS [--rootfs DIR]`
  checks it with python3 alone; the release job runs it before publishing and
  fails when a hashed artifact is missing, and `test/sbom_test.sh` (in
  `make test-host`) covers it with a generated SBOM and deliberately broken ones.
  `tools/generate-sbom.sh` is now a wrapper of `tools/sbom.py`. gcc now carries
  the `GCC-exception-3.1` SPDX expression. docs/FLASHING.md and docs/LICENSING.md
  describe it. Refs #260.

- CI: a release is published only after the whole of CI has passed on the
  tag commit, and only for a commit that is on `main`. `release.yml` now
  first checks that a final tag is on a commit that is an ancestor of `origin/main`
  (`tools/check-release-ancestry.sh`, covered by `make test-host`), then
  calls `ci.yml` as a reusable workflow, so the QEMU boot test, the rcS
  trace goldens, every OMCI suite, `full` and the two-build `reproducible`
  comparison must all succeed before the release job starts; it no longer
  waits for a smaller `make test` copy of them. `ci.yml` stops starting by
  itself on `v*` tags (the call replaces that run, so a tag costs one CI
  run, not two). Running `release.yml` by hand is a dry run that stops
  before publishing. Pre-release tags (`vX.Y.Z-beta.N`) skip the ancestry check, since
  they test unmerged work, but need the same CI call to pass.

- Licensing: every project-owned source file now carries an SPDX copyright
  and license header, and the rest is annotated in `REUSE.toml`; the tree
  passes `reuse lint` (REUSE 3.3), which `make lint` runs when the tool is
  installed and CI always runs, with a pinned, hash-checked version
  (`tools/reuse-requirements.txt`). The 169 headers that said `GPL-2.0`
  (the deprecated identifier, meaning GPL-2.0-only) in `kernel/extra/` and
  `test/` now say `GPL-2.0-or-later`, as the README states for our own
  code. The kernel patches are annotated GPL-2.0-only, like the upstream
  files they modify. `docs/LICENSING.md` says the same. The texts are in
  `LICENSES/`. Refs #277.

- CI: the two tests that install packages inside their Docker container
  (`make test-rcs`, for `strace`, and the stock-flash test) now go through
  `tools/ci-apt.sh`, as the runner steps already did: a read timeout and
  retries per connection, a time limit on each apt call and two attempts.
  The Debian snapshot mirror returning 504 for `strace` held test-qemu in
  an unbounded `apt-get` for 42 minutes until the job timeout cancelled it;
  it now fails in minutes and says the install failed. `tools/ci-apt.sh`
  skips `sudo` when it already runs as root. No change to the image.

- Build: release images are now reproducible. Two clean builds of one commit
  (`ROOT_PW=locked`) give the same tarball, byte for byte; `docs/BUILDING.md`,
  "Reproducible builds", lists every input and how it is fixed. What changed:
  `image/Dockerfile` builds from `debian:bookworm-slim` pinned by digest with
  its packages from a dated snapshot.debian.org archive, instead of a moving
  tag and the current archive; `SOURCE_DATE_EPOCH` (the committer date of
  `HEAD`, `tools/source-date-epoch.sh`) replaces the clock in the image
  manifest, `/etc/version`, the default version date, the squashfs
  (`-mkfs-time`, `-all-time`) and the tar (sorted, owner root, fixed times and modes), and
  `tools/remote-build.sh` passes it to a host whose copy has no history; the
  rootfs is staged under a fixed `umask`; busybox is built with
  `KCONFIG_NOTIMESTAMP=1`, so its banner no longer embeds the build date.
  `tools/compare-images.sh` now compares the tarballs byte for byte, with no
  excluded component (busybox included), and on a difference names the tar
  headers, member, uImage header, or rootfs inode and file that differs.
  Refs #258.

- Kernel: hardening options on, each measured. New: `FORTIFY_SOURCE`,
  hardened user copies, heap memory zeroed on allocation and on free,
  list-corruption checks that stop the kernel (`LIST_HARDENED`,
  `BUG_ON_DATA_CORRUPTION`), the stack-end check on every schedule,
  `dmesg_restrict`, the user heap start randomised too, mmap randomisation
  widened to 15 bits and nothing mapped below 64 KB; and, as `sysctl.*` words
  on the built-in command line, `kptr_restrict=2` and the four
  `fs.protected_*` rules. The strong stack protector and zeroed stack
  variables, already on by default, are now stated. 9,410 bytes of uImage
  (1,172,555 of 1,359,872, 187,317 spare). docs/KERNEL.md ("Hardening") has the
  matrix: what this CPU and toolchain support (no read-only kernel text, no
  kernel address randomisation, no virtually mapped stacks on this MIPS; a
  trapping UBSAN would emit trap instructions the core does not run; gcc
  plugins do not build with the toolchain image), the bytes each option
  costs, and why `ZERO_CALL_USED_REGS`, the slab freelist options (they need
  `SLUB_TINY` off) and `PANIC_ON_OOPS` stay off for now.
  `tools/check-kernel-hardening.sh` fails `make lint` when the seed loses one
  of the options and `kernel/build.sh` when `olddefconfig` drops one.
  Trialled on hardware: an overnight run on a stick in service (O5, OMCI and
  forwarding as before, no oops, free memory flat). Refs #254.

- Hardening of every freestanding binary (omcid, igmpd, diag, nv, omcli,
  omciprobe, omcicap), from one place, `src/harden.mk`:
  `-fstack-protector-strong`, `-fstack-clash-protection`,
  `-ftrivial-auto-var-init=zero`, `-fno-strict-overflow`, `-Wvla` and
  `-z noexecstack`. The stack protector runtime is our own, in `start.S`:
  the guard is seeded from the kernel's `AT_RANDOM` bytes before `main`, and
  a smashed guard writes `stack-protector: stack smashing detected,
  aborting` to stderr and `/dev/kmsg`, then ends the process with SIGABRT
  (init respawns it). The binaries no longer declare an executable stack
  (`PT_GNU_STACK` was `RWE`, from `start.S` lacking its `.note.GNU-stack`).
  Every build checks the result with `tools/check-hardening.sh`, and `make
  test-diag` runs an overrun under qemu (`make -C src/diag ssp-test`). Text
  grows 2-13% per binary (omcid +13.3 KB, +6.5%). FORTIFY, PIE, RELRO and
  BIND_NOW do not apply to binaries with no libc and no dynamic linker;
  `docs/CROSS-COMPILING.md`, "Hardening the freestanding binaries", says
  why for each.

- Respawned daemons get a restart budget. busybox init restarts a daemon the
  instant it exits and has no rate limit, so one that died at once was
  started again and again. `respawn.sh`, the last step of every `svc-*.sh`,
  now waits 1, 2, 4 ... seconds before restarting a daemon whose previous run
  lived under 30 s, up to a cap per daemon (omcid 2 s, dropbear 10 s, confd
  and syslogd 30 s, metricsd, klogd, ntpd and igmpd 60 s); a run of 30 s or
  more, and the first start of the boot, never wait. It is still the same
  process exec-ed in place under init, so signals, `reboot` and pid tracking
  do not change, and it runs only after rcS has returned, so the 120 s
  userland confirmation is untouched. State goes to `/var/run/respawn/<name>`
  (`state=ok|backoff|degraded`, `starts`, `fast`, `delay_s`, `last_run_s`,
  for the exporter to read later) and each restart is one
  `respawn: event=respawn service=... state=... fast=... delay_s=...` kernel
  log line (every tenth beyond the fifth short run). Nothing disables a
  daemon, enters safe mode or reboots: omcid stays with the watchdog, and
  the others are retried at their cap. `services stop` also ends a daemon
  that is in its wait. runsv, already in busybox, was weighed and not used
  (fixed one-second wait, no count, and a supervision tree that changes how
  every daemon is started and found). docs/SETTINGS.md, "Restart budget",
  has the policy; `test/respawn_budget_test.sh` (every `respawn` entry,
  fake clock, in `make test-host`) and a repeated `kill -9` scenario in
  `make test-qemu` cover it. Refs #271.

- omcid: an OLT lock of the Ethernet UNI no longer takes the host port down
  by default. On this SFP that port is the stick's only management path, so
  a lock from the OLT (ME 11 administrative state) cut the web UI, ssh,
  syslog and metrics until the OLT unlocked it, and looked like a frozen
  stick from the host. `OMCI_UNI_LOCK` is now opt-in: `1` honours the lock
  as before, anything else or absent answers and stores it but keeps the
  port up (`OMCI_UNI_LOCK=0` keeps working as it did). Every lock and unlock
  is logged once per change as `event=uni_lock action=ignored|applied|
  unlocked|cleared`; with `1`, the `applied` line is a warning sent before
  the port goes down. A locked-out stick comes back with a boot with the
  fibre unplugged (it starts unlocked, and no OLT can lock it again before
  the setting is turned off); a safe-mode boot after short boots now also
  removes `OMCI_UNI_LOCK=1` (`cleared=OMCI_UNI_LOCK`), so five short boots
  work too, without touching the fibre. docs/SETTINGS.md, docs/TOOLS.md and docs/SWITCH.md
  describe it; the drv golden covers both settings and the event order, and
  test/safe_mode_test.sh the clearing. Refs #287.
- CI: actions/cache 6.1.0, actions/attest-build-provenance 4.2.2 and
  actions/attest-sbom 4.1.0 (Dependabot).

## v1.15.0 (2026-11-02)

- Releases: confd v1.13.0. System, Backup & reset can encrypt the
  configuration backup with a password, in the browser; the file opens
  without the stick with `openssl enc -d -aes-256-cbc -pbkdf2 -iter 200000
  -md sha256`, and restore takes both encrypted and plain files. Config,
  Network shows `SFP_HOST_DDM` (whether the router can read the module
  identity and diagnostics from the stick) and applies it at once, without a
  reboot.

- Releases: every release now also carries `<version>-sources.tar`, the
  complete corresponding source of the image for GPL and LGPL compliance: the
  exact upstream tarballs the build verified (Linux kernel and signature,
  busybox, dropbear, iproute2), uClibc-ng with the odi-toolchain tag (build
  tools gcc, binutils and the Linux headers are named, not included), `confd` and `metricsd` at their
  tags, and this repository at the release commit, with a `SOURCES.md`
  manifest (component, version, license, URL, sha256), rebuild and uClibc-ng
  relink steps and a written offer. Built by `tools/bundle-sources.sh` in the
  release job, listed in `SHA256SUMS` and attested; the job fails if the
  bundle is missing, empty or altered. See `docs/LICENSING.md`. Refs #275.

- omcid checks every received frame before it reads or changes anything. A
  frame under 48 bytes, with a device identifier other than 0x0a (an
  extended-format 0x0b frame included, so the OLT falls back to baseline) or
  with a CRC-32 that does not match bytes 0-43 is dropped with no answer. A
  trailer other than `00 00 00 28` (what our OLT sends) under a valid CRC is
  answered, and only counted. Each reason is counted (`omcli state`, `rx
  dropped`) and logged as `event=rx_reject reason=length|devid|crc|trailer
  action=dropped|accepted`, at most one line per reason a minute. The same
  check covers injected frames: `omcli --inject` now fills in the trailer and
  CRC, and `--inject-raw` sends bytes as given. omcid implements the baseline
  message set only and reports it (OMCC version `0x80`); docs/SETTINGS.md now
  says that an `OMCC_VER` copied from an ONU that advertises the extended set
  gets its extended messages dropped. Frames captured from an OLT carry valid
  CRCs; on a stick, 75 messages from an OLT were answered with none dropped.
  Refs #251.

- docs: `docs/TOOLS.md` points from the syslog and exporter sections to the
  Prometheus alert rules and their notes in the odi-sfp-exporter repository.
  Refs #270.
- docs: `SECURITY.md` states that only the latest release is supported, that
  fixes ship as a new release, and that vulnerabilities are reported privately
  through GitHub private vulnerability reporting. Linked from the README.
  Refs #272.
- release: the SBOM now records `GPL-2.0-or-later` for `confd` and `metricsd`
  instead of `NOASSERTION`, and `docs/LICENSING.md` states it, from the README
  of odi-ui and odi-sfp-exporter. Refs #276.
- build: `make releases` checks `confd`, `confd-assets.tar.gz` and `metricsd`
  against SHA-256 values committed in `src/release-sha256.txt`, on top of the
  release's own `SHA256SUMS`, so a release that replaced an asset and its
  checksum file together is refused. A tag with no committed hash stops the
  build (`ALLOW_UNPINNED_RELEASES=1` makes it a warning, for trials).
  `tools/bump-deps.sh` rewrites the hashes whenever it moves a tag, and stops
  without touching any pin if an asset does not match its release checksums.
  The assets tarball is listed before extraction and refused if it holds a
  link, device, absolute path or `..` component. `test/fetch_releases_test.sh`
  covers replaced checksums and hostile archives. The CI and release download
  caches are keyed on `src/release-sha256.txt` too, so a corrected hash is
  never checked against assets restored from an older cache. Refs #256.
- docs: stale statements corrected. The kernel is 6.18.58 everywhere it is
  named (README, `docs/BUILDING.md`, `docs/HACKING.md`, `docs/KERNEL.md`), and
  the README no longer states the current release. `image/README.md` said the
  root hash is MD5 crypt and that rcS waits for an ARP reply by default; the
  build writes SHA-512 crypt (`image/build.sh`) and the ARP wait is the
  development-only `confirm-arp` flag (`rootfs/skeleton/etc/init.d/rcS.dev`),
  the default being the 120 s userland deadline plus the 300 s trial guard.
  `make lint` now runs `tools/check-doc-pins.sh`, which fails when README.md or
  `docs/BUILDING.md` names a kernel other than the `VERSION=` pin in
  `kernel/618/fetch.sh`, or a confd/metricsd tag other than the one in
  `src/fetch-releases.sh`. Refs #273.
- CI: every action in the workflows is pinned to a full commit SHA with its
  version in a trailing comment, `make lint` rejects a mutable reference, and
  `.github/dependabot.yml` proposes weekly updates for the `github-actions`
  ecosystem (`dependency-bump.yml` only handles the firmware pins, so it is not
  extended). Refs #257.
- CI: `release.yml` is read-only at workflow level; `contents: write`,
  `id-token: write` and `attestations: write` are granted only to the `release`
  job, not to the test job that runs repository code. Checkouts that do not push
  use `persist-credentials: false`. `dependency-bump.yml` keeps the write scopes it needs
  (it pushes a branch and opens a PR), now on its job only. Refs #255.
- CI: every job in `ci.yml` and `release.yml` now has `timeout-minutes`, sized
  from the longest successful run seen plus headroom (a hung test used to hold a
  runner for GitHub's six-hour default). Refs #266.

- igmpd: the IGMP and MLD parsers now read the same VLAN tags the kernel hook
  admits, up to two, each 802.1Q or 802.1ad, instead of one 802.1Q tag only.
  A membership report behind an S-tag or a QinQ pair used to reach igmpd and
  be classified as not IP, so no membership row was installed (and, with the
  PON drop on, the group was dropped). A report is keyed by the VID of its
  outermost tag. Refs #250.
- kernel: enabling IGMP and MLD snooping in one call no longer leaves the
  IGMP rule behind when the MLD rules fail to install: the call now puts the
  switch back to what it held before it, so a failed ioctl means nothing
  changed and igmpd can retry. Refs #249.
- omcid, kernel: a bridge-connection, upstream flow or priority-queue delete
  that the driver refuses is now logged with its result (`[hw] ... -> rc`),
  and a failed clear of a snooping ACL entry is a rate-limited kernel warning
  (`odi_switch_mcast: clearing table ... failed`), instead of both being
  dropped silently. Nothing is retried. Refs #248.
- omcid: a snapshot write that fails (full tmpfs, a failed write or rename) is
  now logged as `event=snapshot_fail rows=N` instead of being dropped
  silently; the previous snapshot stays in place. Refs #247.
- README: the DFP-34X-2C3 moves from "never tried" to reported working, on
  one user's report of v1.14.0 on a 2C3 with stock `V1.1.4-250620`.

## v1.14.0 (2026-10-31)

- Releases: confd v1.12.0. The browser's password dialog is replaced by a
  sign-in page: opening the stick goes to `/login`, and signing in there gives
  the browser a session that ends after 30 minutes without use, after a day,
  when the stick or confd restarts, or at sign-out. Sign out now ends the
  session for certain and says so, instead of answering *cross-site request
  refused* in Chromium browsers or opening the browser's own password dialog.
  Changing the password keeps the browser that changed it signed in and signs
  the others out. `curl -u` and scripts keep using HTTP Basic. A reboot from
  the web page now shows a progress bar until the stick answers again (about a
  minute), then opens the sign-in page. Refs #239.
- kernel, rootfs: the host-side SFP EEPROM, odi-only key `SFP_HOST_DDM`, on by
  default. On the board revisions that wire the SoC's SFP EEPROM emulation to
  the SFP edge (HSGQ v08 / DFP-34X-2C2 v08) the router now reads the module's
  identity (A0h) and live diagnostics (A2h) from the stick, as with the stock
  firmware, instead of 0 C and 0 V. The kernel copies A0h and A2h from the
  module into the slave's 512-byte SRAM, reads it all back, makes it read-only
  to the host except A2h 128-247, and only then turns the slave on (0x2ac bit
  0, then 0x23018 bit 23, then SoC pin 27 out of plain GPIO mode, PIN_GPIO_SELECT
  0x08082001 -> 0x00082001 as the stock v08 image runs it; each a
  read-modify-write of that bit, undone pin first); A2h 96-119
  (the five diagnostics, the TX_DISABLE/TX_FAULT/RX_LOS bits of byte 110, the
  alarm and warning flags) is refreshed every second, and the module is never
  written. `odi_switch.sfp_mirror` and `sfp_mirror_state`, `sfp-host-ddm.sh`
  (a PON step after `laser`), `apply.sh sfp`; off in a safe-mode boot; `0`
  turns it off. No effect on v06 boards, where the host reads a separate fixed
  EEPROM; pin 27 is unused there as measured. The behaviour follows
  measurements by Thaad-QR on a stock v08 stick, and pin 27 Thaad-QR's
  comparison with a stock v08 stick (with it, the router showed live DDM). Host-tested (`test/odi_sfp_mirror_test.c`,
  `test/sfp_host_ddm_test.sh`). On hardware: one reporter's router reads live
  DDM from this image at boot, another's v08 showed it once pin 27 was freed
  by hand, and a v06 stick ran the mirror for three days with the host's
  EEPROM unchanged. Refs #231.
- Host SerDes mode: the new odi-only key `HOST_SERDES` (default `fiber1g`,
  the mode and the thirteen-write sequence the image always ran, value for
  value) selects `2500basex`, for a 2.5G host and the HSGQ XPON stick, or
  `hisgmii` (the stock "HiSGMII" mode, the MAC at 1000 Mb/s). In both the
  host port is forced up (the new kernel parameter `odi_switch.host_forced`;
  cmd 32 then moves the forced link value, so an OLT unlock keeps the
  link; back in `fiber1g` both port words return to what the force found),
  and the PCS auto-negotiation is set per mode (off in 2500BASE-X,
  the only setting seen to link to the 2.5G router port measured; why the
  stock HSGQ firmware links there with it on is not known, and another open
  implementation's measurements linked 2500BASE-X with it on and off
  against a matching host, so it is what that router needs, not a property
  of the lane). The SFP EEPROM still advertises 1000BASE-LX at 1300 MBd,
  so a host that picks its port mode from it (Linux phylink, for example)
  may run 1000BASE-X whatever the key says: a caution, not measured.
  `host-serdes.sh` applies it from `network.sh` at boot and offers `try`, `confirm`, `revert`,
  `reapply` and `status`: a change is a trial that reverts after 120 s
  unless confirmed over the link. Fiber 1G, the most compatible host mode,
  is the fallback everywhere. A 2500BASE-X boot watches the SerDes sync
  (bit 17 of 0x214) and falls back to Fiber 1G, alternating, for up to 10
  minutes, staying in Fiber 1G when neither links (the key unchanged);
  HiSGMII has no link signal and no boot fallback. `HOST_AUTONEG` applies
  to Fiber 1G only and stands aside while another mode runs; its `status`
  then prints the bit the port really runs and `owner=host-serdes`. A mode other
  than `fiber1g` makes three short boots enough for the safe mode, whose
  boot runs Fiber 1G and clears the key, as it clears `HOST_AUTONEG=off`
  (`cleared=` can now name both). Whether the HSGQ board links in Fiber 1G
  is untested. The stock `LAN_SDS_MODE` is not read. The diagnostics bundle carries
  `host-serdes.sh status`. Register values and the HSGQ port by Thaad-QR
  (their reads from a stock HSGQ stick). Host-tested
  (`test/host_serdes_test.sh`, `test/odi_host_forced_test.c`,
  `test/safe_mode_test.sh`). On hardware: 2500BASE-X linked an HSGQ stick to a
  2.5G router port (O5, PPPoE, voice), and Fiber 1G runs as before on a
  DFP-34X-2C2; HiSGMII has not been run on hardware yet. Refs #229.
- omcid tests: the qemu suites no longer flake on a loaded machine. `omcli --inject-file` waits out a full omcid queue (30 s) instead of dropping the tail of the session after half a second, the suites wait for omcid's end-of-session lines (rebuild, multicast policy export, answers) instead of fixed sleeps, and omcid logs `multicast policy unchanged` when its quiet-second export has nothing to write.

## v1.13.0 (2026-10-28)

Not in this release: the host link modes (`HOST_SERDES`, 2500BASE-X and HiSGMII) and the
host-side SFP diagnostics (`SFP_HOST_DDM`) from the v1.13.0 pre-releases are still being
tested and follow in a later release. A stick that needs `HOST_SERDES` (for example an
HSGQ stick at 2.5G) should stay on v1.13.0-beta.2 until then.

- Releases: confd v1.10.0 and metricsd v1.8.0. The web UI no longer shows the
  identity keys as empty on a stick whose config store outgrew its buffer (a
  stick that ran a router firmware before), offers a setup wizard for a stick
  nobody has set up (web password, an ssh key, the ISP login, the VLAN
  handling), says when a newer odi-oss release exists (the browser asks
  GitHub at most once a day; System > Firmware turns it off), shows the SoC
  chip temperature with its alarms, and orders the header bars by severity.
  metricsd exports the chip temperature limits and alarms. Refs #229.
- kernel: die-temperature alarms on the SoC hwmon device (`rtl9602c_soc`):
  `temp1_max` (default 95000) and `temp1_crit` (default 110000) in
  millidegrees, root-writable (0..150000, max <= crit) and not persisted, plus
  read-only `temp1_max_alarm` / `temp1_crit_alarm`. An alarm sets at reading
  >= limit and clears 2 C below it; a 10 s delayed work (one register read)
  logs each crossing once (`pr_warn` max, `pr_crit` crit, `pr_info` on clear)
  and notifies sysfs pollers. Nothing powers off or throttles: the scale is
  not verified. The 95/110 defaults follow another open implementation's
  measurements for the same chip. Host-tested (`test/odi_hwmon_test.c`).
- omcid tests: new suite `getall-test.sh` (`make test-omci-getall`) sends a Get
  of every attribute to each class of the model that has a table or other big
  attribute (voice service profile, multicast operations profile, multicast
  subscriber config and the rest) and to each class whose plain attributes do
  not fit in 25 bytes, and checks the answer against G.988: it stays in the 32
  bytes of the baseline message, a table answers alone with its 4-byte size, and
  what does not fit is named in the failed and unsupported masks. This was
  prompted by another open implementation's report of a buffer overflow on
  such a Get; omcid has none (no code change), and the suite keeps it so.
- diag-bundle: `host-link.txt` reads the host SerDes pages (the 1.25G and
  3.125G pages, RX enable, the analog words, the host port force, ability and
  select words), read-only, and adds `host-serdes.sh status` when that script
  is present, so a host-link report shows where the lane stands. Refs #229, Refs #231.
- test: `count_proof_test.sh` no longer flakes on "no PASS for the MAC order": its stub diag and
  stub sleep run on a fake clock the sleep advances, so the 1 s UNI windows have an exact rate
  ratio (the band and `count-proof.sh` are unchanged).
- omcid: a bridge port of TP type 3 whose pointer names both an 802.1p mapper
  and a GEM interworking TP (entity ids are unique per class only) is read as
  the mapper when that mapper sends any priority to another TP, so every GEM
  port of the mapper gets its bridge connection. Before, only the priority-0
  GEM port did, and on ISP3 the DHCP offers (priority 5) and ARP replies
  (priority 7) were dropped: O5 and no lease. Pinned by `vlan-test.sh`.

## v1.12.0 - 2026-10-26

- Releases: confd v1.9.0: the Phone tab shows what the OLT provisioned for
  voice and the line state a router reports (`OMCI_VOIP_LINE_REPORT`), the
  Config page has every odi-only key (`IGMP_PON_DROP`, `MLD_SNOOPING`, the
  phone-port and Dying_Gasp keys), and the MIB browser refuses the classes
  that carry provisioned secrets. metricsd stays at v1.7.0.

- igmpd: MLD (IPv6) snooping, opt-in with the new key `MLD_SNOOPING=1`
  (needs `IGMP_SNOOPING=1`; `apply.sh igmp` applies it). The switch copies the
  host's MLD queries, reports and dones to igmpd as it copies its IGMP, or
  takes them when the multicast profile asks for an upstream tag, a rate or
  the discard of joins outside its lists, and no other ICMPv6: neighbour and
  router discovery are never touched. IPv6 groups get a row each (their 33:33
  MAC, the host and the PON as members), the profile's IPv6 access control
  list rows and group limit are enforced, refusals land in a ring shown by
  `igmpd -s`, and the reports go up tagged as for IGMP. With
  `IGMP_PON_DROP=1` as well, IPv6 multicast from the PON that no group row
  names is dropped too, after the learning interval; the link-scope groups
  (ff02::/16) and the solicited-node groups always flood both ways and are
  never snooped or refused, so router advertisements, neighbour discovery and
  DHCPv6 keep working. omcid now writes the IPv6 rows of the access control
  lists (row parts 0 and 2) to the multicast policy file as `allow6=` and
  `static6=` lines; with the key unset nothing changes: igmpd runs with the
  same command line and the switch is programmed as before. Run on a stick on
  a line without IPTV: the rows are written, the drop holds, and router and
  neighbour discovery keep working through it; not yet run against a real
  IPv6 multicast stream.
- igmpd (IGMP snooping, still opt-in and experimental): each joined group is
  now metered with a counter in the switch, so the OLT can see which groups
  stopped arriving and how much multicast the subscriber receives. A group
  whose stream came and then stopped for 10 seconds is listed in the multicast
  profile's lost groups (ME 309 attribute 9) and raises its alarm 0, cleared
  as soon as the stream comes back; the subscriber monitor (ME 311) reports the
  bandwidth, and for each active group its VLAN, bandwidth, client address and
  time since the join. A group whose counter has never counted anything is
  reported as unknown, never lost. That the switch counts what enters from the
  fibre was measured on a stick with `tools/mcast-probe/count-proof.sh` (its
  counters matched the port's own count, frame for frame), on a line without
  IPTV, so no joined group's stream has been counted yet. The maximum
  multicast bandwidth of ME 310 is now
  honoured: a new join past it is counted (ME 311 attribute 4) and, when the
  OLT enforces it, refused; a group already playing is never cut. Lines that
  do not turn snooping on, and profiles without a maximum, see no change.
  `diag mc-count` installs the same counters by hand.
- omcid: new opt-in key `OMCI_VOIP_LINE_REPORT=1` (default off): the router
  behind the stick can report whether its SIP client is registered, and the
  VoIP line status of a line the OLT provisioned then reads that (registered,
  in session, or a registration or invite failure) instead of "config done". The report
  comes from odi-ui (`POST /api/voip/status`), carries only the port and a
  state word, and counts only while fresh (300 s); without a fresh report, or
  with the key off, the line status is exactly what it was. `omcicli voip`
  shows the new states. The two voice gauges for the exporter are not part of
  this change.

**Automated dependency bump.**
- linux 6.18.57 -> 6.18.58
- iproute2 7.2.0 -> 7.3.0


- igmpd, omcid: the access control lists, the IGMP version and the preview
  rules of the OLT's multicast profile (ME 309, ME 310) are now honoured, all
  of it only while `IGMP_SNOOPING` is `1`. A list row that names a VLAN is
  held to the joins on that VLAN, so ranges of different VLANs stay apart (when
  the join is on a VLAN some row names; any other join is judged by the
  ranges as before), and a row that names a source address (SSM) is held to
  IGMPv3 records that name sources, while a v2 report goes in as before. A
  group of a row with a preview length is allowed for that many seconds, then
  removed, refused until the repeat time has passed and for good once the
  repeat count is spent; a group of the allowed preview table of ME 310 is full
  access for its minutes. A host message above the profile's IGMP version (a v3
  report under version 2) is handled as before, not dropped (a set-top box
  does not fall back to an older version when it sees no query), and counted
  as `above_version` in the state file and `igmpd -s`. The GEM port and imputed bandwidth of
  each row are carried for the bandwidth limit, not enforced. Rows that say
  nothing about any of these, and profiles with no version, behave as before.
  A Set or Create of ME 309 now refuses (result 9) a value G.988 does not
  define for the IGMP version, function, immediate leave, upstream tag control
  or unauthorized join behaviour; function 1 and 2 are still accepted and the
  multicast stays transparent. The state file and `igmpd -s` count refusals by
  `preview`; `igmpd -r` has a `v3src` event for records that name
  sources.
- omcid: a line whose OLT asks for transparent multicast now gets the VID-0
  multicast tag passed through, as the stock firmware does; no change on lines
  that ask for strip. In the manual VLAN mode the multicast row for
  priority-tagged groups always removed the tag; it now follows the multicast
  profile's downstream tag control (ME 309 attribute 16): type 0 keeps the tag
  as received, type 1 and `OMCI_CUSTOM_MCAST` bit 1 strip it. Types 2 to 7 keep
  the strip, since a VID 0 row cannot be the VLAN they write a tag on.
- diag bundle: the phone lines an OLT provisioned are in the bundle, as
  `pon/omcli_voip.txt`: the SIP servers, codecs, voice VLAN and line status,
  with the phone number, display name and login masked like the serial and no
  password at all. Every SIP credential (password, login, number, display
  name, and a login inside a server URL) is also added to the list of
  secrets the bundle scrubs from every file, as text and as hex, so none of
  them can reach a bug report through a log line either.
- CI: a slow package mirror no longer fails test-qemu. `tools/ci-apt.sh` cut
  each install of qemu-system-mips off after 5 minutes, so a mirror that was
  slow but still sending failed all three attempts. An install may now take
  15 minutes (two attempts; a stalled connection is still caught by apt's
  30-second read timeout and its own retries), and the test-qemu job may run
  45 minutes instead of 20 to fit them.
- igmpd: the upstream IGMP tag, rate and unauthorized-join behaviour the
  OLT's multicast profile asks for (ME 309 attributes 4 to 6 and 15) are now
  applied. When the profile asks for a tag (add, replace the whole tag, or
  replace only the VLAN), a rate, or the discard of joins outside its access
  lists, the switch hands the host's IGMP to igmpd instead of forwarding it,
  and igmpd sends each message up itself: tagged as asked, on the flow that
  carries that VLAN up, no more than the rate allows, and a join for a group
  the lists refuse is kept from the OLT unless the profile says to forward
  it. A query from the host side is not sent. With tag control 0 a message
  goes up exactly as the switch would have tagged it. A profile that asks for
  none of this changes nothing: the host's IGMP is forwarded by the switch as
  before. If igmpd stops or dies, the switch forwards the host's IGMP by
  itself again. The kernel lets igmpd send only IGMP to a multicast address,
  only on an upstream flow a bridge rule uses, and only while it holds the
  snooping lease; nothing from the fibre reaches it. `igmpd -s` shows how
  the reports go up and counts them; `/proc/odi_omci` counts what was sent.
  Still behind `IGMP_SNOOPING=1`, and experimental: no OLT with IPTV has yet
  seen a re-sent report.
- omcid: the downstream multicast tag control of the OLT's multicast profile
  (ME 309 attribute 16) is applied. Strip the tag, replace it, replace its
  VID only, or add one (an S-tag in front of the C-tag, the only tag the
  switch can add to a tagged frame) now acts on the multicast VLANs the
  profile's access control lists name, one switch rule each, in front of the
  rules the line had. The switch tells multicast from unicast by the VLAN
  alone, so nothing is done for untagged or "any VLAN" groups, for a VLAN a
  unicast service of the line also uses, or when no VLAN is named; omcid
  logs once what it left out and why. Types 5 to 7 (the VID of the service
  package table) are not done, and say so. `OMCI_CUSTOM_MCAST` bit 1 now
  strips whatever the profile says, as on the stock firmware, and a change
  of it rebuilds the rules (`apply.sh omci`). Neither of our lines names a
  multicast VLAN, so their rules are unchanged, byte for byte.

- omcid: `omcicli voip` prints the phone lines the OLT provisioned, resolved
  into one block per port: the SIP user, the SIP servers as URLs without any
  `user:password@`, the codecs, the voice VLAN and priority, and the line
  status. The phone number, the display name and the login name are masked
  like the serial in the diag bundle, and the password is never printed;
  `omcicli voip --secrets` shows them, and `--secrets-list` prints only the
  secret strings, one per line, for a scrub list. A login that belongs to the
  TR-069 server is not shown as a SIP credential. With no voice provisioned
  it says so in one line.

- omcid: with `OMCI_POTS_PORTS` on, the voice an OLT provisions now reaches
  the router. The OLT carries it on an IP host; the bridge port it builds on
  that IP host is now the stick's host port, toward the router, instead of
  the stick's own CPU port, where the voice went nowhere. The voice IP host
  is the one the OLT's SIP agent names; any other IP host (TR-069
  management, say) stays as it was and its VLAN never reaches the router. The voice VLAN
  reaches the router tagged, as the OLT sets it, and the router's SIP client
  sends it tagged the same way. A rule that would tag the IP host's untagged
  frames is matched on that VLAN instead, so it cannot take the data
  service's untagged frames; a rule that would match every untagged or every
  tagged frame is not installed, and the log says why. The stick itself is
  still unreachable from the fibre. With the key off nothing changes: a
  bridge port on any IP host gets no rule, as before. Experimental: no OLT
  that offers voice has been tried yet.
- omcid: `OMCI_POTS_PORTS=1` or `2` now tells the OLT of that many phone
  ports, so an OLT that offers voice can provision a SIP line on each for the
  router behind the stick to register: a POTS circuit pack in the first free
  slot (2 unless the VEIP is there; the new `OMCI_POTS_SLOT_ID` picks
  another), a POTS UNI per port with its line status, and the VoIP config
  data, which offers SIP over OMCI only. The stick has no phone socket and
  says so: the line status reads "config done" once the OLT has provisioned a
  line, never "registered"; a line test is refused; no voice alarm is raised.
  Off (the default),
  the MIB upload is the stock one, record for record. Experimental: not yet
  run against an OLT that offers voice. Changing either key re-registers the
  ONU.

- tools: `make lint` no longer passes silently when git is unusable. Its
  git grep rules read a failing git grep as "no match", so a build from a
  git worktree through `tools/remote-build.sh` (whose `.git` file points at
  a path that does not exist on the build host) skipped them all. `make
  lint` now stops with a message when the tree is not a usable git
  checkout, and `tools/remote-build.sh` gives a worktree a throwaway
  repository on the build host so the rules really run there.

- omcid: the VoIP managed entities of G.988 (SIP over OMCI: the SIP agent and
  user, the voice service, media and RTP profiles, the dial plan, the
  application profile and feature codes, the voice CTP, the voice PM
  entities, and the POTS UNI, line status and VoIP config data the ONU
  creates) are now in its model, behind a new experimental key,
  `OMCI_POTS_PORTS` (odi.conf, 0 by default). Off, an OLT that sends one of
  these gets "unknown entity", exactly as before. On (1 or 2), omcid accepts
  what an OLT provisions for a phone line, keeps it, answers it back and
  resumes it after a restart; the SIP password and the subscriber's display
  name stay out of the log and are masked in `omcicli mib get`. The stick does
  not tell the OLT of any phone port yet, so on its own the key changes
  nothing an OLT sees. Changing it re-registers the ONU. To make room for a
  data service and two phone lines at once, omcid now holds 320 managed
  entities (was 256), 32 entities wider than a row (was 16) and 255 table
  rows (was 128); its MIB snapshot reuses one buffer for saving and
  resuming, so its RAM still shrinks by about 37 KB overall.
- kernel, omcid: a downstream GEM flow can now be deleted. A multicast GEM
  port withdrawn by the OLT used to keep its flow, and the GEM port stayed
  received, until the next reboot. Now its flow is taken out of the switch
  when its last address row goes, when its multicast ME goes, and now also
  when the GEM port network CTP that ME points at is deleted. A MIB reset
  deletes every downstream flow too, as it already did the upstream ones,
  so a GEM port the OLT does not provision again after the reset is no
  longer received. The driver clears the row and reads it back before it
  frees the flow id; a row that does not clear is logged and kept, and
  omcid tries again. `/proc/odi_gpon/cam` now also lists the downstream GEM
  port rows (`gem_cam`), as the hardware holds them.

- igmpd: the PON-side drop of unjoined multicast is now its own opt-in key,
  `IGMP_PON_DROP`, instead of coming with `IGMP_SNOOPING=1`. Snooping can run
  (the copy of the host's IGMP, the group rows, the OLT's limits and lists)
  with nothing dropped, which is the part that can cut a stream. Only
  `1` drops; unset is off. A config with `IGMP_SNOOPING=1` that wants the
  drop as before must now set `IGMP_PON_DROP=1` (and run `apply.sh igmp`);
  without it, unjoined multicast is simply no longer dropped on the PON side.
  Snooping is still experimental.
  `igmpd` gets `-d` for it; without `-d` it never asks the switch to drop.

- diag and the switch driver: `diag acl-probe` installs ACL rules by hand to
  measure the multicast path before anything is built on it. A rule counts
  the IPv4 frames of one protocol, or to one address prefix, entering the
  UNI or the PON, in frames or in bytes, without changing where they go; or
  it copies or takes the IGMP entering the UNI to the CPU, which counts what
  arrives. Nothing else can be sent to the CPU, and nothing from the PON.
  Nothing in the image installs a probe; they belong to the diag process that
  installed them and go when it exits, and they and IGMP snooping exclude
  each other. `tools/mcast-probe/mcast-probe.sh` runs the measurements on a
  stick in one unattended run (about 15 minutes), puts everything back
  however it ends, and prints a results block for `docs/SWITCH.md`, where
  the questions it answers are listed, with what a first run on ISP1
  settled: the trap and the copy of the host IGMP work, the counters count
  frames and bytes (the FCS included), and nothing from the PON reached the
  CPU. `diag help` now keeps a space between a long command and its
  description.
- tests: the OMCI suites no longer fail now and then on a busy machine. A
  suite that restarts the daemon into the same log file waited for the
  "answering as" line, and the shell had not always emptied the file yet, so it
  read the previous daemon's line, went on before the new daemon was up, and
  then lost its first frames or stopped it too early. Every start now empties
  the log first. A failed `omcli --inject` in a suite also prints why.
- igmpd: `igmpd -s` says why a channel does not play. It prints the multicast
  policy in force (timers, group limit, the allowed ranges), the groups the
  stick holds and the last eight joins it refused, newest first, with the
  group, the VLAN, the reason (outside the OLT's access control lists, past
  the group limit, or no free table entry) and how long ago; a set-top box
  repeating the same refused report counts once with a repeat count. The data
  comes from the state file igmpd already writes, which gains refusal counters
  and the ring after the group lines; omcid reads it as before and an older
  omcid skips what it does not know. Nothing an OLT sees changes. See
  docs/TOOLS.md.
- omcid: the extra multicast GEM ports an OLT names only in address rows now
  get a downstream flow. G.988 lets one multicast GEM interworking TP (ME 281)
  spread the channels over several GEM ports by writing more rows, with other
  GEM port ids, into its IPv4 or IPv6 address table (or into the multicast
  access control rows of ME 309), without creating a GEM port network CTP
  for them. Only the CTP's own port had a flow, so the others dropped their
  frames. Each such port now gets a multicast downstream flow like the CTP's,
  and loses it when its last row, or the ME, goes; a line whose OLT names one
  port is unchanged.
- omci: a Test on an entity omcid cannot test (anything but the ANI-G, the
  IP host included) is now refused with result 2, "not supported", instead of
  being acknowledged. Before, the OLT got a success and then waited for a Test
  result that never came. The ANI-G test, the one OLTs actually send, is
  unchanged.
- omcid: the LOID and its password are masked in MIB dumps like the other
  credentials, where they appear twice: the LOID authentication entity (class
  65530, its LOID and password) and ONU-G (class 256, its logical ONU id and
  logical password). They show as `**`; the other attributes of both stay
  readable, the OLT's Sets of ONU-G stay in the log, and `--secrets` shows
  the values. Both come from the config store, and `omcicli mib get` printed
  them.
- igmpd: three fixes to the group table. The ME 311 join counter counted
  every join message, refused ones included; it now counts only the joins
  that were accepted (refused ones stay in the denied count of the state
  file). The group limit counted a group once per VLAN, so one group on two
  VLANs used two of the limit; it counts groups now. A group is no longer
  dropped from the table, and its stream cut off while the PON drop is on,
  when the OLT queries less often than the profile says: igmpd measures the
  interval between a host's own reports and holds the group for robustness
  times that plus the response time, never less than the profile's figure and
  at most 1800 s. A leave without immediate leave still shortens the port to
  the last-member time, because igmpd cannot send a group-specific query yet;
  `docs/TOOLS.md` says what that relies on. The golden transcripts of the
  refused-join and group-limit cases change only in their joins line, and so does the ME 311 check fed from igmpd's own state file (the refused join is no longer counted).
- CI: the OMCI suites `tables`, `alarms` and `writes` run in CI too; they were
  left out of the job matrix and ran only under `make test`. `make lint` now
  fails when that matrix and `OMCI_SUITES` in the Makefile name different
  suites. The `tables` suite waits for the downstream flow line before it
  counts the multicast flag, instead of reading the log the moment the frame
  is answered, which failed now and then.

- igmpd: `igmpd -r FILE` replays a transcript of timestamped events (IGMP
  frames with their port and VLAN, clock ticks, policy file changes) through
  the daemon's own decision code and prints what it would do: the rows it
  writes and removes, the lease, the router ports and the state file. It opens
  neither the switch nor the redirect device, so IPTV behaviour can be tested
  with no OLT and no stick. The tests gain golden transcripts (join and leave,
  two ports on one group, a refused join, the group limit, expiry, frames,
  a policy change) and a contract test that feeds the policy file omcid wrote
  to igmpd, and igmpd's state file back to omcid's ME 311. The daemon itself
  behaves as before.
- omcid: the multicast access control list a profile exports to igmpd now
  honours the row parts of G.988. A part 1 or part 2 row (IPv6 source or
  destination prefix, preview fields) was read as an IPv4 range of its own,
  and a part 0 row of an IPv6 range was listed as a tiny IPv4 one. Only part 0
  rows make a range now, a row an IPv6 prefix belongs to is left out, and a
  list of IPv6 rows alone enforces nothing instead of refusing every IPv4
  join. Also: the multicast service package table (ME 310, attribute 6),
  which G.988 uses to pick the profile by VLAN, is not supported yet; it is
  documented in docs/TOOLS.md and logged once when an OLT provisions it.
- omcid: the entities wider than a 96-byte row are now stored whole. A large
  string kept only its number of parts and the first three parts, so any
  address over 75 characters (a SIP proxy, an ACS URL) came back cut, and the
  authentication method dropped its second username. Rows of the classes
  whose attributes add up to more than 96 bytes (large strings, the
  authentication method, IP host config data, the extended multicast profile)
  now take a slot of a small pool of 16 full-width rows, about 5 KB in all.
  If the pool is ever full the row behaves as before (cut, and marked
  truncated) and omcid logs it once. A Get, a MIB upload and `omcicli mib get`
  show the whole row, and the resume snapshot carries it (format 5: a
  snapshot written by an older omcid is not used, and the ONU registers
  again as it does after any format change).
- omcid: the credentials an OLT provisions are no longer printed by default.
  `omcli mib` / `omcicli mib get` (and `mib getattr`) show the username,
  password and realm of an authentication security method (class 148) and
  the parts of a large string (157, which can hold a URL with a password in
  it) or an octet string (307) as `**`, byte for byte the same width, so
  scripts reading the dump keep working; `--secrets` prints the real bytes.
  The log already withheld the contents of Creates and Sets of 148 and 157;
  it now does the same for 307, from the same single list. The diag bundle
  never asks for the real bytes. The web UI's OMCI browser passes this dump
  through, so it shows the mask too.
- omcid: a Get whose attributes do not all fit the answer's 25 bytes of
  values now answers result 9 (attribute failed or unknown), with G.988's
  optional-attribute and attribute execution masks, instead of success with
  the attributes left out. The OLT is told which ones to ask for again. Seen
  on one ISP: GEM port PM history data (class 267) polled for attributes 1
  and 3 to 7 is 33 bytes, and transmitted blocks was never sent. An attribute
  the class does not have is reported in the optional-attribute mask the same
  way. Attribute values also stop at byte 27 of the answer, where the two
  masks begin; before, they could run up to byte 31.

- CI: package installs on the runner are bounded (`tools/ci-apt.sh`): a read
  timeout and retries per connection, a time limit per `apt-get` call, and
  three attempts. A stalled apt mirror connection used
  to hold `test-qemu` for 11 minutes, or past its 20-minute timeout, in a
  step that normally takes 25 s.

- Build: one download cache for several build trees. `DL` (an absolute path)
  now moves it for the package tarballs and the confd/metricsd release assets
  too, not only the kernel tarball, and `tools/remote-build.sh` points it at
  `ODI_REMOTE_DL`, by default a `dl/` beside `ODI_REMOTE_DIR`. A build tree
  copied for one remote build and deleted after it used to download the
  kernel tarball again every time. Every cached file is still checked against
  its pinned SHA-256 on use, and a download is written under a per-process
  name first, so two builds sharing the cache do not write the same file.

- Build: `kernel/618/fetch.sh` stops the `gpg-agent` and `dirmngr` that the
  signature check starts in its throwaway `GNUPGHOME`, and removes that
  directory on exit. Each run used to leave one agent and its keyring in
  `/tmp`; enough of them use up the build host's inotify instances for root,
  and `systemctl` then fails with `Failed to allocate directory watch: Too
  many open files`.
- omcid: no `-Warray-bounds` warning in `ds_flow_send()` (`apply_qos.c`).
  The downstream flow's multicast flag is now written under the same
  `FLOW_MAX` check as its queue key. The index could not reach 64 at run time
  (the flow table walks `caps_flows()`, which is capped at `FLOW_MAX`), but
  the compiler cannot see that across files. No behaviour change.

## v1.11.1 - 2026-10-21

- OMCI: new odi-only key `OMCI_TCONT_ALLOC_UNSET` (`/etc/config/odi.conf`,
  default `ploam`, which changes nothing). `unassigned` makes a T-CONT the
  OLT never Set report Alloc-ID 0x00FF, the initial value of G.988 9.2.2,
  instead of the next Alloc-ID assigned by PLOAM, in a Get, in the MIB
  upload and in `omcicli`. For an OLT that picks the first T-CONT that
  reports empty and so skipped the first one. A T-CONT the OLT Set reports
  its value in both modes, and the datapath is unchanged: the T-CONT is
  still bound to the PLOAM Alloc-ID, so upstream keeps working on an OLT
  that never Sets it (omcid logs a warning for each such T-CONT). Read at
  every Get, so a change is live. `docs/SETTINGS.md`.
  On a stick whose OLT sets the T-CONT Alloc-IDs itself, a re-registration
  with the key at unassigned kept its service (O5, all services, traffic) and
  its T-CONT values.
## v1.11.0 - 2026-10-21

- Releases: confd v1.8.0 (the web UI's management-address change goes
  through the `mgmt-addr.sh` trial and opens the new address by itself) and
  metricsd v1.7.0 (`gpon_soc_temperature_celsius`, from the new SoC
  temperature sensor).

- OMCI, switch: two more shapes of the OLT's extended VLAN table (class 171)
  are applied instead of leaving the whole table transparent. Only on a line
  with `VLAN_CFG_TYPE` other than 1; the manual tag never reads the table, so
  neither of our lines changes. **An EtherType filter on untagged frames**
  (IPoE 0x0800, PPPoE 0x8863 and 0x8864, ARP 0x0806, IPv6 0x86dd, EAPOL 0x888e)
  is a classifier row of the second layout that compares the frame's own
  EtherType, one row a value (PPPoE is two), each with its tag and the way
  back by VID; the untagged frames of every other EtherType, when the table has
  an entry for them, are one row after those. The register facts (the EtherType
  in the low half of rule word 0, in the rows from the layout boundary up) are
  another open implementation's measurements, and the host test pins its words
  (0x000188b5 and 0x0000774a for 0x88b5). An EtherType filter on tagged frames
  is still refused: the value behind a tag is not known. **An entry that
  discards** the C-tagged frames of one VID (a priority named or not) is an ACL
  rule on the UNI ports, which has a drop action the classifier lacks; the ACL
  decides before the classifier, so the priorities of that VID that another
  entry of the table names are left out of the discard (one rule each for the
  rest), and an overlap nothing orders, a discard in a table that acts on
  S-tags, or a discard of untagged, every-VID or double-tagged frames, is
  still a refusal that leaves the table transparent and names the entry and
  the reason. The ACL lookup is turned on for the UNI port and never off;
  entries 16 to 47 on template 5 are the block, the snooping copy keeps entry
  8. The rows the previous change left "unproven" for two tags at once are
  now pinned to the other implementation's measured words (one entry writing
  both tags on an untagged frame, two pops on an S-VID, a pop and a push with
  priority 0), and `docs/SWITCH.md` says which shapes are measured. Host-tested
  (`test/odi_switch_bdgconn_extvlan_test.c`, `src/omci/test/extvlan_test.c`,
  `src/omci/vlan-test.sh`); not run through an OLT that provisions them.

- The SoC die temperature as a standard hwmon sensor: `/sys/class/hwmon/hwmonN`
  with name `rtl9602c_soc`, `temp1_input` in millidegrees C (`CONFIG_ODI_HWMON`,
  `odi_hwmon.c`, which selects `CONFIG_HWMON`; it was off). Read-only: it reads
  two switch-core words (0x138, the sensor control with its enable bit, which
  the boot already leaves set, and 0x13c, whose bits 22:16 are the reading in
  degrees C) and writes neither; with the sensor not enabled the file reads
  `-ENODATA`. The scale and offset are inferred, not calibrated: one stick read
  62 C while its optics module read 46 C, and the stock firmware reads 63 to
  65 C in the same state, so it is a trend, good to a few degrees. The decode
  is host-tested (`test/odi_hwmon_test.c`). Kernel size: the uImage grows by
  2,746 bytes (1,151,597 to 1,154,343 of 1,359,872, 205,529 spare), the
  uncompressed kernel by 8,488. Not yet run on a stick. The exporter serves
  it as `gpon_soc_temperature_celsius` in the odi-sfp-exporter release that
  carries it; this repo pins that exporter only after it is released, so the
  image reports nothing on `/metrics` until then.
  On a stick: hwmon0 rtl9602c_soc read 61 C, and the reading moves between
  reads.

- Classifier layout boundary read the right way round: `CLASSIFY_SETUP` bits
  12:5 count the rows at the top of the table that follow the second rule
  layout, so the first such row is 256 minus the count. The code and
  `docs/SWITCH.md` read the field as the first row itself. The two agree at
  128, the only value this image programs, so nothing changes on a running
  stick; at the stock value 0x1c09 (224) the S-tag rows would have been
  placed in rows the hardware treats as the second layout, where the S-tag
  template is not known to work. The module-load capture of the stock
  firmware agrees: with 224 in place it sweeps rows 0-31 in the first rule
  and mask tables and rows 32-255 in the second. Another open implementation
  measured the same direction. Host test pins the mapping at 128, 224, 0 and
  255.
  On a stick (count 128) the classifier rows land where they did before.

- OMCI: the FEC performance history (ME 312) reports Total code words, and
  the GEM port network CTP (ME 268) reports its Encryption state. Total code
  words was always 0 ("not reported"); the driver now derives it exactly: 153
  code words in each downstream frame with FEC on (152 of 255 bytes and one
  shortened last code word, G.984.3 clause 13.2.1) for every 125 us frame the
  downstream frame counter advanced, counted only between samples that saw FEC
  on and LOS and LOF clear, and a step larger than the time between the
  samples allows (a re-lock) is skipped. It is a new line in
  `/proc/odi_gpon/counters` (`ctr ds_fec_total_cw`) and counts per 15-minute
  interval like the other ME 312 counts, so an OLT computing an error ratio
  has a denominator. Encryption state (attribute 8) was always 0, so an OLT
  and the UI read "unencrypted" for ports the OLT encrypts; it now answers 1
  for a GEM port the downstream path decrypts, from a new `enc_ports` line in
  `/proc/odi_gpon/status` (read back from the AES bit of each downstream slot,
  and from ports the OLT asked for before the slot exists). The comment in the
  GPON driver that said no encrypted port had ever been seen was stale and
  contradicted `docs/SWITCH.md`; it is corrected. Host-tested
  (`test/odi_gpon_ctr_test.c`, `test/odi_switch_ds_encrypt_test.c`,
  `test/procparse_test.c`, and the omcid suites `pm` and `enc`). On a stick
  whose OLT encrypts its data ports: `enc_ports` lists the five, ME 268 reads
  1 on their CTPs and 0 on the OMCC's; on that line downstream FEC is off, so
  Total code words stays 0 as it should. The count with FEC on is host-tested
  only.

- Docs: `GPON_DYING_GASP` (SETTINGS.md and the driver comment) no longer says
  that only the first copy leaves on a reboot: all three have been seen to
  leave on a stick under test, and in another open implementation's later
  measurements. Still off by default.

- GPON: new key `GPON_KEEP_DISABLE` (default `0`, today's behaviour unchanged
  and nothing written or read). With `1`, a stick the OLT disabled with
  Disable_Serial_Number stays in Emergency Stop (O7) as G.984.3 asks: this
  stick's own PON restarts (`gpondeact`/`gponact`, the omcid respawn,
  `apply.sh` identity changes) end in O7 with the laser line high, and so
  does every reboot and power-off, until the OLT enables it. The disable is
  recorded in the marker file `/etc/config/gpon-hold`, written by the kernel
  calling `/etc/scripts/gpon-hold.sh sync`. `gpon-hold.sh clear` releases it by
  hand, and a safe-mode boot ignores it. `/proc/odi_gpon/status` gains
  `o7_hold keep <0|1> held <0|1>`; the kernel logs `event=o7_hold`. New
  `/proc/odi_init` verbs `gponkeep` and `gponhold`; `gponhold sim` is a test
  hook that acts as if the OLT had disabled this ONU and sends nothing.
  On a stick, with the hook: O7 with the laser off (bias 0 mA), held through
  a PON restart and a reboot (the boot never ranged), and `gpon-hold.sh
  clear` took it back to O5 with its services in 30 s. Not yet seen with a
  real disable from an OLT.

- OMCI under a host-side flood, opt-in: with `odi_nic.omci_ring` on (boot
  parameter, or live through `/sys/module/odi_nic/parameters/omci_ring`)
  the NIC runs a second receive ring, and the switch routes the OMCI frames
  it traps to the CPU (internal priority 7) there; every other frame stays
  on the main ring. Every poll serves the OMCI ring first, so a flood toward
  the stick from the host side that keeps the main ring full cannot take
  the OMCI frames' descriptors. Both rings share
  the same frame path, so the PON-port drop that keeps everything but OMCI
  from the fibre out of the IP stack is unchanged. **Off by default**: off,
  the NIC is programmed exactly as before. A first build that always ran
  the second ring left a stick unreachable after boot. The likeliest
  cause, not yet confirmed on hardware: with multi-ring receive on, the
  engine also checks the flow control of the four rings this driver does
  not use, and at their reset values they hold PAUSE. Turning the ring on
  now first parks those four on a ring of their own that cannot ask for
  PAUSE. `/proc/odi_nic` gains `omci_ring`,
  `rx1_taken`, `rx2_taken`, `rx2_other` (non-OMCI frames on the OMCI ring)
  and `rx_missed` (frames the NIC dropped with no free descriptor).
  Host-tested against a model of the flood and of the six rings' flow
  control. On a stick, turned on live: the host side stayed reachable, no
  PAUSE and no drops at rest, and the OMCI of each re-registration arrived
  on the second ring (19 of 19, 98 of 98; once, on the first re-registration
  after the first enable, 19 of 20, not explained). Under a 30 s host flood
  with a re-registration in it, every OMCI frame arrived with the ring off
  as well as on: with flow control on, the switch holds the flood back and
  the single ring did not lose OMCI there, so the ring stays off by default
  until a case that needs it is seen. With it on the NIC drops the surplus
  host frames itself (`rx_missed`) instead.
- The watchdog CPU-port rule counts the frames taken from the main receive
  ring only, so OMCI arriving on the second ring cannot hide a main ring
  that takes nothing.
- A trial boot that nothing reaches goes back by itself: on a trial only
  (not committed, the next boot goes to another slot), `trial-guard.sh`
  registers an `odi_wdt` client that resets the board 300 s later unless
  the management path is seen working both ways first (a TCP session from
  the host, or a neighbour that answered the stick). The reset lands on
  the committed slot, with `reason=wdt_client:trial` in the ramlog; until
  now such a trial stayed up until someone power-cycled it.
  `/etc/config/trial-guard.off` turns it off. Host-tested
  (`test/trial_guard_test.sh`) and in the qemu harness; on a stick it armed
  at boot and released on the first ssh session.
  Run on a stick whose OLT sends none of these entries, with its service
  unchanged; the new shapes themselves are host- and qemu-tested only.

- Toolchain: built with odi-toolchain uclibc-v4 and freestanding-v3, which
  deny the `bltzl` instruction in the ISA audit (it decodes but
  misexecutes on the RLX5281). The compilers and libraries are unchanged:
  a rebuild against the previous images gives the same kernel and the same
  rootfs, file for file, except busybox, whose only difference is the
  build time in its banner.

- Management address: a change of `LAN_IP_ADDR`, `LAN_SUBNET`,
  `LAN_ENABLE_IP2`, `LAN_IP_ADDR2` or `LAN_SUBNET2` is now a trial, like the
  host port. `mgmt-addr.sh try KEY=VALUE...` applies the new address in RAM
  (`network.sh addr` reads `/var/run/mgmt-addr.trial` instead of the store
  while it exists), keeps the previous primary on `br0:4` so the old address
  still answers, and goes back by itself after 90 s unless
  `mgmt-addr.sh confirm` arrives over a new address, which is the proof that
  the new address is reachable; a confirm over the old one is refused. Only
  a confirmed trial writes the keys, so a reboot in the middle of one comes
  up on the old address. No trial in a safe-mode boot, which stores the keys
  directly as before. `network.sh primary` prints the address the primary
  device holds. Host-tested (`test/mgmt_addr_test.sh`); on a stick, an
  unconfirmed trial reverted at its deadline with nothing stored, a confirm
  over the new address stored it, and a reboot mid-trial came up on the old
  address. The web UI side is odi-ui, which stores these keys through the
  trial when this script is present.

- GPON: new experimental, off-by-default key `GPON_DYING_GASP=1`: the stick
  sends the Dying_Gasp PLOAM message (up to three copies) before an orderly
  reboot, halt or power-off, only from O4 or O5 and never when the PON drops,
  so the OLT can tell a planned restart from a lost ONU. The kernel logs
  `Dying_Gasp sent <n> of 3 times before the restart`. What the OLT does with
  it cannot be confirmed from here, hence off by default.

- Diagnostic bundle: `module/chip.txt` decodes the GN25L95 laser driver
  words that set the modulation (ER_CTRL with its automatic
  extinction-ratio loop bit, MOD_DAC, MOD_LIMIT, PERSIST), and
  `host-link.txt` adds the host SerDes lane control word (0x214: receiver
  in sync, lane mode) and its in-band negotiation word (0x21808). Reads
  only; nothing read or exported these before. On a stick under test the
  loop is on and MOD_DAC moves by itself.

- Kernel: the DMA invalidate no longer drops a cache line that is only
  partly inside the buffer. The first and last line of an unaligned range
  are written back and invalidated, the lines in between are invalidated
  as before, so a neighbouring object sharing the line keeps its data. The
  receive buffers are line aligned, so nothing changes for them today.

- OMCI: a MIB reset now drops the Get-Next walk a table Get had primed. A
  Get-Next after the reset answered the bytes of the row the reset had just
  discarded; it is now refused with a parameter error, as a Get-Next with no
  Get before it. The OMCI test suite also gains vectors for the transaction
  correlation id crossing 0x7fff to 0x8000 (the priority bit) in a MIB
  upload and in the retransmission cache, which keys on the whole frame.

- Docs: the by-hand slot restore in `docs/FLASHING.md` now says to write
  the kernel last, lists the checks to make before pointing a boot at the
  restored slot (running and committed slot, the uImage magic, the partition
  against the dump or the tarball) and says plainly that the header and data
  CRC cannot be checked on the stick, then sends the boot through
  `sw_tryactive` and `nv commit`, never `sw_active`.

- Docs and comments: `PORT_VLAN_INDEX` holds two ports per word (12 bits
  each), so the platform step's two plain writes to words 0 and 1 already
  set the PVID of all four ports. The comments in the switch driver and
  `docs/SWITCH.md` said ports 2 and 3 were not written. No behaviour
  change.

## v1.10.0 - 2026-10-21

- Switch: the downstream queues of the UNI (ME 277, the ones the OLT
  gives a Weight) are now scheduled: a queue with Weight 2 or more is
  weighted, at that weight, and one with 0 or 1 is strict, in the queue
  number the OLT names in RelatedPort (a higher number is served first).
  The stock driver ignored the weights and left all eight queues strict, so
  what the OLT provisioned for traffic toward the host had no effect. An OLT
  that sets nothing leaves the queues as the boot had them, the OLT's
  MIB reset puts them back, and `odi_switch.ds_sched=N` turns it off.

- Switch: downstream frames now take the queue the OLT provisions. Every
  frame from the PON had internal priority 0 and went to queue 0 whatever GEM
  port it came on, so the queue schedule above had no effect. The downstream
  classifier row of a bridge connection now assigns the priority of the queue
  its GEM port points at (ME 268 DsPriQPtr, then that ME 277's RelatedPort),
  so the frames land in that queue; the OLT's weights apply to them. A queue
  that is not the UNI's (a VEIP's) or has a priority past the eight queues
  leaves its flow at queue 0, a MIB reset takes it off, and
  `odi_switch.ds_steer=N` turns it off. omcid sends a flow again when the OLT
  writes the queue's RelatedPort after the flow was created. On a stick the
  rows' priority moved the frames to the queue (per-queue page counters), a
  download at about 300 Mbit/s ran at the same speed, and a row cannot be told
  apart by GEM port (the rule field read as the flow index does not hold it),
  so the steering follows the GEM port of the bridge connection.

- Kernel: the config drops the subsystems for hardware the board does not have
  and the interfaces nothing in the image reaches (VT console, input and HID,
  the FPU emulator, power management, seccomp, epoll, PTP and PPS, the block
  I/O schedulers and partition parsers, and a few more; `docs/KERNEL.md` lists
  each with its size and why it is safe). The uImage goes from 1,310,849 to
  1,149,701 bytes, so the kernel partition has 210 KB spare instead of 49 KB,
  and the loader has 12% less to unpack. Nothing the image runs changes; a
  floating-point instruction would now raise SIGILL instead of being
  emulated, and no program in the image has one.

- CI and tests: `make test-host-asan` builds the C unit tests of `make
  test-host` (and the host-built tests of `src/nv` and `src/diag`) with
  AddressSanitizer, UndefinedBehaviorSanitizer and leak detection, and a
  `host-asan` job runs it on every pull request. The first run reported
  nothing. The test scripts compile through `cc ${ODI_SAN_CFLAGS-}`, empty in
  the plain leg, so the two legs build the same way. No shipped code changed.

## v1.9.0 - 2026-10-21

- The image ships confd v1.7.1 (odi-ui: sign-in falls back to the built-in
  credential only when the credential file is missing or empty, never on a
  read error; every API route documented).

- Kernel, GPON: a GPON MAC that is not clocked answers its registers with
  `0xca0eca0f`, which has LOS, LOF, FEC and LOM all set. The line sample and
  the counter sample now treat that word as no reading: it is not fed to the
  state machine as a loss, not counted as an alarm and not added to the
  counters (one log line says so). The boot log also names the chip id, the
  GPON MAC revision and the MAC memory self-test result once
  (`odi_gpon: chip id ..., GPON MAC rev ..., memory self-test ...`).

- Kernel: `panic=10` on the built-in command line. A panic used to wait for
  the userland watchdog deadline (about 44 s) before the board restarted; it
  now restarts 10 s after the panic, through the watchdog restart handler,
  which leaves the trace time to reach the ramlog.

- Kernel, optics: the laser driver id read at boot (the `laser` PON step) is
  tried up to three times, 200 ms apart, before the step gives up. One failed
  read used to leave the laser off and GPON refused for the whole boot with
  the stick otherwise healthy; the module can stop answering for a while
  around an EEPROM copy. A read that succeeds with another id is still
  refused at once, and the page select is put back where it was found even
  when a try failed in the middle.

- Kernel, tools: the three switch registers 0x026008, 0x02600c and 0x026010
  are no longer read. Another open implementation's measurements show a read of
  them stalling the board until the watchdog reset it; ours read 0 in earlier
  dumps, so it depends on the state of the block, and nothing of ours needs
  them. `/dev/odi_sw` register get returns `EPERM` for them (one log line), and
  the register dump list leaves them out. Writes are untouched.

- Kernel, NIC: a receive descriptor that is not both the first and the last
  piece of a frame is dropped and counted (`rx_split_dropped` in
  `/proc/odi_nic`, `rx_length_errors` on the interface, one log line). A frame
  longer than the 1600 byte buffer arrives in pieces and only the first has the
  CPU tag, so the later pieces read as source port 0 and skipped the test that
  keeps everything from the fibre away from the IP stack. Normal frames carry both bits.
  Not yet checked on a stick: if `rx_split_dropped` counts on ordinary traffic the check is wrong.

- OMCI: the FEC performance monitoring history data entity (class 312, which
  an OLT creates and then reads) reports real values instead of zeros, on a
  15-minute interval framework that other history entities can reuse. A Get
  returns the last completed interval and Get current data the one in
  progress; Interval end time counts completed intervals from 0 at Create;
  Synchronize time puts the boundaries on the OLT's quarter hour and starts
  every count again. Corrected bytes, corrected code words and uncorrectable
  code words are the differences of the GPON driver's running totals
  (`/proc/odi_gpon/counters`) over the interval, so nothing in the hardware is
  cleared and the metrics counters are untouched; FEC seconds is the number
  of seconds in which an uncorrectable code word arrived. Total code words
  stays 0: the MAC does not count received code words and a derived figure
  would be a guess. Threshold crossing alerts are not raised. After an omcid
  restart an entity starts a new history, from interval 0. New omcid options
  `-n` and `-t` (counter file, interval clock) are for tests, which run as the
  new `pm` OMCI suite and cover the counter parser on the host.

- Docs: `bltzl` is corrected from "confirmed to work" to "decodes but
  misexecutes". It was listed as working from a probe that could only see
  SIGILL; a probe that checks the delay slot, run on a stick, shows the slot
  always running and control always falling through, for a taken and a
  not-taken operand alike (another open implementation measured the same). Nothing
  in the image contains it (`-mno-branch-likely`, mips1 freestanding, and
  every shipped ELF and the kernel scanned for the encoding), so no binary changes;
  `docs/CROSS-COMPILING.md`, `docs/HACKING.md`, `toolchain/README.md` and the
  ISA field note say so. The ISA gate itself lives in the toolchain image
  (odi-toolchain): its allow list drops `bltzl` and its deny list gains it in
  that repository, and the gate refuses it from the toolchain image release
  that carries the change, after the pin in `toolchain/images.env` moves.
- Docs: `docs/KERNEL.md` says why `0x80000000` is a safe link address (the
  first KiB of the image is the reserved exception-vector area, `head.S`
  `.fill 0x400`; `_stext` is `0x80000400`), that the core runs `ll`/`sc`/`sync`
  and how that is known, and that there is no thread pointer (`rdhwr $3,$29`
  faults on the stick), so no TLS.
- Docs: `docs/FLASHING.md` has a whole-flash backup, check and restore recipe
  (the MTD layout, a ten second streamed read, per-partition checksums, what
  each partition's restore needs), with the warning that the config partition
  holds the identity that cannot be regenerated.

- OMCI, switch: the OLT's extended VLAN tagging operation (class 171) is applied
  entry by entry, not only the one shape "untagged frames get one VLAN". On a
  line with `VLAN_CFG_TYPE` other than 1 (the manual tag never reads the
  table, so neither of our lines changes), each entry becomes a bridge rule
  with the rule that undoes it downstream: a C-VID translated to another
  (the priority kept or assigned), one VID split into rows by priority
  (each row its own rule, on the GEM port its priority maps to), a tag
  removed or added, an S-tag pushed, replaced or popped, in front of a C-tag
  or on its own. The class 84 list of the GEM-side port still decides which
  result VIDs are admitted, and a single-tag default rule that discards stops
  tagged frames the table does not name from passing. A table with one entry
  the classifier cannot carry out (an EtherType or DEI filter, a discard, a
  priority copied between tags, a filter on both an S-tag and a C-tag) is
  still left transparent, and the log now names the entry and why. The
  classifier facts behind the S-tag rows are in `docs/SWITCH.md`
  ("Extended VLAN rules"); the S-tag template register is written only when
  such a row is installed, so a line without one sees the same register
  writes as before. Checked as classifier row words in the host tests and
  under qemu; not yet on an OLT that provisions these shapes, and the
  rows that need an S-tag and a C-tag action together are marked unproven
  there.

- Switch: the bridge-connection command accepts rules of the extended VLAN
  operation, one direction each: a C-VID translated with the priority kept or
  assigned, an S-tag added, replaced or removed, and a match on the S-VID. The
  S-tag template register is written only when such a rule is installed, so
  the lines that do not use one see the same register writes as before. The
  facts behind it are in `docs/SWITCH.md`, "Extended VLAN rules". omcid does
  not send these rules yet.

- OMCI: omcid can now read an extended VLAN tagging entry (class 171) as what
  the switch classifier can carry out: which tags it matches, what happens to
  the C-tag and the S-tag, and the rule that undoes it downstream for each
  downstream mode. Nothing uses it yet, so nothing changes on the wire.

- CI: the third-party sources (busybox, dropbear, iproute2, the kernel tarball and the pinned confd and metricsd releases) are cached between runs, keyed by the files that pin their versions and checksums, so a run no longer fails when an upstream mirror does. A version bump misses the cache and fetches anew; every file is still checked against its pinned checksum.

- omcid: a stored `VLAN_MANU_TAG_PRI` above 7 (255, the factory value) no
  longer switches the manual VLAN tag off. v1.4.2 to v1.8.0 read it as "no
  tag", so a line whose OLT wants the untagged frames of the router tagged
  with `VLAN_MANU_TAG_VID` (VLAN 30, six upstream flows) got every service
  as a VID filter: the router's untagged DHCP requests were dropped in the
  stick (O5, optical normal, no lease), while v1.3.0 worked. The stock stack
  tags with the VID whatever the priority (only VID 65535 with priority 255,
  which its start script builds for the other settings, never matches), and
  no 802.1p tag carries a priority above 7, so the tag is added with
  priority 0, as the switch driver already took it. A new six-flow session
  and its driver-call golden (`omci-session-isp3`) replay the line.
  Refs #42

**Automated dependency bump.**
- Kernel: Linux 6.18.57 (was 6.18.56), the upstream stable update.

- OMCI: a downstream priority queue (ME 277) whose Weight the OLT had set
  went to the switch driver as priority 0, whatever its entity: the row built
  for it had no RelatedPort, so all eight of the UNI's queues carried the
  same priority. Each now carries the priority its entity id gives, and its
  marking, read the way the MIB upload reads them (what the OLT wrote, else
  the stock default). A queue of a VEIP, or of anything that is not a PPTP
  Ethernet UNI, now names the PON port, as its flow already did, instead of
  port 0. The driver does not use either value yet, so nothing changes on the
  wire.

- Multicast (experimental): IGMP snooping that follows what the OLT provisions, off unless
  `IGMP_SNOOPING=1` is set (`apply.sh igmp` applies it). While the OLT asks
  for it (a multicast GEM interworking TP with an address row and a
  multicast operations profile with IGMP function 0), `igmpd` keeps one
  static row a group with the host and the PON as members, honours the
  profile's group limit, dynamic and static access control lists, query
  timers and immediate leave, and, once every group the host already watches
  has had one membership interval to report again, drops IPv4 multicast
  from the PON that no row names, so a group nobody on the host side joined
  stays off the host link. The snooper sees only a copy of the host's IGMP,
  made by one ACL rule on the host port, so the OLT still sees every report
  and leave, and nothing from the fibre reaches the CPU: the rule matches no
  frame of the PON port, the OLT's queries are not copied, and the daemon
  transmits nothing. The daemon holds its switch state as a lease on its
  `/dev/odi_sw` descriptor: if it dies, the switch floods again and its rows
  are deleted. ME 311 now reports the join count and the active group list.
  Without the key nothing changes. Not yet run against a real multicast
  stream, which neither of our lines carries; IPv6 (MLD) is not snooped and
  the multicast tag controls of ME 309 are stored, not acted on.

- OMCI: the multicast operations profile (ME 309) keeps all of its
  attributes. Its row was cut at 64 bytes, so robustness, querier address,
  query interval, maximum response time, last member query interval,
  unauthorized join behaviour and the downstream multicast tag control were
  never stored and every Get of them answered zero. Rows are now 96 bytes.
  omcid also writes what the OLT provisioned for multicast (ME 281, 309,
  310: IGMP function, timers, group limit, access control lists) to
  `/var/run/odi-mcast.conf`, a quiet second after the last OMCI frame, for
  the IGMP snooper to read.

- OMCI: ME 50 (MAC bridge port bridge table data) lists the addresses the
  switch has learned for the bridge port, read from the L2 table at every
  Get: the host's behind a UNI port, the fibre side's behind the others,
  each with its age, whether it was learned or is static, and whether it
  blocks. It always answered an empty table. This is what the OLT asks ME 50
  for, so it now sees the MAC addresses behind the stick. The table is read
  a page at a time through a new driver command (the stock one only counts).

- Switch: the MAC learning limit the OLT sets (ME 47 allowed MACs, else the
  bridge profile's learning depth) is applied to the UNI port; the command
  was refused, so a host could fill the 1088-row table whatever the OLT
  asked. A frame from an address past the limit is still forwarded, only not
  learned. A limit of 0, and any limit on the PON or CPU port, is refused.
  The limit goes back to its boot value when the OLT resets the MIB.

- Switch: the UNI rates of the OLT's traffic descriptors on a bridge port
  (ME 47 inbound and outbound) are applied, an ingress policer and an
  egress shaper on the UNI port, which the driver used to ignore. A rate
  below 1 Mbit/s is refused and the port keeps the rate it had, so a
  malformed descriptor cannot cut the host off; "no limit" is the boot
  state. They go back to it on a MIB reset. omcid now converts a PIR of 512
  Mbyte/s or more correctly: the conversion overflowed and gave a small
  rate, which would now have been applied.

- Switch: the driver commands that are still refused say why in their one
  log line (max frame size, DSCP remap, MAC filters, S-VLAN TPID, the dot1
  rate limiters, UNI QoS). Port bridging (63) set to 1 and pause control
  (40) set to 0 succeed without a write, as both are already the state of
  the stick; the other values are refused, because the stock effects would
  rewrite the PON isolation masks and switch on PAUSE toward the host.

- Switch: a frame that enters from the PON port with the source MAC of a
  host learned on another port is dropped and the host's row stays, where
  before it moved the host to the PON port (SA_MOVED of the PON port set to
  1; it was 0 after the module-load replay). A reflected or forged frame
  from the fibre can no longer divert traffic for a host toward the fibre.
  The other ports keep moving a row. The L3 engine, which survives a warm
  restart in whatever state the previous firmware left it, is turned off at
  boot when it is on (it was already off on the sticks checked). The comment
  at the unknown-multicast flood mask now records why the CPU is out of it
  and what that costs if the stick ever runs IPv6 or joins a group.

- Switch: the L2 table is emptied at every boot, after the CAM is turned on
  and before the stick's own MACs are pinned. The table survives a warm
  restart, so rows left by the stock firmware or an earlier image (static
  multicast groups, the pin of a MAC that has since changed, a row in the
  CAM) stayed until the power was cut. The unicast delete now writes the
  row's own words instead of the key alone and checks that the row is gone,
  because a key-only delete was measured, on the same switch, to leave a row
  behind the CPU port; a pin that cannot be removed is logged.

- OMCI: the ANI-G laser bias current alarm (alarm 6) is now raised, from the
  module's own bias alarm flags (A2h byte 112, high or low). G.988 gives the
  ANI-G no bias threshold to set, so the module's limits decide, and it
  clears its flag itself. Driver command 10 now also answers the DDM alarm
  selector (A2h bytes 110-117 at data[0..7]); an older driver refuses it and
  omcid then leaves the bit alone.

- Optics: a 16-bit DDM reading (temperature, supply voltage, bias current,
  Tx power, Rx power) can no longer come back torn. The controller reads one
  byte at a time and the module refreshes its readings on its own clock, so a
  refresh between the two bytes gave the old high byte with the new low one:
  a value 256 counts off (1 C of temperature, 0.5 mA of bias, 25.6 uW of Rx
  power) for one sample. The high byte is now read once more after the word,
  and the word is read again when it moved (three tries at most). The cost is
  one extra byte read per DDM word, on the DDM ioctl that `diag` and the
  exporter use; the OMCI driver command 10 keeps the stock command's exact
  register sequence (it is replayed against a capture of it). A single multi-byte controller
  transaction, as SFF-8472 describes, was not tried: nothing here shows how
  the controller returns a wider read.

- Documentation of a decision, no behaviour change: why TX-disable is asserted
  in O7 only (comment at `hw_tx_gate()`), checked against G.984.3 10.2.2 and
  Table 10-1: only O7 and O6 ask for no light, and gating O6 would cost more
  than it protects until the MAC's own O6 behaviour is decoded.

- Optics: the laser driver check at boot no longer writes the A2h page
  select (byte 127) when it already reads the wanted page, as it does on a
  stick at rest (page 2): the write changed nothing, cost a non-volatile
  write and, on a part that stops answering for a while after one, risked
  the id read that follows. When a write is needed it is now waited out by
  reading the byte back, at most 20 times 10 ms, and a part that does not
  answer in that time is treated as "no answer" (laser left off), as before.

- GPON: BER_Interval and Key_Switching_Time are acted on and acknowledged
  only when they are addressed to this ONU or to every ONU (the standard
  allows both), and only while the ONU holds an ONU-ID. A copy addressed to
  another ONU on the same PON used to set this ONU's REI interval and its key
  switch time, and was acknowledged under its own ID.

- GPON: Extended_Burst_Length is taken only in the Serial_Number state, as
  the standard has it. Some OLTs repeat it every few seconds for as long as
  the ONU is up, and each copy rewrote the burst header of a running link; a
  later copy now changes nothing. The whole burst overhead it asks for is
  cut at 128 bytes, the standard's maximum, instead of 252.

- GPON: the console lines logged for PLOAM messages (ONU state changes,
  ranging, LOS and LOF, the first Extended_Burst_Length, an unknown message
  type) are printed after the interrupt has finished with the PLOAM queue,
  not between two messages. A console line takes about 10 ms, and a message
  queued behind it used to wait that long with interrupts off. The lines and
  their rate limit are unchanged.

- GPON: a new Request_Key now moves a key switch time that was still waiting
  to a superframe that has already passed, as the standard asks (the earlier
  time belongs to the key being replaced). Before, the new key could be put in
  use at the old time, before the OLT had sent a time for it.

- `nv commit` also sets the bootloader variable `mupgrade_en` to 0, in both
  environment copies and in the same writes that set `sw_commit` (a copy
  without the variable gets it, one that already says 0 is left alone). The
  stock bootloader ships it as 1: after the boot countdown it listens on the
  host link for a firmware image sent by multicast and writes it to flash
  with no authentication, so anyone on the LAN side of the stick during a
  reboot could replace the firmware. Committing an image now turns that
  listener off; a stick committed earlier gets it by running `nv commit`
  again.

- OMCI: a request the OLT repeats (same transaction id, same frame, after it
  did not see the answer) is answered again with the same answer, and no
  longer runs twice: a repeated Create, Set or Delete used to count a second
  MIB data sync. A Create of an instance that already exists is now refused
  with result 7 and does not count.

- OMCI: a Set that names a read-only attribute, or one that can only be given
  at Create, no longer writes it. It answers result 9 with the refused
  attributes in the failed mask, and the other attributes of the Set are
  still written. A Set that stores nothing no longer counts in MIB data sync.

- Boot: the PLOAM password is read from the config store with its XML
  entities undone (`a&amp;b` is sent to the driver as `a&b`), as omcid already
  did. A password with `&`, `<`, `>`, `"` or `'` in it was sent with the
  entity text in it and could not range.

- OMCI build: the generated tables (`omci_autonomous.c`, `omci_defaults.c`,
  `omci_mib.c`) take their entry counts from the arrays themselves instead
  of a number the generator wrote. A table edited by hand with the old count
  left in was read short (its last entries ignored) or past its end.

## v1.8.0 - 2026-10-18

- The image ships confd v1.7.0 (odi-ui: the MIB browser on the OMCI tab is
  three linked panes, tables, their entities and each entity's attributes,
  and lists every class the image knows with its id and official name).

- Build: the generated OMCI model (`omci_mib.json`) now carries each
  entity's name (`desc`), such as "GEM port network CTP" or "MAC bridge
  configuration data". Every class G.988 defines carries the standard's
  own name, word for word; the vendor-specific ones (G.988 leaves 240-255,
  350-399 and 65280-65535 to vendors, so they have no official name) keep
  the stock plugin's, tidied, or a neutral one where it named a make.
  Nothing in the image changes; odi-ui uses the names for its MIB browser.

- Build: `make releases` stops when a local confd is asked for and cannot
  be used (`CONFD_BIN` not a file, no web assets, or `CONFD_TAG=` with no
  `CONFD_BIN`). It used to warn and go on, and the image then shipped
  whatever confd and web pages an earlier build had left in `out/`.
  `CONFD_BIN`, `CONFD_ASSETS` and `METRICSD_BIN` are now relative to where
  the build is run from, as the docs' example assumed (they were taken from
  `src/`), and `CONFD_ASSETS` may also be odi-ui's
  `build/confd-assets.tar.gz`.

## v1.7.1 - 2026-10-18

- The image ships confd v1.6.2 (odi-ui: a line under the NTP server setting
  shows the stick's clock against the browser's and when NTP last synced;
  the OMCI tab reads table names up to 39 characters, so
  MacBridgePortPmMonitorHistoryData works).

- ntpd records its state in `/tmp/ntp-status`, for the web UI to read: the
  last event (`unsync`, `step`, `stratum` or `periodic`), when it happened,
  the offset and stratum ntpd reported, and when the clock was last synced
  (empty if never). busybox ntpd runs `ntp-event.sh` for this through its
  own `-S` hook. The file is replaced whole, so a reader never sees half of
  it, and it is removed whenever ntpd is (re)started or NTP is off, so it
  never claims a sync that no longer holds. Refs #151.

## v1.7.0 - 2026-10-17

- The image ships confd v1.6.1 (odi-ui: field help behind a `?` beside each
  setting, tags beside the field with the cost shown as bars, a table for the
  differences on Restore, and the OMCI_CUSTOM_ME help names bit 64).

- OMCI: omcid has the MAC bridge configuration data (ME 46) that G.988 has
  the ONU create with each MAC bridge service profile (ME 45). An OLT that
  reads it, and expects it in the MIB upload, got "unknown entity" before.
  It appears with each ME 45 the OLT creates (same entity id), goes with it,
  and the OLT can only read it: a Create, Set or Delete of it is refused.
  The stock image has no such entity, so the values are G.988's, for an ONU
  that runs no spanning tree and is the root of its bridge: the stick's own
  MAC address, the priority, hello time and forward delay of the ME 45, a
  designated root made of that priority and that MAC address, a root path
  cost and root port of 0, and the number of ME 47 ports that point at the
  bridge, counted when read. `omcid -m FILE` reads the address from FILE
  (tests only).

- OMCI: the vendor-format dump (`omcicli mib get`) printed every string
  attribute one character short, so ONU-G's four-character vendor id read as
  three. The value sent to the OLT was always whole; only the dump was wrong.

## v1.6.1 - 2026-10-17

- OMCI: omcid saves its resume snapshot before it answers a Create, Set or
  Delete, not after. A crash in between could leave the OLT told "done" while
  the respawned omcid had lost the change (the MIB data sync counter agreed
  with the OLT, the entity did not). The tables test caught it as a race.

- The image ships confd v1.5.1 (odi-ui: the page uses the full window width,
  and the Status, Forwarding and Backup tabs lay out in two columns when
  there is room).

- Kernel: nothing that arrives from the fibre reaches the stick's own IP
  stack: not the web UI, ssh or the exporter, and no ARP or ping answer,
  whatever the OLT provisions. Two independent layers, keyed on the port a
  frame entered by: the switch's isolation mask of the PON port no longer
  holds the CPU (it was all open, as on the stock image), and the NIC driver
  drops every frame from the PON port that is not OMCI before any network
  interface sees it (before, any that got through went to `eth0`, and the
  IP stack answered there for the management address). `/proc/odi_nic`
  counts the frames from the PON port (`rx_pon`) and those dropped
  (`rx_pon_dropped`). OMCI is exempt in the driver;
  `odi_switch.pon_isolate=N` leaves the switch mask open, should an OLT's
  OMCI ever need it.

- Kernel: the only frames the stick sends toward the fibre are the OMCI
  replies. The switch never forwards a frame from the CPU to the PON port by
  lookup (the CPU port's isolation mask loses the PON port, under the same
  `odi_switch.pon_isolate`), and the frames of the stick's own IP stack (its
  ARP, broadcasts and replies) are addressed to the host port alone, as they
  were. The OMCI transmit path is unchanged.

- Kernel: the stick's MAC is a static row on the CPU port of the switch's L2
  table, so a frame from the fibre carrying it as its source can no longer
  move it to the PON port and divert the host's traffic to the stick
  upstream. It follows the address (`ELAN_MAC_ADDR`, set at boot) through a
  netdevice notifier, and survives the OLT's provisioning. Learning of the
  OLT side's MACs on the PON port is unchanged.

## v1.6.0 - 2026-10-17

- The image ships confd v1.5.0 (odi-ui: every input validated by the daemon
  and the page alike, Host and cross-site checks, request deadlines, safer
  writes of the credential and ssh keys, the expert VEIP and private-entity
  controls) and metricsd v1.6.0 (the module's own alarm flags, limits and
  calibration, and a metric for an unreadable module).

- Kernel: the downstream multicast GEM port is flagged as multicast on any
  OLT, as omcid asks, not only when it is GEM port 4095. That port number
  is ISP1's multicast GEM; ISP2's (4094) and any other OLT's were set up as
  a plain data port, unlike the stock firmware. A GEM port 4095 that an OLT
  uses for two-way data now also gets its upstream flow.

- Network: `network.sh` checks the management address and netmask it reads
  (`LAN_IP_ADDR`, `LAN_SUBNET`, `/etc/config/lan-ip`, and the second pair).
  One the kernel refuses (0.0.0.0, loopback, multicast, broadcast, a netmask
  with a gap, a leading zero) used to leave the stick with no address at every
  boot, in both slots, with nothing on the network able to repair it; the boot
  now logs it and uses the stock default 192.168.1.1/24 as when the key is
  absent, and `apply.sh network` changes nothing and fails. The network and
  broadcast address of the mask (192.168.1.0 and 192.168.1.255 on a /24) are
  refused the same way, for both addresses; the web UI refuses the same values.
  A stick on that fallback now confirms the watchdog, so it stays up on
  192.168.1.1 for the address to be corrected; it used to be reset about every
  two minutes. A safe-mode boot adds the rescue address 169.254.1.1/16 on
  `br0:3`, whatever `LAN_ENABLE_IP2` says: link-local, because the stock second
  address 192.168.100.1 is the management address of many cable and fibre
  modems and a duplicate on the LAN breaks both. SETTINGS.md said a wrong
  address was already recoverable through the second address, which is off on
  both sticks the firmware has run on. `apply.sh omci` counts the
  acknowledgement seconds against its wait, so it cannot outlast the 25 s the
  web UI gives it.

- safe mode: it now also catches a reset loop and a dead config partition,
  the two ways a stick could stay out of reach with no power-cycle pattern
  able to help. A boot whose userland never confirms (odi_wdt resets it at
  123 s) or whose client stops pinging clears the short-boot counter at 60 s,
  so five short boots never happened and the stick reset every two to three
  minutes for ever. `safe-mode.sh count` now reads why the previous boot ended
  (`reason=` in `/proc/odi_ramlog_prev`) and keeps `/etc/config/boot-wdt`: +1
  for `wdt_*`, `panic` and `oops`, unchanged after a power cycle, removed by
  anything else and after 600 s of uptime. Three such boots in a row make
  the next one a safe-mode boot (`cause=watchdog`); one or two, or resets
  hours apart, never do. With the config partition not mounted or not
  writable rcS calls `safe-mode.sh no-config`: a safe-mode boot at once
  (`cause=config`, no record, the flag in `/var/run` says it), where before
  the boot was silently not counted. A safe-mode boot now confirms the
  watchdog without the mount, address and confirm-arp checks (the switch
  init must still have worked), so it stays up to be reached; the boot after
  it is a normal one, so the state machine cannot loop. The record and the
  `safe_mode` and `boot_count` events gain `cause=`, `wdt_resets=` and
  `wdt_before=`. The qemu harness declares its plain-directory config store
  mounted. docs/SETTINGS.md, docs/BOOT.md and docs/TOOLS.md have the state
  machine; the test is test/safe_mode_test.sh. The rcS trace goldens gain
  the exec lines of the new reads (written by hand: the trace harness did
  not run on htpc, its strace package install fails there).
  Refs #106

- Comments and this changelog no longer cite files of a private workspace.

- omcid: alarms and attribute value changes. omcid never sent either; the
  stock stack sends both, and an OLT shows the UNI state and the optical
  alarms from them. Once a second, in O5 only: ANI-G low and high received
  and transmit power (alarms 0, 1, 4, 5) from the DDM against the ANI-G
  thresholds or the ONU defaults, as the stock threshold monitor does, with
  a 0.5 dB band before an alarm clears; ANI-G signal fail and degrade (2, 3)
  from the kernel's `signal` lines in `/proc/odi_gpon/status` (#130); PPTP
  Ethernet UNI LAN-LOS from the host link, whose operational state now
  follows the link too. Notifications carry the alarm sequence number (1 to
  255, reset by Get All Alarms and MIB reset); suppressed, as stock, while
  the ONU-G or the circuit pack is locked, the entity's ARC is on, or the
  port is locked. ARC ends after its interval (255: never) with an AVC.
  Get All Alarms and Get All Alarms Next answer from the same alarm table.
  AVCs: an attribute flagged for one, of an entity the ONU creates, whose
  value changes without the OLT writing it (the port's and the VEIP's and
  circuit pack's operational state, the software image flags, ...); not the
  per-attribute flood the stock stack sends after a MIB reset. At most 8
  messages a second. The ANI-G signal fail and degrade thresholds go to the
  kernel (driver command 11) when the OLT sets them and, with the defaults
  5 and 9, at a MIB reset (`omci-drv-golden-isp*.txt` gain those two calls).
  New suite `alarms-test.sh`.

- omcid: table attributes keep every row the OLT sets, not only the last.
  The multicast operations profile's dynamic and static ACLs (class 309, and
  65531 once its class id is fixed), the subscriber config's service package
  and preview group tables (310), the multicast GEM interworking TP's IPv4
  and IPv6 address tables (281) and the MAC bridge port filter table (49) are
  row lists now, like class 171's tagging table already was: a Set adds,
  replaces or deletes a row by the table's own key and set control (G.988,
  checked against the stock plugins), and a Get / Get-Next answers the whole
  table. The rows survive a respawn (the resume snapshot format is version 2,
  so an older snapshot falls back to re-registration once).
- omcid: the multicast flag of a downstream GEM flow (cmd 25 `mcast_filter`)
  follows the stock rule: set while a class 281 on that CTP has address-table
  rows, the flow sent again when the table goes from empty to not empty and
  back, or the 281 is deleted. It was set for every downstream-only CTP. On
  ISP1 and ISP2 the flow is now first sent without the flag and again with it
  when the OLT writes the 224/4 row, as the stock stack does
  (`test/fixtures/omci-drv-golden-isp*.txt` gain that one line each).

- Docs: ONU2-G's vendor product code 15, which omcid reports while the
  identity switch is off, is the stock firmware's built-in default for
  `OMCI_VENDOR_PRODUCT_CODE` (the MIB table compiled into its `configd`),
  not a value captured from one stick: every stick of that firmware reports
  15 unless the key was set. The comments and `docs/SETTINGS.md` said
  "captured". No change to the image.

- omcid: a Get, Get current data, Set or Delete that names an instance of a
  modelled class which omcid holds no row for (it neither creates the
  instance itself nor was given it by the OLT) is logged once per class and
  instance per boot, as `event=no_instance` and a `no_instance` line with a
  count in `/var/log/omcid-unknown.txt` (now up to 64 entries). The answer
  does not change: omcid still answers such a request as if the instance
  existed, where G.988 and the stock stack answer "unknown instance". The
  lines are the data for deciding whether that can be made strict; the ISP1
  and ISP2 sessions on file name no such instance (`events-test.sh`).

- No OLT or ONT make is named in the tree any more: names are generic and
  describe the function. The kernel's PLOAM type 0x81 constant is
  `ODI_GPON_DS_DISABLE_SERIAL_NUMBER_VS` (a vendor-specific variant of
  Disable_Serial_Number, handled as before). In the generated OMCI model the
  six entities whose stock plugins carry a make in their names are now
  `PrivateQueueMap` (243), `Me350`, `Me370`, `Me373`, `PrivateMcastTag`
  (65282) and `PrivateSntp` (65294); the generator applies the renaming by
  class id (`src/omci/tools/mibnames.py`) and the tables are otherwise
  byte-for-byte what they were (names only: `omcicli get tables` and the
  autonomous list show the new names). The stock names are written only to a
  local sidecar (`mib-tables.py --stock-names`) that `mib-instances.py` and
  `mib-defaults.py` read to match a stock dump. Comments, docs and test notes
  that named a make say "an OLT" or "another stock image". Refs #114

- Kernel: unknown multicast (with IGMP snooping off, every IPTV channel) is
  no longer copied to the stick's own CPU. It was flooded to the CPU port as
  well as to the host, a stream's worth of frames a second that the CPU
  drops, against the guards that reset a stick whose CPU stops taking
  frames. The CPU needs none of it; broadcast still reaches it.

- Kernel and diag: signal fail and signal degrade (G.984.3 11.1.2) are
  detected. The downstream bit error rate is measured from the BIP errors
  over every 10 s of operation and compared with the thresholds the OLT
  sets in the ANI-G (driver command 11, which was refused; defaults 10^-5
  and 10^-9), with the standard's hysteresis. `/proc/odi_gpon/status` shows
  them (`signal sf|sd`), and `diag gpon get alarm-status` prints them as
  `Alarm SF` / `Alarm SD`, which the exporter turns into
  `gpon_alarm{alarm="sf"}` / `"sd"`. omcid does not yet report them to the
  OLT as ANI-G alarms.

- Kernel: an OLT that disables the ONU (Disable_Serial_Number, the rogue-ONU
  "emergency stop", state O7) now gets the laser switched off, as G.984.3
  requires and the stock firmware does: the module's TX-disable line is
  driven high in O7 and low again when the OLT re-enables the ONU (or
  gpondeact, gponact or a reboot leaves O7). The state changed but nothing
  stopped the transmitter, although the docs said it did. A laser the
  `laser` verb refused is never switched on by this. `/proc/odi_gpon/status`
  shows it as `laser_emergency_stop`.

- Kernel: removing one bridge port from unknown-unicast flooding (a bridge
  port deleted by the OLT, or ME 45 "discard unknown") no longer stops the
  flooding of every port. The driver wrote all ports on or all ports off,
  whatever port the command named; it now adds or removes that port only,
  in the mask the command names (unknown unicast, broadcast or unknown
  multicast), as the stock firmware does. The provisioning of every OLT
  seen so far gives the same mask as before.

- Kernel: an OLT that locks the Ethernet UNI (ME 11 administrative state,
  for example to suspend a service) now stops it, as the stock firmware
  does: the host port's link is forced down until the OLT unlocks it. The
  lock used to be acknowledged and ignored. On this SFP the UNI is the host
  port, so while locked the stick cannot be reached from the host either;
  OMCI still reaches it over the fibre, and a lock of the PON or CPU port
  is refused.
  A lock no longer outlives the MIB it came from: omcid unlocks every
  Ethernet UNI on a MIB reset and when it starts with an empty MIB (not on
  a resume), so a stale lock cannot leave the stick unreachable until a
  reboot. New odi.conf key `OMCI_UNI_LOCK=0` ignores OLT locks of the UNI
  altogether (docs/SETTINGS.md); the default honours them.

- Kernel: an OLT that encrypts the OMCI channel is served. The
  Encrypted_Port-ID message for the OMCI port used to be applied without its
  encryption bit, so every downstream OMCI frame would have stayed
  unreadable and the stick would never have been provisioned; the port's
  row now decrypts, as on the stock firmware. And a data port the OLT
  encrypts keeps decrypting while omcid rewrites its flow (every QoS or
  bridge change): the flow was written without the AES bit and the bit set
  again right after, a window in which encrypted frames were lost.

- nv: `setenv` and `commit` take a lock (flock(2) on the environment partition
  device, 10 s bounded, held until the process exits), so two writers (an ssh
  session and the web UI, say) no longer erase and write the same copy at
  once; unserialised, that left a half-written copy or lost an update, such
  as a `sw_commit` the UI had reported as committed. A plain `setenv` also
  reads its copy back after the write and exits non-zero when it does not
  match, as `commit` always did. If flock itself is unavailable the write goes
  ahead without the lock, as before, and says so. The syscall number and the
  EAGAIN refusal are run for real under qemu-user (`src/nv/test/flock_probe.c`,
  part of `make -C src/nv test`). On a stick, two writers running in parallel
  ended with no failed write and both environment copies consistent. Found in
  an audit of the update path.

- fwu.sh: an interrupted flash can no longer leave a valid new kernel over a
  missing or old rootfs. It used to erase and write the kernel, then the
  rootfs; a power cut or a failed write in between left a kernel U-Boot
  accepts, and the web UI offered to trial the slot. It now erases the kernel
  and rootfs partitions first, writes the rootfs, and writes the kernel last,
  so any interruption leaves a kernel U-Boot refuses (bad magic or data CRC).
  The guard comment about the watchdog on a committed boot is corrected too:
  odi_wdt arms the watchdog itself once it initialises. New host test
  `test/fwu_write_order_test.sh`. Found in an audit of the update path; a
  trial flash with this order ran on a stick.

- OMCI: the LOID password omcid reports (ONU-G, CTC LOID entity) follows the
  LOID, as the stock `runomci.sh` does: with the same `LOID` and `LOID_OLD`
  the current `LOID_PASSWD` is reported, so a changed password takes effect
  (it used to report `LOID_PASSWD_OLD` whenever the two passwords differed);
  with different LOIDs the old password stays. A MAC bridge ageing time above
  209715 s is clamped to the 21-bit register field instead of being masked
  into a short one. `omcli ident` shows the password length, never the
  password.

- OMCI: values the ONU reports on its own now match what the stock MIB
  reset puts there. ONU-G reports the configured LOID and LOID password
  (attributes 10 and 11), as the CTC LOID entity already did: an OLT that
  authenticates by the ONU-G LOID read an empty one. The TR-069 server's ACS
  address and associated tag read 0xffff (unset) until the OLT writes them,
  not 0x0000, which points at network address 0. The CTC LOID entity's
  operation id reads "CTC ". A MAC bridge ageing time set by the OLT is
  now sent to the switch in tenths of a second like the default: an
  ageing time of N seconds used to age entries out after N/10.
  `OMCI_VEIP_SLOT_ID=255`, the factory value, is read as unset, as the stock
  firmware reads it, instead of being logged as an unusable slot at every
  start and reload. A MIB reset now tears down the bridge connections
  built from the MIB it discards (one quiet second later, as any rebuild):
  they used to stay in the switch until the OLT provisioned again, and an
  OLT that provisioned fewer services left the others forwarding. Refs #103.

- OMCI: the MIB upload is built as the stock firmware builds it. An entity
  whose attributes do not fit in one upload record (26 bytes) now continues
  in as many records as it needs; omcid used to send only the first record
  and drop the rest, so an OLT reading the upload never saw ONU2-G's queue,
  scheduler and GEM port counts, connectivity and QoS flexibility, ONU-G's
  traffic management option and state, the circuit packs' queue and T-CONT
  counts, or the priority queues' drop settings. The upload also leaves out
  what the stock firmware never uploads: private and not-uploaded classes
  (158, 255, 287, 65527, 65530, 65534, 65535), attributes that are not
  supported, private, PM or tables, and the 64 reserved priority queues
  0xff00-0xff3f; and the MAC bridge port tables (49, 50, 79) of one OLT's
  bridge ports are no longer listed as entities the ONU creates for itself.
  They are created the way the stock firmware creates them, when the OLT
  creates a MAC bridge port config data (class 47): one of each at the
  port's own entity id, empty or all zero, and an upload after provisioning
  lists them. They are removed with the port (the stock firmware leaves the
  preassign table behind). omcid keeps 256 MIB rows instead of 192, so the
  three extra rows per port do not crowd out the OLT's own entities.
  An entity the OLT has set is listed once. With the factory configuration
  the upload is 205 entities in 399 records, the same as a decoded upload of
  the stock image. Refs #103.

- OMCI: the managed entity model had wrong class ids for nine entities, all
  of them above 32767: the generator (`src/omci/tools/mib-tables.py`) did not
  follow how the compiler loads such a constant, so it recorded an unrelated
  number instead. omcid now answers and reports them under their real ids:
  65529 (CTC ONU capability, was 9368), 65294 (SNTP, was 13248), 65534 and
  65535 (private T-CONT queue and VLAN configuration, were 9572 and 10212),
  65527 (OLT location, was 9796), 65528 (CTC loop detection), 65408
  (extended ONU-G), 65531 (CTC extended multicast profile, an OLT can now
  create it) and 65282. The MIB upload no longer lists ids no OLT knows.
  The ONU capability entity (65529) is created only when bit 64 of
  `OMCI_CUSTOM_ME` is set, as the stock firmware does; the factory value of
  that key does not set it. The private VLAN configuration (65535) reports
  what the stock firmware is started with: any `VLAN_CFG_TYPE` other than 1
  reads as type 0, and without a manual tag the VID and priority read as
  65535 and 255. The model also had no width for attributes 3 to 15 of ONU
  power shedding (133) and parts 2 to 15 of the large string (157) and
  octet string (307) entities, so an OLT Set of them was dropped and a Get
  of them answered nothing; they now have the stock widths. An
  `OMCI_FIXED_ME` entry for one of the corrected classes no longer applies:
  omcid answers those classes itself. Refs #103.

- Tests: the global PBO thresholds for six and more upstream flows are
  checked against the stock formula, read this time from the stock kernel of
  the image itself (the function at 0x80131f50 of its vmlinux), not only from
  the vendor SDK: six flows give 7154/6994, forty-one reach the 2000/1840
  floor, and the per-flow step is read from the register. This confirms what
  #116 writes; the fifth pair the driver kept before #116 for six flows was
  our own earlier value, not a stock one. No change to the image. Refs #116,
  #42.

- omcid, kernel: an OLT Set no longer interrupts all upstream traffic, and a
  MIB reset or an omcid restart leaves no upstream state behind. Since #116 a
  rebuild of the upstream QoS really deleted and re-created every queue and
  flow (the queues are drained), and it ran after any Set of a referenced
  queue, T-CONT, GEM port or traffic descriptor, and after any traffic
  scheduler Set at all. Now the rebuild compares the MIB with what it
  programmed and changes only that: a T-CONT whose Alloc-ID and queues are
  the same keeps them, a queue whose weight, type or rates moved is sent
  again in place, and a flow is sent only when its GEM port, T-CONT or queue
  changed; a T-CONT that changed is rebuilt alone. Only a change of the PLOAM
  Alloc-ID list still rebuilds everything. A traffic scheduler Set counts
  only when a queue in use names it and the Set carries its policy. Driver
  command 0 (the stock resetMib) now empties every upstream flow, queue and
  T-CONT block; omcid sends it on every MIB reset and when it restarts
  without resuming, which it did not before: a new provisioning could be
  refused queues the old one still held, and old flows stayed valid. The
  resume snapshot keeps what was programmed (format 2: an older snapshot is
  not resumed). Refs #116, #115.

- gpon, omcid (safety): only the OLT, by PLOAM, can put an Alloc-ID in the
  Alloc-ID CAM, the table that decides which upstream grants the stick
  answers. Since #116, driver command 21 wrote any Alloc-ID it was given into
  the CAM, and omcid sends it after every change of the PLOAM list: after a
  loss of signal, a Deactivate_ONU-ID, an Emergency Stop or a deallocation,
  the Alloc-IDs the OLT had set over OMCI came back within about 2 s, the
  deallocated one included, and the stick could answer grants of another
  subscriber's ONU. Command 21 now only looks the Alloc-ID up and refuses
  one the OLT has not assigned in this activation; omcid leaves that T-CONT
  unbound and binds it when the Assign_Alloc-ID arrives (any change of the
  list now rebuilds, not only one under a bound T-CONT). Two more ways in are
  closed: a directed PLOAM is acted on only while the stick holds an ONU-ID
  (O4 to O6), since the "no ONU-ID" value matched messages for ONU 0, another
  ONU; and only Assign_Alloc-ID type 1 assigns (type 2 is not acknowledged,
  the other reserved types are ignored), as G.984.3 9.2.3.10 says. Our own
  Assign_ONU-ID also starts from an empty CAM. Nothing changes on a line
  whose OLT assigns its Alloc-IDs by PLOAM before using them, which all three
  OLTs we have seen do. `/proc/odi_gpon/cam` (new) reads the CAM back from
  the hardware, next to what the driver recorded, to check this on a stick.
  Refs #116.

- CI: `release.yml` runs `make test` before it builds and publishes (the
  release job `needs` it). A tag used to publish an image whether or not the
  tests passed, or had even finished: ci.yml starts on the same tag as a
  separate workflow that release.yml did not wait for. No change to the image.

- igmpd: the group table no longer takes what a snooper must not hold. A
  report for an address outside 224.0.0.0/4, for a link-local group
  (224.0.0.x) or for any group whose 23-bit MAC is a link-local one
  (239.0.0.251 shares 01:00:5e:00:00:fb with mDNS) is ignored, because a
  static entry for it would restrict that traffic to the ports that asked
  (RFC 4541 2.1.2). A v3 record of type ALLOW_NEW_SOURCES with no sources,
  BLOCK_OLD_SOURCES or an unknown type no longer counts as a join. And the
  switch row is written per MAC, not per group: two groups that share a MAC
  (224.1.1.1 and 225.1.1.1), or one group on two VLANs, are one row that holds
  the union of their ports, so the last member of one no longer deletes the
  row the other still needs. Not reachable on a running stick today (igmpd is
  not started and receives no frame); it is the daemon's own unit tests that
  cover it.

- Tests: the `make test-rcs` goldens (`test/fixtures/rcs-trace-*.txt`) are
  regenerated: they predated the `safe-mode.sh count` and `boot-ok` and the
  `host-autoneg.sh boot` steps of rcS (#67, #76, #98) and so failed. The trace
  canonicaliser writes `name.new.<pid>` temporaries as `.new.PID`, which would
  otherwise have changed the golden on every run. `make test-rcs` now also
  runs in the CI `test-qemu` job, which has the busybox it needs. No change
  to the image.

- Tests: the fixed sleeps that waited for an event in `test/host_autoneg_test.sh`
  (the 0.3 s before reading what the stub `setsid` recorded),
  `test/apply_test.sh` (2 s for the delayed network apply) and
  `test/omci_respawn_test.sh` (eight 1 s sleeps around the stub omcid and the
  FIFO reader) are bounded polls for the event itself, as `safe_mode_test.sh`
  has been since #105, so a loaded host (the parallel `make test` legs) no
  longer fails them. Checks that expect nothing to happen keep a short fixed
  wait, since there is nothing to poll for. No change to the image.

- Tests: the OMCI suites no longer pass negative checks against a dead
  daemon. `src/omci/testlib.sh`: a `wait_for` that times out (which `inject_wait`,
  `settled`, `sync_send` and `omcid_up` all go through) now fails the suite
  itself and says so, `inject_wait` fails at once when `omcli --inject` cannot
  send the frame, and the suites end with `suite_exit` (0 or 1) instead of
  `exit "$fail"`, which reported success at 256 failures. The 49 call sites
  that ignored these results needed no change and check what they checked.
  `src/omci/testlib-test.sh` (in `make test-host`) tests the helpers' failure
  paths. No change to omcid or the image.

- Tests: two suites that no `make test` leg and no CI job ran are wired in.
  `make test-omci-respond` runs omcid's class 171 decode and OLT-tag rule
  test (`src/omci/test/extvlan_test.c`), `make test-igmp` runs igmpd's frame,
  group and entry tests (`src/igmp/test`); both are legs of `make test` and
  jobs of ci.yml (`mips-units`, which the `full` job now needs).
  `test/fwu_stock_flash_test.sh` exits 77 instead of 0 when it skips, so a
  skipped first-flash check no longer reads as a pass. No change to the
  image.

- Tests: `make test-qemu` generates its throwaway ssh key pair (`ssh-keygen`,
  ed25519) on every run, in `build/qemu-initramfs/qemu-test-key/`, instead of
  using a private key committed as `test/qemu/id_test`; the committed pair is
  removed. Only the public half goes into the harness initramfs. No change to
  the image.

- Tests and docs: the real MAC addresses in the L2 table examples (a stick ELAN
  address and LAN devices read back from one table, and one address from a
  stock listing) are replaced by locally administered documentation
  addresses (`02:00:00:00:00:NN`) in `test/odi_switch_l2_test.c`,
  `src/diag/` (header, fake table, `l2.golden`, `l2_format_test.c`),
  `docs/KERNEL.md` and `docs/TOOLS.md`, with the encoded table words changed
  to match. The tests check the same decode as before. No change to the
  image.

- DDM: the module can now say how its readings are to be read. Two new `diag`
  commands, from the module itself over I2C: `pon get transceiver
  monitoring-type` (A0h byte 92: whether diagnostics are implemented,
  internal or external calibration, average or OMA received power) and `pon
  get transceiver limits` (A2h 0-39: the alarm and warning limits the module
  raises its own flags against; they differ from one module to the next),
  through three new `odi_ddm.h` selectors (`ODI_DDM_MONITOR_TYPE`,
  `ODI_DDM_LIMITS_A`, `ODI_DDM_LIMITS_B`). The conversions are unchanged and
  still assume internal calibration (the one module read for this had it, with
  identity external constants); the first command is where that assumption can
  now be checked on any stick. The existing readings keep their register
  trace (a consistent re-read of the 16-bit pairs was tried and dropped: the
  DAL golden of command 10 is the stock command's own trace). Tests:
  `test/odi_ddm_limits_test.c`, `make transceiver-test`. The exporter's
  `gpon_transceiver_*` series read `pon get transceiver alarm-status`, which
  was already here.

- Kernel: the OMCI driver logs each command it does not implement once per
  command number. It logged only the first one of a boot, so a diagnostic
  bundle from a new OLT showed one of the missing commands at most.

- Kernel: the GPON MAC follows G.984.3 in two cases neither of our OLTs
  hits. The ONU answers Request_Password with three identical Password
  messages (it sent one; the OLT accepts a password only after three).
  And with no Extended_Burst_Length from the OLT, the ranged burst
  overhead is the 12-byte default, as before ranging, instead of the
  25 bytes one ISP's OLT asks for in its own Extended_Burst_Length.

- omcid: the upstream traffic descriptor of a GEM port (class 280, named by
  the GEM port network CTP) sets the CIR and PIR of its upstream queue, as
  the stock stack does (byte/s divided by 1024; for an RFC 4115 meter the
  PIR less the CIR). Our ONU-G reports traffic management option 2
  (priority and rate controlled), which lets an OLT provision these; they
  used to be dropped, and every queue went out unlimited. A Set or delete
  of such a descriptor, and of a traffic scheduler (class 278, whose
  policy decides strict or weighted queues), now rebuilds the upstream
  queues. Neither of the OLTs this was tested against provisions one; the
  rates reach the hardware with the kernel that programs cmd 23 from its
  arguments.

- Kernel: the upstream PON queues are programmed from what omcid sends,
  not replayed from one ISP's capture. The words cmd 23 wrote per T-CONT
  were ISP1's queue weights and types by T-CONT number; they are now
  decoded (docs/SWITCH.md, "Upstream PON queues") and computed: the queue
  weight and strict/WFQ type from the priority queue the OLT set, its CIR
  and PIR, several queues per T-CONT (a block of queues reserved by cmd 21,
  ordinal by ordinal; a second queue used to be refused and its GEM ports
  got no upstream flow), T-CONTs 5-15 (5 and up used to drain T-CONT 4's
  queue, 8-15 sat on queues their scheduler does not serve), a queue delete
  (cmd 24, was unsupported) and an upstream flow delete (cmd 25 with ena 0,
  which was taken for a create of GEM port 0 on every QoS rebuild). The
  T-CONT index cmd 21 hands back is the row of its Alloc-ID in the Alloc-ID
  CAM, which is how the hardware pairs a T-CONT with its grants, instead of
  the order of the calls. The global PBO thresholds follow the stock
  formula past five upstream flows. On ISP1's provisioning every register
  write is the same as before, which is what the stock driver writes.

- Docs and tests: the key counts are corrected (the image acts on 22 of the 184
  stock keys, plus bit 256 of `OMCI_CUSTOM_ME`, not "reads 21"; 161 are
  read-only), HACKING.md lists every key of `odi.conf`, SETTINGS.md says that
  192.168.100.1 is the stock default second address and that `OMCI_FAKE_OK` is
  not read, the two lines are called ISP1 and ISP2 in the docs and test
  comments, and no OLT vendor name is left in docs, tests or the 1.1.x notes
  (an OLT is described generically). Released sections are otherwise unchanged.

- Docs: the README rows for the OMCI values read from the stick and the VEIP
  and vendor-private keys say what was checked on sticks: the values on two
  sticks on two OLTs against the stock dumps, the new keys only at their
  defaults.

- Tests: `make test` takes about a minute on a native x86_64 host, down from
  about twenty minutes, with the same checks. The
  OMCI suites waited out `omcid -w`, an idle exit in 5 second units, and
  fixed sleeps, on a machine that was idle meanwhile. They now wait for the
  log line a check needs (`src/omci/testlib.sh`: `wait_for`, `inject_wait`,
  `settled`) and stop omcid with SIGTERM; one run in `qemu-test.sh` keeps the
  idle exit itself under test, and the one real timer (the 10 s provisioning
  quiet period) is still waited out once. `make test` runs lint, test-host,
  test-diag and each OMCI suite side by side (`tools/test-legs.sh`,
  `TEST_JOBS` legs at once, output of a passing leg reduced to one line, of
  a failing one shown in full, kept in `out/test-logs/`); the per-suite `make test-omci-<suite>`
  targets and the CI jobs are unchanged. A failed suite now leaves its daemon
  log in `out/test-logs/` instead of the source tree. No change to omcid.

- Tests: `test/safe_mode_test.sh` passes when run as root (htpc, CI
  containers): the "no writable config" check used a read-only directory,
  which root writes to anyway, and now uses a path that is not a directory;
  the check for the spawned boot-ok polls for the file instead of sleeping a
  fixed 0.3 s, which lost the race on a loaded host.

- OMCI: new keys for the entities the ONU creates for itself.
  `OMCI_VEIP_SLOT_ID`, the stock key, moves the virtual Ethernet interface
  point (and its UNI-G, the TR-069 server, the cardholder and the circuit
  pack) to another slot, as on the stock firmware only while bit 256 of
  `OMCI_CUSTOM_ME` is set; unset, the slot is 6 as before. `OMCI_VEIP=0`
  (odi.conf) creates no VEIP at all (nor its UNI-G, TR-069 server, cardholder
  and circuit pack), so the ONU presents only its PPTP Ethernet UNI.
  `OMCI_PRIVATE_ME=0` (odi.conf) leaves out the vendor-private entities 350,
  370 and 373, the last of which carries a third-party product string; they
  stay created by default, as on the stock firmware. A change of either re-registers the ONU.
  New `keys` omcid test suite; docs/SETTINGS.md and docs/TOOLS.md updated
  (the VEIP slot key was listed as having no counterpart in omcid).
- OMCI: values that were captured from one stick and shipped to every stick
  are now answered from the stick itself or from its configuration. The
  circuit pack (ME 6) reports the configured serial number and its vendor id
  (it reported the capture stick's vendor id and an empty serial) and the
  version of the active software image; the ONU2-G SysUpTime counts the time
  since boot in 10 ms units (it was frozen); the ANI-G optical signal and
  transmit levels follow the transceiver readings in 0.002 dBm steps (they
  were frozen); the PPTP Ethernet UNI duplex indication follows the link; the
  ONU-G state attribute follows the PON state; the private VLAN configuration
  (ME 10212) shows the stored VLAN type, mode, VID and priority; the software
  image flags (ME 7) follow the running and committed flash slots; and the
  attributes an OLT writes (priority queue scheduler pointer, TR-069 server
  and VEIP administrative state, the VEIP operational state) read as their
  power-on values until the OLT writes them, instead of the values the
  capture stick's OLT had written. The T-CONT policy stays 1 on all 16
  T-CONTs, as on the stock firmware: some OLTs provision the upstream queues
  only when it is not null, and nothing on the ONU writes it. The 64 reserved priority queues
  no longer carry the text `0x0` in their drop threshold. `mib-defaults.py`
  now refuses OLT-writable attributes, so a regenerated table cannot carry
  provisioning, and a test checks the table. omcid takes `-l file` (test
  only). New `live` omcid test suite.

- Docs: brought in line with what v1.5.0 does and what was then checked on
  sticks: README comparison rows for host port auto-negotiation, the safe
  mode, per-flow counters and committing a trial say what was run on
  hardware (and that the boot fallback was not); new rows for
  `OMCI_ACCEPT_REBOOT` and `OMCI_FIXED_ME`; docs/SETTINGS.md "Host port
  auto-negotiation" and "Not verified yet" rewritten the same way;
  docs/SWITCH.md no longer calls the PCS control bit untested.

- Diagnostics bundle: `syslog.txt` was empty in a bundle taken from the web
  UI ("timeout: can't execute 'logread'"). confd starts its scripts with no
  environment, so programs started through `timeout` looked only in `/bin`
  and `/usr/bin`. `diag-bundle.sh`, `apply.sh`, `flash`, `host-autoneg.sh`
  and `safe-mode.sh` now export a PATH that includes `/sbin` and
  `/usr/sbin`, as `slot-state.sh` already did.

## v1.5.0 - 2026-10-15

- Pin: metricsd v1.5.0 (odi-sfp-exporter): reads the `/proc/odi_gpon/` files,
  `gpon_gem_*` per-flow metrics, host-port and safe-mode metrics with alerts,
  and the 20-digit link-total fix from v1.4.1.

- Pin: confd v1.4.0 (odi-ui): Expert settings with the fixed-ME table, the
  `OMCI_ACCEPT_REBOOT=2` option, the Host port control and the safe-mode and
  fallback bars.

- Removed `local-ui/`, a copy of the odi-ui sources (including a built
  `confd`) that was committed by mistake with the host port change; the image
  takes confd from the pinned odi-ui release.

- `HOST_AUTONEG` (odi.conf, default on): `off` runs the host port (the SFP
  edge) as forced 1000BASE-X full duplex, with no auto-negotiation, for a
  host router whose SFP port has auto-negotiation off; on, as before and as
  the stock image does, it negotiates. The kernel clears or sets the AUTONEG
  bit of the host PCS control word (0x021c00, 0x1140 on every stick so
  far), through the new parameter `odi_switch.host_autoneg` (live through
  sysfs); `odi_switch.host_link` reads the host link. A change is never
  stored directly: `host-autoneg.sh try off` (or the web UI) applies it in
  RAM and reverts after 120 s unless `confirm` arrives over the link it
  changed. A boot with the key off watches the host link and falls back to
  auto-negotiation while the forced link does not come up, alternating 30 s
  each way for up to 10 minutes (`event=fallback`). The safe mode (below)
  clears it. A stick with the key unset writes and reads nothing new at
  boot. State in `/var/run/host-autoneg` and `host-autoneg.sh status`, and
  in the diagnostics bundle (`host-link.txt`). Host-tested; not yet
  trialled on a stick, and the forced side needs a host that can run
  without auto-negotiation.

- Safe mode, a recovery boot from the power alone, for a stick that cannot
  be reached or logged into (a host port mode the router does not speak, a
  forgotten web UI password, lost ssh keys). Every boot counts itself on the
  config partition right after it is mounted (`event=boot_count`, with the
  uptime) and a boot that reaches 60 s of uptime clears the count; after five
  short boots in a row (three with `HOST_AUTONEG=off`) the next boot is a
  safe-mode boot. To enter it: plug the stick in, wait about 80 s, pull it,
  wait a few seconds; repeat; then leave it in. A cold power-on spends about
  54 s before the kernel starts (measured on ISP1's stick; it can vary by
  board), so the window is roughly 60 to 110 s after power-on. For that boot
  only: the PON off (laser left off, no activation), the host port on auto-negotiation, the web UI on
  `admin:admin` and root on `admin` over ssh, passwords offered even on a
  keys-only image. Nothing in the saved configuration changes but the
  removal of `HOST_AUTONEG` when that was off; the credentials are reset in
  memory only (a flag in `/var/run`, a RAM copy of `/etc/passwd`
  bind-mounted over the read-only one). A record on the config partition
  keeps the web UI banner up until acknowledged (`safe-mode.sh ack`). The
  trade-off, documented in docs/SETTINGS.md: anyone who can power-cycle the
  stick can log into it for one boot. Host-tested; not yet trialled on a
  stick.

- `OMCI_FIXED_ME` (odi.conf): a second entry form, `CLASS:INSTANCE:ATTR:HEX`,
  makes a vendor-specific entity that exists only at the configured
  instance. Entries with the same class and instance are one entity with
  several attributes. omcid lists it in the MIB upload (the attributes in
  order, over several upload responses when they do not fit in one), answers
  a Get or Get current data of it with the attributes asked for that it has
  (the answered mask drops the others, as for its own entities), and answers
  a Get of any other instance of the class with result 5, unknown managed
  entity instance. The `CLASS:HEX` form is unchanged (any instance, not in
  the upload); a class in both forms, or the same class, instance and
  attribute twice, turns the key off. The limit is now 16 entries. For an
  OLT that expects a native ONU's vendor-specific entities. The bytes stay
  out of the logs and the diagnostics bundle redacts the key, as before.

- `OMCI_ACCEPT_REBOOT=2` (odi.conf): acknowledge only. The OLT Reboot of the
  ONU-G is answered with success and the stick does not reboot, for an OLT
  that insists on a successful answer. `1` still reboots and `0`, unset or
  any other value still refuses. Logged as `reboot from the OLT
  acknowledged, not rebooting (OMCI_ACCEPT_REBOOT=2)` and
  `event=olt_reboot result=acknowledged`.

- Per-flow GEM counters (Refs #92): `/proc/odi_gpon/flows`, one line per GEM
  flow OMCI created: `ds <flow> gem <port> gem_frames <n> bytes <n> eth_rx
  <n> eth_fwd <n>` and `us <flow> gem <port> gem_frames <n> eth_frames <n>
  bytes <n>`, 64-bit totals since the flow was created, so a silent GEM port
  on a multi-service line shows. The registers clear on read, so the driver
  samples them in the same second and under the same lock as the link
  counters, for the flows it has configured only, and nothing else may read
  them: not `diag register get`, not the stock `gpon show counter flow`. A
  flow created again, and a reset of the flow state, start its totals over.
  `diag gpon get flow-counters` prints the file, and `diag-bundle.sh`
  collects it as `pon/odi_gpon-flows.txt`. metricsd needs the odi-sfp-exporter
  release that exports it (`gpon_gem_*`); an older one ignores the file. The
  stock image has none of this. `diag gpon g f` is now ambiguous between
  `flows` and `flow-counters`; `gpon get flows` is unchanged.

- BREAKING: `/proc/odi_gpon` is now a directory of three files, one per
  topic, like `/proc/odi_wdt/`; there is no `/proc/odi_gpon` file any more
  and no compatibility copy. `status` has the ONU state, ONU id, serial,
  EqD, Alloc-IDs, REI count, TO1/TO2 expiries, interrupt bookkeeping, laser,
  last LOS, Upstream_Overhead and Ranging_Time, and the sampler's alarm,
  FEC and ONU state lines (`alarm_now`, `fec_now`, `ds_superframe`,
  `alarm_events`, `alarm_ms`, `onu_state_ms`, `onu_transitions`);
  `counters` has `ctr_samples`, `ctr` and `ctr_saturated`; `ploam` has
  `ploam ds_rx us_tx`, `ploam_us_dropped`, `ploam_type` and `ploam_ring`.
  The line formats are unchanged. Anything that read `/proc/odi_gpon` must
  open the file it needs: `diag`, `omcid` and `diag-bundle.sh` are updated
  here (the bundle now collects `pon/odi_gpon-status.txt`,
  `odi_gpon-counters.txt` and `odi_gpon-ploam.txt`), and metricsd needs the
  odi-sfp-exporter release that reads the three files: an older one finds no
  `/proc/odi_gpon` file and drops the GPON link and `gpon_provision_tcont*`
  metrics, nothing else. On the stock image, which has no `/proc/odi_gpon`
  at all, nothing changes. Groundwork for per-flow counters (Refs #92).

## v1.4.2 - 2026-10-15

- GPON: with an OLT that also sends an Assign_Alloc-ID for the ONU-ID's own
  default Alloc-ID (some do), upstream user traffic never left the stick.
  That Alloc-ID was not recognised as the default row already in the CAM, so
  it took the first pool row and the data T-CONT's Alloc-ID landed one row
  later, off the row its queue is tied to; the OLT's grants found nothing
  queued. The ONU-ID is now recorded in the default row, so that message
  changes nothing. Not yet validated on a stick with such an OLT.

- Pin: confd v1.3.2 (odi-ui): `OMCI_ACCEPT_REBOOT` on the ONU identity
  subtab, and a select whose key is unset shows the value the image uses
  instead of an empty box.

- `OMCI_ACCEPT_REBOOT` (odi.conf, `flash set`, default off): lets the OLT
  reboot the stick over OMCI. A Reboot of the ONU-G is answered with success
  and, a second later, omcid sends SIGTERM to init, the same path as
  `reboot`. Off, the Reboot is refused with result 2, "command not
  supported" (it was 1, "processing error"), so an OLT can tell a policy from
  a fault. The reboot condition byte is ignored: the stick has no voice, so
  every request is unconditional. Read when the Reboot arrives: no reload, no
  re-registration. Logged as `reboot from the OLT accepted` or `refused`, and
  `event=olt_reboot result=accepted|refused`.

- OMCI: when the tag comes from the OLT (class 171 adds a VLAN to untagged
  frames, `VLAN_CFG_TYPE` other than 1) and the class 84 list on the GEM
  side names that same VLAN, frames that arrive already tagged with it pass
  again, as G.988 leaves them on the table's default rule. Since
  v1.4.2-beta.2 that entry built only the add-tag rule, so a router that
  tags the service VLAN itself was dropped at the LAN port. Manual mode is
  unchanged.

- Pin: confd v1.3.1 (odi-ui): the Config subtab "OLT identity" is now
  "ONU identity", and it carries `OMCI_FIXED_ME`.

- GPON: a broadcast copy of a message G.984.3 directs to one ONU
  (Ranging_Time, Request_Password, Request_Key, Encrypted_Port-ID,
  Assign_Alloc-ID, Configure_Port-ID) is no longer acted on or
  acknowledged. The MAC already drops other ONUs' messages but lets the
  broadcast ID through, and the driver took such a copy as its own: a
  broadcast Ranging_Time would have ranged the stick with another ONU's
  delay. The stock driver checks the ONU-ID the same way. No OLT here has
  sent one.

- `OMCI_FIXED_ME` (odi.conf, `flash set`): omcid answers a Get or Get
  current data of classes it has no model for with configured bytes, under
  the mask the OLT asked for. Up to 8 entries `CLASS:HEX`, for example
  `200:0x0011aa,201:01020304`. For an OLT that reads vendor-specific
  entities of its own ONUs: the owner copies the bytes from a native ONU,
  and they stay on the stick, out of the image and the logs; the diagnostics
  bundle redacts the key. A class omcid implements is never overridden.
  Changing the key re-registers the ONU, as the other identity keys do.

- omcid: an OLT Set of the ANI-G GEM block length (class 263 attribute 3)
  is what omcid passes to the driver. It read attribute 12, the upper
  optical threshold, in its place: a Set of the block length was dropped,
  and a Set of that threshold was passed on as the block length. No
  hardware effect yet: the kernel does not implement that driver command
  (it answers "not implemented"), and neither ISP here sets ANI-G.

- omcid answers OMCI Get current data (message type 28) the way it answers
  a Get: G.988 gives both the same layout, and omcid keeps one set of
  values per entity. An OLT that polls the GEM port PM counters with it
  every minute got "not supported" each time, and the log an unknown
  message type (#29).

- GPON: a broadcast POPUP (O6 back to O4, after a loss the whole PON
  shared) now ranges the stick again from the pre-ranged burst and the
  pre-assigned delay of the last Upstream_Overhead, as in its first O4
  (G.984.3 Table 10-1, the ranging request row). It kept the equalization
  delay and the ranged burst length of the old ranging, so its ranging
  reply arrived late by that delay, and the OLT could fail to range it
  until TO1 sent it back to O2. A directed POPUP keeps both, as before.
  Not seen on a stick yet: no OLT here has sent a broadcast POPUP.

- GPON: a Disable_Serial_Number with the "enable all" code (0x0F) now
  brings a stick out of Emergency Stop (O7) to O2, as G.984.3 clause 9.2.3.6
  and Table 10-1 say, and as docs/TOOLS.md already claimed. It was decoded
  and ignored, so a stick an OLT had disabled stayed in O7, laser off,
  after the operator re-enabled every ONU at once, until it was power
  cycled.

- omcid applies the OLT's class 171 rule "untagged frames get VLAN X" when
  the added tag names the Output TPID attribute with DEI 0 (treatment TPID
  code 6, or 2 and 3, which copy a DEI an untagged frame does not have) and
  that attribute is 0x8100. That is the same tag as code 4, which was the
  only output-TPID form accepted. An OLT that writes 6 got "the added tag's
  TPID is not 0x8100" and a transparent stick, so a router sending
  untagged frames reached nothing.

- omcid builds the service for a bridge port of TP type 5 (the GEM
  interworking TP named directly, no 802.1p mapper). The connection builder
  walked only type 3 (mapper) and 6 (multicast), so an OLT that provisions
  this shape left the stick in O5 with no service at all
  (`provision_end ... services=0`). A type-5 port on the downstream
  broadcast GEM (interworking option 6) stays the broadcast flow, as before.
- omcid: `VLAN_MANU_TAG_PRI` 255, or anything above 7, is no manual tag, as
  on the stock firmware, where 255 is the no-tag value. It used to switch the
  manual tag on with that priority.

- The diagnostics bundle no longer scrubs a secret made of one repeated
  character from its other files. The factory PLOAM password is twenty
  zeros, and scrubbing it replaced every run of zeros in the MIB dumps
  (VLAN filter tables, priority queues, extended VLAN rules) with
  `REDACTED`, which made a reporter's bundle hard to read. Such a value is
  still redacted in the config copy. The qemu harness plants one and checks
  both.

## v1.4.1 - 2026-10-13

- Pins: confd v1.3.0 (odi-ui: the Commit button commits both U-Boot
  environment copies and refreshes the slot state; Config shows no control
  or value for a key the current mode ignores), metricsd v1.4.0 (a port
  counter reset is no longer read as a 32-bit wrap), and the uClibc-ng
  toolchain uclibc-v3: `syslog()` reconnects when syslogd has restarted, so
  klogd, dropbear and ntpd no longer lose their first line after a syslogd
  restart (seen on a stick: the first kernel line after one never reached
  `logread`). Rebuilt with it, only dropbear differs from the uclibc-v2
  build, apart from the documented build stamps.

- The syslog ring keeps days instead of hours. On a release image it
  cycled in about 3-4 hours: the serial-console `login`, respawned by init,
  gave up at its prompt after 60 s and restarted every minute (two lines
  each time), and `odi_wdt` printed its heartbeat every minute. The
  console entry is now `askfirst` (press Enter on the console, then
  `login` runs, with no timeout before that), and the heartbeat is every
  10 minutes (`ODI_WDT_HEARTBEAT_INTERVAL_S`).

- The diagnostics bundle includes the syslog ring (`syslog.txt`, from
  `logread`). omcid sends its `event=` lines (ONU state changes, OMCI
  provisioning, re-registrations) only to syslog, so a bundle had no record
  of them. The file gets the same redaction as every other file in the
  bundle; the qemu harness now plants the secrets and identifiers in syslog
  too and checks `syslog.txt` is scrubbed.

- `services stop omcid` now keeps omcid down. `svc-omcid.sh` checked only
  `modules.off`, so init restarted omcid at once; it now honours
  `omcid.off` like the other six daemons. Stopping omcid also releases its
  watchdog deadline (re-registered with 0, which frees the client), or the
  stick would reset a minute later; `start omcid` registers it again, and
  rcS does not register it while `omcid.off` exists. New
  `test/services_off_test.sh` checks every daemon `services` accepts honours
  its flag; `test/odi_wdt_test.c` covers releasing a client.
- Comments and help corrected: `odi_wdt.h` (registering arms a client's
  deadline, not its first ping), `flash` (the image defaults hold nine
  keys), `odi_init.c` (rcS runs 23 switch steps), `omcid -h` (lists `-i`,
  `-j`, `-h` in the synopsis), `ci.yml` and `kernel/README.md` (the kernel
  version is the one pinned in `kernel/618/fetch.sh`), and the CPU-port RX
  watchdog row in `docs/SETTINGS.md` (the PAUSE it counts is the CPU MAC's,
  not the host port's).

### Changed

- Docs: refreshed for v1.4.0. README: kernel version, the host-PAUSE row
  (now verified on a stick), the VLAN handling row (class 171 rule with
  `VLAN_CFG_TYPE` other than 1), the kernel size spare, the status of the two
  sticks, and a note for updaters on the laser guard (a stick with another
  laser driver chip stays dark until `ODI_LASER_UNVERIFIED` is set). Also:
  FLASHING (check the laser on the trial; `fwu.sh` names missing tools and
  uses no `cut`), BUILDING (kernel 6.18.56, confd and metricsd pins, CI runs
  `make test-qemu`), SWITCH (the random AES key per Request_Key, multicast
  filter wording), KERNEL (the NIC TX-completion interrupt, the laser verb),
  IMPROVEMENTS (a GPON link section), LICENSING (dropbear version), BOOT
  and HACKING (the respawn daemon list, the laser step, `slot-state.sh`, the
  CI jobs, stale line references, kernel 6.18.56), TOOLS and SETTINGS (the
  laser step and `ODI_LASER_UNVERIFIED`, `gponact` refused while the laser is
  off, the OLT class 171 rule, `optics.off`, the daemon switch files, the
  `/proc/odi_wdt` rows). Each claim was checked against the code on `main`.

## v1.4.0 — 2026-10-13

Read before updating:

- **The laser now lights only on the laser driver chip this image is
  tested with** (the Semtech GN25L95 of the ODI DFP-34X-2C2). On a stick
  with another laser driver the image does not switch the laser on or start
  the GPON MAC, so the ONU does not register: `dmesg | grep 'odi_board:
  laser'` says so. That protects the
  other subscribers on the OLT port from a stick whose laser this image
  does not know how to drive. If your stick ran v1.3.0 without trouble,
  `flash set ODI_LASER_UNVERIFIED 1` and a reboot switch it on again.
- **With `VLAN_CFG_TYPE` 0 the stick now applies the VLAN the OLT asks for**
  (class 171, a single "untagged frames get VLAN X" rule), as the stock
  firmware does; until now type 0 passed frames untouched. On such a line,
  remove the VLAN from the router. Manual mode (`VLAN_CFG_TYPE` 1) is
  unchanged.

- kernel: a downstream PLOAM type the driver does not handle is logged
  once per boot instead of on every message. Some OLTs send type 0x18
  (Sleep_Allow, G.984.3 Annex E) every few seconds, which filled `dmesg`
  (#42). The stick does not advertise power saving (no ONU dynamic power
  management control ME), so it does not act on it. `ploam_type ds` in
  `/proc/odi_gpon` still counts every one.

- omcid: the OLT's class 171 (extended VLAN tagging operation) now drives
  the tagging when the stick is not in manual mode (`VLAN_CFG_TYPE` other
  than 1), as the stock firmware does. Until now omcid decoded and stored
  the table but built only the manual rule or a transparent one, so an OLT
  that provisions the service VLAN through class 171 (one of some OLTs: "untagged
  frames: add VLAN 100") reached the router tagged. The supported shape is the
  Ethernet-UNI instance with one untagged-frames rule that adds a single tag
  with a concrete VID (1..4094) and priority (0..7), plus default
  drop/pass rules; it builds the same rule as the manual tag. Any other shape
  stays transparent and is logged once (`OLT rule not applied (...)`). The
  rule is rebuilt when class 171 is set, created or deleted. The manual tag
  keeps priority: ISP1 and ISP2 (type 1) are unchanged. Host-tested in
  `src/omci/test/extvlan_test.c` and `src/omci/vlan-test.sh`. Refs #29.

- kernel: the GPON boot clean covers the downstream multicast MAC filter.
  Its 256 rows are on-chip RAM like the GEM-port and Alloc-ID CAMs, and
  the boot table has it consulted for downstream multicast, but the only
  clear it got was the boot table's own, 256 requests back to back with
  no wait, never read back. The clean now clears every row with the CAM
  handshake (each request waited for), reads every row back before and
  after, retries once, and logs what it found and left (`GPON multicast
  filter before the clean: N/256 rows valid ...`, and `multicast filter
  0/256` in the `clean after` line). The same line reports the eight
  `DS_MC_FILTER_BIST` words (0x70428c), which are the filter RAM's
  self-test status, not filter rows: 0x8000 means the test ran and
  passed, and they are never written. Host-tested
  (`test/odi_gpon_init_clean_test.c`, now with a model of the filter);
  not yet run on a stick.

- kernel: an OLT's optical test of the ONU now gets the module's real
  readings. When the OLT sends a Test to the ANI-G, omcid answers with
  the supply voltage, Rx and Tx power, laser bias and temperature it asks
  the driver for one at a time (cmd 10), and the driver returned success
  with nothing in the answer: every result read 0 V, no light at either
  end, 0 mA and 0 C. The command replayed the register writes of a stock
  capture, which are the start of a two-byte DDM read (the A2h device
  select, the byte address, START), without waiting for the read to
  finish or taking the byte, and it left that read running on the I2C
  master when it let other readers in. Cmd 10 is now the same DDM read
  `diag` and the exporter use (`odi_ddm_get()`, both bytes under the I2C
  lock), at the SFF-8472 A2h offset of the type omcid asks for, and the
  word goes back where omcid reads it; any other type is refused without
  touching the bus. Its register writes are still the capture's, byte
  for byte. Host-tested (`test/odi_switch_cmd10_test.c`); not run on a
  stick yet, as neither ISP's OLT has sent a Test since.

- kernel: the GPON CAM deletes are waited for, and a CAM write that did
  not complete no longer counts as done. The Alloc-ID and GEM port
  deletes (on Deactivate_ONU-ID, gpondeact, LOS or TO1 in O4, TO2, and
  an Alloc-ID deallocation) were issued back to back without waiting for
  the CAM to finish, so a lost delete could leave an Alloc-ID row valid
  into the next activation: the ONU then answers grants meant for an
  Alloc-ID the OLT has given another ONU, the #42 failure, at run time.
  The search before the OMCC port rewrite was not waited for either.
  Each delete and that search now waits for DONE with the bound of every
  CAM poll; the first delete that does not complete ends the release,
  and the rows not deleted stay recorded, so the next release deletes
  them again. A write that does not complete records nothing: no
  Alloc-ID row (the next copy of the OLT's message writes again, and
  `alloc_ids` lists only what is in the CAM), no OMCC port, and for cmd
  25 no DS slot and no flow, with the error returned to omcid, which then
  does not claim the flow. Each case is logged, rate-limited. On a
  working CAM nothing changes: the writes are the same, the waits are
  reads. Host-tested with a CAM that never answers
  (`test/odi_gpon_cam_test.c`).

- kernel: the GPON interrupt now drains every queued downstream PLOAM
  message, not one. The PLOAM_RX event is one bit for however many
  messages arrived, so two queued during one interrupt (an activation
  burst, the three copies the OLT sends of every message) left the second
  in the FIFO until the next message came in, and the lag never shrank;
  the last message of a burst (the third Deactivate_ONU-ID, a
  Key_Switching_Time, an Encrypted_Port-ID) was handled late, or never if
  the OLT went quiet. The stock firmware drains in a loop too: a capture
  shows it popping two messages in one interrupt. The drain runs until the
  FIFO reads empty, at most 16 messages per interrupt; one queued message
  is drained exactly as before, and after a pop the FIFO counts as empty
  if either of two status reads says so, so a status that trails the pop
  cannot make the message just taken count twice. Each message is logged
  and re-arms the TO1/TO2 timers on its own, so a burst that moves the
  ONU through several states in one interrupt logs every transition. On both test lines in
  steady state the FIFO already reads empty and every PLOAM received was
  handled, so no change is expected there. Host-tested against a FIFO
  model (`test/odi_gpon_ds_ploam_test.c`). The counter sampler now takes
  its baseline (the first sample, which only clears) in the boot
  `gponact` instead of a second later: a PLOAM the framer accepted in
  that second was counted in `ploam ds_rx` but cleared out of
  `ctr ds_ploam_accepted`, leaving the two apart for good (by 6 on one
  ISP1 boot).

- slot-state.sh: safe to run again after a commit, for a web UI that does
  (odi-ui #15, not in this release's confd v1.2.1: its Commit button will
  run `nv commit` and then this script, so
  `/var/run/odi-slot`, the login notice and `gpon_uncommitted` follow the
  environment instead of staying as the boot left them). It sets its own
  `PATH` because confd starts its children with an empty environment, and a
  run after the first of a boot no longer waits up to 30 s for syslogd to
  appear before logging, which would have held the UI's reply. The first
  run of a boot keeps the bounded wait. `test/slot_state_test.sh` covers the
  run with no environment and both waits.

- kernel: every Request_Key now gets a fresh AES key from the kernel RNG.
  The ONU answered every one with 16 zero bytes, because nothing ever set
  the key, so on an OLT that encrypts the data ports (ISP1 does) the
  downstream encryption protected nothing. G.984.3 clause 12.3 has the ONU
  generate a cryptographically unpredictable key per request and
  increment the Key_Index with each one; the Key_Index was a fixed 0x02
  and now counts 1, 2, ... from boot. The key sent upstream in the six
  Encryption_Key fragments is the one written into the shadow key slot,
  and each of its eight words is now waited for (bounded) before the next
  one goes into the same register. A Request_Key that replaces a key
  already scheduled lets the next Key_Switching_Time be written even when
  it repeats the old frame number. With all keys zero, a fault in the key
  load or the key switch could not show; with a real key it would show as
  every encrypted downstream port going silent at the first switch.
  `/proc/odi_gpon` is world-readable, so its PLOAM ring now prints the
  eight key bytes of each upstream Encryption_Key (type 0x05) as `xx`,
  keeping the Key_Index and Frag_Index; nothing else prints the key.
  Host-tested end to end against the register model
  (`test/odi_gpon_aes_key_test.c`, and `test/odi_gpon_test.c` for the
  ring).

- kernel: the `gponpw` verb accepts short hex, with an explicit `0x`
  prefix. A value that registers today keeps its meaning: exactly 20
  characters are hex, and 1 to 10 characters are text even when every
  character is a hex digit ("123456" is six ASCII bytes). Short hex is not
  guessed at, because it cannot be told from text; `0x` followed by 2 to 20
  hex digits (an even number) is those bytes, zero-padded to the 10 sent.
  Anything else (11 to 19 characters, more than 20, 20 with a non-hex digit)
  was refused without a word while activation went on with no password; it
  is now logged, by length only, and leaves the old password in place.
  `GPON_PLOAM_FORMAT` is still read by nothing: what it selects is not
  known. The parser moved to `odi_gpon_pw.c`, with `test/odi_gpon_pw_test.sh`
  covering every length from 1 to 20 in each form.

- fwu.sh: no longer calls `cut`. The HSGQ M110 stock firmware has no `cut`
  applet, so the size check of the first flash from it came out empty and a
  user had to patch the script. The cksum and md5sum fields are now split
  with parameter expansion, an unreadable size is a refusal instead of an
  empty number, and the tools left (tar, sed, cksum, md5sum, cmp) are checked
  up front and named before anything is touched.
  `test/fwu_minimal_path_test.sh` runs the script through both member checks
  with only those five tools on PATH; `cut` is off `image/stock-commands.txt`.

- kernel: the 64-bit port counters (ifInOctets and ifOutOctets, and so
  the exporter's octet rates) no longer jump by 4 GiB once in a while.
  Their two 32-bit halves were read low then high with nothing holding
  them together, so a read that straddled a wrap of the low half, about
  every 34 s per direction at gigabit line rate, paired the old low half
  with the new high one. They are now read high, low, high, and read
  again when the high half moved in between (three tries at most, then
  the low half's top bit decides). Host-tested against a counter that
  advances between reads (`test/odi_reg_test.c`); not yet run on a stick.

- kernel: the switch ACL tables are cleared at boot. ACL ingress is on for
  the UNI and PON ports, and the 96 rule and action rows live in RAM that
  survives a reset: after a warm boot from the stock firmware (a trial
  that fell back, or a stick moved from it) its rules were still there
  next to ours, and after a power cut the rows hold random words, where a
  row with its valid bit set would match frames ahead of everything else.
  Only rule 1 was ever written. The platform init now reads every row of
  the rule pattern, rule mask and action tables, writes each all-zero,
  reads them back, and logs the counts (`ACL rows before the clean: N/96
  rules valid ..., ACL rows clean after 1 pass(es)`), before ACL ingress
  is enabled and before the module-load replay writes rule 1. The L2
  table is not cleared: `docs/SWITCH.md`, "Platform init", says why.
  Host-tested against a model of the table engine
  (`test/odi_switch_acl_clean_test.c`); not yet run on a stick.

- kernel: downstream AES is no longer lost when the OLT sends
  Encrypted_Port-ID before OMCI has created the GEM port, or on flows 32
  to 63. The driver dropped an Encrypted_Port-ID for a port with no
  downstream flow yet, and one OLT sends it right after O5, before any
  OMCI; the port was then created with AES off, and every downstream
  frame on it arrived as undecrypted garbage. The flag is now kept per
  GEM port and applied when cmd 25 creates the flow, and when it writes
  an existing flow again (an OMCI MIB reset), which used to clear the
  bit with no new PLOAM to set it back. The kept flags are forgotten
  with the ONU-ID, since the next activation configures the ports again.
  The DS slot table also held 32 of the 64 flows cmd 25 accepts, so
  flows 32 to 63 never got AES; it now covers all 64, and a slot outside
  it would be logged as an error rather than skipped silently.
  `docs/SWITCH.md`, "Downstream AES". Host-tested
  (`test/odi_switch_ds_encrypt_test.c`).

- kernel, rootfs: the laser is switched on only on the laser driver this
  image was validated with. Before lighting it, a new `laser` PON
  step (after the I2C steps, which the probe needs) reads the laser
  driver chip's id over I2C (A2h page 2, byte 0xa0, the same
  check the stock firmware makes); a Semtech GN25L95 (0x6a), the chip in
  the DFP-34X-2C2 sticks this image is tested on, gets the laser as
  before. Any other answer, or none, leaves the laser off, and GPON
  activation is refused, so the GPON MAC never transmits either. Another
  board may need laser set-up this image does not do, or wire the laser
  differently, and a laser that lights outside its timeslots takes down
  every other ONU on the OLT port: a user running v1.2.1 on another
  stick saw exactly that. `/proc/odi_gpon` gains `laser on <0|1> ldd_id
  <id> unverified <0|1>`, and `dmesg` (`odi_board: laser:`) says why.
  `ODI_LASER_UNVERIFIED=1` (`flash set ODI_LASER_UNVERIFIED 1`, an odi
  key in `/etc/config/odi.conf`) switches it on anyway, for a test on
  an OLT port with no other ONU (docs/SETTINGS.md).

- kernel: an upstream PLOAM reply now waits for room in the queue instead
  of being pushed blind. Every reply (Acknowledge, Encryption_Key,
  Password, REI) goes into the urgent upstream queue, and the driver never
  read the queue's full bit, so a reply pushed into a full queue, for
  example during the six Encryption_Key fragments of a Request_Key, was
  lost without a trace; the OLT repeats an unanswered message and
  deactivates an ONU that stays silent. Each push now waits up to 1 ms
  (eight upstream frames) for room, and a reply that finds no room is not
  pushed and is counted on a new `/proc/odi_gpon` line, `ploam_us_dropped`
  (`us_tx` counts only what was pushed), with a rate-limited log line. On
  both test lines in steady state `us_tx` already equals the urgent PLOAMu
  counter, so the count is expected to stay 0. Host-tested against the
  register model (`test/odi_gpon_us_ploam_test.c`).

- rootfs: `flash set` and `flash default cs` replace `lastgood.xml` and
  `lastgood_hs.xml` with a rename inside `/var/config`. They used to build
  the file in `/var/tmp` and `mv` it over the store, but `/var/tmp` is a
  tmpfs and `/var/config` jffs2, so that `mv` deleted the store file and
  then copied the new one in: a power cut or a full partition during a
  save could lose the configuration or the ONU identity (serial number,
  MAC). The new file is now copied beside the old one, compared, and
  renamed over it. Checked on a stick: same content, new inode, nothing
  left behind.

- kernel: an OLT that asks for a burst preamble longer than 56 bytes
  (Extended_Burst_Length) gets it. The burst overhead length was cut at 63
  bytes, though its field is 8 bits, and a ranged length past 255 wrapped
  to a few bytes; it is now cut at 252, the largest the stock driver
  accepts. A cut preamble can keep the OLT from locking on the bursts,
  so the stick never ranges. Neither test line asks for that much (57 is
  the most seen); host-tested with long counts.

- kernel: the boot log names what the GPON tables held before the clean:
  the first eight GEM ports and Alloc-IDs found valid, after the counts
  (`GPON tables before the clean held GEM ports ...; Alloc-IDs ...`).
  A previous session's own entries (a warm boot) and power-up garbage
  used to look the same, as counts only (#29).

## v1.3.0 — 2026-10-12

- confd v1.2.1 (was v1.2.0): the Config page no longer shows `NTP_SERVER`
  and `SYSLOG_SERVER` empty after a reload on this image (odi-ui #14).

- metricsd v1.3.0 (was v1.2.1): exports the GPON link counters the driver
  now accumulates (`gpon_ds_fec_codewords_total`, `gpon_ds_bip_error_*`,
  `gpon_ds_bwmap_allocations_total`, `gpon_alarm_events_total`,
  `gpon_alarm_seconds_total`, `gpon_onu_state_*`, `gpon_fec_active` and
  more; odi-sfp-exporter docs/METRICS.md, "The GPON link").

- kernel: the GPON link counters. The GPON MAC counts the line in 43
  counters (BIP errors, FEC corrected bits, bytes and codewords and the
  uncorrectable ones, PLend, BWmap, PLOAMd and GEM errors, the BWmap
  allocations the OLT grants this ONU, idle and non-idle GEM frames,
  downstream Ethernet, FCS errors and OMCI, upstream bursts, PLOAMu, DBRu,
  GEM frames and bytes), and every one clears when it is read, so any
  reader takes counts from every other. The driver is now their one
  reader: it samples them once a second, from the boot `gponact` on, into
  64-bit totals, and counts a sample that read a counter full. The same
  sample tracks FEC in use per direction and the superframe count; LOS,
  LOF and LOM events and seconds come from every alarm reading, the
  100 ms line poll's included; and the driver counts time in each ONU
  state and every state transition. All of it is in `/proc/odi_gpon`,
  new lines between `ranging_time` and `ploam_ring` (`docs/TOOLS.md`,
  "/proc/odi_gpon: the link counters"); the first odi-sfp-exporter
  release after v1.2.1 exports it. A weak fibre shows as FEC corrections
  and BIP errors climbing; an ONU stuck at O5 after the OLT dropped it, as
  BWmap allocations standing still. The REI carries the BIP error blocks
  counted in O5 since the previous REI, from the same samples. It used to
  read the register itself: the same count, except that the first REI
  after O5 carried everything since the register was last read.
  Host-tested against a clear-on-read register model
  (`test/odi_gpon_ctr_test.c`).

- kernel: a stuck stick no longer takes the host NIC down with PAUSE
  frames. When the upstream stopped draining (no grants from the OLT, or a
  scrambled Alloc-ID CAM), the host port sent XOFF after XOFF to the host,
  as the stock firmware does: it negotiates PAUSE with the host. An Intel
  10G NIC behind it obeyed, stalled its transmit queue past the 5 s
  watchdog and reset itself in a loop, 103,979 and 5,890,332 XOFF frames
  received on the two ports. The platform settings now force the port's
  transmit PAUSE off (`PORT_FORCE_ABILITY(0)` and `PORT_FORCE_SELECT(0)`
  bit 6), and cmd 32, which rewrote that select word with 0 at every
  provisioning, keeps the bit; receiving PAUSE stays negotiated and the
  advertisement is unchanged. The switch then drops at its own buffers, at
  the same thresholds where it used to pause. The kernel parameter
  `odi_switch.host_pause=1` (command line, or live through
  `/sys/module/odi_switch/parameters/host_pause`) restores the stock
  behaviour. `docs/SWITCH.md`, "Host port PAUSE". Verified on a stick: no
  PAUSE toward the host through a fibre pull.

- kernel: after a power cut the stick reached O5 and the OLT deactivated
  it over and over, with OMCI never starting, until one boot of the stock
  firmware; a warm reboot of this image did not help (#42, very likely
  #29). The GPON MAC tables are on-chip RAM, and a power-up leaves them
  full of valid-looking garbage: read back on a stick after a power cut,
  120 of the 128 GEM-port CAM rows and all 32 Alloc-ID CAM rows were
  valid with random entries, so the ONU answered bandwidth grants meant
  for other ONUs. The boot replay did clear them, but right after the
  GPON MAC soft reset and without waiting for it to finish, and a block
  still in reset drops those writes. Every boot now waits for the reset
  to complete (bounded, 20000 reads 1 us apart, logged with the reads it
  took), then clears every GEM-port and Alloc-ID CAM row and every
  downstream flow type word and reads every row back, before any row of
  this ONU is programmed and before activation. dmesg has one line with
  what the boot found and one with what it left; a row that survives is
  logged as an error with its entry, and the clean runs once more.
  `docs/SWITCH.md`, "GPON boot init". Verified on a stick across three
  power cuts: 108-125 garbage GEM-port rows and all 32 Alloc-ID rows found
  and cleared in one pass, O5 and service without a stock boot.

- ci: the pull request wall time drops from about 13 minutes to a few. The
  `test` job is split into `lint`, `host`, `diag` and an `omci` matrix with
  one job per OMCI suite (`qemu`, `drv`, `resume`, `events`, `swdl`, `vlan`,
  `reload`), all side by side; every check still runs. `make test-omci`
  still runs all the suites, now through one target each
  (`make test-omci-<suite>`) after `make test-omci-build`, which builds only
  omcli and omcid. The qemu-test.sh and events-test.sh harnesses used
  `kill %1`, which a non-interactive `sh` rejects ("No such process"), so
  the daemons were never killed: each `wait` sat out the daemon idle exit
  (`-w`), 4.5 minutes in qemu-test.sh alone. They now kill the pid they
  started; as a result the respawn check really does `kill -9` the daemon,
  which it did not before. shellcheck is taken from the runner image.

- gpon: a loss of signal or of framing now drives the ONU state machine,
  as G.984.3 Table 10-1 has it. In O5 the ONU enters O6 (POPUP), forces
  the PON port link down and starts TO2 (100 ms); a directed POPUP brings
  it back to O5 with everything it had, and with none TO2 takes it to O1,
  where the light coming back takes it to O2 and the OLT ranges it again.
  In O2, O3 and O4 a loss goes straight to O1. Before, the driver only
  logged `event=los` and stayed in O5: on ISP2 a fibre pull of a few
  seconds left the stick claiming O5 while the OLT had deregistered it
  (no bandwidth granted, OMCI and PPPoE dead) until a reboot. The line is
  read every 100 ms while the ONU is activated, as well as at every
  interrupt, since a pulled fibre stops the downstream traffic that raises
  them; `event=lof` lines join `event=los`, and the transitions say
  `cause=los`, `lof`, `to2_expired` or `los_cleared` (`docs/TOOLS.md`).
  Refs #42, #29.

- gpon: giving up the ONU-ID now gives up what was tied to it, as the
  gpondeact verb already did: every Alloc-ID CAM row this activation
  wrote, the default one included, and the OMCC GEM port with its flow
  type, upstream map and stream-valid gate. That is TO2 expiry, a loss in
  O4, TO1 expiry in O4 and Disable_Serial_Number from O4 to O6 (both kept
  their ONU-ID until now), and Deactivate_ONU-ID (G.984.3 9.2.3.5).
  Before, those rows stayed valid into the next ranging, and an Alloc-ID
  the OLT did not assign again stayed in the CAM, answering grants that
  could be another ONU's.
  `/proc/odi_gpon` lists no Alloc-ID until the OLT assigns them again,
  which omcid already follows (`event=alloc_ids ids=none`).

- gpon: TO1 and TO2 run from the state entry. Every interrupt used to
  restart them, so with PLOAM traffic flowing TO1 never expired in O3 or
  O4; TO1 now bounds O3 and O4 together, 10 s, as G.984.3 10.2.1 says.

**Automated dependency bump.**
- linux 6.18.55 -> 6.18.56

- diag-bundle: the bundle collects the optics module under `module/`, to
  tell which laser driver a stick has and what state its calibration is
  in: A0h whole, A2h with the page select as found, and in `chip.txt` the
  page select, the laser driver id (A2h page 2 byte 0xa0, 0x6a for a
  Semtech GN25L95), the UX3320 variant markers (page 2 bytes 0x80 and
  0xdc) and INIT_STATE (page 3 byte 0xf0), with the upper half of pages
  0, 2, 3, 4, 5 and 6. Each page select is read back, since a chip can
  refuse one (a GN25L95 refuses page 3), and a page is dumped only when
  it took. Selecting a page is the one write the bundle makes; the page
  found is written back and read back, also when the bundle is
  interrupted, and `chip.txt` records both. The module serial (A0h bytes
  68-83) is blanked in `a0.txt` and masked in every other file like the
  other identifiers, unless `--full`. Without the I2C bus every module
  step is listed as failed and the bundle still succeeds.

- diag-bundle: a secret value shorter than eight characters is scrubbed
  only where it stands as a whole token, no letter or digit touching it as
  text and no hex digit touching it as hex. Before, every occurrence went,
  so the factory USER_PASSWORD `user` turned `userland_ok` into
  `REDACTEDland_ok` in MANIFEST.txt and dmesg, which both mangled the logs
  and told the reader the password, and its hex could cut into a register
  word. A short LOID gets the same rule, so a numeric one no longer cuts
  digits out of counters. Longer values are scrubbed everywhere, as before.

- kernel: the optics I2C port is an i2c adapter, `/dev/i2c-0`, so the
  module's own devices (A0h = 0x50, A2h = 0x51, the laser driver chip) can
  be read with busybox `i2cget` and `i2cdump`, to tell which laser driver a
  stick has and what state it is in. Reading them used to mean driving the
  I2C master by hand with `diag register set`/`get`, with the exporter,
  omcid and confd paused. The adapter shares the one lock and the one byte
  routine with the DDM reads, so neither has to stop for the other. The
  controller does one byte behind a byte address per transaction, so the
  adapter offers SMBus byte-data and I2C block reads (no `i2cdetect`), of
  0x50 and 0x51 only: the controller does not report an absent device,
  and a read of one returns the previous byte read. The only write
  accepted is the A2h page select (byte 127, `i2cset -y 0 0x51 0x7f
  <page>`), to read the paged tables; any other write is refused with
  EPERM before it reaches the bus. `docs/TOOLS.md` has the commands. The
  kernel image is 7.6 KB larger (the i2c core and i2c-dev).

- diag-bundle: the bundle collects the GPON and OMCI state under `pon/`
  (ONU state, alarms, GEM flows, port counters, the PON queue registers
  cmd 23 and 25 write, `/proc/odi_gpon`, `/proc/odi_omci`, and `omcli`
  state, provision, flows, T-CONTs, connections and the datapath MIB
  classes), what issue #42 had to ask for by hand. It now masks the serial
  number, the MACs and the LOID in every file and spelling, to `MASKED-`
  and their last four characters: many OLTs authenticate by serial number
  alone, so a bundle posted publicly could clone the line. `--full` keeps
  them for a bundle shared privately; the web UI never passes it. Secrets
  are also scrubbed as space-separated hex, and the PLOAM ring withholds
  the message bodies that carry the password, a key fragment or the
  serial. A bug report issue template asks for the bundle.

- switch: an upstream GEM flow is mapped to the queue of its own T-CONT
  (`PON_SID2QID`, 7 bits a flow), and only its entry changes. cmd 25
  used to replay the five words captured on ISP1 by flow id modulo 5, so
  on an ISP with six upstream flows (MTS, issue #42) the sixth flow
  replayed flow 0's word and unmapped flows 1-3: the ONU reached O5 and
  OMCI completed, but the service on flow 2 had no upstream queue and DHCP
  failed. A flow on a queue cmd 23 does not program (any but queue 0 of
  T-CONTs 0-15) is refused and logged. The global PBO thresholds
  (`PON_SID_GLB_TH`) follow the number of flows in use and keep the fifth
  captured pair past five. ISP1's five flows get the same register words
  as before (boot5 golden).
- nv: `nv commit 1` works on a stick running slot 1. It read the running
  slot from the first `root=` in `/proc/cmdline`, but that one is the
  built-in slot-0 line (`root=31:5`); U-Boot's per-slot `root=` comes after
  it and is the one the kernel mounts. So on slot 1 it always refused, with
  "running slot is 0", and wrote nothing. It now takes the last `root=`,
  tested on the host with both lines (issue #42). `slot-state.sh`,
  `fwu_starter.sh`, `fwu.sh` and the web UI read the slot correctly and are
  unchanged; `test/fwu_guard_test.sh` gains the two-`root=` case.
- kernel: the idle loop stops the CPU with the Lexra SLEEP instruction
  until the next interrupt, where it used to spin at full speed (the
  R3000 model has no wait instruction). On ISP1 the module runs about
  1.9 C cooler relative to the ISP2 stick, and idle ping rises from 0.28 to
  0.33 ms. The tick is periodic again (`CONFIG_NO_HZ_IDLE` off, high-
  resolution timers kept): tickless idle measured no gain, and the periodic
  tick bounds a wake-up missed just before SLEEP at 4 ms. The kernel image
  is 3.8 KB smaller.
- build: `kernel/tree.sh` fetches the kernel again when
  `kernel/618/mainline` holds a different release than the one pinned in
  `kernel/618/fetch.sh`, and rebuilds the tree tarball unless its new stamp
  (`build/kernel-618/tree.tar.version`) names the pinned release; the stamp
  is also part of `kernel/build.sh`'s incremental-build check, so the build
  volume re-extracts its sources. Before, a checkout or build volume that
  already had a tree kept building the old kernel after a pin bump, with no
  warning: a remote build after the 6.18.55 bump still produced 6.18.54,
  even after the tree was fetched again. Release builds start clean and were
  not affected.
- kernel: TIMER0 gets a oneshot mode (counter mode, 3.125 MHz, 28-bit delta)
  next to the periodic tick, and `CONFIG_HIGH_RES_TIMERS` is on, so the
  kernel runs high-resolution timers (1 ns resolution in `/proc/timer_list`
  on ISP1). Oneshot is opt-in at boot: a short self-test
  times a programmed event against TIMER1, and if it does not fire the
  kernel prints one warning and stays on the 250 Hz periodic tick exactly as
  before. A reprogram never touches a running timer (stop, clear, write,
  enable), and an event that may have been lost returns `-ETIME`. On ISP1
  the self-test passed and the watchdog still resets about 60 s after
  omcid stops. New host test `test/rtl8686_time_test.sh`.
- kernel: TIMER1 is registered as a free-running clocksource
  (`rtl8686-timer1`, 28 bits at 3.125 MHz) and as `sched_clock`, so
  `ktime_get()` and scheduler timestamps have sub-tick resolution instead of
  the 4 ms jiffies step. The TIMER0 tick is unchanged. If TIMER1 does not
  count at boot, one warning is printed and the kernel keeps the jiffies
  clocksource. On ISP1 it is the current clocksource and kept time exactly
  over 61 s across a counter wrap, with no timekeeping warnings.
  `rtl8686_cpu_hz` is renamed `rtl8686_lx_hz`: it is the LX bus clock, not
  the CPU clock.
- nic: TX descriptors are reclaimed from the TX-completion interrupt (IMR/ISR
  bit 6) through the NAPI poll, where before nothing but RX traffic, the next
  transmit or the 10 ms stall work reclaimed them. A completion that has
  descriptors to reclaim counts as work for the interrupt storm guard, so TX
  load cannot trip it. A new 100 ms backstop reclaims while frames are in
  flight, so an interrupt that does not fire degrades to the old behaviour,
  never to a TX stall; the IO_CMD value is unchanged. `/proc/odi_nic` (new)
  shows `tok_irqs`, `tok_work`, `tok_idle` and `backstop_reclaims`, and the
  same counters join the debug dump and the pre-reset line. On ISP1 the
  interrupt fires with the stock IO_CMD (147 of the first 161 entries had
  work, 5 backstop reclaims), and four parallel ping floods plus a 30 MB
  transfer ran with no loss, no pause frames and no storm trip. A TOK that finds
  nothing to reclaim because the transmit path took the descriptor first (a
  busy flood, about 2300 such entries per 2 s on one stream) does not count
  toward the storm guard while frames keep being queued; a TOK that re-fires
  with no TX activity still does. Host test
  `odi_nic_hw_test` covers the work rule and the guard.
- docs: the NIC, GPIO and I2C register fields that were decoded by
  cross-checking against an independent driver for a later chip of the same
  family now have names, in the code. No value written changes: the I2C setup
  words are built from the named fields and asserted equal to the literals
  they replace, and the kernel objects of every odi driver are byte-identical
  to the previous build. NIC 0x58 is the MAC status register (flow-control
  controls at bits 7:5, speed and link status below; it reads 0xf0 on a
  running stick); 0x1370 is a priority-to-ring route register, not one word
  per ring; GPIO 0x3324/0x3328 are bank 1 direction and data; 0x3264 is the
  watchdog interrupt/status register; the I2C setup word has clock divider,
  widths and a 7-bit device address. The VLAN row layout is left as it was; a
  note says the stock default row words are not decoded beyond the nibbles
  our code uses.
- release: a tag with a hyphen after the version (`v1.2.2-beta.1`) is
  published as a GitHub pre-release, not marked latest, with the
  `## Unreleased` changelog section as its notes under a test-build warning;
  AGENTS.md describes it next to the release checklist. Lets a reporter test
  a fix without a full release.
- gpon: the equalization delay follows G.984.3 more closely (issue 29, an
  OLT that deactivates the ONU a few milliseconds after O5). Three changes,
  none of which alters the registers written on the two test lines:
  - the DELAY_HI term is added to the delay before it is split into FRAMES and
    INFRAME, so INFRAME never reaches a whole frame; before, a delay whose
    remainder was 118400 bits or more (about a quarter of all fibre lengths)
    left INFRAME above a frame and FRAMES one short;
  - a Ranging_Time with the protection path bit set is ignored, in O4 and in
    the repeats read in O5, instead of being applied as the main path delay;
  - an Upstream_Overhead that enables the pre-assigned delay now has it
    written to USF_EQ_DELAY before ranging (units of 32 bytes), where it was
    discarded and 0 written.
  New host test `test/odi_gpon_eqd_test.sh` covers the split (a no-carry case
  identical to the old result, and carry cases), the pre-assigned delay and
  the protection path rule.
- gpon: `/proc/odi_gpon` shows the PLOAM content, for debugging an OLT that
  deactivates the ONU right after O5 (issue 29). Each `ploam_ring` entry now
  carries the ONU-ID and the 10 content bytes in hex; two new lines show the
  last Upstream_Overhead decoded (guard, preambles, pattern, delimiter, the
  pre-assigned delay flag and value in 32-byte units and in bits, power level)
  and the last Ranging_Time (EqD bits, protection path bit, the
  USF_MIN_RESP_DELAY value read and the MULTFRAME/INFRAME written). The kernel
  log gets one `event=ranging` line per O4 to O5 transition, on the same
  ratelimit as `event=onu_state`. No behaviour change; the ring grows by
  128 bytes of RAM and the decoded copies take about 50.
  The `ploam_ring` timestamps and `last_los_ms` are now milliseconds since
  boot; they were raw jiffies, which start near 2^32 and wrapped minutes
  after boot.

**Automated dependency bump.**
- linux 6.18.54 -> 6.18.55

- docs: repair text merged into CHANGELOG.md by union-resolved rebases: the
  v1.1.0 section had lost the entries of five changes merged the same
  morning (the ramlog reset reason and `flash get` of an unset key, `nv
  commit` and `slot-state.sh`, the autonomous entities in the MIB dumps, the
  link and provisioning events, `omcli provision`), restored below from their
  commits; a stray blank line inside the v1.2.0 list is removed, and
  test/qemu/run-qemu.sh has its blank line back before the diagnostics scenario.

## v1.2.1 — 2026-09-29

- Pins odi-sfp-exporter v1.2.1 (was v1.2.0): its alert rules no longer count
  the port-3 undersize frames (one per upstream OMCI reply, harmless).
- AGENTS.md has a release checklist: every change since the last tag has an
  entry, no merge debris, `Unreleased` moved into the tag section, and the
  published notes checked (odi-oss also checks its confd/metricsd pins).
- Fixes the NIC sending PAUSE frames toward the switch CPU port (port 3) with
  its receive ring empty: 1-6 per second on both test lines, about 12 a
  minute plus bursts of up to 50 a second, where the stock firmware sends
  none. The register next to the flow-control thresholds is the index of
  the last receive descriptor the CPU handed back, which the hardware
  compares against its own; `odi_nic` wrote it once at init and never
  again, so once per lap of the ring the hardware saw itself within the
  threshold of it. The driver now writes it after every poll, starts it at
  depth - 1, and uses the stock thresholds (assert at 16 descriptors left,
  release at 48) as a margin in descriptors. This replaces the v1.0.2
  rescale of those thresholds, which only shrank the window. The ring
  indices are also reset when the rings are reallocated, to match the
  hardware after its reset. The index is written after every descriptor,
  as the stock does, not once per poll: a poll that took the whole NAPI
  budget (the ring depth) wrote the same byte as the poll before.
  `test/odi_nic_hw_test.c` checks the index against a model of the
  comparator, idle and under a flood.
- Fixes a flood of frames to the stick (`ping -f` at 1400 bytes, ISP1)
  leaving the management path dead until a power cycle: the `odi_nic`
  interrupt-storm guard counted every interrupt, and with NAPI a CPU that
  keeps up takes one interrupt per frame, so 2500 frames a second for 2 s
  masked the NIC and disabled its interrupt for good. It now counts only
  interrupts that schedule no RX work (a status bit that fires again, or
  the line firing while the poll is already scheduled); a line with
  nothing set for us is left to the kernel spurious-interrupt detector.
- New watchdog rule, CPU-port RX: `odi_wdt` resets the board when the switch
  keeps offering frames to the CPU port (port 3 delivered, discarded or
  held back by our PAUSE, from the switch MIB) for 30 s while the NIC takes
  none, logging one line of NIC state first; the ramlog reason is
  `wdt_cpu_rx`. An idle CPU port never counts. A trial that wedges the
  management path now reverts by itself instead of needing a power cycle.
  Host-tested in `test/odi_wdt_test.c`; `docs/SETTINGS.md`, "Watchdog
  rules".

## v1.2.0 — 2026-09-29

- Pins odi-ui confd v1.2.0 (was v1.1.1): light/dark toggle, equal readout
  sizes, the VLAN keys as LIVE, rows for ONU_HW_VERSION, OLT_SW_DOWNLOAD and
  OMCI_UNKNOWN_ME_OK, and every setting and risky option value saying whether
  it disrupts the link.
- Upstream OMCI and upstream data survive a second provisioning in one boot.
  The kernel placed each upstream priority queue (driver command 23) by a
  count of the calls since boot and ignored the direction and T-CONT in its
  argument, so the second ISP1 provisioning (an omcid identity reload, a
  respawned omcid, any re-registration or QoS rebuild) wrote T-CONTs 5 to
  17: it overwrote the scheduler word of the OMCC T-CONT and cleared the
  OMCC and the five data T-CONTs from the T-CONT enable word. The ONU still
  ranged to O5, but the OLT got no OMCI answer and deactivated it every
  30 s until a reboot (seen on ISP1 at the second identity reload). A
  downstream queue now writes only its port queue map, an upstream one the
  words of the T-CONT it names, T-CONTs 0 to 15 and one queue per T-CONT
  (the captured shape; anything else is refused and logged instead of
  landing on another T-CONT). `test/odi_reregister_test.sh` provisions ISP1
  three times in one boot, with two deactivations and an OLT deallocation,
  and requires the OMCC path and every upstream queue word to be what the
  first provisioning left.
- CHANGELOG.md: three stray merge-conflict marker lines removed.
- `apply.sh omci` no longer stops omcid and starts its own beside the one
  init respawns. That was the bug: every respawn of `svc-omcid.sh` found the
  stray omcid holding redirect type 1, exited 1, and its re-provisioning path
  ran `gpondeact`/`gponact` -- 44 exits in about two minutes on hardware, the
  ONU cycling O1 to O3 and never reaching O5 until the stray was killed. The
  script now sends SIGHUP to the running omcid (`pidof`), waits, bounded, for
  the outcome in `/var/run/omcid-reload` and reports it (`apply.sh omci
  [seconds]`, 20 s by default, inside the web UI's 25 s); it exits 1 when no
  omcid runs or none answers, and never starts a process. Init owns omcid; a
  genuine crash still respawns and re-provisions as before.
- omcid reloads on SIGHUP (the Unix convention for rereading configuration)
  instead of being restarted: it rereads the store (`lastgood*.xml`,
  `odi.conf`, `omci-identity.on`) and compares it with what it runs. Nothing
  changed: logged, nothing done. Only `VLAN_CFG_TYPE`, `VLAN_MANU_MODE`,
  `VLAN_MANU_TAG_VID` or `VLAN_MANU_TAG_PRI`: the bridge connections and
  VLAN rules are torn down and built again in place from the MIB it holds,
  by the same code as the first build, with the ONU staying in O5; the rules
  are byte for byte a fresh omcid's (`src/omci/reload-test.sh` compares both
  ISP sessions, with a new tag and with transparent mode). `GPON_SN`,
  `GPON_PLOAM_PASSWD`, the LOID keys, `OMCI_SW_VER1/2`, `GPON_ONU_MODEL`,
  `OMCC_VER`, `OMCI_VENDOR_PRODUCT_CODE`, `ONU_HW_VERSION`, the identity
  switch or `OMCI_UNKNOWN_ME_OK`: omcid itself writes `gpondeact`, clears its
  MIB as a MIB reset does, holds three seconds, gives the driver the new
  serial number and password (`gponsn`, `gponpw`), writes `gponact` and lets
  the OLT provision it again, in the same process (no respawn, no snapshot
  resume). The re-registration is a state machine in the main loop: nothing
  sleeps, the watchdog ping and the CLI keep being served, a second SIGHUP
  during it folds into one more reload, and it gives up after 150 s. New
  lines `event=reload`, `event=reload_step` and `event=reload_done` (never
  rate limited; key names, never values) and `/var/run/omcid-reload` say
  what happened. `omcid -i file -j seconds` are test flags.
- The kernel takes a new serial number after boot: `gponsn` while the ONU is
  deactivated now rewrites the PLOAM slot the boot activation armed (the
  serial-number slot of `gpon_init.bin`, found by its `sn_word` records)
  instead of only the driver's copy, which the OLT would then have failed to
  match on the next Assign_ONU-ID. In any other state it answers `-EBUSY` and
  changes nothing. Before, a new `GPON_SN` needed a reboot.
- The four VLAN keys are LIVE now: they apply through `apply.sh omci` without
  dropping the fibre service (odi-ui schema moves with this).
- omcid takes the T-CONT Alloc-IDs from the OLT instead of a table captured
  on one ISP1 session. A T-CONT the OLT sets over OMCI keeps what it set
  (G.988 9.2.2, as ISP2 does); one it never sets (ISP1 sets none) is bound to
  the next Alloc-ID the OLT assigned by PLOAM, in assignment order, from
  `alloc_ids` in `/proc/odi_gpon`, and reads 0x00FF (unassigned, not
  programmed) when there is none. A MIB upload and a Get of the AllocID
  answer the same. A change in the PLOAM list reprograms the T-CONTs bound
  to it, and logs `event=alloc_ids`. Before, an OLT assigning any other
  Alloc-IDs got ISP1's reported and sent to the driver. `omcli tcont` shows
  each T-CONT's Alloc-ID and its source; `omcid -g` reads a file in place
  of `/proc/odi_gpon` for the tests.
- omcid no longer treats a deleted or MIB-reset row as live: the store walk
  behind the upstream QoS and bridge-connection rebuilds handed out free
  slots, which keep the class and data of their last row. After one OLT
  session and a MIB reset, the next rebuild programmed every GEM port and
  T-CONT of the old session again (seen under qemu: the ISP1 session, then
  the ISP2 one, into one omcid).
- The kernel releases an Alloc-ID the OLT deallocates (Assign_Alloc-ID type
  255, G.984.3 9.2.3.9): its CAM row is deleted and freed for the next
  assignment, and it leaves `alloc_ids`. Before, a deallocation was stored as
  one more assignment.
- omcid logs what it does not model, once per boot: `event=unknown_me
  class=<n> op=<create|set|get|delete|get_next|test>` for a managed entity
  class it has no model for and `event=unknown_msg type=<n> class=<n>` for a
  message type it does not handle, each first sighting one syslog line (the
  usual 20-a-minute event limit applies), every sighting counted in
  `/var/log/omcid-unknown.txt`, which `diag-bundle.sh` collects as
  `odi-diag/log/omcid-unknown.txt`. A respawned omcid reads the file back
  and does not log the same thing twice in one boot. The ISP1 OLT session
  already shows three: class 351, and message types 17 and 1 sent to ONU-G.
- New odi-only key `OMCI_UNKNOWN_ME_OK` (`/etc/config/odi.conf`, default
  off): `1` answers a Create, Set or Get of a class omcid does not model with
  success instead of "unknown entity", the counterpart of the stock
  `OMCI_FAKE_OK`, for an OLT that stalls on the error. Nothing is stored, a
  Get answers no attributes, MIB data sync counts the faked writes. Read at
  omcid start: INTERRUPTS INTERNET (`apply.sh omci`). `docs/SETTINGS.md` has
  the risk.
- omcid no longer counts a refused Set (an unknown class) in MIB data sync:
  the OLT counts only the writes it was told succeeded.
- omcid accepts a software download from the OLT instead of refusing it, and
  never installs it: Start, Download section (acknowledged per G.988 window,
  a window with a missing section refused so the OLT resends it), End (the
  image CRC-32 and size checked), Activate and Commit are answered with
  success, the image is counted and discarded, and the software image entity
  reports the flags the OLT expects (`is_valid`, `is_active`,
  `is_committed`) for the rest of the boot. Nothing is written to flash or the
  U-Boot environment and the stick never reboots. A new odi-only key,
  `OLT_SW_DOWNLOAD` in `/etc/config/odi.conf`: `accept` (the default) or
  `reject` (the old "not supported" answers), read at every download. Every
  step is an `event=sw_image` line with the sections and `crc=ok|bad`. ISP2's
  End software download to ONU-G at every session start is no longer logged
  as a software image step. `make test-omci` runs both modes, under
  `qemu -strace` for accept, and asserts no flash, exec or reboot.
- Transparent VLAN handling, documented as a choice (`docs/SETTINGS.md`,
  "VLAN handling"): `VLAN_MANU_MODE` 0 is "router tags" -- the stick adds
  and removes no tag and every frame passes with its own tags, so one port
  can carry internet, IPTV and voice as router subinterfaces; 1 is "stick
  tags", the manual `VLAN_MANU_TAG_VID`. No new key: 0 is the stock
  firmware's own "no manual tag", so both slots agree. On a line without
  class 84 (ISP2) transparent builds forward-all rules, which now also put
  the UNI and PON on every VLAN row; on a class 84 line (ISP1, FwdOp 0x10)
  the listed VIDs pass tagged and untagged frames are discarded, as the OLT
  provisions. `omcli vlan` prints the mode (`handling` line) and omcid says
  `manual vlan off (transparent)` at start. `src/omci/vlan-test.sh` (in
  `make test-omci`) builds both ISP sessions in both modes.
- omcid reads the class 84 forward operation (FwdOp, G.988 table 9.3.11-1)
  and every entry of its VLAN filter list, up to twelve. Before, it built one
  VID filter from the first entry whatever the code, which is only right for
  0x10 (the mandatory code; ISP1 sends it with one entry per port, so
  nothing changes there) and dropped every VLAN after the first on a
  multi-VLAN port. Now: 0x00 forwards everything, 0x01 untagged only,
  0x02/0x15 tagged frames of any VID, and the positive filters -- by VID
  (0x03/0x04/0x0f/0x10/0x1c/0x1d), priority (0x07/0x08/0x11/0x12/0x1e/0x1f) or
  both (0x0b/0x0c/0x13/0x14/0x20/0x21) -- one rule per entry, plus an
  untagged rule for the codes that bridge untagged frames (the manual
  add-tag rule when the manual tag is on). Negative filtering (0x05/0x06/
  0x09/0x0a/0x0d/0x0e), filtering by TCI and MAC address (0x16-0x1b) and
  codes past the table are built as 0x10 and logged once per code as
  `event=vlan_fwdop code=0x.. inst=.. result=unsupported built_as=0x10`.
  Every code has a check in `make test-omci`; the ISP1 and ISP2 driver-call
  goldens are unchanged.
- The switch driver gives a bridge rule that passes tagged frames of any VID
  (forward-all, tagged-only, a priority filter) VLAN table membership on
  every row 2..4094 (UNI and PON, tagged), and takes it off every row when
  the rule is released. Those rules have no VID of their own to put a row
  on, and the rows the cmd 51 sweep leaves are 0; a VID filter keeps its own
  row as before. No stock capture has such a rule, so no golden moves;
  `test/odi_switch_bdgconn_vlan_test.sh` pins the rows.

## v1.1.1 — 2026-09-29

- Pins odi-ui confd v1.1.1 (was v1.1.0): the trial banner no longer claims
  what the other partition holds; "Keep" is "Commit" everywhere, with a
  "What committing means" note under the partitions; a smaller receive
  readout; forwarding rates to 2 decimals; the T-CONT card counts only
  assigned Alloc-IDs.
- The README web UI screenshot now links odi-ui (single source) instead of
  a copy in docs/images.

## v1.1.0 — 2026-09-29

- Pins odi-ui confd v1.1.0 (was v1.0.8): the redesigned web UI (Status,
  Config with subtabs, OMCI, System), a trial-boot banner with "Keep this
  image", `GET /api/diag` for the diagnostics bundle. Pins odi-sfp-exporter
  v1.2.0 (was v1.1.2): `gpon_boot_count`, `gpon_last_reset_reason`,
  `gpon_config_info`, `gpon_uncommitted` and the slot series,
  `gpon_provision_*`, and the alert rules in its `prometheus/alerts.yml`.
- The kernel records why each boot ended, and the next boot of this image
  shows it: `/proc/odi_ramlog_prev` now ends its `previous boot:` line with
  `reason=` -- `wdt_client:<name>`, `wdt_mem`, `wdt_userland` (written by
  `odi_wdt` just before it forces the reset), `reboot`, `halt`, `poweroff`
  (a reboot notifier), `panic` (a panic notifier), `oops` (a die notifier,
  kernel mode only), `power` when neither ramlog page survived (DRAM lost: a
  power cycle), and `unknown` when the pages survived but nothing recorded a
  reason (a hang the hardware watchdog caught). The existing fields and
  lines are unchanged. It lives in a new 32-byte reason block in page A
  just before the metadata block, which keeps its offset (format 2), so page
  A now keeps the first 3984 bytes of the log, not 4016.
  `tools/memprobe/ramlog-read.sh` decodes it; host tests cover the block,
  the render and the watchdog rule chosen when several fire at once.
- `flash get` on an odi-only key that holds no value (`SYSLOG_SERVER`,
  `NTP_SERVER`) now prints `KEY=` and exits 0, like a stock key present with
  an empty value, instead of `GET fail.` and 1. A name that is no key at all
  still fails. `svc-syslogd.sh` and `svc-ntpd.sh` read both the same way.
- `METRICSD_BIN=<binary>` puts a local odi-sfp-exporter build in the image
  instead of the pinned release (`src/fetch-releases.sh`), for trials of an
  unreleased exporter.
- docs/TOOLS.md lists the exporter metrics `gpon_boot_count` and
  `gpon_last_reset_reason{reason,client}`, which read the reason above
  (odi-sfp-exporter, after v1.1.2).
- New `nv commit <slot>`, now the documented way to commit a trial by hand
  once you have checked it (`docs/FLASHING.md`, "Committing"): `sw_commit`
  into both U-Boot environment copies, the primary first and read back byte
  for byte, only then the fallback, then both re-read. Refuses a slot that
  is not running (`root=` in `/proc/cmdline`) or not `sw_active`, and an
  environment without two valid copies; never writes `sw_active`.
  Host-tested against a fake flash that fails at every step
  (`src/nv/test/test_commit.c`, now part of `make test-host` with the
  existing `nv` block tests).
- An uncommitted image now says so. `/etc/scripts/slot-state.sh`, a new
  `/etc/inittab` `once` entry, reads the U-Boot environment at boot (never
  writes it) and records the running slot (`root=` in `/proc/cmdline`),
  `sw_active`, `sw_tryactive`, both copies of `sw_commit`, the next boot
  slot and `uncommitted` (1 when either copy names another slot) in
  `/var/run/odi-slot`, a `KEY=value` file for the exporter and the web UI
  (format in `docs/TOOLS.md`, "Slot state"). While uncommitted it puts a
  `TRIAL BOOT` (or `HALF COMMITTED`, naming the copy that disagrees)
  notice with the exact `nv commit` command in `/etc/motd`, which dropbear
  prints at every interactive login, and in syslog. Nothing commits a
  trial automatically: the update procedure ends with a manual `nv commit`
  after checking the trial (`docs/FLASHING.md`, "Committing"). Host-tested
  against a stub nv (`test/slot_state_test.sh`); test-qemu checks the file
  and the login banner under the real busybox and dropbear.
- Fixes `omcicli mib get <class>` answering `0 rows` for every entity the
  ONU creates for itself (ONT data, SWImage, ONT2-G, the T-CONTs, ANI-G, the
  traffic schedulers, ...), found on ISP1: the dump walked only the store
  rows, which exist for an autonomous entity only once the OLT sets one of
  its attributes, while a MIB upload listed all 301. So the web UI showed
  an empty software version and no T-CONTs. `omcicli mib get`, `omcli mib`
  and `omcicli mib getattr` now cover every entity, autonomous and
  OLT-created, sorted by class and instance, with the values a Get returns
  (what the OLT wrote, else the built-in answer), not the zeroes of an
  unwritten row. Every dump now has one shape: the vendor-rendered classes
  (84, 131, 171, 256) print their banner and blocks as before, every other
  class prints omcid's own block, and every answer, empty or not, ends with
  one `N rows` line (the vendor-rendered ones had none, so an empty answer
  printed nothing at all). A name that is no class (`mib get Foo`) is
  refused instead of dumping the whole MIB. test-omci covers an autonomous
  class, the vendor-rendered and the empty shapes, and `getattr`.
- **Link and provisioning events in syslog: after an outage the log says
  who started it.** One greppable `event=` line per event, `key=value`
  after it (docs/TOOLS.md, "Link and provisioning events", has the table
  and how to read an outage with it). From the GPON driver, as printk
  through klogd: every ONU state change (`event=onu_state from=O5 to=O2
  in_state_s=... cause=deactivate_onu_id side=olt`), where `side` is `olt`
  (a PLOAM message: deactivate, disable serial number, ranging), `timer`
  (TO1/TO2), `line` or `local` (gponact/gpondeact), and the downstream LOS
  bit going on and off (`event=los`), sampled at every interrupt and every
  BER interval, so a fibre pull in O5 shows within 10 s. From omcid, to
  `/dev/log` (facility daemon, the syslog(3) datagram; omcid has no libc)
  and to omcid.log: its start (`run=boot|restart`, `mode=resume` or
  `reregister` and why), an OLT or CLI MIB reset, the MIB upload, the first
  and last write of a provisioning burst with its totals (`provision_end
  creates=82 sets=102 ...` for the ISP1 session), and the reboot and
  software-download requests it refuses. Rate limited on both sides (30
  and 20 lines a minute, each saying what it dropped); nothing per
  message. The old rate-limited `odi_gpon: state O4 -> O5 (onu_id 3)`
  line is replaced by `event=onu_state`. test-host covers the cause
  mapping, test-omci (`events-test.sh`) every omcid line against the ISP1
  session, and test-qemu that kernel messages and omcid lines reach
  `logread`.
- New `omcli provision` and `alloc_ids` in `/proc/odi_gpon`, for the
  exporter's `gpon_provision_*` metrics, so an ISP plan change (speed tier,
  VLAN, T-CONT) shows on a graph. `omcli provision` prints one line per T-CONT
  omcid programmed, GEM port network CTP, VLAN (from the VLAN tagging filter
  data and the extended VLAN tagging table, matched or set, sorted by VID) and
  traffic descriptor, and a summary line. `/proc/odi_gpon` gains `alloc_ids`:
  the Alloc-IDs the OLT assigned by Assign_Alloc-ID, the T-CONT list the
  exporter uses. omcid's own T-CONT Alloc-IDs can differ from it: for an OLT
  that never sets them over OMCI (ISP1), a Get answered the value captured from
  the stock stack. `METRICSD_TAG= METRICSD_BIN=<binary>` builds an image with a
  local exporter, as `CONFD_BIN` does for confd. `events-test.sh` checks the
  provision answer for the ISP1 session.
- Adds `/etc/scripts/diag-bundle.sh`, the diagnostics bundle: the previous
  boot ramlog, `/var/log/*`, dmesg, `/etc/odi-build`, `/etc/version`,
  `/proc/odi_wdt/*`, uptime, meminfo, mounts, `ps`, the slot variables from
  both environment copies, an exporter scrape and the config store, in one
  tar.gz. Every password is redacted in the config copy (a fixed key list
  plus any key named like a secret, `docs/TOOLS.md`) and each secret value is
  then scrubbed from every file in the bundle, as text and hex. Busybox only;
  every step under `timeout`, logs capped at 1 MB, the bundle at 2 MB. The web
  UI serves it as `GET /api/diag` from the next odi-ui release; this image
  still pins confd v1.0.8. `make test-qemu` plants secrets and asserts none
  of them reaches the bundle.
- Adds `tools/config-backup.sh`: a host-side (POSIX sh, Linux and macOS)
  backup of the config store through the web UI `GET /api/backup`, keeping a
  dated copy only when a setting changed and the newest `ODI_KEEP` (30), the
  credential from a file or the environment, every download bounded by
  `curl -m`, non-zero exit on failure. `docs/SETTINGS.md`, "Backing up from a
  host", has cron and systemd-timer examples.

## v1.0.8 — 2026-09-28

- Fixes `SYSLOG_SERVER` and `NTP_SERVER` never saving from the web UI on a real
  stick (`did not stick, device holds ''`; `flash get` printed `GET fail.`). Both
  are keys the stock firmware never had, and `flash` only edited keys already in
  `lastgood*.xml`. They now live in `/etc/config/odi.conf`, a plain `KEY=value`
  file written by temp file and rename; `flash set/get`, `flash all cs` and
  `svc-syslogd.sh` / `svc-ntpd.sh` use it for those names, and an empty value
  clears the key. Stock keys and the XML are untouched, and the stock image
  ignores the file. test-qemu now runs the real `flash` against a writable
  config dir (it used to seed a fixture XML) and asserts a UI save of
  `SYSLOG_SERVER` reaches `syslogd -R`.
- Pins odi-ui confd v1.0.8 (was v1.0.7): the Config page can clear
  `SYSLOG_SERVER` and `NTP_SERVER` (an empty value removes the key).

## v1.0.7 — 2026-09-28

- Pins odi-ui confd v1.0.7 (was v1.0.6): `SYSLOG_SERVER` and `NTP_SERVER` are
  now set from the web UI (Config, other). Saving one runs `apply.sh syslog` /
  `apply.sh ntp`, so syslogd or ntpd restarts under init respawn with the new
  value; no reboot, fibre service untouched. confd refuses empty values, so
  clearing either key still needs a shell.

## v1.0.6 — 2026-09-28

Same changes as the v1.0.5 tag, which was never released: its release run
failed at the SBOM attestation because the CycloneDX SBOM had no
`serialNumber`, which the attestation action requires. `tools/generate-sbom.sh`
now emits one, derived from the version and the commit so the SBOM stays
reproducible.

**A respawned omcid resumes instead of getting re-provisioned: zero
outage, not eighteen seconds.** v1.0.4's `omci-respawn-reprovision.sh` made
a respawn recover, but through a visible re-range (O5 -> O1 -> O5).
`omcid` now snapshots its whole MIB -- every managed entity, the MIB data
sync counter, and the bookkeeping that maps an entity to what is actually
programmed in the switch (GEM flow ids, T-CONT map, broadcast flow,
service table) -- to `/var/run/omcid-mib.snap` after every Create, Set or
Delete, and again after the quiet-second `us_qos_rebuild()`/`bdgconn_rebuild()`
that actually fills the flow, T-CONT and service-table bookkeeping (the
message handler alone snapshots too early to catch it), written atomically
with a version header and a CRC32. A respawned
`omcid` whose PON is still O5 and whose snapshot names this same device
loads it by plain memory copy and resumes answering -- no re-registration,
and not one switch-programming driver call, because the datapath was
never touched. `omci-respawn-reprovision.sh` waits briefly on `omcid`'s own
decision (`/var/run/omcid-resume-decision`) and skips its deactivate/
reactivate sequence entirely on a "resumed" decision; anything else (no
snapshot, a mismatched device, not O5, or a MIB the OLT has since reset --
which deletes the snapshot) falls back to the v1.0.4 path unchanged. See
docs/BOOT.md, "Resume without re-registration", and
`src/omci/resume-test.sh`, the driver-call golden replayed across a
`kill -9` and respawn.

**Release CI: no more double-publish on a tag, plus SBOM and attestations.**
A tag has landed on `release.yml` twice before, the second run failing at
`gh release create` because the first already made the release; the workflow
now serialises same-tag runs (`concurrency:`) and its publish step is
idempotent (edits and re-uploads with `--clobber` if the release already
exists, instead of failing). Every release now also ships a CycloneDX SBOM
(`tools/generate-sbom.sh`, built from the pins already in this tree) and
build-provenance + SBOM attestations for the tarball, its checksums and the
SBOM itself; `docs/FLASHING.md` says how to verify them.

**Reproducibility check, and a real kernel non-determinism it found.** `ci.yml`
gained a `reproducible` job (tags and `workflow_dispatch` only -- a full image
build twice is too slow to run on every PR) that builds the same version in
two clean checkouts, each with its own docker build-cache volume, and diffs
the tarballs member by member (`tools/compare-images.sh`), tolerating only the
documented build-time stamps (`etc/version`, `etc/odi-build`, busybox and
every applet hardlinked to it, and the `/etc/passwd` that `ROOT_PW=locked`
writes). Its first real run caught a genuine bug: `kernel/build.sh` set no
`SOURCE_DATE_EPOCH`/`KBUILD_BUILD_TIMESTAMP`/`KBUILD_BUILD_USER`/`KBUILD_BUILD_HOST`,
so every kernel build embedded its own real build time, host and account
(`scripts/mkcompile_h`) and no two builds of the same commit ever produced
the same `uImage`. Now pinned: the timestamp from the commit being built,
fixed strings for the user and host.

**Weekly dependency check.** A new `dependency-bump.yml` (`tools/bump-deps.sh`)
checks the Linux point release, busybox, dropbear, iproute2, the three
odi-toolchain image digests and the odi-sfp-exporter/odi-ui release tags
against upstream, verifies whatever it finds the way the existing fetch
scripts always have, runs the test suite against the result, and opens a PR
only if that passes. Its first real run already found and verified a kernel
point-release bump, linux 6.18.53 -> 6.18.54, included in this change.
**An opt-in NTP client.** The stock image has no RTC and no NTP client at
all. `svc-ntpd.sh`, a new
respawn entry, starts busybox `ntpd` in the foreground against
`NTP_SERVER` (docs/SETTINGS.md) only while that key is set; unset, it runs
the same off-flag placeholder every other disabled service uses, so the
static inittab entry does nothing rather than needing to be commented out.
`apply.sh ntp` (SERVICE RESTART) starts or stops it live, no reboot,
whenever the setting changes. Covered end to end in `test-qemu`: the
harness points `NTP_SERVER` at the qemu user-net gateway address and
checks the guest clock is actually corrected against a host-side NTP
responder (skipped, not failed, when the build host has none).

**syslogd and klogd, with a circular buffer `logread` reads, plus optional
remote forwarding.** The stock image has neither a syslog daemon nor
anywhere central `logread` can read from.
Two new respawn entries,
`svc-syslogd.sh`/`svc-klogd.sh`, start busybox syslogd/klogd in the
foreground with a 64 KB circular buffer (`-C64`), the same off-flag and
config-store-read idiom every other `svc-*.sh` here uses. Setting
`SYSLOG_SERVER` in the config store (docs/SETTINGS.md) adds `-R host:port
-L`: forwarded remotely, kept locally too. `dropbear` now logs through
syslog like everything else here, instead of straight to
`/var/log/services.log` (dropped its own `-E`, which is what was
redirecting it away from syslog); `confd`, `metricsd` and `omcid` have no
syslog option of their own to switch on, so their existing `/var/log/*.log`
files are unchanged. `apply.sh syslog` (SERVICE RESTART) restarts syslogd
without a reboot or an OMCI interruption.

**The optics model gets alarm/warning flags and an optical LOS status, plus
a scriptable host-side transceiver behind the same modelled I2C controller
the driver tests already use.** `pon get transceiver alarm-status` reads
SFF-8472 A2h's alarm and warning flags (bytes 112/113, 116/117) and the
RX_LOS status bit (byte 110) in one contiguous 8-byte transaction
(`odi_ddm.h`'s new `ODI_DDM_ALARM_STATUS` selector, `hw_transceiver_alarms_get()`
on the diag side). `test/odi_optics_model.h` adds a full scriptable SFF-8472
device (A0h/A2h pages, an "absent module" mode, and the IO_GPIO_EN routing
gate from `docs/kb/dfp34x-optics-on-i2c-port1-gated-by-io-gpio-en.md`) behind
`odi_switch_mock.h`'s existing register write-hook, so a host test can drive
a real I2C transaction through `odi_i2c_read_bytes()`/`odi_ddm_get()` into
NACK, low-rx-power alarm/warning, or LOS scenarios rather than a fixed byte
table. The exporter contract (`src/diag/test/exporter.txt`) gains the new
command and three scripted-scenario goldens (rx power drifting to about
-28 dBm, LOS asserted, module absent). See `docs/HACKING.md`, "Optics
model", for how to script a scenario.

## v1.0.4 — 2026-09-28

**A respawned omcid gets provisioned again, not just restarted.** Hardware
finding (rc5, claro, 2026-09-28): after `kill -9 omcid`, init respawns it
within 8 s and it pings and serves `omcicli`, but `gpon_omci_services`
stays 0 for 200+ s -- the OLT had already provisioned this ONU and does not
re-send the MIB to a fresh omcid with an empty one. `svc-omcid.sh` now
tells a respawn apart from the boot's first start with a marker under
`/var/run` (tmpfs, gone at the next reboot) and, on a respawn, backgrounds
`omci-respawn-reprovision.sh`: deactivate, wait for the new omcid to
register, re-apply the PLOAM password, hold, reactivate -- the same
hardware-proven sequence the web UI's `apply.sh omci` already uses, forcing
the OLT to re-range and re-provision. The two callers now share that tail
as `rcs-lib.sh`'s `omci_reactivate` rather than keeping two copies. The
respawned omcid process itself is never touched -- init already supervises
it as a `respawn` entry, so killing or restarting it from here would only
trigger another respawn. `docs/BOOT.md` has the new "Respawn
re-provisioning" section.

**A registered watchdog client is armed at registration, not on its own
first ping.** Hardware trial (rc4, claro, 2026-09-28): omcid answered two
omcicli commands after registering with odi_omci, then stopped -- 0x800
filled to its 64-message cap, `gpon_omci_services` stayed 0 for 5+ minutes,
and `/proc/odi_wdt/clients` showed `armed=0` at 279 s uptime: a client that
never pings at all was never armed, so odi_wdt's per-client deadline never
had anything to check, and a daemon stuck before its first ping was never
reset. Investigated thoroughly (the rc4 diff, three separate
`qemu-mips-static` reproduction attempts combining a CLI flood -- matching
metricsd v1.1.2's/confd v1.0.6's own 2 s-timeout-then-SIGKILL pattern --
with the real mib-reset/mib-upload/mib-upload-next sequence from the
trace, single- and dual-flooder); none reproduced the stall itself
(qemu-user has no netlink, so this needs either real line timing/volume or
a mechanism outside the CLI/MIB-volume angle). Closed the watchdog gap
regardless, since it is a real, independently-justified bug:
`odi_wdt_client_register()`
(`kernel/extra/drivers/net/ethernet/odi/odi_wdt.c`) now takes the
registration uptime and arms immediately, so "never pinged" is caught by
the same deadline as "pinged once and then stalled". This changes the
previously-intentional "registered but unarmed never resets" behaviour (the
modules.off dev case), so `rcS` now only registers "omcid" with odi_wdt
under the same two conditions `svc-omcid.sh` gates the actual exec on
(`/etc/config/modules.off` absent and `/bin/omcid` executable) -- a dev
image that will never start omcid no longer registers it either, so there
is no legitimate registered-but-silent-forever case left. `test/odi_wdt_test.c`
rewritten to match (`test_registered_but_never_pinged_client_still_resets`,
`test_client_reregister_resets_the_clock`).

**`mq_send()` (`src/omci/omci_msgq.h`) is bounded too.** The one SysV IPC
call left in this daemon's whole "bound every wait" sweep that was still a
bare blocking `msgsnd` -- every client (`omcli`/`omcicli`, so confd and
metricsd both) uses it to enqueue a request onto 0x800 or the native queue,
and it had no `IPC_NOWAIT` of its own. metricsd's/confd's own timeouts
SIGKILL the *child*, which does not stop it parking in `msgsnd` first if
the queue omcid owns is momentarily full -- exactly when several such
clients pile in at once. Now `IPC_NOWAIT` plus a bounded retry (100 x 5 ms),
the same shape omcid's own reply paths already use (`vqsrv.c`'s
`vq_reply()`, `clisrv.c`'s `cli_chunk()`).

**Every wait on another process is bounded now, not only the two hit on
hardware.** Audited every `diag`, `omcli`/`omcicli`, `arping`, `nv` and
`/proc` verb write/read in `rootfs/skeleton/etc/` for an unbounded wait.
Added `write_proc_bounded`/`read_proc_bounded` (`scripts/rcs-lib.sh`,
`timeout $PROC_WRITE_TIMEOUT_S sh -c '...'`, 5 s default) for the bare
`echo verb > /proc/odi_init`/`/proc/odi_omci` writes a plain `timeout`
wrapper cannot reach (they are the calling shell's own `write(2)`, not a
forked command) -- used by the `odi_wdt` register write (`rcS`),
`rcs-lib.sh`'s PON steps and `switch_init`, and `apply.sh`'s
`gpondeact`/`gponpw`/`gponact`. `fwu_starter.sh`'s `nv getenv sw_active`
fallback gained a plain `timeout 5`. `docs/SETTINGS.md` gained a "Bounded
waits" section listing the pattern per case, including two documented
exceptions: `rcS.dev`'s dev-hook diag probes (a process parked in an
uninterruptible-sleep kernel wait ignores `timeout`'s signal too), and
`rcS`'s 23-step switch SDK-init loop -- every boot, no exception, and
`write_proc_bounded` would have turned each step's zero-fork builtin
`echo`/`cat` into a `timeout`+`sh` fork pair, close to 90 extra fork/execs
on the path this image has fought hardest to keep fast (rc2 to rc3: 85-90 s
boot down to 31 s), for a step that has never been observed to hang; the
existing before/after crumb already names a stuck one in the ramlog if it
ever does. No change needed in `network.sh` or `rcS.dev`'s confirm-arp
path -- both already wrapped every `diag`/`arping`/`/proc` call in
`timeout`.

**`confd` pinned to v1.0.6.** Same rule applied on the odi-ui side: bounded
every `run_to_buf`/`run_script_to_buf` child wait in the confd daemon
itself (omcicli, diag, flash, apply, ping, fwu, reboot, md5sum, nv --
odi-ui's own CHANGELOG has the full list and the per-command timeouts),
plus an ssh connect/keepalive bound on the maintainer scripts
(`scripts/deploy.sh` and the capture/schema-drift tools) that had none.

**`metricsd` pinned to v1.1.2.** Fixes the matching hardware-trial bug on the
exporter side: `metricsd`'s own child wait (`omcicli dump srvflow`) had no
timeout either, so a stuck omcid (the queue-reclaim bug above) hung the whole
exporter, not just the omcicli queue. See odi-sfp-exporter's CHANGELOG
(v1.1.2) for the fix -- bounded child waits, `gpon_omci_up` reporting a
timeout as a metric instead of silence.

**A respawned omcid reclaims the 0x800 (omcicli) queue instead of waiting
forever for `omci_app`.** Hardware trial (rc3, claro, 2026-09-28): after
`kill -9 omcid` + respawn, `vq_ensure()`'s `IPC_CREAT|IPC_EXCL` failed
`EEXIST` against the queue the previous omcid instance left behind (`kill
-9` skips `on_signal()`'s `mq_remove()`), and the daemon logged "the
omcicli queue is still omci_app's" and never served it again -- 42 queued
requests piled up unanswered. `omci_app` never runs on this image at all
(`src/omci/README.md`), so a stale queue at 0x800 can only be a dead
instance of omcid's own: `vq_ensure()` (`src/omci/respond/vqsrv.c`) now
removes it and recreates on `EEXIST`, same as `mq_open_fresh()` already
does for the omcli queue. `src/omci/qemu-test.sh` gained a `kill -9` +
respawn scenario asserting the reclaim and that `dump srvflow` (the
exporter's own command) answers within 2 s afterwards.

**The omcid watchdog ping is wall-clock-gated now, not loop-iteration-counted.**
Hardware trial (rc2, claro, 2026-09-28): filling `/tmp` to `ENOSPC` left
`last_ping_age` at 27 s against the 15 s the main loop was supposed to
guarantee. Root cause: the loop counted `NL_POLL_US`-spaced iterations and
called `1000000/NL_POLL_US` of them "one second" -- true only if every
iteration takes exactly `NL_POLL_US`; one slow iteration (a full apply on
an OMCI frame, a write that blocks while `/tmp` is nearly full) stretches a
counted second past its real length, so 15 counted seconds can cover well
more than 15 real ones. The `src/omci/respond/main.c` main loop now reads
`CLOCK_MONOTONIC` (`sys_clock_gettime`, already available) and gates both
the 1 s sample tick and the 15 s `wdt_ping()` on elapsed wall-clock time
instead, independent of how long any one iteration takes.

**Daemons are `/etc/inittab` `respawn` entries now, not `supervise.sh`.**
Hardware trial (rc2, claro, 2026-09-28): `kill -9` of omcid was not
respawned, and `odi_wdt` correctly reset the board 61 s later when omcid
missed its ping deadline -- the old shell-loop supervisor
(`rootfs/skeleton/etc/scripts/supervise.sh`, removed) had lost track of
the child. Native over hand-rolled: busybox init forks each daemon itself
and never loses the pid. metricsd, confd, dropbear and omcid are each now
a `respawn` entry running a small per-daemon script
(`rootfs/skeleton/etc/scripts/svc-*.sh`) that checks its own
`/etc/config/<name>.off` flag and execs into `respawn.sh` (sets
`oom_score_adj`, then execs the daemon in the foreground; dropbear already
ran `-F`, and omcid `-d` was never a daemonize flag either -- neither ever
double-forked). `/etc/init.d/services` is now a `stop`/`start` hand tool
(writes/clears the `.off` flag and kills the current process so init
restarts it at once) rather than a boot-time launcher.

**The PON steps split into `rcS` (sysinit) and a new `rcS.pon` (`once`),
so daemon respawn no longer waits on them.** Mounts, the config partition
and the management address stay in `rcS`, which every respawn entry still
waits on (busybox init starts nothing until sysinit returns); the PON
steps (optics, the switch init, `gponsn`/`gponpw`/`gponact`) moved to
`rcS.pon`, a `once` entry started in the same breath as the four daemon
`respawn` entries -- the same real-world concurrency `services start &`
already had with them. Since omcid now starts concurrently rather than
being forked inline mid-PON-steps, `gponact` first polls the live
`/proc/odi_omci` registration table for omcid, bounded at 10 s, rather
than assuming it has already registered (an OLT that activates before
omcid is up would otherwise see its first OMCI frames dropped
unregistered). Shared functions (`config_mounted`, `crumb`,
`confirm_watchdog`, the PON step helpers) live in a new
`rootfs/skeleton/etc/scripts/rcs-lib.sh`, sourced by both scripts. The
`make test-rcs` golden traces were regenerated and reviewed by hand: the
`rcS` trace shrinks to the stages that stayed in it; the PON steps and the
watchdog confirmation no longer appear (they are `rcS.pon` now, untraced
by this harness, which only ever ran `rcS` itself). The `make test-qemu`
`kill -9` resilience scenario now also covers omcid, which starts under
qemu for the first time (previously gated on `/proc/odi_omci`, absent
there; the new `svc-omcid.sh` gates only on `/etc/config/modules.off` and
the binary existing, so it starts, and degrades harmlessly exactly as it
already did on any kernel without `/proc/odi_omci`).

**v1.0.2 and v1.0.3 are both also withdrawn: no management network on real
hardware.** Trialled on claro (2026-09-28), v1.0.3 hung unreachable for 10+
minutes with no ARP reply for the configured management address, and did
not self-revert -- the same symptom the v1.0.3 watchdog redesign was
supposed to have fixed. Root cause: `network.sh` falls back to a guessed
`DEF_IP` (192.168.1.1) whenever the config partition or `lastgood.xml` is
not readable at boot, and `rcS` confirmed the watchdog (`userland_ok`)
unconditionally, before `omci_start`/`switch_init` even ran -- so a stick
that came up with a guessed, unreachable address, or a failed `switch_init`
(`/proc/odi_omci write failed` in the ramlog), still confirmed and never
reverted. `rcS` gained `confirm_watchdog()`, called right after
`omci_start`, which requires the config partition mounted, a real
(non-guessed) management address, and a successful `switch_init` before
writing `userland_ok` -- still with no ARP or reachability dependency. (The
"real, non-guessed address" check was reworked again below, into a live
`network.sh configured` call, superseding the flag file this paragraph
originally described.) `test/rcs_trace_inner.sh`'s `network.sh` stub
updated to match.

**Mounts are declarative now, and "config mounted" is a live check, not a
flag.** `/etc/fstab` (new) lists every mount rcS makes -- `proc`, `sysfs`,
`/var` and `/var/tmp` (tmpfs, capped), `devpts`, and the config partition
as `mtd:config /var/config jffs2` (found by MTD partition NAME, no
`/dev/mtdblockN` node, no `/proc/mtd` lookup, no `mdev` race to lose --
`mount-config.sh` is cut down to the one thing left that a mount option
cannot express, the `/etc/config` symlink check). rcS runs `mount -a`
twice (creating the `/var` subdirectories in between -- `mount` never
makes a missing mountpoint, and nothing under `/var` exists until `/var`
itself has landed). `config_mounted()`, the one place anything asks
whether the config partition is mounted, now greps the live
`/proc/mounts` for a real jffs2 mount at `/var/config` plus the symlink
resolving, instead of trusting a flag file `mount-config.sh` wrote once
and nothing ever rechecked. Caught in the rewrite: the first `mount -a`
must not be traced into `/tmp` -- `/tmp` is a symlink to `/var/tmp`, which
does not exist until that same call has mounted `/var`, and a shell
redirected into a missing directory never runs the command at all
(`make test-qemu` caught this one: ssh never came up).

**Root cause, confirmed on a stick: `kernel/618/config` had neither
`CONFIG_SHMEM` nor `CONFIG_TMPFS`.** Every fix above treated the symptom
(no management network, `/var` read-only) as a boot-order or gating
problem; the actual cause was one line below both mounts in the fstab
rewrite: a `mount -t tmpfs ... size=6m` on a kernel with neither symbol
falls through to `mm/shmem.c`'s `!CONFIG_SHMEM` stub (`ramfs_get_sb`),
which mounts fine but rejects `size=` outright (`Invalid argument`) --
confirmed with the exact error on real hardware. That is why the capped
`/var` and `/var/tmp` mounts silently never landed, `/var` stayed the
read-only squashfs, and nothing downstream of it (the config partition,
`/var/log`) could be created. Fixed by enabling `CONFIG_SHMEM=y` and
`CONFIG_TMPFS=y` in `kernel/618/config` -- uImage grew from about
1,274,6xx to 1,281,554 bytes, comfortably under the 1,359,872-byte
partition cap (78,318 bytes spare). Two more gates against a repeat:
`required_mounts_ok()` (new, in `rcS`) checks `/proc/mounts` for `/var`
and `/var/tmp` both actually being tmpfs right after the second `mount -a`
and logs to kmsg the moment either is missing, and `confirm_watchdog()`
now refuses to confirm without it, on top of the existing config-partition
and `switch_init` checks. The `odi-toolchain-qemu-kernel-malta` kernel
fragment gained explicit `CONFIG_SHMEM`/`CONFIG_MTD`/`CONFIG_JFFS2_FS`
lines (already implied by `malta_defconfig`'s defaults, so no new kernel
tag was needed this time, but the previous mismatch was exactly this kind
of implicit-default drift going unnoticed) and `make test-qemu` gained an
assertion reading `/proc/mounts` for `/var`/`/var/tmp` tmpfs at their
configured sizes -- the check that would have caught this the first time.

**The `/var/run/network-configured` flag file is gone.** `rcS`'s watchdog
confirmation asked a flag written once, at boot, whether `network.sh` had
applied a real (non-guessed) address. Replaced with `network.sh
configured`, a new live-check mode: it recomputes the same IP `network.sh`
would apply at boot and reads it straight back off `br0` (or `eth0`, if
the bridge path failed) via `/proc/mounts`-style live inspection, so a
later re-address or a `network.sh addr` that failed cannot leave a stale
`configured` flag behind.

## v1.0.3 — 2026-09-28

**WITHDRAWN.** Broken on hardware: no management network. Root cause,
confirmed after the fact (v1.0.4 below): `kernel/618/config` had neither
`CONFIG_SHMEM` nor `CONFIG_TMPFS`, so the capped `/var` tmpfs mount failed
and the config partition never mounted. Release images deleted from
GitHub 2026-09-28. Use v1.0.1 or v1.0.4.

**v1.0.2 is withdrawn (marked pre-release) and must not be used.** Trialled
on claro (2026-09-27), it hung for 15 minutes with no ARP or ping despite
the SFP link staying up, and did not self-revert: the ramlog showed the
new userland health-kicker withholding its kick
(`health-kicker: withholding kick (omcid_ok=0 mem_ok=1)`) every 30 s while
`odi_wdt` kept kicking the hardware regardless
(`odi_wdt: alive at 799 s ... userland_ok=1`), because `userland_ok` is a
one-shot flag latched at boot -- the kernel never enforced the health
kicker's periodic confirmation at all. v1.0.1's plain 120 s boot-only
deadline was, ironically, safer: it at least reset a boot that never
confirmed, which is more than v1.0.2's unenforced addition did. The
root cause of `omcid_ok=0` itself was not fully recoverable from that
boot: the DRAM ramlog is a two-page ring buffer and only the later page
survived, so the rcS/PON-step portion of that boot is gone. The redesign
below removes the userland health-kicker and the failure mode it
depended on entirely, rather than patching around one unconfirmed cause.

**Watchdog redesign: the kernel (`odi_wdt`) is now the only owner of the
hardware watchdog** (`docs/SETTINGS.md`, "Watchdog rules"; `docs/BOOT.md`).
Three independent rules, judged and enforced kernel-side, any one of
which stops the kicker:

- the one-shot boot confirmation, unchanged from v1.0.1
  (`/proc/odi_wdt/userland_ok`, 120 s, the `confirm-arp` dev opt-in kept);
- a per-client ping deadline: rcS registers a client by name and deadline
  (`/proc/odi_wdt/register`, e.g. `omcid 60`), the client pings its own
  deadline from its own main loop (`/proc/odi_wdt/ping`) -- omcid does
  this every 15 s (`src/omci/respond/main.c`) -- and the kernel resets the
  board if an armed client's deadline is missed. `/proc/odi_wdt/clients`
  shows each client's state (armed, last-ping age) for debugging;
- a kernel-side memory floor: `MemAvailable` (`si_mem_available()`, no
  userland reader to lose along with the memory it reports on) held below
  2048 KB for 3 consecutive 5 s checks.

The v1.0.2 userland health-kicker (`health-kicker.sh`,
`/proc/odi_wdt/health_kick`) is removed entirely. Every reset now logs
which rule fired, ramlog-visible, before it happens. Host-tested against
a fake clock (`test/odi_wdt_test.c`); the reset itself needs real
hardware to verify (`/proc/odi_wdt`, not exercised by `make test-qemu`).

**Every other v1.0.2 change was reviewed and kept**, unless noted:
size-capped `/tmp`/`/var` tmpfs, `oom_score_adj` biasing, daemon respawn
via `supervise()`, `vm.min_free_kbytes`, `make test-qemu` (the qemu
full-system harness, its health-kicker scenario replaced with a check
that rcS's client registration degrades harmlessly without
`/proc/odi_wdt`), and the `cpu-pause` PAUSE-watermark fix (verified on
hardware, ISP1, 2026-09-27).

**Fixed `make test-rcs`'s golden traces**, stale since v1.0.2 (d7839bd
added the `/var`/`/tmp` tmpfs mounts and wrapped omcid in `supervise()`
but never regenerated `test/fixtures/rcs-trace-*.txt`) -- `test-rcs` had
been failing since v1.0.2 and nobody had run it, since it needs
Docker+ptrace and is not part of `make test` or CI. Regenerated and
reviewed by hand: the only differences are the tmpfs mounts and omcid's
supervise-restart loop against this harness's stub binary.

## v1.0.2 — 2026-09-27

**WITHDRAWN.** Broken on hardware: no management network. Root cause,
confirmed after the fact (v1.0.4 below): `kernel/618/config` had neither
`CONFIG_SHMEM` nor `CONFIG_TMPFS`, so the capped `/var` tmpfs mount failed
and the config partition never mounted. Release images deleted from
GitHub 2026-09-28. Use v1.0.1 or v1.0.4.

**Resilience.** A 20 MB `scp` into `/tmp` on ISP1 exhausted RAM (`/tmp` was
ramfs, unbounded and unreclaimable): the OOM killer took dropbear, confd
and omcid, none of them restarted, and the box stayed unmanageable until a
power cycle even though the hardware datapath kept forwarding on its own.
Fixed together (`docs/SETTINGS.md`, "Resilience"; `docs/IMPROVEMENTS.md`):

- `/tmp` (`/var/tmp`) and `/var` are size-capped tmpfs (8 MB / 6 MB)
  instead of unbounded ramfs, so a full `/tmp` gives `ENOSPC` rather than
  taking the box down with it.
- `oom_score_adj`: `-1000` for omcid and dropbear, `-500` for confd, the
  kernel default (`0`) for metricsd -- the one daemon whose loss costs
  neither management access nor GPON state.
- omcid, dropbear, confd and metricsd now restart automatically if they
  die, rate-limited to 5 restarts per 60 s window (`supervise()`,
  `rootfs/skeleton/etc/scripts/supervise.sh`).
- A periodic health kicker (`rootfs/skeleton/etc/scripts/health-kicker.sh`,
  `/proc/odi_wdt/health_kick`, `kernel/extra/drivers/net/ethernet/odi/odi_wdt.c`)
  resets the board through the watchdog if userland health (omcid alive,
  `MemAvailable` above a floor) stops being reported later in the boot --
  independent of, and in addition to, the one-shot boot confirmation
  (`/proc/odi_wdt/userland_ok`) this image already had, which only ever
  covered a boot that never comes up at all.
- `vm.min_free_kbytes` raised to 1536 (from the kernel default of roughly
  128 KB on a box this size) at boot.

**A qemu full-system test harness** (`make test-qemu`, `docs/HACKING.md`):
boots the real rootfs (busybox, inittab, services, dropbear, confd,
metricsd -- unmodified) under `qemu-system-mips -M malta`, standing in for
the RTL9602C board qemu cannot emulate, and checks ssh (a test-only key),
the web UI and the exporter, plus the resilience scenarios above, end to
end. The kernel it boots is a stock, prebuilt mainline build
(`ghcr.io/andreziviani/odi-toolchain-qemu-kernel-malta`, a new image
published from the odi-toolchain repository, `toolchain/images.env`), not
this repository's own -- so CI never has to build a kernel from source to
run it. Runs in CI on every push and pull request
(`.github/workflows/ci.yml`).

**Fixed the odi_nic driver asserting PAUSE** toward the switch CPU port at
ordinary traffic levels (merged from branch `cpu-pause`).
`ODI_NIC_FC_ON_LEVEL`/`ODI_NIC_FC_OFF_LEVEL` (the free-descriptor
watermarks that gate the NIC's own flow control) were derived as a flat
quarter/three-quarter of the RX ring depth, asserting PAUSE at only 75%
ring-used and holding it until the ring drained back to 25% used — a band
wide enough for ordinary NAPI scheduling jitter at a few packets a second
to cross and hold, with no real congestion behind it. Rescaled to the
stock firmware's own near-exhaustion trigger proportion (assert near 94%
used, deassert near 81% used) instead, so PAUSE only fires near actual
ring exhaustion. Verified on hardware: 3003 → 13 PAUSE frames/min on ISP1.

- **Lint no longer chokes on binary files.** The plan/task-id grep in
  `make lint` now skips binary files (`git grep -I`); `docs/images/*.png`
  matched the pattern as raw bytes on the CI runner only, never locally.

## v1.0.1 — 2026-09-27

`metricsd` (the Prometheus exporter) v1.1.1, up from v1.0.3. The `diag`
commands the exporter sends are unchanged (the exporter contract goldens
in `src/diag/test/` still pass byte for byte), but its metric output
changed: `oversize` is now a frame-size bucket rather than a receive-error
`kind`, there is a new frame-size histogram, and an octet-counter wrap
guard. No odi-oss doc or golden pinned the old `kind="oversize"` label or
`gpon_port_receive_errors_total` by name, so nothing else in this repo
needed updating.

## v1.0.0 — 2026-09-26

The first tagged release, built and published by `.github/workflows/release.yml`
on push of a `v*` tag. `SHA256SUMS` beside the tarball on the
[release page](https://github.com/AndreZiviani/odi-oss/releases) is what to
check the download against.

- **Release images are keys-only.** `ROOT_PW=locked` (the release workflow's
  default; `docs/BUILDING.md`) ships root with no password at all — the
  `/etc/passwd` field is `!` and dropbear runs with `-s`, so ssh does not
  even offer a password prompt. A source build still defaults to a random
  per-build password unless it also passes `ROOT_PW=locked`. First access to
  a keys-only image is through the web UI (`confd`, port 80, `admin`/`admin`
  until you change it), whose SSH-key admin page adds a key for root;
  `docs/FLASHING.md` and `docs/ACCESS.md` have the exact steps. The three
  shapes (`locked`, `none`, the default password) are written by the new
  `image/gen-root-account.sh`, tested on its own by `test/root_pw_test.sh`.
- **confd v1.0.5.** Config page reorder, MIB class picker, text sweep in the
  web UI.
- **Prebuilt toolchain.** The gcc 16.2.0 / binutils 2.47 / uClibc-ng 1.0.59
  toolchain and the freestanding Debian cross-gcc are container images from
  the odi-toolchain repository, pinned by digest in `toolchain/images.env`
  and pulled on first use, instead of an hour of compiling into a local
  Docker volume. The same recipe builds them from source there, and the
  image built with them is identical file for file to one built with the
  old volume, build timestamps aside. The ISA audits are the shared ones
  in those images.
- **The boot script reads top to bottom.** `/etc/init.d/rcS` is the boot
  in order, 233 lines instead of 637, and `docs/BOOT.md` says why each
  stage is where it is. The development aids moved to
  `/etc/init.d/rcS.dev`, which ships in the image: `breadcrumbs.on`,
  `confirm-arp` and `pon-steps` work as before. The files only earlier
  images read (`bdgconn-probe`, `skip-steps`, `replay/`, `regtrace.*`) are
  ignored; `docs/SETTINGS.md` lists them. The switch init and omcid start
  whenever the switch driver is there, so an image built without
  `/etc/kernel-config` no longer comes up without OMCI.
  A failed `devmem` read in the optics setup no longer ends the boot
  before the PON steps.
- **The PLOAM password and the serial number stay out of the logs.** The
  boot logged each PON step with its argument, so `gponpw <password>` and
  `gponsn <serial>` reached the kernel log, the DRAM ramlog and, with
  `breadcrumbs.on`, the breadcrumbs file. The steps are now logged by name.
- **A fast metrics scrape no longer holds service back.** omcid builds the
  bridge connections one quiet second after the OLT stops sending, and it
  counted any CLI request as activity: a client polling `omcicli` every
  few seconds kept the stick in O5 with no service (on ISP1 all 497 OMCI
  frames were in by 44 s and no service was up at 467 s). The quiet second
  now counts OMCI frames only.
- **Quieter NIC.** The NIC state dump after the first open and the ring
  register read-backs now need `odi_nic.debug=1`, like its other traces.
- Internal restructuring with the boot register stream, the OMCI driver
  calls and the boot actions pinned by goldens: one register replay engine
  for the four captured sequences; the switch-core leaves in six files by
  topic and the bridge-connection derivation in its own; omcid's driver
  path and MIB handlers in five files by topic; the comment pass over all
  of them; the capture and replay helpers the 6.18 kernel cannot use
  deleted.
- **The driver bring-up scaffolding is gone.** The switches that only a
  bisection trial ever set: `modload.mask`, `sdkinit.mask`, `parity.table`
  and `parity.mask` on the config partition (each also armed a reboot 600 s
  into every boot, now gone too), the `platform_init_mask` kernel
  parameter, and the `init_parity`, `parity_add`, `sdkinit_mask`,
  `ds_encrypt`, `peek`, `poke`, `ddm` and `i2c` writes of `/proc/odi_omci`,
  whose one write is now `switch_init`. Registers are read and written with
  `diag register get/set`, the optics with `diag pon get transceiver`. A
  normal boot writes the same registers in the same order as before
  (checked on the host against a golden of the whole boot). The kernel
  config has four odi-oss symbols instead of ten. uImage 2.3 KB smaller.
- **`reboot` resets at once.** The kernel registers a restart handler that
  resets the board through the watchdog at its shortest timeout, about a
  third of a second after shutdown, instead of spinning until the
  watchdog window ran out. On ISP1, `reboot` to the stock image answering
  again went from about 115 s to about 75 s. The NIC DMA is stopped first,
  on an emergency restart too. `halt` and `poweroff` hang with the watchdog
  armed and reset about 42 s later.
- **Root password hash: SHA-512 crypt (`$6$`), not MD5 (`$1$`).**
  `image/build.sh` now hashes the per-build root password with
  `openssl passwd -6` at the algorithm's default rounds (5000, no `rounds=`
  tag). Both verifiers on the stick go through the same libc `crypt()`:
  dropbear's password auth calls it directly, and busybox login/su default to
  `USE_BB_CRYPT_SHA=y`. yescrypt (`$y$`) was considered and ruled out:
  uClibc-ng 1.0.59 has no yescrypt code at all, and dropbear's `crypt()`
  would return NULL for a `$y$` hash and lock root out over ssh -- the only
  way in, this device has no serial console. A crypt() microbenchmark cross-
  compiled with our toolchain and timed under a calibrated qemu-mips-static
  proxy lands around 0.2s per login check on this ~300 BogoMIPS core at the
  default rounds, well under the ~0.5s where fewer rounds would be worth
  considering, so the rounds stay at the default.
- **Two names the stock firmware chose, gone.** `/proc/rtk_init` is now
  `/proc/odi_init`, and `/proc/luna_watchdog` is now `/proc/odi_wdt`
  (`userland_ok` and `watchdog_flag` keep their names). `odi_omci`'s
  netlink transport rides its own protocol number instead of
  `NETLINK_USERSOCK`, so it no longer shares a family with unrelated
  users. Every consumer in this repo (rcS, `apply.sh`, tests, goldens,
  docs) moved with them; nothing on the wire or in the MIB path changed.

## odi-oss-260925-618z2 — 2026-09-25

On top of 618z1:

- **The MAC table.** `diag l2-table get all` reads the switch L2 lookup
  table back (a row is in use when the table status HIT bit says so after
  the read), and the web UI's "Read the MAC table" shows it. `igmpd -j`/`-l`
  add and remove a static multicast entry by hand; IGMP snooping itself
  stays off.
- **Boot confirmation without the network.** rcS confirms userland to the
  watchdog once its own steps have run, whatever address the host uses; the
  120 s deadline still resets a boot that hangs and still reverts a trial.
  `/etc/config/confirm-arp` brings back the ARP-on-.2 bar for development.
- `confd` v1.0.4.
- Tried on ISP1: O5, the 4 learned addresses listed, a manual multicast
  join and leave, exporter 8/8.

## odi-oss-260925-618z1 — 2026-09-25

The first release. Everything in `docs/IMPROVEMENTS.md`: Linux 6.18,
fully open — our own switch, GPON MAC, CPU-port NIC and OMCI transport
drivers, an independent implementation built straight into the kernel, no
proprietary kernel module anywhere in the image; our own OMCI daemon
(`omcid`) driving GPON provisioning end to end; watchdog rescue for a hung
kernel or a boot whose userland never comes up; the DRAM ramlog console for
reading a boot with no serial console; devtmpfs; seeded entropy; a current
dropbear with SSH keys and `scp`; per-build root password; the web UI
(`confd`) and the Prometheus exporter (`metricsd`) with
`gpon_omci_services`; one-shot trial boots with `fwu.sh` guards; our own
toolchain (gcc 16 / binutils 2.47 / uClibc-ng), building the kernel and
every userland binary; `make image-all` from a clean clone, and CI for
lint, the host tests, `diag` and the OMCI suite.

Tested: on ISP1, 15 of 15 consecutive trial boots and a clean 3-hour soak;
on ISP2, carrying a household's Internet link. Both as a one-shot trial
over the stock firmware (see "Status and limitations" in `README.md`).

What changed in the last round of trial builds before this release:

**Kernel** (`docs/KERNEL.md`)

- Linux 6.18.53. The patches to mainline files are down to 19 changed
  lines in 6 files (`tools/kernel-footprint.sh`): the RLX5281 probe case,
  the board's System type entry and the Makefile and Kconfig hooks.
  Everything else is in the `kernel/extra` overlay.
- The CPU is built as plain `CPU_R3000`, so mainline's own R3000 exception
  model, TLB and context-switch code run unchanged. Barriers go through the
  board's `__wbflush` (a `sync`), and our DMA ordering points use
  `rtl8686_sync()`, which does not depend on `CPU_HAS_SYNC`.
- The NIC DMA the loader leaves running is stopped in `prom_init()`,
  before the kernel owns any memory. Until then it could write received
  frames into page-cache pages: the cause of the rare boots that died with
  init or `login` killed by SIGBUS.
- The PON packet-buffer (PBO) downstream window is reserved from
  `0x016ff000`, where the hardware has it; it used to start one page
  higher.
- icache coherence: `flush_data_cache_page()` and `flush_cache_page()` on
  an executable mapping invalidate the whole icache, and `free_initmem()`
  flushes both caches before the init sections are freed.
- Register replay tables are firmware files (`/lib/firmware/odi/*.bin`,
  loaded with `request_firmware()` and released after use): about 484 KB
  of RAM and 8.5 KB of kernel image back.
- Config diet: AIO, signalfd, timerfd, eventfd, inotify, fhandle,
  fadvise/madvise, rseq, membarrier, cross-memory attach, the page monitor,
  core dumps, ethtool netlink and swap are off. `POSIX_TIMERS`,
  `FILE_LOCKING` and `VM_EVENT_COUNTERS` are deliberately kept.
- `print-fatal-signals=1` on the command line, so a process killed by a
  signal leaves its registers in the ramlog.
- The ramlog keeps the previous boot: `/proc/odi_ramlog_prev` and
  `/proc/odi_ramlog_prev_raw`, with a boot counter, slot, build id and last
  early crumb in a metadata block at the end of page A. Early crumbs are on
  by default; crumbs in core files are opt-in (`CRUMBS_CORE=1`,
  `kernel/618/debug/`).
- Locking over the shared switch engines: `odi_switch_lock` (mutex),
  `odi_switch_dsf_lock` (IRQ-safe leaf spinlock), `odi_i2c_lock` over the
  whole DDM byte sequence, and a lock for the watchdog enable writers.
- The OMCI netlink socket refuses senders without `CAP_NET_ADMIN`.
- NIC error paths: RX refill never leaves the hardware pointing at a freed
  buffer, a stopped TX queue is always woken, every DMA mapping is checked.
- Build warnings: 22, all GCC 16 "'retain' attribute ignored" in mainline
  networking files; none from our code.

**Userland**

- `diag` is our own CLI (`src/diag/README.md`). The commands
  the exporter runs are byte-compatible with the stock CLI, pinned by the
  golden files in `make test-diag`.
- Settings (`docs/SETTINGS.md`): the web UI offers the 21 keys this image
  reads, each with an apply class (LIVE, SERVICE RESTART, INTERRUPTS
  INTERNET, REBOOT); the 163 stock-only keys are shown read-only.
  `apply.sh` applies the management addresses live and the OMCI settings
  by re-ranging the ONU; `fwu_starter.sh` writes an uploaded image to the
  inactive slot from the UI; a second management address (`br0:2`); the
  OLT identity keys are reported only with `/etc/config/omci-identity.on`.
- `confd` v1.0.4 (the MAC table) and `metricsd` v1.0.3.
- IGMP snooping is off: `igmpd` ships, is not started, and its switch path
  does not work on this kernel.
