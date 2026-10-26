-- Deterministic tests of scheduler admission and per-resume accounting.
local factory = assert(loadfile('automation/runtime.lua'))()
local function fixture(used)
  local clock, minute, failures, clock_calls = 0, 0, 0, 0
  local watched={};local latest_handle=0
  local host = {
    calendar=function() clock_calls=clock_calls+1;return {minute,0,1,1,0,126,0} end,
    now=function() return clock end, monotonic=function() clock_calls=clock_calls+1;return clock end,
    handle=function(id) latest_handle=id;return id end, handle_id=function(id) return id end,
    begin=function() end, finish=function() end, discard=function() end,
    failed=function() return false end,
    failure=function() failures=failures+1 end,
    serializable=function(value) return value end,
    watch=function(kind,added)watched[kind]=(watched[kind]or 0)+(added and 1 or -1);assert(watched[kind]>=0)end,
    limit=function(_,instructions,seconds)
      assert(instructions==200000 and seconds==nil)
    end,
    -- Simulate native work or descheduling exceeding the former wall deadline.
    -- Each resume still uses less than the instruction budget.
    spent=function() return used or 150000,10 end,
  }
  host.history_begin=function(token,target,options)
    if target=='missing' then return nil,'unknown device' end
    host.history_token=token
    return true
  end
  host.history_cancel=function(token) host.history_cancelled=token end
  local runtime = factory(host)
  return runtime, function(value) clock=value;minute=value end,
    function() return failures end, function() return clock_calls end, function(kind)return watched[kind]or 0 end, function()return latest_handle end, host
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

-- Interest tracks listener removal, event completion, timeout and cancellation.
do
  local runtime, advance, failures, _, watching, latest = fixture()
  local listener, task
  runtime.initialize(function()
    listener=runtime.env.events.on({type='remote.action'},function()end)
    runtime.env.automation.after(1,function()
      local event=runtime.env.events.wait_for({type='device.value'},1)
      assert(event==nil)
    end)
    runtime.env.automation.after(1,function()
      runtime.env.events.wait_for({type='device.changed'})
    end)
    runtime.env.automation.after(1,function()
      runtime.env.events.wait_for({type='device.offline'})
    end)
  end)
  assert(watching('remote.action')==1)
  runtime.env.events.off(listener);runtime.env.events.off(listener)
  assert(watching('remote.action')==0)
  advance(1);runtime.tick();runtime.tick();runtime.tick()
  task=latest();assert(watching('device.offline')==1)
  assert(runtime.env.automation.cancel(task) and watching('device.offline')==0)
  assert(watching('device.value')==1 and watching('device.changed')==1)
  runtime.event({type='device.changed'})
  assert(watching('device.changed')==0)
  advance(2);runtime.tick();runtime.tick()
  assert(watching('device.value')==0 and failures()==0)
end

-- Public control wrappers preserve errors, enforce thermostat value types,
-- and return immutable scene snapshots without changing earlier snapshots.
do
  local calls = 0
  local current = {id='7', name='Downstairs', scene_id='2', scene_name='Evening', off=false}
  local host = {
    calendar=function() return {0,0,1,1,0,126,0} end,
    action=function(operation, target, args)
      calls=calls+1
      assert(operation=='thermostat' and target=='trv')
      assert(args.field=='occupied_heating_setpoint' and args.value=='21')
      return nil,'queue full'
    end,
    scene_get=function(group)
      if group=='missing' then return nil,'unknown scene group' end
      return current
    end,
  }
  local env=factory(host).env
  local ok,err=env.devices.thermostat('trv','occupied_heating_setpoint',21)
  assert(ok==nil and err=='queue full' and calls==1)
  assert(not pcall(env.devices.thermostat,'trv','system_mode',true))
  assert(not pcall(env.devices.thermostat,'trv','system_mode',{}))
  assert(not pcall(env.devices.thermostat,'trv','system_mode',math.huge))
  assert(not pcall(env.devices.thermostat,'trv','system_mode',0/0))
  assert(calls==1)
  local before=env.scenes.get('7')
  assert(not pcall(function()before.off=true end))
  current.off=true
  assert(before.off==false and env.scenes.get('7').off==true)
  local group,reason=env.scenes.get('missing')
  assert(group==nil and reason=='unknown scene group')
end

-- Common libraries share one protected basis; script globals and API tables do not.
do
  local first=fixture()
  local second=fixture()
  assert(first.env~=second.env and first.env._G==first.env and second.env._G==second.env)
  assert(first.env.math==second.env.math and first.env.string==second.env.string and first.env.table==second.env.table)
  assert(first.env.events~=second.env.events and first.env.automation~=second.env.automation)
  first.env.private_global=42
  assert(second.env.private_global==nil)
  assert(not pcall(function()first.env.math.abs=nil end))
  assert(second.env.math.abs(-3)==3)
end

