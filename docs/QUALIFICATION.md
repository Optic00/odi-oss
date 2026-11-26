# Qualification: what has been checked, on what

This page says which boards, optics, hosts, SerDes modes, OLT lines and
feature paths of odi-oss have been run on real hardware, by whom, and on
which release, and which have only been run against a model or not at all.
It exists so that "works" and "supported" are never read into a row that
only has a unit test behind it.

The firmware is written for one board, the ODI DFP-34X-2C2 (see the README,
"Supported hardware", which lists it and the boards users have reported
on). This page records the evidence for every case that has come up, so a
reader can see what a green row stands on.

## States

Every row carries one of these, strongest evidence first. A row whose
evidence mixes kinds is split in two or carries the **weaker** state. Only
**Maintainer-tested** may be narrowed, with "partly" (the notes name what did
not run) or "injected" (the OLT's frames were played in from a file on the
stick, no OLT sent them).

| state | meaning |
|---|---|
| **Maintainer-tested** | The maintainer ran it on a stick with the fibre or the host attached, and the row names the release (or, for something not yet released, "Unreleased") it was last checked on. |
| **Community-reported** | A user ran it on their own stick and wrote it up in an issue (linked). Not reproduced here; the report is the evidence, and the row says which release the reporter used. |
| **Host-tested** | Only a unit, register-model, golden or qemu test covers it (named). The code path exists and is exercised against a model of the hardware, never against the hardware. |
| **Untested** | No run of any kind, or a design that is not built. |

"Last checked" is the last time the maintainer looked, not a promise about
later releases. Releases are listed in [`CHANGELOG.md`](../CHANGELOG.md).

## Matrix A: hardware

Two sticks are in the lab, called **ISP1** and **ISP2** after the line each
sits on. Both are DFP-34X-2C2, hardware revision v06, with the Semtech
GN25L95 laser driver. Each runs a published release or a build of `main`
as its committed image (pre-releases are trialled there before they are
tagged), with the stock firmware kept in the other slot.

### Boards

