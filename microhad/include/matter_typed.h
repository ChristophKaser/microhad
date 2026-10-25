#ifndef MICROHAD_MATTER_TYPED_H
#define MICROHAD_MATTER_TYPED_H
#include "matter_protocol.h"
#include <string>
namespace MatterWire {
bool validResponse(unsigned char,const MatterProto::MessageBody&);
bool validEvent(const MatterProto::MessageBody&);
std::string hexId(uint64_t);
}
#endif
