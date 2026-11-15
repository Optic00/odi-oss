#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The odi-oss contributors
# SPDX-License-Identifier: GPL-2.0-or-later

"""Generate the compatibility MIB from specification facts and a device profile."""
import argparse
import hashlib
import json
import re
from pathlib import Path

MODEL = Path(__file__).resolve().parents[1] / 'model'
GENERATED = MODEL.parent / 'generated'
MESSAGE_TYPES = {
    'create': 4, 'create-complete': 5, 'delete': 6, 'delete-complete': 7,
    'set': 8, 'get': 9, 'get-complete': 10, 'get-all-alarms': 11,
    'get-all-alarms-next': 12, 'mib-upload': 13, 'mib-upload-next': 14,
    'mib-reset': 15, 'alarm': 16, 'avc': 17, 'test': 18,
    'start-sw-download': 19, 'download-section': 20, 'end-sw-download': 21,
    'activate-sw': 22, 'commit-sw': 23, 'sync-time': 24, 'reboot': 25,
    'get-next': 26, 'test-result': 27, 'get-current-data': 28, 'set-table': 29,
}
ATTR_FIELDS = ('size', 'type', 'oltAcc', 'isIndex', 'avcFlag', 'optionType')
C_TYPES = {t: 'OMCI_' + t.upper() for t in
           ('u8', 'u16', 'u32', 'u64', 'string', 'octets')}
TYPE_CODES = {t: i for i, t in enumerate(('u8', 'u16', 'u32', 'u64', 'string', 'octets'))}
CLASS_KEYS = {'classId', 'ident', 'stockBacked', 'name', 'shortName', 'desc', 'entrySize',
              'stdType', 'overrides', 'attrs', 'privateAttrs'}
ATTR_KEYS = {'index', 'name', 'len', 'mibSave', 'outStyle', 'overrides'}
CATEGORIES = {'REPRESENTATION', 'SUPPORT', 'SPEC_NEWER', 'VENDOR_DEVIATION', 'SUSPECT'}


# u8 also stores byte arrays; wider integers must keep their exact width.
FIXED_WIDTHS = {'u16': 2, 'u32': 4, 'u64': 8}


def is_private_class_id(class_id):
    return type(class_id) is int and (240 <= class_id <= 255 or 350 <= class_id <= 399 or
                                      65280 <= class_id <= 65535)


def is_printable_name(value):
    return isinstance(value, str) and value != '' and all(' ' <= c <= '~' for c in value)


def unique_object(pairs):
    obj = {}
    for key, value in pairs:
        if key in obj:
            raise ValueError('Duplicate JSON key: ' + key)
        obj[key] = value
    return obj


def read_json(path):
    return json.loads(Path(path).read_text(), object_pairs_hook=unique_object)


def action_mask(actions):
    if len(set(actions)) != len(actions) or any(a not in MESSAGE_TYPES for a in actions):
        raise ValueError('Duplicate or unknown action')
    return sum(1 << MESSAGE_TYPES[a] for a in actions)


def validate_facts(facts):
    ids = []
    for entry in facts['classes']:
        class_id = entry['classId']
        ids.append(class_id)
        if type(class_id) is not int or not 0 < class_id <= 65535:
            raise ValueError('Invalid standard class ID')
        if not re.fullmatch(r'9\.\d+\.\d+', entry['clause']):
            raise ValueError('Invalid clause reference')
        if entry['createdBy'] not in ('onu', 'olt'):
            raise ValueError('Invalid creation model')
        action_mask(entry['actions'])
        attrs = entry['attrs']
        if not attrs or len(attrs) > 17:
            raise ValueError('Standard attribute mask exceeds 16 bits')
        if any(type(a['index']) is not int for a in attrs) or \
                [a['index'] for a in attrs] != list(range(len(attrs))):
            raise ValueError('Standard attribute indices must be contiguous')
        for attr in attrs:
            if type(attr['size']) is not int or not 0 < attr['size'] <= 65535:
                raise ValueError('Attribute size must be positive')
            if not attr['access'] or len(set(attr['access'])) != len(attr['access']) or \
                    not set(attr['access']) <= {'R', 'W', 'SBC'}:
                raise ValueError('Invalid attribute access')
            if attr['requirement'] not in ('M', 'O'):
                raise ValueError('Invalid attribute requirement')
            if attr['kind'] not in {'uint', 'sint', 'string', 'bitfield', 'pointer',
                                    'enum', 'table', 'octets'}:
                raise ValueError('Invalid attribute kind')
            if any(type(attr[f]) is not bool for f in ('avc', 'table', 'pm')):
                raise ValueError('Attribute flags must be booleans')
    if ids != sorted(set(ids)):
        raise ValueError('Standard class IDs must be sorted and unique')


