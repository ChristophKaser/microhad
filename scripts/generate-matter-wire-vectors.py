#!/usr/bin/env python3
"""Generate protobuf interoperability and malformed-input vectors."""
import json
import struct
import sys
from pathlib import Path
import matter_wire as w
from protobuf_wire import varint, encode
root=Path(__file__).resolve().parents[1]
vectors=[]
def write(path,content):
    if '--check' in sys.argv:
        if not path.exists() or path.read_text()!=content:raise SystemExit('Stale vector artifact: '+str(path))
    elif not path.exists() or path.read_text()!=content:path.write_text(content)
def raw(name,message,valid=False):
    vectors.append(dict(name=name,wire=(struct.pack('<H',len(message))+message).hex(),valid=valid))
def add(name,body,op=1,kind=0,ident=1,valid=True,extra=None):
    wire=w.frame(op,kind,ident,body,**(extra or {}))
    expected=w.convert(w.loads(wire[2:])['body'],'body',kind,op,incoming=True) if valid else {}
    vectors.append(dict(name=name,wire=wire.hex(),valid=valid,kind=kind,opcode=op,id=ident,body=expected))
add('empty-status',{})
add('ble-bytes-and-full-u64',dict(schema=1,session='ffffffffffffffff',generation=1,sequence=1,data='0080ff'),7)
add('max-ble-bytes',dict(schema=1,session='8000000000000001',generation=4294967295,sequence=1,data='ff'*244),7)
add('max-dataset-bytes',dict(schema=1,action='import',zigbee_channel=15,dataset='ff'*254),9)
add('correlated-failure',dict(schema=1,ok=False,error='not_ready'),9,1,4294967295)
add('measurement-event',dict(schema=1,event='matter_report',data=dict(node='fedcba9876543210',endpoint=1,cluster=1026,attribute=0,value=dict(type='measurement',value=dict(value=-5.25,unit='celsius',quality='measured')))),12,2,0)
add('thermostat-write',dict(schema=1,command='thermostat',node='0000000000000002',endpoint=2,field='occupied_cooling_setpoint',value='25.5'),11)
add('thermostat-scalar-event',dict(schema=1,event='matter_report',data=dict(node='0000000000000002',endpoint=2,cluster=513,attribute=18,value=dict(type='thermostat',value=-525))),12,2,0)
add('thermostat-null-event',dict(schema=1,event='matter_report',data=dict(node='0000000000000002',endpoint=2,cluster=513,attribute=0,value=dict(type='thermostat',value=None))),12,2,0)
add('thermostat-capabilities-event',dict(schema=1,event='matter_report',data=dict(node='0000000000000002',endpoint=2,cluster=513,attribute=65531,value=dict(type='thermostat_list',value=[0,17,18,28,65532]))),12,2,0)
thermostat_result=dict(node='0000000000000002',endpoint=2,cluster=513,attribute=65537,value=dict(type='thermostat_schedule',value=dict(mode=3,transitions=[dict(time=60,heat=20.0,cool=25.0)])))
add('thermostat-schedule-job',dict(schema=1,ok=True,result=dict(job='0000000000000003',state='complete',result=thermostat_result)),10,1)
add('local-delete',dict(schema=1,action='delete',node='0000000000000002',local=True),10)
add('restore-subscriptions',dict(schema=1,action='subscriptions',node='0000000000000002',restore=True,cursor=0),10)
add('sync-time-zone',dict(schema=1,command='sync_time',node='0000000000000002',endpoint=0,time_zone='Europe/Berlin',offset_seconds=-3600),11)
# Exercise explicit nulls, empty collections and both protobuf numeric encodings.
add('empty-capabilities',dict(schema=1,event='matter_report',data=dict(node='0000000000000002',endpoint=1,cluster=513,attribute=65531,value=dict(type='thermostat_list',value=[]))),12,2,0)
add('minimum-signed-reading',dict(schema=1,event='matter_report',data=dict(node='0000000000000002',endpoint=1,cluster=513,attribute=0,value=dict(type='thermostat',value=-(2**63)))),12,2,0)
for count in (512,513):
    body=dict(schema=1,event='matter_report',data=dict(node='0000000000000002',endpoint=1,cluster=513,attribute=65531,value=dict(type='thermostat_list',value=[0]*count)))
    add('repeated-'+str(count),body,12,2,0,valid=count==512)
    if count==513:del vectors[-1]['body']
for sample in list(vectors):
    if sample['name']=='thermostat-capabilities-event':
        m=w.loads(bytes.fromhex(sample['wire'])[2:]);m['body']=w.normalize(m['body'],'Body',2,12)
        raw('packed-capabilities',encode(m,'Envelope',packed=True),True)
        vectors[-1].update(kind=2,opcode=12,id=0,body=sample['body'])
def field_bytes(tag,data):return varint(tag*8+2)+varint(len(data))+data
def envelope(body,opcode=1):return b'\x08\x03\x10\x00'+varint(24)+varint(opcode)+b'\x20\x01'+field_bytes(5,body)
raw('wrong-body-choice',envelope(field_bytes(2,b'')))
raw('conflicting-body-oneof',envelope(field_bytes(1,b'')+field_bytes(2,b'')))
raw('nested-duplicate-schema',envelope(field_bytes(7,b'\x08\x01\x08\x01'),7))
raw('invalid-utf8',envelope(field_bytes(3,field_bytes(5,b'\xff')),3))
raw('nested-unknown-field',envelope(field_bytes(1,varint(100*8)+b'\x01')),True)
vectors[-1].update(kind=0,opcode=1,id=1,body={})
base=w.frame(1,0,1,{})[2:]
for name,suffix in [('duplicate-version',b'\x08\x03'),('trailing-zero',b'\x00'),('invalid-tag',b'\x0e'),('varint-overflow',b'\x30'+b'\xff'*10),('truncated-field',b'\x32\x05xx')]:raw(name,base+suffix)
raw('old-cbor-v2',bytes.fromhex('a56776657273696f6e02646b696e6400666f70636f6465016269640164626f6479a0'))
raw('old-protobuf-version',b'\x08\x02'+base[2:])
for n in (2048,2049):
    for length in range(n):
        msg=base+varint(100*8+2)+varint(length)+bytes(length)
        if len(msg)==n:break
    raw('frame-'+str(n),msg,n==2048)
    if n==2048:vectors[-1].update(kind=0,opcode=1,id=1,body={})
raw('unknown-envelope',base+varint(100*8+2)+b'\x03abc',True)
vectors[-1].update(kind=0,opcode=1,id=1,body={})
write(root/'protocol/matter-wire-vectors.json',json.dumps(vectors,indent=2)+'\n')
output='// Generated by scripts/generate-matter-wire-vectors.py; do not edit.\n'
output+='struct WireVector { const char* name; const char* hex; bool valid; const char* json; };\nconst WireVector wireVectors[] = {\n'
for v in vectors:
    body='' if v.get('kind')==0 and v.get('opcode',0)<=2 else json.dumps(v.get('body',{}),separators=(',',':'))
    output+='    {'+json.dumps(v['name'])+','+json.dumps(v['wire'])+','+str(v['valid']).lower()+','+json.dumps(body)+'},\n'
output+='};\n'
write(root/'microhad/include/matter_wire_vectors.inc',output)
