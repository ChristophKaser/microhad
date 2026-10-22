local function leaf() return benchmark.tick() end
local function four() return leaf() end
local function three() return four() end
local function two() return three() end
local function one() return two() end

function run()
  local total = 0
  for i = 1, 10000 do total = total + one() end
  return total
end
