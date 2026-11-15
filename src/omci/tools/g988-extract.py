#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The odi-oss contributors
# SPDX-License-Identifier: GPL-2.0-or-later

"""Extract factual model fields from the approved G.988 PDF with pdftotext.

Developer tool only. The checked-in generator and tests need no PDF or
external package. Variable table widths describe one row or byte element;
the profile supplies the local storage representation.
"""
import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path

CLAUSES = {2: '9.1.3',
 5: '9.1.5',
 6: '9.1.6',
 7: '9.1.4',
 11: '9.5.1',
 24: '9.5.2',
 45: '9.3.1',
 46: '9.3.2',
 47: '9.3.4',
 49: '9.3.6',
 50: '9.3.8',
 52: '9.3.9',
 53: '9.9.1',
 58: '9.9.6',
 78: '9.3.12',
 79: '9.3.7',
 83: '9.13.3',
 84: '9.3.11',
 89: '9.5.3',
 130: '9.3.10',
 131: '9.12.2',
 133: '9.1.7',
 134: '9.4.1',
 136: '9.4.3',
 137: '9.12.3',
 138: '9.9.18',
 139: '9.9.4',
 140: '9.9.12',
 141: '9.9.11',
 142: '9.9.5',
 143: '9.9.7',
 144: '9.9.13',
 145: '9.9.10',
 146: '9.9.8',
 147: '9.9.9',
 148: '9.12.4',
 150: '9.9.3',
 151: '9.9.14',
 152: '9.9.15',
 153: '9.9.2',
 157: '9.12.5',
 158: '9.1.12',
 171: '9.3.13',
 256: '9.1.1',
 257: '9.1.2',
 262: '9.2.2',
 263: '9.2.1',
 264: '9.12.1',
 266: '9.2.4',
 268: '9.2.3',
 272: '9.2.7',
 273: '9.12.6',
 274: '9.12.7',
 277: '9.2.10',
 278: '9.2.11',
 280: '9.2.12',
 281: '9.2.5',
 284: '9.8.7',
 287: '9.12.8',
 290: '9.3.14',
 296: '9.5.4',
 298: '9.3.18',
 307: '9.12.11',
 308: '9.12.12',
 309: '9.3.27',
 310: '9.3.28',
 311: '9.3.29',
 312: '9.2.9',
 321: '9.3.31',
 322: '9.3.30',
 329: '9.5.5',
 330: '9.12.14',
 334: '9.3.32',
 340: '9.12.16',
 341: '9.2.13'}

def parse_text(text):
    text=text.replace('\f','\n')
    text=re.sub(r'^.*Rec\. ITU-T G\.988.*$','',text,flags=re.M)
    heads=list(re.finditer(r'^(9\.\d+\.\d+)\s+([^\n]+)$',text,re.M))
    sections={}
    for i,h in enumerate(heads):
        body=text[h.end():heads[i+1].start() if i+1<len(heads) else len(text)]
        sections[h[1]]=(h[2].strip(),body)
    return sections

ACTIONS = {'create': 'create',
 'delete': 'delete',
 'get': 'get',
 'set': 'set',
 'get all alarms': 'get-all-alarms',
 'get all alarms next': 'get-all-alarms-next',
 'mib reset': 'mib-reset',
 'mib upload': 'mib-upload',
 'mib upload next': 'mib-upload-next',
 'get next': 'get-next',
 'get current data': 'get-current-data',
 'set table': 'set-table',
 'synchronize time': 'sync-time',
 'test': 'test',
 'reboot': 'reboot',
 'start download': 'start-sw-download',
 'end download': 'end-sw-download',
 'activate image': 'activate-sw',
 'commit image': 'commit-sw',
 'start software download': 'start-sw-download',
 'download section': 'download-section',
 'end software download': 'end-sw-download',
 'activate software': 'activate-sw',
 'commit software': 'commit-sw'}

