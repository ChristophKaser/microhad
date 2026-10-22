-- Deterministic tests of scheduler admission and per-resume accounting.
local factory = assert(loadfile('automation/runtime.lua'))
local function fixture()
  local clock, minute, failures = 0, 0, 0
  local host = {
    calendar=function() return {minute,0,1,1,0,126,0} end,
    now=function() return clock end, monotonic=function() return clock end,
    handle=function(id) return id end, handle_id=function(id) return id end,
    begin=function() end, finish=function() end, discard=function() end,
    failed=function() return false end,
    failure=function() failures=failures+1 end,
    serializable=function(value) return value end,
    limit=function(_,instructions,seconds)
      assert(instructions==200000 and seconds==0.2)
    end,
    -- Each resume uses most of its budget; multiple resumes must still work.
    spent=function() return 150000,0.15 end,
  }
  local runtime = factory(host)
  return runtime, function(value) clock=value;minute=value end,
    function() return failures end
end

for _,cron in ipairs({false,true}) do
  for _,from_timer in ipairs({false,true}) do
    local runtime, advance, failures = fixture()
    local env, fired = runtime.env, 0
    assert(runtime.initialize(function()
      for _=1,128 do
        local handle
        handle=env.events.on({type=from_timer and 'timer.fired' or 'device.value'},function()
          env.events.off(handle)
        end)
      end
      if cron then env.automation.cron('* * * * *',function() fired=fired+1 end)
      else env.automation.every(1,function() fired=fired+1 end) end
    end))
    if not from_timer then runtime.event({type='device.value'}) end
    advance(1);runtime.tick()
    assert(failures()>0 and fired==0,'callback admission must fail at capacity')
    for _=1,130 do runtime.tick() end
    advance(2);runtime.tick()
    assert(fired==1,'recurring timer must recover after saturation')
    advance(3);runtime.tick()
    assert(fired==2,'recurring timer must remain scheduled')
  end
end

do
  local runtime, advance, failures = fixture()
  local completed = false
  assert(runtime.initialize(function()
    runtime.env.automation.after(1,function()
      for _=1,4 do runtime.env.automation.sleep(1) end
      completed=true
    end)
  end))
  for now=1,5 do advance(now);runtime.tick() end
  assert(completed and failures()==0,'time and instruction budgets reset after each yield')
end
