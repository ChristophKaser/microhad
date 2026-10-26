# Automation scripting

MicroHAD automation scripts are Lua source files stored on the gateway. They
can be edited from the dashboard or over SSH. The source file is always the
source of truth: the dashboard does not maintain a separate copy of a script.

Lua automation is built into the native daemon. The dashboard's Automations
tab provides script management, a plain-text editor, validation, status and logs.

See [Lua scripting API](lua_scripting_api.md) for available functions and
[Lua script lifecycle](lua_script_lifecycle.md) for reload, timer, coroutine,
and persistent-state semantics.

## Listening for remote buttons

Use `events.on({ type = "remote.action", ... }, callback)` for recognized
Zigbee and Matter remote actions. The event has the remote's device snapshot,
normalized `action`, and `endpoint`; Matter button events may also carry
`position` and `press_count`. See the API reference for action names and a
complete example. Remote events continue to reach automations when the same
button is configured with a gateway-scene rule.

## Example scripts

Copy each Lua example into its own script in **Automations**, replace the
example names with your devices and configured scene groups, then **Validate**
and **Save and reload**. Names must resolve uniquely; string stable IDs also
work and survive renaming. These examples register callbacks, so saving them
does not immediately switch devices. A successful command means it was queued;
check device reports for the resulting physical state.

### Remind me to open a window

The [window-airing example](../microhad/automation/examples/window-airing.lua)
checks every 15 minutes from 07:00 through 22:45 in gateway local time. Replace
its sensor name, validate, and save/reload it in Automations. Enable gateway
notifications first.

It reads the latest recorded opening and alerts once after more than four hours.
Overnight elapsed time counts, so a reminder may arrive at 07:00. An open window
suppresses alerts. If history has no retained opening, it waits four hours from
first installation; this fallback and notification deduplication survive restarts.
History errors suppress the reminder until a later successful check. Stored
history and observed opening events are combined, and current state is rechecked
after each suspended read.

```lua
-- Replace with your window sensor's unique name or stable ID.
local WINDOW = "Living room window"
local FOUR_HOURS = 4 * 60 * 60

if persistent.observed_since == nil then
  persistent.observed_since = automation.now()
end

-- Callback-local persistent transactions are isolated while suspended. Keep
-- observed openings in an upvalue too, so a quick open/close during a read wins.
local observed_open = persistent.last_open_at
local function opened()
  observed_open = automation.now()
  persistent.last_open_at = observed_open
  persistent.alerted_for = nil
end

events.on({
  type = "device.changed", device = WINDOW,
  field = "contact_open", value = true,
}, opened)

local function check_window()
  local window = devices.get(WINDOW)
  if not window or window.contact_open == nil then return end
  if window.contact_open then opened(); return end

  -- This callback suspends while history pages are read. Other callbacks run.
  local page, err = history.query(WINDOW, {
    field = "contact_open", value = true, limit = 1,
  })
  if not page then
    log("warn", "Cannot read window history", { error = err })
    return
  end

  -- The window may have changed while this callback was suspended.
  window = devices.get(WINDOW)
  if not window or window.contact_open ~= false then return end
  local last_open = observed_open
  if page.samples[1] then
    last_open = math.max(last_open or 0, page.samples[1].at)
  end
  if last_open then
    -- Retain a known opening if its history page is later evicted.
    observed_open = last_open
    persistent.last_open_at = last_open
  end
  local since = last_open or persistent.observed_since
  if automation.now() - since <= FOUR_HOURS then return end
  if persistent.alerted_for == since then return end

  local ok, notify_err = notify.send(
    "Time to air the room",
    window.name .. " has not been opened for more than four hours.",
    { device_id = window.id, tag = "window-airing" }
  )
  if ok then
    persistent.alerted_for = since
  else
    log("warn", "Window alert failed", { error = notify_err })
  end
end

automation.cron("*/15 7-22 * * *", check_window)
```

### Low-battery warnings for any device

This script watches every device with a reported battery percentage; no device
names need replacing. It warns once below 15% and again below 10%, with warning
state saved across reloads and restarts. Both warnings reset when the battery
reaches 15% again. If a device is first seen below 10% (or drops directly below
10%), only the more urgent warning is sent. Exactly 15% and 10% do not trigger
their respective warnings.

