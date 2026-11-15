#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The odi-oss contributors
# SPDX-License-Identifier: GPL-2.0-or-later

"""Hold src/omci/model/g988.json against omci-lib-go's G.988 definitions.

g988.json is extracted from the Recommendation by our own script;
omci-lib-go is generated from it by an unrelated parser. Where the two
agree, an extraction error is unlikely. Every field that differs must be
listed in KNOWN with its reason; anything else is reported and fails.
"""
import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# omci-lib-go v2.2.4 predates these additions to the Recommendation.
NEWER = 'added after the edition omci-lib-go was generated from'
OPEN = 'open: check the clause in the PDF'
KNOWN = {
    (11, 16, 'missing'): NEWER,
    (148, 6, 'missing'): NEWER,
    (148, 7, 'missing'): NEWER,
    (148, 8, 'missing'): NEWER,
    (153, 13, 'missing'): NEWER,
    (280, 9, 'missing'): NEWER,
    (330, None, 'missing'): 'class not defined in omci-lib-go',
    (6, 8, 'requirement'): OPEN,
    (134, 16, 'avc'): OPEN,
    (310, 7, 'requirement'): OPEN,
}

ACCESS = (('R', 'R'), ('W', 'W'), ('SBC', 'C'))


def access(fact):
    return ''.join(lib for name, lib in ACCESS if name in fact['access'])


def compare(facts, lib):
    lib = {c['classId']: {a['index']: a for a in c['attrs']} for c in lib}
    diffs = []
    for cls in facts['classes']:
        cid = cls['classId']
        attrs = lib.get(cid)
        if attrs is None:
            diffs.append((cid, None, 'missing', 'class', None))
            continue
        for fact in cls['attrs']:
            idx = fact['index']
            other = attrs.get(idx)
            if other is None:
                diffs.append((cid, idx, 'missing', 'attribute', None))
                continue
            pairs = [('access', access(fact), other['access']),
                     ('table', fact['table'], other['table']),
                     ('avc', fact['avc'], other['avc']),
                     ('requirement', fact['requirement'].startswith('O'), other['optional'])]
            # A table attribute's width is one row in g988.json; omci-lib-go
            # gives a variable size there.
            if not fact['table'] and other['size'] > 0:
                pairs.append(('size', fact['size'], other['size']))
            diffs += [(cid, idx, f, a, b) for f, a, b in pairs if a != b]
    return diffs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('lib', type=Path, help='output of the omci-libcheck exporter')
    parser.add_argument('--facts', type=Path, default=ROOT / 'src/omci/model/g988.json')
    args = parser.parse_args()
    facts = json.loads(args.facts.read_text())
    diffs = compare(facts, json.loads(args.lib.read_text()))
    unknown = 0
    for cid, idx, field, ours, theirs in diffs:
        reason = KNOWN.get((cid, idx, field))
        where = f'class {cid}' + (f' attribute {idx}' if idx is not None else '')
        if reason is None:
            unknown += 1
            print(f'DIFF   {where} {field}: g988.json {ours}, omci-lib-go {theirs}')
        else:
            print(f'known  {where} {field}: {reason}')
    n = sum(len(c['attrs']) for c in facts['classes'])
    print(f'{len(facts["classes"])} classes, {n} attributes, '
          f'{len(diffs)} differences, {unknown} not explained')
    stale = set(KNOWN) - {(c, i, f) for c, i, f, _, _ in diffs}
    for key in sorted(stale, key=str):
        print(f'stale  KNOWN entry {key}: no longer differs, remove it')
    sys.exit(1 if unknown or stale else 0)


if __name__ == '__main__':
    main()
