-- Replace Schedule button/Office thermostat; check thermostat setpoint and transition limits.
events.on({
  type = "remote.action", device = "Schedule button", action = "matter_press",
}, function()
  local schedule = '{"mode":1,"transitions":[{"time":0,"heat":17},{"time":420,"heat":21},{"time":1320,"heat":17}]}'
  local ok, err = devices.thermostat("Office thermostat", "schedule_1", schedule)
  if not ok then log("warn", "Cannot set Monday schedule", { error = err }) end
end)
