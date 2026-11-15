#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The odi-oss contributors
# SPDX-License-Identifier: GPL-2.0-or-later

# Check source/output drift, pinned compatibility and invalid profile mutations.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TOOL="$ROOT/src/omci/tools/mib-gen.py"
GEN=${MIB_GEN_OUTPUT:-"$ROOT/src/omci/generated"}
python3 -I -B "$TOOL" --check --output-dir "$GEN"
python3 -I -B - "$ROOT" "$GEN" <<'PY'
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

root, generated = map(Path, sys.argv[1:])
tool = root / 'src/omci/tools/mib-gen.py'
spec = importlib.util.spec_from_file_location('mib_gen_test', tool)
gen = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gen)
facts = gen.read_json(gen.MODEL / 'g988.json')
profile = gen.read_json(gen.MODEL / 'profile.json')
model, identifiers = gen.build_model(facts, profile)
assert len(model) == 99 and sum(len(e['attrs']) for e in model) == 941
assert len(facts['classes']) == 75
assert len(profile['legacyClasses']) == 1
assert len(profile['privateClasses']) == 23
assert sum(len(c.get('privateAttrs', [])) for c in profile['classes']) == 12

# The reference revision supplies hashes, not a second copy of model tables.
c = (generated / 'omci_mib.c').read_bytes().split(b'*/', 1)[1]
h = re.sub(rb'/\*.*?\*/', b'', (generated / 'omci_mib.h').read_bytes(), flags=re.S)
assert hashlib.sha256(c).hexdigest() == 'bc9b6d3ebcf4d484b956f055aca836cbc7a110dfbf808c4f3e4e6d45281c326e'
assert hashlib.sha256(h).hexdigest() == '09f7bcd635864686004b4b596e33073a6aed45273f1a3e5b2230967e47e2d5b6'
# Raw bytes also pin key order and formatting, not only values.
j = (generated / 'omci_mib.json').read_bytes()
assert hashlib.sha256(j).hexdigest() == '819d4ef6a187c82a2ce4e4fbfb146e63a5515c956f63385db976d7d6e3729214'
print('ok    model tables, ABI and JSON contract match the pinned reference')

with tempfile.TemporaryDirectory() as tmp:
    first, second = Path(tmp) / 'first', Path(tmp) / 'second'
    for output in (first, second):
        subprocess.run([sys.executable, '-I', '-B', str(tool), '--output-dir', str(output)],
                       check=True, timeout=30)
    for name in ('omci_mib.c', 'omci_mib.h', 'omci_mib.json'):
        assert (first / name).read_bytes() == (second / name).read_bytes()
    # Drift detection runs on a changed temporary output, never the checkout.
    with (second / 'omci_mib.c').open('a') as stream:
        stream.write('\n')
    result = subprocess.run([sys.executable, '-I', '-B', str(tool), '--check',
                             '--output-dir', str(second)], capture_output=True, timeout=30)
    assert result.returncode == 1 and b'generated output differs' in result.stderr
    changed_profile = copy.deepcopy(profile)
    changed_profile['source']['metadataProbe'] = 'Metadata-only source change.'
    profile_path = Path(tmp) / 'profile.json'
    profile_path.write_text(json.dumps(changed_profile, indent=2) + '\n')
    result = subprocess.run([sys.executable, '-I', '-B', str(tool), '--check',
                             '--profile', str(profile_path), '--output-dir', str(first)],
                            capture_output=True, timeout=30)
    assert result.returncode == 1 and b'generated output differs' in result.stderr
    changed_facts = copy.deepcopy(facts)
    changed_facts['source']['metadataProbe'] = 'Metadata-only source change.'
    facts_path = Path(tmp) / 'g988.json'
    facts_path.write_text(json.dumps(changed_facts, indent=2) + '\n')
    result = subprocess.run([sys.executable, '-I', '-B', str(tool), '--check',
                             '--facts', str(facts_path), '--output-dir', str(first)],
                            capture_output=True, timeout=30)
    assert result.returncode == 1 and b'generated output differs' in result.stderr
    # Invalid input fails through the CLI with a message, not a traceback.
    broken_profile = copy.deepcopy(profile)
    broken_profile['privateClasses'][0]['attrs'] = []
    profile_path.write_text(json.dumps(broken_profile, indent=2) + '\n')
    result = subprocess.run([sys.executable, '-I', '-B', str(tool), '--validate-only',
                             '--profile', str(profile_path)], capture_output=True, timeout=30)
    assert result.returncode == 1 and result.stderr.startswith(b'mib-gen: '), result.stderr
    assert b'Traceback' not in result.stderr
print('ok    deterministic generation and output/source drift detection')

# Tiny independent inputs make failures about validation rather than baseline size.
small_facts = {'source': {'obsoleteClassIds': []}, 'classes': [{
    'classId': 2, 'clause': '9.1.3', 'createdBy': 'onu', 'actions': ['get'],
    'attrs': [{'index': 0, 'size': 2, 'access': ['R'], 'requirement': 'M',
               'avc': False, 'table': False, 'pm': False, 'kind': 'uint'}]}]}
small_profile = {'classOrder': [2], 'privateClasses': [], 'legacyClasses': [], 'classes': [{
    'classId': 2, 'ident': 'Synthetic', 'stockBacked': False, 'name': 'Synthetic',
    'shortName': 'SYNTHETIC', 'desc': 'Synthetic input', 'entrySize': 2, 'stdType': 1,
    'overrides': [], 'privateAttrs': [],
    'attrs': [{'index': 0, 'name': 'EntityID', 'mibSave': 0, 'outStyle': None,
               'overrides': []}]}]}
