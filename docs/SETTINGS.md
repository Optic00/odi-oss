# Settings

What you can configure on a stick running this image, what each setting
actually does here, and what applying it costs. The config store this image
shares with the stock firmware holds 184 keys; this image acts on 24 of them
(22 the web UI (`confd`) offers for editing, bits of `OMCI_CUSTOM_ME` and one
bit of `OMCI_CUSTOM_MCAST`), besides keys of its own that the stock firmware
never had. `omcicli` also prints the four `OMCI_CUSTOM_*` masks and
`DUAL_MGMT_MODE`, as the stock one does; apart from those bits, they change
nothing here. This page says
which is which, and why each one costs what it does.

The programs named below are described in [`docs/TOOLS.md`](TOOLS.md).

## Read this first

**Where settings live.** In two XML files on the jffs2 `config` partition,
`/var/config/lastgood.xml` (CS: the service settings) and
`/var/config/lastgood_hs.xml` (HS: the hardware identity), reached as
`/etc/config/`. The web UI writes them through `/etc/scripts/flash set`,
which changes the file and nothing else; applying is a separate step (below).
A few more settings are plain files on the same partition ("Switch files").

**The partition is shared with the other slot.** Reflashing never touches
it, and whichever slot boots reads it. On a stick that keeps the stock
firmware as its committed slot, every key here -- including the 161 this
image does not act on -- is still what the stock firmware reads when the stick
falls back to it. Do not delete keys this image does not use, and keep
backups whole: the UI's backup and restore carry every key.

**The management is reachable from the host side only.** The web UI
(port 80), ssh and the exporter (port 9100) listen on every address of the
stick, and no setting changes that: nothing that arrives from the fibre
reaches the stick's IP stack, whatever the OLT provisions or the VLAN
settings below say. The switch never forwards a frame from the PON port to
the CPU, and the NIC driver drops any non-OMCI frame from the PON port that
gets there anyway (`docs/SWITCH.md`, "PON port isolation"). The other
way, the stick sends nothing toward the fibre but its OMCI replies. The kernel
parameter `odi_switch.pon_isolate=N` turns the switch half off, should an
OLT's OMCI ever need it; the driver half has no switch. Everyone on the
host side can reach the management, so change the web UI's default login.

**Apply classes.** Every setting and control is marked with one of four
classes, in this document and beside it in the UI:

| class | meaning | how it is applied |
|---|---|---|
| **LIVE** | takes effect at once, no interruption | `apply.sh network` (the management addresses), `apply.sh omci` (the four VLAN keys, rebuilt in place) or `apply.sh sfp` (`SFP_HOST_DDM`); the UI runs it straight after the save |
| **SERVICE RESTART** | a daemon restarts; the fibre service stays up | `apply.sh syslog`/`apply.sh ntp` |
| **INTERRUPTS INTERNET** | applied without a reboot, but the fibre service drops while it is | `apply.sh omci`; the UI offers "Apply now" behind a confirmation |
| **REBOOT** | read only at boot | a reboot; the UI offers "Reboot now" behind a confirmation |

**How apply works (`apply.sh omci`).** The script sends SIGHUP to the
running omcid, the Unix convention for "reread your configuration", and
waits for its answer; it never starts or restarts an omcid, init owns the
process. omcid rereads the store and compares it with what it is running:

- **Nothing changed**: logged (`event=reload changed=none`), nothing done.
- **Only the four VLAN keys changed** (`VLAN_CFG_TYPE`, `VLAN_MANU_MODE`
  including transparent mode 0, `VLAN_MANU_TAG_VID`, `VLAN_MANU_TAG_PRI`),
  or bit 1 of `OMCI_CUSTOM_MCAST`:
  omcid tears the bridge connections down and builds them again from the MIB
  it holds, by the same code as the first build. The ONU stays in O5 and is
  not re-ranged: **LIVE**. The rules it builds are the ones a fresh omcid
  builds from the same MIB and store (`src/omci/reload-test.sh` compares them
  byte for byte).
- **Anything the OLT must see again** changed (`GPON_SN`,
  `GPON_PLOAM_PASSWD`, the LOID keys, `OMCI_SW_VER1/2`, `GPON_ONU_MODEL`,
  `OMCC_VER`, `OMCI_VENDOR_PRODUCT_CODE`, `ONU_HW_VERSION`, the identity
  switch, `OMCI_UNKNOWN_ME_OK`): **INTERRUPTS INTERNET**. omcid itself writes
  `gpondeact`, clears its MIB as a MIB reset does, holds three seconds, gives
  the driver the new serial number and password (`gponsn`, `gponpw`, the
  same verbs rcS writes at boot), writes `gponact`, and the OLT sees the ONU
  range again, resets its MIB and provisions every service from scratch. The
  process stays up: no respawn, no snapshot resume, no second omcid. The
  report keys count only while the identity switch is on, since with it off
  they are stored and shown, never sent.

