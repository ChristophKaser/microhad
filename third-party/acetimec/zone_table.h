/* Local whitelist table; zone processor layouts remain opaque to Rust. */
#include "zoneinfo/zone_info.h"
struct GatewayZone { const char *name; const AtcZoneInfo *info; };
extern const struct GatewayZone gateway_zones[];
extern const unsigned gateway_zone_count;
