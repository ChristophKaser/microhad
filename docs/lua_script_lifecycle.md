# Lua automation script lifecycle

This document specifies the intended lifecycle for Lua automation scripts in
MicroHAD.  It is a design contract for the script runtime and its dashboard
editor; it does not describe a shell, filesystem, network, or raw-radio API.

Scripts are source files in the gateway automation directory.  The source
files are authoritative: they may be edited through SSH or written by the
dashboard editor using an atomic replace.  A script is identified by its file
stem, which also scopes its persistent values and its runtime registrations.

## Execution and isolation

The daemon runs Lua in a dedicated automation worker thread.  The coordinator
thread never evaluates Lua and never waits for a script to finish.  Instead it
publishes immutable device-event snapshots and dashboard-visible state to the
worker through a bounded queue.  The worker submits approved device actions
back through the existing serialized gateway-command path.

Each script has a separate Lua environment.  Loading a script evaluates its
top-level statements once.  This is the script's initialization phase: it
normally registers event listeners, creates cron schedules, reads current
device state, and starts any timers that currently make sense.  A script's
ordinary globals, locals, closures, listeners, schedules, and timers belong
only to that loaded instance.

The worker runs at most one runnable callback for an individual script at a
time, which makes ordinary Lua state predictable.  A callback may cooperatively
yield as a coroutine; while it is suspended, another ready callback for that
script may run.  Separate scripts may be dispatched independently by the
worker scheduler, but the worker must preserve arrival order for events
delivered to one script.

## Events and callbacks

Scripts register ordinary Lua functions; callbacks do not need global names.
For example:

```lua
events.on({
  type = "device.changed",
  device = "Office sensor",
  field = "temperature_celsius",
}, function(event)
  log("info", "temperature is " .. tostring(event.value))
end)
```

An event is an immutable table with at least `type`, `device_id`, `source`,
`field`, `old`, `value`, and `at`.  It may also include causal-chain metadata
for diagnostics.  Device and scene arguments accept either their stable IDs or
their exact current names.  Name matching is case-insensitive, but a name must
resolve to exactly one object; ambiguity and unknown names fail the individual
action and are logged with the stable IDs that can be used instead.

The worker queues script-caused state changes as later events, rather than
calling listeners recursively.  A bounded causal-chain count prevents an
automation loop from repeatedly triggering itself.

## Timers and schedules

Timers are runtime-only.  They are deliberately *not* restored across a
script reload or daemon restart.  A timer may retain an anonymous or nested
function because it only exists as long as its containing Lua state exists.

```lua
local low_temperature_timer

local function send_low_temperature_alert()
  local sensor = devices.get("Office sensor")
  if sensor.temperature_celsius and sensor.temperature_celsius < 18 then
    notify.send("Office", "Temperature is still below 18°C")
  end
  low_temperature_timer = nil
end

events.on({
  type = "device.changed",
  device = "Office sensor",
  field = "temperature_celsius",
}, function(event)
  if event.value < 18 and not low_temperature_timer then
    low_temperature_timer = automation.after(300, send_low_temperature_alert)
  elseif event.value >= 18 and low_temperature_timer then
    automation.cancel(low_temperature_timer)
    low_temperature_timer = nil
  end
end)

-- Initialization is also run after reload or a daemon restart.
local sensor = devices.get("Office sensor")
if sensor.temperature_celsius and sensor.temperature_celsius < 18 then
  low_temperature_timer = automation.after(300, send_low_temperature_alert)
end
```

`automation.after(seconds, callback[, payload])` returns an opaque timer
handle.  `automation.cancel(handle)` prevents a pending callback from running.
`automation.every(seconds, callback[, payload])` creates a repeating timer,
and `automation.cron(expression, callback)` registers a local-time five-field
cron schedule.  Payloads must be JSON-compatible.  Cron schedules do not
replay runs missed while MicroHAD was stopped.

The callback should re-read current device state before doing a consequential
action.  This handles a value that changed while a timer was pending and makes
initialization after a restart safe.

## Coroutines and asynchronous work

Callbacks run in cooperative Lua coroutines.  They never create operating
system threads and must use the automation-provided yield operations rather
than a busy wait.  Coroutines are intentionally runtime-only: a reload,
disable, removal, or MicroHAD restart cancels every suspended coroutine along
with its associated timer or event wait.  Use `persistent` plus initialization
code when a workflow must reconstruct work after a reboot.

