#ifndef MICROHAD_ZIGBEE_JSON_UTIL_H
#define MICROHAD_ZIGBEE_JSON_UTIL_H

#include <string>

/* Helpers for the flat JSON objects persisted and consumed by the gateway. */
bool jsonStringField(const std::string& input, const std::string& name, std::string* value);
bool jsonLongField(const std::string& input, const std::string& name, long* value);
std::string jsonEscape(const std::string& value);
bool readTextFile(const std::string& filename, std::string* content);

#endif
