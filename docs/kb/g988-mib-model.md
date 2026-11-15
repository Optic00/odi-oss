# The OMCI model is generated from G.988 facts and a device profile

`src/omci/generated/omci_mib.*` no longer needs a stock root filesystem to regenerate: about 95% of the attribute fields of the standard classes follow from ITU-T G.988 alone, and the rest is an explicit, reviewable profile.

*Last verified: 2026-11-15*

---

## What

`src/omci/tools/mib-gen.py` builds the three generated files from two checked-in sources.

- `src/omci/model/g988.json` holds facts of G.988 (11/2022) with Amendment 1 (03/2024) and Amendment 2 (05/2025): 75 classes and 746 attributes, each with its clause. The prepublished Amendment 3 (08/2026) is not public and is not covered. The table carries numbers and flags, no wording of the Recommendation; the PDF is a developer input and is never stored here.
- `src/omci/model/profile.json` holds what G.988 does not define: which classes the device implements and in which order, the names the CLI output depends on, row sizes and class flags, the 23 private classes, the private attributes inside standard classes, and class 267, whose identifier G.988 lists as obsolete without a definition.

The generator derives type, stored size, OLT access, index, AVC and option flags from the fact table. A profile override replaces a derived value and must name a classification and a reason:

| classification | meaning |
|---|---|
| `REPRESENTATION` | same bytes on the wire, another storage type |
| `SUPPORT` | the device carries the attribute but reports it unsupported |
| `SPEC_NEWER` | the current G.988 defines attributes or actions the device does not carry |
| `VENDOR_DEVIATION` | a deliberate difference that operation against a real OLT depends on |
| `SUSPECT` | a difference from G.988 with no known reason, kept unchanged |

## Why it matters

Of the 4554 attribute fields of the 76 standard classes (six fields for each of 759 attributes), 4322 match the pure derivation, 94.91%. The ten attributes of class 267 and the twelve private attributes count as profile values in that figure. The names are not from G.988: they are the ones the earlier model carried.

65 fields are `SUSPECT`. They are candidates for a check against an OLT, not known errors, and changing one changes what omcid puts on the wire.

## Evidence

`src/omci/tools/mib-compare.py` reports every field with its classification. `test/omci_mib_gen_test.sh` (in `make test-host`) fails when the generated files differ from what the sources produce.

## See also

- [The stock firmware's OMCI information model](rtl9601-omci-mib-model.md)