-- Manual triggers use ordinary coroutine admission, with script-local identities.
do
  local runtime, advance, failures=fixture()
  local other=fixture()
  local fired=0
  local t=runtime.env.triggers
  assert(not pcall(t.register,'','Empty',function()end))
  assert(not pcall(t.register,'id','',function()end))
  assert(not pcall(t.register,'id','Label',false))
  t.register('id','First',function()fired=100 end)
  t.register('id','Second',function()runtime.env.automation.sleep(1);fired=fired+1 end)
  assert(#runtime.triggers()==1 and runtime.triggers()[1].label=='Second')
  assert(other.trigger('id')=='trigger unavailable')
  assert(runtime.trigger('id')=='');runtime.tick();assert(fired==0)
  advance(1);runtime.tick();assert(fired==1)
  t.unregister('id');assert(#runtime.triggers()==0)
  assert(runtime.trigger('id')=='trigger unavailable')
  t.register('id','Error',function()error('trigger callback error')end)
  assert(runtime.trigger('id')=='');runtime.tick();assert(failures()==1)
  t.register('id','Queue',function()end)
  for _=1,128 do assert(runtime.trigger('id')=='') end
  assert(runtime.trigger('id')=='callback queue limit reached')
end

-- History suspends only the caller and resumes only after a completed read.
do
  local runtime,advance,failures,_,_,latest=fixture()
  local env=runtime.env
  assert(not pcall(env.history.query,'window',{field='contact_open'}))
  local resumed,other=false,false
  runtime.initialize(function()
    env.automation.after(1,function()
      local page=assert(env.history.query('window',{field='contact_open',limit=1}))
      assert(page.samples[1].value==true)
      assert(not pcall(function()page.samples[1].value=false end))
      resumed=true
    end)
    env.automation.after(1,function()other=true end)
  end)
  advance(1);runtime.tick();local token=tostring(latest())
  runtime.tick();assert(other and not resumed)
  runtime.history_result({token='stale',page={samples={}}});runtime.tick();assert(not resumed)
  runtime.history_result({token=token,page={samples={{at=1,value=true}},next_before=0}})
  assert(not resumed);runtime.tick();assert(resumed and failures()==0)
end

-- Immediate errors do not suspend; cancellation discards delayed completions.
do
  local runtime,advance,failures,_,_,latest=fixture()
  local env=runtime.env;local resumed=false
  runtime.initialize(function()
    env.automation.after(1,function()
      local page,err=env.history.query('missing',{field='contact_open'})
      assert(page==nil and err=='unknown device')
      env.history.query('window',{field='contact_open'})
      resumed=true
    end)
  end)
  advance(1);runtime.tick();local token=tostring(latest())
  -- Timer ID, callback ID, then the two query IDs.
  assert(env.automation.cancel(latest()-2))
  runtime.history_result({token=token,page={samples={}}});runtime.tick()
  assert(not resumed and failures()==0)
end

-- Exercise the shipped reminder, including state changes during a history read.
do
  local now,open,sample,query_error,send_ok=100,false,nil,nil,true
  local callback,on_open,after_query
  local sent,queries=0,0
  local state={}
  local env={
    persistent=state,math=math,
    automation={now=function()return now end,cron=function(spec,fn)assert(spec=='*/15 7-22 * * *');callback=fn end},
    events={on=function(filter,fn)assert(filter.field=='contact_open' and filter.value==true);on_open=fn end},
    devices={get=function()return {id='window',name='Window',contact_open=open}end},
    history={query=function(_,options)
      queries=queries+1;assert(options.field=='contact_open' and options.value==true and options.limit==1)
      if after_query then local fn=after_query;after_query=nil;fn()end
      if query_error then return nil,query_error end
      return {samples=sample and {{at=sample,value=true}} or {}}
    end},
    notify={send=function()if send_ok then sent=sent+1;return true end;return nil,'disabled'end},
    log=function()end,
  }
  local function load_example()assert(loadfile('automation/examples/window-airing.lua','t',env))()end
  load_example();assert(state.observed_since==100)
  now=14500;callback();assert(sent==0) -- exactly four hours
  now=14501;callback();assert(sent==1)
  callback();load_example();callback();assert(sent==1) -- survives reload
  open=true;on_open();callback();assert(sent==1)
  open=false;sample=14501;now=28902;send_ok=false;callback();assert(sent==1)
  send_ok=true;callback();assert(sent==2)
  -- A fresh installation can immediately use an old retained opening.
  state.last_open_at=nil;state.alerted_for=nil;state.observed_since=now
  load_example();sample=100;callback();assert(sent==3 and state.last_open_at==100)
  -- Retention eviction and reload must not duplicate an alert.
  sample=nil;load_example();callback();assert(sent==3)
  -- Read errors never become evidence of a missing opening.
  state.alerted_for=nil;query_error='unreadable';callback();assert(sent==3)
  query_error=nil;open=nil;local before=queries;callback();assert(queries==before)
  open=false;after_query=function()open=true end;callback();assert(sent==3)
  -- A quick open/close while suspended must override the old history snapshot.
  open=false;after_query=function()on_open()end;callback();assert(sent==3)
  now=now+14401;callback();assert(sent==4)
end

-- Query identities must use serial IDs, not recyclable userdata addresses.
do
  local runtime,advance,failures,_,_,_,host=fixture()
  local original=host.handle
  host.handle=function(id)original(id);return setmetatable({id=id},{__tostring=function()return 'reused-address'end})end
  host.handle_id=function(handle)return handle.id end
  local completed=0
  runtime.initialize(function()
    for _=1,2 do runtime.env.automation.after(1,function()
      assert(runtime.env.history.query('window',{field='contact_open'}))
      completed=completed+1
    end)end
  end)
  advance(1);runtime.tick();local first=host.history_token
  runtime.tick();local second=host.history_token
  assert(first~=second)
  runtime.history_result({token=second,page={samples={}}})
  runtime.history_result({token=first,page={samples={}}})
  runtime.tick();runtime.tick();assert(completed==2 and failures()==0)
end

-- Instruction-budget failure after submission releases the native query.
do
  local runtime,advance,failures,_,_,_,host=fixture()
  runtime.initialize(function()
    runtime.env.automation.after(1,function()
      runtime.env.history.query('window',{field='contact_open'})
    end)
  end)
  local original=host.history_begin
  host.history_begin=function(...)
    host.spent=function()return 200000 end
    return original(...)
  end
  advance(1);runtime.tick()
  assert(failures()==1 and host.history_cancelled==host.history_token)
end
