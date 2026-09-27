-- json_codec 参照端口(与 json_codec.aria 同算法同规模):生成嵌套文档并序列化,再走
-- 「解析 -> 再序列化 -> 逐位比对」往返,外加剧段截断片段的合法性校验。解析器手写递归下降,
-- 禁用内建 cjson;序列化按 canonical 约定对对象键排序;rnd 调用步序与 aria 逐行对齐。
--
-- Lua 表示约定(与 aria 值类型的对应):
--   * aria 的 nil 在 Lua 表里存不住(存 nil 等于删除条目),用唯一哨兵 NIL 代替;
--   * list 用 1..n 的序列表,map 用字符串键的表;下标整体比 aria 偏移 1。
--   * 两种容器都是 table,用「含 v[1] ~= nil」判 list:生成与往返两段的容器都保证非空
--     (gen_value/gen_doc 的元素数下限为 1,且 2400 份文档逐位往返成立),而空表只出现在
--     校验段解析成功后被丢弃、不再参与序列化与计数的值里,故该判法对全部可比值成立。
local NIL = {}

local doc_count = 120  -- 规模:文档数
local passes = 20      -- 规模:往返遍数
local snippets = 2000  -- 规模:校验片段数

local state = 7
local function rnd(n)
    state = (state * 48271) % 2147483647
    return state % n
end

local names = {"id", "name", "count", "active", "tags", "meta", "score", "note", "kind", "level"}
local words = {"core", "edge", "node", "wire", "mesh", "flux", "grid", "pulse"}

