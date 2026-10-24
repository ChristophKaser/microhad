"""Host-fixture protobuf v3 adapter; internal/public JSON stays unchanged."""
import json
import struct
from pathlib import Path
from protobuf_wire import encode, decode, normalize
SHAPES = json.loads((Path(__file__).resolve().parents[1]/'protocol/matter-wire-shapes.json').read_text())
def shape(context,value,kind,opcode,event):
    if context=='body' and kind==0:
        action=value.get('command' if opcode==11 else 'action','') if isinstance(value,dict) else ''
        return f'request{opcode}:{action}' if opcode in (9,10,11) and action else f'request{opcode}'
    if context=='body':return 'event' if kind==2 else 'matter' if opcode==1 else 'thread' if opcode==2 else 'reply'
    if context=='result':return 'session' if opcode<=5 else f'ack{opcode}' if opcode<=8 else 'thread' if opcode==9 else 'subscription-health' if 'subscriptions' in value else 'page' if 'devices' in value else 'job'
    if context=='event-data':return {'thread':'thread','commissioning':'session','ble_write':'ble-write','matter_report':'reading','matter_button':'button','matter_inventory':'inventory','matter_inventory_nodes':'inventory-nodes'}.get(event,'')
    return context

def convert(value,context,kind,opcode,event='',incoming=False):
    if context=='@id' and value is not None:
        if incoming:return f'{value:016x}' if isinstance(value,int) else value
        try:return int(value,16)
        except (ValueError,TypeError):return value
    if context=='@bytes':
        if incoming:return value.hex() if isinstance(value,bytes) else value
        try:return bytes.fromhex(value)
        except (ValueError,TypeError):return value
    ctx=shape(context,value,kind,opcode,event)
    if isinstance(value,list):return [convert(v,ctx,kind,opcode,event,incoming) for v in value]
    if isinstance(value,dict):
        event=value.get('event',event)
        return {k:convert(v,SHAPES.get(ctx,{}).get(k,''),kind,opcode,event,incoming) for k,v in value.items()}
    return value

def dumps(message):
    kind,op=message['kind'],message['opcode']
    message=dict(message)
    message['body']=normalize(message['body'],'Body',kind,op)
    return encode(message,'Envelope')
def loads(data):
    if not 0<len(data)<=2048:raise ValueError('frame size')
    m=decode(bytes(data),'Envelope')
    if m.get('version')!=3 or m.get('kind',3)>2 or not 1<=m.get('opcode',0)<=12 or (m['kind']==2)!=(m.get('id',0)==0) or 'body' not in m or (m['kind']==2)!=(m['opcode']==12):raise ValueError('invalid envelope')
    kind,op=m['kind'],m['opcode']
    expected=f'request{op}' if kind==0 else 'event' if kind==2 else 'matter' if op==1 else 'thread' if op==2 else 'reply'
    if expected not in m['body']:raise ValueError('mismatched body')
    m['body']=normalize(m['body'],'Body',kind,op,incoming=True)
    return m
def frame(opcode,kind,ident,body=b'',version=3,**extra):
    if isinstance(body,(bytes,str)):body=json.loads(body) if body else {}
    message=dict(version=version,kind=kind,opcode=opcode,id=ident,body=convert(body,'body',kind,opcode))
    message.update(extra)
    encoded=dumps(message)
    return struct.pack('<H',len(encoded))+encoded

def receive(peer,exact):
    length,=struct.unpack('<H',exact(peer,2))
    assert 0<length<=2048
    m=loads(exact(peer,length));kind,op,ident=m['kind'],m['opcode'],m['id']
    return kind,op,ident,convert(m['body'],'body',kind,op,incoming=True)
