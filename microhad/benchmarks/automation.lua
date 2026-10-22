local state = { occupied = false, count = 0, timers = {} }

local function action(kind, value)
  state.count = state.count + 1
  state.timers[state.count % 32] = { kind = kind, value = value, due = state.count + 60 }
  return benchmark.action(kind, value)
end

function run()
  for i = 1, 10000 do
    local event = (i % 5) == 0 and "occupied" or "temperature"
    if event == "occupied" then state.occupied = not state.occupied end
    if state.occupied and (i % 4) == 0 then action("scene", i % 3) end
    if (i % 40) == 0 then action("notify", i) end
    benchmark.tick()
  end
end
