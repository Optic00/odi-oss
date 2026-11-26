# Release qualification records

This is the release-specific evidence ledger for
[QUALIFICATION.md](QUALIFICATION.md). A row is **one combination on one exact
released commit**. It is not a compatibility claim for every stick of that
model or for another host, line, configuration, or release. No tests were
run to prepare the initial historical rows: they transcribe existing
qualification evidence, not new field results.

## Recording a release

- Record the release tag and its peeled commit SHA (the commit, not the
  annotated tag object). For local or downstream builds record the actual
  commit, local modifications and image identifier separately; never attach
  their results to the upstream tag merely because the version looks similar.
- Record board revision, laser/optics, host and port setting, SerDes mode,
  anonymized OLT/ISP and line services, and trial/committed state where known.
  Use `unknown` rather than inferring a missing field.
- Classify evidence as Maintainer-tested, Community-reported, Host-tested or
  Untested, using the definitions in QUALIFICATION.md. Only the maintainer's
  completed minimum suite on one release can qualify a hardware path as
  supported. A host unit test cannot stand in for hardware.
- Each minimum-suite result is `pass`, `fail`, `not run`, `not reported`
  (historical evidence lacks a result), or `not applicable` with a reason.
  Evidence of a successful feature is not evidence for a separate feature.
- Record a reproducible reference: a public redacted diagnostic report or
  a test name/result with date; for private lab evidence, the anonymized
  line name, date and reference in QUALIFICATION.md. Do not include serials,
  MACs, LOIDs, passwords, subscriber IDs, public address ranges or raw
  diagnostic bundles. Review any artifact before sharing separately.

## Historical evidence (partial)

The tag-to-commit mapping below identifies the **reported release**, not an
independently read-back binary from a reporter. The peeled SHA was resolved
from the upstream release tag. Sources are the matching rows of
[QUALIFICATION.md](QUALIFICATION.md) and their linked reports.

| Release | Commit SHA | Board / optics | Host / SerDes | Line | State | Observed only | Missing or not established |
|---|---|---|---|---|---|---|---|
| v1.14.0 | [b8c6c94dff6b28290f96693c2ebd15d28006a3ad](https://github.com/AndreZiviani/odi-oss/commit/b8c6c94dff6b28290f96693c2ebd15d28006a3ad) | 2C3, SC/APC; revision unknown; laser id 0x6a | UniFi gateway, forced 1G; `HOST_AUTONEG=off`; exact SerDes value not reported | anonymized community line, downstream AES | Community-reported | stock installation, trial, committed; O5, OMCI, IPv4 and IPv6 ([#241](https://github.com/AndreZiviani/odi-oss/issues/241)) | full minimum suite, timed self-revert, soak duration, recovery |
| v1.15.0 | [c49c35b4fe7b37bf1d9b34286f2f99b028083918](https://github.com/AndreZiviani/odi-oss/commit/c49c35b4fe7b37bf1d9b34286f2f99b028083918) | 2C2 v06; GN25L95 | x86 / Intel X520 SFP+, 1G; exact SerDes setting not reported | ISP2 (PPPoE; no IPTV/voice) | Maintainer-tested, partly | 2026-11-02: O5, two services, no OMCI receive drops, flows at baseline, PPPoE; X520 error counters zero during soak | remaining minimum suite results for this exact release not reported; IPTV and voice not run |
| v1.15.0 | [c49c35b4fe7b37bf1d9b34286f2f99b028083918](https://github.com/AndreZiviani/odi-oss/commit/c49c35b4fe7b37bf1d9b34286f2f99b028083918) | 2C2 v08 / HSGQ; laser id 0x6a | MikroTik RB5009, 2.5G, `2500basex` | anonymized community line | Community-reported | O5 and router diagnostics for about three hours ([#229](https://github.com/AndreZiviani/odi-oss/issues/229), [#231](https://github.com/AndreZiviani/odi-oss/issues/231)) | eight-hour soak, Fiber 1G, complete minimum suite not established |

The release tag v1.19.0 resolves to [e5dbb6bad7c5b84cc5766f44c100c7c09b5cba4d](https://github.com/AndreZiviani/odi-oss/commit/e5dbb6bad7c5b84cc5766f44c100c7c09b5cba4d).
No complete board/host/line qualification record for that exact release has
been transcribed into this ledger. This is missing evidence, not a failed
test or a claim that no tests were performed.

Do not combine observations across community reports into a synthetic
fully-qualified row. In particular, a router carrying IPTV or SIP is not
evidence of on-stick multicast or voice operation.

## Blank record (for the next release)

Copy this record for each distinct board/host/line configuration. Fill only
observed results; no entry here claims a field test has been performed.

| Field | Value |
|---|---|
| Release tag / peeled commit SHA / image identifier | unknown |
| Date / evidence class / redacted evidence reference | Untested |
| Board model / revision / stock version | unknown |
| Optical connector / laser driver ID | unknown |
| Host make/model / port mode / speed | unknown |
| SerDes mode / auto-negotiation | unknown |
| Anonymized ISP/OLT and services | unknown |
| Trial or committed slot | unknown |

| Minimum suite (QUALIFICATION.md) | Result | Evidence / limitations |
|---|---|---|
| Flash, trial, confirmation and failed trial self-revert | not run | |
| O5, provisioning and baseline flow match | not run | |
| Traffic and error counters | not run | |
| Overnight soak | not run | |
| Reboot and power cut | not run | |
| Fibre pull and recovery | not run | |
| Commit and reboot into committed slot | not run | |
| Host link, rebound, fallback as applicable | not run | |
| Independent other link healthy before writes | not run | |
| Optional features (IPTV/IGMP, MLD, voice, encryption and others) | not run | record separately per feature |

Qualification decision: **not established**. A reviewer must explicitly
record the supporting results before changing it.
