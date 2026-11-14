# Tools and daemons

Every program this image ships that you would run, restart or read the log
of: what it is, how to invoke it, what it reads, where it writes, and who
starts it. Settings -- the config-store keys, the web UI's controls and the
switch files on the config partition -- are in
[`docs/SETTINGS.md`](SETTINGS.md).

Everything in `src/` is ours: freestanding, statically linked MIPS binaries
with no libc (see `docs/CROSS-COMPILING.md` for what that means and why).
`confd` (the web UI) and `metricsd` (the exporter) are separate projects,
fetched as pinned releases by `src/fetch-releases.sh` rather than built from
source here; `/etc/odi-build` on the stick records which releases went in
(`exporter=`, `confd=`).

## At a glance

| program | kind | started by | talks on | logs to | if it dies |
|---|---|---|---|---|---|
| `omcid` | daemon, the OMCI stack | inittab `respawn` (`/etc/scripts/svc-omcid.sh`) | odi_omci netlink (redirect type 1), SysV queues for `omcli`/`omcicli` | `/var/log/omcid.log` | respawned |
| `confd` | daemon, the web UI | inittab `respawn` (`/etc/scripts/svc-confd.sh`) | TCP 80 | `/var/log/services.log` | respawned |
| `metricsd` | daemon, the Prometheus exporter | inittab `respawn` (`/etc/scripts/svc-metricsd.sh`) | TCP 9100 | `/var/log/services.log` | respawned |
| `dropbear` | daemon, ssh and scp | inittab `respawn` (`/etc/scripts/svc-dropbear.sh`) | TCP 22 | syslog (every login) | respawned |
| `syslogd` | daemon, the system log | inittab `respawn` (`/etc/scripts/svc-syslogd.sh`) | UDP 514, if `SYSLOG_SERVER` is set | its own circular buffer (`logread`) | respawned |
| `klogd` | daemon, kernel log to syslog | inittab `respawn` (`/etc/scripts/svc-klogd.sh`) | -- | syslogd | respawned |
| `ntpd` | daemon, NTP client (opt-in) | inittab `respawn` (`/etc/scripts/svc-ntpd.sh`), only while `NTP_SERVER` is set | UDP 123 to `NTP_SERVER` | syslog | respawned |
| `igmpd` | daemon, IGMP snooping (opt-in) | inittab `respawn` (`/etc/scripts/svc-igmpd.sh`), only while `IGMP_SNOOPING=1` | odi_omci netlink (redirect type 4), `/dev/odi_sw`, `/var/run/odi-mcast.conf` | `/var/log/services.log` | respawned |
| `login` | serial console login | inittab `askfirst` (press Enter on the console) | ttyS0 | -- | respawned |
| `diag` | CLI: optics, GPON state, counters, registers | you, rcS, network.sh, metricsd, confd | `/dev/odi_sw`, `/proc/odi_gpon/status`, netlink | stdout | -- |
| `i2cget` / `i2cdump` / `i2cset` | CLI: the optics module I2C devices (busybox) | you | `/dev/i2c-0` | stdout | -- |
| `omcli` / `omcicli` | CLI for omcid | you, metricsd, confd | omcid's queues | stdout | -- |
| `omciprobe` | CLI: one raw driver command | you | odi_omci netlink | stdout | -- |
| `omcicap` | CLI: capture the OMCI channel | you | odi_omci netlink (takes type 1) | stdout | -- |
| `nv` | CLI: the U-Boot environment | you, confd | mtd `env`, `env2` | stdout | -- |
| `flash` | CLI: the config store | confd, you | `/var/config/lastgood*.xml` | stderr | -- |
| `fwu.sh` | CLI: flash one slot | you, `fwu_starter.sh` (from the image tarball) | mtd `k0`/`r0` or `k1`/`r1` | stdout | -- |
| `fwu_starter.sh` | CLI: write an uploaded tarball to the inactive slot | confd, you | runs `fwu.sh` from the tarball | `/tmp/fwu.log`, `/tmp/fwu.state` | -- |
| `apply.sh` | CLI: apply saved settings without a reboot | confd, you | `network.sh addr`; SIGHUP to omcid | stdout | -- |
| `diag-bundle.sh` | CLI: the diagnostics bundle, secrets redacted, identity masked | confd (`GET /api/diag`), you | reads logs, `/proc`, `nv`, the exporter, the optics module (`/dev/i2c-0`) | the tar.gz it writes | -- |

Every daemon -- `omcid`, `confd`, `metricsd`, `dropbear`, `syslogd`, `klogd`
and `ntpd` -- and the serial `login` are all `respawn` entries in
`/etc/inittab`: busybox init restarts whichever one exits, forever, the
instant it does (no restart budget, no rate limit -- `docs/SETTINGS.md`,
"native over hand-rolled"). There is no telnet server (busybox is built
without telnetd).

## How the image starts things

`/etc/inittab` runs `/etc/init.d/rcS` as `sysinit`, and busybox init starts
no `once` or `respawn` entry until it returns -- so rcS never blocks, and
backgrounds everything slow. In order, rcS:

1. mounts `/proc`, `/sys`, `/var` (tmpfs, capped at 6 MB) and `/var/tmp`
   (its own tmpfs, capped at 8 MB -- `/tmp` is a symlink to it), makes the
   `/var` directories, raises `vm.min_free_kbytes`, and mounts the jffs2
   `config` partition on `/var/config`
   (`/etc/scripts/mount-config.sh`; `/etc/config` is a symlink to it);
2. seeds entropy (`seedrng`, seed kept in `/etc/config/seedrng`), sets the
   hostname and `lo`;
3. runs the switch SDK init verbs through `/proc/odi_init` (`intr` ...
   `ponmac`), one crumb before and after each; the kernel loads the
   register replay behind each verb from `/lib/firmware/odi/sdkinit.bin`
   and releases it again (`docs/KERNEL.md`);
4. brings up management networking with `/etc/scripts/network.sh` (host
   SerDes check, MAC, `br0` over `eth0.2`, the address, and the second
   address `br0:2` when `LAN_ENABLE_IP2` is 1, and the static routes of
   `LAN_ROUTES`, if any);
5. registers omcid as a watchdog client (`/proc/odi_wdt/register`, 60 s
   deadline): omcid pings its own deadline from its own main loop once it
   starts, and the kernel -- the only owner of the hardware watchdog --
   stops kicking if an armed client's deadline is missed,
   `docs/SETTINGS.md` ("Watchdog rules").

rcS returns there. Busybox init then fires `/etc/init.d/rcS.pon` (a `once`
entry -- started, not waited for) and starts the four daemon `respawn`
entries in the same breath: `metricsd`, `confd` and `dropbear`
(`/etc/scripts/svc-metricsd.sh`, `svc-confd.sh`, `svc-dropbear.sh`;
`dropbear` generates the ed25519 host key on first boot) and `omcid`
(`/etc/scripts/svc-omcid.sh`, `-a -d`). Each can be turned off with a
`.off` file, see `docs/SETTINGS.md`; each script sets its own
`oom_score_adj` right before it execs the daemon in place
(`/etc/scripts/respawn.sh`), and busybox init restarts whichever one exits,
forever, on its own -- no `supervise()`, no restart budget any more.

Concurrently with those four, `rcS.pon`:

6. sets the optics pins, the `optics` verb of `/proc/odi_init`
   (`PIN_GPIO_SELECT` only, outside the step list);
7. runs the PON steps: `i2c 1`, `i2cen 1`, `laser`, `gpon`, `rxsd`, `gpondrv`,
   `gpondev`, then the switch init (the platform settings and module-load
   replay, through `/proc/odi_omci`) and the watchdog confirmation
   (`/proc/odi_wdt/userland_ok`; without it the board resets at 120 s of
   uptime -- the network is not a condition, unless `/etc/config/confirm-arp`
   exists, development only, then only after an ARP reply from the address
   it holds), then `gponsn` and `gponpw` (serial number and PLOAM
   password from the config store), then `gponact` -- which first polls
   `/proc/odi_omci` for the omcid registration itself, bounded at 10 s,
   since omcid now starts as its own respawn entry rather than being forked
   inline here. `/etc/config/pon-steps` replaces the whole list
   (development, `docs/BOOT.md`).

   The `laser` step reads the id byte of the laser driver chip (A2h page 2,
   byte 0xa0) and drives the TX-enable (SoC GPIO 13) only for the Semtech
   GN25L95 (id 0x6a). On any other chip, or when the id cannot be read, the
   laser stays off and `gponact` is refused (`-EPERM`; `dmesg` says why).
   `ODI_LASER_UNVERIFIED=1` in `/etc/config/odi.conf` makes the boot run
   `laser unverified` instead, which switches the laser on regardless
   (`docs/SETTINGS.md`); `/etc/config/optics.off` skips the step.
   `/proc/odi_gpon/status` shows the outcome: `laser on <0|1> ldd_id <id> unverified
   <0|1>`.

A second `once` entry, `/etc/scripts/slot-state.sh`, starts with them: it
writes the slot state file and the login notice (below, "Slot state") and
exits.

`docs/BOOT.md` has why each stage is where it is, and the development
aids of `/etc/init.d/rcS.dev` (`breadcrumbs.on`, `confirm-arp`,
`pon-steps`), shared by rcS and rcS.pon. A background job in rcS also
trims `/var/log/omcid.log` and
`/var/log/services.log` once a minute: past 256 KB each is cut back to its
last 128 KB. `/var` is RAM, so every log is gone at reboot; the only
records that survive one are the DRAM ramlog (`/proc/odi_ramlog_prev` on
the next boot of this image, `tools/memprobe` from the stock one) and, when
`/etc/config/breadcrumbs.on` is set, `/etc/config/breadcrumbs` and
`/etc/config/trial-diag.txt` on flash.

## Daemons

### `omcid` -- the OMCI stack

Answers the OLT's OMCI, holds the MIB the OLT provisions, and turns it into
switch programming: T-CONTs, GEM flows, queues, bridge connections and
their VLAN rules, over the odi_omci netlink command path into the
`odi_switch` driver built into the kernel (`docs/KERNEL.md`). This is the
daemon that decides whether the OLT sees a working ONU.

    omcid [-a] [-r] [-d] [-f] [-w units] [-c caps-hex] [-s state] [-g file]
          [-l file] [-m file] [-i file] [-j seconds]

    -a          program the switch (apply mode); without it, dry run
    -r          restart: first remove every bridge connection a previous
                omcid left in the switch (with -a)
    -d          daemon: no frame or idle limit
    -w units    exit after this many idle 5 s units (default 24)
    -c hex      use this capability blob instead of the driver one
    -s state    use this ONU state for the resume decision instead of
                asking the driver (test only; see docs/BOOT.md, "Resume
                without re-registration")
    -g file     read the Alloc-IDs and the serial from this file instead
                of /proc/odi_gpon/status (test only)
    -l file     read the UNI link bit (0 or 1) from this file instead of
                the odi_switch host_link parameter (test only)
    -m file     read the bridge MAC address of class 46 from this file
                instead of /sys/class/net/eth0/address (test only)
    -i file     append the driver verbs of a reload to this file instead of
                writing /proc/odi_init (test only)
    -j seconds  how long a reload waits for O5 (default 150; test only)
    -f          start even if a live process holds redirect type 1
    -h          this text; starts nothing

At startup, before opening the OLT-facing socket, a respawned `omcid` also
decides whether to resume its MIB from `/var/run/omcid-mib.snap` instead of
starting empty -- see docs/BOOT.md, "Resume without re-registration". That
snapshot is written after every Create, Set or Delete, and deleted on a
MIB reset; none of it is a command-line concern beyond `-s`.

init runs `/etc/scripts/svc-omcid.sh`, a `respawn` entry that execs
`/bin/omcid -a -d` into `/var/log/omcid.log`, unless
`/etc/config/modules.off` is set; nothing else starts an omcid. Arguments are matched whole;
anything unrecognised exits before a socket is opened, and a second omcid
refuses to start while a live one holds redirect type 1 (`-f` overrides).

It reads, from the config store, at start and again on SIGHUP (below): the manual VLAN
(`VLAN_MANU_TAG_VID` and `VLAN_MANU_TAG_PRI`, applied only with
`VLAN_CFG_TYPE` 1 and `VLAN_MANU_MODE` 1, as the stock firmware gates them;
with `VLAN_CFG_TYPE` other than 1 it applies the OLT's own class 171 rule when
it has the one supported shape, otherwise the service is transparent,
`docs/SETTINGS.md`, "VLAN handling"),
the serial number until the kernel reports one (`GPON_SN`), the LOID keys
(answered in the CTC LOID-authentication entity), the six OLT identity keys
(the ONU-G hardware version, `ONU_HW_VERSION`, from `/etc/config/odi.conf`;
all six reported only while `/etc/config/omci-identity.on` exists), and
`DUAL_MGMT_MODE` and the `OMCI_CUSTOM_*` masks for display only; and, from
`/etc/config/odi.conf`, `OMCI_UNKNOWN_ME_OK` (its start-up line `unknown
entities: ...` says which way) `OMCI_PRIVATE_ME`, `OMCI_VEIP`, `OMCI_POTS_PORTS` and `OMCI_POTS_SLOT_ID` (the start-up
line `phone: ... in slot ...` when on); the VEIP slot (`OMCI_VEIP_SLOT_ID`,
with bit 256 of `OMCI_CUSTOM_ME`) and the private-entity switch are on the start-up line `VEIP in slot ...`. At every
Start software download, Activate and Commit from the OLT it reads
`OLT_SW_DOWNLOAD` from `/etc/config/odi.conf`: accept (the default) answers the
download and discards the image, reject refuses it; it never flashes and never
reboots (docs/SETTINGS.md, "Software download from the OLT"). On a Reboot
of the ONU-G it reads `OMCI_ACCEPT_REBOOT` the same way: off (the default) is
answered "command not supported", `1` is answered success and followed by a
SIGTERM to init a second later, `2` is answered success and nothing follows. Each key is
looked up in both store files, the one xmlconfig assigns it to first. Its
start-up line in the log says what it found (`store: loid ..., manual vlan
..., identity report ...`). A change takes effect with `apply.sh omci`, which
sends the SIGHUP; `docs/SETTINGS.md` has what each one does.

The T-CONT Alloc-IDs are the OLT's own. A T-CONT (ME 262) the OLT set over
OMCI keeps the Alloc-ID it was set to (G.988 9.2.2). One it never set -- ISP1
sets none -- takes the next Alloc-ID the OLT assigned by PLOAM that no set
T-CONT claims, in assignment order, from the kernel's list (`alloc_ids` in
`/proc/odi_gpon/status`, read once a second); with none left it reads 0x00FF,
unassigned, and is not programmed. That is what a MIB upload and a Get of the
AllocID answer, and what goes to the driver. When the kernel list changes (an
assignment, a deallocation, a re-ranging) the whole upstream is
reprogrammed a quiet second later; any other change reprograms only the
T-CONTs, queues and flows it changed (docs/SWITCH.md, upstream PON queues).
`omcli tcont` shows each binding and where it came from.

It logs every OMCI frame in and out (`<-` / `->` lines) and every driver
call to `/var/log/omcid.log`, about 1.7 MB a day on a busy OLT before the
trim. `omcli` and `omcicli` are its clients; `cat /proc/odi_omci` shows the
kernel side (who holds each redirect type, frames delivered and dropped).

On SIGINT or SIGTERM it deregisters from the OMCI channel and exits, and init
respawns it (see "Restarting a daemon" below). While it is not running the
OLT's OMCI goes unanswered; after a respawn it resumes from its snapshot or
starts with an empty MIB and keeps the switch programming already in place
until the OLT provisions it again.

**SIGHUP rereads the config store**, the Unix convention for a reload, and is
what `apply.sh omci` sends. The handler sets a flag and the main loop does the
work, so a second SIGHUP during a reload is folded into one more reload and the
watchdog ping never stops. omcid compares what it reads with what it is
running (only what the OLT or a connection would see: with the identity
switch off the report keys do not count) and does the least that applies:

| what changed | what omcid does | the ONU |
|---|---|---|
| nothing | logs `event=reload changed=none` | untouched |
| only `VLAN_CFG_TYPE`, `VLAN_MANU_MODE`, `VLAN_MANU_TAG_VID`, `VLAN_MANU_TAG_PRI` | tears the bridge connections down and builds them again from the MIB it holds, by the same `bdgconn_rebuild()` the first build uses | stays in O5, nothing re-ranged |
| `GPON_SN`, `GPON_PLOAM_PASSWD`, the LOID keys, `OMCI_SW_VER1/2`, `GPON_ONU_MODEL`, `OMCC_VER`, `OMCI_VENDOR_PRODUCT_CODE`, `ONU_HW_VERSION`, the identity switch, `OMCI_UNKNOWN_ME_OK`, `OMCI_FIXED_ME`, `OMCI_PRIVATE_ME`, `OMCI_VEIP`, `OMCI_POTS_PORTS`, `OMCI_POTS_SLOT_ID` (with ports), `OMCI_VEIP_SLOT_ID` (with `OMCI_CUSTOM_ME`), bit 64 of `OMCI_CUSTOM_ME` (with or without VLAN keys) | writes `gpondeact` to `/proc/odi_init`, clears its MIB as a MIB reset does (`event=mib_reset side=local`), holds three seconds, hands the driver the serial number (`gponsn`, only if it changed) and the password (`gponpw`), writes `gponact`, and waits for O5 and the services | deactivated and ranged again; the process is never restarted |

