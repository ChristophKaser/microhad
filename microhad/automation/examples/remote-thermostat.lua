-- Replace Office remote/thermostat; thermostat must support heating and off modes.
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
