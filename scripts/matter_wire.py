"""Host-fixture CBOR v2 codec. No dependencies and no payload logging."""
import json
import math
import struct
from pathlib import Path
SHAPES = json.loads((Path(__file__).resolve().parents[1] / 'protocol/matter-wire-shapes.json').read_text())

def dumps(v):
    def head(major,n):
        return bytes([major << 5 | n]) if n<24 else bytes([major << 5 | 24])+bytes([n]) if n<256 else bytes([major << 5 | 25])+struct.pack('>H',n) if n<65536 else bytes([major << 5 | 26])+struct.pack('>I',n) if n<2**32 else bytes([major << 5 | 27])+struct.pack('>Q',n)
    if v is None:return b'\xf6'
    if isinstance(v,bool):return b'\xf5' if v else b'\xf4'
    if isinstance(v,int):return head(0,v) if v>=0 else head(1,-1-v)
    if isinstance(v,float):return b'\xfb'+struct.pack('>d',v)
    if isinstance(v,str):v=v.encode();return head(3,len(v))+v
    if isinstance(v,bytes):return head(2,len(v))+v
    if isinstance(v,list):return head(4,len(v))+b''.join(map(dumps,v))
    if isinstance(v,dict):return head(5,len(v))+b''.join(dumps(k)+dumps(val) for k,val in v.items())
    raise ValueError('unsupported CBOR type')

def loads(data):
    data=bytes(data)
    at=0
    def read(depth=0):
        nonlocal at
        first=data[at];at+=1;major=first>>5;ai=first&31
        if ai>=28:raise ValueError('indefinite/reserved CBOR')
        n=ai
        if ai>=24:
            width=1<<(ai-24);part=data[at:at+width]
            if len(part)!=width:raise ValueError('truncated CBOR')
            at+=width;n=int.from_bytes(part,'big')
        if major==0:return n
        if major==1:return -1-n
        if major in (2,3):
            part=data[at:at+n];at+=n
            if len(part)!=n:raise ValueError('truncated CBOR')
            return part if major==2 else part.decode()
        if major in (4,5):
            if depth>=16 or n>(512 if major==4 else 256):raise ValueError('CBOR bounds')
            if major==4:return [read(depth+1) for _ in range(n)]
            out={}
            for _ in range(n):
                k=read(depth+1)
                if not isinstance(k,str) or k in out:raise ValueError('CBOR map key')
                out[k]=read(depth+1)
            return out
        if major==7:
            if ai in (20,21):return ai==21
            if ai==22:return None
            if ai in (25,26,27):
                value=struct.unpack({25:'>e',26:'>f',27:'>d'}[ai],part)[0]
                if not math.isfinite(value):raise ValueError('nonfinite CBOR')
                return value
        raise ValueError('unsupported CBOR')
    result=read()
    if at!=len(data):raise ValueError('trailing CBOR')
    return result

def shape(context,value,kind,opcode,event):
    if context=='body' and kind==0:
        action=value.get('command' if opcode==11 else 'action','') if isinstance(value,dict) else ''
        return f'request{opcode}:{action}' if opcode in (9,10,11) and action else f'request{opcode}'
    if context=='body':return 'event' if kind==2 else 'matter' if opcode==1 else 'thread' if opcode==2 else 'reply'
    if context=='result':return 'session' if opcode<=5 else f'ack{opcode}' if opcode<=8 else 'thread' if opcode==9 else 'page' if 'devices' in value else 'job'
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

def frame(opcode,kind,ident,body=b'',version=2,**extra):
    if isinstance(body,(bytes,str)):
        try:body=json.loads(body) if body else {}
        except (ValueError,UnicodeError):body=body.decode(errors='replace') if isinstance(body,bytes) else body
    body=convert(body,'body',kind,opcode)
    message={'version':version,'kind':kind,'opcode':opcode,'id':ident,'body':body}
    message.update(extra)
    encoded=dumps(message)
    return struct.pack('<H',len(encoded))+encoded

def receive(peer,exact):
    length,=struct.unpack('<H',exact(peer,2))
    assert 0<length<=2048
    message=loads(exact(peer,length))
    assert message['version']==2
    kind,opcode,ident=message['kind'],message['opcode'],message['id']
    body=convert(message['body'],'body',kind,opcode,incoming=True)
    return kind,opcode,ident,body