| board | state | last checked / evidence | notes |
|---|---|---|---|
| DFP-34X-2C2, hardware v06 | Maintainer-tested | ISP2: `main` after v1.16.0-beta.3 (2026-11-10), v1.15.0 (2026-11-02). ISP1: v1.16.0-beta.3 content plus the PON-ID decoding (2026-11-10) | The reference board. |
| DFP-34X-2C2 sold as the HSGQ XPON stick, hardware v08 | Community-reported | [#229](https://github.com/AndreZiviani/odi-oss/issues/229), [#231](https://github.com/AndreZiviani/odi-oss/issues/231): v1.13.0 and v1.14.0 pre-releases, then v1.15.0 held 2.5G for about 3 h with O5 and the router showing diagnostics. [#300](https://github.com/AndreZiviani/odi-oss/issues/300): v1.15.0 carrying the reporter's household, PPPoE, IPTV and voice, the router reading diagnostics (2026-11-07). [#369](https://github.com/AndreZiviani/odi-oss/issues/369): v1.16.0 at 2.5G with IPTV, a second reporter (2026-11-14) | Listed in the README as Community-reported. Same U-Boot, environment and flash layout as the v06; nothing on the board tells the two apart, so the settings that differ are keys (`HOST_SERDES`, `SFP_HOST_DDM`). Never run by the maintainer. |
| DFP-34X-2C3 (SC/APC) | Community-reported | [#241](https://github.com/AndreZiviani/odi-oss/issues/241): v1.14.0 unmodified, installed from stock `V1.1.4-250620`, trial, committed; O5, full OMCI provisioning, IPv4 and IPv6. [#42](https://github.com/AndreZiviani/odi-oss/issues/42): a 2C3 hardware v2.0 on v1.2.2-beta.1 and beta.2, committed. [#299](https://github.com/AndreZiviani/odi-oss/issues/299): v1.15.0 flashed from the stock web page | A diagnostics bundle from #241 compared with a 2C2's on the same release: same flash layout, memory and laser driver. That makes it a 2C2 with another connector; it is not a hardware run here. The image still calls it a 2C2. |
| Luleey LL-XS2510 | Community-reported | [#302](https://github.com/AndreZiviani/odi-oss/issues/302), [#319](https://github.com/AndreZiviani/odi-oss/issues/319), [#321](https://github.com/AndreZiviani/odi-oss/issues/321): v1.15.0 and v1.16.0-beta.1 trial-booted from stock `V1.0.2-241026`; O5, full OMCI provisioning, PPPoE, with `HOST_SERDES=2500basex` set from the stock shell before the first boot | A rebrand on the same SoC (RTL9602C) and laser driver (id 0x6a); its host side runs at 2.5 Gb/s. The reporter's diagnostics bundles show the same flash layout. Never run by the maintainer. |
| Other ODI models, other RTL9601/RTL9602C sticks, rebrands | Untested | none | Not supported. Open an issue with the model, the stock version and the `mtdparts` line (see "Adding a report"). |

### Optics (laser driver)

| variant | state | last checked / evidence | notes |
|---|---|---|---|
| Semtech GN25L95 (laser driver id 0x6a) | Maintainer-tested | both sticks, the published releases; ISP2 v1.15.0 | Per-unit laser calibration differs between sticks and is not a variant. Also read as 0x6a by the 2C3 reporter ([#241](https://github.com/AndreZiviani/odi-oss/issues/241)) and the LL-XS2510 reporter ([#302](https://github.com/AndreZiviani/odi-oss/issues/302)), and reported on the HSGQ v08 board ([#229](https://github.com/AndreZiviani/odi-oss/issues/229)). |
| Any other laser driver, or none that answers | Host-tested | `test/odi_board_test.c` (the laser stays off and activation is refused unless `ODI_LASER_UNVERIFIED=1`) | No stick with another driver has been run here. The 2C3 reporter noted that a different driver variant exists in the wild and theirs is not it. |

### Host side (what the stick is plugged into)

| host | state | last checked / evidence | notes |
|---|---|---|---|
| x86 host with an Intel 10G NIC on its SFP+ port (1G) | Maintainer-tested | ISP2: v1.15.0 (2026-11-02), X520 error counters 0 over the soak; PAUSE fix checked on v1.3.0 | The host that showed the PAUSE-storm problem on the stock firmware. |
| x86 software router | Maintainer-tested | ISP1: v1.12.0 content (2026-10-27) | The line that carries the household's traffic. |
| A router with an SFP+ LAN port, link forced and auto-negotiated | Maintainer-tested | the build that became v1.5.0 (2026-10-15): `HOST_AUTONEG` try, expiry and confirm, a forced stick against a forced host | Both hosts tried accept a forced partner, so the boot fallback never had to act ("HOST_AUTONEG" below). |
| UniFi gateway, SFP+ port forced to 1G | Community-reported | [#241](https://github.com/AndreZiviani/odi-oss/issues/241), v1.14.0, `HOST_AUTONEG=off` | |
| MikroTik RB5009, SFP port at 2.5G | Community-reported | [#229](https://github.com/AndreZiviani/odi-oss/issues/229), [#231](https://github.com/AndreZiviani/odi-oss/issues/231): `HOST_SERDES=2500basex`, v1.14.0-beta.1 onwards, v1.15.0. [#369](https://github.com/AndreZiviani/odi-oss/issues/369), a second HSGQ v08: v1.15.0 and v1.16.0, warm reboot and cold power-up. #476: `hisgmii` trials on an HSGQ stick of a 2026 batch, v1.18.1 with the re-latch (RouterOS 7.25.1) | The link comes up with auto-negotiation off; why it needs that is not known. |
| MikroTik hEX S 2025 (E60iUGS), SFP port at 2.5G-baseX | Community-reported | #476: `HOST_SERDES=2500basex` on HSGQ sticks of 2025 and 2026 batches, v1.18.1 with the re-latch | Without it a trial whose router port is switched after it applied never synced; a boot with the port already at 2.5G synced in 1 s, as it still does. |
| UniFi Cloud Gateway Fiber, SFP+ port run at 2.5G by a third-party tweak | Community-reported | [#321](https://github.com/AndreZiviani/odi-oss/issues/321): an LL-XS2510 with `HOST_SERDES=2500basex`, v1.15.0 and v1.16.0-beta.1 | Not a stock UniFi setting. `hisgmii` synced but passed no frames there. Fiber 1G not tried. |
| Media converter on the host port (HiSGMII) | Community-reported | [#229](https://github.com/AndreZiviani/odi-oss/issues/229): the reporter's own build before the setting existed. [#478](https://github.com/AndreZiviani/odi-oss/issues/478): a DFP-34X-2C3 through a 2.5G media converter to a Keenetic Titan 1812, `HOST_SERDES=hisgmii`, v1.18.1 (2026-11-19) | At 1G in Fiber 1G, the default; 2.5G once the key was set. |
| A Linux host that picks its port mode from the module EEPROM (phylink) | Untested | none | The EEPROM the host reads says 1000BASE-LX at 1300 MBd and cannot be changed ("Host port SerDes mode" in `SETTINGS.md`). |
| Any other host | Untested | none | |

### Host SerDes mode

| mode | state | last checked / evidence | notes |
|---|---|---|---|
| `fiber1g` (default, 1000BASE-X) | Maintainer-tested | both sticks, the published releases; ISP2 v1.15.0 | The sequence is the stock one, value for value. `reapply` of it on a stick left all 13 SerDes words unchanged (v1.14.0 content). |
| `fiber1g` with `HOST_AUTONEG=off` | Maintainer-tested, partly | the build that became v1.5.0 (2026-10-15): write, trial, expiry, confirm, a forced boot. Community: [#241](https://github.com/AndreZiviani/odi-oss/issues/241) | The boot fallback (30 s alternation between the two modes when nothing links) is Host-tested only: `test/odi_host_autoneg_test.c`, `test/host_autoneg_test.sh`. |
| `2500basex` against a 2.5G host | Community-reported | [#229](https://github.com/AndreZiviani/odi-oss/issues/229), v1.15.0 (HSGQ v08). [#321](https://github.com/AndreZiviani/odi-oss/issues/321), v1.15.0 and v1.16.0-beta.1 (LL-XS2510). [#369](https://github.com/AndreZiviani/odi-oss/issues/369), v1.16.0 (a second HSGQ v08, RB5009): a live `host-serdes.sh try` and `confirm`, a warm reboot (in sync after 1 s) and a cold power-up (12 s) | Link, O5, PPPoE and voice on the first reporter's line; linked within a second on the second. |
| `2500basex` trial, revert and boot fallback to Fiber 1G | Maintainer-tested | ISP2 stick, v1.14.0 content (2026-10-28), against a 1G-only host: a 120 s trial that reverted by itself with every SerDes and port word restored; a boot with the key set that fell back to Fiber 1G and linked after 34 s | Needs no 2.5G host, which is why it could be run here. |
| `2500basex` trial with the host port switched after it applied (the re-latch) | Host-tested | `test/host_serdes_test.sh` (a host that syncs only with a lane started after it sends: the trial, the boot watch, re-latches inside a phase, controls). Community: #476, a MikroTik hEX S 2025 | The trial synced within seconds of the switch and was confirmed. |
| `hisgmii` trial with the host port switched after it applied (the re-latch) | Host-tested | `test/host_serdes_test.sh` (the same host model, which syncs HiSGMII too). Community: #477, the race measured on an LL-XS2510 against a MikroTik RB5009 (bit 17 clear, no frames until `reapply`); #478 is likely the same. #476: an HSGQ stick of a 2026 batch against a MikroTik RB5009, a downstream build of v1.18.1 with the re-latch, the router port switched by a script: at 1G-baseX when the trial applied and at 2.5G-baseX about 12 s later, the trial synced and passed frames 1.8 s after the switch; the same run with the trial's re-latch held off (`RESYNC_S` past the window) did not sync in the about 110 s the router sent 2.5G | The re-latch of 0x214 alone brought HiSGMII up; no `reapply`. None of these trials was confirmed. |
| `2500basex` link lost and found again while the stick keeps power (the keeper) | Host-tested | `test/host_serdes_test.sh` (the host port set to 1G and back, a single read out of sync, a single read back in sync, a lane that syncs between the keeper's reads and its re-latch (no `serdes_lost`; in a trial and the boot watch, no re-latch or failure said), a busy lock, a trial started between the keeper's load and its lock, a `fiber1g` trial applied, Fiber 1G running, a second keeper, `keeper=` in `status`, the spawns). Community: #477, HSGQ sticks with a downstream build of v1.18.1 carrying it: a MikroTik hEX S 2025 (E60iUGS), the port set to 1G and back to 2.5G-baseX (`serdes_back` after 52 s, most of it at 1G) and disabled and enabled (after 10 s); a MikroTik RB5009, a `fiber1g` trial that expired back to 2500BASE-X with the port still at 1G, set back to 2.5G-baseX by hand (after 83 s, the time at 1G included) | The mode stayed `2500basex`, `fallback=0`. Not run against the UniFi Cloud Gateway Fiber of #477, which moves its port from 1G to 2.5G about 250 s after its kernel starts: whether the stick keeps power through that reboot is not measured, and if it does not, that case is the boot watch's, not the keeper's. |
| `hisgmii` | Host-tested | `test/host_serdes_test.sh`, `test/odi_host_forced_test.c`. Community: [#321](https://github.com/AndreZiviani/odi-oss/issues/321), v1.15.0 on an LL-XS2510 against a 2.5G router port, before 0x21808 bit 8 was set: SerDes sync, no frames, and the trial went back to stock. #476: an HSGQ stick of a 2026 batch against a MikroTik RB5009 with its port already at 2.5G-baseX, a v1.18.1 trial: in sync at the first read and passing frames, then reverted (not confirmed; throughput above 1 Gbit/s not measured) | Downstream report, not a release of this repo, A/B on two sticks (this sequence against the one before it, without the 3.125G page, the receive pulse and 0x21808 bit 8): a MediaTek MT7988A host (Banana Pi BPI-R4), a MikroTik RB5009 (2026 batch) and a media converter with no 1G mode (2026 batch) passed frames only with this sequence; a media converter of 2022 passed frames with both. Which of the three writes the newer hosts need is not isolated. The RB5009 ran with module EEPROM A0h byte 12 at 0x1F (3100 MBd); this image reports 1300 MBd, so a host that picks its rate from the EEPROM is untested with it. It has no boot watch: nothing tells a dead host from a live one. |
| Whether the HSGQ board links in Fiber 1G | Untested | none | The one unmodified run changed two things at once. Until known, a safe-mode boot on that board may be unreachable. |

### OLT lines

No OLT is named here. "OLT" below means the one on that line.

| line | state | last checked / evidence | notes |
|---|---|---|---|
| ISP1 | Maintainer-tested | v1.12.0 content (2026-10-27): O5, 6 services, flows identical to the baseline | Full OMCI provisioning. Carries no IPTV and no voice. |
| ISP2 | Maintainer-tested | v1.15.0 (2026-11-02): O5, 2 services, OMCI receive drops 0, flows identical to the baseline | PPPoE through the stick. Carries no IPTV and no voice. |
| A line that sends the OLT's VLAN in ME 171 (`VLAN_CFG_TYPE` 0) | Community-reported | [#29](https://github.com/AndreZiviani/odi-oss/issues/29): v1.4.0 | An earlier build failed there (deactivated right after O5, see the issue). |
| A line with six upstream GEM flows on three T-CONTs | Community-reported | [#42](https://github.com/AndreZiviani/odi-oss/issues/42): v1.2.2-beta.2, committed | Reproduced and fixed from the report; its own lab check is the five-flow ISP1. |
| The 2C3 reporter's line | Community-reported | [#241](https://github.com/AndreZiviani/odi-oss/issues/241): v1.14.0 | Its OLT encrypts every GEM port downstream ("Line and data path" below). |
| The LL-XS2510 reporter's line | Community-reported | [#302](https://github.com/AndreZiviani/odi-oss/issues/302): v1.15.0 and v1.16.0-beta.1 | Its OLT adds VLAN 1 to untagged frames in ME 171 with no class 84 and passes tagged ones; tagged PPPoE broke on v1.15.0 and works from v1.16.0-beta.1. It also sends the PON-ID message with the launch power ([#321](https://github.com/AndreZiviani/odi-oss/issues/321)). |
| A contributor's line | Community-reported | pull request [#228](https://github.com/AndreZiviani/odi-oss/pull/228): v1.12.0 and main, O5 and DHCP | Where a TP type 3 pointer naming a mapper and an IW TP was seen. |
| Any other OLT | Untested | none | Several OLT behaviours have been found only by a user's line (the list above is where each came from). Expect the next one. |

## Matrix B: feature paths

"ISP1" and "ISP2" are the lab sticks above. Test names are in `test/` unless
a directory is given. A feature that is **off by default** says so.

### Flashing and the safety net

| path | state | last checked / evidence | notes |
|---|---|---|---|
| Flash from stock with `fwu.sh` into the inactive slot | Maintainer-tested | the published releases; ISP2 v1.15.0 flashed from the published tarball (2026-11-02). Community: [#241](https://github.com/AndreZiviani/odi-oss/issues/241), from a newer stock version than the lab's | The lab's stock base is `V1.0-220923`. `fwu_stock_flash_test.sh`, `fwu_write_order_test.sh`, `fwu_guard_test.sh` cover the guards. |
| Trial boot (`sw_tryactive`) | Maintainer-tested | the published releases; community [#241](https://github.com/AndreZiviani/odi-oss/issues/241), [#42](https://github.com/AndreZiviani/odi-oss/issues/42) | |
| Trial self-revert, a boot that hangs or never confirms | Maintainer-tested | repeated on both sticks with the one-shot trial and the watchdog; `kill -STOP omcid` gave a watchdog reset after about 54 s and the OEM slot booted; three such resets in a row gave the safe mode (v1.6.0 candidate, 2026-10-17) | Only the omcid-heartbeat and boot-confirmation rules have been seen to reset a stick since the redesign. The memory floor rule is Host-tested (`odi_wdt_test.c`); the CPU-port NIC rule ran on a stick on v1.2.1. |
| Trial guard (reverts a trial whose management path is never seen, after 300 s) | Community-reported | [#241](https://github.com/AndreZiviani/odi-oss/issues/241): reverted by itself on a first attempt with no host link, v1.14.0. [#321](https://github.com/AndreZiviani/odi-oss/issues/321): a `hisgmii` trial whose host link passed no frames went back to stock after about seven minutes, v1.15.0 | Maintainer-tested only for the other half: armed on every trial boot and released by the first ssh login (`trial_guard_test.sh`, `odi_wdt_test.c` cover the logic). The lab has never been made to revert by it. |
| Commit (`nv commit <slot>`) | Maintainer-tested | the published releases; both copies written and read back. Community: [#42](https://github.com/AndreZiviani/odi-oss/issues/42), [#241](https://github.com/AndreZiviani/odi-oss/issues/241) | `src/nv/test/test_commit.c` fails the flash at every step. |
| Flashing from the web UI (Firmware page, Write) | Host-tested | `fwu_starter_test.sh`, `fwu_guard_test.sh` | The guards and the background job are tested; running the write on a stick from the page is recorded as not done (`SETTINGS.md`, "Not verified yet"). Flash by hand with `fwu.sh` until it is. |
| Web UI reboot | Maintainer-tested | ISP2, v1.14.0 candidate (2026-10-31): two reboots from the page, the page answering again after about 61 s | |
| Safe mode, three short boots with `HOST_AUTONEG=off` | Maintainer-tested | the build that became v1.5.0 (2026-10-15): three real power cycles gave the recovery boot (laser off, ONU in O1, key cleared, default password on the web UI and ssh); acknowledge and a normal reboot restored the saved ones | `test/safe_mode_test.sh` covers the state machine. |
| Safe mode, five short boots (the default) | Maintainer-tested, partly | the five-count path was driven by setting the counter and rebooting, not by five power cycles | Counting window is about 60 to 110 s after power-on. |
| Safe mode after three watchdog resets | Maintainer-tested | v1.6.0 candidate (2026-10-17) | |
| Safe mode when the config partition is not mounted | Host-tested | `safe_mode_test.sh` | |
| What safe mode clears: `HOST_AUTONEG=off`, a `HOST_SERDES` mode other than `fiber1g`, `OMCI_UNI_LOCK=1` | Host-tested | `safe_mode_test.sh`; only `HOST_AUTONEG` was seen on a stick | A safe-mode boot also keeps the PON off and accepts `admin`/`admin` for that boot only. |
| Safe mode with `HOST_SERDES` set, or with `OMCI_UNI_LOCK=1` | Untested | none | |
| Safe mode login: the default password only after a power cycle (`login=default`), the saved one otherwise (`login=stored`) | Maintainer-tested, partly | `safe_mode_test.sh`, every cause against every `reason=`; the confd half in the odi-ui smoke test. On a test stick (2026-11-22): the boot counter set and a reboot kept the saved login; a real power cut gave the safe mode with `admin`/`admin` on the web UI and the saved password refused, the PON off and the ssh banner line there | Seen with one power cut, not five real power cycles in a row; what a short power cut leaves in the DRAM is not measured. The web UI half of `login=stored` needs a confd release this image does not pin yet. |
| Safe mode timed exit (reboot at 1800 s unless acknowledged, waits for a firmware write) | Maintainer-tested, partly | `safe_mode_test.sh`, with a stub `reboot`, a stub `sleep` and a stand-in for the writer lock. On a test stick (2026-11-22), left unacknowledged: it rebooted by itself and the next boot was normal | Not seen on a stick: the wait behind a real `fwu.sh`, what it boots with a trial armed, and the cycle with a fault that persists. |

### Line and data path

| path | state | last checked / evidence | notes |
|---|---|---|---|
| Ranging to O5, full OMCI provisioning | Maintainer-tested | ISP1 v1.12.0 content (2026-10-27), ISP2 v1.15.0 (2026-11-02). Community: [#29](https://github.com/AndreZiviani/odi-oss/issues/29), [#42](https://github.com/AndreZiviani/odi-oss/issues/42), [#241](https://github.com/AndreZiviani/odi-oss/issues/241), [#229](https://github.com/AndreZiviani/odi-oss/issues/229) | |
| Cold boot after a power cut (GPON MAC tables start as garbage) | Maintainer-tested | ISP2, three power cuts on v1.3.0 (2026-10-12): O5 each time, tables cleaned on the first pass | `odi_gpon_init_clean_test.c` covers the sequence. |
| Internet through the stick, PPPoE | Maintainer-tested | ISP2: PPPoE session carrying the household, v1.15.0 | |
| Internet through the stick, routed or DHCP | Maintainer-tested | ISP1: v1.12.0 content | Community: #42, #241, #228. |
| IPv6 | Maintainer-tested | ISP1 delivers the household's IPv6 (DHCPv6), seen 2026-10-21 on v1.11.0 content. Community: [#241](https://github.com/AndreZiviani/odi-oss/issues/241) | |
| Fibre pull and return (LOS, LOF) | Maintainer-tested | ISP2, v1.3.0 content (2026-10-12): three pulls of 16 to 27 s, O5 to O6 to O1 and back to O5 within seconds of the light, with PPPoE back at once; `odi_gpon_los_test.c` | The state machine follows G.984.3 Table 10-1. |
| Downstream AES (Encrypted_Port-ID, key switching) | Community-reported | [#241](https://github.com/AndreZiviani/odi-oss/issues/241): the diagnostics bundle posted there shows the OLT's Encrypted_Port-ID messages for every GEM port, and the line carries traffic | Neither lab line uses it. `odi_gpon_aes_key_test.c`, `odi_switch_ds_encrypt_test.c`. Upstream key switching has not been seen from an OLT. |
| Dying Gasp (`GPON_DYING_GASP=1`, off by default) | Maintainer-tested, partly | ISP1, v1.11.0 content (2026-10-21): three copies sent before the restart and the line back to O5 | What the OLT does with them has not been seen. |
| OLT disable and O7 (`GPON_KEEP_DISABLE=1`, off by default) | Host-tested | `odi_gpon_o7_test.c`, `odi_gpon_hold_test.c`, `gpon_hold_test.sh`; on ISP1 a simulated disable, held across a restart and a reboot, v1.11.0 content (2026-10-21) | No OLT has disabled either stick. |
| OLT reboot request (`OMCI_ACCEPT_REBOOT` 0, 1, 2) | Maintainer-tested, injected | an OLT Reboot played in on a stick (v1.4.2 pre-releases) | |
| OLT software download (`OLT_SW_DOWNLOAD`) | Host-tested | omcid under qemu with a syscall trace: no flash, no exec, no reboot | |
| UNI lock by the OLT (`OMCI_UNI_LOCK`, off by default) | Maintainer-tested, injected | ISP2, v1.16.0-beta.1 content (2026-11-04): a lock and unlock played in, with the key absent (kept up) and set (port down for the lock window) | In the v1.16.0 pre-releases. Recovery by a boot with the fibre out is described in `SETTINGS.md`. |
| PON-ID message (G.984.3 Annex C: ODN class, launch power, PON identifier) | Maintainer-tested, partly | ISP1, v1.16.0-beta.3 content plus the decoding (2026-11-10): one message 14 minutes after boot, `pon_id` matching the raw PLOAM bytes, class B+, the launch power not reported by that OLT | ISP2's OLT sends none. A reported launch power (the path-loss estimate) has not been seen on a lab line; the LL-XS2510 reporter's OLT sends one ([#321](https://github.com/AndreZiviani/odi-oss/issues/321)). |
| Upstream and downstream flow handling | Maintainer-tested | flows identical to a recorded baseline after every image on both lines | Not a feature of its own; it is the check that stops a regression. |
| Management never reachable from the fibre | Maintainer-tested | ISP1, v1.6.1 content (2026-10-17): a 30 minute passive check with real PON traffic, only OMCI frames reached the CPU | Nothing was sent at the stick from the OLT side. `odi_pon_isolation_test.c`. |
| Host PAUSE off | Maintainer-tested | ISP2, v1.3.0 content (2026-10-12): the host NIC's XOFF counter stayed flat through a stall | |
| Die temperature and its alarm limits | Maintainer-tested | ISP2, v1.14.0 content (2026-10-28): limits lowered to force each alarm, logged and cleared | The scale is inferred, not calibrated. |

### Multicast and voice (all off by default)

| path | state | last checked / evidence | notes |
|---|---|---|---|
| Multicast flooding to the host | Maintainer-tested | the multicast GEM flow is set up as the OLT provisions it on both lines, flows identical to baseline | Neither line carries a multicast stream, so no stream has been forwarded. |
| IGMP snooping (`IGMP_SNOOPING=1`) | Host-tested | `odi_switch_mcast_test.c`, `odi_igmp_rx_test.c`, `svc_igmpd_test.sh`, `src/igmp/test` | On ISP1 (v1.12.0 content, 2026-10-25) the switch rule that copies or traps the host's IGMP to the CPU was run: 32 of 32 reports counted, nothing from the fibre reached the CPU. The group rows, the re-tagged upstream report and the OLT's reaction have not been seen: neither line carries IPTV. **IPTV: Untested.** |
| `IGMP_PON_DROP=1` (IPv4) | Host-tested | `odi_switch_mcast_test.c` | The line carries no IPv4 multicast, so the drop has seen none. The PON-side counting itself was proven on ISP1 against the switch's port counters (1149 of 1149 frames, 2026-10-26). |
| MLD snooping (`MLD_SNOOPING=1`), with the IPv6 drop of unjoined groups | Maintainer-tested, partly | ISP1, v1.12.0 content (2026-10-26): MLDv1 and v2 joins from a host, both groups held in igmpd and their rows written and removed per join and leave, unregistered IPv6 dropped from the PON, router and neighbour discovery unaffected | Same limit: no multicast stream from the fibre. |
| Phone ports (`OMCI_POTS_PORTS`, SIP over OMCI) | Host-tested | `voip`, `keys`, `reload` and `drv` omcid tests | Driven on ISP1 with the key on and off, and with injected voice frames, to check the model and the diagnostics bundle's masking. No OLT has provisioned voice to either lab stick, so this has not been seen end to end. A report of a router's own SIP over a data VLAN ([#229](https://github.com/AndreZiviani/odi-oss/issues/229)) does not use this path. |

### Optics, host diagnostics and operations

| path | state | last checked / evidence | notes |
|---|---|---|---|
| Optics readings (DDM) on the stick | Maintainer-tested | both sticks, the published releases | |
| Host-side diagnostics, the SoC SFP EEPROM mirror (`SFP_HOST_DDM`, on by default) on hardware v06 | Maintainer-tested | ISP2, v1.14.0 and v1.15.0: the host's view of the module EEPROM is byte for byte unchanged | On v06 the host is not wired to the mirror, so nothing is expected to change there. |
| The same on hardware v08 | Community-reported | [#231](https://github.com/AndreZiviani/odi-oss/issues/231): the router shows live diagnostics once pin 27 is freed, which the image now does at boot (v1.14.0); a second user's v08 board showed the same (`CHANGELOG.md`, v1.14.0) | The register values are the reporters'. |
| Syslog to a remote server (`SYSLOG_SERVER`) | Maintainer-tested | ISP1: set from the web UI, applied, syslogd restarted (v1.0.7), and the reconnect after a syslogd restart (v1.4.1). A contributor used it to capture a trial boot ([#228](https://github.com/AndreZiviani/odi-oss/pull/228)) | |
| NTP (`NTP_SERVER`) | Maintainer-tested | both sticks, v1.2.x (2026-10-06). Community: [#82](https://github.com/AndreZiviani/odi-oss/issues/82), [#151](https://github.com/AndreZiviani/odi-oss/issues/151) | |
| Metrics exporter | Maintainer-tested | scraped continuously from both sticks | The exporter is a separate repository with its own releases. |
| Changing the management address (90 s revert) | Maintainer-tested | ISP1, v1.11.0 content (2026-10-21) | |
| Diagnostics bundle, secrets masked | Maintainer-tested | both sticks, the published releases: the planted secrets were absent as text and hex. Community: [#229](https://github.com/AndreZiviani/odi-oss/issues/229), [#241](https://github.com/AndreZiviani/odi-oss/issues/241) | See "Adding a report". |
| Config backup and restore | Host-tested | `tools/config-backup.sh`, `src/omci/test` | The web UI's encrypted backup belongs to the web UI's repository. |

### Scope

The web UI (`confd`) and the exporter (`metricsd`) are separate projects with
their own releases and tests. Rows here that involve them say what was
checked from this image's side; their own pages are in their repositories.

## The minimum qualification suite

A hardware path (a board, a laser driver, a host or a SerDes mode) is called
**supported** only when all of the following have passed on it, on one
release, with the result written in the row. This is what the maintainer runs
before a release. A path that has not been through it can be Community-reported
at best, however many users it works for.

1. **Flash and trial.** Flash from stock into the other slot, boot it on
   trial with `sw_tryactive`, and see the userland confirmed in time. A trial
   that fails must come back to the stock slot by itself.
2. **Line.** O5 with the services the OLT provisions, for the lines the path
   is claimed on (at least one routed and, where the path claims it, one PPPoE
   line). The flows and registers match the recorded baseline for that line.
3. **Traffic.** A real transfer through the stick, with no errors on the
   host NIC or the stick.
4. **Overnight soak.** About eight hours with no reboot, O5 the whole time,
   OMCI receive drops 0, flows, the mirror and the host EEPROM view unchanged,
   host NIC error counters 0, no kernel oops, free memory flat.
5. **Reboot and power cycle.** A reboot, and a power cut for a cold boot: back
   to O5 each time, the cold boot with the GPON tables cleaned.
6. **Fibre pull.** Unplug the fibre for a few seconds to half a minute and plug
   it back: LOS, then O5 again with no reboot.
7. **Commit.** `nv commit` on the trial, both copies read back, and a reboot
   that stays on the committed slot.
8. **Host side.** For a host or SerDes mode: the link comes up at boot and
   after the host port is bounced, and for a non-default SerDes mode the
   trial reverts by itself and the boot falls back to Fiber 1G.
9. **Other link healthy first.** Before any write that can take a line down,
   the other line is checked (rx power, metrics scrape, default route).

Not part of the suite, because no lab line can give them: multicast streams,
voice from an OLT, and OLT-initiated disable, download or encryption key
switching. Those stay Host-tested or Community-reported until a user's line
shows them.

## Adding a report

Open an issue on this repository (the bug template has the fields) with:

- **Board:** model and revision as printed on the label (2C2, 2C3, HSGQ v08),
  and the stock firmware version it came with.
- **Optics:** `/proc/odi_gpon/status` shows `ldd_id <id>`; also say if `dmesg
  | grep odi_board` mentions the laser.
- **Host:** the make and model of the router or NIC, the port speed, and
  whether its port is forced or auto-negotiating.
- **SerDes mode:** the stored `HOST_SERDES` and `HOST_AUTONEG` values and
  `host-serdes.sh status`.
- **Release:** the image version (`/etc/odi-build`) and whether it was
  committed or a trial.
- **Line:** a name for it (ISP1, "my line"), not the OLT's make, and what the
  line provisions (internet, IPTV, voice).
- **What you ran:** which rows above, and what you saw. A negative result is
  as useful as a positive one.
- **A diagnostics bundle**, from the web UI (Admin, download diagnostics) or
  `/etc/scripts/diag-bundle.sh`, **without** `--full`. In that default mode
  it redacts every password, key and secret in the configuration and scrubs
  them from every file as text and hex, and it masks the serial number, the
  MACs, the LOID, the module serial and the phone numbers to `MASKED-` and
  their last four characters. The web UI never passes `--full`. Read it once
  before you attach it: the bundle holds your public address range and your
  ISP's names in logs, which are not masked.

A maintainer then adds or updates the row, with your issue linked and the
release you used.

## Keeping it up to date

The matrices above are the **last checked** view, not a claim about the
latest release. Keep that view; additionally record release-specific
evidence in [RELEASE-QUALIFICATION.md](RELEASE-QUALIFICATION.md). Each
record binds one hardware/line combination and its observed feature
results to the exact released commit (or identifies a non-release build
separately). A release does not inherit a previous release's passes.
The checklist above is a guide to what remains to be verified, not a
requirement to run unneeded or unsafe field tests merely to publish a record.

When reporting a release, identify the immutable release commit, then
append only checks actually run on it. Record untested/not applicable
checks as such; missing details remain unknown. The existing matrices
continue to tell the reader the newest evidence for each hardware and
feature path.

Change a row when:

- a check on a stick is run (update "last checked" and the state);
- a user report is received or retracted (add or remove the link);
- a release changes a path a row covers in a way its evidence does not
  cover (set it back to Host-tested until it is run again).

A row's state never goes up without a run or a linked report behind it.
