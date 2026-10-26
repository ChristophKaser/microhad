# Lua automation examples

These files are examples to copy into the dashboard's **Automations** editor.
Deployment does not install or enable them. Create a separate script for each
example you want, replace its device and scene names, then **Validate** and
**Save and reload**. Unique names and stable device IDs both work; stable IDs
survive renaming. Save each example under a different filename so its persistent
state stays separate.

| Script | Behavior | Setup |
| --- | --- | --- |
| [Window airing](window-airing.lua) | Checks every 15 minutes during 07:00–23:00; reminds you once after more than four hours without opening the window. | Replace the window name; enable notifications. Uses retained history, with a four-hour fallback when no opening is known. Overnight time counts. |
| [Low battery](low-battery.lua) | Warns once below 15% and again below 10%; resets when charge reaches 15%. Checks existing devices when loaded. | Enable notifications. No device names to replace. Saving may immediately send alerts. |
| [Remote discovery](remote-discovery.lua) | Dumps remote events to debug logs so you can discover actions, endpoints and press counts. | Replace the remote name; press its buttons and inspect the script logs. |
| [Remote light](remote-light.lua) | On selects 65% brightness; off turns the light off. | Replace the Zigbee remote and light names. Omit brightness for a light without dimming. |
| [Remote actor toggle](remote-toggle-actor.lua) | Toggles a plug or relay using its latest reported state. | Replace the button and actor names; adjust the action and endpoint for your remote. Unknown states are skipped. |
| [Remote scenes](remote-scenes.lua) | Right arrow selects Evening; left arrow switches the group off. | Create the scene/group, then replace their names and the remote name/actions. |
| [Scheduled scene](scheduled-scene.lua) | Selects Evening at 18:00 in gateway local time. | Create and name the scene/group; edit the cron expression for your preferred time. |
| [Scene inspection](scene-inspection.lua) | Logs a group's selected scene and group-off state when a button is pressed. | Replace the remote and group names/action. This reads gateway selection, not physical lamp state. |
| [Remote thermostat](remote-thermostat.lua) | On selects heating at 21°C; off selects off mode. Logs reported state and errors. | Replace the thermostat/remote names and actions; confirm heating/off support and target limits. |
| [Thermostat weekly schedule](thermostat-weekly-schedule.lua) | A button writes Monday's heating schedule: 17°C at midnight, 21°C at 07:00, 17°C at 22:00. | Replace the button/thermostat names and action; check the thermostat's schedule and setpoint limits. The schedule runs on the thermostat. |

Use remote discovery first when adapting a remote example. Zigbee and Matter
buttons can report different action names and endpoints. Existing gateway scene
rules still run for the same buttons, so remove conflicting rules when adopting
a Lua remote script. Device commands are queued; device reports confirm their
physical effect. Cron schedules use the gateway's local clock and do not replay
runs missed during downtime.

See [automation scripting](../../../docs/scripting.md), the
[Lua API](../../../docs/lua_scripting_api.md), and
[script lifecycle](../../../docs/lua_script_lifecycle.md) for notification setup,
capabilities, history reads, persistence and reload behavior.
