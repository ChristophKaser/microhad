#include <stddef.h>
#include "zone_table.h"
const char *gateway_tz_name(unsigned index)
{
    return index < gateway_zone_count ? gateway_zones[index].name : NULL;
}