def facts(clause,title,body):
    attr=re.split(r'^Attributes\s*$',body,flags=re.M)[1].split('\nActions')[0]
    # Normalize the reversed order of the requirement and width parentheses.
    attr=re.sub(r'(\([RW][^)]*\)\s*)(\([^)]*(?:byte|octet)[^)]*\))\s*(\((?:mandatory|optional)[^)]*\))',r'\1\3 \2',attr)
    metas=list(re.finditer(r'\(([RW][^)]{0,60})\)\s*\((mandatory|optional)[^)]*\)\s*\(([^)]*)\)',attr,re.I))
    records=[]; prev=0
    for m in metas:
        raw=attr[prev:m.start()]
        titles=[t for t in re.finditer(r'^ {0,12}([A-Za-z][^:\n]{0,100}):',raw,re.M) if not t[1].startswith('NOTE')]
        label=titles[-1][1] if titles else ''
        if titles:raw=raw[titles[-1].start():]
        access=['R'] if 'R' in m[1] else []
        if 'W' in m[1]:access+=['W']
        if 'create' in m[1].lower() or 'SC' in m[1]:access+=['SBC']
        width=m[3];nums=re.findall(r'\d+',width)
        variable=bool(re.search(r'\b[NM]\b',width))
        val=int(nums[0]) if nums else (1 if variable else None)
        if variable and nums and not re.match(r'\s*\d+\s*\*',width):val=int(nums[-1])
        kind='uint' if val in (1,2,4,8) else 'octets'
        low=raw.lower()
        if 'string' in low or 'ascii' in low or 'character' in low:kind='string'
        if 'bit mask' in low or 'bitmask' in low:kind='bitfield'
        if 'pointer' in label.lower() or 'points to' in low or 'a pointer to' in low:kind='pointer'
        if 'complement' in low:kind='sint'
        table=(bool(re.search(r'\btable\b',label.lower())) and 'size' not in label.lower()) or variable or 'row part' in width.lower()
        if table:kind='table'
        if val in (1,2,4,8) and kind=='string':kind='uint'
        i=len(records)
        if i==0:kind='pointer'
        if clause=='9.1.6' and i==10:table=False;kind='enum'
        if label.lower()=='serial number':kind='octets'
        if clause=='9.9.9' and i>0:kind='string'
        if (clause,i) in {('9.3.10',11),('9.3.13',8),('9.9.3',11)}:kind='octets'
        pm=('performance monitoring' in title.lower() or 'extended PM' in title) and (i==1 or i>=3)
        records.append({'index':i,'size':val,'access':access,'requirement':'M' if m[2].lower()=='mandatory' else 'O','avc':False,'table':table,'pm':pm,'kind':kind,'_size':width,'_label':label,'_text':re.sub(r'\s+',' ',raw)[-120:]})
        if variable:records[-1]['variable']=True
        prev=m.end()
    # Clauses 9.12.5 and 9.12.11 define a repeated group rather than 15 lists.
    if clause in ('9.12.5','9.12.11'):
        records=records[:2]
        for i in range(2,17):
            records.append({'index':i,'size':25,'access':['R','W'],'requirement':'M' if clause=='9.12.5' or i==2 else 'O','avc':False,'table':False,'pm':False,'kind':'string' if clause=='9.12.5' else 'octets','_label':'','_size':'25','_text':''})
    act=body.split('\nActions',1)[1].split('\nNotifications',1)[0] if '\nActions' in body else ''
    actions=set()
    for line in act.splitlines():
        label=line.strip().split(':')[0].split('(')[0].strip().lower()
        for a in re.split(r',\s*|\s+and\s+',label):
            if a in ACTIONS:actions.add(ACTIONS[a])
    notify=body.split('\nNotifications',1)[1] if '\nNotifications' in body else ''
    avc=notify.split('Attribute value change',1)[1] if 'Attribute value change' in notify else ''
    avc=re.split(r'(?m)^\s*(?:Alarm|Test result|Vendor-specific usage)\b',avc)[0]
    for line in avc.splitlines():
        m=re.match(r'^ {0,20}(\d+)(?:\.\.(\d+))?\s+(.+)',line)
        if m and not re.match(r'(?:N/A|Reserved)',m[3],re.I):
            for j in range(int(m[1]),int(m[2] or m[1])+1):
                if j<len(records):records[j]['avc']=True
    created = 'olt' if 'create' in actions else 'onu'
    if re.search(r'ONU\s+automatically creates', body.split('Attributes')[0]):
        created = 'onu'
    return {'clause':clause,'createdBy':created,'actions':sorted(actions),'attrs':records,'_title':title}


def extract(pdf, manifest):
    sources = manifest['sources']
    digest = hashlib.sha256(pdf.read_bytes()).hexdigest()
    latest = sources[-1]
    if digest != latest['sha256']:
        raise ValueError('PDF hash does not match the approved source manifest')
    text = subprocess.check_output(
        ['pdftotext', '-layout', str(pdf), '-'], timeout=120).decode('utf-8')
    sections = parse_text(text)
    classes = []
    for class_id, clause in sorted(CLAUSES.items()):
        title, body = sections[clause]
        if clause == '9.3.31':
            body = sections['9.3.30'][1]
        entry = facts(clause, title, body)
        entry = {k: v for k, v in entry.items() if not k.startswith('_')}
        entry['classId'] = class_id
        entry['attrs'] = [{k: v for k, v in a.items() if not k.startswith('_')}
                          for a in entry['attrs']]
        if not entry['attrs'] or any(not isinstance(a['size'], int) or a['size'] <= 0
                                     for a in entry['attrs']):
            raise ValueError('Unresolved attribute width in clause ' + clause)
        if len(entry['attrs']) > 17:
            raise ValueError('Too many standard attributes in clause ' + clause)
        classes.append({'classId': entry.pop('classId'), **entry})
    def metadata(s):
        return {k: (v.replace('-', '') if k == 'date' else v)
                for k, v in s.items() if k != 'bytes'}
    source = metadata(latest)
    source['components'] = [metadata(s) for s in sources]
    source['excludedAmendments'] = ['3 (08/2026)']
    source['obsoleteClassIds'] = [267]
    return {'source': source, 'classes': classes}


def dump_compact(value, depth=0):
    """Serialize with one flat object per line, so a diff shows one attribute."""
    pad = ' ' * depth
    flat = lambda v: not isinstance(v, (dict, list)) or \
        (isinstance(v, list) and all(not isinstance(x, (dict, list)) for x in v))
    if isinstance(value, dict) and value and not all(flat(v) for v in value.values()):
        return '{\n' + ',\n'.join('%s %s: %s' % (pad, json.dumps(k), dump_compact(v, depth + 1))
                                    for k, v in value.items()) + '\n' + pad + '}'
    if isinstance(value, list) and value and not flat(value):
        return '[\n' + ',\n'.join(pad + ' ' + dump_compact(v, depth + 1)
                                    for v in value) + '\n' + pad + ']'
    return json.dumps(value)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pdf', type=Path, required=True)
    parser.add_argument('--sources', type=Path, required=True,
                        help='local manifest with ordered edition, date, URL and SHA-256')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        result = extract(args.pdf, json.loads(args.sources.read_text()))
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(dump_compact(result) + '\n')
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        parser.exit(1, 'g988-extract: ' + str(error) + '\n')


if __name__ == '__main__':
    main()
