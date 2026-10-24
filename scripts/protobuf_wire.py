"""Bounded typed protobuf wire codec for host fixtures; no runtime dependencies."""
import json
import math
import struct
from pathlib import Path
SCHEMA=json.loads((Path(__file__).resolve().parents[1]/'protocol/generated/schema.json').read_text())

def varint(n):
    if not 0<=n<2**64:raise ValueError('integer overflow')
    out=bytearray()
    while n>127:out.append((n&127)|128);n>>=7
    out.append(n);return bytes(out)

def shape_name(s):return ''.join(x.title() for x in s.split('-'))
def normalize(v,msg,kind,op,event='',incoming=False):
    fields=SCHEMA[msg]
    if msg.startswith('List') or msg=='UintList':
        return [normalize(x,fields[0]['message'],kind,op,event,incoming) if fields[0]['type']==11 else x for x in v.get('items',[])] if incoming else {'items':[normalize(x,fields[0]['message'],kind,op,event) if fields[0]['type']==11 else x for x in v]}
    if fields and fields[0]['oneof']:
        if incoming:
            if len(v)!=1:raise ValueError('missing/conflicting choice')
            k,x=next(iter(v.items()));f=next(f for f in fields if f['name']==k)
            if k=='null':
                if x is not True:raise ValueError('invalid null')
                return None
            return normalize(x,f['message'],kind,op,event,True) if f['type']==11 else x
        if msg=='Body':key=f'request{op}' if kind==0 else 'event' if kind==2 else 'matter' if op==1 else 'thread' if op==2 else 'reply'
        elif msg=='Result':key='session' if op<=5 else f'ack{op}' if op<=8 else 'thread' if op==9 else 'subscription_health' if 'subscriptions' in v else 'page' if 'devices' in v else 'job'
        elif msg=='EventData':key={'thread':'thread','commissioning':'session','ble_write':'ble_write','matter_report':'reading','matter_button':'button','matter_inventory':'inventory','matter_inventory_nodes':'inventory_nodes'}[event]
        elif v is None:key='null'
        elif msg=='NullableText':key='text'
        elif msg in ('NullableUint','NullableInt'):key='integer'
        elif isinstance(v,bool):key='boolean'
        elif isinstance(v,float):key='number'
        elif isinstance(v,int):key='integer' if v<0 else 'unsigned' if msg=='ReadingValue' else 'integer'
        elif isinstance(v,str):key='text'
        elif isinstance(v,list):key='list'
        else:key='measurement'
        f=next(f for f in fields if f['name']==key)
        x=True if key=='null' else v
        return {key:normalize(x,f['message'],kind,op,event) if f['type']==11 else x}
    if not isinstance(v,dict):raise ValueError('expected message')
    event=v.get('event',event) if isinstance(v.get('event',event),str) else event
    out={}
    for f in fields:
        k=f['name']
        if k not in v:continue
        x=v[k]
        if f['type']==11:x=normalize(x,f['message'],kind,op,event,incoming)
        out[k]=x
    return out

def encode(v,msg,packed=False):
    out=bytearray()
    for f in SCHEMA[msg]:
        if f['name'] not in v:continue
        values=v[f['name']] if f['repeated'] else [v[f['name']]]
        if packed and f['repeated'] and f['type'] in (4,13,18,8):
            content=b''.join(varint((x<<1)^(x>>63) if f['type']==18 else int(x)) for x in values)
            if content:out+=varint(f['tag']*8+2)+varint(len(content))+content
            continue
        for x in values:
            t=f['type'];wt=1 if t==1 else 2 if t in (9,11,12) else 0
            out+=varint(f['tag']*8+wt)
            if wt==0:
                if t==8:
                    if not isinstance(x,bool):raise ValueError('expected bool')
                    x=int(x)
                elif not isinstance(x,int) or isinstance(x,bool):raise ValueError('expected integer')
                if t==18:
                    if not -2**63<=x<2**63:raise ValueError('integer overflow')
                    x=(x<<1)^(x>>63)
                if t==13 and not 0<=x<2**32:raise ValueError('integer overflow')
                out+=varint(x)
            elif wt==1:
                if not math.isfinite(x):raise ValueError('nonfinite')
                out+=struct.pack('<d',x)
            else:
                x=encode(x,f['message'],packed) if t==11 else x.encode() if t==9 else bytes(x)
                out+=varint(len(x))+x
    return bytes(out)

def decode(data,msg,depth=0):
    if depth>16:raise ValueError('nesting limit')
    at=0;out={};fields={f['tag']:f for f in SCHEMA[msg]};choice=False
    def read():
        nonlocal at
        n=0
        for i in range(10):
            if at>=len(data):raise ValueError('truncated varint')
            b=data[at];at+=1
            if i==9 and b>1:raise ValueError('overflow')
            n|=(b&127)<<(i*7)
            if b<128:return n
        raise ValueError('overflow')
    while at<len(data):
        tag=read();num,wt=tag>>3,tag&7
        if num==0 or num>=2**29 or wt not in (0,1,2,5):raise ValueError('invalid tag')
        if wt==0:x=read()
        else:
            n=read() if wt==2 else 8 if wt==1 else 4
            if n>len(data)-at:raise ValueError('truncated field')
            x=data[at:at+n];at+=n
        f=fields.get(num)
        if f is None:continue
        k=f['name'];t=f['type'];expected=1 if t==1 else 2 if t in (9,11,12) else 0
        if f['repeated'] and wt==2 and t in (4,13,18,8):
            packed_data=x;pos=0
            a=out.setdefault(k,[])
            while pos<len(packed_data):
                n=0
                for i in range(10):
                    if pos>=len(packed_data):raise ValueError('truncated packed varint')
                    b=packed_data[pos];pos+=1
                    if i==9 and b>1:raise ValueError('overflow')
                    n|=(b&127)<<(i*7)
                    if b<128:break
                else:raise ValueError('overflow')
                if (t==13 and n>=2**32) or (t==8 and n>1) or len(a)>=512:raise ValueError('packed bounds')
                a.append((n>>1)^-(n&1) if t==18 else bool(n) if t==8 else n)
            continue
        if wt!=expected or (k in out and not f['repeated']) or (f['oneof'] and choice):raise ValueError('type/duplicate')
        choice|=f['oneof']
        if t==11:x=decode(x,f['message'],depth+1)
        elif t==9:x=x.decode('utf8')
        elif t==1:
            x=struct.unpack('<d',x)[0]
            if not math.isfinite(x):raise ValueError('nonfinite')
        elif t==18:x=(x>>1)^-(x&1)
        elif t==13 and x>=2**32:raise ValueError('overflow')
        elif t==8:
            if x>1:raise ValueError('boolean')
            x=bool(x)
        if f['repeated']:
            a=out.setdefault(k,[])
            if len(a)>=512:raise ValueError('collection limit')
            a.append(x)
        else:out[k]=x
    return out
