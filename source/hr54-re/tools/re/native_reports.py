#!/usr/bin/env python3
"""Correlate generated local evidence into reviewable Markdown inventories."""
import argparse
import json
import re
from pathlib import Path


def dtvwm(path):
    clients=json.loads((path/'dtvwm-commands.json').read_text())
    server={r['command']:r for r in json.loads((path/'dtvwm-server-table.json').read_text())}
    print('# DTVWM client/server correlation\n')
    print('Addresses are ELF virtual addresses. Handler and constructor assignment are **Proven statically**; unlisted struct members and backend effects are **Unknown**. All request rows require UMP setup. Response status is normally 2 for acknowledgements, 3 for values; result word 1 remains command-specific. The server checks ID <112. Empty IDs below have no handler recovered, not a fabricated operation.\n')
    print('| ID | Client method @ address; data/additional bytes | Server handler @ address | Output / prerequisites / side effects / errors |\n|---:|---|---|---|')
    for command in range(112):
        cc=[r for r in clients if r['command']==command];ss=server.get(command)
        c='<br>'.join('`'+r['method'].split('(')[0]+'` @ '+r['function_address']+'; '+str(r['data_bytes'])+'/'+str(r['additional_bytes']) for r in cc) or 'Unknown constructor'
        h=('`'+ss['handler'].split('(')[0]+'` @ '+ss['handler_address']) if ss else 'No recovered handler'
        note='Unknown detailed payload/result/errors; operation named by actual symbol' if ss else 'Unknown'
        if command in (7,8,9,12,13,14,28,29,40,41,46,47,95,98,99,100):note='See [detailed protocol](../native-dtvwm-protocol.md); other result codes Unknown'
        if command==109:note='Client disableJsonGraphicsSupport also sends 109; server routes to disableSleepModeSupport'
        if command==110:note='Server disableJsonGraphicsSupport; no matching client constructor recovered'
        print(f'| {command} | {c} | {h} | {note} |')


def keys(path):
    s=(path/'key-rates.javap').read_text().split('// com/ucentric/pvrdirect/PlayMode')[0]
    blocks=s.split('// Field oShefTbl:Ljava/util/TreeMap;')[1:];rows=[]
    for block in blocks:
        name=re.search(r'// Utf8 (\S+)\n',block);value=re.search(r'// int (\d+)',block)
        if name and value and 'Integer."<init>"' in block:rows.append((name[1],int(value[1])))
    stock=(path/'input-java-stock.javap').read_text()
    exclusive=set()
    for block in stock.split('// Field m_setDruidExclusiveKeys:Ljava/util/TreeSet;')[1:]:
        m=re.search(r'// int (\d+)',block)
        if m and 'TreeSet.add:' in block.split('pop',1)[0]:exclusive.add(int(m[1]))
    print('# Remote raw-key mapping\n')
    print('SHEF KeyTranslator constructor assignments are **Proven statically**. Druid set membership is from KeyEventTranslator; runtime socket ownership depends on session/device and registration. Registration strips the press mask. Browser exposure is **Unknown** unless indicated; existing native MENU prototype supports interception before stock presentation, not arbitrary conflict takeover.\n')
    print('| Remote/SHEF key | Raw code | Registration | Stock routing evidence | Browser-visible? | Native ownership possible? |\n|---|---|---|---|---|---|')
    for name,raw in rows:
        owner='Druid special-key set; default exception path' if raw in exclusive else 'Stock remote map; current owner Unknown'
        browser='MENU absent in existing browser path' if name=='menu' else 'Unknown'
        possible='Existing native prototype + static path' if name=='menu' else 'Generic map supports code; conflicts Unknown'
        print(f'| {name} | `0x{raw:04x}` | `0x{raw:04x}` | {owner} | {browser} | {possible} |')


def media(path):
    rows=json.loads((path/'media-schemas.json').read_text())
    print('# MediaPlayer generated message schema\n')
    print('Generated CAR constructors and Add methods: **Proven statically**. This table alone does not prove the server implements the operation. Offsets are dtv.car file offsets. Primitive widths: Z/B=1, S=2, I=4, J=8 bytes; other descriptors require nested serialization not implemented by the prototype.\n')
    print('| Message | Tag | CAR offset | Fields: name / tag / descriptor / method offset |\n|---|---|---|---|')
    for s in rows['schemas']:
        fields='<br>'.join(f"{f['name']} / `{f['tag']}` / `{f['descriptor']}` / {f['offset']}" for f in s['fields']) or 'None'
        print(f"| {s['name']} | `{s['message_tag']}` | {s['offset']} | {fields} |")
    if rows['failures']:print('\nUnsupported classes: '+json.dumps(rows['failures']))


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('mode',choices=['dtvwm','keys','media']);p.add_argument('--evidence',type=Path,default=Path(__file__).resolve().parents[2]/'docs/native-evidence')
    a=p.parse_args();globals()[a.mode](a.evidence)
