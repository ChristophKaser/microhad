-- Replace Desk button and Desk plug; use the action/endpoint reported by your remote.
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
