# Lua scripting API

This document describes the public API for MicroHAD Lua automations. It is used
by scripts running in the dedicated automation worker; lifecycle and reload
semantics are specified in [Lua script lifecycle](lua_script_lifecycle.md).

Scripts may observe gateway state and perform ordinary home-automation actions.
They cannot run a shell, access arbitrary files or network endpoints, send raw
radio commands, pair devices, or administer the gateway.

All functions raise a Lua error for invalid arguments. A well-formed request
that cannot be applied returns `nil, error_message`. Device and scene arguments
accept either a stable ID or an exact current display name. Names are matched
case-insensitively but must resolve uniquely; use stable IDs for rename-proof
automations.

For copyable remote, light, actor, and scene scripts, see
[Example scripts](scripting.md#example-scripts). That guide also covers thermostat control and scene-state queries.

## Script initialization

Top-level script code is initialization code. It runs on every initial load,
reload, and daemon restart, so scripts register listeners and reconstruct
needed timers from current device state there.

```lua
local kitchen_light = "Kitchen light"

events.on({ type = "device.added" }, function(event)
  log("info", "new device: " .. event.device.name)
end)
```

## Manual triggers

Register a button handler during initialization:

```lua
triggers.register("evening", "Evening lights", function()
  scenes.activate("Downstairs", "Evening")
end)
```

`triggers.register(id, label, callback)` registers or replaces a handler scoped
to the current script. IDs must be nonempty strings of at most 192 bytes;
labels must be nonempty strings of at most 256 bytes. Each script supports
128 registered triggers. The callback receives no arguments and can sleep or
wait for events like other automation callbacks. Errors use the usual script logs.

Registered buttons appear under **Manual triggers** on the Automations page.
To place one on the dashboard, choose **Edit dashboard**, add an **Automation
trigger** widget, select the trigger, and save the layout.

`triggers.unregister(id)` removes a handler; calling it again is safe.
Reload, disable, deletion, and daemon restart remove runtime handlers and cancel
pending callbacks. Saved dashboard buttons remain, showing **Unavailable** until
the same stable script ID and trigger ID are registered again. Placement survives
daemon restart. Renaming a label preserves placement; changing either ID does not.
Unavailable presses are rejected and never replayed. Successful presses confirm
scheduling, while callback errors are reported in automation logs.

## Events

### `events.on(filter, callback)`

Registers `callback(event)` and returns an opaque subscription handle.
`events.off(handle)` unregisters it. All subscriptions are removed on a
script reload, disable, source removal, or daemon restart.

```lua
events.on({
  type = "device.changed",
  device = "Office sensor",
  field = "temperature_celsius",
  value_below = 18,
}, function(event)
  notify.send("Office", "Temperature is " .. tostring(event.value) .. "°C")
end)
```

`filter.type` is required. Other filter fields narrow the event. Supported
event types are:

| Type | When emitted | Important fields |
| --- | --- | --- |
| `device.added` | A Zigbee, Matter, Shelly, or Fritz device becomes available. | `device`, `source` |
| `device.removed` | A gateway device is removed. | `device`, `source`, `reason` |
| `device.value` | A device supplies an accepted value, even if it is unchanged. | `device`, `device_id`, `field`, `value` |
| `device.changed` | A dashboard-visible field changes. | `device`, `device_id`, `field`, `old`, `value` |
| `device.online` / `device.offline` | Reachability changes. | `device` |
| `remote.action` | A recognized Zigbee or Matter remote/button action arrives. | `device`, `action`, `endpoint`; Matter events can also include `position`, `press_count` |
| `timer.fired` | An optional named timer expires. | `timer_id`, `payload` |
| `automation.error` | The current script has a nonfatal error. | `operation`, `message` |

Every event includes `type`, `at` (Unix seconds), `source`, and a
causal-chain identifier. `event.device` is an immutable snapshot. For numeric
`device.value` and `device.changed` values, filters can use `value_below`, `value_at_most`,
`value_above`, or `value_at_least`; `value` requests exact equality. Applicable
filters also include `device`, `device_id`, `source`, and `field`.

`remote.action` supports the ordinary device filters plus exact `action`, `endpoint`,
`position`, and `press_count` filters. `action` is the gateway's normalized remote
action. Examples include Zigbee `on`, `off`, `toggle`, `move`, `step`, `stop`,
`arrow_left`, and `arrow_right`; Matter remotes emit `matter_press`, `matter_hold`,
`matter_release`, or `matter_multi_press`. The optional Matter `position` and
`press_count` fields are present only when supplied by the device. A recognized
button action is delivered to automations even when that remote is also configured
to trigger a gateway scene.

### Matter switch models

Matter defines two relevant switch models. MicroHAD currently exposes the
**Generic Switch** model: an endpoint advertising the Switch *server* cluster
(`0x003B`, decimal `59`) is discovered as a Matter remote and its standardized
Switch-cluster events produce `remote.action` events. `matter_press` maps to
InitialPress, `matter_hold` to LongPress, `matter_release` to ShortRelease or
LongRelease, and `matter_multi_press` to MultiPressComplete. The event endpoint
identifies the control; `position` identifies it more precisely when the device
reports that field, and `press_count` is present for a reported multi-press
count.

A **command-capable light switch** is different: it uses On/Off and/or Level
Control *client* clusters and, after Matter binding, sends those cluster commands
directly to its bound light or group. The light responds to the command rather
than to a button-position identifier. MicroHAD does not currently translate those
outgoing client commands into `remote.action` events. A switch must advertise the
Switch server cluster to be available as an event-style Matter remote in Lua.

```lua
events.on({
  type = "remote.action",
  device = "Hall remote",
  action = "matter_press",
  endpoint = 1,
}, function(event)
  log("info", "button pressed at position " .. tostring(event.position))
  scenes.activate("Downstairs", "Evening")
end)
```

Use `device.value` when every accepted measurement matters, such as resetting
a stale-reading watchdog or collecting an audit trail. Use `device.changed`
when an automation should run only after the value crosses into a different
gateway-visible state. The runtime may coalesce redundant `device.changed`
events under queue pressure, but must retain accepted `device.value` reports
within its configured safety limits.

### `events.off(handle)`

Unregisters a subscription. Calling it for an already removed handle is safe.

### `events.wait_for(filter[, timeout_seconds])`

Suspends the current callback coroutine until a matching event arrives. It
returns the event, or `nil, "timeout"` after the optional timeout. The wait is
canceled if its script reloads, is disabled, is removed, or MicroHAD restarts.

```lua
local event, reason = events.wait_for({
  type = "device.changed", device = "Office sensor",
  field = "temperature_celsius", value_at_least = 18,
}, 1800)
```

## Devices and readings

### `devices.get(device)`

Returns a current immutable dashboard-visible snapshot, or `nil, error_message`.
It is the preferred way for a timer or coroutine to recheck state before a
consequential action.

```lua
local sensor, err = devices.get("Office sensor")
if sensor and sensor.temperature_celsius and sensor.temperature_celsius < 18 then
  -- The reading is still low.
end
```

A snapshot always has `id`, `name`, `source`, `kind`, `online`, and `last_seen`.
It contains supported dashboard fields such as `temperature_celsius`,
`humidity_percent`, `contact_open`, `water_detected`, `occupied`, `light_on`,
`brightness_percent`, `actor_on`, `power_watts`, `battery_percent`, and link
quality values. A missing capability is `nil`, never false or zero.

### `devices.list([filter])`

Returns snapshots matching optional `source`, `kind`, `online`, and `name`
filter fields. It is for initialization and inspection, not high-rate polling.

### `devices.actor(device, on)`

Queues an actor on/off command. Returns `true` when accepted by the gateway
queue, or `nil, error_message` if the target cannot act.

### `devices.light(device, state)`

Queues light state. `state.on` is an optional boolean and `state.brightness` is
an optional integer from 0 through 100; at least one is required.

```lua
local ok, err = devices.light("Kitchen light", { on = true, brightness = 65 })
if not ok then log("warn", "cannot set kitchen light: " .. err) end
```

### `devices.thermostat(device, field, value)`

Queues one thermostat setting for a Zigbee or Matter thermostat. Returns `true`
on acceptance or `nil, error_message`. `value` must be a string or finite
number. Field names, Celsius units, reported limits, capabilities, and schedule
formats follow the [thermostat API](../microhad/docs/thermostats.md#http-api).
For example, `system_mode` uses `0` off, `1` auto, `3` cool, and `4` heat where
supported. Numeric strings are accepted; booleans and tables are not.

```lua
local ok, err = devices.thermostat("Office thermostat", "occupied_heating_setpoint", 21)
if not ok then log("warn", "Cannot set heating target", { error = err }) end
```

Use `schedule_0` through `schedule_6` (Sunday through Saturday) with a schedule
JSON string, or `"__read__"` to request a day's schedule. Supported model-specific
settings and temperature-source settings use the same field names as the
HTTP API; a linked temperature source must be supplied as its stable device ID.
Each call changes one field; multiple calls are separate requests, not an
atomic transaction.

Thermostat snapshots have an immutable `thermostat` object matching the
dashboard representation: `values`, `supported`, `fields`, `modes`, schedule
capabilities, `schedules`, `schedule_details`, `pending`, `source_id`,
`stale_minutes`, `source_observed_at`, `status`, and `error`. Lua arrays are
1-based: Sunday is slot 1 of `schedules` and `schedule_details`; unknown entries
are `nil`. Devices without this capability omit `thermostat`.

A changed thermostat snapshot emits `device.changed` with `field = "thermostat"`
and immutable `old` and `value` objects, including pending/error-only changes.
A queued request is not a confirmed setting. Observe reported values, pending
fields, and errors; execution failures also appear in gateway diagnostics.
Validation checks settings without submitting commands or changing pending state.

### `devices.refresh(device)`

Requests a normal gateway refresh. It returns whether the request was accepted;
the refreshed value arrives later as a `device.changed` event.

## Scenes

### `scenes.activate(group, scene)` and `scenes.off(group)`

Queue activation of a configured scene or switch off a configured scene group.
Each returns `true` on queue acceptance or `nil, error_message`.

```lua
scenes.activate("Downstairs", "Evening")
-- scenes.off("Downstairs")
```

### `scenes.get(group)`

Returns an immutable group snapshot or `nil, error_message`. It accepts the same
stable string IDs and unique case-insensitive names as scene actions.

| Field | Meaning |
| --- | --- |
| `id`, `name` | Group string ID and display name |
| `scene_id`, `scene_name` | Last selected configured scene, omitted if unknown or removed |
| `off` | Gateway group-off state, omitted if unknown |

Turning a group off retains its last selected scene. Selection is the gateway's
runtime state after its scene recall path updates it, not physical confirmation
that every lamp matches. Pending requests may still show the previous selection.
Individual light changes do not recalculate the selection. Changes made through
the dashboard, Lua, or gateway remote rules are visible in subsequent snapshots.
Snapshots do not change in place; call `scenes.get()` again for fresh state.
There is no scene-change event.

```lua
local group, err = scenes.get("Downstairs")
if group then
  dump(group, "gateway scene selection")
else
  log("warn", "Cannot read scene group", { error = err })
end
```

## Timers and schedules

Timers and schedules are runtime-only and are discarded on reload, disable,
source removal, and daemon restart. Initialization must recreate valid work.

### `automation.after(seconds, callback[, payload])`

Creates a one-shot timer and returns an opaque handle. `callback(payload)` runs
after a positive number of seconds. It may be an anonymous or nested function.

### `automation.every(seconds, callback[, payload])`

Creates a repeating interval timer and returns an opaque handle. The next run
is scheduled after the previous callback completes; missed intervals never run
in a burst.

### `automation.cancel(handle)`

Cancels a pending one-shot, interval, sleep, cron, or wait-timeout handle.
Returns `true` if it canceled work and `false` if it had already completed.

### `automation.sleep(seconds)`

Suspends the current callback coroutine for a positive duration without blocking
the automation worker or the gateway coordinator. The suspension is canceled
when its script is unloaded.

### `automation.cron(expression, callback)`

Registers a local-time five-field cron schedule (`minute hour day-of-month month
day-of-week`) and returns a cancelable handle. Missed calendar runs are not
replayed after downtime. DST follows the gateway's local system clock.

## Value history

### `history.query(device, options)`

Reads retained history by stable device ID or unique name. Call only inside a
callback: the calling coroutine suspends until the query finishes, then receives
an immutable page or `nil, error`. Top-level initialization and validation cannot
read history. Other callbacks and events continue to run between storage pages.

```lua
automation.every(900, function()
  local page, err = history.query("Living room window", {
    field = "contact_open", value = true, limit = 1,
  })
  if page and page.samples[1] then
    log("info", "Last recorded opening", { at = page.samples[1].at })
  elseif err then
    log("warn", "History query failed", { error = err })
  end
end)
```

`options.field` is required. Supported fields are `temperature_celsius`,
`humidity_percent`, `contact_open`, `power_watts`, `occupied`, `battery_percent`,
`co2_ppm`, `pm25_micrograms_m3`, and `air_quality`. Other fields have no queryable
recorded history. Sources excluded from history return empty results.

Optional options:

- `value`: equality filter, applied before the result limit. Use booleans for
  contact/occupancy, an air-quality string for air quality, and finite numbers
  for other fields. Numeric comparisons use the history's stored float precision.
- `since`: inclusive Unix timestamp in seconds, default zero. No predecessor
  samples outside this interval are included.
- `before`: exclusive history cursor, default zero (newest records).
- `limit`: integer from 1 to 256, default 128.

The page contains newest-first `samples`, each with `cursor`, `at` (Unix seconds),
`value`, and `unit`. Boolean readings are Lua booleans; air quality uses its usual
string labels. `next_before` is zero when exhausted; otherwise pass it as `before`
to continue with the same filters. Continuation is conservative: the next page
may be empty if no further matching records remain.

Reads run in the automation thread, with at most one history storage page read
per normal worker-loop iteration globally. Concurrent reads take turns. Queries
keep only indexed page metadata and a bounded result set, and read until the
requested number of matches is established or eligible history is exhausted.
There are at most 16 outstanding queries; capacity errors can be retried later.
Reload, disable, removal, or callback cancellation discards unfinished queries.
Scripts that never call this API perform no history reads or query allocation.

A query uses the indexed pages present when it starts, rather than a transactional
snapshot. Later appends are excluded; reused or unreadable pages return an error
so the caller can retry. Retained history can be incomplete or evicted, and no
matching record does not prove a value never occurred. Recheck current device
state after a query before sending a notification or controlling a device.

## Notifications and logs

### `notify.send(title, body[, options])`

Queues a user notification using MicroHAD's configured notification service.
The required title and body are strings. Optional `options.device_id` and short
`options.tag` identify the related device and allow client grouping. It returns
`true` after acceptance or `nil, error_message` when notifications are
unavailable or disabled. Credentials are never exposed to Lua.

### `log(level, message[, fields])`

Writes a structured automation log. `level` is `debug`, `info`, `warn`, or
`error`; `message` is a short string; `fields` is an optional JSON-compatible
table. Entries automatically contain the script ID and generation.

```lua
log("warn", "low temperature remains", {
  device = "Office sensor", temperature = 17.2,
})
```

### `dump(value[, label])`

Writes a debug-level, recursive representation of any Lua value, primarily for
inspecting event, device, and state tables while authoring a script. `label` is
an optional short prefix.

```lua
events.on({ type = "device.value", device = "Office sensor" }, function(event)
  dump(event, "office value event")
  dump(devices.get("Office sensor"), "current office snapshot")
end)
```

`dump` walks tables, shows their keys and value types, and detects cycles rather
than recursing indefinitely. It attempts to include every field of a supported
table, but enforces an overall log-record byte limit; overlong output ends with
an explicit truncation marker. Functions, threads, userdata, and opaque handles
are represented by their type and identity rather than being executed or
dereferenced.

## State and utility functions

### `persistent`

This injected per-script table-like proxy is the only script data that survives
reload or reboot. It accepts nil, booleans, finite numbers, strings, and
acyclic tables with string keys. Writes commit atomically after successful
initialization or callback completion.

```lua
persistent.alert_count = (persistent.alert_count or 0) + 1
persistent.last_alert_at = automation.now()
```

### `shared`

This injected table-like proxy is shared by currently loaded scripts. It
survives an individual script reload but clears on daemon restart. It supports
the same serializable values as `persistent`; functions, userdata, threads, and
cyclic tables are rejected so a retired script's closures cannot remain live.

### `automation.now()` and `automation.script_id()`

`automation.now()` returns Unix time in seconds. `automation.script_id()`
returns the current script's file-stem identifier. Neither is a replacement for
timer APIs or a busy wait.

## Errors, limits, and ordering

Callbacks run as cooperative Lua coroutines on the automation worker. The
runtime enforces execution-time, instruction, memory, recursion, timer, and
queue limits. An uncaught callback error or limit breach is logged and aborts
only that callback; it cannot stall coordinator polling or unrelated scripts.

Device actions are serialized through the coordinator. A successful call means
the action was accepted for execution, not that a physical device has already
confirmed it. Resulting state changes are delivered later as queued events,
never recursively during an action call.

The API excludes arbitrary HTTP/webhooks, custom native modules, shell and
filesystem access, raw Zigbee/Matter commands, pairing, and network
administration. Those need separate security and lifecycle designs.
