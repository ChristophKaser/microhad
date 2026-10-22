-- Trusted scheduler. Only the returned environment is exposed to author code.
local host = ...
local co, unpack, pack = coroutine, table.unpack, table.pack
local next, pairs, type, error, assert = next, pairs, type, error, assert
local tasks, listeners, timers = {}, {}, {}
local serial, last_minute = 0, table.concat(host.calendar(),':')
local current, initializing
local env = {}
local function strict(v, t, name)
  if type(v) ~= t then error((name or 'argument') .. ' must be ' .. t, 3) end
  return v
end
local function duration(n)
  strict(n, 'number', 'seconds')
  assert(n > 0 and n < math.huge and n <= 31536000, 'invalid duration')
  return n
end
local function handle()
  serial = serial + 1
  return host.handle(serial)
end
local function immutable(value)
  if type(value) ~= 'table' then return value end
  local contents = {}
  for k,v in pairs(value) do contents[k] = immutable(v) end
  return setmetatable({}, { __index=contents, __newindex=function() error('snapshot is immutable',2) end,
    __pairs=function() return next, contents, nil end, __len=function() return #contents end, __metatable=false })
end
local function count(t) local n=0 for _ in pairs(t) do n=n+1 end return n end
local function filter(f)
  strict(f,'table','filter'); strict(f.type,'string','filter.type')
  local types={['device.added']=true,['device.removed']=true,['device.value']=true,['device.changed']=true,
    ['device.online']=true,['device.offline']=true,['timer.fired']=true,['automation.error']=true}
  assert(types[f.type], 'unsupported event type')
  local allowed={type=true,device=true,device_id=true,source=true,field=true,value=true,
    value_below=true,value_at_most=true,value_above=true,value_at_least=true}
  local copy={}
  for k,v in pairs(f) do
    assert(allowed[k], 'unsupported filter field: '..tostring(k))
    if k:sub(1,6)=='value_' then strict(v,'number',k); assert(v==v and math.abs(v)<math.huge,'invalid threshold')
    elseif k~='value' then strict(v,'string',k) end
    copy[k]=v
  end
  if copy.device then local id,err=host.resolve(copy.device); assert(id,err); copy.device_id=id; copy.device=nil end
  return copy
end
local function matches(f,e)
  for _,k in ipairs({'type','device_id','source','field','value'}) do if f[k]~=nil and f[k]~=e[k] then return false end end
  for k,compare in pairs({value_below=function(a,b)return a<b end,value_at_most=function(a,b)return a<=b end,
    value_above=function(a,b)return a>b end,value_at_least=function(a,b)return a>=b end}) do
    if f[k]~=nil and (type(e.value)~='number' or not compare(e.value,f[k])) then return false end
  end
  return true
end
local function report(message, operation)
  host.failure(tostring(message),operation)
end
local function start(fn,args,chain,depth,on_done)
  if count(tasks)>=64 then report('callback queue limit reached','queue'); return end
  local h=handle()
  tasks[h]={thread=co.create(fn),args=args,instructions=100000,time=0.1,chain=chain or '',depth=depth or 0,on_done=on_done}
  return h
end
local function resume(h,t)
  current=t
  host.begin(tostring(h),t.chain,t.depth)
  host.limit(t.thread,t.instructions,t.time)
  local args=t.args or {}
  local result=pack(co.resume(t.thread,unpack(args,1,args.n or #args)))
  local used, elapsed=host.spent()
  t.instructions=t.instructions-used; t.time=t.time-elapsed
  current=nil; t.args=nil
  local ok=result[1] and t.instructions>0 and t.time>0
  local done=not ok or co.status(t.thread)=='dead'
  host.finish(ok,done)
  if done then
    tasks[h]=nil
    if t.on_done then t.on_done() end
    if not ok then report(result[1] and 'callback budget exceeded' or result[2],'callback') end
  elseif type(result[2])=='table' and result[2].kind then
    t.wait=result[2]
  else
    tasks[h]=nil;host.discard(tostring(h));report('unsupported coroutine yield','callback')
  end
end
local events={}
function events.on(f,fn)
  strict(fn,'function','callback'); f=filter(f)
  assert(count(listeners)<64,'listener limit reached')
  local h=handle();listeners[h]={filter=f,fn=fn};return h
end
function events.off(h) host.handle_id(h);listeners[h]=nil end
local automation={now=host.now,script_id=host.script_id}
local function timer(seconds,fn,payload,repeating)
  duration(seconds);strict(fn,'function','callback');payload=host.serializable(payload)
  assert(count(timers)<32,'timer limit reached')
  local h=handle();timers[h]={due=host.monotonic()+seconds,seconds=seconds,fn=fn,payload=payload,repeat_timer=repeating};return h
end
function automation.after(n,fn,payload)return timer(n,fn,payload,false)end
function automation.every(n,fn,payload)return timer(n,fn,payload,true)end
function automation.cancel(h)
  host.handle_id(h)
  if timers[h] then timers[h]=nil;return true end
  if tasks[h] and tasks[h].wait then tasks[h]=nil;host.discard(tostring(h));return true end
  return false
end
function automation.sleep(n)
  assert(current and not initializing,'sleep is only available in callbacks')
  return co.yield({kind='sleep',due=host.monotonic()+duration(n)})
end
function events.wait_for(f,timeout)
  assert(current and not initializing,'wait_for is only available in callbacks')
  return co.yield({kind='event',filter=filter(f),due=timeout and (host.monotonic()+duration(timeout))})
end
local function cron_field(text,min,max)
  local values={}
  assert(text~='' and text:sub(-1)~=',' and not text:find(',,',1,true),'invalid cron field')
  for part in text:gmatch('[^,]+') do
    local base,step=part:match('^(.-)/(%d+)$');if not base then base=part;step=1 else step=tonumber(step) end
    assert(step>0 and step<=max-min+1,'invalid cron step')
    local first,last
    if base=='*' then first=min;last=max else
      first,last=base:match('^(%d+)%-(%d+)$')
      if first then first=tonumber(first);last=tonumber(last) else first=tonumber(base);last=first end
    end
    assert(first and last and first%1==0 and first>=min and last<=max and last>=first,'invalid cron field')
    for n=first,last,step do values[n]=true end
  end
  return values,text=='*'
end
function automation.cron(expression,fn)
  strict(expression,'string','cron expression');strict(fn,'function','callback')
  local parts={} for p in expression:gmatch('%S+')do parts[#parts+1]=p end
  assert(#parts==5,'cron requires five fields');assert(count(timers)<32,'timer limit reached')
  local ranges={{0,59},{0,23},{1,31},{1,12},{0,7}};local fields,stars={},{}
  for i=1,5 do fields[i],stars[i]=cron_field(parts[i],unpack(ranges[i])) end
  if fields[5][7]then fields[5][0]=true end
  local h=handle();timers[h]={cron=fields,stars=stars,fn=fn};return h
end
local function state_proxy(scope,path)
  local function at(root)
    for _,key in ipairs(path)do root=type(root)=='table' and root[key] or nil end
    return root
  end
  return setmetatable({}, {
    __index=function(_,key)
      strict(key,'string','state key');local value=at(host.read_state(scope))
      value=type(value)=='table' and value[key] or nil
      if type(value)=='table' then local nested={unpack(path)};nested[#nested+1]=key;return state_proxy(scope,nested) end
      return value
    end,
    __newindex=function(_,key,value)
      strict(key,'string','state key');host.serializable(value)
      local root=host.read_state(scope);local target=at(root)
      assert(type(target)=='table','state table no longer exists');target[key]=value;host.write_state(scope,root)
    end,
    __pairs=function()return next,at(host.read_state(scope)) or {},nil end,
    __metatable=false
  })
end
for _,name in ipairs({'assert','error','ipairs','next','pairs','pcall','select','tonumber','tostring','type','xpcall','rawequal'}) do env[name]=_G[name] end
env._VERSION=_VERSION;env._G=env
env.math=immutable(math);env.string=immutable(string);env.table=immutable(table)
-- Expensive string operations are capped by the allocator and execution budget.
env.events=events;env.automation=automation
env.devices={get=function(target)local v,e=host.device_get(target);return v and immutable(v),e end,
  list=function(f)local values=host.device_list(f);for i,v in ipairs(values)do values[i]=immutable(v)end return values end,
  actor=function(target,on)strict(on,'boolean','on');return host.action('actor',target,{on=on})end,
  light=function(target,state)
    strict(state,'table','light state');assert(state.on~=nil or state.brightness~=nil,'empty light state')
    for k,v in pairs(state)do if k=='on'then strict(v,'boolean','on')elseif k=='brightness'then strict(v,'number','brightness');assert(v%1==0 and v>=0 and v<=100,'brightness must be 0–100')else error('unsupported light field')end end
    return host.action('light',target,state)
  end,
  refresh=function(target)return host.action('refresh',target,{})end}
env.scenes={activate=function(group,scene)return host.scene(group,scene)end,off=function(group)return host.scene(group,nil)end}
env.notify={send=host.notify};env.log=host.log
local function unwrap(value,seen)
  if type(value)~='table'then return value end
  if seen[value]then return seen[value]end
  local out={};seen[value]=out
  for k,v in pairs(value)do out[k]=unwrap(v,seen)end
  return out
end
env.dump=function(value,label)return host.dump(unwrap(value,{}),label)end
env.persistent=state_proxy('persistent',{});env.shared=state_proxy('shared',{})
local runtime={env=env}
function runtime.initialize(fn)
  initializing=true;local h=start(fn,{});resume(h,tasks[h]);initializing=false
  if host.failed()then listeners={};timers={};tasks={};return false end
  return true
end
function runtime.event(event)
  local snapshot=immutable(event)
  for _,entry in pairs(listeners) do if matches(entry.filter,event)then start(entry.fn,{snapshot},event.chain_id,event.chain_depth)end end
  for _,task in pairs(tasks)do if task.wait and task.wait.kind=='event' and matches(task.wait.filter,event)then task.wait=nil;task.args={snapshot}end end
end
function runtime.tick()
  local now=host.monotonic();local calendar=host.calendar();local minute=table.concat(calendar,':')
  for h,t in pairs(timers)do
    local due=t.due and t.due<=now
    if t.cron and minute~=last_minute and not t.active then
      local f=t.cron;local day=(f[3][calendar[3]] and (t.stars[5] or not t.stars[3])) or (f[5][calendar[5]] and (t.stars[3] or not t.stars[5]))
      due=f[1][calendar[1]] and f[2][calendar[2]] and day and f[4][calendar[4]]
    end
    if due and not t.active then
      t.active=true
      runtime.event({type='timer.fired',at=host.now(),source='automation',timer_id=tostring(h),payload=t.payload,chain_id='',chain_depth=0})
      if not t.repeat_timer and not t.cron then timers[h]=nil end
      start(t.fn,{t.payload},'',0,function()if timers[h]then t.active=false;t.due=not t.cron and (host.monotonic()+t.seconds)or nil end end)
    end
  end
  last_minute=minute
  -- Snapshot the ready queue: newly registered callbacks run on the next tick.
  local ready={} for h,t in pairs(tasks)do ready[#ready+1]={h,t}end
  table.sort(ready,function(a,b)return host.handle_id(a[1])<host.handle_id(b[1])end)
  local dispatched=false
  for _,entry in ipairs(ready)do local h,t=unpack(entry)
    if tasks[h] then
      if t.wait and t.wait.due and t.wait.due<=now then local kind=t.wait.kind;t.wait=nil;t.args=kind=='event' and pack(nil,'timeout')or {} end
      if not t.wait and not dispatched then resume(h,t);dispatched=true end
    end
  end
end
function runtime.stats()local suspended=0 for _,t in pairs(tasks)do if t.wait then suspended=suspended+1 end end return count(listeners),count(timers),suspended end
return runtime
