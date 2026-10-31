#ifndef MICROHAD_GATEWAY_TIMEZONE_H
#define MICROHAD_GATEWAY_TIMEZONE_H
#include <string>
#include <stdint.h>
bool gatewayTimeZoneValid(const std::string& name);
bool gatewayTimeZoneOffset(const std::string& name, int64_t unixSeconds, long* offset, long* standardOffset = 0);
std::string gatewayTimeZonesJson();
#endif
