-- binary_trees 参照端口(与 binary_trees.aria 同算法同规模)。
local function make_tree(d)
    if d == 0 then
        return {left = nil, right = nil}
    end
    return {left = make_tree(d - 1), right = make_tree(d - 1)}
end

local function count_nodes(node)
    if node == nil then
        return 0
    end
    return 1 + count_nodes(node.left) + count_nodes(node.right)
end

local depth = 12
local iterations = 300

local t0 = os.clock()
local total = 0
for _ = 1, iterations do
    total = total + count_nodes(make_tree(depth))
end
local dt = (os.clock() - t0) * 1000.0

assert(total == 2457300, "checksum drift")
assert(total == iterations * 8191, "node count drift")
print("value checksum: " .. total)
print("value per_tree: " .. count_nodes(make_tree(depth)))
print("bench-time: " .. dt .. " ms")
