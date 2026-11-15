# omci-libcheck

A second, independent reading of ITU-T G.988 for `src/omci/model/g988.json`.

`g988.json` comes from our own extraction of the Recommendation
(`src/omci/tools/g988-extract.py`). [omci-lib-go](https://github.com/opencord/omci-lib-go)
(ONF, Apache-2.0) generates its managed-entity definitions from G.988 with an
unrelated parser. Where both agree on an attribute's width, access,
requirement, AVC and table flag, an extraction error on either side is
unlikely. Where they differ, one of them is wrong or they read different
editions.

This is a developer tool. Nothing here is built into the image, and the
library is not linked into anything that ships. It needs Go and network
access to fetch the module, so it is not part of `make test`.

    cd tools/omci-libcheck
    go run . > /tmp/omci-lib.json
    ./check.py /tmp/omci-lib.json

`main.go` writes every class omci-lib-go defines as JSON. `check.py` compares
them with `g988.json` and fails on any difference not listed in its `KNOWN`
table, and on a `KNOWN` entry that no longer differs.

## Result with omci-lib-go v2.2.4

75 classes and 746 attributes, 10 differences:

- 6 attributes and class 330 that G.988 added after the edition the library
  was generated from.
- 3 that need a look at the clause: class 6 attribute 8 and class 310
  attribute 7 are optional in `g988.json` and mandatory in the library;
  class 134 attribute 16 has an AVC in the library and none in `g988.json`.

`mib-compare.py` marks 59 attribute fields of the stock baseline as SUSPECT
against `g988.json`. 50 of them can be held against the library (it lacks
class 330, and a table's row width and the stored type are not comparable),
and the library agrees with `g988.json` on all 50.