local function gen_string()
    local n = 2 + rnd(4)
    local out = ""
    for _ = 1, n do
        out = out .. words[rnd(#words) + 1]
        local k = rnd(9)
        if k == 0 then
            out = out .. "\\"
        elseif k == 1 then
            out = out .. "\""
        elseif k == 2 then
            out = out .. "\n"
        end
    end
    return out
end

local function gen_value(depth)
    local t = rnd(10)
    if depth >= 3 or t < 3 then
        return rnd(100000)
    end
    if t == 3 then
        return gen_string()
    end
    if t == 4 then
        return NIL
    end
    if t <= 6 then
        local arr = {}
        local n = 1 + rnd(4)
        for _ = 1, n do
            arr[#arr + 1] = gen_value(depth + 1)
        end
        return arr
    end
    local obj = {}
    local n = 1 + rnd(3)
    for _ = 1, n do
        -- 显式先取键再求值,不依赖实现里「键、值」的求值次序,保住与 aria 一致的 rnd 步序。
        local key = names[rnd(#names) + 1]
        obj[key] = gen_value(depth + 1)
    end
    return obj
end

local function gen_doc()
    local obj = {}
    local n = 3 + rnd(4)
    for _ = 1, n do
        local key = names[rnd(#names) + 1]
        obj[key] = gen_value(1)
    end
    return obj
end

local function escape_json(s)
    local out = "\""
    for i = 1, #s do
        local c = s:sub(i, i)
        if c == "\"" or c == "\\" then
            out = out .. "\\" .. c
        elseif c == "\n" then
            out = out .. "\\n"
        elseif c == "\t" then
            out = out .. "\\t"
        else
            out = out .. c
        end
    end
    return out .. "\""
end

local function is_list(v)
    return type(v) == "table" and v[1] ~= nil
end

local function json_stringify(v)
    if v == NIL then
        return "null"
    end
    local tv = type(v)
    if tv == "number" then
        return tostring(v)
    end
    if tv == "string" then
        return escape_json(v)
    end
    if is_list(v) then
        local out = "["
        local first = true
        for _, item in ipairs(v) do
            if not first then
                out = out .. ","
            end
            first = false
            out = out .. json_stringify(item)
        end
        return out .. "]"
    end
    local out = "{"
    local ks = {}
    for k in pairs(v) do
        ks[#ks + 1] = k
    end
    table.sort(ks)
    local first = true
    for _, k in ipairs(ks) do
        if not first then
            out = out .. ","
        end
        first = false
        out = out .. escape_json(k) .. ":" .. json_stringify(v[k])
    end
    return out .. "}"
end

-- 解析器:pos 与 aria 同为 0 基,字符访问统一经 sub(pos + 1, pos + 1) 换算。
local function parser_fail(p, msg)
    error("json: " .. msg, 0)
end

local function parser_skip_ws(p)
    while p.pos < #p.src do
        local c = p.src:sub(p.pos + 1, p.pos + 1)
        if c == " " or c == "\n" or c == "\t" or c == "\r" then
            p.pos = p.pos + 1
        else
            break
        end
    end
end

local function parser_peek(p)
    if p.pos >= #p.src then
        parser_fail(p, "unexpected end")
    end
    return p.src:sub(p.pos + 1, p.pos + 1)
end

local function parser_expect(p, ch)
    if parser_peek(p) ~= ch then
        parser_fail(p, "expected '" .. ch .. "' at " .. p.pos)
    end
    p.pos = p.pos + 1
end

local function parser_parse_lit(p, word, value)
    local last = p.pos + #word
    if last > #p.src or p.src:sub(p.pos + 1, last) ~= word then
        parser_fail(p, "bad literal")
    end
    p.pos = last
    return value
end

local function parser_parse_number(p)
    local start = p.pos
    while p.pos < #p.src do
        local c = p.src:sub(p.pos + 1, p.pos + 1)
        if c == "-" or (c >= "0" and c <= "9") then
            p.pos = p.pos + 1
        else
            break
        end
    end
    if p.pos == start then
        parser_fail(p, "bad number at " .. start)
    end
    local v = tonumber(p.src:sub(start + 1, p.pos))
    if v == nil then
        parser_fail(p, "bad number at " .. start)
    end
    return v
end

local function parser_parse_string(p)
    parser_expect(p, "\"")
    local out = ""
    while true do
        if p.pos >= #p.src then
            parser_fail(p, "unterminated string")
        end
        local c = p.src:sub(p.pos + 1, p.pos + 1)
        if c == "\"" then
            p.pos = p.pos + 1
            return out
        end
        if c == "\\" then
            p.pos = p.pos + 1
            local e = parser_peek(p)
            if e == "n" then
                out = out .. "\n"
            elseif e == "t" then
                out = out .. "\t"
            else
                out = out .. e
            end
            p.pos = p.pos + 1
            goto continue
        end
        out = out .. c
        p.pos = p.pos + 1
        ::continue::
    end
end

local parser_parse_value  -- 前置声明:array/object 与 value 相互递归

local function parser_parse_array(p)
    parser_expect(p, "[")
    local arr = {}
    parser_skip_ws(p)
    if parser_peek(p) == "]" then
        p.pos = p.pos + 1
        return arr
    end
    -- Lua 的 return 必须是块内最后一条语句,continue 标签放在循环体顶部,向后 goto 回去。
    while true do
        ::continue::
        arr[#arr + 1] = parser_parse_value(p)
        parser_skip_ws(p)
        if parser_peek(p) == "," then
            p.pos = p.pos + 1
            goto continue
        end
        parser_expect(p, "]")
        return arr
    end
end

local function parser_parse_object(p)
    parser_expect(p, "{")
    local obj = {}
    parser_skip_ws(p)
    if parser_peek(p) == "}" then
        p.pos = p.pos + 1
        return obj
    end
    while true do
        ::continue::
        local key = parser_parse_string(p)
        parser_skip_ws(p)
        parser_expect(p, ":")
        obj[key] = parser_parse_value(p)
        parser_skip_ws(p)
        if parser_peek(p) == "," then
            p.pos = p.pos + 1
            goto continue
        end
        parser_expect(p, "}")
        return obj
    end
end

parser_parse_value = function(p)
    parser_skip_ws(p)
    local c = parser_peek(p)
    if c == "{" then
        return parser_parse_object(p)
    end
    if c == "[" then
        return parser_parse_array(p)
    end
    if c == "\"" then
        return parser_parse_string(p)
    end
    if c == "n" then
        return parser_parse_lit(p, "null", NIL)
    end
    return parser_parse_number(p)
end

local function parse_json(src)
    local p = {src = src, pos = 0}
    local v = parser_parse_value(p)
    parser_skip_ws(p)
    if p.pos ~= #src then
        parser_fail(p, "trailing content")
    end
    return v
end

local function count_nodes(v)
    if is_list(v) then
        local n = 1
        for _, item in ipairs(v) do
            n = n + count_nodes(item)
        end
        return n
    end
    if type(v) == "table" then
        local n = 1
        for k in pairs(v) do
            n = n + count_nodes(v[k])
        end
        return n
    end
    return 1
end

local t0 = os.clock()

local docs = {}
for _ = 1, doc_count do
    docs[#docs + 1] = json_stringify(gen_doc())
end

local node_acc = 0
local roundtrip_ok = 0
for _ = 1, passes do
    for _, text in ipairs(docs) do
        local v = parse_json(text)
        if json_stringify(v) == text then
            roundtrip_ok = roundtrip_ok + 1
        end
        node_acc = (node_acc + count_nodes(v)) % 1000003
    end
end

local valid = 0
local invalid = 0
for i = 0, snippets - 1 do
    local text = docs[i % doc_count + 1]
    local probe = text:sub(1, 1 + rnd(#text // 2))
    if rnd(3) == 0 then
        probe = probe .. "}"
    elseif rnd(3) == 0 then
        probe = probe .. text:sub(1, 1)
    end
    if pcall(parse_json, probe) then
        valid = valid + 1
    else
        invalid = invalid + 1
    end
end

local dt = (os.clock() - t0) * 1000.0

assert(roundtrip_ok == doc_count * passes, "roundtrip drift")
assert(roundtrip_ok == 2400, "roundtrip drift")
assert(node_acc == 31480, "node_acc drift")
assert(valid == 58, "valid drift")
assert(invalid == 1942, "invalid drift")
print("value roundtrip_ok: " .. roundtrip_ok)
print("value node_acc: " .. node_acc)
print("value valid: " .. valid)
print("value invalid: " .. invalid)
print("bench-time: " .. dt .. " ms")
