#ifndef MICROHAD_MATTER_JSON_H
#define MICROHAD_MATTER_JSON_H
#include "matter_protocol.h"
#include <string>
// Public HTTP/WebSocket and test-fixture boundary only. IPC itself is typed.
namespace MatterWire {
bool parseJson(unsigned char kind,unsigned char opcode,const std::string&,MatterProto::MessageBody*);
std::string jsonBody(unsigned char kind,unsigned char opcode,const MatterProto::MessageBody&);
}
#endif
