-- Replace Hall remote and Hall light. Omit brightness for non-dimmable lights.
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
