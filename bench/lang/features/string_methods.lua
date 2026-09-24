-- string_methods 参照端口(与 string_methods.aria 同算法同规模):split/trim 手写,replace 用 gsub。
local function split(s, sep)
    local out, start = {}, 1
    while true do
        local i = s:find(sep, start, true)
        if not i then
            out[#out + 1] = s:sub(start)
            break
        end
        out[#out + 1] = s:sub(start, i - 1)
        start = i + #sep
    end
    return out
end

local function trim(s)
    return (s:gsub("^%s+", ""):gsub("%s+$", ""))
end

local rounds = 450000
local base = "  the quick brown fox jumps over the lazy dog  "

local t0 = os.clock()
local acc = 0
for _ = 1, rounds do
    local t = trim(base)
    local parts = split(t, " ")
    local joined = table.concat(parts, "-")
    local up = t:upper()
    if t:sub(1, 3) == "the" and t:sub(-3) == "dog" then
        acc = acc + 1
    end
    acc = (acc + (t:find("fox", 1, true) - 1) + #(t:gsub("o", "0")) + #joined + #up +
           #(t:sub(5, 9))) % 1000003
end
local dt = (os.clock() - t0) * 1000.0

assert(acc == 949799, "checksum drift")
print("value checksum: " .. acc)
print("bench-time: " .. dt .. " ms")
