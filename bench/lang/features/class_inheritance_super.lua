-- class_inheritance_super 参照端口(与 class_inheritance_super.aria 同算法同规模):元表链 + 显式
-- 调父实现(Base.describe(self))即 Lua 的 super。
local Base = {}
Base.__index = Base

function Base.new(v)
    return setmetatable({v = v}, Base)
end

function Base:describe()
    return 1
end

local Derived = setmetatable({}, {__index = Base})
Derived.__index = Derived

function Derived.new(v)
    return setmetatable(Base.new(v), Derived)
end

function Derived:describe()
    return 10 + Base.describe(self)
end

local obj = Derived.new(7)
local iterations = 4000000

local t0 = os.clock()
local acc = 0
local i = 0
while i < iterations do
    acc = (acc + obj:describe()) % 1000003
    i = i + 1
end
local dt = (os.clock() - t0) * 1000.0

assert(acc == 999871, "checksum drift")
print("value checksum: " .. acc)
print("bench-time: " .. dt .. " ms")