`automation.sleep(seconds)` suspends the current callback and resumes it after
the duration.  It is equivalent to an internal one-shot timer, but keeps the
local variables and control flow of the currently loaded script:

```lua
events.on({
  type = "device.changed",
  device = "Office sensor",
  field = "contact_open",
  value = true,
}, function(event)
  devices.actor("Hall light", true)
  automation.sleep(300) -- yields; it does not block the gateway or worker

  -- Check again because the contact may have changed while we slept.
  local contact = devices.get("Office sensor")
  if contact.contact_open then
    notify.send("Office", "Door has remained open for five minutes")
  end
end)
```

`automation.wait_for(filter[, timeout_seconds])` suspends the current
coroutine until a matching normalized event arrives.  It returns the matching
event, or `nil, "timeout"` when the optional timeout expires:

```lua
events.on({
  type = "device.changed",
  device = "Office sensor",
  field = "temperature_celsius",
  value_below = 18,
}, function()
  notify.send("Office", "Temperature is low; waiting for recovery")

  local event, reason = automation.wait_for({
    type = "device.changed",
    device = "Office sensor",
    field = "temperature_celsius",
    value_at_least = 18,
  }, 1800)

  if event then
    notify.send("Office", "Temperature recovered to " .. tostring(event.value) .. "°C")
  elseif reason == "timeout" then
    notify.send("Office", "Temperature remained low for 30 minutes")
  end
end)
```

The worker resumes a coroutine only from its own queue, so it cannot be resumed
twice or run while the script is being torn down.  The same per-callback time,
instruction, memory, recursion, queue, and timer limits apply cumulatively to
each coroutine across its resume steps.  A script should use a regular timer
instead of `sleep` when it needs a simple cancelable delay, and should use
initialization plus `persistent` when the delay must be reconstructed after a
restart.

## Reload, disable, and removal

A source-file change is a fresh-start boundary, not an incremental patch.

1. The worker stops delivering new events to the old script instance and waits
   until its active callback reaches a completion boundary.
2. It cancels all timers, unregisters all event listeners and cron schedules,
   and destroys the old Lua environment.
3. It creates a fresh environment, attaches the script's persistent-value
   proxy, evaluates the replacement source, and activates only registrations
   made by that new evaluation.

If replacement initialization fails, the script remains unloaded.  The prior
instance is not revived: its transient state and registrations were
intentionally discarded at the reload boundary.  The dashboard and log expose
the compilation or initialization failure.

Disabling a script or removing its source file follows the same teardown.  No
callback, timer, or cron registration from the retired instance may run after
the teardown boundary.  Actions already accepted by the gateway command queue
before that boundary remain ordered commands and may complete; newly submitted
actions from the retired generation are rejected.

## Persistent and shared values

Ordinary Lua globals do not persist.  A script must opt in to reboot-persistent
data through its injected `persistent` object:

```lua
persistent.alert_count = (persistent.alert_count or 0) + 1
persistent.last_alert_at = automation.now()
```

`persistent` is private to a script ID and survives both reload and MicroHAD
restart.  It stores only `nil`, booleans, finite numbers, strings, and acyclic
tables with string keys.  Functions, userdata, threads, cyclic tables, and
unsupported keys are rejected.  Changes are atomically checkpointed after a
successful initialization or callback; an explicit dashboard/API reset is
required to delete a script's persistent values.

The injected `shared` object is different: it is a nonpersistent object shared
by all currently loaded scripts.

```lua
shared.quiet_hours = true
if not shared.quiet_hours then
  notify.send("Office", "Temperature is low")
end
```

`shared` survives an individual script reload so that scripts can coordinate
while the daemon is running, but it is cleared on every MicroHAD restart.  It
accepts the same data types as `persistent` and deliberately rejects functions
and userdata.  That avoids retaining closures belonging to a script that has
since been reloaded or removed.

## Resource limits and shutdown

The runtime applies a per-callback instruction/time budget, memory allocation
limit, recursion limit, per-script timer limit, and queue limit.  A script
error or limit breach is logged and leaves the script enabled; it must not
stall coordinator polling or other scripts.  Under queue pressure, superseded
state updates for the same device field may be coalesced, but edge events such
as contact, button, and trigger changes must be retained.

On shutdown, the worker stops accepting new events, cancels outstanding
runtime-only timers, lets active callbacks reach a short bounded completion
point, checkpoints changed `persistent` values, and exits.  On the next
startup MicroHAD evaluates each enabled script from source, so that the script
itself reconstructs valid listeners and timers from current gateway state.
