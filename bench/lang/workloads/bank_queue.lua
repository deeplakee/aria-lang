-- bank_queue 参照端口(与 bank_queue.aria 同算法同规模)。
local customers_n = 78000
local key_unit = 1000000

local state = 424242
local function rnd(n)
    state = (state * 48271) % 2147483647
    return state % n
end

local tellers = {}
local ti = 0
while ti < 4 do
    ti = ti + 1
    tellers[#tellers + 1] = {busy_until = 0, served = 0}
end

local heap = {}
local queue = {}
local seq = 0
local served = 0
local wait_sum = 0
local last_time = 0

local function heap_push(entry)
    heap[#heap + 1] = entry
    local i = #heap
    while i > 1 do
        local parent = i // 2
        if heap[parent][1] <= heap[i][1] then
            break
        end
        local tmp = heap[parent]
        heap[parent] = heap[i]
        heap[i] = tmp
        i = parent
    end
end

local function heap_pop()
    local top = heap[1]
    local last = table.remove(heap)
    local n = #heap
    if n > 0 then
        heap[1] = last
        local i = 1
        while true do
            local l = 2 * i
            local r = l + 1
            local m = i
            if l <= n and heap[l][1] < heap[m][1] then
                m = l
            end
            if r <= n and heap[r][1] < heap[m][1] then
                m = r
            end
            if m == i then
                break
            end
            local tmp = heap[m]
            heap[m] = heap[i]
            heap[i] = tmp
            i = m
        end
    end
    return top
end

local function start_service(idx, now)
    served = served + 1
    tellers[idx].served = tellers[idx].served + 1
    local svc = 20 + rnd(60)
    tellers[idx].busy_until = now + svc
    seq = seq + 1
    heap_push({(now + svc) * key_unit + seq, 1, idx})
end

local function on_arrival(key)
    local now = key // key_unit
    last_time = now
    local nxt = now + 1 + rnd(40)
    seq = seq + 1
    heap_push({nxt * key_unit + seq, 0, 0})
    local free = -1
    local k = 0
    while k < #tellers do
        k = k + 1
        if tellers[k].busy_until <= now then
            free = k
        end
    end
    if free >= 0 then
        start_service(free, now)
    else
        queue[#queue + 1] = now
    end
end

local function on_done(key, idx)
    local now = key // key_unit
    last_time = now
    if #queue > 0 then
        local arrive = table.remove(queue, 1)
        wait_sum = wait_sum + now - arrive
        start_service(idx, now)
    end
end

local t0 = os.clock()

seq = seq + 1
heap_push({(1 + rnd(40)) * key_unit + seq, 0, 0})
while served < customers_n do
    local ev = heap_pop()
    if ev[2] == 0 then
        on_arrival(ev[1])
    else
        on_done(ev[1], ev[3])
    end
end

local load_sum = 0
local k2 = 0
while k2 < #tellers do
    k2 = k2 + 1
    load_sum = load_sum + tellers[k2].served
end

assert(served == customers_n, "bank_queue served drift")
assert(load_sum == served, "bank_queue load_sum drift")
assert(wait_sum == 101322, "bank_queue wait_sum drift")
assert(last_time == 1604597, "bank_queue last_time drift")

local dt = (os.clock() - t0) * 1000.0

print("value served: " .. served)
print("value wait_sum: " .. wait_sum)
print("value last_time: " .. last_time)
print("value load_sum: " .. load_sum)
print("bench-time: " .. dt .. " ms")