The re-registration is a state machine advanced by the main loop, so no step
sleeps: the CLI, the queues and the watchdog ping are served while the ONU is
down. It ends when `/proc/odi_gpon/status` says O5 and at least one service exists,
at `result=timeout` (O5 not reached) or `result=no_services` after 150 s, or
at `result=failed` when the driver refused `gpondeact` (nothing was touched,
and omcid goes back to the values it was running with, so the next SIGHUP
tries again) or `gponact` (the ONU is left off the line). A refused `gponsn`
does not stop the activation: the ONU comes back on the old serial number and
the result is `sn_not_applied`. The serial number is rewritten into the
PLOAM slot the boot activation armed, and only while the ONU is deactivated.

Three event lines tell the story (below): `event=reload changed=... action=...
keys=...` (key names, never values), `event=reload_step verb=... rc=...` for
each driver verb, and `event=reload_done ... result=... duration_s=...
[o5_s=...] services=N`. They are never rate limited. `/var/run/omcid-reload`
holds the same outcome for `apply.sh` (`id` pid.sequence, `state`, `changed`,
`action`, `result`, `keys`, `duration_ms`, `o5_ms`, `services`), replaced whole
by a rename; omcid removes a stale one at start.

### `confd` -- the web UI

The separate odi-ui project, fetched as a release. Serves the UI and its
JSON API on the port given as its only argument (`confd 80` here), HTTP
Basic with the credential in `/etc/config/confd.auth` (`user:password`;
default `admin` / `admin` while that file is absent). It reads its pages and
five `.tsv` tables (`keys`, `meta`, `consumers`, `features`, `settings`)
from `/etc/confd/`, and any file of the same name in
`/etc/config/confd/` overrides the shipped one; `settings.tsv` among them
is the list of keys this image reads and what applying each costs. It runs
`/etc/scripts/flash`, `/etc/scripts/apply.sh`, `/etc/scripts/fwu_starter.sh`,
`/bin/diag`, `/bin/omcicli`, `/bin/nv`, `/bin/ping` and `/sbin/reboot` for
its pages, is single-threaded (a ping or an `apply.sh omci` blocks every
other request for its duration; a firmware write no longer does), and logs
nothing of its own to speak of.
`docs/ACCESS.md` has the ports and the SSH-key API; `docs/SETTINGS.md` has
what each control does on this image, including the ones that cannot work
here.

### `metricsd` -- the Prometheus exporter

The separate odi-sfp-exporter project, fetched as a release. `metricsd [port]`,
default 9100, any path answers. One scrape runs one `diag` batch (optics,
ONU state, alarms, per-port MIB counters) and one `omcicli dump srvflow`,
and reads `/proc/meminfo`, `/proc/net/dev`, `/proc/loadavg`,
`/proc/uptime`, `/etc/odi-build` and, once per boot,
`/proc/odi_ramlog_prev`. `gpon_omci_services` -- how many
services the OLT provisioned -- is what tells "reachable but unconfigured"
apart from a working ONU; `gpon_image_info` says which image is running.
Its `path` label is `argv[0]`, so a hand-started copy is visible as such.
`gpon_boot_count` is this boot's ramlog boot counter (boots of this image
since the last power cycle), and `gpon_last_reset_reason{reason="..."} 1`
is why the previous boot ended, the `reason=` of `/proc/odi_ramlog_prev`
(`docs/HACKING.md`, "Reading a boot you could not see"): `wdt_client`
with a `client` label (e.g. `client="omcid"`), `wdt_mem`, `wdt_cpu_rx`, `wdt_userland`,
`reboot`, `halt`, `poweroff`, `panic`, `oops`, `power` or `unknown`. An
alert on `gpon_last_reset_reason{reason=~"wdt_.*"}` fires after a watchdog
reset. Both need an exporter newer than v1.1.2.

From the first odi-sfp-exporter release after v1.1.2 on, the
`gpon_provision_*` families say what the ISP provisioned, so a plan change --
a speed tier, a moved VLAN, another T-CONT -- shows up in Grafana:

| metric | from |
|---|---|
| `gpon_provision_tconts`, `gpon_provision_tcont_info{alloc_id}` | `/proc/odi_gpon/status`, `alloc_ids`: the Alloc-IDs the OLT assigned by PLOAM |
| `gpon_provision_gem_ports`, `gpon_provision_gem_port_info{gem_port,direction}` | `omcicli provision`: the GEM port network CTPs the OLT created |
| `gpon_provision_vlan_info{vlan,source}` | the same: each VID, once per place (`vlan_filter`, `ext_vlan_filter`, `ext_vlan_treatment`) |
| `gpon_provision_traffic_descriptors`, `gpon_provision_traffic_descriptor_{cir,pir}_bytes_per_second{descriptor}` | the same: the OLT's traffic descriptors (ME 280) and their rates |
| `gpon_provision_mib_entities`, `gpon_provision_mib_data_sync` | the same: what omcid holds, and the MIB data sync counter |

`omcicli provision` is one more fork per scrape, under the same 2 s bound as
`dump srvflow` and skipped when that one timed out. Its answer, one line per
item, all numbers decimal:

    tcont me=32768 alloc_id=282 index=0      a T-CONT omcid programmed
    gem me=2 port=1434 direction=3 tcont_me=32768 us_td=0 ds_td=0
    vlan vid=10 source=vlan_filter           sorted by VID, one per source
    td me=1 cir=12500000 pir=62500000 cbs=0 pbs=0
    summary rows=161 tconts=5 gem_ports=6 vlans=6 traffic_descriptors=0 services=6 mib_data_sync=184

The `tcont` lines are omcid's view (the T-CONTs its GEM ports use, with the
Alloc-ID a Get of the T-CONT returns); the exporter takes the T-CONTs from
`/proc/odi_gpon/status` instead, the OLT's own assignment.

From the first odi-sfp-exporter release after v1.2.1 on, the same read of
`/proc/odi_gpon/` gives the GPON link: every `ctr` total as a `*_total`
counter (`gpon_ds_bip_error_blocks_total`,
`gpon_ds_fec_codewords_total{result}`, `gpon_ds_bwmap_allocations_total`,
`gpon_us_gem_bytes_total`, ...), `gpon_alarm_events_total{alarm}` and
`gpon_alarm_seconds_total{alarm}`, `gpon_onu_state_seconds_total{state}`,
`gpon_onu_state_transitions_total{from,to}`, `gpon_fec_active{direction}`,
and the driver's PLOAM, REI, TO1/TO2 and interrupt counts. The exporter's
`docs/METRICS.md`, "The GPON link", has the list and the PromQL for a weak
fibre and a stuck ONU.

### `dropbear` -- ssh and scp

Started by `svc-dropbear.sh` (through `respawn.sh`) as

    /sbin/dropbear -F -r /etc/config/dropbear.d/ed25519 -D /etc/config/dropbear.d -p 22

(`-s` is added on an image built with `ROOT_PW=locked`, which refuses
password logins.)

No `-E`: dropbear logs through syslog by default (every login is logged),
same as everything else here that can reach it -- `logread` shows it. The
host key and `authorized_keys` live on the config partition, so both
survive a reflash; the keys file is read at every login, so adding a key
needs no restart. `/bin/scp` is the same multi-call binary (legacy scp
protocol: OpenSSH 9+ clients need `scp -O`). See `docs/ACCESS.md`.

### `syslogd`/`klogd` -- the system log

Two respawn entries, `svc-syslogd.sh`/`svc-klogd.sh`: syslogd in the
foreground with a 64 KB circular buffer (`-n -C64`), read with `logread`;
klogd (`-n`) forwards kernel messages into it. Neither exists on the stock
image. With `SYSLOG_SERVER`
set in the config store (docs/SETTINGS.md), syslogd also adds `-L -R
host[:port]`: forwarded remotely, kept in the local circular buffer too.
`apply.sh syslog` restarts it without a reboot.

