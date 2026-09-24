-- fannkuch_redux 参照端口(与 fannkuch_redux.aria 同算法同规模):表 1-based,置换值仍是 0..n-1。
local n = 9

local t0 = os.clock()
local perm = {}
for i = 1, n do
    perm[i] = i - 1
end
local max_flips = 0
local flips_sum = 0
local perm_count = 0
while true do
    if perm[1] ~= 0 then
        local work = {}
        for c = 1, n do
            work[c] = perm[c]
        end
        local flips = 0
        while work[1] ~= 0 do
            local k = work[1] + 1
            local left, right = 1, k
            while left < right do
                local tmp = work[left]
                work[left] = work[right]
                work[right] = tmp
                left = left + 1
                right = right - 1
            end
            flips = flips + 1
        end
        if flips > max_flips then
            max_flips = flips
        end
        flips_sum = flips_sum + flips
        perm_count = perm_count + 1
    end
    local i = n - 1
    while i >= 1 and perm[i] >= perm[i + 1] do
        i = i - 1
    end
    if i < 1 then
        break
    end
    local j = n
    while perm[j] <= perm[i] do
        j = j - 1
    end
    local tmp2 = perm[i]
    perm[i] = perm[j]
    perm[j] = tmp2
    local left2, right2 = i + 1, n
    while left2 < right2 do
        local tmp3 = perm[left2]
        perm[left2] = perm[right2]
        perm[right2] = tmp3
        left2 = left2 + 1
        right2 = right2 - 1
    end
end
local dt = (os.clock() - t0) * 1000.0

assert(flips_sum == 1911505, "checksum drift")
assert(max_flips == 30, "max_flips drift")
assert(perm_count == 322560, "perm_count drift")
print("value checksum: " .. flips_sum)
print("value max_flips: " .. max_flips)
print("value perm_count: " .. perm_count)
print("bench-time: " .. dt .. " ms")
