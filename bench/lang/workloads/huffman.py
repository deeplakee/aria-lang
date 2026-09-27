"""huffman 参照端口(与 huffman.aria 同算法同规模):倾斜词频文本 -> 频率统计(建树队列按
词首次出现序)-> 插入序稳定队列建树 -> 递归码表 -> 逐字符编码 -> 逐位解码还原。"""
import time

messages = 1150
msg_len = 80

letters = "etaoinshrdlucmfwypvbgkxjqz"
weights = [113, 90, 80, 75, 70, 67, 63, 61, 60, 43, 40, 28, 24, 22, 21, 19, 19, 18, 17, 16, 15, 13, 10, 9, 5, 2]

state = 31337


def rnd(n):
    global state
    state = (state * 48271) % 2147483647
    return state % n


def gen_char():
    r = rnd(1000)
    acc = 0
    for i in range(len(weights)):
        acc += weights[i]
        if r < acc:
            return letters[i]
    return letters[0]


class HNode:
    __slots__ = ("w", "ch", "left", "right")

    def __init__(self, w, ch, left, right):
        self.w = w
        self.ch = ch
        self.left = left
        self.right = right

    def is_leaf(self):
        return self.left is None


t0 = time.perf_counter()

texts = []
for _ in range(messages):
    parts = []
    for _ in range(msg_len):
        parts.append(gen_char())
    texts.append("".join(parts))

freq = {}
order = []
for msg in texts:
    for ch in msg:
        if ch in freq:
            freq[ch] += 1
        else:
            freq[ch] = 1
            order.append(ch)

pq = []
for ch in order:
    n = freq[ch]
    pos = len(pq)
    while pos > 0 and pq[pos - 1].w > n:
        pos -= 1
    pq.insert(pos, HNode(n, ch, None, None))
while len(pq) > 1:
    a = pq.pop(0)
    b = pq.pop(0)
    merged = HNode(a.w + b.w, "", a, b)
    pos = len(pq)
    while pos > 0 and pq[pos - 1].w > merged.w:
        pos -= 1
    pq.insert(pos, merged)
root = pq[0]

codes = {}


def walk(node, prefix):
    if node.is_leaf():
        codes[node.ch] = prefix
        return
    walk(node.left, prefix + "0")
    walk(node.right, prefix + "1")


walk(root, "")

bit_total = 0
encoded = []
for msg in texts:
    bits = []
    for ch in msg:
        code = codes[ch]
        bit_total += len(code)
        for j in range(len(code)):
            bits.append(code[j])
    encoded.append("".join(bits))

decoded_ok = 0
for di in range(messages):
    enc = encoded[di]
    node = root
    out = []
    for i in range(len(enc)):
        if enc[i] == "0":
            node = node.left
        else:
            node = node.right
        if node.is_leaf():
            out.append(node.ch)
            node = root
    if "".join(out) == texts[di]:
        decoded_ok += 1

assert len(codes) == 26, "huffman symbols drift"
assert decoded_ok == messages, "huffman decode drift"

dt = (time.perf_counter() - t0) * 1000.0

print("value symbols:", len(codes))
print("value bit_total:", bit_total)
print("value decoded_ok:", decoded_ok)
print("bench-time:", dt, "ms")
