/* Serialized by the Rust wrapper (and by the standalone validation harness). */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "acetimec/epoch.h"
#include "acetimec/zone_processor.h"
#include "zone_table.h"

static AtcZoneProcessor processor;

int32_t gateway_tz_find(const uint8_t *name, size_t length)
{
    unsigned i;
    for (i = 0; i < gateway_zone_count; ++i) {
        if (strlen(gateway_zones[i].name) == length &&
            memcmp(name, gateway_zones[i].name, length) == 0) return (int32_t)i;
    }
    return -1;
}

int32_t gateway_tz_offsets(uint32_t zone, int64_t unix_seconds,
                           int32_t *standard, int32_t *total)
{
    AtcFindResult result;
    /* UTC 2025-01-01 through 2100-01-01, end exclusive. Convert through
       AceTime's 2050 epoch, never through the target libc's 32-bit time_t. */
    if (zone >= gateway_zone_count || unix_seconds < INT64_C(1735689600) ||
        unix_seconds >= INT64_C(4102444800)) return -1;
    atc_processor_init_for_zone_info(&processor, gateway_zones[zone].info);
    atc_processor_find_by_epoch_seconds(&processor,
        atc_epoch_seconds_from_unix_seconds(unix_seconds), &result);
    if (result.type == kAtcFindResultNotFound) return -1;
    *standard = result.std_offset_seconds;
    *total = result.std_offset_seconds + result.dst_offset_seconds;
    return 0;
}

/* Used by standalone validation, discarded from service by section GC. */
uint32_t gateway_tz_workspace_size(void) { return sizeof(processor); }
