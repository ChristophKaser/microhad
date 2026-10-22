# Automation scripting

MicroHAD automation scripts are Lua source files stored on the gateway. They
can be edited from the dashboard or over SSH. The source file is always the
source of truth: the dashboard does not maintain a separate copy of a script.

This describes the intended scripting workflow while the feature is being
implemented; it is not an indication that a released gateway already exposes
the described dashboard controls.

See [Lua scripting API](lua_scripting_api.md) for available functions and
[Lua script lifecycle](lua_script_lifecycle.md) for reload, timer, coroutine,
and persistent-state semantics.

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

The editor is intended to make ordinary automation authoring comfortable
without turning the dashboard into a full debugger. It provides Lua syntax
highlighting, line numbers, bracket matching, undo/redo, find and replace, and
the normal keyboard shortcuts for saving and searching. Before changing the
selected script or leaving the page, it asks what to do with unsaved edits.

The editor offers **Format source**, which formats only the in-browser draft.
Formatting never saves, validates, reloads, or otherwise changes the gateway
source until **Save** or **Save and reload** is selected.

### API help and diagnostics

Completion and inline reference help cover the supported Lua API, including
event filters, device and scene actions, timers, `persistent`, `shared`,
`log()`, and `dump()`. This help follows the [Lua scripting API](lua_scripting_api.md);
it does not advertise shell, filesystem, networking, or raw protocol access.

Syntax and API diagnostics appear inline at their source line and column.
They include invalid arguments, unsupported capabilities, and initialization
errors. Each diagnostic links to the matching record in **Logs and status**.
Validation is isolated and side-effect-free: it does not register listeners,
run timers, write `persistent`, or send device actions or notifications.

### Insert devices and scenes

The **Insert target** picker searches current gateway devices, scene groups,
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
automation.on({ type = "device.value", device = "Office sensor" }, function(event)
  dump(event, "office report")
  dump(devices.get("Office sensor"), "current device state")
end)
```

The UI supports filtering script logs by severity and time, copying a record,
and clearing the displayed log view. Clearing the view does not erase the
gateway's retained system log. Do not use `dump()` for credentials, Matter
identity values, network keys, or raw protocol data.

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

