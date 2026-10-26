-- Replace with your window sensor's unique name or stable ID.
local WINDOW = "Living room window"
local FOUR_HOURS = 4 * 60 * 60

if persistent.observed_since == nil then
  persistent.observed_since = automation.now()
end

-- Callback-local persistent transactions are isolated while suspended. Keep
-- observed openings in an upvalue too, so a quick open/close during a read wins.
local observed_open = persistent.last_open_at
local function opened()
  observed_open = automation.now()
  persistent.last_open_at = observed_open
  persistent.alerted_for = nil
end

events.on({
  type = "device.changed", device = WINDOW,
  field = "contact_open", value = true,
}, opened)

local function check_window()
  local window = devices.get(WINDOW)
  if not window or window.contact_open == nil then return end
  if window.contact_open then opened(); return end

  -- This callback suspends while history pages are read. Other callbacks run.
  local page, err = history.query(WINDOW, {
    field = "contact_open", value = true, limit = 1,
  })
  if not page then
    log("warn", "Cannot read window history", { error = err })
    return
  end

  -- The window may have changed while this callback was suspended.
  window = devices.get(WINDOW)
  if not window or window.contact_open ~= false then return end
  local last_open = observed_open
  if page.samples[1] then
    last_open = math.max(last_open or 0, page.samples[1].at)
  end
  if last_open then
    -- Retain a known opening if its history page is later evicted.
    observed_open = last_open
    persistent.last_open_at = last_open
  end
  local since = last_open or persistent.observed_since
  if automation.now() - since <= FOUR_HOURS then return end
  if persistent.alerted_for == since then return end

  local ok, notify_err = notify.send(
    "Time to air the room",
    window.name .. " has not been opened for more than four hours.",
    { device_id = window.id, tag = "window-airing" }
  )
  if ok then
    persistent.alerted_for = since
  else
    log("warn", "Window alert failed", { error = notify_err })
  end
end

automation.cron("*/15 7-22 * * *", check_window)
