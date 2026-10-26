-- Replace Hall remote, Downstairs and Evening; create the group/scene first.
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
