-- Watches all devices; enable gateway notifications before saving.
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
