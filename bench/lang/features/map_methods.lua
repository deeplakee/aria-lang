-- map_methods 参照端口(与 map_methods.aria 同算法同规模):键集恒定(删一个再插同一个),故 size 用
-- 常量 entries;keys/values/pairs 三份快照按本语言写法逐对收集(aria 是原生快照)。
local entries = 20000
local rounds = 1600

local m = {}
for i = 0, entries - 1 do
    m[i] = i * 2
end

local t0 = os.clock()
local acc = 0
for r = 0, rounds - 1 do
    local key = r % entries
    m[key] = nil
    m[key] = r
    if m[key] ~= nil then
        acc = (acc + m[key]) % 1000003
    end
    acc = (acc + entries) % 1000003
    if r % 10 == 0 then
        local ks, vs = {}, {}
        for k, v in pairs(m) do
            ks[#ks + 1] = k
            vs[#vs + 1] = v
        end
        acc = (acc + #ks + #vs + #ks) % 1000003
        for j = 1, #ks do
            acc = (acc + ks[j]) % 1000003
        end
    end
end
m = {}
acc = acc % 1000003
local dt = (os.clock() - t0) * 1000.0

assert(acc == 183077, "checksum drift")
print("value checksum: " .. acc)
print("bench-time: " .. dt .. " ms")