The script reports which of the three happened and, for a re-registration,
how long until the ONU was back in O5 with services; it waits 20 s by
default (`APPLY_WAIT`, counted from the signal), inside the 25 s the web UI gives it, and a
re-registration still running then is reported as running, not as failed.
Exit 1: no omcid running, no answer to the signal, or a `failed`, `timeout`,
`refused`, `held`, `no_services` or `sn_not_applied` result (`docs/TOOLS.md`, "omcid"). Every
step is an `event=reload` line in syslog (`docs/TOOLS.md`, "Link and
provisioning events").

**Refused re-registrations are counted, and two in a row hold the next
change back.** omcid records every re-registration and whether the OLT took
it (`/etc/config/odi-reregister`, `omcli state`); three Deactivate_ONU-IDs
before O5, or a disabled serial number (O7), are a refusal, and so is no O5 at
all. After the first refusal `apply.sh omci` warns; after two in a row, within
24 h, omcid does not re-register for a changed identity until the reload is
confirmed with `apply.sh omci --confirm`, and the ONU stays on the identity it
runs (`result=held`). The store keeps what was saved: a reboot or an omcid
respawn applies it unconfirmed. Details in `docs/TOOLS.md` ("omcid",
"Re-registrations are counted").

A serial number is applied by rewriting the PLOAM slot the boot activation
armed, which the driver allows only while the ONU is deactivated; the
password is read by the driver at every Request_Password, so it needs only
the verb. Measured on ISP1 with the 6.18 kernel, for the earlier apply that
restarted omcid: the ONU back at O5 with its six services about 13 s after
the command. The in-daemon re-registration writes the same kernel verbs and
was run on a stick and came back to O5 with its six services; its time was
not measured (below, "Not verified yet").
The earlier apply started a second omcid beside the one init respawns, and
each respawn found the first still running and forced the line down and up
again: 44 forced re-activations in two minutes, the ONU cycling O1 to O3 and
never reaching O5. There is one omcid now, and a genuine crash still
respawns and re-provisions as before (`docs/BOOT.md`).

**Every REBOOT also interrupts internet.** A reboot takes the ONU off the
line: on ISP1, 75 to 81 s from `reboot` until userland confirms (the
management address answers), then the PON steps range the ONU back to O5. And on a stick running
a **trial slot**, a reboot does not come back to this image: it boots the
committed slot, which is normally the stock firmware (`docs/FLASHING.md`).
The UI's reboot confirmation says which slot it comes back on.

## Settings this image reads

| key | UI label | what it does here | read by | apply |
|---|---|---|---|---|
| `LAN_IP_ADDR` | Management IP | the `br0` address ssh, the UI and the exporter answer on | `network.sh` | LIVE (a trial) |
| `LAN_SUBNET` | Management netmask | its netmask | `network.sh` | LIVE (a trial) |
| `LAN_ENABLE_IP2` | Second management address | 1 adds a second address on `br0:2`, 0 removes it | `network.sh` | LIVE (a trial) |
| `LAN_IP_ADDR2` | Second management IP | that address (192.168.100.1 is the stock default, and what both sticks store) | `network.sh` | LIVE (a trial) |
| `LAN_SUBNET2` | Second management netmask | its netmask | `network.sh` | LIVE (a trial) |
| `LAN_ROUTES` | Static routes of the management interface | Optional routes so the stick answers clients on other subnets without NAT, e.g. `default via 192.168.1.253` when a router holds the stick subnet at `192.168.1.253`. Up to 8 entries separated by commas, each `DEST via GW` (single spaces around `via`, extra spaces tolerated): `DEST` is `default` or an IPv4 `a.b.c.d/len` (len 0 to 32, host bits zero; `0.0.0.0/0` is `default`), `GW` an IPv4 host address inside the primary management subnet or, with `LAN_ENABLE_IP2` on, the second one (not the stick own address). Anything malformed, more than 8 entries, a destination twice or a gateway outside those subnets turns the whole key off, with one `network: LAN_ROUTES off: ...` line saying why. Empty or absent is no routes, the default, and the default route is allowed. Odi-only key, kept in `/etc/config/odi.conf`; IPv4 only. Applied by `network.sh` after the addresses, at boot and on `apply.sh network`, on the device that carries the primary address (`br0`, or `eth0` after the bridge fallback), never on another: the routes of `proto static` on that device are flushed and the listed ones installed with `ip route replace ... proto static`, so a re-apply drops what the key no longer lists and touches nothing else. Not applied in a safe-mode boot. A change is a trial, with the addresses ("Management address trial" below). The routes change what the stick answers, not what reaches it: nothing is reachable from the fibre whatever they say (docs/SWITCH.md, "PON port isolation") | `network.sh` | LIVE (a trial) |
| `VLAN_CFG_TYPE` | VLAN config mode | 1 lets the manual tag through; anything else is no manual tag, and the OLT's class 171 rule drives the tagging when it has the supported shape ("VLAN handling" below) | omcid, on SIGHUP | LIVE |
| `VLAN_MANU_MODE` | VLAN handling | 1 ("stick tags"): tags with the VID and priority below; 0 ("router tags", transparent): no tag added or removed, every tag passes as the OLT provisioned it ("VLAN handling" below); anything else is 0 | omcid, on SIGHUP | LIVE |
| `VLAN_MANU_TAG_VID` | Service VLAN ID | the C-VLAN the ONU adds upstream and strips downstream on the services the OLT provisions | omcid, on SIGHUP | LIVE |
| `VLAN_MANU_TAG_PRI` | VLAN priority | the 802.1p bits of the tag the stick adds to the router's untagged frames going up, 0 to 7; a stored value above 7 (255 is the factory one) is used as 0 and the tag stays on. Downstream the tag is removed whatever priority the OLT sends: only the VID is matched, except with the manual VID 0, beside a class 84 filter that goes by priority alone, or beside the class 171 rules of an IP host, where the tag is removed only at this priority and omcid logs which of them applies | omcid, on SIGHUP | LIVE |
| `VLAN_EAPOL_UNTAGGED` | 802.1X EAPOL without the service tag | `1`, with the manual tag on ("stick tags"): EAPOL frames (EtherType 0x888e) cross the stick **untagged** in both directions while everything else keeps the manual tag. Anything else, or absent, is off, and then no rule differs from the manual tag alone. With the manual tag off the key does nothing, and omcid logs that once. For an ISP that authenticates the line with 802.1X on the untagged (or VLAN 0) frames while the service itself is on a VLAN ("VLAN_EAPOL_UNTAGGED" below). Odi-only key, kept in `/etc/config/odi.conf`. Not run yet against a line that uses EAP | omcid, on SIGHUP (`apply.sh omci`) | LIVE |
| `RTK_DEVINFO_SPECVER` | VLAN forward operation override | the stock firmware's FwdOp override, same key and format: `EntityId[:FwdOp]` entries separated by commas (e.g. `0x1102:0x00,0x110b`; no FwdOp means 0x02). The bridge rules of each class 84 port named are built with that FwdOp instead of the OLT's; the attribute keeps the OLT's value. Anything malformed turns the whole key off ("VLAN handling" below) | omcid, on SIGHUP | LIVE |
| `GPON_PLOAM_PASSWD` | PLOAM password (identity) | the PLOAM password: exactly 20 hex digits are 10 bytes, 1 to 10 characters are the text itself (even if they are hex digits), `0x` and 2 to 20 hex digits are those bytes zero-padded; anything else is refused and logged, keeping the old password; empty means none is sent | the `gponpw` PON step in rcS (`rcs-lib.sh`, `pon_step_gponpw`, skipped when empty); omcid re-sends it on a re-registration | INTERRUPTS INTERNET |
| `LOID`, `LOID_PASSWD` | LOID, LOID password | answered to CTC-profile OLTs in the LOID authentication entity | omcid, on SIGHUP | INTERRUPTS INTERNET |
| `LOID_OLD`, `LOID_PASSWD_OLD` | LOID (in force) | the value actually answered when it differs from the one above | omcid, on SIGHUP | INTERRUPTS INTERNET |
| `OMCI_SW_VER1`, `OMCI_SW_VER2` | Software version, image 0/1 | the version reported for each software image (ME 7 attribute 1, 14 characters) | omcid, on SIGHUP, with `omci-identity.on` | INTERRUPTS INTERNET |
| `GPON_ONU_MODEL` | ONU model (identity) | the equipment id in ONU2-G (attribute 1, 20 characters) | omcid, on SIGHUP, with `omci-identity.on` | INTERRUPTS INTERNET |
| `OMCC_VER` | OMCC version | ONU2-G attribute 2, decimal. Without `omci-identity.on` omcid reports `0x80` (128), the baseline message set only. omcid implements only the baseline set: a value copied from another ONU that advertises the extended set too lets the OLT send extended (device identifier `0x0b`) messages, which omcid drops and counts (`event=rx_reject reason=devid`, `omcli state`) and an OLT that does not fall back to baseline then never provisions the stick; keep a baseline-only value | omcid, on SIGHUP, with `omci-identity.on` | INTERRUPTS INTERNET |
| `OMCI_VENDOR_PRODUCT_CODE` | Vendor product code | ONU2-G attribute 3, decimal | omcid, on SIGHUP, with `omci-identity.on` | INTERRUPTS INTERNET |
| `ONU_HW_VERSION` | ONU hardware version (identity) | the Version in ONU-G (attribute 2, at most 14 printable ASCII characters), the hardware version some OLTs whitelist. Odi-only key, kept in `/etc/config/odi.conf` (see "Odi-only keys" below) | omcid, on SIGHUP, with `omci-identity.on` | INTERRUPTS INTERNET |
| `ELAN_MAC_ADDR` | UNI MAC address (identity) | the MAC of `eth0`, `eth0.2` and `br0` | `network.sh` at boot | REBOOT |
| `GPON_SN` | ONU serial number (identity) | the serial the OLT authenticates; a wrong value means no service | the `gponsn` PON step in rcS (`rcs-lib.sh`, `pon_step_gponsn`); omcid on SIGHUP | INTERRUPTS INTERNET |
| `SYSLOG_SERVER` | Remote syslog server | `host[:port]` syslogd forwards a copy of every message to, with `-R` -- kernel messages and the link and provisioning `event=` lines included (docs/TOOLS.md, "Link and provisioning events"); empty means local only (the circular buffer, `logread`). Odi-only key, kept in `/etc/config/odi.conf` (see "Odi-only keys" below). Set from the web UI (Config, other), which saves it and runs `apply.sh syslog`; an empty value clears it (clearing from the UI needs odi-ui v1.0.8) | `svc-syslogd.sh` | SERVICE RESTART |
| `OMCI_UNKNOWN_ME_OK` | Answer unknown entities with success | `1` answers a Create, Set or Get of a managed entity class omcid has no model for with success instead of "unknown entity" (result 4), the counterpart of the stock `OMCI_FAKE_OK`, which this image does not read (ME 290, Dot1X port extension package, is modelled and needs no key: a Set of its writable attributes is answered with success and stored, not acted on); anything else, or absent, keeps the error. Default off. Odi-only key, kept in `/etc/config/odi.conf` (see "Odi-only keys" below). For an OLT that stalls its provisioning at an "unknown entity" answer; the risk is below | omcid, at start and on SIGHUP | INTERRUPTS INTERNET |
| `OMCI_FIXED_ME` | Answer unknown classes with fixed bytes | Up to 16 entries, separated by spaces or commas, in two forms. `CLASS:HEX`: the class in decimal (1 to 65535) and the attribute bytes in hex (optional `0x`, 1 to 25 bytes), e.g. `200:0x0011aa`; omcid answers a Get or Get current data of ANY instance of the class with the bytes, under the attribute mask the OLT asked for, and the entity is not in the MIB upload. `CLASS:INSTANCE:ATTR:HEX`: one attribute of one entity, the instance 0 to 65535 in decimal or `0x` hex and the attribute 1 to 16, e.g. `200:0x8001:1:0x0011aa`; entries with the same `CLASS:INSTANCE` make one entity with several attributes. That entity is listed in the MIB upload (its attributes in attribute order, split over more than one upload response when they do not fit in one), a Get of it answers the attributes asked for that it has (an attribute it does not have is left out of the answered mask, as omcid does for its own entities), and a Get of any other instance of the class answers "unknown managed entity instance" (result 5). Only for classes omcid has no model of its own for: a class it implements is never overridden. For an OLT that expects a native ONU's vendor-specific entities; copy the bytes from a native ONU. Empty or absent is off; anything malformed (a bad entry, a class twice in the first form, the same class, instance and attribute twice, a class in both forms, more than 16 entries) turns the whole key off. Odi-only key, kept in `/etc/config/odi.conf`; the bytes are not logged, and the diagnostics bundle redacts the key. A Create or Set of such a class is answered as for any unknown class (`OMCI_UNKNOWN_ME_OK`) | omcid, at start and on SIGHUP | INTERRUPTS INTERNET |
| `OMCI_VEIP_SLOT_ID` | VEIP slot | The slot number the virtual Ethernet interface point (VEIP) is created in, decimal, 2 to 254 and not 128. Unset, the VEIP is in slot 6, as on the stock firmware: the VEIP, its UNI-G and the TR-069 server are entity `0x0601`, the cardholder and circuit pack `0x0106`. With a slot, those are `slot << 8 \| 1` and `0x0100 \| slot` (slot 14: `0x0e01` and `0x010e`). As on the stock firmware the slot is used only while bit 256 (`0x100`) of `OMCI_CUSTOM_ME` is set; without that bit the VEIP stays in slot 6 and only the VEIP attribute "customized slot id" (329 attribute 6) reports the key. The stock key, kept in `lastgood.xml` (not `odi.conf`); a value that is not a number or not in range is ignored, with a line in the omcid log. For an OLT that provisions the VEIP at a slot other than 6; the ids the OLT addresses must match | omcid, at start and on SIGHUP (re-registers) | INTERRUPTS INTERNET |
| `OMCI_VEIP` | Create a VEIP | `0` creates no VEIP at all, so the ONU presents only its PPTP Ethernet UNI (cardholder and circuit pack `0x0101` and `0x0180`, PPTP Ethernet UNI `0x0101`): the VEIP, its UNI-G, the TR-069 server and the VEIP slot's cardholder and circuit pack (five entities) are not created, the MIB upload announces five fewer, and a Get of them is refused as an unknown instance. Anything else, or absent, creates the VEIP (in the slot `OMCI_VEIP_SLOT_ID` names). A separate odi-only key, kept in `/etc/config/odi.conf` (see "Odi-only keys" below), and not a value of `OMCI_VEIP_SLOT_ID`: the stock firmware reads that key as a slot number only, so a `0` there would put its VEIP in slot 0 if the stick fell back to it. For an OLT that provisions a PPTP-only ONU; the OLT must not create bridge ports on a VEIP | omcid, at start and on SIGHUP (re-registers) | INTERRUPTS INTERNET |
| `OMCI_PRIVATE_ME` | Vendor-private entities | `0` leaves out the three vendor-private managed entities the ONU creates for itself, ME 350, 370 and 373 (ME 373 carries a third-party product string, `PRODUCT ID`); they are then absent from the MIB upload and a Get of them is refused as an unknown entity. Anything else, or absent, creates them, which is what the stock firmware does on both sticks it was checked on (the stock firmware has no key for it: it creates them whatever `OMCI_CUSTOM_BDP` holds). Odi-only key, kept in `/etc/config/odi.conf` (see "Odi-only keys" below). Turn it off only for an OLT that rejects or mistrusts these entities | omcid, at start and on SIGHUP (re-registers) | INTERRUPTS INTERNET |
| `OMCI_POTS_PORTS` | Phone (POTS) ports (experimental) | `1` or `2`: the ONU tells the OLT it has that many phone ports, so an OLT that offers voice can provision a SIP line on each (SIP over OMCI, G.988 clause 9.9), for the router behind the stick to register: the MIB upload gains a circuit pack of type POTS with its cardholder in a slot of its own (`OMCI_POTS_SLOT_ID`), one PPTP POTS UNI per port with its UNI-G and VoIP line status, and the VoIP config data, which offers SIP configured over OMCI only (an OLT that picks another method, which would fetch a file or talk to a server toward the fibre, reads the configuration state as failed). omcid then accepts, keeps and answers back everything the OLT creates and sets for the line (the SIP agent and user, the voice service, media and RTP profiles, the dial plan, the application profile and feature codes, the voice CTP, the voice PM entities); the SIP password and the subscriber's display name are kept out of the log and masked in `omcicli mib get` like the other credentials (docs/TOOLS.md). **The stick has no phone socket, places no call and registers nothing**, and says only that: a POTS UNI is enabled while the OLT leaves it unlocked (the port is provisioned, not working), its hook is on, a Test of it is refused (result 2), no voice alarm is ever raised, the PM counters stay 0, and the line status reads "port not configured" (13) until the OLT provisions a SIP line on the port and "config done" (14) once it has -- never "registered" or "in session", which only the router could say (with `OMCI_VOIP_LINE_REPORT` the router's own report may say so, below). Impedance, gains, holdover time and the like are stored and have no effect. Emergency calls: the stick provides none, and no battery holdover; what the router does is the router's. **The voice goes to the router, tagged as the OLT sets it**: a bridge port the OLT builds on the voice IP host is the host port, toward the router, not the stick's own CPU port as in the stock firmware. The voice IP host is the one a SIP agent names, through its TCP/UDP config data; any other IP host the OLT builds (for TR-069 management, say) keeps the stock mapping and gets no rule, so its VLAN never reaches the router. The voice VLAN the OLT names -- in the IP host's extended VLAN table (class 171), which tags the IP host's untagged frames, or in the VLAN filter of the voice GEM port (class 84) -- reaches the router tagged, with the tag kept downstream, and the router's SIP client sends it tagged with that VLAN; the priority the OLT assigns is set on the way up. A rule of the IP host that would match untagged frames, or tagged ones of any VLAN, is not installed, because on the host port those are every other service's frames (the data service's untagged handoff included), and omcid logs why; such an OLT plan carries no voice. The stick itself stays unreachable from the fibre: no rule names its CPU port, and the PON isolation still cuts it (docs/SWITCH.md, "PON port isolation"). An OLT told of a phone port may also start line tests, expect line status "registered", or provision the data service differently: not observed, which is why this is off by default. `0`, empty, absent or anything else: no phone port, the MIB upload is the stock one, the VoIP classes are unknown to the OLT ("unknown entity", result 4), as before omcid modelled them, and a bridge port on any IP host gets no rule at all. Odi-only key, kept in `/etc/config/odi.conf` (see "Odi-only keys" below) | omcid, at start and on SIGHUP (re-registers) | INTERRUPTS INTERNET |
| `OMCI_POTS_SLOT_ID` | Phone ports' slot | The slot of the POTS circuit pack `OMCI_POTS_PORTS` creates, decimal, 2 to 254 and not 6, 128 or the VEIP's (`OMCI_VEIP_SLOT_ID`): the cardholder and circuit pack are `0x0100 \| slot`, the ports `slot << 8 \| port` (slot 9: `0x0109`, `0x0901` and `0x0902`). Unset, or a value that is not a free slot (logged and ignored), the first slot from 2 up that the ONU does not already use, 1 the Ethernet UNI, 128 the ANI, 6 and the VEIP's: slot 2 unless the VEIP is there. For an OLT whose voice profile expects the ports at a given slot. Odi-only key, kept in `/etc/config/odi.conf` (see "Odi-only keys" below); no effect while `OMCI_POTS_PORTS` is 0 | omcid, at start and on SIGHUP (re-registers) | INTERRUPTS INTERNET |
| `OMCI_VOIP_LINE_REPORT` | Router-reported SIP line status (experimental) | `1` lets the router behind the stick tell the stick what its SIP client really does, and the VoIP line status (ME 141, voice server status) of a line the OLT has provisioned follows it: 1 registered, 2 in session, 3 to 7 registration failed and 8 to 12 invite failed (each: ICMP error, TCP error, authentication, timeout, server fail code; G.988 9.9.11), 0 none. The router reports through the web UI (`POST /api/voip/status`, odi-ui `API.md`), which keeps the report in `/var/run/voip-line-status` (RAM, gone on a reboot): one line a port, the state word and the time it was received, no number, name, server or password. **Only fresh facts count**: a report older than 300 s, one from the future, an unreadable one, an unknown state word or a report for a port the stick does not advertise is ignored, and the line reads "config done" (14) as it does without the key; a port the OLT has not provisioned a line on stays "port not configured" (13) whatever the router says, and the router must report at least every few minutes (every 60 s is plenty) to keep a state standing. It changes nothing but this one attribute, only while `OMCI_POTS_PORTS` is on, and an OLT that read "registered" is told nothing more than the Get answers. Anything else, empty or absent: the report is not read at all and the line status is exactly as before. Odi-only key, kept in `/etc/config/odi.conf` (see "Odi-only keys" below); read at every Get, so a change needs neither a reload nor a re-registration | omcid, at every Get of ME 141 | LIVE (the next Get) |
| `NTP_SERVER` | NTP server | starts `ntpd` against this server; empty means no NTP client runs at all (new versus stock, which has neither an RTC nor an NTP client). Odi-only key, kept in `/etc/config/odi.conf` (see "Odi-only keys" below). Set from the web UI (Config, other), which saves it and runs `apply.sh ntp`; an empty value clears it (clearing from the UI needs odi-ui v1.0.8) | `svc-ntpd.sh`; ntpd's state is written to `/tmp/ntp-status` (docs/TOOLS.md, `ntpd`) | SERVICE RESTART |
| `ODI_LASER_UNVERIFIED` | Laser on an unverified laser driver | `1` switches the laser on even when the laser driver chip is not the Semtech GN25L95 this image was validated with (the boot runs the `laser unverified` form of the `laser` PON verb); anything else, or absent, keeps the laser off on such a stick and refuses GPON activation: the `gponact` verb fails (`-EPERM`, so `apply.sh omci` reports a re-registration as `failed`), `/proc/odi_gpon/status` shows `laser on 0 ldd_id <id byte, or -1> unverified 0`, and the reason is in `dmesg` (`odi_board: laser:`). A forced laser shows `laser on 1 ldd_id <id> unverified 1`. Other boards may need laser set-up this image does not do; a laser that lights outside its timeslots takes down every other ONU on the OLT port. Set it only on an OLT port with no other ONU. Odi-only key, kept in `/etc/config/odi.conf` | rcs-lib.sh, the `laser` PON step at boot | REBOOT |
| `OLT_SW_DOWNLOAD` | OLT software download | what omcid answers when the OLT pushes a software image: `accept` (the default, also when the key is absent, empty or any other value) answers every step with success and discards the image; `reject` answers "not supported", as omcid did before. Nothing is ever written to flash or the U-Boot environment and the stick never reboots, in either mode ("Software download from the OLT" below). Odi-only key, kept in `/etc/config/odi.conf` | omcid, at every Start software download, Activate and Commit | LIVE (the next download) |
| `OMCI_ACCEPT_REBOOT` | Accept a reboot from the OLT | `1` lets the OLT reboot the stick: a Reboot (OMCI message type 25) of the ONU-G is answered with success and, about a second later, omcid sends SIGTERM to init, which is what `reboot` does, so the shutdown entries of `inittab` run. `2` acknowledges only: the Reboot is answered with success and nothing else happens, for an OLT that insists on a successful answer. Anything else, or absent, refuses it with result 2, "command not supported". Default off. The reboot condition byte of the request (unconditional, or only without a voice or emergency call) is ignored: the stick has no voice service, so every request is unconditional. A Reboot of any other class keeps the old answer. Every request is logged (`reboot from the OLT accepted`, `acknowledged, not rebooting` or `refused`, and `event=olt_reboot`, docs/TOOLS.md). A reboot with no committed trial returns to the stock slot, as any reboot does. Odi-only key, kept in `/etc/config/odi.conf`; read when the Reboot arrives, so a change needs neither a reload nor a re-registration | omcid, at every Reboot from the OLT | LIVE (the next Reboot) |
| `OMCI_TCONT_ALLOC_UNSET` | What a T-CONT the OLT never set reports as its Alloc-ID | `ploam` (the default, also for an absent or unknown value) reports, for a T-CONT the OLT has not Set, the next Alloc-ID the OLT assigned by PLOAM, in assignment order, as the stock firmware does. `unassigned` reports `0x00FF`, the "not assigned" value of the standard (G.988 9.2.2), in a Get, in the MIB upload and in `omcicli`. For an OLT that provisions a T-CONT by Setting its Alloc-ID and picks the first T-CONT that reports empty: with `ploam` it sees the first one as taken and puts the service on the second (`0x8001` instead of `0x8000`). A T-CONT the OLT did Set reports the OLT's value in both modes, and the key changes nothing else: the T-CONT still takes the PLOAM Alloc-ID in the switch, the GEM port network CTP pointers (ME 268) are the same, and a line whose OLT never Sets the Alloc-ID keeps its upstream. Use it only with an OLT that Sets the Alloc-ID of ME 262: one that does not Set it and reads the Alloc-ID back would see `0x00FF` and may treat the T-CONT as unusable. omcid logs a warning (`OMCI_TCONT_ALLOC_UNSET=unassigned`) for each T-CONT that carries GEM traffic without an Alloc-ID the OLT Set. Odi-only key, kept in `/etc/config/odi.conf` (see "Odi-only keys" below); read at every Get and rebuild, so a change needs neither a reload nor a re-registration | omcid, at every Get and MIB upload | LIVE (the next Get) |
| `OMCI_UNI_LOCK` | Honour an OLT lock of the Ethernet UNI | `1` honours it as the stock firmware does: when the OLT sets the PPTP Ethernet UNI administrative state to locked (ME 11 attribute 5), the host port's link is forced down until the OLT unlocks it. **On this SFP the UNI is the host port, the stick's only management path, so a lock cuts the web UI, ssh, syslog and metrics too** (OMCI still arrives over the fibre). Anything else, or absent, ignores it: the Set is answered and stored as always, and the port stays up. Default off, because an OLT that locks the port would otherwise take the stick away from the host that manages it, and the lock is the OLT's way to suspend a service (G.988 9.5.1), not something this stick needs to obey to work. Every lock and unlock is logged, once per change (`event=uni_lock`, docs/TOOLS.md): `action=ignored` by default, and with `1` an `action=applied` warning sent before the port goes down, so a host that collects the syslog sees why the stick went silent. Whatever the setting, omcid unlocks every Ethernet UNI when a MIB reset arrives (the reset puts the attribute back to unlocked, and the OLT then sends no unlock of its own) and when it starts with an empty MIB; it does not on a resume from a snapshot, whose MIB and switch state are still the old ones. To get back in after a lock with `1`: unplug the fibre and power-cycle the stick. Without the OLT it never reaches O5, so omcid starts with an empty MIB and unlocks the port; turn the setting off, then plug the fibre back in. Five short boots work too: the safe-mode boot keeps the PON off and removes `OMCI_UNI_LOCK=1` ("Recovery: the safe mode" below). Odi-only key, kept in `/etc/config/odi.conf`; read at every Set of the attribute, so a change needs neither a reload nor a re-registration | omcid, at every ME 11 Set, MIB reset and start; safe-mode.sh | LIVE (the next Set) |
| `HOST_AUTONEG` | Host port auto-negotiation | `off` runs the host port (the SFP edge, toward the router) as forced 1000BASE-X full duplex with no auto-negotiation, for a host whose SFP port has auto-negotiation off; `on`, empty or absent negotiates (clause 37), as the stock image and every host seen so far do; any other value runs `on` and logs `event=bad_value`. Never stored directly: a change is a trial, applied in RAM and reverted after 120 s unless confirmed over the link it changed; a boot with `off` falls back to auto-negotiation while the host link stays down; three short boots in a row make the next a safe-mode boot, which clears it ("Host port auto-negotiation" and "Recovery: the safe mode" below). Odi-only key, kept in `/etc/config/odi.conf` | `host-autoneg.sh`, at boot and for a trial | LIVE (a trial: kept only when confirmed) |
| `HOST_SERDES` | Host port SerDes mode | `2500basex` runs the host port as 2500BASE-X, for a host whose SFP port runs at 2.5 Gb/s, with the PCS auto-negotiation off and the port forced up; `hisgmii` runs HiSGMII, the MAC forced to 1000 Mb/s; `fiber1g`, empty or absent runs 1000BASE-X (Fiber 1G), the mode of the stock DFP-34X-2C2 image; any other value runs `fiber1g` and logs `event=bad_value`. Fiber 1G is the fallback everywhere, as the most compatible host mode. The stock firmware of the HSGQ XPON stick runs `2500basex` or `hisgmii`, and nothing on the board tells it apart; whether it links in Fiber 1G is untested. Never stored directly: a change is a trial, applied in RAM and reverted after 120 s unless confirmed over the link it changed; a boot in `2500basex` falls back to Fiber 1G while the SerDes does not sync, and stays there if neither links; three short boots in a row make the next a safe-mode boot, which runs Fiber 1G and clears it ("Host port SerDes mode" and "Recovery: the safe mode" below). Odi-only key, kept in `/etc/config/odi.conf` | `host-serdes.sh` (from `network.sh` at boot, and for a trial) | LIVE (a trial: kept only when confirmed) |
| `IGMP_SNOOPING` | IGMP snooping | `1` runs `igmpd`, which then snoops the host's IGMP while the OLT provisions a multicast profile that asks for it (ME 281 with an address row, ME 309 IGMP function 0): one static row a group, the host and the PON as members, and the group limit and access control lists of the profile enforced (the rows' VLAN and source address, previews and the allowed preview groups too, and messages above the profile's IGMP version are counted: `docs/TOOLS.md`, `igmpd`). When the profile asks for an upstream IGMP tag, a rate, or the discard of joins outside its lists, `igmpd` takes the host's IGMP from the switch and sends it up itself, tagged as asked (`docs/TOOLS.md`, `igmpd`). Multicast the PON sends is still flooded unless `IGMP_PON_DROP` asks for the drop. Empty, absent or anything else: no daemon, and multicast is flooded as it always was. Odi-only key, kept in `/etc/config/odi.conf`. Takes effect with `apply.sh igmp`, no reboot | `svc-igmpd.sh`; `docs/TOOLS.md`, `igmpd` | SERVICE RESTART |
| `IGMP_PON_DROP` | Drop unjoined multicast from the PON | `1` makes `igmpd` also drop IPv4 multicast from the PON that no group row names, one membership interval after snooping starts, so a group nobody on the host joined stays off the UNI link; `0` snoops without dropping anything. It is the one part of snooping that can cut a stream, hence its own key. Default off: unset or anything but `1` snoops without dropping. A config with `IGMP_SNOOPING=1` that wants the drop as before must now set `IGMP_PON_DROP=1`. With `MLD_SNOOPING=1` it drops unjoined IPv6 multicast as well, never the link-scope or solicited-node groups. Has no effect while `IGMP_SNOOPING` is not `1`. Odi-only key, kept in `/etc/config/odi.conf`. Takes effect with `apply.sh igmp`, no reboot | `svc-igmpd.sh` (`igmpd -d`); `docs/TOOLS.md`, `igmpd` | SERVICE RESTART |
| `MLD_SNOOPING` | MLD (IPv6) snooping | `1` makes `igmpd` snoop the host's MLD as it snoops its IGMP, on the same multicast profile: the switch copies (or takes) the host's MLD queries, reports and dones to the CPU and nothing else of ICMPv6, IPv6 groups get a row each (keyed on their 33:33 MAC, the host and the PON as members), the profile's IPv6 access control list rows and its group limit are enforced, and the reports go up tagged as the profile says. With `IGMP_PON_DROP=1` as well, IPv6 multicast from the PON that no group row names is dropped too, one membership interval after snooping starts; the link-scope groups ff02::/16 and the solicited-node groups always flood, both ways, so router advertisements, neighbour discovery and DHCPv6 are never pruned, and they are never snooped or refused. Empty, absent or anything else: MLD is not snooped and IPv6 multicast is flooded as it always was; `igmpd` runs exactly as without the key. Has no effect while `IGMP_SNOOPING` is not `1`. Odi-only key, kept in `/etc/config/odi.conf`. Takes effect with `apply.sh igmp`, no reboot | `svc-igmpd.sh` (`igmpd -6`); `docs/TOOLS.md`, `igmpd` | SERVICE RESTART |
| `GPON_DYING_GASP` | Dying_Gasp on a reboot (experimental) | `1` makes the stick send the GPON Dying_Gasp PLOAM message (G.984.3 clause 9.2.4.3, up to three copies, each given 50 ms to leave) just before an orderly restart, halt or power-off, so the OLT can tell a planned reboot or firmware update from a lost ONU. Sent only when the ONU is ranging or operating (O4 or O5), only from the reboot path and once per reboot; never when the PON drops. The kernel logs `Dying_Gasp sent <n> of 3 times before the restart`, which survives the reboot in `/proc/odi_ramlog_prev`. Empty, absent or anything else: not sent. Default off, because this project cannot see what the OLT does with it: on a reboot in O5 all three copies have been seen to leave (on a stick under test, and in another open implementation's measurements on the same chip), and G.984.3 tells the OLT not to stop allocations on a Dying_Gasp alone, but an OLT that gets one without a restart following may deactivate and re-activate the ONU. Odi-only key, kept in `/etc/config/odi.conf` | rcs-lib.sh, the `gpondg` PON step at boot; the driver's reboot notifier | REBOOT |
| `GPON_KEEP_DISABLE` | Stay disabled when the OLT disables this ONU | `1` keeps the ONU in Emergency Stop (O7) once the OLT has disabled it with Disable_Serial_Number (G.984.3), across this stick's own PON restarts and across reboots and power-offs, until the OLT enables it again, as the standard asks ("Keeping the OLT's disable" below). The disable is recorded in the marker file `/etc/config/gpon-hold`; a boot that finds it starts in O7 with the laser off and waits for the enable. Empty, absent or anything else: today's behaviour, the stick leaves O7 on any restart, and no marker is written or read. Default off, because a stick that stays dark when the OLT disabled it cannot be brought back from the fibre side except by the OLT itself, and an OLT that disabled it by mistake is not asked. Odi-only key, kept in `/etc/config/odi.conf` | rcs-lib.sh, the `gponkeep` and `gponhold` PON steps at boot; the driver; `gpon-hold.sh` | REBOOT |
| `SFP_HOST_DDM` | Diagnostics for the router (host-side SFP EEPROM) | the router reads the module's identity (A0h) and its live diagnostics (A2h: temperature, supply voltage, bias current, tx and rx power, the alarm and warning flags, and the TX_DISABLE, TX_FAULT and RX_LOS bits) from the stick, as from any SFP with digital diagnostics, on the board revisions that wire the SoC's own SFP EEPROM emulation to the SFP edge (HSGQ v08 / DFP-34X-2C2 v08, where the stock firmware does the same). `0` turns it off; `1`, empty or absent serve it (the default); any other value serves it and logs `event=bad_value`. On v06 boards the router reads a separate fixed EEPROM with no diagnostics whatever this says: the emulation is not wired there. Left off in a safe-mode boot. Odi-only key, kept in `/etc/config/odi.conf`. Takes effect with `apply.sh sfp`, no reboot ("Host-side diagnostics" below) | `sfp-host-ddm.sh`: a PON step at boot, and `apply.sh sfp`; the kernel does the rest (`odi_switch.sfp_mirror`) | LIVE |

Odi-only keys. `SYSLOG_SERVER`, `NTP_SERVER`, `ONU_HW_VERSION`,
`OMCI_UNKNOWN_ME_OK`, `OMCI_FIXED_ME`, `OMCI_PRIVATE_ME`, `OMCI_VEIP`,
`OMCI_POTS_PORTS`, `OMCI_POTS_SLOT_ID`, `OMCI_VOIP_LINE_REPORT`, `OLT_SW_DOWNLOAD`, `OMCI_ACCEPT_REBOOT`, `OMCI_UNI_LOCK`, `VLAN_EAPOL_UNTAGGED`, `OMCI_TCONT_ALLOC_UNSET`, `ODI_LASER_UNVERIFIED`,
`HOST_AUTONEG`, `HOST_SERDES`, `IGMP_SNOOPING`, `IGMP_PON_DROP`, `MLD_SNOOPING`, `GPON_DYING_GASP`, `GPON_KEEP_DISABLE`, `SFP_HOST_DDM` and `LAN_ROUTES` are the only
keys this image has that the stock firmware never had, so they are not in `lastgood.xml` and
`flash` (which edits keys already in the XML) could not save them. They live
in `/etc/config/odi.conf` instead, a plain `KEY=value` file on the jffs2
config partition, written by temp file and rename in the same directory.
`flash set` / `flash get` and `svc-syslogd.sh` / `svc-ntpd.sh` / `svc-igmpd.sh` (which read
through `flash get`) use it, and omcid reads `ONU_HW_VERSION` from it directly
at start (the first `KEY=value` line for the key wins, as with `flash get`), for exactly the names in `ODI_KEYS` at the top of
`rootfs/skeleton/etc/scripts/flash`; every other key still goes to
`lastgood*.xml`, so the stock image reads its own keys and ignores this file.
An empty value removes the key. `flash all cs` appends the odi keys that hold a
value as `<Value/>` lines so confd sees them. A line that is not `KEY=value` for
a listed key is ignored and kept. A factory reset (`flash default cs`) does not
clear this file. They must never be written into `lastgood*.xml`: measured on a
ISP1 stick (2026-09-28), the OEM image boots with an unknown key added to that
file and ignores it on load, but its first save rewrites the file from its
in-memory table and silently drops the key.

**The risk of `OMCI_UNKNOWN_ME_OK`.** The OLT is told a class was created or
set when nothing was: omcid keeps no row for it and programs nothing from it.
If the entity carried part of a service (a VLAN rule, a queue, a vendor
extension the OLT relies on), that part is silently missing while the OLT
believes it is in place -- the ONU reaches O5 and looks provisioned, and the
service may still not work. A Get answers success with no attributes, and a
MIB upload never lists the entity, so an OLT that audits the MIB against what
it wrote can find the difference and resynchronise, over and over. MIB data
sync counts each faked Create and Set, as the OLT does. Delete and Test of an
unknown class already answer success without the key. Turn it on only when
`logread | grep event=unknown_me` (or `/var/log/omcid-unknown.txt`) shows the
OLT stopping at such a class, and check the service afterwards. It does not
touch unknown message types (`event=unknown_msg`), which still answer "not
supported".
omcid reads `OLT_SW_DOWNLOAD` straight from `odi.conf` (it has no shell to run
`flash get` through): the first `OLT_SW_DOWNLOAD=` line wins, as it does for
`flash get`, and the file is read whole into a 4 KB buffer each time, a line
past that reading as absent.

Why `ELAN_MAC_ADDR` cannot be applied live, and what changed for `GPON_SN`:

- `ELAN_MAC_ADDR`: Linux refuses a new MAC on an interface that is up, so
  `network.sh` sets it on `eth0` and its host port with the links down,
  before `br0` exists. Doing that on a running stick takes down every way in
  in the middle of the change, with nothing to put it back if it fails.
- `GPON_SN` is no longer in this list: the kernel writes the serial into the
  PON MAC at the first activation of a boot (the `gpon_init` replay), and a
  `gponsn` verb after boot used to update only the driver's own copy, not what
  goes upstream. It now rewrites the PLOAM slot that replay armed, while the
  ONU is deactivated (`-EBUSY` in any other state), which is what a
  re-registration by omcid does. A new serial reaching the line has not been
  run on a stick ("Not verified yet"); a reboot applies it as before.

Details that bite:

- **`/etc/config/lan-ip`, when it exists, overrides `LAN_IP_ADDR`**, and the
  netmask then falls back to 255.255.255.0 whatever `LAN_SUBNET` says. With
  neither, the address is 192.168.1.1/24 (the stock default). The UI says the
  key is ignored while the file exists. `apply.sh network` follows the same
  precedence. An address or netmask the kernel would refuse (0.0.0.0, loopback,
  multicast, a mask with a gap, a leading zero, which `ifconfig` reads as octal)
  is treated as no address at boot: `network.sh` logs it and uses 192.168.1.1/24,
  and `apply.sh network` changes nothing and fails. So is the network or
  broadcast address of the mask (192.168.1.0 and 192.168.1.255 on a /24),
  which the kernel accepts and nothing can reach. The web UI refuses the same
  values (odi-ui types `ipv4host` and `netmask`). A stick on this fallback
  confirms the watchdog like any other, so it stays up on 192.168.1.1 for you
  to correct the address (a stick with no address stored at all does not, and
  resets every two minutes).
- **Changing the management addresses is a trial** ("Management address
  trial" below): the new address is applied in RAM and the stick goes back to
  the old one after 90 s unless the change is confirmed over the new address,
  so a typo cannot lock you out. Only a confirmed trial reaches the store.
  The old address stays up meanwhile.
- **The manual VLAN tag is gated the way the stock firmware gates it**
  (`/etc/runomci.sh`): applied only with `VLAN_CFG_TYPE` 1, `VLAN_MANU_MODE` 1,
  and both a VID and a priority present. With it absent or gated off,
  services carry no manual tag. Both lines run 1/1 (ISP1: VID 11, priority 0,
  read 2026-09-24; ISP2: VID 10 in the stored backup), so neither changes.
  `omcli vlan` prints whether the tag is applied.
- **Transparent is `VLAN_MANU_MODE` 0**, not a key of its own: "VLAN
  handling" below.
- **The OLD LOID wins.** When `LOID` and `LOID_OLD` differ, the OLD value is
  answered -- including an empty OLD beside a set LOID, which answers
  nothing. The password follows the LOID, as in the stock `runomci.sh`: with
  the same LOID in both keys the current `LOID_PASSWD` is answered (a changed
  password takes effect), with different LOIDs `LOID_PASSWD_OLD` is answered.
  That is the stock rule, kept so both slots agree. The UI writes `LOID_OLD` together with `LOID` (and the
  password pair), and shows the `_OLD` keys read-only. Untested: neither
  ISP1 nor ISP2 uses a LOID, and neither runs against a CTC OLT.
- **omcid looks for each key in both files**, the one xmlconfig assigns it
  to first. ISP1 keeps `GPON_PLOAM_PASSWD` and `LOID_PASSWD_OLD` in the CS
  file, and until 2026-09-24 omcid looked in HS only and reported both as
  missing (display only; rcS reads the right file, so ranging was fine).
- **The identity keys (`GPON_SN`, `GPON_PLOAM_PASSWD`, `ELAN_MAC_ADDR`,
  `GPON_ONU_MODEL`) need the UI's confirmation tick.** They cannot be
  regenerated: take a backup first.
- **A key cannot be cleared from the UI**: `flash set` refuses an empty value.

### VLAN handling: the stick tags, or the router does

A service's VLAN comes from one of two places, and `VLAN_MANU_MODE` picks
which:

| `VLAN_MANU_MODE` | name | what the stick does | the router |
|---|---|---|---|
| 1 (with `VLAN_CFG_TYPE` 1, a VID and a priority) | stick tags | adds `VLAN_MANU_TAG_VID`/`_PRI` to untagged frames upstream and removes it downstream (and from multicast); every other service as the OLT provisions it | sends the service untagged |
| 0 | router tags (transparent) | adds and removes no tag; every frame passes with the tags it has, both ways | tags each VLAN itself: internet, IPTV and voice on one port, as subinterfaces |

**Not in manual mode: the OLT's own rule.** With `VLAN_CFG_TYPE` other than
1 (0 is what a stock stick with no key reads), the stock firmware applies the
extended VLAN tagging operation (class 171) the OLT provisions, and so does
omcid. Two ways, tried in this order:

1. The shape that was there first: the instance associated with the Ethernet
   UNI (association type 2) has a rule for untagged frames that adds a single
   tag with a concrete VID (1 to 4094) and priority (0 to 7) at TPID 0x8100,
   and nothing else but default rules that drop or pass. It builds the same
   rule as the manual tag with that VID and priority: untagged frames get the
   tag upstream, the tag is removed downstream and from multicast, and the
   router sees untagged frames. Frames the router already tags with that VID
   still pass upstream unchanged when the OLT's VLAN filter (class 84) on the
   GEM side lists the VID, since the table leaves tagged frames on its default
   rule (G.988); the manual tag, by contrast, takes the place of that entry.
   With no VLAN filter on the GEM side, frames the router tags with any VLAN
   pass unchanged beside the added tag, unless the table's single-tag default
   rule discards them; a table without that rule passes them, as G.988's
   default does. On a line whose OLT adds VLAN 1 and passes the rest, the
   router's PPPoE on its own VLAN goes through this way. The manual tag keeps
   its one rule, so frames the router tags do not pass it.
   omcid logs `untagged frames get VLAN <vid> pri <p> from the OLT`.
2. Any other table: each entry becomes its own rule, with the rule that
   undoes it downstream as DsMode says. A single VID translated to another
   (C-VID 410 to 1410), one VID split into rows by priority, an S-tag pushed
   or popped, a tag removed or added, untagged frames of one EtherType (IPoE,
   PPPoE, ARP, IPv6, EAPOL) given their own tag, the C-tagged frames of one
   VID discarded: `docs/SWITCH.md`, "Extended VLAN rules", has every shape,
   what is done and what is refused. A discard is an ACL rule on the UNI
   ports; the priorities of its VID that another entry names are left out of
   it, and `omcli bridge` shows it as `rule ext-vlan discard vid <v> pri
   <p>`; an EtherType entry shows as `rule ext-vlan ethertype`, one a value
   (PPPoE two). The GEM port of a rule
   is the priority its result leaves with, through the 802.1p mapper (a rule
   that keeps the frame's priority on a mapper that splits them becomes one
   rule per priority); the class 84 list of the GEM-side port still decides
   which result VIDs are admitted. The single-tag default rule decides whether
   tagged frames the table does not name pass: a discard there means they do
   not. The table is applied whole or not at all: one entry the classifier
   cannot carry out leaves every frame as it comes, and omcid logs once
   `ext vlan 171/<instance> entry <n>: OLT rule not applied (<reason>),
   transparent`; that line is what to report.

A table with no entry of either kind (no class 171 at all, or only default
rules) stays transparent and omcid logs once `OLT rule not applied
(<reason>), transparent`. The rules follow class 171 as the OLT changes it
(Set, Create, Delete). `omcli bridge` lists them (`rule ext-vlan vid <v> pri
<p>`, the VID and priority the rule matches).

The manual tag is checked first and never yields: with `VLAN_CFG_TYPE` 1 the
table is not read, whatever `VLAN_MANU_MODE` says. That matters on ISP1,
whose UNI instance adds VLAN 1 while its service VID is 11, and on ISP2,
whose only instance sits on the VEIP with the "no VID" sentinel: both run
type 1, so neither changes. A stick of that shape moved to type 0 would tag
with VLAN 1, as the stock firmware does; to be transparent keep type 1 and
set `VLAN_MANU_MODE` to 0.

Transparent is the stock firmware's own meaning of mode 0 ("no manual
tag"), so both slots of a stick agree on it and no odi-only key is needed:
a second key would only raise the question of which of two wins. The VID
stays in `VLAN_MANU_TAG_VID`, so switching back is one value. `omcli vlan`
prints the mode in force on its `handling` line, and omcid's start-up line
says `manual vlan off (transparent)`.

What passes in transparent mode is still what the OLT provisioned. On a line
whose OLT sends class 84 VLAN filters, their forward operation (FwdOp, G.988
table 9.3.11-1, `docs/TOOLS.md`) decides: ISP1 sends FwdOp 0x10 with one VID
per GEM port (10 to 14), so the router's frames tagged with those VIDs pass,
each on its own GEM port, and untagged frames are discarded -- the OLT asked
for that. A line with no class 84 (ISP2) gets one forward-all rule per GEM
port: every tag, and untagged frames, both ways. The switch then carries
every VID 2..4094 on the UNI and the PON (`docs/SWITCH.md`, "CF rows").
With the manual tag on, a FwdOp that bridges untagged frames gets the
add-tag rule in place of a plain untagged one.

Some lines need a FwdOp other than the OLT's. One (odi-oss #301) filters
every GEM port with FwdOp 0x10 and the service VID while its 802.1X EAPOL
frames come down priority-tagged (VID 0): the filter drops every one and
the router never authenticates. `RTK_DEVINFO_SPECVER` overrides it per class
84 instance, with the same key and format as the stock firmware's "VLAN
Forward Operation" page, so a guide written for the stock firmware works
here and both slots agree: `0x1102:0x00,0x110b:0x00` bridges everything,
tagged and untagged, on those two ports. The instances are the ones
`omcli mib get 84` lists. The stock firmware writes the code over the
attribute; omcid keeps the OLT's value there, so a Get answers the OLT with
what it wrote and the UI shows it, and only the rules change. `omcli vlan`
lists the overrides in force, and each one is logged once
(`event=vlan_fwdop ... result=override`).

Measured under qemu for both shapes (`src/omci/vlan-test.sh`); on hardware,
not yet: PPPoE discovery on ISP2 and DHCP on ISP1 through router-tagged
subinterfaces are the checks to run.

### `VLAN_EAPOL_UNTAGGED`: 802.1X beside the service tag

Some ISPs authenticate the line with 802.1X (EAPOL, EtherType 0x888e) and
expect those frames untagged or with VLAN 0, while the internet service is on
a VLAN. A router with one untagged WAN interface sends everything untagged,
and "stick tags" would put the service VLAN on the EAPOL frames too. With
`VLAN_EAPOL_UNTAGGED=1` and the manual tag on, three rules go beside the
manual add-tag rule, on its GEM port:

| `omcli bridge` rule | frames | what happens |
|---|---|---|
| `eapol-up` | untagged EAPOL from the router | up the PON as they are, no tag |
| `eapol-down` | untagged EAPOL from the OLT | to the router as they are |
| `eapol-down-vid0` | priority-tagged frames from the OLT (VID 0, any priority) | to the router as they are, the VID 0 tag kept |

Everything else is as without the key: the router sends the service
untagged and the stick adds `VLAN_MANU_TAG_VID`/`_PRI`. The add-tag rule
names no VID, so the switch ranks its upstream row after every row that names
an EtherType (`docs/SWITCH.md`, "Extended VLAN rules"), and the EAPOL row
takes its frames first; nothing about the add-tag rule changes, which is why a
stick with the key off or absent has exactly the rules it had.

Setup (the key takes effect without a reboot):

```
/etc/scripts/flash set VLAN_EAPOL_UNTAGGED 1
/etc/scripts/apply.sh omci
```

omcid reads the key with the VLAN keys and holds it, so an edit of
`odi.conf` changes nothing until `apply.sh omci`, even when the OLT makes
omcid rebuild its rules in between; the reload then names the key
(`keys=VLAN_EAPOL_UNTAGGED`) and rebuilds.

On the router: one WAN interface, untagged (the stick adds the service VLAN),
with the supplicant listening on that same interface, untagged and for VLAN 0
(the stick delivers the OLT's EAPOL as it arrives, which on some lines is
priority-tagged VID 0). With the FwdOp override (`RTK_DEVINFO_SPECVER`) the
VID 0 frames pass as tagged frames already; this key does it without
opening every other tagged VID.

Limits, and what is not known:

- **Not yet run against a line that uses EAP.** It is tested as the bridge
  rules omcid builds and as the classifier rows they become, under qemu and
  on the host. Whether the OLT's EAP exchange then completes needs a line
  that authenticates.
- Upstream, the EtherType match is the one measured by another open
  implementation. Downstream (`eapol-down`) uses the same row with the
  direction bit set. The same implementation measured a downstream row of
  that kind on its own driver (untagged frames of one EtherType took the row,
  those of another did not; `docs/SWITCH.md`, the EtherType slot), but this
  image's row has not been run on a stick. If the switch did not compare the
  EtherType downstream here, it would go one of two ways, and nothing on the
  host tells which: the row matches nothing, and EAPOL from the OLT falls to
  the rows that follow (dropped, as without the key); or the row ignores the
  EtherType, and `eapol-down` sends the router **every untagged frame** the
  OLT sends, of any EtherType and on any GEM port, none of which a rule omcid
  builds passes with the key off.
- A downstream row cannot name the GEM port a frame came on (`docs/SWITCH.md`,
  "a row cannot be told apart by GEM port"). So `eapol-down` takes untagged
  EAPOL from every GEM port of the line, and `eapol-down-vid0` every VID 0
  frame from every GEM port, the multicast one included, apart from what a
  row ahead of it takes.
- `eapol-down-vid0` names no EtherType, because the classifier is not known to
  compare it behind a tag: it passes every priority-tagged frame with VID 0
  that the OLT sends, not only EAPOL. On a line with multicast under the manual
  tag, VID 0 priority 0 is the multicast strip row (it goes first).
- With `VLAN_MANU_TAG_VID` 0 there is no `eapol-down-vid0`: the add-tag rule's
  own downstream half takes the VID 0 frames of `VLAN_MANU_TAG_PRI` and strips
  the tag (EAPOL included), and a VID 0 rule would also pass the service's
  frames of every other priority with their tag, which no rule omcid builds
  passes with the key off. VID 0 EAPOL of another priority is then not delivered.
- When the OLT gives the add-tag rule to more than one GEM port (several
  GEM-side bridge ports with no VLAN filter, or with a forward operation that
  bridges untagged frames), the untagged frames go up the GEM port whose rule
  went in last, and the three EAPOL rules go on that GEM port, once.
- The key needs the manual tag (`VLAN_CFG_TYPE` 1, `VLAN_MANU_MODE` 1, a VID
  and a priority). Without it, and with a tag from the OLT's own table
  (type other than 1), it has no effect, and omcid logs
  `VLAN_EAPOL_UNTAGGED has no effect without the manual tag` once.
  `omcli vlan` shows the key when it is set.
- Off by default: some ISPs expect EAP on the service VLAN.

### The OLT identity keys, and their switch

`OMCI_SW_VER1`, `OMCI_SW_VER2`, `GPON_ONU_MODEL`, `OMCC_VER`,
`OMCI_VENDOR_PRODUCT_CODE` and `ONU_HW_VERSION` are reported to the OLT **only while
`/etc/config/omci-identity.on` exists** (the UI's "OLT identity" switch on the
Config tab). Off, or with a key empty or absent, omcid answers what it always
has:

| entity, attribute | switch off, or key empty | switch on, key set |
|---|---|---|
| software image 0 and 1, Version | `0.0.0` | `OMCI_SW_VER1`, `OMCI_SW_VER2` |
| ONU2-G, Equipment id | the device id (`RTL9602C`) | `GPON_ONU_MODEL` |
| ONU2-G, OMCC version | 0x80 | `OMCC_VER` |
| ONU2-G, Vendor product code | 15, the stock firmware's default for the key | `OMCI_VENDOR_PRODUCT_CODE` |
| ONU-G, Version (hardware version) | the device id (`RTL9602C`) | `ONU_HW_VERSION` |

`ONU_HW_VERSION` is odi-only (`/etc/config/odi.conf`, set with `flash set
ONU_HW_VERSION <value>` or from the UI), so unlike the other five neither
stick carries it until someone sets it. A value longer than the attribute (14
characters) or with anything outside printable ASCII is ignored whole, not
cut: a truncated version is a different version to an OLT that whitelists it,
so the device id is answered instead and `omcli ident` says why. The stock
store keeps a key of its own, `HW_HWVER` (lastgood_hs.xml; on our sticks one
of `RTL960x`, `TCG2232` or a 14-character vendor string, depending on which
firmware wrote the store), which this image does not read: nothing on file
shows which ONU-G value the stock firmware derives from it, and a value an ISP
whitelists is better set on purpose than inherited. Copying it by hand is
`flash set ONU_HW_VERSION "$(flash get HW_HWVER | cut -d= -f2)"`.

The switch exists because the plain rule -- an empty key keeps today's value
-- does not keep today's value on our sticks: **both already store all five,
written by the stock firmware**. ISP1 (read 2026-09-24): `OMCI_SW_VER1` and
`OMCI_SW_VER2` `V1.0-220923`, `OMCI_VENDOR_PRODUCT_CODE` 15, `OMCC_VER` 128,
`GPON_ONU_MODEL` `IGD`; ISP2's backup holds the same model, product code and
OMCC version and other version strings. Honouring them unconditionally would
change the software version both OLTs see from `0.0.0` to a stock string and
the equipment id from `RTL9602C` to `IGD`. With the switch off, nothing
changes. The stock firmware may also rewrite `OMCI_SW_VER1/2` itself when it
boots, so a value set here does not necessarily survive a fall-back to the
stock slot. `omcli ident` shows what is stored and whether it is reported.

### Keeping the OLT's disable (`GPON_KEEP_DISABLE`)

An OLT disables an ONU with the Disable_Serial_Number PLOAM message: to stop
a rogue ONU, or one an operator has barred. The ONU goes to Emergency Stop
(O7), shuts its laser off and ignores everything but the OLT's enable.
G.984.3 (Table 10-1 and the DIS alarm) says it stays there even after
power-off, until the OLT enables it with Disable_Serial_Number and the enable
code, or with the broadcast enable. Without this key the stick keeps O7 only
until its own next PON restart or reboot, and then ranges again.

With `GPON_KEEP_DISABLE=1`:

- When the OLT disables the ONU, the driver notes it, and the kernel runs
  `/etc/scripts/gpon-hold.sh sync` once, which writes the marker file
  `/etc/config/gpon-hold` (a flag file next to `optics.off` and `modules.off`:
  what the OLT did, not a setting, so it is not in `odi.conf`). The marker is
  on the jffs2 config partition, which a reboot, a power-off and a firmware
  update keep; the script flushes it at once, so only a power cut inside that
  moment loses it.
- While the disable is held, this stick's own restarts do not undo it:
  `gpondeact` and `gponact` (the omcid respawn re-provisioning, `apply.sh`
  changing the serial number or the password, a reload omcid makes on SIGHUP,
  a write to `/proc/odi_init` by hand) end in O7, the laser line still high.
  Changing the serial number does not clear it.
- A boot with the key set and the marker present starts in O7: the `gponhold 1`
  PON step raises the laser line before the MAC is brought up, the activation
  ends in O7, and nothing answers on the fibre. The OLT's enable moves it to
  O2 and ranging goes on as usual; the driver then asks `gpon-hold.sh` to
  remove the marker.
- In O7 the laser line is high, so nothing leaves the stick on the fibre; the
  feature opens no service to the PON side, and OMCI stays the only way the
  OLT talks to the stick.
- A safe-mode boot ignores the key and the marker and leaves both as they are
  (it does not activate the PON in any case), so a stick that was disabled
  can always be reached from the host side.

To undo it by hand, from the host side (ssh, the web UI's shell, or a
safe-mode boot):

    /etc/scripts/gpon-hold.sh status     key=, marker=, held= (what the driver says)
    /etc/scripts/gpon-hold.sh clear      remove the marker and release the hold now

`clear` is what the OLT's enable does without the OLT: a stick in O7 is
activated and ranges again. Whether the OLT then lets it in is its decision;
if it disabled the ONU on purpose it will disable it again. To turn the
feature off, `flash set GPON_KEEP_DISABLE 0` (the next boot ignores the marker;
the file stays until `clear`, and counts again if the key is set again).

`/proc/odi_gpon/status` has the line `o7_hold keep <0|1> held <0|1>`: `keep 1`
is the key as the driver was told at boot, `held 1` is a kept disable in force.
`state 7 (O7)` alone does not say which: without the key, O7 is also reached
by an OLT disable and left by any restart. The kernel log has
`odi_gpon: event=o7_hold action=<recorded|kept|released|cleared> cause=<...>`
and `gpon-hold: event=marker action=<written|removed|failed>`.

**Testing it without an OLT.** On a stick with the key set,
`echo "gponhold sim" > /proc/odi_init` acts as if the OLT had sent the stick
Disable_Serial_Number for its own serial number: O7, laser line high, the
hold recorded and the marker written. It is a test hook, answered only when
the ONU is between O2 and O6, and it sends nothing upstream. Then
`gpondeact` and `gponact` in `/proc/odi_init` still end in O7, a reboot starts
in O7, and `gpon-hold.sh clear` ends it. It refuses (`-EPERM`) while the key
is off.

### Host-side diagnostics (`SFP_HOST_DDM`)

A router reads an SFP module's identity at I2C address 0x50 (A0h) and, when
A0h byte 92 says the module has digital diagnostics, the live readings at
0x51 (A2h, SFF-8472). On this stick the host pins do not reach the optics
module's own diagnostics. The RTL9602C has an SFP EEPROM emulation: an I2C slave on
the SFP edge pins answering from a 512-byte SRAM. On the board revisions that
wire it to the edge (HSGQ v08 / DFP-34X-2C2 v08) the stock firmware turns it
on and keeps it filled, and the router shows the module's temperature,
voltage, bias and optical powers; with it off, as earlier builds of this
image left it, the router shows 0 C and 0 V (measurements by Thaad-QR on a
stock v08 stick, from the router's own EEPROM dump). On v06 boards the host
pins go to a separate fixed EEPROM with no diagnostics, and turning the
slave on there changes nothing the host reads.

With `SFP_HOST_DDM` on (the default), the boot, once the optics I2C answers
(after the `laser` PON step), asks the kernel for the mirror. The kernel
copies the module's A0h and A2h (the upper half from page 0) into the SRAM,
reads all of it back, makes every byte read-only to the host except the user
EEPROM area (A2h 128-247), and only then turns the slave on and gives it SoC
pin 27 (out of plain GPIO mode, as the stock v08 image runs it: on v08 the
router saw nothing until that pin was given, Thaad-QR's comparison with a
stock v08 stick; on v06 nothing measured uses it); it never writes the
module. From then on it rewrites A2h 96-119 once a second from the
module: the five diagnostic words, the status/control byte 110 (bits 7, 2
and 1: TX_DISABLE, TX_FAULT and RX_LOS as the module reports them) and the
alarm and warning flags. A module read that fails leaves the last values and
is logged once. Writes from the host are refused except in A2h 128-247, and
nothing acts on those: the host cannot switch the laser off through byte
110. For that reason A0h byte 93 keeps only what is served (alarm and
warning flags, soft TX_FAULT and RX_LOS monitoring) and drops any soft
control the module advertises, with its checksum moved to match; a module
that advertises none, as seen so far, reaches the host unchanged.

Check it with `cat /sys/module/odi_switch/parameters/sfp_mirror_state`
(`on`, `off`, `filling` or `failed`) and `dmesg | grep odi_sfp_mirror`;
`sfp-host-ddm.sh status` shows the key too. `SFP_HOST_DDM=0` and `apply.sh
sfp` turn the slave off at once and give pin 27 back; the router then reads
what the board gives it with the emulation off.

### Software download from the OLT

An OLT can push a firmware image to the ONU over OMCI (G.988: Start software
download, Download section, End software download, Activate image, Commit
image, on the software image entity). This image never runs an ISP firmware,
and omcid never installs one. What `OLT_SW_DOWNLOAD` chooses is only what the
OLT is told:

| step | `accept` (default) | `reject` |
|---|---|---|
| Start software download | success, with the window size the OLT asked for; refused (parameter error) for the image that is active or committed, as G.988 and the stock stack do; the image then reads not valid | not supported |
| Download section | the last section of each window is acknowledged; a window with a section missing is a processing error, so the OLT sends it again; every byte goes through the CRC and is dropped | not supported |
| End software download | success when the CRC-32 the OLT sends matches the image and the size matches Start; a processing error otherwise; the image then reads valid | not supported |
| Activate image | success; that image now reads active, the other not. **No reboot** | not supported |
| Commit image | success; that image now reads committed, the other not | not supported |

The flags are what omcid **reports** in the software image entity
(`is_committed`, `is_active`, `is_valid`), for the OLT to read back. The real
slots, `sw_commit` and `sw_tryactive` are never touched: `make test-omci` runs
a whole download under `qemu -strace` and asserts no open of `/dev/mtd`, no
exec (so no `nv`) and no reboot. The flags hold for the rest of the boot,
across an omcid respawn (in `/var/run/omcid-swimage`, RAM), and a reboot comes
back reporting image 0 committed and active again. The version each image
reports stays `OMCI_SW_VER1`/`OMCI_SW_VER2` (with the OLT identity switch on)
or `0.0.0`: no OMCI download message carries a version string -- it is only
inside the ISP image, which is discarded unread.

What the OLT does next is its own business, and untested on any line: an OLT
that expects the ONU to reboot into the new image after Activate, and to
report the new version, may time out waiting, or repeat the download at every
boot. Every step is one `event=sw_image` line (`docs/TOOLS.md`, "Link and
provisioning events"), so the log shows it if it does; `reject` goes back to
the old answers.

### Host port auto-negotiation

The host port, the SFP edge the router plugs into, runs 1000BASE-X with the
clause-37 auto-negotiation, as the stock image does. A router whose SFP port
has auto-negotiation off sees no link from it; `HOST_AUTONEG=off` turns the
negotiation off on the stick too, forced 1000 Mb/s full duplex
(`docs/SWITCH.md`, "Host port auto-negotiation"). Leave it unset unless the
router needs it: a mode the two ends disagree on takes the only way into
the stick down with the internet, so everything below exists to get it
back. It is the auto-negotiation of Fiber 1G: while `HOST_SERDES` runs
another mode, that mode sets the bit itself, and `HOST_AUTONEG` is neither
applied at boot (`event=serdes_mode` when it is off) nor open to a trial
("Host port SerDes mode" below).

**A change is a trial.** The web UI (Config, Network, "Host port") or
`host-autoneg.sh try off` (`docs/TOOLS.md`) applies the mode one second
later, in RAM only, and starts a 120 s timer. Within that window, confirm
it from the same page, or with `host-autoneg.sh confirm` over ssh: only
then is `HOST_AUTONEG` stored. The confirm can only arrive over the host
link, so a mode that breaks the link cannot be confirmed, and at the end of
the window the stick goes back to the mode it ran before, by itself, and
logs `event=trial_expired`. A reboot during the trial also undoes it: the
key was never stored. Going back to auto-negotiation is a trial the same
way (confirming it removes the key). A trial is refused while another runs
and while the boot watch (below) is still deciding.

**The boot watch, and the fallback.** A boot with `HOST_AUTONEG=off` forces
the mode right after `network.sh` and watches the host link for 30 s. With
no link it switches to auto-negotiation for 30 s, then back to forced, and
so on, until a link comes up in either mode or 10 minutes have passed (then
it stays on auto-negotiation, the default, for the rest of the boot: a
forced router that comes up later than that finds no link until the stick
reboots). The alternation is for a router
that boots slower than the stick after a power cut: a forced router that
comes up late still finds the stick forced within 30 s. The key is never
changed by the watch. When the link comes up only with auto-negotiation,
that is the **fallback**: `event=fallback`, the state file says
`fallback=1`, and the web UI shows a banner saying the stick runs
auto-negotiation although `HOST_AUTONEG` is off. Check the router port
(auto-negotiation off, 1000 Mb/s full duplex) and try off again, or set the
key back to on. The fallback clears by itself: it is the state of one boot,
in `/var/run`, and the next boot that brings the forced link up has none.
30 s is long against a forced link (it comes up within a second or two of
both ends being up) and short against the 75 to 80 s a reboot already costs;
each phase is the time a wrong mode keeps the stick unreachable.

**The safe mode.** Three short power cycles in a row with `HOST_AUTONEG=off`
(each: plug in, wait about 80 s, pull) make the next boot a safe-mode boot, which clears the key and runs
auto-negotiation ("Recovery: the safe mode" below). The state of the
running boot is `/var/run/host-autoneg`, `key=value` lines replaced by a
rename (`docs/TOOLS.md`, "host-autoneg.sh"); the web UI and the diagnostics
bundle (`host-link.txt`) read it.

Verified on ISP1's stick: the register write (the PCS control word
0x1140 becomes 0x0140, and the port keeps reporting the link of a forced
PCS); a host with auto-negotiation on (an Intel 82599 and a router SFP+
port) links with the stick forced off and carries traffic, as those hosts
accept a forced partner; a host forced off (`ethtool -s <if> autoneg off
speed 1000 duplex full` on the router SFP+ port) gets a link but no traffic
from a stick with auto-negotiation on, and traffic once the stick is forced
off too, which is the case the setting is for. The trial behaved as
described above: `try off` with no answer reverted by itself
(`event=trial_expired`, key unchanged), `try off` and `confirm` over the
same link stored the key, `try on` and `confirm` removed it, and a boot with
the key off against a forced host logged `event=forced_link` at about 10 s of
uptime. What no stick has shown is the boot fallback: both hosts accept a
forced partner, so the forced side never lacked a link ("Not verified
yet").

### Host port SerDes mode

The host port's SerDes runs Fiber 1G (1000BASE-X) by default: the mode the
stock image of the DFP-34X-2C2 runs, and the one every host of that stick
speaks. `HOST_SERDES` picks another:

- `2500basex`: 2500BASE-X, for a host whose SFP port runs at 2.5 Gb/s. The
  PCS auto-negotiation is off and the switch port forced (link up, full
  duplex) in this mode, whatever `HOST_AUTONEG` says. Against the 2.5G
  router SFP port it was measured on, the link came up only with the
  auto-negotiation off, although a stick running its stock HSGQ firmware
  links to the same port with it on; why is not known. Another open
  implementation's measurements linked 2500BASE-X with the
  auto-negotiation on and off against a matching host, re-latching 10 to
  14 s after a host change (inside the 30 s phase of the boot watch,
  below). So auto-negotiation off is what that router needs, not a
  property of the lane, and it is a setting that works there, not an
  explanation.
- `hisgmii`: HiSGMII, the MAC forced to 1000 Mb/s, the 1000BASE-X PCS
  powered down and the port forced. This is the mode the stock firmware
  calls `LAN_SDS_MODE` 4 ("2500Mbps HiSGMII PHY"); it is not a 1G mode on
  the wire. Tested by its reporter against a media converter. On a Luleey
  LL-XS2510 against a 2.5G router port (a UniFi gateway whose SFP+ port a
  third-party tweak runs at 2.5G) it synced but passed no frames, where
  `2500basex` linked.

**What the host reads from the module.** The SFP's own EEPROM page the
host reads (A0h, fixed memory) advertises the module as 1000BASE-LX at a
nominal 1300 MBd, and this image cannot change it. A host that picks its
port mode from that page (Linux phylink and its SFP driver, for example)
may run its port at 1000BASE-X whatever `HOST_SERDES` says; a host whose
port mode is set by hand (a router SFP port forced to 2.5G, as in the
measurement above) is not affected. This is a caution, not a measured
failure: no Linux host has been tried against `2500basex`.

The HSGQ XPON stick is the same board as far as this image can see (the
same U-Boot, environment and flash layout), and its stock firmware runs
one of these two. Nothing on the board says which stick it is, so the key
is the only input, and the stock `LAN_SDS_MODE` is not read: a DFP-34X-2C2
can hold 3 there and run Fiber 1G. Whether the HSGQ board links in Fiber
1G is untested (the one run without the key changed two things at once,
against an unknown host), and its reporter has been asked. Until it is
known, set the key on an HSGQ stick before the first boot of this image:
from the stock firmware's shell, `echo HOST_SERDES=2500basex >>
/var/config/odi.conf`, then flash and trial the image as usual
(`docs/FLASHING.md`). A trial boot that does not come up returns to the
stock slot by itself.

**Fiber 1G is the fallback everywhere.** 1000BASE-X is the most compatible
host mode, so every way back below ends in it: the trial revert from a
Fiber 1G boot, the boot watch of 2500basex when nothing links, and the
safe mode, which also clears the key.

The register sequence of each mode and the port force are in
`docs/SWITCH.md`, "Host port SerDes mode". `network.sh` runs
`host-serdes.sh boot` before the MAC and the bridge, as the Fiber 1G
sequence always ran there; Fiber 1G is the sequence it always was, value
for value, and a stick already in it is not written at all.

**A change is a trial**, as with `HOST_AUTONEG`: `host-serdes.sh try
2500basex` (`docs/TOOLS.md`) answers at once, applies the mode one second
later, in RAM only, and reverts by itself after 120 s unless `host-serdes.sh
confirm` arrives first, which can only happen over the link in the new
mode. Only the confirm stores `HOST_SERDES` (confirming `fiber1g` removes
it). A reboot during the trial also undoes it. A trial is refused while
another one runs, while the boot watch (below) runs, while a
`host-autoneg.sh` trial or boot watch runs, in a safe-mode boot, and with
`/etc/config/sds.off`. The web UI does not offer the setting yet.
`host-serdes.sh reapply` runs the sequence of the running mode again, live,
with no trial and nothing stored: the way to check on a stick that the
sequence itself brings a link back.

**What protects each mode.** The kernel's `host_link` is the switch port's
link, and a forced port reads it as up whatever the host does, so in
`2500basex` and `hisgmii` it says nothing about the host. `2500basex` has
another signal: bit 17 of `HOST_SERDES_MODE` (0x214) is set while the SerDes
is in sync with the host (0x00260016 on a stick that links, 0x00240016 on
one that does not). `hisgmii` has none known. omcid reads `host_link` too,
for the Ethernet UNI the OLT sees (ME 11), so in the two forced modes the
OLT sees the UNI up at 1000 Mb/s full duplex whatever the host does; an OLT
lock of the UNI still takes the link down (`docs/SWITCH.md`). So the
protections are not the same in every mode, and not the same as
`HOST_AUTONEG`'s:

| mode | trial revert | boot watch and fallback | safe mode |
|---|---|---|---|
| `fiber1g` | yes | `HOST_AUTONEG`'s, when that is off (above) | the mode of every safe-mode boot |
| `2500basex` | yes | yes: the SerDes sync, falling back to Fiber 1G and ending there (below) | counts after three short boots; that boot runs Fiber 1G and clears the key |
| `hisgmii` | yes | none: nothing tells a dead host from a live one | as `2500basex` |

**The boot watch of 2500basex.** A boot with `HOST_SERDES=2500basex`
applies it and watches the SerDes sync for 30 s. With no sync it switches
to Fiber 1G for 30 s, watching `host_link` there (the port is no longer
forced), then back to 2500BASE-X, and so on until one of them links, for at
most 10 minutes. A link in Fiber 1G is the **fallback**: `event=fallback`,
`fallback=1` in the state file, the key unchanged. Past the 10 minutes with
no link in either mode it stays on Fiber 1G (`event=fallback_nolink`,
`fallback=1`), as `HOST_AUTONEG` ends on its default; the key is not
changed by the watch, so the next boot tries 2500BASE-X again. A 2.5G host
that comes up later than that finds the stick in Fiber 1G until it
reboots. In Fiber 1G during and after the watch the auto-negotiation is
on.

**The auto-negotiation.** `host-serdes.sh` sets the PCS auto-negotiation
bit itself in the two other modes (off in `2500basex`, on in `hisgmii`,
where the PCS is powered down anyway), through `odi_switch.host_autoneg`,
in RAM. `HOST_AUTONEG` keeps its stored value and applies again once the
port runs Fiber 1G at a boot; while another mode runs, `host-autoneg.sh`
applies nothing, starts no boot watch of its own (its on/off alternation
would put 2500BASE-X into the auto-negotiation that does not link), and
refuses a trial; its `status` then says `owner=host-serdes`, with
`running=` the bit the port really runs and `setting=` the stored
`HOST_AUTONEG`. Back in Fiber 1G during a boot (a trial reverted, the
fallback), the bit goes back to what `host-autoneg.sh` last ran.

The state of the running boot is `/var/run/host-serdes`, `key=value` lines
replaced by a rename (`docs/TOOLS.md`, "host-serdes.sh"); the diagnostics
bundle (`host-link.txt`) carries its `status`.

### Recovery: the safe mode

A way back into a stick that cannot be reached or logged into -- a host
port mode the router does not speak, a forgotten web UI password, lost ssh
keys -- with nothing but its power:

**Plug the stick in, wait about 80 seconds, pull it, wait a few seconds.
Do that five times in a row (three times when `HOST_AUTONEG` is off or
`HOST_SERDES` is `2500basex` or `hisgmii`). Then
plug it in and leave it in: that boot is the safe-mode boot. Do not pull it
again.**

The window is roughly 60 to 110 seconds after power-on, and 80 s sits in
the middle of it. Every boot adds one to a counter on the config partition
right after the partition is mounted, before the network comes up, and
logs the uptime of that moment (`event=boot_count ... uptime=`); a boot
that reaches 60 s of uptime clears it. A boot that finds five short boots
before it (three with `HOST_AUTONEG=off` or a `HOST_SERDES` mode other than
`fiber1g`) is a safe-mode boot. Uptime
starts when the kernel does, not at power-on: measured on ISP1's stick, a
cold power-on spends about 54 s in U-Boot and loading the image, the count
lands at about 5 s of uptime (about 59 s after power-on) and the clear at
60 s of uptime (about 114 s). So a power-up counts only if it lasts past
about 60 s and ends before about 110 s. The time before the kernel can
vary by board and image; the logged uptime of the count, plus that board's
own boot time, gives its window. A watchdog reset or a crash that reboots
the stick before 60 s of uptime counts as a short boot too: a stick that
cannot stay up ends in the safe mode by itself, which is the point.

Two more things make a boot the safe one without anyone touching the power
(the state machine is below): three boots in a row that each ended in a
watchdog reset, and a config partition that is not mounted.

A safe-mode boot lasts that one boot. It:

- runs the host port in Fiber 1G (1000BASE-X, the most compatible host
  mode) with auto-negotiation, whatever `HOST_SERDES` and `HOST_AUTONEG`
  say;
- leaves the PON off: the PON steps keep the laser off and skip the
  activation, so there is no internet through the stick and it cannot
  disturb the OLT or the other ONUs on it while someone works on it;
- accepts the built-in default password: `admin` / `admin` on the web UI,
  and `admin` for root over ssh (password logins are offered for that boot
  even on a keys-only image), on top of the ssh keys already installed;
- writes nothing to the config partition but its record,
  `/etc/config/safe-mode`, and, when `HOST_AUTONEG=off` or a `HOST_SERDES`
  mode other than `fiber1g` lowered the count to three, the removal of
  those keys, so the boots after it stay in Fiber 1G with
  auto-negotiation too. The saved web UI credential, the ssh keys
  and every other setting are left as they are, and the next boot uses
  them again. The default passwords are in memory only: the web UI reads a
  flag in `/var/run`, and root's account file is a copy in RAM bind-mounted
  over the read-only one;
- confirms the boot to the kernel watchdog without the checks that a dead
  config partition or a missing address fail (`confirm_watchdog` in
  `rcs-lib.sh`; the switch init must still have worked), so a safe-mode boot
  stays up for someone to reach instead of being reset at 123 s again;
- does not register `omcid` with the kernel watchdog (rcS), so it stays up
  whatever state omcid is in: with the PON off omcid has no work, and an
  omcid that dies at every start (a value in the config store, which both
  slots share) would otherwise reset this boot after 60 s like the three
  before it. `services start omcid` does not arm the deadline in it either;
- starts the web UI and ssh whatever `/etc/config/confd.off` and
  `/etc/config/dropbear.off` say, and leaves both files where they are, so
  the next normal boot is as configured. Both stopped at once would
  otherwise leave the rescue address with nothing listening on it, and a
  reflash does not remove the files;
- runs ssh on a host key in RAM (`/var/run`) when there is none and none
  can be written (the config partition full, read-only or not mounted). The
  ssh client then meets a host key it does not know.

The web UI says so on every view while it lasts: that this is the safe
mode, that the PON is off, that the password is the default for this boot
only, and how to leave: set a new web UI password (System, Access) or add
an ssh key if that is what was lost -- both are saved and in force from the
next boot -- acknowledge, and reboot. The record outlives the boot, so the
banner says a safe-mode boot happened until someone acknowledges it (its
button, or `safe-mode.sh ack`).

**The SerDes mode.** Fiber 1G is the fallback here as everywhere: a
safe-mode boot runs it, and after short boots it removes a `HOST_SERDES`
mode other than `fiber1g`, as it removes `HOST_AUTONEG=off` (`cleared=`
names what it removed: `HOST_AUTONEG`, `HOST_SERDES`, `OMCI_UNI_LOCK`, a
comma-separated list of those, `none`, or `failed`). A safe mode after
watchdog resets removes none of them. Such a mode
also lowers the count to three short boots, as `HOST_AUTONEG=off` does.
Whether the HSGQ board links in Fiber 1G is untested; if it does not, a
safe-mode boot there is unreachable, and the key has to be set again from
outside (the stock slot), as on its first boot.

**An OLT lock of the host port.** With `OMCI_UNI_LOCK=1`, an OLT that
locks the Ethernet UNI takes the host link down. A boot always starts
unlocked, but with the fibre in, the OLT can lock it again as soon as it
provisions the stick. The simple way back is a boot with the fibre out
(the setting can then be turned off before plugging it back in); five
short boots also work, without touching the fibre: the safe-mode boot
keeps the PON off and removes `OMCI_UNI_LOCK=1` (`cleared=OMCI_UNI_LOCK`),
so the boots after it answer the lock without obeying it. The limit stays
five. Without the key (the default) no lock takes the port down, and there
is nothing to recover.

**The trade-off.** Anyone who can power-cycle the stick five times can log
into it for one boot, with the password everybody knows, and read or change
its settings, the line credentials included. Physical possession already
goes a long way (the flash can be read with a programmer), and this is what
makes a lost password recoverable at all; but it is a real change for a stick
whose host port is reachable by people who should not log into it. The PON
is off for that boot, so the stick does not carry anyone's traffic
meanwhile. The counters cost two small writes to the config partition per
boot (the count, and its removal at 60 s), on every stick; the watchdog one
is written only when it changes, which is after a watchdog reset.

**The state machine.** Three counters decide, all read in `safe-mode.sh
count`, once per boot, right after the config partition is mounted:

| signal | where it lives | how it moves |
|---|---|---|
| short boots | `/etc/config/boot-count` | +1 at every boot (about 5 s of uptime), removed at 60 s |
| watchdog resets | `/etc/config/boot-wdt` | +1 when the previous boot's `reason=` (`/proc/odi_ramlog_prev`, `docs/KERNEL.md`) is `wdt_*` (a watchdog rule fired: boot never confirmed, a client missed its ping, memory floor, CPU port), `panic` or `oops`; unchanged on `power` (DRAM lost: a power cycle) or when the file is absent (an older kernel); removed on anything else (`reboot`, `halt`, `poweroff`, `unknown`); removed at 600 s of uptime |
| config partition | the mount table (`config_mounted`), and `/etc/config` writable | checked first, by rcS and again by `count` |

and one of them ends in the safe mode:

1. config partition not mounted or not writable: safe mode at once
   (`safe-mode.sh no-config`, `cause=config`), with no record, since there
   is nowhere to write it. The flag in `/var/run` still says it, and the
   web UI reads the flag.
2. `boot-count` reached 5 short boots before this one (3 with
   `HOST_AUTONEG=off` or `HOST_SERDES` `2500basex` or `hisgmii`):
   `cause=short_boots`.
3. `boot-wdt` reached 3 boots before this one: `cause=watchdog`. The
   safe-mode record says `wdt_resets=3`.

Entering the safe mode removes both counters. The boot after a safe-mode
boot is a normal one and finds a `reason=` of `reboot` (or `power`), so it
starts from zero: the safe mode is never two boots in a row, it takes the
whole count again, and so it cannot loop. What a safe-mode boot does is stay
up: it confirms the watchdog (above), so a stick whose fault is a dead
config partition or a userland that cannot configure itself is reachable
until someone reboots it, with the PON off. If the fault persists and the
person reboots anyway, the cycle is three more resets (about ten minutes:
54 s of U-Boot and 123 s of unconfirmed userland each), then another
safe-mode boot that stays up. A safe-mode boot that the watchdog resets
anyway (the switch init failed, so nothing is reachable) is not a safe mode
that helped; it counts as the first of the next three.

Why three watchdog resets. One legitimate reset (an omcid that hung and was
caught, a memory floor) must not take the PON down, and two close together
are not unheard of; a loop that strands a stick repeats every two to three
minutes, so three fire in about ten. A boot that lives 10 minutes ends the
count, so a reset every few hours (a slow leak, a flaky OLT) never adds up
and never turns the PON off. `unknown` clears the count rather than adding
to it: it is also what the first boot after the stock image reports, and an
unexplained reason is a poor ground for turning the PON off.

The dead-config rule has a price: with the partition unreadable, the
default password is in force for that boot without anyone touching the
power, for as long as the stick stays up. The partition being gone is also
the case where the stored credential and the ssh keys cannot be read, so the
alternative is a stick nobody can log into or reach reliably; but a stick
on a shared host port is exposed to whoever can reach it meanwhile. It lasts
until the next reboot, which tries the partition again.

The management address is not moved: moving it on a pattern of power cuts
would move the address of a stick whose only fault was the power. A
safe-mode boot adds one, though: the rescue address **169.254.1.1/16**, on
`br0:3`, whatever `LAN_ENABLE_IP2` and `LAN_IP_ADDR2` say and whatever the
primary address is. It is link-local on purpose. The stock second address,
192.168.100.1, is the management address of many cable and fibre modems, and
a second device with the same address on the LAN breaks both; 169.254.0.0/16
is not used by routers or DHCP servers, and a computer with no lease picks its
own address in it, so one plugged straight into the stick reaches 169.254.1.1
with no setup (a computer with a lease needs a 169.254.x.y/16 address added by
hand). It is not added when the primary address is already 169.254.1.1. The
other ways in are the second address when it was on (its own alias,
`br0:2`), and `/etc/config/lan-ip` from a shell. A change of the management
address from this boot is stored directly, with no trial ("Management address
trial" above): the trial needs a confirm over the new address, which a
computer on the rescue link cannot make. The U-Boot environment is never written:
U-Boot here has no boot counter, and an environment written on every boot
wears the flash and risks the boot itself.

The files: `/etc/config/boot-count` (one number; present from a boot's
count until it reaches 60 s, or through a run of short boots),
`/etc/config/boot-wdt` (one number; present from a watchdog reset until
the next boot that lives 600 s or ends in a reboot), `/etc/config/safe-mode`
(`cause=` `short_boots`, `watchdog` or `config`, `short_boots=`,
`wdt_resets=`, `uptime=`, `cleared=` (`HOST_AUTONEG`, `HOST_SERDES`,
`OMCI_UNI_LOCK`, a comma-separated list of those, `none` or `failed`), `build=`; until
acknowledged), and `/var/run/safe-mode` (the same, plus
`root_password=default` or `failed`; this boot only). `safe-mode.sh status` prints them (`docs/TOOLS.md`), and
the diagnostics bundle carries it (`host-link.txt`).

## Management address trial

A wrong management address is the one mistake that takes every way in with
it, so a change of `LAN_IP_ADDR`, `LAN_SUBNET`, `LAN_ENABLE_IP2`,
`LAN_IP_ADDR2`, `LAN_SUBNET2` or `LAN_ROUTES` is a trial, the way the host port is
("Host port auto-negotiation" above). The web UI (Config, Network, Save) or
`mgmt-addr.sh try LAN_IP_ADDR=10.1.1.1` (`docs/TOOLS.md`) does not store the
keys. One second later (so the answer leaves first) `network.sh addr` applies
them in RAM, and a 90 s timer starts. Within it the change is confirmed from
a client that reached the stick **on the new address**, from the page opened
there or with `mgmt-addr.sh confirm` over ssh to it: only then are the keys
stored. At the end of the window, or on Revert, the stick goes back to the
addresses it had, and logs `event=trial_expired`.

- **The old address stays up during the trial.** The new primary goes on
  `br0` and the primary the stick ran before moves to its own alias, `br0:4`
  (the second address, `br0:2`, follows the trial). So the page opened on the
  old address still answers, shows the countdown and can revert; it cannot
  confirm. The old address is dropped when the trial ends either way.
- **The confirm must arrive over a new address.** confd hands the script the
  address the request came in on (over ssh it is `SSH_CONNECTION`), and a
  confirm on an address the stick already had is refused, naming the one to
  use: it would prove nothing about the new one. When no address is new (only
  the netmask changed, or the second address went away) the client must be on
  the link of the new netmask. A confirm that cannot say where it arrived is
  refused.
- **`LAN_ROUTES` is the sixth key of the trial.** `mgmt-addr.sh try
  LAN_ROUTES="default via 192.168.1.253"` installs the routes in RAM (the
  routes of `proto static` on the management device are flushed first) and
  reverts them with the trial; `confirm` stores the key (an empty value
  removes it). A trial that changes **only** `LAN_ROUTES` is confirmed from
  **any** client, over any address: no address is new, and the confirm
  arriving at all proves that the client still reaches the stick with the
  routes in place, which is the one thing a route change risks. A trial that
  changes an address key too keeps the rule above. `try` refuses a
  `LAN_ROUTES` that `network.sh` would turn off (the reason is printed and
  nothing starts), and it judges the stored routes against the new
  addresses: moving the primary out of the subnet of a stored gateway is
  refused until the same trial also changes or clears `LAN_ROUTES`.
- **A reboot during the trial comes up on the old address.** The keys are
  written only by the confirm, and the trial state and file are in `/var/run`,
  which the boot starts empty; `network.sh` at the boot (and the watchdog
  check) never reads the trial. Cutting the power in the milliseconds between
  the confirm writing two keys of a changed address and netmask leaves the
  stored pair half changed, which `network.sh` refuses like any invalid pair
  (stock default 192.168.1.1/24 and a boot you can reach).
- **A change the stick would refuse is refused up front**: `try` runs
  `network.sh addr -n` first, so a loopback address, a netmask with a gap or
  the broadcast address of its mask fails at once with the reason, and nothing
  starts.
- **Not while `/etc/config/lan-ip` exists** (it wins over `LAN_IP_ADDR`, so a
  trial would change nothing), and not in a safe-mode boot.
- **The safe mode is the way out, with no trial.** A safe-mode boot is reached
  on the rescue address 169.254.1.1 ("Recovery: the safe mode"), usually from a
  computer that is not on the network the stored address belongs to, so a
  confirm over the new address could not be made. There the web UI stores the
  keys directly and applies them with `apply.sh network`, as before the trial
  existed; `mgmt-addr.sh try` refuses.
- An image whose `confd` is older than the script (or the reverse) keeps the
  old behaviour: the page stores the keys directly when `/etc/scripts/mgmt-addr.sh`
  is absent, and the daemon refuses them in `POST /api/config` only when it
  is present.

The state of the running boot is `/var/run/mgmt-addr` (`docs/TOOLS.md`,
"mgmt-addr.sh"); the web UI reads it.

## Web UI controls

| control (tab) | on this image | apply |
|---|---|---|
| Save (Config) | writes the keys, reads each back, then applies LIVE ones and offers the rest | per key, above |
| Apply now (save bar, OLT identity) | `apply.sh omci`, after a confirmation: omcid re-registers the ONU | INTERRUPTS INTERNET |
| Reboot now (save bar, Firmware) | reboots, after a confirmation naming the slot it comes back on | REBOOT |
| Save of a management address key (Config, Network) | `mgmt-addr.sh try KEY=VALUE...`, then a dialog counting down the 90 s with the new address to open: Keep (on the new address) runs `confirm` and stores the keys, Revert runs `revert`; no confirm reverts by itself. Not used in a safe-mode boot | LIVE (a trial) |
| Host port (Config, Network) | `host-autoneg.sh try on\|off`, then a dialog counting down the 120 s: Keep runs `confirm` (stores `HOST_AUTONEG`), Revert runs `revert`; no answer reverts by itself | LIVE (a trial) |
| Host link and safe-mode banner (every view) | shown while the boot fallback runs auto-negotiation with `HOST_AUTONEG=off`, during a safe-mode boot, and while a safe-mode record waits; its button acknowledges the safe mode (`safe-mode.sh ack`) | -- |
| OLT identity switch (Config) | creates or removes `/etc/config/omci-identity.on` | INTERRUPTS INTERNET (with Apply now) |
| Stock keys (tab) | the stock keys this image does not act on (161), read-only | -- |
| Config UI credentials (Admin) | `/etc/config/confd.auth` | LIVE (sign in again) |
| SSH keys (Admin) | `/etc/config/dropbear.d/authorized_keys`, read at every login | LIVE |
| Download a full config backup (Admin) | both stores, every key, identity included; `tools/config-backup.sh` takes the same file from a host (below) | -- |
| Download a diagnostics bundle (Admin) | logs, the previous boot ramlog, slot state, the GPON/OMCI state and the config, every password redacted and the serial number, MACs and LOID masked, as a tar.gz (`/etc/scripts/diag-bundle.sh`, `docs/TOOLS.md`) | -- |
| Restore (Admin) | writes the differences, every key, through the same checks as Save | per key; restored keys are not applied |
| Reset the service config (Admin) | `flash default cs`: merges `/etc/config_default.xml` into CS (below) | LOID keys: INTERRUPTS INTERNET; the rest are stock-only |
| Ping (Tools) | IPv4 literals, three packets | LIVE |
| Kernel log (Tools) | the ring buffer, read without consuming it | -- |
| Status: optics, ONU state, alarms, counters | `diag` batch | -- |
| Read the MAC table (Status) | works: confd runs `diag l2-table get entry address valid` (the stock spelling of `diag l2-table get all`), which walks the switch L2 lookup table through `/dev/odi_sw`; read-only | -- |
| Services, MIB | `omcicli` answered by omcid | -- |
| Firmware: Upload | into `/tmp/img.tar`, which is RAM (8 MB cap) | -- |
| Firmware: Write | `fwu_starter.sh`: the inactive slot only, in the background; the page follows its log | -- (then Try) |
| Firmware: Try a partition | sets `sw_tryactive` with our `nv` | REBOOT |
| Firmware: Keep | sets `sw_commit`: removes the fallback to the other slot | at the next boot |

**Reset** merges the image defaults instead of erasing: every key
`/etc/config_default.xml` names (the four LOID keys, emptied; `DEVICE_TYPE`,
`DUAL_MGMT_MODE` and the three `OMCI_CUSTOM` masks, which only the stock
firmware reads) takes its default, a default key absent from both files is
added, and every other key -- the management addresses, the VLAN, the PLOAM
password, the identity -- is left alone. The HS file is never touched; a
default key that lives there (`LOID_PASSWD_OLD` on a stick that keeps it
where xmlconfig puts it) stays as it is. The reset keys are reset for the
stock slot too, because the store is shared. The UI downloads a backup
before it starts.

The Firmware tab shows, for the running partition, the image it runs
(`image=` in `/etc/odi-build`, or `/etc/version`), and the U-Boot record
`sw_version0/1` beside it when they differ: `fwu.sh` does not update that
record unless asked, so it often still names the firmware the slot held
before.

### Backing up from a host

The config partition is the one thing on a stick nothing can regenerate: a
reflash never touches it, and losing it (or a silent change to the serial,
PLOAM password, LOID or VLAN) is an outage. `tools/config-backup.sh` takes the
same backup the Admin tab downloads (odi-ui `GET /api/backup`), from any
Linux or macOS host with `curl`, and keeps a dated copy only when the
settings changed:

    ODI_HOST=192.168.1.1 ODI_AUTH_FILE=/etc/odi/ui.auth tools/config-backup.sh

| variable | meaning | default |
|---|---|---|
| `ODI_HOST` | the stick management address | required |
| `ODI_AUTH_FILE` | a file holding the web UI `user:password`, one line | -- |
| `ODI_USER`, `ODI_PASSWORD` | the same credential from the environment, when there is no file | -- |
| `ODI_BACKUP_DIR` | where the copies go | `./odi-config/<ODI_HOST>` |
| `ODI_KEEP` | how many copies to keep | 30 |
| `ODI_TIMEOUT` | seconds for the whole download (`curl -m`) | 20 |
| `ODI_PORT` | confd port | 80 |

Copies are named `odi-config-<UTC time>.xml`, mode 600 in a directory made
mode 700: they hold every identity key and password in clear. A run whose
settings match the newest copy writes nothing (the leading comment, which
names the confd build, is not compared, so a UI upgrade alone is not a
change); the oldest copies past `ODI_KEEP` are removed. The credential goes
to curl on stdin, never on its command line. Exit status 0 means the backup
was taken (new or unchanged), 1 that it failed -- the stick was unreachable,
refused the credential, or sent something that is not a backup -- and 2 a
usage error, so a scheduler can alert on it. Restore a copy with the Admin
tab "Restore".

Every 6 hours from cron, mailing only on failure:

    0 */6 * * *  ODI_HOST=192.168.1.1 ODI_AUTH_FILE=/etc/odi/ui.auth ODI_BACKUP_DIR=/srv/odi-config/isp1 /opt/odi-oss/tools/config-backup.sh >/dev/null

Or as a systemd timer, `/etc/systemd/system/odi-config-backup.service`:

    [Unit]
    Description=Back up the ODI stick config store
    Wants=network-online.target
    After=network-online.target

    [Service]
    Type=oneshot
    Environment=ODI_HOST=192.168.1.1
    Environment=ODI_AUTH_FILE=/etc/odi/ui.auth
    Environment=ODI_BACKUP_DIR=/srv/odi-config/isp1
    ExecStart=/opt/odi-oss/tools/config-backup.sh

and `/etc/systemd/system/odi-config-backup.timer`:

    [Unit]
    Description=Back up the ODI stick config store every 6 hours

    [Timer]
    OnCalendar=00/6:00
    RandomizedDelaySec=10min
    Persistent=true

    [Install]
    WantedBy=timers.target

then `systemctl enable --now odi-config-backup.timer`; a failed run shows in
`systemctl --failed`. The exporter reports a hash of each config file
(`gpon_config_info`, odi-sfp-exporter), so an alert on a change can point at
the backup taken before it.

## Keys this image stores but does not use

Writing any of these changes the file and nothing else on this image; the UI
shows them read-only on its Stock keys tab. They still steer the stock
firmware if the stick falls back to it, so leave them as they are unless you
mean to change the stock slot -- over ssh with `flash set`, or through a
restore.

| keys | why |
|---|---|
| `DEVICE_TYPE` | this image is a bridge (SFU) only |
| `OMCI_CUSTOM_BDP`, `OMCI_CUSTOM_RDP`, `OMCI_CUSTOM_MCAST`, `OMCI_CUSTOM_ME`, `DUAL_MGMT_MODE` | select stock OMCI plugins; omcid displays them (`omcicli get cflag`, `get dmmode`) and acts on two bits of `OMCI_CUSTOM_ME`: 256 (with `OMCI_VEIP_SLOT_ID`, above) and 64, which creates the CTC ONU capability entity (class 65529) as the stock firmware does; and on bit 1 of `OMCI_CUSTOM_MCAST`, which strips the tag of downstream multicast whatever the OLT's multicast profile asks (ME 309 attribute 16 read as type 1, as the stock plugin rewrites it), on the multicast VLANs the profile names (`docs/SWITCH.md`, "Downstream tag control"); `apply.sh omci` rebuilds the rules in place |
| `OMCI_FAKE_OK` | the stock OMCI stack option; this image reads its own odi-only `OMCI_UNKNOWN_ME_OK` instead (above), so the stock slot keeps its own setting |
| `OMCI_OLT_MODE`, `OMCI_PORT_TYPE`, `OMCI_TM_OPT`, `OMCI_WAN_QOS_QUEUE_NUM` | stock OMCI stack options with no counterpart in omcid |
| `OMCI_LOGFILE`, `OMCI_LOGFILE_MASK`, `OMCI_DBGLVL` | omcid always logs every frame to `/var/log/omcid.log` |
| `PON_MODE`, `PON_DETECT_ENABLE`, `EPON_*` | this image is GPON only and never rewrites the PON type |
| `PON_VENDOR_ID`, `MAC_KEY`, `GPON_PLOAM_FORMAT` | the vendor id comes from `GPON_SN`; the other two are read by the stock firmware only. Keep them in backups |
| `FIBER_MODE`, `LAN_SDS_MODE` | the host SerDes mode is the odi-only `HOST_SERDES` (Fiber 1G unless set, "Host port SerDes mode" above): a stick that runs Fiber 1G can hold any `LAN_SDS_MODE`, so the stock value cannot choose it; the UI never writes them |
| `LASER_POLARITY_TYPE`, `PON_LED_SPEC` | rcS drives the laser enable itself; there is no PON LED |
| `SUSER_NAME`, `SUSER_PASSWORD`, `USER_*`, `E8BDUSER_*`, `SUPER_*` | stock firmware accounts; this image logs in as root with SSH keys or the per-build password |
| `SNOOPING_ENABLED`, `RTK_IGMP_*` | the stock keys are not read: `igmpd` takes its settings from what the OLT provisions (ME 309, ME 310) and runs only while the odi-only key `IGMP_SNOOPING` is `1`; the PON drop has its own key, `IGMP_PON_DROP` |
| `DNS_MODE`, `DNS1`-`DNS3` | a bridge resolves nothing |
| `SW_PORT_TBL[*]`, `AUTO_PVC_SEARCH_TBL[*]` | omcid programs the switch from the OLT's MIB |
| `WAN_*`, `LAN_RIP`, `LAN_AUTOSEARCH`, `UPNP*`, `SYSLOG*`, `NTP_EXT_ITF`, `TFTP_SERVER_ADDR`, `POSIX_TZ_STRING`, `REBOOT_TIME`, `DHCP_PORT_FILTER`, `BR_*`, `SNMP_SYS_NAME`, `DEVICE_NAME`, `RTK_DEVID_*`, `RTK_DEVINFO_*`, `HW_CWMP_*`, `INIT_*`, `MP_*`, `SPC_*`, `MIB_*_MAC_CTRL`, and the other stock gateway keys (`BYTE`, `WORD`, `OUI`, `PORT_REMAPPING`, ...) | home-gateway and TR-069 features of the stock firmware |
| `ADSL*`, `HW_WLAN*`, `WLAN_MAC_ADDR`, `HW_RF_TYPE` and the other radio calibration keys | hardware this board does not have |

The list the UI uses is odi-ui `schema/settings.tsv`; a key missing from it
is a key the UI will not offer, so the two must change together.

## Switch files on the config partition

Plain files under `/etc/config`, created with `touch` or `echo`, removed
with `rm`. They survive reflashing like the keys do, so a switch left behind
applies to every later image.

| file | effect | apply |
|---|---|---|
| `omci-identity.on` | report the six OLT identity keys (above); the UI toggles it | INTERRUPTS INTERNET (`apply.sh omci`) |
| `dropbear.off`, `confd.off`, `metricsd.off`, `syslogd.off`, `klogd.off`, `ntpd.off`, `omcid.off` | that daemon is not started (`/etc/init.d/services stop <name>` writes the flag and kills the process; `start` removes it). `omcid.off` leaves the ONU with no OMCI, so the OLT provisions no service: `stop omcid` also releases omcid's watchdog deadline (registers it with 0) and rcS does not register it while the flag exists; `start omcid` registers it again | REBOOT |
| `lan-ip` | the management address, one line; overrides `LAN_IP_ADDR` | LIVE (`apply.sh network`) |
| `pon-steps` | development: a PON step list that replaces the built-in one (`docs/BOOT.md`); a wrong list means no PON | REBOOT |
| `sds.off` | skip the host SerDes check and fix in `network.sh` | REBOOT |
| `boot-count`, `boot-wdt`, `safe-mode` | not switches: the boot counters and the safe-mode record ("Recovery: the safe mode" above), written by `safe-mode.sh`; removing the record is the acknowledgement | -- |
| `optics.off` | skip the `laser` PON step (the I2C steps still run): **the laser stays off, no service** | REBOOT |
| `modules.off` | skip the platform init and omcid: **no OMCI, no service**; `apply.sh omci` refuses | REBOOT |
| `breadcrumbs.on` | development: boot crumbs, `trial-diag.txt` and a heartbeat, on flash | REBOOT |
| `confirm-arp` | development only: one line, the IPv4 address to ARP (`echo 192.168.1.2 > /etc/config/confirm-arp`); the boot confirms to the watchdog only after an ARP reply from it, so an unreachable trial reverts by itself; **a stick whose host stops answering there then resets every 120 s**. An empty file, or one that is not a dotted IPv4 address, is no target: logged, and the boot confirms as usual. `nv commit` refuses while it exists (`nv commit --force <slot>` overrides) and the login banner names it | REBOOT |
| `confd.auth` | the UI credential, `user:password` | LIVE |
| `dropbear.d/authorized_keys` | root's SSH keys | LIVE |
| `confd/` | replacement web UI files and `.tsv` tables, by name | LIVE |

Every REBOOT row interrupts internet, as every reboot does. The three
development rows are read by `/etc/init.d/rcS.dev` and exist for trial
boots; `docs/BOOT.md` has what each does. Of them only `confirm-arp` can
reboot a stick by itself: left on one whose host stops answering at the address
in it, it resets every 120 s; that is why `nv commit` refuses while it
exists. `breadcrumbs.on` only gets a one-line note from `nv commit`; the
ssh login banner names both files while they exist.

Files of earlier images that this one ignores, and that can be removed:
`bdgconn-probe` (an older image rebooted unconditionally after the seconds
it held), `skip-steps`, `replay/`, `regtrace.*`, `regtrace-mark`,
`regtrace-all`, `modload.mask`, `sdkinit.mask` and `parity.*`.

`docs/HACKING.md` ("Feature toggles") is the developer reference behind this
table: where each file is read, its default, and which ones are development
aids, together with the kernel parameters, build variables and
`/proc` control files.

## Die temperature limits (runtime, not an odi.conf key)

The SoC hwmon device (`rtl9602c_soc`) carries `temp1_max` (default 95000) and
`temp1_crit` (default 110000), in millidegrees C. Root writes them through
sysfs (`echo 90000 > /sys/class/hwmon/hwmonN/temp1_max`); the accepted range
is 0..150000 with max <= crit, and they are **not persisted**: every boot
starts at the defaults. `temp1_max_alarm` and `temp1_crit_alarm` read 0 or 1.
The alarms only flag and log (kernel log, on transitions); nothing powers
off or throttles, because the temperature scale is not verified. See
[`KERNEL.md`](KERNEL.md), "Our drivers".

## Watchdog rules

**The kernel (`odi_wdt`) is the only owner of the hardware watchdog.** It
stops kicking -- so the board resets -- when any one of three independent
rules fails. There is no separate userland process guessing at any of this
from `/proc`: v1.0.2 shipped one (a "health kicker") and withdrew it the
same day (`CHANGELOG.md`, v1.0.2) after it withheld its kick correctly on
ISP1 but the kernel had no enforcement of its own, so the board never
reset and the trial hung for 15 minutes until someone power-cycled it. In
this design every rule is judged, and every reset is forced, by the
kernel itself.

| rule | deadline / floor | armed by | source |
|---|---|---|---|
| boot confirmation | 120 s of uptime (`ODI_WDT_USERLAND_DEADLINE_S`) | always, from boot | rcS writes `1` to `/proc/odi_wdt/userland_ok` once userland is up (unchanged from v1.0.1); `/etc/config/confirm-arp` (development) makes that wait for an ARP reply from the address in the file first (no valid address: no wait) |
| per-client ping deadline | one per registered client, e.g. 60 s for omcid (`ODI_WDT_OMCID_DEADLINE_S`) | registration itself, counted from that moment | rcS registers each required client once, by name and deadline, with `echo "<name> <deadline_s>" > /proc/odi_wdt/register` (idempotent: re-registering just updates the deadline AND resets the clock). The client itself pings its own deadline from its own main loop with `echo "<name>" > /proc/odi_wdt/ping` -- omcid does this every 15 s (`src/omci/respond/main.c`, `wdt_ping()`), a quarter of its 60 s deadline. A registered client that never pings at all is caught the same way one that pinged once and then stalled is (fixed 2026-09-28, hardware trial rc4: a client armed only by its own first ping is never checked if that first ping never comes, which is exactly the shape a daemon stuck before it ever pings takes). Because of this, rcS only registers a client under the same conditions that will actually start it (`svc-omcid.sh`'s own `modules.off`/binary-exists gate, mirrored in rcS before the `/proc/odi_wdt/register` write) -- a kernel too old to have `/proc/odi_wdt/register` still degrades to "never checked", silently, same as before |
| trial guard | 300 s from registration, never pinged | `trial-guard.sh`, on a trial boot only | a client like omcid's, registered as `trial 300` and released (`trial 0`) once the management path is seen working both ways or the slot is committed; unreleased, it resets a trial nobody could reach back to the committed slot (`docs/FLASHING.md`, "The trial guard") |
| memory floor | `MemAvailable` below 2048 KB (`ODI_WDT_MEM_FLOOR_KB`) for 3 consecutive 5 s checks (`ODI_WDT_MEM_FLOOR_CONSEC`) | always, from boot | sampled by the kernel itself every tick (`si_mem_available()`, `kernel/extra/drivers/net/ethernet/odi/odi_wdt.c`) -- no userland reader to lose along with the memory it would be reporting on. Several consecutive samples, not one, so a single allocation spike does not reset a box that is otherwise fine. This is the same "OOM took the box and nothing came back" case a 20 MB `scp` into an unbounded `/tmp` produced on ISP1 (2026-09-27), now caught kernel-side instead of by a process that OOM can also kill |
| CPU-port RX | 6 consecutive 5 s checks (`ODI_WDT_CPU_RX_STALL_TICKS`, 30 s) in which the switch offered frames to the CPU port and the NIC took no RX descriptor back | whenever a NIC device is open | the kernel samples it each tick: "offered" is port 3 `ifOutUcastPkts` + `ifOutMulticastPkts` + `ifOutBroadcastPkts` + `ifOutDiscards` + `dot3InPauseFrames` from the switch MIB (delivered, dropped for it, or held back by PAUSE from the CPU MAC, `odi_nic`; this is the internal CPU port, not the host port whose PAUSE v1.3.0 turned off), "taken" is the descriptors the `odi_nic` poll handed back from RX ring 1, the ring host and management frames arrive on; with `odi_nic.omci_ring` on, OMCI frames on the second ring do not count, so they cannot hide a dead ring 1 (`odi_switch.c`, `odi_switch_cpu_rx_sample()`, `odi_nic_rx_taken()`). A check with nothing offered neither counts nor clears, so an idle CPU port never resets; any descriptor taken clears it. This is the case where the kernel, omcid and the switch datapath are all fine and only the stick itself is unreachable -- a flood that tripped the NIC interrupt-storm guard did that on ISP1 (2026-09-29), with no other rule able to see it. Before the reset it logs one line of NIC state (`odi_nic_report()`) |

Debugging: `cat /proc/odi_wdt/clients` shows one line per registered
client -- name, deadline, whether the first ping has armed it, and how
long ago the last ping landed. Every reset logs which rule fired,
ramlog-visible, before it happens (`odi_wdt_deadline_timer_fn()`):
`"userland did not confirm within 120 s"`, `"client omcid missed its 60 s
deadline"`, `"MemAvailable ... below the 2048 KB floor for 3
consecutive checks"`, or `"CPU port offered frames for 6 ticks (30 s) and
the NIC took none"` followed by the `odi_nic:` state line. The rule is also
recorded as the ramlog reset reason, so the next boot shows it as
`reason=wdt_userland`, `reason=wdt_client:omcid`, `reason=wdt_mem` or
`reason=wdt_cpu_rx` on the `previous boot:` line
of `/proc/odi_ramlog_prev` (`docs/HACKING.md`, "Reading a boot you could
not see"), and metricsd exports it as `gpon_last_reset_reason`.

Registering a new client (say, dropbear or confd, neither wired up today)
means two things, both required: rcS gets an
`echo "name deadline_s" > /proc/odi_wdt/register` line at boot, gated on
that client actually being about to start (mirror whatever `svc-<name>.sh`
already checks), and the client's own main loop gets a periodic
`echo name > /proc/odi_wdt/ping` well inside that deadline. The registration
line alone is enough to arm the deadline (see the table above) -- omitting
the ping is a bug that gets the board reset once the deadline elapses, not
a silent no-op -- and a ping to a name nobody registered is refused.

## Resilience

Background, unchanged from v1.0.2: a 20 MB `scp` into `/tmp` on ISP1
(2026-09-27) exhausted RAM (`/tmp` was ramfs, unbounded and unreclaimable),
the OOM killer took dropbear, confd and omcid, and none of them
restarted -- the stick stayed at O5 (the hardware datapath keeps
forwarding on its own) but was unmanageable until a power cycle. Not
config-partition settings -- there is no UI or file toggle for any of
these, they are fixed at boot.

| what | value | why |
|---|---|---|
| `/tmp` (`/var/tmp`) size cap | 8 MB tmpfs | fits a firmware upload (`fwu_starter.sh` stages the tarball there, about 2.6 MB, plus its unpacked squashfs/uImage) and an scp of a few MB with room to spare; past it, a write gets `ENOSPC`, not a system-wide OOM |
| `/var` (log/run/lock/config fallback) size cap | 6 MB tmpfs | `/var/log` is separately trimmed at 256 KB past a 128 KB floor already (rcS); this is the outer bound if that trim ever falls behind |
| `oom_score_adj` | omcid, dropbear: `-1000` (never killed while anything else can be); confd: `-500`; metricsd: `0`, the kernel default | omcid and dropbear are what keeps the ONU provisioned and the box reachable; confd is a convenience next after them; metricsd is the one daemon whose loss costs neither -- first in line if the killer has to take something |
| respawn | omcid, dropbear, confd, metricsd, syslogd, klogd, ntpd, igmpd: each an `/etc/inittab` `respawn` entry (`/etc/scripts/svc-*.sh`, execing into `/etc/scripts/respawn.sh`), restarted by busybox init the instant it exits; `respawn.sh` adds a bounded wait before the exec when the daemon keeps dying ("Restart budget" below) | native over hand-rolled: a v1.0.4 trial on hardware killed omcid with `kill -9` and it stayed dead -- the old `supervise()` shell loop had lost track of it -- while busybox init, which forked the process itself and never lost the pid, does not have that failure mode. The budget keeps that: nothing runs beside the daemon, the wait happens in the process init already tracks, and the daemon is exec-ed in place |
| `vm.min_free_kbytes` | 1536 (`rcS`, up from the kernel's own default of roughly 128 KB on a box this size) | keeps a slightly larger page-allocator reserve free under pressure, so the OOM killer and a starved `oom_score_adj -1000` daemon get a better chance to make forward progress instead of every allocator racing for the same last few pages |

`make test-qemu` (`docs/HACKING.md`) exercises everything above except the
watchdog rules themselves, which need `/proc/odi_wdt` -- real hardware,
not the stock kernel the harness boots; those are host-tested instead
(`test/odi_wdt_test.c`, against a fake clock).

## Restart budget

busybox init restarts a `respawn` entry the moment it exits and has no rate
limit of its own, so a daemon that dies at once would be started again and
again, at the cost of CPU and of log lines. `/etc/scripts/respawn.sh`, the
last step of every `svc-*.sh` before the daemon is exec-ed, now spaces such
restarts out. It adds no process and no loop: the wait is a `sleep` in the
shell init already tracks, then the same in-place `exec` as before.

**Why not runit or start-stop-daemon.** busybox here already has `runsv`,
`runsvdir`, `sv` and `svlogd` built in (no size to add), and they were
weighed first. `runsv` waits a fixed second between restarts and keeps no
count, so a budget would still be a hand-written `finish` script, on top of
a supervision tree (`runsvdir`, one `runsv` per daemon, service directories
in RAM) that changes how every daemon is started, found by `services` and
`apply.sh`, signalled and stopped, and moves the pid init tracks. That is
the kind of change this image paid for in v1.0.4. `start-stop-daemon` only
starts a daemon, it does not supervise. The script is about forty lines of
logic, and the one thing it must not do (lose the daemon) it does not do:
it execs.

**The rule.** Each start records the uptime at the exec; the next start (init
ran the script again, so the daemon is gone) subtracts it. A run shorter
than 30 s is a short run, 30 s or longer clears the count.

| short runs in a row | wait before the start | state |
|---|---|---|
| 0 | none, as before | `ok` |
| 1, 2, 3 ... | 1, 2, 4, 8 ... s, never above the daemon cap | `backoff` |
| 5 or more | the cap | `degraded` |

A first start of the boot never waits, so a normal boot is as fast as it was,
and the 120 s userland confirmation (sysinit, `rcS`) is finished before init
reads a single `respawn` entry: nothing here can delay it. Start order does
not change. A reboot or shutdown during a wait ends the waiting shell like
any process init signals; a `services stop <name>` also kills a wait in
progress, so the daemon does not start after the flag is written.

**Policy per daemon.** Nothing here disables a daemon, enters safe mode or
reboots, because no daemon but omcid is worth a reboot, and a reboot loop
or a stick whose only login is switched off is worse than a daemon that
keeps being retried slowly. A daemon is never parked: at the cap it is
retried every `cap` seconds, so a cause that clears (a missing peer, a
full `/var`, a bad key later fixed) heals without anyone. To switch one
off on purpose, `services stop <name>` (it survives a reboot).

| daemon | cap | when it will not stay up |
|---|---|---|
| `omcid` | 2 s | watchdog: it stops pinging its 60 s deadline and the kernel resets the board, as before. The wait only stops a spin and stays far below that deadline, so it cannot hold back a recovery the watchdog would have given |
| `dropbear` | 10 s | retried every 10 s at most: the only way in stays as close to a plain restart as a loop can be |
| `confd`, `syslogd` | 30 s | retried every 30 s |
| `metricsd`, `klogd`, `ntpd`, `igmpd` | 60 s | retried every 60 s |

What changes for omcid is only the wait (1 s, then 2 s) after a start that
lived under 30 s, the state file and the log line; a single crash after a
long run is restarted at once, and the re-provisioning that follows a
respawn (docs/BOOT.md) is untouched. The caps are fixed in `respawn.sh`:
there is no setting.

**Where it shows.** `/var/run/respawn/<name>` (RAM, written by rename,
`key=value`), read by anything that wants it:

    state=degraded
    starts=9
    fast=8
    last_run_s=1
    delay_s=30
    cap_s=30
    stable_s=30
    last_start_s=842

`state` is as of the latest start and is not rewritten when the daemon
stays up, so a reader takes the daemon as healthy again once `uptime -
last_start_s >= stable_s`. `starts` counts starts since boot, the first
included; `fast` the short runs in a row. One kernel-log line per restart
(`dmesg`, the ramlog, `logread` through klogd, and `SYSLOG_SERVER`), at
notice for a restart after a long run and at warning for a short one:

    respawn: event=respawn service=confd state=backoff starts=4 fast=3 last_run_s=1 delay_s=4 cap_s=30

Beyond the fifth short run only every tenth line is written, so a crash
loop cannot cycle the 64 KB ring; the state file always has the exact
count. The daemon output is still appended to `/var/log/services.log` (omcid:
`/var/log/omcid.log`), where its own last words are. The exit status is not
recorded: init, not the script, reaps the daemon. A `services start` or a
`kill` of a daemon within 30 s of its start counts as one short run (a wait
of a second); several in a row, as in a test, back off like any crash.

Tested by `test/respawn_budget_test.sh` (every `::respawn:` entry of the
inittab, against a fake clock; in `make test-host`) and by the repeated
`kill -9` scenario of `make test-qemu`.

## Bounded waits

Every command a script here runs, and every wait one process does on
another, is bounded unless there is a stated reason not to -- found
necessary the hard way (rc3, ISP1, 2026-09-28): a stuck omcid left both
`metricsd`'s own child wait and `vq_ensure()`'s queue-reclaim logic
waiting on it forever (CHANGELOG.md, "Unreleased"). The pattern:

- **A forked command**: `timeout SECS cmd args...` (busybox `timeout`,
  built into this image's busybox -- `packages/busybox/config.fragment`).
  Already the shape of every `diag` call (`network.sh`'s `sds_read`/
  `sds_write`, 10 s; `rcS.dev`'s dev-hook diag probes, 10 s) and of the
  `/proc` reads and `arping` in `rcS.dev`'s confirm-arp path (5 s, 2 s).
- **A bare `echo verb > /proc/odi_init` or `/proc/odi_omci`**: this is the
  calling shell's own `write(2)`, not a forked command, so `timeout` has
  nothing to wrap unless the write runs inside a process of its own.
  `write_proc_bounded`/`read_proc_bounded` (`scripts/rcs-lib.sh`, sourced by
  `rcS`, `rcS.pon` and `omci-respawn-reprovision.sh`) do exactly that: `timeout
  $PROC_WRITE_TIMEOUT_S sh -c '...'`, default 5 s. Used by every PON step
  and the `switch_init` verb (`rcs-lib.sh`), the `odi_wdt` register write
  (`rcS`), and the `gpondeact`/`gponpw`/`gponact` writes of the respawn
  re-provisioning (`omci-respawn-reprovision.sh`, `omci_reactivate()`) -- a
  handful of calls each, off the tightest part of the boot or off it
  altogether. (`apply.sh omci` writes none of them: omcid does, on SIGHUP.)
- **A documented exception, by cost rather than by risk**: `rcS`'s switch
  SDK-init loop (23 verbs, every boot, no exception) deliberately still
  uses a bare `echo`/`cat` -- `write_proc_bounded` turns a zero-fork shell
  builtin into a `timeout`+`sh` fork pair, and at 23 steps that is up to
  ~90 extra fork/execs on the one path this image has fought hardest to
  keep fast (rc2 to rc3: 85-90 s boot down to 31 s). No step in this loop
  has ever been observed to hang; the crumb before and after each already
  names a stuck one in the ramlog if it ever does. Bound a hot, unproven
  path only if it actually hangs -- do not pay the fork cost everywhere on
  principle.
- **A documented exception, by kernel behaviour**: `rcS.dev`'s
  SerDes/trial-diag dev hook deliberately runs no `diag` call of its own
  reasoning ("a diag ioctl can block where timeout cannot") -- `timeout`
  sends a signal to the process it started, and a process parked in an
  uninterruptible-sleep (`D` state) kernel wait ignores every signal, that
  one included. This bounds the ordinary hang (a driver waiting on a lock
  or a condition that resolves on its own, which is what has actually been
  observed) but is not a guarantee against every kind of stuck kernel call.
- **The freestanding C daemons** (`odi-sfp-exporter`'s `metricsd`, this
  image's own `omcid`) have no `timeout` binary to shell out to: they poll
  the child's pipe with a timeout and `SIGKILL` + reap it on expiry
  instead (`drain_bounded()`/`kill_and_reap()`,
  odi-sfp-exporter's `src/syscall.h`) and report the failure as a metric
  (`gpon_omci_up`) rather than going silent.

## Not verified yet

- `HOST_SERDES`: the register sequences and the port force are the
  reporter's, read off a stock HSGQ stick. On this image 2500BASE-X linked an
  HSGQ stick to a 2.5G router port, and on a DFP-34X-2C2 `reapply` of Fiber
  1G, a 2500basex trial and its revert, and the boot watch falling back to
  Fiber 1G with no 2.5G host left the SerDes and port words as before. Not run
  on hardware: `hisgmii` with this image, the safe mode with the key set,
  whether the HSGQ board links in Fiber 1G, and bit 17 of 0x214 as the sync
  signal beyond those runs (`test/host_serdes_test.sh` and
  `test/odi_host_forced_test.c` cover them as write sequences).
- The host-side SFP EEPROM (`SFP_HOST_DDM`): how the host write protect
  packs its bits inside a word is taken from the register table and not
  checked; only A2h 224-255 depend on it. The rest has run on hardware: one
  reporter's router reads live DDM from it at boot, and a v06 stick ran it
  for three days with the host's EEPROM unchanged.
- The `HOST_AUTONEG` boot fallback on a stick: the write, the trial, its
  expiry and the forced link against a host forced off were all seen
  ("Host port auto-negotiation"), but both hosts tried accept a forced
  partner, so the forced side never lacked a link and the alternation of
  30 s phases is host-tested only.
- The safe mode with the key unset: the five-cycle path was exercised by
  setting the counter and rebooting, and the safe-mode boot happened as
  designed, but not by five real power cycles. The three-cycle path with
  `HOST_AUTONEG=off` was run with real power cycles (laser off, the ONU in
  O1, the key cleared, `admin:admin` on the web UI, `admin` for root over
  ssh, the state on `/api/hostlink` and the exporter, then acknowledge and a
  normal reboot back on the saved password and keys).
- The SIGHUP reload on a stick: the re-registration ran and the ONU came
  back to O5 with its six services, but the time it takes was not measured,
  and a new `GPON_SN` reaching the line through the rewritten PLOAM slot has
  not been run (host tested as a register sequence only). The in-place VLAN
  rebuild ran on ISP1's stick: a changed `VLAN_CFG_TYPE` and class 171 rule
  followed by SIGHUP rebuilt the rules, and tagged and untagged frames
  reached the GEM port afterwards. The earlier apply, which restarted
  omcid, was run end to end once, on ISP1.
- `apply.sh network` moving the primary address on a stick. Adding and
  removing the second address live was checked on ISP1 (`br0:2` came and
  went, the primary address and O5 untouched); the host test drives a stub
  `ifconfig` for the rest.
- `fwu_starter.sh` writing a slot from the UI (the guards and the background
  job are host-tested; `fwu.sh` itself is the flasher used by hand).
- The OLT identity keys against an OLT that checks them; LOID against a CTC
  OLT. Untested: neither ISP1 nor ISP2 does either.
