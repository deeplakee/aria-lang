"""binary_trees 参照端口(与 binary_trees.aria 同算法同规模):建满二叉树再数节点,量分配与字段
访问;节点数是闭式 2^(depth+1)-1。"""
import time

depth = 12
iterations = 300


class Node:
    def __init__(self, left, right):
        self.left = left
        self.right = right


def make_tree(d):
    if d == 0:
        return Node(None, None)
    return Node(make_tree(d - 1), make_tree(d - 1))


def count_nodes(node):
    if node is None:
        return 0
    return 1 + count_nodes(node.left) + count_nodes(node.right)


t0 = time.perf_counter()
total = 0
i = 0
while i < iterations:
    total += count_nodes(make_tree(depth))
    i += 1
dt = (time.perf_counter() - t0) * 1000.0

assert total == 2457300, "binary_trees total drift"
assert total == iterations * 8191, "binary_trees node count drift"


print("value checksum:", total)
print("value per_tree:", 2 ** (depth + 1) - 1)
print("bench-time:", dt, "ms")