The script also checks existing devices when loaded, so saving it may
immediately queue notifications. Enable and configure gateway push notifications
first. A failed notification request leaves the warning eligible for retry on
the next battery report.

```lua
local function check_battery(device)
  local battery = device.battery_percent
  if type(battery) ~= "number" then return end

  local warnings = persistent.warnings or {}
  local stage = warnings[device.id] or 0

  if battery >= 15 then
    if stage ~= 0 then
      warnings[device.id] = nil
      persistent.warnings = warnings
    end
    return
  end

  local next_stage = battery < 10 and 2 or 1
  if next_stage <= stage then return end

  local ok, err = notify.send(
    next_stage == 2 and "Very low battery" or "Low battery",
    device.name .. ": " .. tostring(battery) .. "% remaining",
    { device_id = device.id, tag = "low-battery" }
  )

  if ok then
    warnings[device.id] = next_stage
    persistent.warnings = warnings
  else
    log("warn", "Battery notification failed", {
      device = device.id,
      error = err,
    })
  end
end

events.on({
  type = "device.value",
  field = "battery_percent",
}, function(event)
  check_battery(event.device)
end)

events.on({ type = "device.added" }, function(event)
  check_battery(event.device)
end)

for _, device in ipairs(devices.list()) do
  check_battery(device)
end
```

### Discover a remote's buttons

Save this first, press the buttons, and inspect the script's debug logs. Use
the reported `action` and `endpoint` (and Matter `position` or `press_count`
when present) in subsequent filters.

```lua
events.on({ type = "remote.action", device = "Hall remote" }, function(event)
  dump(event, "remote button")
end)
```