gen.build_model(small_facts, small_profile)


def rejected(label, mutate, expected):
    f, p = copy.deepcopy(small_facts), copy.deepcopy(small_profile)
    mutate(f, p)
    try:
        gen.build_model(f, p)
    except ValueError as error:
        assert expected in str(error), (label, str(error))
    else:
        raise AssertionError(label + ' was accepted')
    print('ok    rejects ' + label)


rejected('ineffective override', lambda f, p: p['classes'][0]['attrs'][0]['overrides'].append(
    {'field': 'size', 'value': 2, 'class': 'REPRESENTATION', 'reason': 'No change.'}), 'Ineffective')
rejected('override without reason', lambda f, p: p['classes'][0]['attrs'][0]['overrides'].append(
    {'field': 'size', 'value': 3, 'class': 'REPRESENTATION'}), 'lacks a reason')
rejected('unknown attribute key', lambda f, p: p['classes'][0]['attrs'][0].update(dataType=1),
         'Unknown profile attribute key')
rejected('unknown class key', lambda f, p: p['classes'][0].update(reason='Synthetic.'),
         'Unknown profile class key')
rejected('ineffective len', lambda f, p: p['classes'][0]['attrs'][0].update(len=2),
         'Ineffective len')
rejected('unknown attribute', lambda f, p: p['classes'][0]['attrs'][0].update(index=1),
         'Unknown or duplicate profile attribute')
rejected('zero width', lambda f, p: f['classes'][0]['attrs'][0].update(size=0), 'positive')
rejected('attribute index hole', lambda f, p: f['classes'][0]['attrs'][0].update(index=1), 'contiguous')
rejected('undersized row', lambda f, p: p['classes'][0].update(entrySize=1), 'exceed the row')
rejected('unclassified override', lambda f, p: p['classes'][0]['attrs'][0]['overrides'].append(
    {'field': 'size', 'value': 3, 'reason': 'Synthetic change.'}), 'classification')
rejected('duplicate class order', lambda f, p: p.update(classOrder=[2, 2]), 'order')
rejected('unknown override field', lambda f, p: p['classes'][0]['attrs'][0]['overrides'].append(
    {'field': 'bogus', 'value': 1, 'class': 'SUSPECT', 'reason': 'Synthetic change.'}),
    'Unknown or duplicate override field')
rejected('out-of-range class flags', lambda f, p: p['classes'][0].update(stdType=0x10000),
         'class flags')
rejected('empty attribute name', lambda f, p: p['classes'][0]['attrs'][0].update(name=''),
         'printable ASCII')
rejected('legacy class without obsolete identifier', lambda f, p: p['legacyClasses'].append(
    {'reason': 'Synthetic legacy input.', 'ident': 'Legacy', 'stockBacked': False,
     'table': {'classId': 267}, 'attrs': []}), 'obsolete identifier')


def private_extension(option, is_index):
    def mutate(f, p):
        p['classes'][0]['entrySize'] = 3
        p['classes'][0]['privateAttrs'].append({'reason': 'Synthetic extension.', 'attribute': {
            'index': 1, 'name': 'Private', 'dataType': None, 'len': 1, 'isIndex': is_index,
            'mibSave': 0, 'outStyle': None, 'oltAcc': 1, 'avcFlag': 0,
            'optionType': option, 'type': 'u8', 'size': 1}})
    return mutate


SYNTHETIC_PRIVATE = {
    'reason': 'Synthetic private class.', 'ident': 'SyntheticPrivate', 'stockBacked': False,
    'table': {'name': 'Private', 'shortName': 'PRIVATE', 'desc': 'Synthetic private class',
              'classId': 65280, 'initType': 0, 'stdType': 0x10, 'actionType': 1 << 9,
              'attrNum': 1, 'entrySize': 2, 'actions': ['get']},
    'attrs': [{'index': 0, 'name': 'EntityID', 'dataType': None, 'len': 2, 'isIndex': 1,
               'mibSave': 0, 'outStyle': None, 'oltAcc': 1, 'avcFlag': 0,
               'optionType': 0x001, 'type': 'u16', 'size': 2}]}


def private_class(mutate_class):
    def mutate(f, p):
        config = copy.deepcopy(SYNTHETIC_PRIVATE)
        mutate_class(config)
        p['privateClasses'].append(config)
        p['classOrder'].append(config['table']['classId'])
    return mutate


f, p = copy.deepcopy(small_facts), copy.deepcopy(small_profile)
private_class(lambda c: None)(f, p)
gen.build_model(f, p)
rejected('private class ID beyond 16 bits', private_class(
    lambda c: c['table'].update(classId=65536)), 'reserved ranges')
rejected('private class without attributes', private_class(
    lambda c: c.update(attrs=[])), 'at least its entity index')
rejected('fixed-width size mismatch', private_class(
    lambda c: c['attrs'][0].update(size=1)), 'Fixed-width')
rejected('unmarked private extension', private_extension(0x004, 0), 'explicitly private')
rejected('second index in a standard class', private_extension(0x014, 1), 'Only attribute 0')
f, p = copy.deepcopy(small_facts), copy.deepcopy(small_profile)
private_extension(0x014, 0)(f, p)
gen.build_model(f, p)
with tempfile.TemporaryDirectory() as tmp:
    duplicate = Path(tmp) / 'duplicate.json'
    duplicate.write_text('{"a": 1, "a": 2}\n')
    try:
        gen.read_json(duplicate)
    except ValueError as error:
        assert 'Duplicate JSON key' in str(error)
    else:
        raise AssertionError('duplicate JSON key was accepted')
print('ok    rejects duplicate JSON keys')
print('ok    schema and compatibility checks')
PY