def derive_attribute(attr):
    kind, width = attr['kind'], attr['size']
    if kind == 'string':
        data_type = 'string'
    elif kind in {'uint', 'sint', 'bitfield', 'pointer', 'enum'} and width in (1, 2, 4, 8):
        data_type = 'u' + str(width * 8)
    else:
        data_type = 'octets'
    option = (1 if attr['requirement'] == 'M' else 4)
    option |= (0x100 if attr['pm'] else 0) | (0x200 if attr['table'] else 0)
    return {'index': attr['index'], 'type': data_type,
            'size': width + (data_type == 'string'),
            'oltAcc': sum({'R': 1, 'W': 2, 'SBC': 4}[a] for a in attr['access']),
            'isIndex': int(attr['index'] == 0), 'avcFlag': int(attr['avc']),
            'optionType': option}


def derive_class(entry):
    return {'classId': entry['classId'], 'initType': int(entry['createdBy'] == 'olt'),
            'actionType': action_mask(entry['actions']),
            'attrs': [derive_attribute(a) for a in entry['attrs']]}


HEADER = '''/* The OMCI information model this device implements.
 * Generated from specification facts and the explicit compatibility profile.
 * Access, creation, notifications and option flags follow G.988 terminology.
 * Local strings store one trailing byte beyond their wire width.
 * Do not edit; regenerate with tools/mib-gen.py.
 */
#ifndef OMCI_MIB_H
#define OMCI_MIB_H

#include <stdint.h>

/* Numeric types use fixed widths; strings include a local terminator. */
enum omci_type { OMCI_U8, OMCI_U16, OMCI_U32, OMCI_U64, OMCI_STRING, OMCI_OCTETS };

struct omci_attr {
	const char *name;
	uint8_t type;       /* enum omci_type */
	uint16_t size;      /* stored bytes, including a string terminator */
	uint8_t oltAcc;     /* R=1, W=2, set-by-create=4 */
	uint8_t isIndex;
	uint8_t avc;        /* raises an attribute value change */
	uint16_t opt;       /* option mask: OMCI_OPT_* below */
};

/* Attribute requirement, support and upload flags.
 * The upload walk omits attributes selected by OMCI_OPT_NOT_UPLOADED. */
#define OMCI_OPT_MANDATORY      0x001
#define OMCI_OPT_M_NOT_SUPPORT  0x002
#define OMCI_OPT_OPTIONAL       0x004
#define OMCI_OPT_O_NOT_SUPPORT  0x008
#define OMCI_OPT_PRIVATE        0x010
#define OMCI_OPT_PM             0x100
#define OMCI_OPT_TABLE          0x200
#define OMCI_OPT_NOT_UPLOADED   0x31a

/* Class support and upload flags.
 * The upload walk omits classes selected by OMCI_STD_NOT_UPLOADED. */
#define OMCI_STD_PRIVATE        0x010
#define OMCI_STD_NO_UPLOAD      0x100
#define OMCI_STD_NOT_UPLOADED   0x110

struct omci_class {
	uint16_t classId;
	const char *name;
	uint16_t rowSize;   /* local row storage, at least the sum of attribute sizes */
	const struct omci_attr *attrs;
	uint8_t nattr;
	uint32_t actions;   /* 1 << msgType for every accepted action */
	uint16_t std;       /* class flags: OMCI_STD_* */
	uint8_t init;       /* 0: ONU-created; 1: OLT-created */
};

extern const struct omci_class omci_classes[];
extern const unsigned omci_class_count;

#endif
'''