The local buffer is small and lives in RAM, so for anything beyond a bench
setup set `SYSLOG_SERVER`. Alerting on the metrics side is covered by the
exporter project: a ready-made Prometheus rule file
([`prometheus/alerts.yml`](https://github.com/AndreZiviani/odi-sfp-exporter/blob/main/prometheus/alerts.yml))
and the notes on loading and routing it
([`docs/ALERTS.md`](https://github.com/AndreZiviani/odi-sfp-exporter/blob/main/docs/ALERTS.md)).

### `ntpd` -- NTP client (opt-in)

`svc-ntpd.sh` starts busybox ntpd (`-n -p NTP_SERVER`) only while
`NTP_SERVER` is set in the config store (docs/SETTINGS.md); unset, the
respawn entry runs the same off-flag placeholder every other disabled
service uses, and does nothing. Also new versus stock, which has no RTC
and no NTP client at all. `apply.sh ntp` starts or stops it, live, when
the setting changes.

ntpd runs `/etc/scripts/ntp-event.sh` (`-S`) on each of its sync events
(`unsync`, `step`, `stratum`, `periodic`), which writes `/tmp/ntp-status`
(`event`, `time`, `offset`, `stratum`, `last_sync`, one `key=value` per
line; `last_sync` is the epoch time of the last event that was not
`unsync`, empty if never) by renaming a temp file over the old one.
`svc-ntpd.sh` removes the file on every start, so a new server or a
disabled NTP never shows an old sync.

### `igmpd` -- IGMP snooping (opt-in)

    igmpd [-w] [-d] [-6] [-f] [-n COUNT]
    igmpd [-6] -r FILE
    igmpd -s [STATE [POLICY [STATE6]]]
    igmpd [-w] -j GROUP -p PORTS [-v VID]
    igmpd [-w] -l GROUP [-v VID]

    -w         WRITE to the switch. Off by default: the daemon then prints
               what it would do and holds no lease
    -d         also drop multicast no row names on the PON side, after one
               membership interval (IGMP_PON_DROP)
    -6         MLD (IPv6) as well (MLD_SNOOPING; below)
    -n COUNT   stop after COUNT frames (0, the default, is forever)
    -f         start even if a live process holds uid 4
    -j GROUP   one-shot: program GROUP with member PORTS (hardware port
               bits, 0x1 the UNI) and exit
    -l GROUP   one-shot: remove GROUP and exit
    -r FILE    replay a transcript of timestamped events (- is stdin) and
               print what the daemon decides; no device, no uid 4
    -s         print the policy in force, the groups held and the last
               refused joins, then exit (details below)
    -v VID     the VID a one-shot names (default 0); printed only, the
               entry is keyed on filtering id 0 (SVL)
    -h         this text; registers nothing

**Replay.** `igmpd -r FILE` runs the daemon's own decision code (frame
parse, group table, policy, the rows and the lease it would write) on a
transcript instead of the switch, with a clock the transcript sets, and prints
the result; it never opens `/dev/odi_sw` or the redirect device and refuses
`-w`. One event a line, `#` comments:

    0 policy                       # the lines up to "end" are the policy file
    snooping=1
    end
    0 tick                         # the daemon's once-a-second step at T
    1 v2join 1 0 192.168.1.10 239.1.1.1    # T VERB PORT VID SRC GROUP
    5 v2leave 1 0 192.168.1.10 239.1.1.1
    7 rx 010000...                 # a raw payload: port (one-based), VID, frame
    9 state                        # the state file it would write

Other events: `v3join`, `v3leave`, `v3src incl|excl PORT VID SRC GROUP S1[,S2..]`
(a v3 record that names sources), `query PORT VID SRC`, `policy-file PATH`,
`nopolicy`, `status` (what `igmpd -s` prints), `ctr GROUP BYTES` (what the
counter of the group's MAC has counted, read on the next second as the
switch's would be), `meter` (the meter file it would write, below), and the
MLD ones, which reach the IPv6 table only with `-6`: `mld1join`, `mld1done`,
`mld2join`, `mld2leave` (`T VERB PORT VID SRC6 GROUP6`) and `mldquery PORT VID SRC6`. A policy change is read at the next second the clock moves to.
While the policy has the reports taken (below), the frames the verbs build are
marked taken as the kernel marks them (an `rx` payload says so itself, bit 7 of
its second byte), and the transcript shows what would go up and with which
tag (`up:` lines); with tag control 0 the tag is the classifier row's, which
only the kernel can say, so the line says only that.
Golden transcripts are in `src/igmp/test/replay`, run by `make test-igmp`;
`src/omci/mcast-test.sh` replays the policy file omcid wrote.

**Why a channel does not play.** `igmpd -s` answers it on the stick, without
touching the switch or the redirect channel: the policy in force with its
ranges, the counters, the groups held, and the last refused joins, newest
first, each with its VLAN, the reason and how long ago:

    policy: snooping, function 0, immediate leave, 2/125/1 s, limit 2, 1 allow + 0 static ranges
      allow 225.0.0.0 - 225.255.255.255
    joins 3, refused by the profile 2 (lists 1, group limit 1), table full 0
    groups held: 2
      225.1.1.1
      225.1.1.2
    recent refusals, newest first:
      239.9.9.9  vid 0  outside the access control lists (acl)  12 s ago
      225.1.1.3  vid 0  past the group limit (limit)  x3  40 s ago

The four reasons are `acl` (outside the lists the OLT provisioned), `limit`
(past the group limit of ME 310), `bw` (past the bandwidth of ME 310, when
the OLT enforces it; below) and `full` (igmpd's own table of 128 entries
has no free slot; not counted in ME 311's refused figure). A host that
repeats the same refused report (an unsolicited report is sent twice) makes
one entry with a count, and the ring keeps the last 8 distinct ones: it
goes into the state file, which omcid reads in 4096 bytes next to up to 128
group lines, so it is small and fixed, and a set-top box re-reports the
group it wants every query interval, so a few entries show what is being
asked for. The report is read from `/var/run/odi-mcast.state` (written by the
daemon only under `-w`, which is how the service runs it) and
`/var/run/odi-mcast.conf`; naming two files reads those instead, e.g. copies
taken off a stick, and then times are the daemon's seconds, not ages.
Nothing here is OMCI: the OLT sees the same ME 311 counters as before, and
`denied` still holds the refusals by the lists and the limit. While the
reports are taken (below), a second line under the policy says how they go
up, and a line under the joins counts them:

      upstream: reports taken and sent by igmpd; tag added, vid 420 pri 5; at most 2 a second; unauthorized joins withheld
    sent up 40, withheld 3, over the rate 0, not sent 0

With `-6` an MLD part follows, in the same shape, from
`/var/run/odi-mcast6.state` (or the third file named): the IPv6 ranges of the
lists, the MLD counts, the IPv6 groups held and the last IPv6 refusals. It
is printed only when there is an IPv6 range or an IPv6 state file, so the
report of a daemon without `-6` reads as before.

`withheld` counts the messages of joins outside the lists alone and the
queries from the host side; `not sent` the ones that should have gone and
could not (no classifier row takes the frame, which the switch would have
dropped too, or a failed send), which the log names each time.

**Off unless asked for.** `svc-igmpd.sh` runs `igmpd -w` only while
`IGMP_SNOOPING=1` is in the config store (`apply.sh igmp` applies a change);
otherwise the respawn entry parks and the switch floods multicast as it
always did. With the key set the daemon is still idle until the OLT
provisions what it needs (below), so setting it on a line that carries no
multicast does nothing. The PON-side drop of IPv4 multicast no row names
(`igmpd -d`) is a separate opt-in, `IGMP_PON_DROP`: only `1` drops, unset
snoops only (an existing `IGMP_SNOOPING=1` config that wants the drop must
now set `IGMP_PON_DROP=1`). Without `-d` the PON keeps flooding and only the
copy, the rows and the state run. MLD is a third opt-in, `MLD_SNOOPING`
(`igmpd -6`, below); unset, the daemon runs with the command line it always
had.

**What the OLT decides.** omcid writes what it was provisioned to
`/var/run/odi-mcast.conf` (a quiet second after the last OMCI frame, only
when it changed, or with the 5 s cap on a rebuild of the QoS or the bridge
connections; a Set that changes only the multicast profile is not capped and
waits for the quiet second; the format is in `src/omci/respond/mcast.c`): snooping is
on when a multicast GEM interworking TP (ME 281) holds an address row and
the multicast operations profile (ME 309) of the UNI's subscriber (ME 310)
says IGMP function 0. The profile also supplies

- the timers: robustness, query interval and maximum response time give the
  membership interval (260 s at the defaults), robustness times the last
  member query interval the time a member that left is kept. igmpd never
  sees the OLT's queries, only the host answering them, so a group that the
  host re-reports at a longer interval than the profile says is held for
  robustness times that observed interval plus the response time instead
  (never less than the profile's figure, at most 1800 s; the first
  membership of a group still runs on the profile alone);
- immediate leave: the port is removed on the leave itself. Without it
  igmpd cannot send the group-specific query a router would send, so it keeps
  the leaving port for the last-member time and relies on the OLT's own
  query reaching any other set-top box behind that port: its report keeps
  the port. An OLT that does not query leaves such a box without the stream
  until its next report;
- the group limit of ME 310 and the dynamic and static access control lists
  of ME 309: a join outside the lists, or past the limit, gets no row, so
  with the PON's drop on the stream never reaches the host. An empty list
  restricts nothing (the OLT is the authority; a table nobody provisioned
  must not block a stream), and so does a list longer than 32 ranges. A
  static range of up to 16 groups becomes a row a group with the UNI as
  member and never expires; a wider one cannot be named in the switch, and
  igmpd then leaves the PON's drop off so the range still flows.

The access control lists are read as G.988 9.3.27 lays them out: a logical
row is a part 0 row (GEM port, VLAN, IPv4 source, start and end of the
range) and, from OMCC version 0xA1 on, optional parts 1 (IPv6 source prefix,
preview fields) and 2 (IPv6 destination prefix) of the same row key. Only
part 0 makes a range. A row whose part 1 or part 2 prefix is not the IPv4
form (ten zero bytes, then 0000 or ffff) is an IPv6 row and is left out of
the IPv4 ranges; so a list of IPv6 rows alone restricts nothing of IPv4,
never an IPv4 list that refuses every join. A row whose part 2 is IPv6 is an
IPv6 range (part 2's twelve bytes, then the last four of part 0's start and
end), written as `allow6=` or `static6=` for MLD (below); one whose part 1
alone is IPv6 has an IPv6 source and IPv4 destinations, and is neither (no
source is filtered).

What more a row says, when it says it (an empty field is a wildcard, so a row
that says nothing behaves as a bare range always did):

- the VLAN (ANI): a join is held to the row of its own VLAN, so ranges of
  different VLANs stay apart. A row's VLAN is the one its stream runs on
  downstream, and the message asking for it need not be on that VLAN (the
  upstream IGMP tag control may put another), so a join is held to the rows'
  VLANs only when its own VLAN, as it goes up (the profile's tag with control
  1 to 3, the message's own tag with control 0), is one that some row names.
  A join on any other VLAN, or an untagged one with control 0, is judged by the
  ranges alone;
- the source address (SSM): an IGMPv3 record that names sources is held to it,
  INCLUDE needing the row's source among them and EXCLUDE not naming it. A v1
  or v2 report cannot name one and goes in on the group alone;
- the GEM port and the imputed bandwidth are read and carried, not used
  (`struct igmp_range`, `imputed_bw`, for the bandwidth limit);
- the preview (row part 1): a group of a row with a preview length is
  allowed that many seconds, then removed whatever the host keeps reporting,
  and refused until the row's repeat time has passed since it ended (or since
  the host left); at most the row's repeat count of previews, counted per group,
  reset every 24 hours unless the reset time is 0 or 255 (the OLT's own reset
  action is not seen). A group a row with no preview also covers is full
  access. The refusals count under `preview` in the state file;
- the allowed preview groups table of ME 310 (attribute 7), IPv4 rows only:
  a group in it is fully authorized for its minutes (0: until the OLT removes
  it), ahead of the preview rows, even if no row lists it. The ONU's own count
  down of the time left starts when igmpd first sees the row (at the first
  join that uses it, if the daemon has no clock yet), and a row the OLT
  lengthens keeps its start.

The profile's IGMP version (ME 309 attribute 1) only counts the host's messages
above it (a v3 report under version 2, a v2 report or leave under version 1):
they are handled as any other, since a set-top box does not fall back to an older
version when it sees no query, and the count shows as `above_version` in the
state file and `igmpd -s`. The
querier address (attribute 11) is stored and shown, nothing is sent from it.

A Set or Create of ME 309 refuses, with result 9 and the attribute in the mask,
a value G.988 does not define for the IGMP version (1, 2, 3, 16, 17), the IGMP
function (0 to 2), immediate leave and unauthorized join behaviour (0, 1) and
the upstream tag control (0 to 3). Every defined value is accepted whether or
not it is done: function 1 and 2 are stored and the multicast stays
transparent.

**Not supported yet: the multicast service package table.** G.988 lets the
service package table of ME 310 (attribute 6) choose the profile by the VLAN
of the join, and says the profile pointer (attribute 2) is then ignored. This
daemon does not do that: the profile named by the pointer is always the one
exported, whatever the table holds, and an OLT that gives several profiles to
one UNI by VLAN gets the pointer's profile for every join. omcid logs
`mcast: 310/<id> has a multicast service package table, which does not select
a profile by VID here` once when the OLT first provisions the table.

An IGMP function other than 0 (snooping with proxy reporting, proxy) is not
done: the OLT then expects the ONU to answer its queries, which this one
does not, so the multicast stays transparent.

**The upstream IGMP tag, the rate and unauthorized joins** (ME 309 attributes
4 to 6 and 15, G.988 9.3.27). The switch forwards the host's IGMP like any
other frame, by the extended VLAN rules, which is what tag control 0 asks.
When the profile asks for more, the copy becomes a take: the switch hands
the host's IGMP to the CPU and forwards none of it, and igmpd sends each
message up itself. That happens when the profile has a tag control of 1 to 3,
a rate other than 0, or access control lists that restrict something with
unauthorized joins discarded (attribute 15 false, the G.988 default). Then:

- a message whose joins the lists all refuse is withheld (with attribute 15
  true it goes); a leave, an allowed join, a join past the group limit and a
  message igmpd cannot parse go up as the switch would have sent them;
- a query from the host side is not sent: the OLT is the querier;
- no more than the profile's rate of messages go up in one second;
- tag control 1 puts the profile's TCI in front as a C-tag (TPID 0x8100);
  2 replaces the whole TCI of the outer tag, 3 its VID alone (priority and
  DEI kept), and both add the TCI as 1 does when the frame is untagged. The
  message goes up the flow of the classifier row that carries that VLAN (and
  priority) upstream, or, when no row does, the flow the host's own frame
  would have taken;
- tag control 0: the message gets the tag treatment of the classifier row its
  frame matches (the extended VLAN discards first, then the lowest matching
  row) and goes up that row's flow. A frame no row takes, or one a discard
  drops, is not sent, as the switch would not have sent it.

The kernel says which row and flow (`ODI_SW_IOC_MC_ROUTE`) and does the
send (`ODI_SW_IOC_MC_SEND`), and sends only for the holder of the lease, only
while it holds the take, only an IPv4 IGMP frame to an IPv4 multicast MAC,
(or, with MLD, an MLD report or done to an IPv6 multicast MAC), and only on a
flow an upstream row queues on (`docs/SWITCH.md`, "Snooping").
If igmpd dies, the lease goes with its descriptor, the rule with it, and the
switch forwards the host's IGMP by itself again.

**What it does with the switch.** While snooping is on the daemon holds a
lease on the switch (an ioctl on its open `/dev/odi_sw`, `docs/KERNEL.md`):

1. an ACL rule copies IGMP entering the UNI port to the CPU. The frame is
   forwarded exactly as before (the OLT sees every report and leave) and a
   copy goes to igmpd on redirect type 4. When the profile asks for an
   upstream tag, a rate or the discard of unauthorized joins, the same rule
   takes the frame instead and igmpd sends it up (above);
2. for one membership interval the switch keeps flooding, so every group the
   host already watches can report again; then
3. IPv4 multicast from the PON that no row names is dropped, except
   224.0.0.0/24 (the OLT's general queries go to 224.0.0.1).

Every row has the host and the PON as members: the PON because the OLT side
is where the querier is, and a report to a group is forwarded to the row's
members; without it the report would go nowhere. The CPU is never a member.
If the daemon dies or is stopped, its descriptor closes, the lease goes with
it, the rows it wrote are deleted and the PON floods again.

**Nothing from the fibre reaches the CPU.** The ACL rule has the UNI port as
its only ingress, so a frame from the PON never matches it, the OLT's
queries are not copied (the host answers them itself and the answers are the
reports the daemon sees), and the NIC still drops every PON frame that is
not OMCI. The daemon's one transmit is the host's own report, taken, sent
toward the OLT; it never answers anything from the fibre.

**What it reports.** `/var/run/odi-mcast.state` holds the join count and the
groups the subscriber holds; omcid answers the multicast subscriber monitor
(ME 311) from it: the join message counter, the joins past the bandwidth
(attribute 4) and, when there is no meter file, the IPv4 active group list
with the group alone.

**Lost groups and bandwidth.** While it holds the lease, igmpd meters each
group it holds from the dynamic lists with a counter in the switch (`diag
mc-count` installs the same by hand; 16 of them, one per group MAC, on the
PON ingress, counting bytes, read once a second). A group whose counter has
grown since it went in and then stood still for 10 s is **lost**, and flows
again the second it grows; the log says both (`lost: 239.1.1.1: nothing
arrived for 10 s`, `flowing again: 239.1.1.1`). A counter that has never
grown says nothing: that group is **unknown**, never lost, and so is a group
past the sixteenth MAC. That a counter on the PON ingress counts at all has
not been measured on a stick yet (`docs/SWITCH.md`, "Multicast group
counters"); until it is, a stick on which it does not reads every group as
unknown, and the OLT is told nothing wrong. `/var/run/odi-mcast.meter`, a
file per group, has what omcid answers from it:

    bps=1500
    lost=1
    group=239.1.2.3 vid=10 client=192.168.1.10 joined=4211 bps=1000 flow=up
    group=225.200.7.9 vid=0 client=192.168.1.11 joined=4302 bps=500 flow=lost

the bytes a second of the metered groups (over the last 5 s, the FCS in it),
how many are lost, and per (VLAN, group) the client of its last join, the
second of it on the monotonic clock, its bandwidth and its flow. omcid reads
it only while the policy asks for snooping, and from it answers ME 311's
bandwidth (attribute 2) and the active group list with the VLAN, the
bandwidth, the client and the seconds since the join; the lost groups list of
the subscriber's multicast profile (ME 309 attribute 9: VLAN, source 0, the
group); and that profile's alarm 0, raised while a group is lost.

**Bandwidth of ME 310.** When the OLT sets a maximum multicast bandwidth on
the subscriber (attribute 4, bytes a second), each new group a host joins is
weighed by the bandwidth the dynamic list imputes to it (bytes 18 to 21 of
its row): a join that would take the total of the groups held past the
maximum is counted (`bw_exceeded`, ME 311 attribute 4), and refused only
when attribute 5 enforces it (reason `bw`). A group already held is never
refused, so lowering the maximum cuts no stream that plays, and a range that
imputes 0 is outside the limit (G.988). With no maximum, the policy file and
the decisions are as they were. `igmpd -s` adds a line under the policy:

      bandwidth: at most 1000 bytes a second, enforced; ranges with an imputed one: 1

and one under the joins once anything went past it.

The downstream tag control (attribute 16) is not igmpd's: omcid puts it in
the bridge rules of the multicast port, on the VLANs the access control lists
name (`docs/SWITCH.md`, "Downstream tag control").

**MLD (IPv6), opt-in.** Without `-6` IPv6 multicast is flooded as it always
was and an MLD frame is not the daemon's. With `MLD_SNOOPING=1`
(`svc-igmpd.sh` adds `-6`; it needs `IGMP_SNOOPING=1`, and `apply.sh igmp`
applies a change) the host's MLD is snooped as its IGMP is, on the same
profile (ME 309 is the IGMP/MLD profile):

- the switch copies (or, while the reports are taken, takes) the MLD queries,
  v1 reports, v1 dones and v2 reports entering the UNI port (three more ACL
  rules, `docs/SWITCH.md`, "Snooping"), and nothing else of ICMPv6: neighbour
  and router discovery are never copied or taken;
- the IPv6 groups go into a table of their own (128 groups), one row a group
  keyed on its MAC, 33:33 and the low 32 bits of the address, the host and
  the PON as members, by the profile's timers and immediate leave; an MLD
  join is held to the group limit of ME 310 with the IPv4 groups counted in
  (an IGMP join still counts the IPv4 groups alone);
- the IPv6 rows of the access control lists (`allow6=`, `static6=` in the
  policy file) restrict IPv6 joins as the IPv4 rows restrict IGMP ones, with
  the same refusals in the same ring shape; IPv4 rows alone restrict nothing
  of IPv6, and IPv6 rows alone nothing of IPv4;
- the way up is the IGMP one: the same take, tag, rate and unauthorized-join
  rule, and the same counts;
- with `-d`, after the learning interval, the PON also drops IPv6 multicast
  no row names, and the switch is told to flood the link-scope groups
  (ff02::/16) and the solicited-node groups whatever the drop says.

A group of scope 2 or less (ff02::/16 and every other link-local scope) is
never snooped: no state, no row, no refusal, whatever the lists say. Router
advertisements (ff02::1), DHCPv6 (ff02::1:2), MLDv2 itself (ff02::16) and
neighbour discovery (the solicited-node groups) ride them, and another open
implementation's measurements on this chip saw router advertisements and
DHCPv6 stop when they were pruned. A group whose MAC aliases one of them
(ff3e::1 has the MAC of ff02::1) is snooped all the same: a row names the UNI
and the PON, which is where flooding sends the frame, so it prunes nothing.
The IPv6 state (counts, groups, refusals) is written to
`/var/run/odi-mcast6.state`, which omcid does not read: ME 311's IPv6 active
group list stays empty. MLD function 1 and 2 (proxy) are transparent, as
for IGMP.

`-j` and `-l` drive the row path by hand, without registering anything, so
an entry can be put in and checked without an IGMP frame:

    igmpd -w -j 239.1.2.3 -p 0x5        # members: the UNI and the PON
    diag l2-table get all               # 01:00:5E:01:02:03 ... SVL mc 0x5
    igmpd -w -l 239.1.2.3               # a second -l: no such entry in the switch

Without `-w` both print the entry they would write and touch nothing. Rows
written this way are not under a lease: they stay until `-l`.

## CLIs

### `diag` -- the switch, GPON and optics CLI

Our own CLI over our kernel's interfaces. `src/diag/README.md` is the full
reference; the commands:

    pon get transceiver vendor-name|part-number|temperature|voltage|
                        bias-current|tx-power|rx-power|all|alarm-status
    gpon get onu-state                GPON state machine state, O1-O7
    gpon get alarm-status             LOS, LOF and LOM, live; SF and SD
    gpon get flows                    GEM flows omcid programmed
    gpon get flow-counters            per-flow GEM counters (/proc/odi_gpon/flows)
    mib dump counter port <ports>     port MIB counters; all, 2, 0-3, 0,2-3
    l2-table get all                  the MAC table: every valid L2 table row
    l2-table get entry address valid  the same, in the stock spelling
    l2-table get index <index>        one row by number, with its raw words
    register get <address> <words>    switch-core register read
    register set <address> <value>    switch-core register write
    acl-probe count|bytes <slot> port <port> proto <proto>
    acl-probe count|bytes <slot> port <port> group <group> <length>
                                      count IPv4 entering port 0 (UNI) or 2 (PON)
    acl-probe mirror|trap <slot> igmp [bytes]
                                      the UNI IGMP copied or taken to the CPU
    acl-probe show <slot>             a probe, its count, what reached the CPU
    acl-probe delete <slot>|all       remove probes
    mc-count frames|bytes <slot> pon|uni mac <high> <low> <length>
    mc-count frames|bytes <slot> pon|uni ipv4 <address> <length>
                                      count a multicast destination entering
                                      the PON or the UNI (igmpd's counters)
    mc-count show <slot>              a counter and what it counted
    mc-count delete <slot>|all        remove counters
    help                              the list, with descriptions
    exit

Keywords may be shortened while unambiguous (`reg g 0x1d0 1`), and a
trailing `?` lists what matches. Two ways to run it:

    diag pon get transceiver rx-power     # one command from argv, then exit
    printf 'gpon get onu-state\nexit\n' | diag   # commands on stdin, one per line

On stdin it reads until `exit` or end of input. A caller whose stdin never
closes leaves diag waiting for the next line forever, so scripts feed it a
pipe or a file and wrap it in `timeout` (rcS and network.sh use
`timeout 10`). Over ssh, pipe inside the remote command
(`ssh root@<stick> 'printf "...\n" | diag'`): `ssh root@<stick> diag < file`
reaches diag as an immediate end of input.

`l2-table get all` walks the switch L2 lookup table (1,024 hashed rows,
plus the 64 CAM rows while those are on, as they are on this image: 1,088)
and prints one line
per valid row:

    L2 table: 1088 rows, IPv4 multicast looked up on MAC + VID/FID
    MACAddress        Spa Fid Age Vid  State  Ext Hash Type Ports Index
    02:00:00:00:00:01 0   0   7   0    Auto   0   SVL  uc   -     0x06c
    01:00:5E:01:02:03 -   -   -   0    Static -   SVL  mc   0x1   0x17c
    02:00:00:00:00:02 2   0   7   14   Auto   0   SVL  uc   -     0x270 ctag
    02:00:00:00:00:03 0   0   7   0    Auto   0   SVL  uc   -     0x364
    02:00:00:00:00:04 3   0   7   0    Auto   0   SVL  uc   -     0x390
    5 entries

(a trial stick, with `igmpd -w -j 239.1.2.3 -p 0x1` in place: the host
on the UNI, the OLT side on the PON with its C-tag, the stick own CPU
port, and the static group.)

`Spa` is the port a unicast address was learned on (0 the UNI, 2 the PON,
3 the CPU), `Age` counts down from 7 and stops at 0, `Auto` is learned and
`Static` written. A multicast row has no source port or age and lists its
member ports instead. The header words are the stock listing's, which is
what lets the web UI read either; `l2-table get entry address valid` is
kept as a second spelling because confd sends it, and the same confd runs on
the stock slot. `l2-table get index <n>` reads one row whatever its state
and prints its three raw words, for checking the layout against the
hardware. All three are read-only.

Which rows are listed is the table engine answer, not a bit of the row:
after each row read the engine reports whether the row holds an entry
(TABLE_STATUS.HIT, 0x012004 bit 12). The row own bit 77, which a write sets
to place an entry, reads back set on every row, empty ones included, and an
empty row reads back with its bucket in the low MAC octet -- so a listing
that trusted bit 77 showed all 1,024 hashed rows as `00:00:00:00:00:NN`. `get
index` still prints any row, with `valid yes` or `valid no` from the
engine.

The commands the exporter runs keep the stock CLI's syntax and output byte
for byte, and `register get` keeps its `0x<address> 0x<value>` layout for
rcS and network.sh. `register set` writes the switch core
directly: it is how rcS and network.sh program the optics and the host
SerDes, and a wrong value there can take the host link away.

`acl-probe` is for measuring the switch, and nothing in the image runs it.
It installs one of eight ACL rules (entries 88 to 95) that counts the IPv4
frames of one protocol, or to one address prefix, entering the UNI or the
PON, in frames or bytes, without changing where they go; or that copies
(`mirror`) or takes (`trap`) the IGMP entering the UNI to the CPU. It
refuses anything else to the CPU, and anything at all from the PON to the
CPU. The rules belong to the diag process that installed the first one and
go when it exits, so they are installed and read from one diag fed on
stdin; `show` works from any process. It refuses while igmpd holds its
snooping lease, and igmpd is refused while probes are in.
`tools/mcast-probe/mcast-probe.sh` runs the measurements
(`docs/SWITCH.md`, "Multicast").

`mc-count` installs by hand the counters igmpd meters its groups with: one of
sixteen ACL rules (entries 48 to 63) counting, in frames or in bytes, what
enters the PON or the UNI to a destination MAC prefix (the first two octets,
the last four and a prefix length: `mac 0x3333 0x00000001 48` is
33:33:00:00:00:01) or to an IPv4 prefix. It only counts. Held like the
probes, by the diag that installed the first, and the probes and these
exclude each other; while igmpd meters its groups neither goes in.
`tools/mcast-probe/count-proof.sh` is the measurement that proves a counter
on the PON ingress counts (`docs/SWITCH.md`, "Multicast group counters").

### `i2cget`, `i2cdump`, `i2cset` -- the optics module over I2C

The module's own I2C devices are on `/dev/i2c-0`, the SoC optics port
(`odi_i2c_adapter.c`): 0x50 is A0h, the identification EEPROM (vendor,
part number, serial), and 0x51 is A2h, the diagnostics, served by the
laser driver chip. The busybox applets read them:

    i2cdump -y 0 0x50               # A0h, all 256 bytes
    i2cdump -y 0 0x51               # A2h: DDM in 96-105, page select at 127,
                                    # the selected page in 128-255
    i2cget -y 0 0x51 0x7f           # which page is selected
    i2cset -y 0 0x51 0x7f 2         # select page 2

The controller does one byte per transaction, behind a device select and a
byte address, so the adapter offers byte-data reads (`i2cget`, `i2cdump`
mode `b`, the default), I2C block reads (`i2cdump` mode `i`) and nothing
else: `i2cdetect` cannot scan it, since there is no transaction without a
byte address. Only 0x50 and 0x51 are answered; any other address fails
with "No such device or address", because the controller does not report
a missing device and a read there would return the previous byte read. Every transaction is bounded in the kernel (5 ms a byte) and
shares one lock with the exporter's DDM reads, so neither needs pausing.

The one write the kernel accepts is the page select, A2h byte 127. Any
other `i2cset` fails with "Operation not permitted" before anything reaches
the bus: the other pages are the laser driver's own tables, and a stray
write there could change how the laser is driven. To look at a page, note
the current one and put it back afterwards:

    p=$(i2cget -y 0 0x51 0x7f); i2cset -y 0 0x51 0x7f 2
    i2cget -y 0 0x51 0xa0           # page 2, byte 0xa0
    i2cset -y 0 0x51 0x7f "$p"

On the ISP1 stick page 2 byte 0xa0 reads 0x6a, which the stock firmware's
start-up takes for a Semtech GN25L95 laser driver. A0h bytes 68-83 are the
module serial number: mask them before posting a dump. `diag-bundle.sh`
collects all of this, masked, under `module/`.

### `omcli` and `omcicli` -- the omcid client

One binary; `/bin/omcicli` is a symlink to `omcli`. When omcid is running,
omcli uses omcid's own queue and commands:

    omcli state                          serial, device, ONU state, MIB sync,
                                         dropped frames, and the MIB resets
                                         since omcid started (`mib resets`:
                                         from the OLT, local, from the OLT in
                                         the last 600 s, whether that is a
                                         loop now, and how many loops)
    omcli provision                      what the OLT provisioned: T-CONTs,
                                         GEM ports, VLANs, traffic descriptors
                                         (the format is under metricsd)
    omcli conn                           the bridge connections omcid built
    omcli flows                          the GEM flow tables, per direction
    omcli mib [all|classId] [entityId] [--secrets]
                                         every managed entity, the ONU's own
                                         and the OLT's, with the values a Get
                                         returns; ends with an `N rows` line.
                                         Credentials the OLT provisioned are
                                         masked, see below
    omcli voip [--secrets | --secrets-list]
                                         the phone lines the OLT provisioned,
                                         one block per POTS port; see below
    omcli caps                           the device capability blob, decoded
    omcli tcont                          T-CONT entity id to driver index, the
                                         PLOAM Alloc-IDs, and each T-CONT's
                                         Alloc-ID and where it came from
    omcli vlan [cs.xml]                  the manual VLAN from the config store,
                                         and whether the tag is applied
    omcli ident [cs.xml hs.xml [odi.conf]]  the identity and the OLT
                                         identity keys, and whether those are
                                         reported (passwords are never printed)
    omcli bridge <ingress|any> <gem> <dir>   build one bridge rule by hand
    omcli cfgset <file> <dir> <key> <v>  write one key into a config store
    omcli help

`bridge` and `cfgset` write; the rest read. A rule built with `bridge` lasts
until omcid next rebuilds its bridge connections, which withdraws every
service it holds. The file arguments of `vlan` and
`ident` are for tests: omcid loads what they name in place of the store, and
keeps using it until it restarts. The stock command shapes
(`get sn|log|logfile|devmode|dmmode|cflag|version`, `mib get <class>
[entity]`, `dump conn|srvflow|qmap|tasks`) are answered by omcid too, in the
stock output format, which is what `confd`'s MIB and Services pages and the
exporter's `omcicli dump srvflow` rely on. `omcli` with no arguments prints
the stock command list. omcid implements none of the setters in it
(`set log|logfile|sn|devmode|dmmode|loid|resetlauth`, `mib set|create|delete`,
`debug simavc`; the list marks them `[stock daemon only]`; omcid accepts `set
logfile` and does nothing with it): when omcid is listening, `omcli`
says `omcid does not implement ...; nothing was sent` and exits 1, without
sending, with or without `-f` (and for `--vendor --id N`). They are sent only to
the stock daemon, which does implement them. Change the MIB with the OLT, or
set a key in `odi.conf` and run `apply.sh omci`. `mib reset` is the only
setter omcid carries out. `test/omcli_table_test.sh` compares the marks with
`vq_dispatch()` in `respond/vqsrv.c`.
`omcicli get tables` is safe against our omcid (it has no lock to deadlock
on), but the stock daemon wedges on it, so the web UI refuses it outright.

Secrets in the MIB dump: the username, password and realm of an
authentication security method (class 148), and the parts of a large string
(157, which can hold a URL with `user:password@` in it) or an octet string
(307), and the display name of a SIP user (153, the subscriber's own) are
shown as `**` for every byte, in `omcli mib`, `omcicli mib get` and
`mib getattr`. The line keeps its width and its key, so a parser sees the same
fields; an attribute that was never set still shows its zeros. `--secrets`
(anywhere on the command line, native or `--vendor`) prints the real bytes.
omcid also keeps the contents of a Create or Set of those classes out of its
log. The diag bundle never passes `--secrets`.

`omcli voip` follows the pointers between the SIP entities an OLT provisions
and prints, per phone port: the SIP user (number and display name), the login
(username, realm), the SIP agent (proxy, outbound proxy, registrar, host part,
port and protocol, DNS, registration expiry), the media profile (codecs and
packet periods, DTMF mode, RTP ports and DSCP), the voice service profile, the
voice VLAN and priority the OLT sets for the IP host the voice rides on, and
the line status the stick reports to the OLT ("config done", never
"registered": the stick does not register). Server URLs are printed without
any `user:password@`. By default the number, the display name and the login
name are masked the way the diag bundle masks the serial (`MASKED-` and the
last four characters), and the password is only said to be set. `--secrets`
prints all of them in clear, the one place a SIP password is printed.
`--secrets-list` prints nothing but the secret strings, one per line (number,
display name, usernames, passwords, and the user and password cut out of any
URL), for a scrub list. An authentication method (class 148) counts as a SIP
credential only when a SIP user or one of the agent's servers points at it:
the same class holds the TR-069 login, which `voip` never shows. With no
voice provisioned it prints `no voice provisioned`.

Maintenance flags: `--rmq KEY` removes a message queue left by a killed
client (there is no `ipcrm` in the busybox), `--inject HEX` feeds a 48-byte
OMCI frame in as if it came off the line, with the trailer and CRC filled in
(omcid drops a frame whose trailer or CRC is wrong; `--inject-raw HEX` sends
bytes exactly as given, of any length, to test that; `--inject-file F` every
frame in F, one per line, back to back), `--vendor` talks to a stock
daemon instead, `--id N [-f]` sends a raw command id.

### `omciprobe` -- one raw driver command

    omciprobe [-f] <cmd|name> [hexbyte ...]
    omciprobe -l            list the known commands
    omciprobe -h            this text

Sends one OMCI driver command to odi_switch over the odi_omci netlink
socket and dumps the reply, bypassing omcid. Never registers for the OMCI
channel. Only read-only commands run without `-f`; a write sent this way
reconfigures the switch behind omcid's back, and omcid has no way to know.

### `omcicap` -- capture the OMCI channel

    omcicap [-f] [-x] [-n frames] [-w ticks]

    -n frames  stop after this many frames (default 100)
    -w ticks   stop after this many idle 5 s ticks (default 12)
    -x         hex-dump the head of every frame
    -f         take the channel even from a live holder (omcid)
    -h         this text; registers nothing

Registers for OMCI redirect type 1 and prints every frame the OLT sends.
Only one process can hold that channel, so it refuses to start while omcid
holds it. The procedure is: stop omcid, capture, start omcid again (see
below). While omcicap runs, and until omcid is back, the OLT's OMCI goes
unanswered and the OLT will eventually re-range the ONU -- which is
usually the conversation worth recording. Never on a stick you need to stay
in service.

### `nv` -- the U-Boot environment

    nv getenv [name]              print one variable, or all of them
    nv setenv [-c 1|2] name value set one (in the copy that was read, or copy 1|2; an invalid copy is repaired first)
    nv fallback [name]            read the OTHER copy, the one U-Boot falls back to
    nv commit [--force] slot      sw_commit=slot, mupgrade_en=0 and sw_tryactive=2 in BOTH copies, verified
    nv slotcheck slot             a PARTIAL check of kN and rN: uImage magic, CRCs and size, squashfs superblock

Reads and writes the redundant environment (mtd `env` and `env2`) with a
CRC check and no vendor library. It is what arms and commits a trial boot:
`sw_tryactive`, `sw_commit`, `sw_active`, `sw_version0/1`
(`docs/FLASHING.md`). `setenv` erases and rewrites a partition U-Boot
needs; with both copies valid, a power cut in that window leaves that copy
invalid and U-Boot on the other, stale one. **Never write `sw_commit` before
you have booted and verified a trial from the running image.**

When one copy is already invalid (a write cut by a power loss is how), `setenv`
and `commit` repair it as part of the write: the new environment is built and
checked in memory, written to the INVALID copy first and read back (CRC and
content), and only then is the valid copy rewritten, so the only good copy is
never erased while it is the only one and a second power cut still leaves a
valid copy. The repaired copy gets the flags byte that keeps the valid copy
the one U-Boot picks until the sequence is over (the valid copy's own byte
when the repaired one is `env2`, one below it when it is `env`). The one
exception is `env2` holding flags 0: nothing is below 0 and a tie goes to
`env`, so the repaired `env`, which already holds the complete new
environment, becomes the one picked. `setenv -c N` naming the invalid copy
writes only that one; naming the valid copy still repairs the other first. A
plain `setenv` then also rewrites the valid copy, because that is the copy
U-Boot reads and the change would otherwise not take effect. Both invalid:
nothing is written. Only a copy read in full whose CRC is wrong counts as
invalid. A copy that could not be read completely (open, a failed or short
read) says nothing about its content: `setenv` and `commit` then write nothing,
`-c N` included, and print `a copy could not be read completely, nothing
written`. After a transient failure, running them again is enough. While a
copy stays unreadable, `commit` and every form of `setenv` stay refused
(`commit --force` only skips the confirm-arp guard, not this), and `nv`
offers no way out until the partition can be read again. A raw restore of the
environment partitions (`docs/FLASHING.md`, "Restore") is the documented
route for a damaged environment; it is not promised to help when the flash
itself cannot be read. `getenv` and `fallback` do not check for this: they
use the copy they could read.

`nv commit` also sets `mupgrade_en=0`, in the same two writes. The stock
bootloader ships it as 1, which makes it listen on the host link after the
boot countdown for a firmware image sent by multicast and flash it with no
authentication; 0 turns that listener off.

It sets `sw_tryactive=2` (no trial pending) in both copies the same way.
U-Boot writes 2 into the copy it saves as it starts a trial, but the other
copy keeps the value that armed it, and if U-Boot ever had to fall back to
that copy, the next boot would be a trial nobody asked for. A value other
than 2 in the primary copy is different: U-Boot cleared it before this boot,
so someone queued a trial since (`nv setenv sw_tryactive`, the web UI), and
`commit` refuses it (`a trial is queued`), nothing written, rather than drop
it. Let the trial run first, or cancel it with `nv setenv sw_tryactive 2`.
`--force` does not override this.

`commit` is the commit, as one operation: `sw_commit=<slot>` into the
primary copy (the one U-Boot boots from), read back and compared byte for
byte, then into the fallback copy the same way, then both read again. It
refuses, before writing anything, a slot other than the one the kernel was
booted from (the last `root=` in `/proc/cmdline`, the one the kernel
mounted: the built-in slot-0 line comes before U-Boot's) or than `sw_active` in the
primary, a trial queued in the primary (above), and an environment with
no valid copy; it never writes
`sw_active`. With one invalid copy it repairs it first (above), so a commit
no longer needs a hand repair. A copy that already says so is not rewritten, so a
second run writes nothing. It also refuses, naming the file, while the
development file `/etc/config/confirm-arp` exists: each boot then waits for
an ARP reply from the address in it and a committed slot has no trial to
fall back to, so a host that stops answering means a reset every 120 s.
`--force` (or `-f`) commits anyway, with a warning. `/etc/config/breadcrumbs.on`
never blocks; it earns a one-line note. Exit 0 means both copies now commit the slot;
anything else says which step failed and whether the fallback copy was
touched (it never is before the primary has verified). Interrupting it is
safe at every step: until the primary is verified the fallback still holds
the old environment, and after that the primary wins with the new one.
It is how a trial is committed, by hand, once you have checked it
(`docs/FLASHING.md`, "Committing"); run `slot-state.sh` after it to refresh
the login notice and the exporter.
After a successful commit, and after its own result line, it warns when
`/var/run/odi-slot` records a failed check of the slot it left behind (the
check below, run by `slot-state.sh`); it does not read that slot itself.

`slotcheck` reads the kernel partition of a slot and the first 96 bytes of
its rootfs partition, and answers with one line: exit 0, `passes the partial
check`, exit 1, `FAILS the partial check: <reason>`, exit 2, `not checked:
<reason>` (a partition missing or unreadable, which says nothing about the
slot). It checks the uImage magic, header CRC and data CRC as U-Boot's
`bootm` does, but not the other header fields, plus that the image fits the
partition, and a version 4.0 squashfs
superblock (block size, `bytes_used` within the partition). It is not proof
that the slot boots (`docs/FLASHING.md`, "Checking the other slot").
`slot-state.sh` runs it on the other slot at every boot.

### Slot state -- `/var/run/odi-slot`

`/etc/scripts/slot-state.sh` writes it at every boot (an `/etc/inittab`
`once` entry), and again whenever it is run by hand or after a commit from the web UI (confd runs it with no environment, so it sets its own `PATH`; a run after the first of a boot does not wait for syslogd). It only reads the
environment (`nv getenv`, `nv fallback`) and `/proc/cmdline`, never writes
either, and replaces the file by rename. One `KEY=value` per line, always
all twelve keys, a value empty when it cannot be told:

| key | value |
|---|---|
| `running` | the slot the kernel was booted from, `0` or `1` (`root=31:N` naming `r0` or `r1`) |
| `sw_active` | `sw_active` in the primary copy, `0` or `1` |
| `sw_tryactive` | `sw_tryactive` in the primary copy, as stored (`2` when no trial is pending) |
| `primary_copy` | `1` or `2`: the copy U-Boot boots from (`env` or `env2`); empty when neither is valid |
| `primary_sw_commit` | `sw_commit` in that copy, `0` or `1` |
| `fallback_copy` | the other copy, `1` or `2`; empty when it is not valid |
| `fallback_sw_commit` | `sw_commit` in that copy, `0` or `1` |
| `next_boot` | the slot the next reset boots: `sw_tryactive` when it is `0` or `1`, else `primary_sw_commit` |
| `uncommitted` | `1` when a valid copy's `sw_commit` is not `running`; `0` when every valid copy names it; empty when the running slot or the environment cannot be read |
| `other_slot` | the slot that is not running, `0` or `1`; empty when `running` is |
| `other_slot_check` | `nv slotcheck` on it (`docs/FLASHING.md`, "Checking the other slot"), a partial check, not proof that the slot boots: `ok`, `fail`, `unknown` (not readable, or no answer), `pending` while it runs; empty when there is no `other_slot` |
| `other_slot_detail` | what `nv slotcheck` said: the whole line on `ok`, the reason on `fail` and `unknown` |

The exporter (metricsd) turns it into `gpon_boot_slot`,
`gpon_committed_slot` and `gpon_uncommitted`; the web UI reads it for its
trial banner. When `uncommitted` is not `0` the same script puts a one-line
notice in `/var/run/motd` (`/etc/motd`, which dropbear prints at every
interactive login) and in syslog (tag `slot-state`) -- `TRIAL BOOT`,
`HALF COMMITTED` (the fallback copy still names the other slot) or `SLOT
STATE UNKNOWN`, each naming the copies and the command that clears it; when
it is `0` the motd is empty. A failed check of the other slot adds an
`OTHER SLOT BROKEN` line to both, an unreadable one `OTHER SLOT UNCHECKED` to
the motd only. The file is first written with `other_slot_check=pending` and
replaced once the check has answered, so `trial-guard.sh` never waits on the
flash read. The first nine keys keep their order: omcid reads `running` and
`uncommitted` from the first 256 bytes.

### `flash` -- the config store

`/etc/scripts/flash` is ours: a shell script over the two XML files on the
config partition, `/var/config/lastgood.xml` (CS, the service keys) and
`/var/config/lastgood_hs.xml` (HS, the hardware identity).

    flash all cs|hs          print one store
    flash get KEY            KEY=VALUE, from whichever store has it
    flash set KEY VALUE      change an existing key, print KEY=VALUE back
    flash default cs         merge /etc/config_default.xml into CS

`set` only changes a key that already exists, refuses values carrying a
quote, `<` or `&`, and installs the file by rename (the new copy is written
beside the target, compared with the source, then renamed over it) so a power
cut leaves the old one whole. It changes the file and nothing else: whatever reads the key reads
it at its own time, or when `apply.sh` makes it. confd writes every setting
through it. `default cs` sets every key the image defaults file names to its
default (adding one that is in neither file) and leaves every other key, and
the HS file, alone; `docs/SETTINGS.md` has what that resets.
The partition is shared with the other slot, so a key written here is also
what the stock image reads if the stick falls back to it.

### `fwu.sh` -- flash one slot

Travels inside the image tarball, not in the rootfs:

    tar xf odi-oss-<version>.tar fwu.sh md5.txt
    ./fwu.sh <slot> odi-oss-<version>.tar

Refuses the running slot, checks both members' md5 and sizes before erasing
anything, streams them out of the tarball, and never touches the config
partition. It needs only `tar`, `sed`, `cksum`, `md5sum` and `cmp` (no `cut`,
which some stock images lack, so the first flash can run on stock firmware)
and names a missing one before touching anything. It writes `sw_version<slot>`
to the U-Boot environment only with `FWU_RECORD_VERSION=1`. `docs/FLASHING.md` is the procedure.

### `fwu_starter.sh` -- the same, from the web UI

    /etc/scripts/fwu_starter.sh <slot> <image.tar>                background
    /etc/scripts/fwu_starter.sh --foreground <slot> <image.tar>   and wait

What the Firmware tab's Write runs. Refuses a second write while one is
running, and any slot but the inactive one (read the way `fwu.sh` reads it:
`root=31:N` in `/proc/cmdline`, then `sw_active`); extracts `fwu.sh` and
`md5.txt` from the tarball into `/tmp/fwu.d` and checks `fwu.sh` against its
md5 line; then runs `fwu.sh <slot> <tar>` there, detached with `setsid`, its
output in `/tmp/fwu.log` and its outcome in `/tmp/fwu.state` (`running`,
`ok` or `failed <rc>`), which confd reports and the page polls. Never writes
`sw_commit` or `sw_tryactive`; trying the slot is a separate step.

### `apply.sh` -- apply saved settings without a reboot

    /etc/scripts/apply.sh network      LIVE
    /etc/scripts/apply.sh omci         INTERRUPTS INTERNET

`network` runs `network.sh addr -n` to print the plan, then `network.sh addr`
one second later, detached -- moving the primary address drops the
connection that asked for it, so the answer leaves first. It applies what the
store holds, at once and with no way back; the web UI changes the management
addresses through `mgmt-addr.sh` instead (below), and uses this only in a
safe-mode boot. `omci [seconds]`
sends SIGHUP to the running omcid (`pidof omcid`) and waits for its answer in
`/var/run/omcid-reload`: first that omcid took the signal (5 s), then the
outcome (20 s, `APPLY_WAIT` or the argument; the web UI gives the script 25).
omcid decides what the change needs (the table under `omcid`): nothing, a
VLAN rebuild in place, or a re-registration. The report says which:

    apply: omcid 412 told to reload its configuration
    apply: VLAN handling changed (VLAN_MANU_TAG_VID): connections rebuilt in place in 0.4 s, the ONU stayed in O5

    apply: identity changed (GPON_SN): the ONU re-registered and is back in O5 with 6 service(s) after 41.3 s (omcid 44700 ms in all)

A re-registration still running at the bound is reported as still working and
is not a failure. Exit 1 when no omcid runs (or `/etc/config/modules.off` is
set), when omcid does not answer the signal within 5 s (an older build treats
SIGHUP as stop), or when the reload's result is `failed`, `timeout`,
`no_services` or `sn_not_applied`. It never starts an omcid and writes
nothing to the driver: init owns the process, and a second omcid started here
used to make the ONU cycle through O1 to O3 while init's respawn entry
re-provisioned around it.

### `host-autoneg.sh` -- the host-port auto-negotiation

    /etc/scripts/host-autoneg.sh try on|off [SECONDS]   a trial, 120 s by default (10 to 600)
    /etc/scripts/host-autoneg.sh confirm                keep it: stores HOST_AUTONEG
    /etc/scripts/host-autoneg.sh revert                 end the trial now
    /etc/scripts/host-autoneg.sh status                 the state, key=value
    /etc/scripts/host-autoneg.sh boot                   rcS only

Applies `HOST_AUTONEG` (`docs/SETTINGS.md`, "Host port auto-negotiation")
through the kernel parameter `odi_switch.host_autoneg`, and reads the host
link from `odi_switch.host_link` (`docs/SWITCH.md`). `try` answers at once
and writes the mode one second later, detached, so the answer leaves before
the link changes; the trial reverts by itself at the end of its window
unless `confirm` (over ssh or the web UI, so over the link being tried)
stores the key first. `confirm` reads the key back and fails, leaving the
trial to revert, when it did not stick. `try` refuses while a trial or the
boot watch runs, and says so when the mode asked for is already the one
running and stored. It also refuses while the port runs a `HOST_SERDES`
mode other than `fiber1g`, or while `host-serdes.sh` runs a trial or its
boot watch, and a boot then applies nothing (`host-serdes.sh` below). Exit 1
on every refusal; the reason is on stderr. Every state change takes a lock
(`/var/run/host-autoneg.lock`, a directory; a holder that died is found by
its pid), the host-port lock `host-serdes.sh` takes too, and replaces the
state file in one rename.

The state file, `/var/run/host-autoneg`, and `status` (the same fields,
plus the live link, the uptime and whether this is a safe-mode boot), one `key=value`
per line; the web UI reads the file, and `status` is in the diagnostics
bundle (`host-link.txt`):

| key | value |
|---|---|
| `setting` | `on` or `off`: `HOST_AUTONEG` as this boot read it, or as a confirm stored it |
| `running` | `on` or `off`: what the host port runs now in Fiber 1G. While `host-serdes.sh` owns the port (below) the file keeps what this script runs in Fiber 1G, the value the port gets back there; `status` prints the bit the kernel holds instead |
| `owner` (`status` only) | `host-autoneg`, or `host-serdes` while a `HOST_SERDES` mode other than `fiber1g` runs, or a `host-serdes.sh` trial, reapply or boot watch does: that script sets the bit then, and this one applies nothing |
| `owner_message` (`status` only, with `owner=host-serdes`) | which link mode owns the bit, and that `HOST_AUTONEG` applies in Fiber 1G |
| `phase` | `steady`, `watch` (the boot watch is deciding) or `trial` |
| `fallback` | `1` while the port runs auto-negotiation with the key off because the forced link did not come up this boot |
| `trial_mode`, `trial_deadline` | the mode on trial and the uptime, in seconds, at which it reverts |
| `watch_until` | the uptime by which the boot watch ends at the latest |
| `event`, `event_uptime`, `message` | the last event, below |
| `link` (`status` only) | `1`, `0`, or `unknown` without the kernel parameter |
| `safe_mode_boot` (`status` only) | `1` on a safe-mode boot (`safe-mode.sh`) |

Every event is one line to the kernel log (`host-autoneg: event=<name>
setting=... running=... <text>`, so dmesg, the ramlog and syslog through
klogd), at notice:

| event | when |
|---|---|
| `applied` | a boot with the key off forced the mode and started the boot watch |
| `forced_link` | the boot watch saw the host link come up forced; no fallback |
| `watch_switch` | no link for 30 s; the watch switches mode |
| `fallback` | the link came up only with auto-negotiation; the key stays off |
| `fallback_nolink` | no link in either mode for 10 minutes; stays on auto-negotiation |
| `safe_mode_boot` | a safe-mode boot: nothing applied, auto-negotiation on, trials refused |
| `serdes_mode` | a boot with the key off while the port runs a `HOST_SERDES` mode other than `fiber1g` (or its boot watch): nothing applied, that mode sets the bit |
| `trial_started`, `trial_expired`, `trial_reverted`, `trial_confirmed`, `trial_failed` | a trial and how it ended |
| `bad_value`, `no_kernel`, `apply_failed` | a key that is neither on nor off, a kernel without the parameter, a write that failed: the port runs auto-negotiation |

A boot with the key unset logs nothing and writes nothing.

### `host-serdes.sh` -- the host SerDes mode

    /etc/scripts/host-serdes.sh try fiber1g|2500basex|hisgmii [SECONDS]
                                                     a trial, 120 s by default (10 to 600)
    /etc/scripts/host-serdes.sh confirm              keep it: stores HOST_SERDES
    /etc/scripts/host-serdes.sh revert               end the trial now
    /etc/scripts/host-serdes.sh reapply              run the sequence of the running mode again
    /etc/scripts/host-serdes.sh status               the state, key=value
    /etc/scripts/host-serdes.sh boot                 network.sh only

Applies `HOST_SERDES` (`docs/SETTINGS.md`, "Host port SerDes mode"): the
mode's register sequence through `diag` (`docs/SWITCH.md`, "Host port
SerDes mode"), then the kernel parameters `odi_switch.host_forced` and
`odi_switch.host_autoneg`. It does not touch the SFP EEPROM the host
reads, which still advertises 1000BASE-LX (`docs/SETTINGS.md`, "What the
host reads from the module"). These verbs and the `status` keys below are the
interface the web UI is to use (through `/api/hostlink`, as for
`host-autoneg.sh`); they are kept stable.

`boot` runs from `network.sh`, before the MAC and the bridge: it reads every
register of the mode and writes nothing when they already hold it, applies
the whole sequence otherwise, and in 2500BASE-X starts the boot watch,
detached. `try` answers at once and switches one second later, detached,
so the answer leaves before the link changes; the trial reverts by itself
at the end of its window unless `confirm` (over ssh, so over the link being
tried) stores the key first. `confirm` reads the key back and fails,
leaving the trial to revert, when it did not stick. A trial whose
registers do not read back after the switch goes back at once
(`trial_failed`). `reapply` answers at once and, one second later, runs the
whole sequence of the running mode and its port side again, with no trial
and nothing stored; the event says whether the registers read back. `try` and `reapply` refuse while a trial, a reapply or the boot
watch runs, while `host-autoneg.sh` runs a trial or its boot watch, in a
safe-mode boot (`try`), with `/etc/config/sds.off`, without `diag`, and for
2500BASE-X and HiSGMII on a kernel without `host_forced`; `try` says so
when the mode asked for is already running and stored. Exit 1 on every
refusal; the reason is on stderr. State changes take the host-port lock
(`/var/run/host-autoneg.lock`, shared with `host-autoneg.sh`) and replace
the state file in one rename.

The state file, `/var/run/host-serdes`, and `status` (the same fields, plus
the live values below), one `key=value` per line; `status` is in the
diagnostics bundle (`host-link.txt`):

| key | value |
|---|---|
| `setting` | `fiber1g`, `2500basex` or `hisgmii`: `HOST_SERDES` as this boot read it, or as a confirm stored it |
| `running` | the mode the host SerDes runs now; `unmanaged` with `sds.off` or without `diag`, `unknown` when the registers could not be read (nothing written) or, in `status`, with no state file |
| `phase` | `steady`, `watch` (the boot watch of 2500basex is deciding), `trial` or `reapply` |
| `fallback` | `1` while the port runs Fiber 1G with the key at 2500basex because only Fiber 1G linked this boot, or nothing linked in either mode |
| `trial_mode`, `trial_deadline` | the mode on trial and the uptime, in seconds, at which it reverts |
| `watch_until` | the uptime by which the boot watch ends at the latest |
| `event`, `event_uptime`, `message` | the last event, below |
| `uptime` (`status` only) | the uptime, in seconds |
| `autoneg` (`status` only) | `on`, `off` or `unknown`: `odi_switch.host_autoneg` now |
| `forced` (`status` only) | `1`, `0` or `unknown`: `odi_switch.host_forced` now |
| `link` (`status` only) | `1`, `0` or `unknown`, from `link_source` |
| `link_source` (`status` only) | `host_link` (Fiber 1G: the kernel parameter), `serdes_sync` (2500BASE-X: bit 17 of 0x214) or `none` (HiSGMII: `link` is `unknown`) |
| `safe_mode_boot` (`status` only) | `1` on a safe-mode boot (`safe-mode.sh`) |

Every event is one line to the kernel log (`host-serdes: event=<name>
setting=... running=... <text>`, so dmesg, the ramlog and syslog through
klogd), at notice:

| event | when |
|---|---|
| `applied` | a boot applied 2500basex (and started the boot watch) or hisgmii (no watch: no link signal) |
| `serdes_link` | the boot watch saw the 2500BASE-X sync; no fallback |
| `watch_switch` | no link for 30 s; the watch switches mode |
| `fallback` | the host linked only in Fiber 1G; the key stays 2500basex |
| `fallback_nolink` | no link in either mode for 10 minutes; stays on Fiber 1G, the key unchanged |
| `safe_mode_boot` | a safe-mode boot: Fiber 1G, nothing applied, trials refused |
| `trial_started`, `trial_expired`, `trial_reverted`, `trial_confirmed`, `trial_failed` | a trial and how it ended |
| `reapply_started`, `reapplied`, `reapply_failed` | a reapply, and whether the registers read back after it |
| `bad_value`, `no_kernel`, `apply_failed`, `apply_unverified` | a key that is none of the three, a kernel without `host_forced` (both: the port runs Fiber 1G), a register or parameter that could not be read or written, a sequence that did not read back (the boot carries on in the mode) |
| `sds_off`, `no_diag` | `/etc/config/sds.off` or no `diag`: nothing written to the host port |

A boot in Fiber 1G logs nothing; its messages (`SerDes already Fiber 1G`,
or the thirteen writes and `SerDes now Fiber 1G`) go to the boot output,
as they did from `network.sh`.

### `mgmt-addr.sh` -- the management addresses, as a trial

    /etc/scripts/mgmt-addr.sh try [-s SECONDS] KEY=VALUE...   a trial, 90 s by default (10 to 600)
    /etc/scripts/mgmt-addr.sh confirm [LOCAL [PEER]]          keep it: stores the keys
    /etc/scripts/mgmt-addr.sh revert                          end the trial now
    /etc/scripts/mgmt-addr.sh status                          the state, key=value

Applies a change of `LAN_IP_ADDR`, `LAN_SUBNET`, `LAN_ENABLE_IP2`,
`LAN_IP_ADDR2`, `LAN_SUBNET2` and `LAN_ROUTES` (`docs/SETTINGS.md`, "Management
address trial") without storing it. `LAN_ROUTES` (`try LAN_ROUTES="default via
192.168.1.253"`, an empty value clears it) is applied by `network.sh addr` on
the management device after the addresses, and a trial that changes only it
is confirmed from any client, as `docs/SETTINGS.md` says; `try` refuses a
`LAN_ROUTES` that `network.sh` would turn off. `try` takes the keys it changes as `KEY=VALUE`
words, reads the rest from the store, has `network.sh addr -n` judge the
whole set (a value the kernel would refuse fails there, with the reason, and
nothing starts), writes the trial file `/var/run/mgmt-addr.trial` and answers
at once; one second later, detached, `network.sh addr` reads that file instead
of the store, moves the primary to the new address and keeps the previous one
on `br0:4` for the length of the trial. The trial reverts by itself at the end
of its window: the file goes and `network.sh addr` puts the stored addresses
back.

`confirm` stores the keys that changed, reading each back (a key that does not
stick puts the others back and leaves the trial running), then ends the trial.
The keys go to the store one at a time (`flash set` writes one), and the ones
put back are read back too: when one of those does not stick either, the
refusal names it with the value the store holds, because the store then has
new values next to old ones until the confirm is repeated or the old values
are stored.
`LOCAL` and `PEER` are the address the request arrived on and the client's;
confd passes them from the socket, and over ssh they come from
`SSH_CONNECTION`. The confirm is refused unless `LOCAL` is an address the
trial added, the proof that the new address answers; with none added (the
netmask moved, or the second address went away) `PEER` must be on the link of
the new netmask (except when only `LAN_ROUTES` changed: any client). `revert` ends the trial at once and moves back one second
later, so an answer sent over the address being left still arrives. `try`
refuses while a trial runs, in a safe-mode boot, while `/etc/config/lan-ip`
exists and the primary or its netmask is asked for, and for any key but those
six. Exit 1 on every refusal; the reason is on stderr. State changes take a
lock (`/var/run/mgmt-addr.lock`) and replace the state file in one rename, as
`host-autoneg.sh` does.

The state file, `/var/run/mgmt-addr`, and `status`, one `key=value` per line;
the web UI reads the file:

| key | value |
|---|---|
| `phase` | `steady` or `trial` |
| `applied` | `1` once the trial addresses are on the interface (a second after `try`) |
| `trial_deadline` | the uptime, in seconds, at which the trial reverts |
| `prev_ip`, `prev_mask` | the primary the stick ran before the trial, kept on `br0:4` |
| `old_<KEY>`, `new_<KEY>` | the six keys (`LAN_ROUTES` included) as the store holds them and as the trial asks for them |
| `event`, `event_uptime`, `message` | the last event: `trial_started`, `trial_confirmed`, `trial_expired`, `trial_reverted` or `trial_failed` (also one line to the kernel log, `mgmt-addr: event=...`) |
| `safe_mode_boot` (`status` only) | `1` on a safe-mode boot |

Nothing persists across a reboot: the state and the trial file are in
`/var/run`, and the keys are written only by `confirm`.

### `safe-mode.sh` -- the boot counter and the recovery boot

    /etc/scripts/safe-mode.sh status     this boot and the record, key=value
    /etc/scripts/safe-mode.sh ack        acknowledge a safe-mode boot: removes its record
    /etc/scripts/safe-mode.sh active     exit 0 on a safe-mode boot
    /etc/scripts/safe-mode.sh count      rcS only, right after the config partition is mounted
    /etc/scripts/safe-mode.sh no-config  rcS only, instead of count, when it is not: a safe-mode boot with no record

`count` adds one to `/etc/config/boot-count` and spawns `boot-ok`, which
removes it once the uptime reaches 60 s. It also reads why the previous boot
ended (`reason=` in `/proc/odi_ramlog_prev`) and keeps `/etc/config/boot-wdt`:
+1 for `wdt_*`, `panic` and `oops`, unchanged for `power`, removed for
anything else; `boot-ok` removes it at 600 s of uptime. Three boots in a
row that ended that way make this boot a safe-mode boot as well
(`cause=watchdog`). Uptime starts with the kernel,
about 54 s after a cold power-on on the sticks measured (the count lands at
about 5 s of uptime), so a power-up counts as short when it is cut roughly
60 to 110 s after power-on: "plug in, wait about 80 s, pull" (docs/SETTINGS.md). A boot that finds five short boots
before it (three with `HOST_AUTONEG=off`, or with `HOST_SERDES` at
`2500basex` or `hisgmii`) is a safe-mode boot: it writes
`/var/run/safe-mode` and the record `/etc/config/safe-mode`, removes
`HOST_AUTONEG` when that was off and `HOST_SERDES` when it held another
mode than `fiber1g`, and bind-mounts a RAM copy of
`/etc/passwd` with root's password `admin` over the read-only one
(`docs/SETTINGS.md`, "Recovery: the safe mode"). The flag in `/var/run` is
what the rest reads: `host-serdes.sh` runs Fiber 1G, `host-autoneg.sh`
applies nothing, the PON steps
leave the laser off and skip `gponact` (`rcs-lib.sh`), `svc-dropbear.sh`
offers passwords, and confd accepts `admin:admin`. `status` prints
`active=0|1` with the flag's fields as `now_*` (`now_cause=` is
`short_boots`, `watchdog` or `config`), `record=0|1` with the record's as
`record_*`, `boot_count` while there is one and `wdt_count` likewise.

Its events, to the kernel log at notice: `safe-mode: event=boot_count
boot=N short_before=N limit=5|3 wdt_before=N wdt_limit=3 prev=REASON
uptime=S` on every boot (the uptime is how early the count lands on a given
stick; `prev` is the previous boot's `reason=`, `none` without a ramlog),
`event=safe_mode cause=short_boots|watchdog|config short_boots=N
wdt_resets=N cleared=HOST_AUTONEG|HOST_SERDES|HOST_AUTONEG,HOST_SERDES|none|failed
root_password=default|failed`, and
`event=safe_mode_ack`.

### `diag-bundle.sh` -- the diagnostics bundle

    /etc/scripts/diag-bundle.sh [--full] [OUT]   default OUT: /tmp/odi-diag.tar.gz

Collects what a bug report needs into one tar.gz, `odi-diag/` inside, and
prints OUT: the previous boot ramlog (`/proc/odi_ramlog_prev`), the tail of
every `/var/log` file (256 KB each, 1 MB in all), `dmesg`, `/etc/odi-build`,
`/etc/version`, `/proc/odi_wdt/{watchdog_flag,userland_ok,clients}`,
`uptime`, `/proc/meminfo`, `/proc/mounts`, `/proc/cmdline`, `ps`,
`sw_active`, `sw_commit`, `sw_tryactive` and `sw_version0/1` from both
environment copies (`nv getenv` and `nv fallback`), one scrape of the
exporter, and the config store. `MANIFEST.txt` lists every step with its
exit status and size, which keys were redacted, and whether the identity
was masked. The web UI serves the same file as "Download a diagnostics
bundle" on the Admin tab (odi-ui `GET /api/diag`, which only runs this and
streams the result).

Under `pon/`, the GPON and OMCI state a PON problem needs:

| file | from |
|---|---|
| `diag.txt` | one `diag` run: `gpon get onu-state`, `gpon get alarm-status`, `gpon get flows`, `mib dump counter port all`, and `register get 0xf020a8 240`, the PON queue window that cmd 23 and 25 write (`docs/SWITCH.md`) |
| `odi_gpon-status.txt`, `odi_gpon-counters.txt`, `odi_gpon-flows.txt`, `odi_gpon-ploam.txt` | `/proc/odi_gpon/status`: ONU state, Alloc-IDs, alarm and state times; `counters`: the link counters; `flows`: the per-flow GEM counters; `ploam`: PLOAM totals and the ring (bodies withheld, below) |
| `odi_omci.txt` | `/proc/odi_omci`: the OMCI channel counters |
| `omcli_{state,provision,flows,tcont,conn,vlan}.txt` | `omcli`: what omcid holds and programmed; `vlan` is the VLAN handling (stick tags or transparent) and any `FwdOp` override of class 84 from `RTK_DEVINFO_SPECVER`, numbers only, so the bundle alone shows whether the override is in force |
| `omcli_mib_<class>.txt` | `omcli mib <class>` for ONU-G, ONU2-G, PPTP Ethernet UNI, VEIP, the bridge and mapper classes, VLAN filtering and extended VLAN tagging, T-CONT, ANI-G, GEM ports and interworking points, priority queues, traffic schedulers and descriptors, multicast |

The MIB classes are a fixed list, not the whole MIB: an OLT can push
credentials through OMCI (SIP user data, authentication methods, large
strings, the TR-069 server), and none of those classes is collected. When
omcid does not answer `omcli state`, the other `omcli` steps are skipped.

Under `module/`, the optics module on `/dev/i2c-0`, which tells which laser
driver the stick has and what state its calibration is in:

| file | from |
|---|---|
| `a0.txt` | `i2cdump -y 0 0x50`: A0h, the identification EEPROM, bytes 68-83 (the module serial) blanked as `XX` unless `--full` |
| `a2.txt` | `i2cdump -y 0 0x51`: A2h with the page select as found, untouched: DDM, and the upper half of whatever page was selected |
| `chip.txt` | A2h byte 127 (the page select) as found; for each page selected, what byte 127 read back; page 2 bytes 0xa0 (0x6a: Semtech GN25L95), 0x80 and 0xdc (the UX3320 variant markers); page 3 byte 0xf0 (UX3320 INIT_STATE); and the page select written back and read back |
| `a2-p<N>.txt` | `i2cdump -y -r 0x80-0xff 0 0x51` with page N selected, for N in 2, 3, 0, 4, 5, 6, only when byte 127 read back N |

Selecting a page is the one write the bundle makes anywhere, and the only
one the kernel accepts on this bus. A chip may refuse a page and keep the
one it had (a GN25L95 refuses page 3), which is why each select is read
back and a page is dumped only when it took. The page work is one short
block under a lock (`flock` on `/tmp/odi-diag-i2c.lock`, so two bundles
never interleave their selects): the first failure ends the selects, and
the page found is always written back, also when the bundle is
interrupted. Without the bus (qemu, or a build without the adapter)
every `module/` step is listed in `MANIFEST.txt` with a failing status
and the bundle still succeeds.

**Redaction.** In the copies of `lastgood.xml`, `lastgood_hs.xml` and
`odi.conf`, the value of every key below is replaced by `REDACTED` (an empty
value stays empty, so "not set" still shows):

- by name: `GPON_PLOAM_PASSWD`, `LOID_PASSWD`, `LOID_PASSWD_OLD`,
  `USER_PASSWORD`, `SUSER_PASSWORD`, `E8BDUSER_PASSWORD`, `SUPER_PASSWORD`,
  `MAC_KEY`, `HW_FON_KEYWORD`;
- and any key whose name contains `PASS`, `PWD`, `PSK`, `SECRET`, `TOKEN`,
  `KEY`, `COMMUNITY` or `CRED`.

Then every secret value found that way, plus the web UI password from
`/etc/config/confd.auth`, is scrubbed from every file in the bundle, as text
and as upper- and lowercase hex, run together and space-separated, in case a
log quoted one; values shorter than three characters are redacted in the
config copies only. `confd.auth`, the dropbear keys and `/etc/passwd` are
never collected.

**Masking.** Many OLTs authenticate an ONU by its serial number alone, so a
bundle posted in a public issue would hand out what it takes to clone the
line. By default the serial number (`GPON_SN` and the one in
`/proc/odi_gpon/status`), the MACs (`ELAN_MAC_ADDR` and every interface), the
LOID (`LOID`, `LOID_OLD`) and the module serial (A0h bytes 68-83, also
blanked in `module/a0.txt`, where the dump splits it over two lines) are
replaced in every file, in every spelling
(text, hex, a MAC with or without separators), by `MASKED-` and their last
four characters, which still tells two sticks apart. `--full` keeps them,
for a bundle you send privately; the web UI never passes it.

The PLOAM ring in `/proc/odi_gpon/ploam` prints the first ten bytes of each
message body, which can cut a value in two where the scrub cannot find it.
So the bodies of Password and Encryption_Key are always withheld, and those
that carry the serial number (Serial_Number_ONU, Acknowledge,
Serial_Number_Mask, Assign_ONU-ID, Disable_Serial_Number) unless `--full`.

Every command runs under busybox `timeout` (5 s, 10 s for the exporter
scrape; each `i2cget`, `i2cset` and `i2cdump` on its own), and a bundle over 2 MB is refused rather than written. Exits 1, with
the reason on stderr and no OUT left behind, when it cannot finish.
`make test-qemu` plants known secrets and identifiers in the config,
`confd.auth` and a log, asserts none of them is anywhere in the bundle, and
that `--full` keeps the identifiers but still no secret; and that the
`module/` steps are listed, failing, in a bundle that still succeeds.

## Boot scripts you can run again

| script | what | safe to re-run |
|---|---|---|
| `/etc/scripts/network.sh` | host SerDes check (and fix, `host-serdes.sh boot`), MAC, `br0` over `eth0.2`, the addresses | yes: it checks before it writes, and re-running it is how a management-path problem is debugged. A re-run is a new boot to `host-serdes.sh`: in 2500BASE-X it starts the boot watch again |
| `/etc/scripts/host-serdes.sh status` | the host SerDes mode state (above) | yes: read-only |
| `/etc/scripts/host-autoneg.sh status` | the host-port auto-negotiation state (above) | yes: read-only |
| `/etc/scripts/sfp-host-ddm.sh status\|apply` | the host-side SFP EEPROM (`SFP_HOST_DDM`, `docs/SETTINGS.md`): `status` is the key, the kernel request and its state; `apply` is `apply.sh sfp` (on or off from the key, waits up to 10 s for the kernel). The boot runs `sfp-host-ddm.sh boot` as a PON step | `status` yes: read-only; `apply` yes |
| `/etc/scripts/safe-mode.sh status` | the safe mode: this boot, the record, the boot count | yes: read-only |
| `/etc/scripts/network.sh addr [-n]` | only the addresses: the primary and the second one (`br0:2`), live; `-n` says what it would change | yes; this is `apply.sh network` |
| `/etc/scripts/gpon-hold.sh status\|clear` | the kept Emergency Stop (`GPON_KEEP_DISABLE`, `docs/SETTINGS.md`): `status` is key, marker file and what the driver holds; `clear` removes `/etc/config/gpon-hold` and releases the hold now. The kernel runs `gpon-hold.sh sync` itself to write or remove the marker | `status` yes: read-only; `clear` yes |
| `/etc/scripts/mount-config.sh [name] [dir]` | find mtd `config` by name, mount it jffs2 | only if it is not mounted |
| `/etc/scripts/respawn.sh <oom> <log> <cmd...>` | not run directly: exec-ed by each `svc-*.sh` inittab entry, sets `oom_score_adj` then execs the daemon in place | n/a |
| `/etc/init.d/services {stop\|start} <name>` | write/clear `/etc/config/<name>.off` and kill the current process so busybox init restarts it at once (`docs/SETTINGS.md`) | yes |

## Kernel control files

The files userland drives the kernel through. `docs/KERNEL.md` has the
drivers behind them.

| file | read | write |
|---|---|---|
| `/dev/odi_sw` | ioctls: registers, MIB counters, DDM, the L2 table, the ACL probes, the multicast group counters (diag, metricsd, igmpd) | register writes (diag), L2 multicast writes, the snooping lease and the group counters (igmpd -w), the ACL probes (diag acl-probe), the group counters (diag mc-count) |
| `/proc/odi_gpon/status` | ONU state, ONU id, the serial number, EqD, the Alloc-IDs the OLT assigned (`alloc_ids`, in CAM row order: the assignment order, a row freed by a deallocation reused first), the GEM ports the downstream path decrypts (`enc_ports <n> <port>...`: each DS slot whose AES bit is set in the hardware, and any port the OLT asked to decrypt that has no slot yet; omcid answers the GEM port network CTP's Encryption state from it), the REI count, the TO1/TO2 expiries, `o7_hold keep <0|1> held <0|1>` (`GPON_KEEP_DISABLE`: the key reached the driver, a kept Emergency Stop is in force), the interrupt bookkeeping, `laser on <0|1> ldd_id <id byte, -1 if unread> unverified <0|1>` (the `laser` verb's outcome: `on 0` means the laser was left off and `gponact` is refused, `docs/SETTINGS.md`), `laser_emergency_stop <0|1>` (1 while the OLT holds the ONU in Emergency Stop, O7: the TX-disable line is high and the laser off until the OLT re-enables it, or `gpondeact`/`gponact` or a reboot leaves O7), the last LOS sample, the last Upstream_Overhead and Ranging_Time decoded (`upstream_overhead`, `ranging_time`, with the USF_MIN_RESP_DELAY value and the MULTFRAME/INFRAME written), the last PON-ID since the last loss of signal (`pon_id odn_class <0-7> reach_extender <0|1> tol <n> id <14 hex digits> ms <ms since boot>`, or `pon_id none`: G.984.3 Annex C, sent only by an OLT provisioned to; `odn_class` 0 A, 1 B, 2 B+, 3 C, 4 C+; `reach_extender` 1 when TOL is a reach extender's rather than the OLT's; `tol` the mean launch power in 0.1 dB steps above -30 dBm, so `tol 334` is +3.4 dBm and the path loss is about that less the stick's receive power, 65535 when the OLT does not report it; `id` the PON identifier the operator set), and the live alarm and FEC bits and the alarm and ONU state times (below) | -- |
| `/proc/odi_gpon/counters` | the sampled GPON link counters (below) | -- |
| `/proc/odi_gpon/flows` | the sampled per-flow GEM counters, one line per flow OMCI created (below) | -- |
| `/proc/odi_gpon/ploam` | PLOAM message totals (`ploam ds_rx <n> us_tx <n>`, `ploam_us_dropped`: replies not sent because the upstream queue stayed full for 1 ms; `ploam_type ds|us 0x<type> <count>`, one line per type seen), and the last 32 PLOAM messages (`ploam_ring`: direction, ONU-ID, type, the 10 content bytes; the eight AES key bytes of an upstream Encryption_Key, type 0x05, print as `xx`) | -- |
| `/proc/odi_gpon/cam` | the Alloc-ID CAM read back from the hardware, the table that decides which upstream grants the ONU answers: `alloc_cam <row> hw <Alloc-ID|-> recorded <Alloc-ID|->` for each row valid in the hardware or recorded by the driver (row 16 is the ONU-ID, the default Alloc-ID), then `alloc_cam_valid <n>`. A row whose `hw` and `recorded` differ is a fault. Then the downstream GEM port CAM, the table that decides which GEM ports the ONU receives: `gem_cam <row> hw <GEM port|-> type 0x<FLAGS>` for each of rows 0..64 valid in the hardware or with a flow type word not zero (rows 0..63 are the downstream flows OMCI created, the row being the flow id, `diag gpon get flows`; row 64 is the OMCC; FLAGS bit 0 multicast, 1 Ethernet, 2 OMCI, 4 AES), then `gem_cam_valid <n>`. A flow OMCI deleted leaves no line | -- |
| `/proc/odi_omci` | redirect registrations, frame and command counters | `switch_init`: the platform settings and the module-load replay (rcS does this once) |
| `/proc/odi_init` | the last verb's return code | one SDK init or PON verb (rcS does these once) |
| `/proc/odi_wdt/userland_ok` | -- | `1`: userland is up, stop the 120 s reset (one-shot, boot only) |
| `/proc/odi_wdt/watchdog_flag` | -- | `1`: keep kicking the hardware watchdog |
| `/proc/odi_wdt/register` | -- | `"<name> <deadline_s>"`, e.g. `omcid 60`: register (or update) a watchdog client; idempotent, arms the deadline from that moment and resets its clock (`docs/SETTINGS.md`, "Watchdog rules") |
| `/proc/odi_wdt/ping` | -- | `"<name>"`: a registered client's own ping, which restarts its deadline; refused (`-EINVAL`) for an unregistered name |
| `/proc/odi_wdt/clients` | one line per registered client: name, deadline, armed, last-ping age | -- |
| `/sys/module/odi_switch/parameters/host_autoneg` | `Y` or `N`: the host port negotiates, or runs forced 1000BASE-X | `1`/`0`: applied at once (`host-autoneg.sh` does this; by hand it lasts until the next boot) |
| `/sys/module/odi_switch/parameters/host_forced` | `Y` or `N`: the host port runs forced (link up, 1000 full duplex), as 2500BASE-X and HiSGMII need | `1`/`0`: applied at once (`host-serdes.sh` does this; by hand it lasts until the next boot) |
| `/sys/module/odi_switch/parameters/host_link` | `1` while the host port has a link; always `1` while `host_forced` is `Y` | -- |
| `/sys/module/odi_switch/parameters/sfp_mirror` | `Y` or `N`: the host-side SFP EEPROM asked for or not | `1`: fill the SoC SFP slave from the module and turn it on (in the background); `0`: off at once (`sfp-host-ddm.sh` does this; by hand it lasts until the next boot) |
| `/sys/module/odi_switch/parameters/sfp_mirror_state` | `off`, `filling`, `on` or `failed` | -- |
| `/proc/odi_ramlog_prev` | the previous boot's DRAM ramlog, decoded: this boot's counter and slot, the previous boot's counter, slot, build id, last early crumb and reset reason (`reason=`, `docs/HACKING.md`), then its first 3984 bytes and its last 4080 (root only) | -- |
| `/proc/odi_ramlog_prev_raw` | the same two pages as 8192 raw bytes, page A then page B, for `ramlog-read.sh`-style decoding off the stick (root only) | -- |

Writing a verb or an init command by hand re-programs the switch or the
PON MAC under a running omcid; do it on a trial stick only.

### /proc/odi_gpon/: the link counters

`/proc/odi_gpon` is a directory of four read-only files, one per topic, the
way `/proc/odi_wdt/` is. There is no `/proc/odi_gpon` file: a reader opens
the file it needs, and on a stock image, which has none of them, treats a
missing one as nothing to report.

| file | lines |
|---|---|
| `status` | `state`, `onu_id`, `sn`, `eqd`, `alloc_ids`, `enc_ports`, `rei_count`, `to1_expiries`, `to2_expiries`, `irq_*`, `laser`, `laser_emergency_stop`, `last_los_ms`, `upstream_overhead`, `ranging_time`, `pon_id`, then the sampler's `alarm_now`, `fec_now`, `ds_superframe`, `alarm_events`, `alarm_ms`, `onu_state_ms`, `onu_transitions` |
| `counters` | `ctr_samples`, `ctr_ds_superframe`, `ctr_fec_now`, `ctr`, `ctr_saturated` |
| `flows` | `flow_ack_timeouts`, `flow_saturated`, `ds`, `us` |
| `ploam` | `ploam ds_rx us_tx`, `ploam_us_dropped`, `ploam_type`, `ploam_ring` |

Every line is one fact, a key and then words, all numbers decimal. The GPON MAC counters clear when they are
read, so the driver is their one reader (`docs/KERNEL.md`, "The GPON link
counters"): it samples them once a second from the boot `gponact` on and
prints 64-bit totals since its first sample, taken in that `gponact`. Times are milliseconds: to
the second for the ONU states, to the 100 ms line poll for the alarms while
the ONU is activated.

In `status` (the live bits are absent before the first sample):

    alarm_now los 0                   the live DSF_ALARM_STATE bits at the last
    alarm_now lof 0                   reading: los, lof, lom
    alarm_now lom 0
    fec_now ds 1                      FEC in use: downstream (the OLT sends
    fec_now us 1                      it), upstream (the ONU sends it)
    ds_superframe 305491896           the downstream superframe count
    signal sf 0 threshold 5           signal fail and signal degrade (G.984.3
    signal sd 0 threshold 9           11.1.2): the downstream BER over each
                                      10 s of O5, from DSF_BIP_ERR_BITS, set
                                      at >= 10^-threshold, cleared below
                                      10^-(threshold+1), both 0 outside O5;
                                      threshold from the OLT's ANI-G (cmd 11),
                                      defaults 5 and 9
    signal_events sf 0                clear -> set, per signal
    signal_window ms 10000 bip_err_bits 0   the last whole window evaluated
    alarm_events los 1                clear -> set, as read; one per alarm
    alarm_ms los 2000                 time seen set; one per alarm
    onu_state_ms O5 8000              time in each state, O1 to O7
    onu_transitions O4 O5 1           state changes, by state left and
                                      entered; only pairs that happened

In `counters`:

    ctr_samples 11                    samples taken since boot
    ctr_ds_superframe 305491896       the superframe count and downstream FEC
    ctr_fec_now ds 1                  bit at the last sample, as in status;
                                      here so that one read pairs them with
                                      the totals (absent before the first
                                      sample)
    ctr ds_bip_err_blocks 4           every counter, always
    ctr_saturated ds_gem_idle 1       samples that read it full; only
                                      counters that ever were

The `ctr` names, by bank: downstream framer `ds_bip_err_blocks`,
`ds_bip_err_bits`, `ds_fec_corrected_bits`, `ds_fec_corrected_bytes`,
`ds_fec_corrected_cw`, `ds_fec_uncorrectable_cw`, `ds_superframe_loss`,
`ds_plend_fail`, `ds_ploam_accepted`, `ds_ploam_crc_err`,
`ds_ploam_overflow`, `ds_bwmap_crc_err`, `ds_bwmap_overflow`,
`ds_bwmap_invalid_0`, `ds_bwmap_invalid_1`, `ds_sn_requests`,
`ds_ranging_requests`, `ds_bwmap_accepted`, `ds_gem_los`,
`ds_gem_hec_corrected`, `ds_gem_idle`, `ds_gem_len_mismatch`,
`ds_gem_port_mismatch`, `ds_gem_non_idle`, `ds_plend_corrected`,
`ds_plend_uncorrectable`, `ds_blen_too_long`; downstream GEM
`ds_eth_multicast`, `ds_eth_unicast`, `ds_eth_multicast_fwd`,
`ds_eth_multicast_leak`, `ds_eth_fcs_err`, `ds_gem_over_interleave`,
`ds_omci`; upstream `us_bursts`, `us_ploam`, `us_dbru`, `us_gem_frames`,
`us_ploam_cpu_normal`, `us_ploam_cpu_urgent`, `us_ploam_no_message`,
`us_ploam_sn`, `us_gem_bytes`. One more line follows them, `ctr ds_fec_total_cw`: the downstream code words received with FEC on and the line locked, which no register counts. The driver derives it, 153 code words (152 of 255 bytes and a shortened last one, G.984.3 clause 13.2.1) for each 125 us frame the `ds_superframe` counter advanced between two samples that both saw FEC on and LOS and LOF clear, and skips a step larger than the time between the samples allows (a re-lock); omcid reports it as the FEC history data's total code words. `kernel/extra/drivers/net/ethernet/odi/odi_gpon_ctr.c`
has the register and field of each, and `test/fixtures/odi_gpon_ctr_counters.txt`
a whole file (`odi_gpon_ctr_status.txt` is the sampler lines of `status`). `ds_bwmap_accepted` is the one to watch for a stuck ONU:
on ISP2 it runs about 2,070 a second when served, stops when the OLT has
deregistered the ONU that still says O5, and runs about nine times faster
when the Alloc-ID table holds rows the OLT never assigned.

In `flows`, one line per flow, in index order, downstream then upstream:

    flow_ack_timeouts 0               counts skipped because the register
                                      never answered within its bound
    flow_saturated 0                  counts read as all ones: lost counts
    ds 1 gem 1434 gem_frames 20000 bytes 28000000 eth_rx 20000 eth_fwd 20000
    us 4 gem 1566 gem_frames 330 eth_frames 50 bytes 4294994796

`ds <flow>` and `us <flow>` are the driver flow index, the id omcid sent
with the flow (a row of the downstream GEM port CAM, a slot of the upstream
flow table), not the GEM port id: `gem <port>` is the port the flow was
created with. The OMCI channel is a downstream flow (`gem 4095`) and has no
upstream one. Downstream, `gem_frames` and `bytes` are what the GEM layer
received on the flow, `eth_rx` the Ethernet frames it carried and `eth_fwd`
the ones passed on to the switch; the three frame counts were equal per
flow on a stick. Upstream, `gem_frames` ran about 6.6 times `eth_frames` on
that stick, and `bytes` is a 64-bit count. A flow appears when OMCI creates it
and its totals start from zero then, and again whenever it is created again;
a reset of the flow state empties the file. A line has no flow that was not
created, so an image that never provisions a service has none. The file
exists only on this image: the stock image has no `/proc/odi_gpon` at all,
and a reader treats a missing file as no flows.
`kernel/extra/drivers/net/ethernet/odi/odi_gpon_flow.c` has the registers,
and `test/fixtures/odi_gpon_flows.txt` a whole file. `diag gpon get
flow-counters` prints it.

New lines go in as new keys; a parser skips a key or a counter name it
does not know. Reading one of these registers by hand (`diag register
get`, `memprobe`) takes its count away from the totals and from the REI.
The per-flow registers (`0x701140`-`0x701144`, `0x704040`-`0x704050`,
`0x706048`-`0x70604c` and the byte array at `0x706800`) clear on read as
well and have the driver as their one reader, so nothing else may read
them: not `diag register get`, not `memprobe`, and not the stock firmware
`gpon show counter flow`, which would take its counts away from the totals.

## Logs

| where | what | survives |
|---|---|---|
| `/var/log/omcid.log` | every OMCI frame and driver call, omcid's startup | nothing (RAM, trimmed to 128-256 KB) |
| `/var/log/omcid-unknown.txt` | each managed entity class and message type the OLT sent that omcid does not model, with a count ("Link and provisioning events") | an omcid respawn, not a reboot (RAM) |
| `/var/log/services.log` | stderr of services: dropbear logins, confd, metricsd | nothing (RAM, trimmed) |
| `/var/run/respawn/<name>` | the restart count, wait and state of each respawned daemon ("Restart budget" in docs/SETTINGS.md); the line in `logread` is `respawn: event=respawn service=...` | nothing (RAM) |
| `/var/log/*.err` | the rcS `/proc/odi_omci` writes that failed | nothing |
| `dmesg` | the kernel ring buffer, including `rcS:` progress lines | nothing; the web UI's Tools tab shows it too |
| DRAM ramlog | console output of the last boot | a watchdog reset, not a power cut (`tools/memprobe` from the stock image; `/proc/odi_ramlog_prev` from ours, one boot back) |
| `/etc/config/breadcrumbs`, `trial-diag.txt`, `trial.log` | one line per boot stage, network state, trial rounds | reboots and reflashes (config partition); only with `breadcrumbs.on` or a trial knob |
| `logread` (syslogd) | kernel messages (through klogd), dropbear logins, and the `event=` lines below | nothing locally (64 KB, RAM); everything, forwarded, with `SYSLOG_SERVER` |

## Link and provisioning events

After an outage the question is who started it: the ISP (the fibre, the OLT)
or this stick. Every event that can answer it is one line, `event=<name>`
followed by `key=value` pairs, in syslog -- so `logread | grep event=` on the
stick, or the collector `SYSLOG_SERVER` names (docs/SETTINGS.md), has the
whole story in order, with syslogd's timestamps. Nothing is logged per
message: a whole provisioning session is six lines, plus one per thing the
OLT sent that omcid does not model (nine on ISP1).

The kernel's lines are printk (`kernel: odi_gpon: event=...`, facility kern),
which klogd hands to syslogd; the ONU state machine lives in the driver, so
that is where they come from. omcid's are `omcid[pid]: event=...`, facility
daemon, sent to `/dev/log` the way syslog(3) does (omcid has no libc), and
the same line goes to `/var/log/omcid.log` among the frames around it.

| line | level | when |
|---|---|---|
| `odi_gpon: event=onu_state from=O5 to=O2 in_state_s=3605.120 cause=deactivate_onu_id side=olt onu_id=0` | notice when leaving O5 or entering O7, else info | every ONU state change: the state left, how long it lasted, and why |
| `odi_gpon: event=ranging eqd_bits=... protection=... preassigned=... preassigned_delay=... multframe=... inframe=... onu_id=1` | info | once per O4 to O5 transition: what the Ranging_Time decoded to and the delay written |
| `odi_gpon: event=los state=on onu_state=O5` | notice | the downstream LOS bit set, with the state the loss found; sampled at every GPON interrupt and every 100 ms while the ONU is activated, so a fibre pull shows within 100 ms even with no interrupt. The `event=onu_state` line of what it does to the state follows it (`cause=los`) |
| `odi_gpon: event=los state=off lasted_s=42.310 onu_state=O1` | info | the LOS bit clear again |
| `odi_gpon: event=lof state=on onu_state=O5` / `event=lof state=off lasted_s=...` | notice / info | the same for LOF, loss of downstream framing: light without a GPON frame the framer can lock to |
| `omcid: event=start run=boot mode=reregister reason=not_o5 onu_state=O2 rows=0` | notice on `run=restart`, else info | omcid starting: the first start of the boot or a later one (a respawn), and whether it resumed from its snapshot (`mode=resume`) or starts empty for the OLT to re-provision; `reason` is `not_o5`, `no_snapshot`, `bad_snapshot` or `other_device` |
| `omcid: event=mib_reset side=olt rows=161 mib_data_sync=184 services=6 recent=1 before=get:256.0,set:171.257` | notice | a MIB reset: `side=olt` from the OLT, `side=local` from `omcicli mib reset`; what it threw away. On `side=olt` also `recent`, the OLT resets within the last 600 s with this one, and `before`, the last six requests since the previous reset as `op:class.instance`, oldest first (no MIB upload Nexts, download sections or retransmissions; no key when there were none): what the OLT read or wrote just before it gave up, which is where to look for what it checks |
| `omcid: event=mib_reset_loop state=start resets=3 window_s=600 services=0` | warning (`start`), notice (`end`) | three or more MIB resets from the OLT within 600 s: a reset loop, logged once when it starts and once (`state=end`, with `resets`, the OLT resets of the loop, and `duration_s` from its first reset to its last) after 600 s without one; neither is rate limited. Typical when the OLT does not accept what it reads, but a line that keeps dropping out of O5 resets as often: if `event=onu_state` lines come between the resets, start there. Otherwise check the identity (`GPON_ONU_MODEL`, the vendor and product codes, `OMCI_SW_VER1/2`, docs/SETTINGS.md) and the classes in `/var/log/omcid-unknown.txt`, and read the `before` of the resets. `omcli state` shows the counts (`mib resets`) |
| `omcid: event=mib_upload_begin entities=399` / `event=mib_upload_end entities=399 duration_s=0.009` | info | the OLT reading the MIB back; `entities` is the number of upload records announced (an entity takes one record per 26 bytes of its attributes, as on the stock ONU); an upload it abandons has no end line |
| `omcid: event=provision_begin op=set class=256 inst=0 after_mib_reset=1` | info | the first Create, Set or Delete of a burst; `after_mib_reset=1` is a full re-provisioning |
| `omcid: event=provision_end creates=82 sets=102 deletes=0 duration_s=0.827 rows=161 services=6 after_mib_reset=1` | info | 10 s after the burst's last write: what it added up to (Gets and Tests do not count) |
| `omcid: event=alloc_ids count=5 ids=282,794,1050,1306,538` | info | the Alloc-IDs the OLT assigned by PLOAM (`/proc/odi_gpon/status`), when omcid first reads them and whenever they change: ranging, a deallocation, a re-ranging (`ids=none`); the T-CONTs the OLT does not set are bound to them |
| `odi_gpon: event=o7_hold action=recorded cause=disable_serial_number` | notice | `GPON_KEEP_DISABLE=1`: the OLT disabled this ONU and the stick will stay disabled across its own restarts and reboots. `action` is `recorded` (the OLT disabled it; `cause=gponhold` for the test hook), `kept` (a boot found the marker and starts in O7), `released` (the OLT enabled it; `cause` is `enable_serial_number` or `enable_all_serial_numbers`) or `cleared` (by hand, `gpon-hold.sh clear`); `marker_not_synced` says the helper that writes the marker could not be started |
| `gpon-hold: event=marker action=written` | notice | the marker file `/etc/config/gpon-hold` was written, `removed` or `failed what=write` (or `remove`) |
| `omcid: event=olt_reboot class=256 inst=0 result=refused` | notice | the OLT asked for a reboot: `result=refused` (`OMCI_ACCEPT_REBOOT` off, answered "command not supported"), `accepted` (answered success, the stick reboots a second later), `acknowledged` (`OMCI_ACCEPT_REBOOT=2`, answered success, no reboot) or, for any class but ONU-G, `not_supported` (docs/SETTINGS.md) |
| `omcid: event=uni_lock inst=257 port=0 action=applied effect=host_link_down_until_unlock` | warning (`applied`), notice | the OLT locked or unlocked an Ethernet UNI (ME 11 AdministrativeState), once per change of the port's state. `action=ignored`: a lock answered and stored, the port kept up (the default, `OMCI_UNI_LOCK` not `1`). `action=applied`: `OMCI_UNI_LOCK=1`, the host link goes down now and with it the web UI, ssh, syslog and metrics; sent before the port is touched, so it is the last line a host sees. `action=unlocked`: the OLT unlocked it. `action=cleared` (`inst=0`): a MIB reset or an omcid start unlocked a port a lock had taken down (docs/SETTINGS.md) |
| `omcid: event=snapshot_fail rows=161` | notice | omcid could not write its MIB snapshot (`/var/run/omcid-mib.snap`: a full tmpfs, a failed write or rename); `rows` is the MIB rows held. The previous snapshot stays on disk, so a respawn resumes from an older MIB than the OLT was told about, or re-registers if there is none |
| `omcid: event=sw_image op=download_start inst=1 size=2621440 window=32 result=ok` | notice | a software download step from the OLT: `op=download_start` (the image size and sections per window), `download_end` (`sections=N`, the sections accepted, and `crc=ok` or `bad`), `activate`, `commit`. `result` is `ok`; `not_supported` with `OLT_SW_DOWNLOAD=reject`; `refused_active` (a download to the image that is active or committed), `refused_invalid` (activate or commit of an image that is not valid), `crc_error`, `size_mismatch`, `short` or `no_download` otherwise. Accepted or not, the image is discarded and nothing is flashed (docs/SETTINGS.md, "Software download from the OLT") |
| `omcid: event=unknown_me class=351 op=create mt=4` | notice | the first request this boot for a managed entity class omcid has no model for, per class and operation (`op` is `create`, `set`, `get`, `delete`, `get_next` or `test`; `mt` the message type); answered "unknown entity" unless `OMCI_UNKNOWN_ME_OK=1` (docs/SETTINGS.md) |
| `omcid: event=rx_reject reason=crc count=1 total=1 action=dropped len=48` | notice | a received frame omcid flagged before it read or changed anything. `action=dropped`, no answer: `reason` is `length` (under the 48 bytes of a baseline frame), `devid` (device identifier not 0x0a; the extended format, 0x0b, is not implemented, so an OLT that sends it is left to fall back to baseline; the line adds `devid=11`. omcid reports the baseline set only (`0x80`) unless `OMCC_VER` is set with `omci-identity.on`, see `docs/SETTINGS.md`) or `crc` (the CRC-32 in bytes 44-47 does not match bytes 0-43). `reason=trailer action=accepted`: bytes 40-43 are not `00 00 00 28` (what our OLT sends) but the CRC is valid, so the frame is answered and only counted. At most one line per reason a minute, with `count` the frames since the last line and `total` since omcid started; `omcli state` shows the totals (`rx dropped`). The frames our OLT sends never trigger it |
| `omcid: event=unknown_msg type=17 class=256` | notice | the first frame this boot of a message type omcid does not handle, per type, with the class of that first frame; answered "not supported" |
| `omcid: event=no_instance class=45 inst=2457 op=get mt=9` | notice | the first Get, Get current data, Set or Delete this boot that names an instance of a modelled class which omcid neither created itself nor was given by the OLT, per class and instance (`op` and `mt` are those of the first such request). Diagnostic only: omcid answers it as before (built-in values, a new row for a Set, success for a Delete), where G.988 and the stock stack answer "unknown instance"; the lines say whether an OLT relies on that |
| `omcid: event=vlan_fwdop code=0x06 inst=18 result=unsupported built_as=0x10` | notice | a class 84 forward operation no bridge rule can express (negative filtering, filtering by TCI and MAC address, a code past G.988 table 9.3.11-1): built as 0x10 instead, once per code per omcid run |
| `omcid: event=vlan_fwdop code=0x10 inst=4354 result=override built_as=0x00` | notice | a class 84 port built with the FwdOp the owner set in `RTK_DEVINFO_SPECVER` (docs/SETTINGS.md) instead of the code the OLT wrote; once per port and pair of codes per omcid run |
| `omcid: event=reload changed=vlan action=rebuild keys=VLAN_MANU_TAG_VID` | notice | omcid took a SIGHUP (`apply.sh omci`) and reread the store: `changed` is `none`, `vlan` (bridge connections rebuilt in place, the ONU stays in O5) or `identity` (`action=reregister`: the ONU is deactivated and ranged again); `keys` names the store keys that differ, never their values. Never rate limited |
| `omcid: event=reload_step verb=gponsn rc=0` | info, notice when `rc` is not 0 | one driver verb of a re-registration (`gpondeact`, `gponsn`, `gponpw`, `gponact`) and its errno (0 is success) |
| `omcid: event=reload_done changed=identity action=reregister result=ok duration_s=44.700 o5_s=41.310 services=6` | notice | the reload ended. `result` is `ok`; `failed` (the driver refused `gpondeact`, nothing touched, or `gponact`, the ONU left off the line); `timeout` (O5 not reached in 150 s); `no_services` (O5, but none built); `sn_not_applied` (back, on the old serial number). `o5_s` is the time from `gponact` to O5 with services |
| `host-autoneg: event=fallback setting=off running=on ...` | notice | the host-port auto-negotiation (`HOST_AUTONEG`): the boot watch, the fallback and every trial; the events are listed under `host-autoneg.sh` above |
| `host-serdes: event=fallback setting=2500basex running=fiber1g ...` | notice | the host SerDes mode (`HOST_SERDES`): the boot watch, the fallback, every trial and reapply; the events are listed under `host-serdes.sh` above |
| `sfp-host-ddm: event=requested setting=on ...` | notice | the host-side SFP EEPROM (`SFP_HOST_DDM`): `requested` (the boot asked the kernel), `off` (the key is 0), `safe_mode_boot`, `bad_value`, `no_kernel`, and from `apply` `applied`, `apply_failed` or `apply_timeout`. The kernel's own lines start `odi_sfp_mirror:` (`host EEPROM on`, `host EEPROM off`, and each failure, once) |
| `safe-mode: event=boot_count boot=1 short_before=0 limit=5 wdt_before=0 wdt_limit=3 prev=reboot uptime=11` | notice | every boot's count, and a safe-mode boot (`event=safe_mode`); listed under `safe-mode.sh` above |
| `omcid: event=suppressed count=12 window_s=60` | notice | omcid's rate limit dropped that many lines in the last minute |

`cause` and `side` on `event=onu_state`:

| cause | side | meaning |
|---|---|---|
| `upstream_overhead`, `assign_onu_id`, `ranging_time` | olt | the OLT ranging this ONU: O2 to O3, O3 to O4, O4 to O5 |
| `deactivate_onu_id` | olt | the OLT deactivated this ONU (Deactivate_ONU-ID): back to O2, its ONU-ID, Alloc-IDs and OMCC port given up until the OLT ranges it again |
| `disable_serial_number` | olt | the OLT disabled this serial number (rogue ONU handling, or by hand at the OLT): O7, laser off (the module's TX-disable line, GPIO 13, driven high; `laser_emergency_stop 1` in `/proc/odi_gpon/status`) |
| `enable_serial_number`, `enable_all_serial_numbers` | olt | re-enabled: O7 to O2, laser on again |
| `popup` | olt | POPUP: O6 back to O5 (directed), or to O4 (broadcast) to be ranged again from the pre-assigned delay |
| `to1_expired` | timer | O3 or O4 to O2: the OLT did not finish ranging within TO1 (10 s from entering O3) |
| `to2_expired` | timer | O6 to O1: no POPUP within TO2 (100 ms); the ONU-ID and every Alloc-ID and OMCC row go, and it ranges again once the light is back |
| `los`, `lof` | line | loss of signal, or of framing alone (G.984.3 Table 10-1, "ONU detects LOS or LOF"): O5 to O6 (TO2 starts), O2, O3 or O4 to O1 |
| `los_cleared` | line | LOS and LOF both clear: O1 to O2, and the OLT ranges the ONU again; also right after `to2_expired` when the light came back inside the O6 window |
| `gponact`, `gpondeact` | local | this side: the boot activation, a respawn re-provisioning (`omci-respawn-reprovision.sh`), a re-registration omcid makes on SIGHUP (`apply.sh omci`), or a write to `/proc/odi_init` by hand |
| `ploam_other`, `ploam_unknown` | olt | a transition after any other downstream message; not expected |

Reading an outage:

- `event=los state=on`, `event=onu_state from=O5 to=O6 ... cause=los`,
  `from=O6 to=O1 ... cause=to2_expired`, then, once the light is back,
  `event=los state=off`, `from=O1 to=O2 ... cause=los_cleared` and the
  ranging (`upstream_overhead`, `assign_onu_id`, `ranging_time`): the
  fibre, or the OLT side of it, and the ONU coming back by itself. With
  the light back, an ONU that goes O2 to O3 and back on `to1_expired`
  again and again is on an OLT that does not take it: a fibre to another
  ISP, or a serial number this OLT does not know.
- `event=onu_state from=O5 ... cause=deactivate_onu_id side=olt` with no LOS:
  the OLT dropped this ONU. A `mib_reset side=olt` and a provisioning burst
  follow if it re-provisions.
- `event=reload changed=identity ...` from omcid, then `cause=gpondeact
  side=local` and, some seconds later, the OLT ranging the ONU again and
  `event=reload_done result=ok`: `apply.sh omci` re-registered the ONU after an
  identity key changed, and the process never restarted (no `event=start`).
- `event=start run=restart mode=reregister` from omcid, then
  `from=O5 ... cause=gpondeact side=local`: omcid died and this side forced
  the re-registration. `mode=resume` and no state change: omcid died and
  nothing on the line noticed.
- `event=olt_reboot result=refused` followed by a deactivation: the OLT
  gave up on a reboot this image refuses (`OMCI_ACCEPT_REBOOT` is off).
- `event=sw_image op=activate ... result=ok`, then a deactivation or a new
  `download_start`: the OLT expected the ONU to reboot into the image it
  sent; omcid only reports it active (docs/SETTINGS.md).
- A board reset by the watchdog leaves no line here (the syslog buffer is in
  RAM, and forwarding stops with the board); the ramlog of the next boot
  records it (docs/KERNEL.md).

What omcid does not model. Each unknown (class, operation) and each unknown
message type is one line per boot, however often the OLT repeats it, and
`/var/log/omcid-unknown.txt` keeps all of them with a count and the uptime of
the first, even the ones the rate limit dropped:

    unknown_me class=351 op=create mt=4 count=1 first_uptime_s=38
    unknown_msg type=17 class=256 count=9 first_uptime_s=39
    no_instance class=45 inst=2457 op=get mt=9 count=3 first_uptime_s=52

A respawned omcid reads the file back first, so "once" holds across a
restart; `/var/log` is RAM, so a reboot starts it empty. It holds 64 distinct
entries and counts the rest on an `overflow` line. `diag-bundle.sh` collects
it with the rest of `/var/log`, as `odi-diag/log/omcid-unknown.txt`. On ISP1
the OLT sends class 351 once and message types 17 and 1 to ONU-G three times
each per provisioning: those three lines are what a session looks like there.

Rate limits: the kernel lines have their own printk ratelimit, 30 a minute
(the kernel prints how many it dropped); omcid's, 20 a minute, with an
`event=suppressed` line for what it dropped. A flapping ONU (O2 to O3 to O2
every 10 s) stays well under either.

## Restarting a daemon

Since v1.0.4, every daemon (metricsd, confd, dropbear, omcid, and later
syslogd, klogd and ntpd) is a `respawn` entry in `/etc/inittab` (`docs/SETTINGS.md`, "native over hand-rolled"):
busybox init, which forked each one itself, restarts it the instant it
exits -- no session hang-up can reach the daemon, since it is a child of
init (pid 1), never of your ssh session. So the plain way to
restart one is just to kill it:

    kill $(pidof metricsd)
    kill $(pidof confd)
    kill $(pidof omcid)

each comes back with a new pid within a second or two, no `setsid` needed.
A daemon killed again within 30 s of its start waits first, 1 s, then 2 s,
4 s ... up to a cap of 2 s (omcid) to 60 s; `cat /var/run/respawn/<name>`
shows the count and the wait, and the kernel log carries one `respawn:
event=respawn` line per restart (`docs/SETTINGS.md`, "Restart budget").
For dropbear, kill the listening process (the one without `-2` in `ps`);
open sessions are separate processes and stay up.

After an omcid restart, check `cat /proc/odi_omci` shows `registered:
type=1` with the new pid, and `omcli state`. The new omcid resumes from its
snapshot when the ONU is still in O5, and otherwise has an empty MIB until
the OLT provisions again. To make it reread the config store there is no
need to restart it: `kill -HUP $(pidof omcid)` does that, and
`/etc/scripts/apply.sh omci` is the same signal with a report of the outcome
(a VLAN change is rebuilt in place, an identity change re-registers the ONU
and interrupts internet for it). Do not start an omcid by hand next to the
one init runs: the second refuses to start while the first holds the OMCI
channel, and one started with `-f` takes it and leaves init's respawn entry
re-provisioning around a stranger.

`/etc/init.d/services {stop|start} <name>` (syslogd, klogd, ntpd, metricsd,
confd, dropbear or omcid) is for turning one off across restarts: `stop` writes
`/etc/config/<name>.off` (so it stays off across a reboot, until removed)
and kills the current process; `start` clears the flag and kills whatever
is currently running under that entry, so init brings the real daemon back
at once. A plain `kill` alone, with no `.off` flag, always just comes back
-- that is the whole point of `respawn`.

## Dangerous or risky commands

See `AGENTS.md` for the same list aimed at anyone (human or AI) changing
this repository:

- `omcicap` takes the OMCI channel from omcid (it refuses without `-f`), and
  `omciprobe -f` writes behind omcid's back.
- Killing omcid leaves the OLT unanswered until init has respawned it.
- `diag register set` and writes to `/proc/odi_init` or `/proc/odi_omci`
  reprogram the hardware under a running stack.
- `diag acl-probe trap` keeps the IGMP of the host from the OLT while it is
  in: a line carrying IPTV loses its joins for that long. The counting
  probes change no forwarding.
- `igmpd -w` (the daemon or `-j`/`-l`) writes static L2 multicast entries.
  The driver refuses unicast, broadcast and 01:80:c2:00:00:0x addresses, but
  a wrong member mask on a group that carries traffic misforwards it
  silently. `diag l2-table` itself only reads.
- Reading an undecoded SoC register address with `devmem` can stall the bus
  until the watchdog resets the stick.
- `nv setenv sw_commit` removes the trial-boot fallback; `flash_eraseall` on
  the config partition erases the identity (`GPON_SN`, `ELAN_MAC_ADDR`,
  `MAC_KEY`) the line authenticates on.
- `diag` fed from a stdin that never closes waits forever: give it a pipe or
  a file, and a `timeout`.
