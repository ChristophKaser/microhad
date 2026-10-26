-- Replace Hall remote and Downstairs; logs the gateway scene selection.
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