Zigbee actions include `on`, `off`, `toggle`, `move`, `step`, `stop`,
`arrow_left`, and `arrow_right`. Matter Generic Switch actions include
`matter_press`, `matter_hold`, `matter_release`, and `matter_multi_press`.
See [Matter switch models](lua_scripting_api.md#matter-switch-models) for which
Matter switches produce these events. An existing gateway-scene rule also
runs for the same button; remove conflicting rules when using Lua instead.

### Remote controls a light

This Zigbee example maps the on button to 65% brightness and the off button to
off. For a light without dimming, omit `brightness`.

```lua
local remote = "Hall remote"
local light = "Hall light"

local function set_light(state)
  local ok, err = devices.light(light, state)
  if not ok then log("warn", "Cannot control hall light", { error = err }) end
end

events.on({ type = "remote.action", device = remote, action = "on" }, function()
  set_light({ on = true, brightness = 65 })
end)
events.on({ type = "remote.action", device = remote, action = "off" }, function()
  set_light({ on = false })
end)
```

### Remote toggles an actor (plug or relay)

Read `actor_on` from a fresh snapshot before toggling. An unknown state is
`nil`; do not treat it as off. Rapid presses before the device reports its
new state can request the same target state repeatedly.

```lua
local actor = "Desk plug"

events.on({
  type = "remote.action", device = "Desk button",
  action = "matter_press", endpoint = 1,
}, function()
  local device, err = devices.get(actor)
  if not device or device.actor_on == nil then
    log("warn", "Actor state unavailable", { error = err, device = actor })
    return
  end
  local ok, action_err = devices.actor(actor, not device.actor_on)
  if not ok then log("warn", "Cannot toggle actor", { error = action_err }) end
end)
```

For a Zigbee toggle button, replace the action with `toggle` and use the
endpoint observed in the discovery example. To request a definite state,
use `devices.actor(actor, true)` or `devices.actor(actor, false)`.

### Remote selects a scene or turns a group off

Create the group and its scenes in **Scenes & groups** first. This example
selects Evening with the right arrow and switches the group off with the left
arrow. Replace the actions with those emitted by your remote.

```lua
local group = "Downstairs"

events.on({
  type = "remote.action", device = "Hall remote", action = "arrow_right",
}, function()
  local ok, err = scenes.activate(group, "Evening")
  if not ok then log("warn", "Cannot activate scene", { error = err }) end
end)
events.on({
  type = "remote.action", device = "Hall remote", action = "arrow_left",
}, function()
  local ok, err = scenes.off(group)
  if not ok then log("warn", "Cannot turn group off", { error = err }) end
end)
```

To select a scene on a schedule instead, use a separate script:

```lua
automation.cron("0 18 * * *", function()
  local ok, err = scenes.activate("Downstairs", "Evening")
  if not ok then log("warn", "Cannot activate evening scene", { error = err }) end
end)
```

This runs at 18:00 in the gateway's local time. Missed runs during downtime
are not replayed.

### Get the current scene of a group

Use `scenes.get()` to read the gateway's selected scene and group-off state.
For example, this button logs the selection without changing it:

```lua
events.on({
  type = "remote.action", device = "Hall remote", action = "matter_press",
}, function()
  local group, err = scenes.get("Downstairs")
  if not group then
    log("warn", "Cannot read scene group", { error = err })
    return
  end
  dump(group, "current scene group")
  if group.off == true then
    log("info", "Downstairs is switched off")
  elseif group.scene_id then
    log("info", "Selected scene: " .. group.scene_name)
  else
    log("info", "No known scene selection")
  end
end)
```

`scene_id` and `scene_name` are absent before a known selection or after the
selected scene is removed. `off` is absent when unknown. Turning a group off
retains its last selected scene. The getter follows gateway selection, including
dashboard and remote rules; it does not infer a scene from individual lamp
reports or confirm that every light still matches. A pending recall can still
show the previous selection. See [`scenes.get()`](lua_scripting_api.md#scenesgetgroup).

### Remote controls a thermostat

This Zigbee remote selects heating at 21°C with its on button and off mode
with its off button. Replace the actions with those reported by your remote.
The thermostat must support heating and off modes. Each setting is a separate
queued request; reported state changes when the device confirms it.

```lua
local thermostat = "Office thermostat"
local function set_setting(field, value)
  local ok, err = devices.thermostat(thermostat, field, value)
  if not ok then log("warn", "Cannot control thermostat", { field = field, error = err }) end
end

events.on({ type = "remote.action", device = "Office remote", action = "on" }, function()
  set_setting("occupied_heating_setpoint", 21)
  set_setting("system_mode", 4)
end)
events.on({ type = "remote.action", device = "Office remote", action = "off" }, function()
  set_setting("system_mode", 0)
end)
events.on({
  type = "device.changed", device = thermostat, field = "thermostat",
}, function(event)
  dump(event.value, "thermostat reported state and pending settings")
  if event.value.error ~= "" then
    log("warn", "Thermostat request failed", { error = event.value.error })
  end
end)
```

Read current settings with `devices.get(thermostat).thermostat`; inspect `values`,
`supported`, `fields`, and `modes` before choosing targets for your model.
Unsupported settings return an error rather than silently changing another
capability. Cooling uses `occupied_cooling_setpoint` and mode `3` when supported.

### Set a thermostat weekly schedule

Weekly schedules execute on the thermostat itself. This example copies a
heating schedule to Monday when the schedule button is pressed. Day fields
are `schedule_0` (Sunday) through `schedule_6` (Saturday); transition times are
minutes after midnight. Follow the thermostat's transition limits and setpoint
range. Some models require a transition at midnight.

```lua
events.on({
  type = "remote.action", device = "Schedule button", action = "matter_press",
}, function()
  local schedule = '{"mode":1,"transitions":[{"time":0,"heat":17},{"time":420,"heat":21},{"time":1320,"heat":17}]}'
  local ok, err = devices.thermostat("Office thermostat", "schedule_1", schedule)
  if not ok then log("warn", "Cannot set Monday schedule", { error = err }) end
end)
```

Use `"__read__"` as the value to request the device's schedule instead. See the
[Lua thermostat API](lua_scripting_api.md#devicesthermostatdevice-field-value)
and [thermostat settings](../microhad/docs/thermostats.md#http-api) for additional
standard and model-specific settings, external temperature sources, and formats.

## Script location and names

Enabled scripts are `*.lua` files in:

```sh
/tuya/native/automations/
```

The filename without `.lua` is the script ID. For example,
`/tuya/native/automations/office-temperature.lua` has the ID
`office-temperature`. A script's `persistent` values belong to that ID, so
renaming a file intentionally gives it a new persistent-state namespace.

To disable a script without deleting its source, rename it with a
`.lua.disabled` suffix. Disabled source is retained but is not loaded.

```sh
# gateway, over SSH
mv /tuya/native/automations/office-temperature.lua \
  /tuya/native/automations/office-temperature.lua.disabled
```

## Editing in the Web UI

The dashboard's **Automation scripts** page lists every script with its ID,
source filename, enabled state, currently loaded generation, last reload time,
and most recent load or callback error.

1. Select a script, or choose **New script** and give it a filename ending in
   `.lua`.
2. Edit the source in the editor. The **Validate** action compiles and evaluates
   the candidate in an isolated temporary environment; it does not register
   listeners, run timers, write `persistent`, or control devices.
3. Choose **Save and reload** to atomically replace the source file and reload
   it. The new script starts from top-level initialization code.
4. Use **Enable** or **Disable** to change the source suffix without deleting
   it. Use **Delete** only when the source is no longer wanted; deletion does
   not erase persistent values.

The editor writes a temporary file in the automation directory, fsyncs it, and
renames it into place. SSH users therefore see the same source as the UI and
never a partly-written file.

## Script editor

The current editor uses a plain-text area with native undo/redo and Ctrl/Cmd-S
to save. Before selecting another script, creating a draft, leaving the tab or
closing the browser, it protects unsaved edits. Runtime diagnostics and log
records link to the affected source line.

Syntax highlighting, a formatter, completion, inline diagnostics, and the target
picker described below are deferred editor enhancements; they are not controls
in the current plain-text editor.

### API help and diagnostics

Planned completion and inline reference help will cover the supported Lua API, including
event filters, device and scene actions, timers, `persistent`, `shared`,
`log()`, and `dump()`. This help follows the [Lua scripting API](lua_scripting_api.md);
it does not advertise shell, filesystem, networking, or raw protocol access.

Validation and callback errors appear in status and **Logs and status**, with
source lines when Lua supplies them. Validation includes invalid arguments,
unsupported capabilities, and initialization errors.
Validation is isolated and side-effect-free: it does not register listeners,
run timers, write `persistent`, or send device actions or notifications.

### Insert devices and scenes

The planned **Insert target** picker will search current gateway devices, scene groups,
and scenes. For each result, it shows the display name, stable ID, source, and
capability where relevant. It inserts either a quoted display name or a quoted
stable ID at the current cursor position, selected by the author.

Names keep scripts readable but can become ambiguous after a rename or when
two objects share a name. The picker warns about ambiguous names before
insertion; stable IDs avoid that ambiguity. It never rewrites existing script
source after a device or scene changes name.

### Save, reload, and status controls

Draft content remains in the browser until the author explicitly chooses an
action:

- **Validate** checks the draft without changing the source file or live
  script.
- **Save** atomically writes the source file but does not request a dashboard
  reload; the normal file-change watcher queues the reload.
- **Save and reload** atomically writes the source and explicitly queues the
  selected script for reload.
- **Reload** and **Reload all** reload existing source files without changing
  them. **Enable**, **Disable**, **Delete**, and **Reset persistent state**
  remain separate, explicit actions.

The editor displays the loaded generation, last reload time, enabled state,
last successful validation, and the most recent load or callback failure.
Reload and persistent-state behavior are defined in [What reload does](#what-reload-does),
not by the editor.

## Editing over SSH

Use an editor that writes atomically when possible. A simple safe workflow is
to write a separate temporary filename and rename it after syntax checking.

```sh
# gateway, over SSH
vi /tuya/native/automations/office-temperature.lua.next
mv /tuya/native/automations/office-temperature.lua.next \
  /tuya/native/automations/office-temperature.lua
```

MicroHAD detects a completed source-file replacement and queues a reload. The
dashboard also provides **Reload** for a selected script and **Reload all**
when several scripts were changed. Reload requests are serialized with script
callbacks; repeated reload requests for the same file coalesce to the newest
file contents.

Do not edit a live `.lua` file in place if the editor truncates it before
rewriting. MicroHAD may observe that intermediate incomplete version and leave
the script unloaded after the resulting load error. Atomic rename avoids this.

## What reload does

Reload is a fresh-start boundary for that script:

- Its listeners, cron schedules, timers, coroutines, ordinary globals, locals,
  and closures are discarded.
- Its Lua file is evaluated again from the top. Initialization code must
  register desired listeners and inspect present device state to start any
  still-valid timers.
- Its `persistent` table remains intact. The process-wide nonpersistent
  `shared` table also remains intact across an individual script reload.
- If the replacement cannot compile or initialization fails, the old script is
  not restored; the script is unloaded and the dashboard records the error.

The **Reset persistent state** dashboard action deletes only the selected
script's `persistent` data after a confirmation. It is separate from reload,
disable, and delete so an edit cannot silently discard durable script values.

## Debugging from the Web UI

Open a script's **Logs and status** panel to inspect:

- load, reload, syntax, and initialization errors with source line numbers;
- uncaught callback errors and resource-limit breaches;
- active listener, cron, timer, and suspended-coroutine counts;
- last event and callback timestamps; and
- structured records written by `log()` and `dump()`.

Use `log(level, message[, fields])` for intentional operational messages:

```lua
log("info", "office temperature check", { temperature = 17.2 })
```

Use `dump(value[, label])` while developing to inspect a complete Lua table in
the debug log. It handles recursive tables safely and marks output that exceeds
the log-size limit.

```lua
events.on({ type = "device.value", device = "Office sensor" }, function(event)
  dump(event, "office report")
  dump(devices.get("Office sensor"), "current device state")
end)
```

The UI supports filtering script logs by severity and time, copying a record,
and clearing the displayed log view. Clearing the view does not erase the
gateway's retained system log.

## Recovering from a broken script

An error in one script does not stop the gateway or other scripts. To recover:

1. Use the dashboard error location and log record to identify the fault.
2. Fix the source in the editor or through an SSH atomic replacement.
3. Validate, then reload the script.
4. If needed, disable the script first; this cancels its pending timers and
   coroutines immediately.

After a MicroHAD restart every enabled source file is evaluated afresh. Timers
and coroutines do not survive restart, so scripts should rebuild required work
from current device state during initialization and reserve `persistent` for
the values that genuinely need to survive reboot.

## Runtime bounds and HTTP interface

Each script has a 1 MiB Lua heap, 200,000 instructions and 200 ms of active
execution per resume (fresh budgets after every cooperative yield), a 128-frame
recursion limit, 128 listeners, 64 timers/cron registrations and 128 pending
callbacks. Source is limited to 256 KiB and persistent state to 128 KiB per ID.
The worker accepts 512 queued events and the coordinator accepts 128 automation
actions. Overflow produces an explicit error record. Only one ready callback
per script is resumed per scheduler turn so other scripts keep making progress.

Management requests have a 64-entry queue, accepted device reports a 512-entry
buffer, and causal chains a 32-action depth limit. Script IDs allow 192 characters.
Serializable Lua tables allow 48 nested levels; JSON decoding allows 64 levels
and 16,384 nodes. Log records are capped at 8 KiB. Notifications allow a
512-byte title, 8 KiB body and 256-byte option values. Timer durations may be up
to two years. These scripting limits are doubled from the initial implementation;
shared transport queues and ordinary HTTP request limits remain unchanged.

Source writes, enable/disable and deletion are serialized on the worker before
watcher discovery, preventing duplicate initialization from one management
operation. Queued device actions are discarded if their generation has retired
before execution. Recurring timers skip a callback rejected by a full task queue
and remain scheduled for their next interval or matching cron minute.

Persistent checkpoints are private JSON objects under `automations/state/`.
Successful callback completion atomically checkpoints changed values; errors
discard that callback's writes. Suspended callbacks keep their transaction,
and independent keys committed by other callbacks are preserved. Reset clears
the selected ID's values without reloading its source or deleting other state.
Source discovery compares file contents every 250 ms, including same-size edits.
Enabled/disabled files with the same ID cause a load error; neither file is
silently overwritten. All source and checkpoint operations reject symlinks.

`GET /api/automations` lists scripts. `GET /api/automations/FILENAME` returns
`{filename, source}` and `GET /api/automations/logs` returns structured records.
POST requests use URL-encoded form bodies:

| Endpoint | Fields |
| --- | --- |
| `/api/automations/validate` | `filename`, draft `source` |
| `/api/automations/save` | `filename`, `source`, optional `reload=0` for watcher-only reload |
| `/api/automations/reload` | `filename`, or `all` for all scripts |
| `/api/automations/enable`, `/disable`, `/delete` | current `filename` |
| `/api/automations/reset` | `filename` whose persistent namespace is reset |

Script logs retain the latest 512 records in memory and also use syslog for
operational output. UI clearing changes only its display cursor. Validation
diagnostics are retained, while draft `log()`/`dump()` calls do not publish
operational output.

