#!/usr/bin/env python3
"""Check bounded fixtures against standard generated protobuf bindings.
Run in the generation venv (protocol/protobuf-requirements.txt).
"""
import json
from pathlib import Path
import sys
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'protocol/generated'))
from matter_ipc_v3_pb2 import Envelope
import matter_wire as wire
count=0
for vector in json.loads((ROOT/'protocol/matter-wire-vectors.json').read_text()):
    if not vector['valid']:continue
    raw=bytes.fromhex(vector['wire'])[2:]
    message=Envelope.FromString(raw)
    assert message.version==3,vector['name']
    assert message.body.WhichOneof('choice') is not None,vector['name']
    assert wire.loads(message.SerializeToString())==wire.loads(raw),vector['name']
    count+=1
# Scalar presence and explicit null/empty lists survive standard code generation.
m=Envelope();m.version=3;m.kind=0;m.opcode=1;m.id=1;m.body.request1.SetInParent()
assert m.HasField('kind') and m.kind==0
assert m.SerializeToString()==wire.frame(1,0,1,{})[2:]
print(f'PASS {count} bounded vectors against standard protobuf bindings')
