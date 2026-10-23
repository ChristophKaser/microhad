-- Deterministic tests of scheduler admission and per-resume accounting.
local factory = assert(loadfile('automation/runtime.lua'))
local function fixture(used)
  local clock, minute, failures, clock_calls = 0, 0, 0, 0
  local host = {
    calendar=function() clock_calls=clock_calls+1;return {minute,0,1,1,0,126,0} end,
    now=function() return clock end, monotonic=function() clock_calls=clock_calls+1;return clock end,
    handle=function(id) return id end, handle_id=function(id) return id end,
    begin=function() end, finish=function() end, discard=function() end,
    failed=function() return false end,
    failure=function() failures=failures+1 end,
    serializable=function(value) return value end,
    limit=function(_,instructions,seconds)
      assert(instructions==200000 and seconds==nil)
    end,
    -- Simulate native work or descheduling exceeding the former wall deadline.
    -- Each resume still uses less than the instruction budget.
    spent=function() return used or 150000,10 end,
  }
  local runtime = factory(host)
  return runtime, function(value) clock=value;minute=value end,
    function() return failures end, function() return clock_calls end
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
  assert(completed and failures()==0,'instruction budgets reset after each yield regardless of elapsed time')
end

-- Even a caught hook error must not let an exhausted callback succeed.
do
  local runtime, _, failures = fixture(200000)
  runtime.initialize(function() end)
  assert(failures()==1,'exhausted instruction budget must fail after resume')
end

-- Idle listeners require no clock/calendar work and unmatched events need no
-- immutable device-table copy. These are hot paths on the single-core gateway.
do
  local runtime, _, failures, calls = fixture()
  runtime.initialize(function() runtime.env.events.on({type='remote.action'},function()end) end)
  local before=calls()
  for _=1,100 do runtime.tick() end
  assert(calls()==before,'idle tick should not ask the host for time')
  runtime.event({type='device.value',device=setmetatable({}, {__pairs=function()error('unmatched snapshot copied')end})})
  assert(failures()==0)
end