def require_reason(entry):
    if not isinstance(entry.get('reason'), str) or not entry['reason'].strip():
        raise ValueError('Profile entry or override lacks a reason')


def apply_overrides(values, overrides, allowed):
    result = dict(values)
    seen = set()
    for override in overrides:
        require_reason(override)
        field = override.get('field')
        if field not in allowed or field in seen:
            raise ValueError('Unknown or duplicate override field: ' + str(field))
        if override.get('class') not in CATEGORIES:
            raise ValueError('Override lacks a valid classification')
        if result[field] == override['value']:
            raise ValueError('Ineffective override: ' + field)
        result[field] = override['value']
        seen.add(field)
    return result


def validate_output(entry, standard_indexing):
    table, attrs = entry['table'], entry['attrs']
    if type(table['classId']) is not int or not 0 < table['classId'] <= 65535:
        raise ValueError('Invalid output class ID')
    if not attrs:
        raise ValueError('A class needs at least its entity index attribute')
    if not all(is_printable_name(table[k]) for k in ('name', 'shortName', 'desc')):
        raise ValueError('Class names must be non-empty printable ASCII')
    if type(table['stdType']) is not int or not 0 <= table['stdType'] <= 0xffff:
        raise ValueError('Invalid class flags')
    if type(table['entrySize']) is not int or not 0 < table['entrySize'] <= 65535:
        raise ValueError('Invalid row size')
    if len(attrs) > 255 or [a['index'] for a in attrs] != list(range(len(attrs))):
        raise ValueError('Output attribute indices must be contiguous')
    if sum(a['size'] for a in attrs) > table['entrySize']:
        raise ValueError('Attribute sizes exceed the row size')
    if table['initType'] not in (0, 1) or type(table['initType']) is not int:
        raise ValueError('Invalid output creation model')
    if type(table['actionType']) is not int or not 0 <= table['actionType'] < 1 << 32:
        raise ValueError('Invalid output action mask')
    if action_mask(table['actions']) != table['actionType']:
        raise ValueError('Action list and mask disagree')
    if type(table['attrNum']) is not int or not 0 < table['attrNum'] <= 255:
        raise ValueError('Invalid compatibility attribute count')
    if attrs[0]['isIndex'] != 1:
        raise ValueError('Attribute 0 must be the entity index')
    # Private classes may mark further local index attributes; standard ones may not.
    if standard_indexing and any(a['isIndex'] for a in attrs[1:]):
        raise ValueError('Only attribute 0 may be an index in a standard class')
    for attr in attrs:
        if attr['type'] not in C_TYPES:
            raise ValueError('Unknown storage type')
        if type(attr['size']) is not int or not 0 < attr['size'] <= 65535:
            raise ValueError('Invalid stored attribute size')
        for field, lower, upper in (('oltAcc', 1, 7), ('isIndex', 0, 1),
                                    ('avcFlag', 0, 1), ('optionType', 0, 0x3ff)):
            if type(attr[field]) is not int or not lower <= attr[field] <= upper:
                raise ValueError('Invalid output attribute field: ' + field)
        if attr['size'] != FIXED_WIDTHS.get(attr['type'], attr['size']):
            raise ValueError('Fixed-width type and stored size disagree')
        if attr['type'] == 'string' and attr['size'] < 2:
            raise ValueError('A stored string needs room for its terminator')
        for field in ('name', 'dataType', 'len', 'mibSave', 'outStyle'):
            if field not in attr:
                raise ValueError('Missing compatibility metadata: ' + field)
        if not is_printable_name(attr['name']):
            raise ValueError('Attribute names must be non-empty printable ASCII')
        if type(attr['len']) is not int or not 0 < attr['len'] <= 65535:
            raise ValueError('Invalid compatibility attribute length')


