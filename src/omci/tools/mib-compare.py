#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The odi-oss contributors
# SPDX-License-Identifier: GPL-2.0-or-later

"""Compare pure specification derivation with an explicit compatibility baseline."""
import argparse
import importlib.util
import json
from collections import Counter
from pathlib import Path

SPEC = importlib.util.spec_from_file_location('mib_gen', Path(__file__).with_name('mib-gen.py'))
GEN = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GEN)

# Earliest inspected source containing the omitted attribute, where established.
NEWER = {11: 'G.988 (11/2022)', 148: 'Amendment 2 (05/2025)',
         153: 'Amendment 2 (05/2025)', 171: 'G.988 (11/2022)',
         280: 'Amendment 2 (05/2025)'}


def classify(field, expected, actual, fact=None, base=None):
    if expected == actual:
        return 'MATCH', 'Equal without a profile override.'
    if expected is None:
        return 'SUPPORT', 'Explicit legacy or private compatibility data, outside current standard fields.'
    if field == 'optionType' and actual & 0x00a:
        return 'SUPPORT', 'The baseline marks this attribute unsupported; preserve its support flags.'
    if field in ('size', 'type') and fact is not None and base is not None:
        baseline_width = base['size'] - (base['type'] == 'string')
        if baseline_width == fact['size']:
            return 'REPRESENTATION', 'The wire width is equal; preserve the baseline storage representation.'
    return 'SUSPECT', 'The baseline differs from the cited clause without a documented operational justification; preserve it.'


def compare(facts, baseline):
    GEN.validate_facts(facts)
    standard = {c['classId']: c for c in facts['classes']}
    rows, class_rows = [], []
    totals = Counter()
    matched = Counter()
    first_stone = []
    for entry in baseline:
        table, attrs = entry['table'], entry['attrs']
        class_id = table['classId']
        if GEN.is_private_class_id(class_id):
            continue
        source = standard.get(class_id)
        pure = GEN.derive_class(source) if source else None
        clause = source['clause'] if source else '11.2.4-1 (obsolete identifier only)'
        for attr in attrs:
            index = attr['index']
            private = bool(attr['optionType'] & 0x10)
            fact = source['attrs'][index] if source and index < len(source['attrs']) and not private else None
            derived = pure['attrs'][index] if fact else None
            for field in GEN.ATTR_FIELDS:
                expected = derived[field] if derived else None
                actual = attr[field]
                category, reason = classify(field, expected, actual, fact, attr)
                rows.append({'classId': class_id, 'clause': clause, 'index': index,
                             'field': field, 'derived': expected, 'baseline': actual,
                             'class': category, 'reason': reason})
                totals[field] += 1
                matched[field] += category == 'MATCH'
        if source:
            standard_indices = [a['index'] for a in attrs if not a['optionType'] & 0x10]
            fields = {'attrNum': (len(source['attrs']), len(attrs)),
                      'attributeOrder': (list(range(len(source['attrs']))), [a['index'] for a in attrs]),
                      'standardAttributes': (list(range(len(source['attrs']))), standard_indices),
                      'actionType': (pure['actionType'], table['actionType']),
                      'initType': (pure['initType'], table['initType'])}
            for field, (expected, actual) in fields.items():
                category, reason = classify(field, expected, actual)
                if expected != actual and field in {'attrNum', 'attributeOrder', 'standardAttributes'}:
                    if class_id in NEWER:
                        category = 'SPEC_NEWER'
                        reason = 'Additional standard attributes occur in ' + NEWER[class_id] + '; preserve the baseline selection and private extensions.'
                    else:
                        category = 'SUPPORT'
                        reason = 'Preserve the baseline selection and explicit private extensions; additional optional standard attributes are not implemented.'
                if field == 'actionType' and expected != actual:
                    missing = expected & ~actual
                    if not actual & ~expected and (class_id == 6 or missing == 1 << GEN.MESSAGE_TYPES['set-table']):
                        category = 'SUPPORT'
                        reason = 'The cited clause permits optional actions that the baseline does not implement.'
                class_rows.append({'classId': class_id, 'clause': clause, 'field': field,
                                   'derived': expected, 'baseline': actual,
                                   'class': category, 'reason': reason})
        if entry['plugin'] is None:
            first_stone.append(class_id)
    count = sum(totals.values())
    same = sum(matched.values())
    return {'coverage': {'matched': same, 'total': count, 'percent': 100 * same / count,
                         'fields': {f: {'matched': matched[f], 'total': totals[f],
                                        'percent': 100 * matched[f] / totals[f]}
                                    for f in GEN.ATTR_FIELDS}},
            'attributes': rows, 'classes': class_rows,
            'g988OnlyClassIds': sorted(first_stone)}


def markdown(result):
    coverage = result['coverage']
    lines = ['# Specification-to-baseline deviations', '',
             'Pure derivation, before any profile override. Amendment 3 is excluded.', '',
             'The denominator includes all 759 attributes of the 76 nominally standard',
             'baseline classes, including legacy class 267 and private extensions.',
             'Each has six compared attribute fields. Class fields are reported separately.', '',
             '| Field | Equal without override | Total | Percent |', '|---|---:|---:|---:|']
    for f, c in coverage['fields'].items():
        lines.append(f"| {f} | {c['matched']} | {c['total']} | {c['percent']:.2f}% |")
    lines += [f"| All attribute fields | {coverage['matched']} | {coverage['total']} | {coverage['percent']:.2f}% |", '',
              '## Field differences', '',
              '| Class | Clause | Attribute | Field | Pure derivation | Baseline | Classification | Reason |',
              '|---:|---|---:|---|---|---|---|---|']
    for row in result['attributes'] + result['classes']:
        if row['class'] == 'MATCH':
            continue
        vals = [row['classId'], row['clause'], row.get('index', ''), row['field'],
                json.dumps(row['derived']), json.dumps(row['baseline']), row['class'], row['reason']]
        lines.append('| ' + ' | '.join(str(v).replace('|', '/') for v in vals) + ' |')
    lines += ['', '## Suspect wire effects', '',
              'Every SUSPECT remains unchanged in this migration. A size difference can',
              'change payload width or shift following fields; differing creation access',
              'can change Create payload layout. AVC flags affect notifications and',
              'option flags affect upload eligibility. Action-mask differences affect',
              'accepted requests. Type differences with unequal widths need the same',
              'payload review as size differences. Entity-specific table handling can',
              'qualify these effects, so no observed production failure is inferred.', '',
              '## Existing specification-declared classes', '',
              'The 18 pre-existing specification-declared class IDs are: ' +
              ', '.join(str(c) for c in result['g988OnlyClassIds']) + '.', '',
              'Their differences, if any, are included above with the same classification',
              'rules; they are not assumed correct merely because of their origin.', '']
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--facts', type=Path, default=GEN.MODEL / 'g988.json')
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--json', type=Path, required=True)
    parser.add_argument('--markdown', type=Path, required=True)
    args = parser.parse_args()
    try:
        result = compare(GEN.read_json(args.facts), GEN.read_json(args.baseline))
        args.json.write_text(json.dumps(result, indent=2) + '\n')
        args.markdown.write_text(markdown(result))
        print(f"{result['coverage']['percent']:.2f}% of attribute fields match without overrides")
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, 'mib-compare: ' + str(error) + '\n')


if __name__ == '__main__':
    main()
