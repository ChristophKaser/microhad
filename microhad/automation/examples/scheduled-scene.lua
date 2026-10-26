-- Replace Downstairs and Evening. Runs at 18:00 in gateway local time.
automation.cron("0 18 * * *", function()
  local ok, err = scenes.activate("Downstairs", "Evening")
  if not ok then log("warn", "Cannot activate evening scene", { error = err }) end
end)