def compatibility_entry(config, table, attrs, standard_indexing):
    if not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', config['ident']):
        raise ValueError('Invalid C array identifier')
    if type(config['stockBacked']) is not bool:
        raise ValueError('stockBacked must be boolean')
    # The legacy consumer key is derived from the neutral identifier for compatibility.
    plugin = 'mib_' + config['ident'] + '.so' if config['stockBacked'] else None
    entry = {'plugin': plugin, 'table': table, 'attrs': attrs}
    validate_output(entry, standard_indexing)
    return entry


def build_model(facts, profile):
    validate_facts(facts)
    standard = {e['classId']: e for e in facts['classes']}
    entries, identifiers = {}, {}

    def add(config, entry):
        class_id = entry['table']['classId']
        if class_id in entries or config['ident'] in identifiers.values():
            raise ValueError('Duplicate output class or array identifier')
        entries[class_id] = entry
        identifiers[class_id] = config['ident']

    configured = set()
    for config in profile['classes']:
        if not set(config) <= CLASS_KEYS:
            raise ValueError('Unknown profile class key')
        class_id = config['classId']
        if class_id not in standard or class_id in configured:
            raise ValueError('Unknown or duplicate standard profile class')
        configured.add(class_id)
        source = standard[class_id]
        pure = derive_class(source)
        initial = {k: pure[k] for k in ('actionType', 'initType')}
        initial['standardAttributes'] = list(range(len(source['attrs'])))
        effective = apply_overrides(initial, config.get('overrides', []), set(initial))
        selected = effective['standardAttributes']
        if not isinstance(selected, list) or not selected or selected[0] != 0 or \
                any(type(i) is not int or i < 0 or i >= len(source['attrs']) for i in selected) or \
                selected != sorted(set(selected)):
            raise ValueError('Unknown or unordered standard attribute selection')
        attrs = {}
        for attr_config in config['attrs']:
            if not set(attr_config) <= ATTR_KEYS:
                raise ValueError('Unknown profile attribute key')
            index = attr_config['index']
            if type(index) is not int or index not in selected or index in attrs:
                raise ValueError('Unknown or duplicate profile attribute')
            value = apply_overrides(pure['attrs'][index], attr_config.get('overrides', []),
                                    set(ATTR_FIELDS))
            # The legacy dataType and len keys follow from the storage type and size.
            # The hand-declared classes never carried a dataType, and a profile
            # names len only where the existing model stores another value.
            length = value['size'] - (value['type'] == 'string')
            if 'len' in attr_config:
                if attr_config['len'] == length:
                    raise ValueError('Ineffective len in profile attribute')
                length = attr_config['len']
            attrs[index] = {'index': index, 'name': attr_config['name'],
                            'dataType': TYPE_CODES[value['type']] if config['stockBacked'] else None,
                            'len': length,
                            'isIndex': value['isIndex'],
                            **{k: attr_config[k] for k in ('mibSave', 'outStyle')},
                            **{k: value[k] for k in ('oltAcc', 'avcFlag', 'optionType', 'type', 'size')}}
        if set(attrs) != set(selected):
            raise ValueError('Missing selected standard attribute metadata')
        for extension in config.get('privateAttrs', []):
            require_reason(extension)
            attr = dict(extension['attribute'])
            index = attr['index']
            if type(index) is not int or index < 1 or index in attrs or not attr['optionType'] & 0x10:
                raise ValueError('Private extension must be unique and explicitly private')
            attrs[index] = attr
        # Specification-declared classes historically list classId before desc.
        keys = ('name', 'shortName', 'desc', 'classId') if config['stockBacked'] else \
            ('name', 'shortName', 'classId', 'desc')
        table = {k: config[k] for k in keys}
        table.update(initType=effective['initType'], stdType=config['stdType'],
                     actionType=effective['actionType'], attrNum=len(attrs),
                     entrySize=config['entrySize'],
                     actions=[a for a, bit in sorted(MESSAGE_TYPES.items(), key=lambda p: p[1])
                              if effective['actionType'] & (1 << bit)])
        add(config, compatibility_entry(config, table, [attrs[i] for i in sorted(attrs)], True))
    if configured != set(standard):
        raise ValueError('Missing standard profile class')
    for group in ('privateClasses', 'legacyClasses'):
        for config in profile[group]:
            require_reason(config)
            table = dict(config['table'])
            class_id = table['classId']
            if group == 'legacyClasses':
                if class_id not in facts['source'].get('obsoleteClassIds', []):
                    raise ValueError('Legacy class lacks an obsolete identifier declaration')
                if table.get('attrNum') != len(config['attrs']):
                    raise ValueError('Legacy attribute count disagrees with its attributes')
            elif not is_private_class_id(class_id):
                raise ValueError('Private class ID is outside reserved ranges')
            if 'extra' in table:
                raise ValueError('Unused compatibility keys do not belong in the profile')
            add(config, compatibility_entry(config, table, [dict(a) for a in config['attrs']],
                                            group == 'legacyClasses'))
    order = profile['classOrder']
    if len(order) != len(set(order)) or set(order) != set(entries):
        raise ValueError('Output class order is incomplete or duplicated')
    return [entries[i] for i in order], identifiers


