-- template_render 参照端口(与 template_render.aria 同算法同规模):mustache 式模板编译成捕获
-- 片段的闭包列表(文本/变量/section 三种段,section 递归编译),对 LCG 造的订单数据反复渲染;
-- 变量段做 HTML 转义与缺失键兜底。Lua 下标是 1 基闭区间,find_from/sub 助手换算成 aria 的
-- 0 基半开区间,compile 主体与 aria 逐行对齐。
local orders_n = 900  -- 规模:订单数
local rounds = 20     -- 规模:渲染轮数

local state = 2024
local function rnd(n)
    state = (state * 48271) % 2147483647
    return state % n
end

local function esc_html(s)
    -- gsub 返 (串, 次数) 两个值,外层括号截成一个;模式里无魔法字符,即字面替换。
    return (s:gsub("&", "&amp;"):gsub("<", "&lt;"):gsub(">", "&gt;"))
end

local function find_from(src, needle, from)
    -- string.find 的 init 是 1 基起点:命中换算回 0 基(from + 局部偏移 - 1),未命中 nil。
    local hit = src:find(needle, from + 1, true)
    if hit == nil then
        return nil
    end
    return hit - 1
end

local function sub(src, from, to)
    -- aria 的 substring(from, to) 半开区间 -> Lua 的 sub(from+1, to) 闭区间。
    return src:sub(from + 1, to)
end

local function render(segs, data)
    local out = ""
    for _, seg in ipairs(segs) do
        out = out .. seg(data)
    end
    return out
end

local function compile(src)
    local segs = {}
    local pos = 0
    while true do
        local open = find_from(src, "{{", pos)
        if open == nil then
            local rest = sub(src, pos, #src)
            if #rest > 0 then
                local tail = rest
                segs[#segs + 1] = function(d)
                    return tail
                end
            end
            break
        end
        if open > pos then
            local lit = sub(src, pos, open)
            segs[#segs + 1] = function(d)
                return lit
            end
        end
        local close = find_from(src, "}}", open)
        local tag = sub(src, open + 2, close)
        if tag:sub(1, 1) == "#" then
            local name = tag:sub(2)
            local endtag = "{{/" .. name .. "}}"
            local stop = find_from(src, endtag, close)
            local body = compile(sub(src, close + 2, stop))
            segs[#segs + 1] = function(d)
                local items = d[name]
                if items == nil then
                    return ""
                end
                local out = ""
                for _, item in ipairs(items) do
                    out = out .. render(body, item)
                end
                return out
            end
            pos = stop + #endtag
        else
            local name2 = tag
            segs[#segs + 1] = function(d)
                local v = d[name2]
                if v == nil then
                    return ""
                end
                return esc_html(tostring(v))
            end
            pos = close + 2
        end
    end
    return segs
end

local t0 = os.clock()

local item_names = {"widget", "gadget", "cable", "mount", "panel", "sensor"}
local buyers = {"acme", "globex", "initech", "umbrella", "stark", "wayne"}
local orders = {}
local i = 0
while i < orders_n do
    local items = {}
    local n = 1 + rnd(4)
    local j = 0
    while j < n do
        local item = {}
        item["name"] = item_names[rnd(#item_names) + 1]
        item["qty"] = 1 + rnd(9)
        item["price"] = 5 + rnd(200)
        items[#items + 1] = item
        j = j + 1
    end
    local order = {}
    order["id"] = 1000 + i
    order["customer"] = buyers[rnd(#buyers) + 1]
    order["items"] = items
    orders[#orders + 1] = order
    i = i + 1
end

local tpl_invoice = compile("Invoice #{{id}} -- {{customer}}\n{{#items}}  {{name}} x{{qty}} @ {{price}}\n{{/items}}")
local tpl_receipt = compile("Receipt {{id}} ({{customer}}): {{#items}}{{qty}}x{{name}};{{/items}}")
local tpl_summary = compile("Summary {{customer}} {{missing_key}} {{#items}}{{name}},{{/items}}")

local total = 0
local r = 0
while r < rounds do
    for _, o in ipairs(orders) do
        local a = render(tpl_invoice, o)
        local b = render(tpl_receipt, o)
        local c = render(tpl_summary, o)
        total = (total + #a + #b + #c) % 1000003
    end
    r = r + 1
end

assert(total == 552394, "checksum drift")

local sample = #render(tpl_invoice, orders[1])
local dt = (os.clock() - t0) * 1000.0

print("value checksum: " .. total)
print("value sample: " .. sample)
print("bench-time: " .. dt .. " ms")