def emit(model, identifiers, fingerprint):
    lines = ['/* Generated by tools/mib-gen.py -- %d managed entities, %d attributes.'
             % (len(model), sum(len(e['attrs']) for e in model)),
             ' * Source SHA-256: ' + fingerprint,
             ' * Do not edit; see omci_mib.h. */', '#include "omci_mib.h"', '']
    rows = []
    for entry in sorted(model, key=lambda e: e['table']['classId']):
        table = entry['table']
        ident = identifiers[table['classId']]
        lines.append('static const struct omci_attr attr_%s[] = {' % ident)
        for attr in entry['attrs']:
            lines.append('\t{ %s, %s, %d, %d, %d, %d, 0x%03x },' % (
                json.dumps(attr['name'], ensure_ascii=True), C_TYPES[attr['type']],
                attr['size'], attr['oltAcc'], attr['isIndex'], attr['avcFlag'], attr['optionType']))
        lines.append('};')
        rows.append('\t{ %5d, %s, %d, attr_%s, %d, 0x%08xu, 0x%03x, %d },' % (
            table['classId'], json.dumps(table['name'], ensure_ascii=True), table['entrySize'],
            ident, len(entry['attrs']), table['actionType'], table['stdType'], table['initType']))
    lines += ['', 'const struct omci_class omci_classes[] = {'] + rows + ['};', '',
              '/* Counted from the array, not written as a number: a hand-edited table\n * with a stale count is read short, or past its end. */',
              'const unsigned omci_class_count = sizeof omci_classes / sizeof omci_classes[0];']
    return {'omci_mib.c': '\n'.join(lines) + '\n', 'omci_mib.h': HEADER,
            'omci_mib.json': json.dumps(model, indent=1) + '\n'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--facts', type=Path, default=MODEL / 'g988.json')
    parser.add_argument('--profile', type=Path, default=MODEL / 'profile.json')
    parser.add_argument('--output-dir', type=Path, default=GENERATED)
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--validate-only', action='store_true')
    args = parser.parse_args()
    try:
        model, identifiers = build_model(read_json(args.facts), read_json(args.profile))
        if args.validate_only:
            return
        fingerprint = hashlib.sha256(args.facts.read_bytes() + b'\0' +
                                     args.profile.read_bytes()).hexdigest()
        outputs = emit(model, identifiers, fingerprint)
        if args.check:
            drift = [name for name, data in outputs.items()
                     if not (args.output_dir / name).exists() or
                     (args.output_dir / name).read_bytes() != data.encode('utf-8')]
            if drift:
                parser.exit(1, 'mib-gen: generated output differs: ' + ', '.join(drift) + '\n')
        else:
            args.output_dir.mkdir(parents=True, exist_ok=True)
            for name, data in outputs.items():
                (args.output_dir / name).write_bytes(data.encode('utf-8'))
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, 'mib-gen: ' + str(error) + '\n')


if __name__ == '__main__':
    main()
