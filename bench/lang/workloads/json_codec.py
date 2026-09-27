"""json_codec 参照端口(与 json_codec.aria 同算法同规模):生成嵌套文档并序列化,再走
「解析 -> 再序列化 -> 逐位比对」往返,外加剧段截断片段的合法性校验。解析器手写递归下降,
禁用内建 json 库;序列化按 canonical 约定对对象键排序;rnd 调用步序与 aria 逐行对齐。"""
import time

doc_count = 120  # 规模:文档数
passes = 20      # 规模:往返遍数
snippets = 2000  # 规模:校验片段数

state = 7


def rnd(n):
    global state
    state = (state * 48271) % 2147483647
    return state % n


names = ["id", "name", "count", "active", "tags", "meta", "score", "note", "kind", "level"]
words = ["core", "edge", "node", "wire", "mesh", "flux", "grid", "pulse"]


def gen_string():
    n = 2 + rnd(4)
    out = ""
    i = 0
    while i < n:
        out += words[rnd(len(words))]
        k = rnd(9)
        if k == 0:
            out += "\\"
        elif k == 1:
            out += "\""
        elif k == 2:
            out += "\n"
        i += 1
    return out


def gen_value(depth):
    t = rnd(10)
    if depth >= 3 or t < 3:
        return rnd(100000)
    if t == 3:
        return gen_string()
    if t == 4:
        return None
    if t <= 6:
        arr = []
        n = 1 + rnd(4)
        i = 0
        while i < n:
            arr.append(gen_value(depth + 1))
            i += 1
        return arr
    obj = {}
    n = 1 + rnd(3)
    i = 0
    while i < n:
        # Python 对 obj[key] = value 先求右侧再求键,显式先取键以保住与 aria 一致的 rnd 步序。
        key = names[rnd(len(names))]
        obj[key] = gen_value(depth + 1)
        i += 1
    return obj


def gen_doc():
    obj = {}
    n = 3 + rnd(4)
    i = 0
    while i < n:
        key = names[rnd(len(names))]
        obj[key] = gen_value(1)
        i += 1
    return obj


def escape_json(s):
    out = "\""
    i = 0
    while i < len(s):
        c = s[i]
        if c == "\"" or c == "\\":
            out += "\\" + c
        elif c == "\n":
            out += "\\n"
        elif c == "\t":
            out += "\\t"
        else:
            out += c
        i += 1
    return out + "\""


def json_stringify(v):
    if v is None:
        return "null"
    if isinstance(v, int):
        return str(v)
    if isinstance(v, str):
        return escape_json(v)
    if isinstance(v, list):
        out = "["
        first = True
        for item in v:
            if not first:
                out += ","
            first = False
            out += json_stringify(item)
        return out + "]"
    out = "{"
    ks = list(v.keys())
    ks.sort()
    first = True
    for k in ks:
        if not first:
            out += ","
        first = False
        out += escape_json(k) + ":" + json_stringify(v[k])
    return out + "}"


class JsonError(Exception):
    pass


class JsonParser:
    def __init__(self, src):
        self.src = src
        self.pos = 0

    def fail(self, msg):
        raise JsonError("json: " + msg)

    def skip_ws(self):
        while self.pos < len(self.src):
            c = self.src[self.pos]
            if c == " " or c == "\n" or c == "\t" or c == "\r":
                self.pos += 1
            else:
                break

    def peek(self):
        if self.pos >= len(self.src):
            self.fail("unexpected end")
        return self.src[self.pos]

    def expect(self, ch):
        if self.peek() != ch:
            self.fail("expected '" + ch + "' at " + str(self.pos))
        self.pos += 1

    def parse_lit(self, word, value):
        end = self.pos + len(word)
        if end > len(self.src) or self.src[self.pos:end] != word:
            self.fail("bad literal")
        self.pos = end
        return value

    def parse_number(self):
        start = self.pos
        while self.pos < len(self.src):
            c = self.src[self.pos]
            if c == "-" or (c >= "0" and c <= "9"):
                self.pos += 1
            else:
                break
        if self.pos == start:
            self.fail("bad number at " + str(start))
        try:
            return int(self.src[start:self.pos])
        except ValueError:
            self.fail("bad number at " + str(start))

    def parse_string(self):
        self.expect("\"")
        out = ""
        while True:
            if self.pos >= len(self.src):
                self.fail("unterminated string")
            c = self.src[self.pos]
            if c == "\"":
                self.pos += 1
                return out
            if c == "\\":
                self.pos += 1
                e = self.peek()
                if e == "n":
                    out += "\n"
                elif e == "t":
                    out += "\t"
                else:
                    out += e
                self.pos += 1
                continue
            out += c
            self.pos += 1

    def parse_array(self):
        self.expect("[")
        arr = []
        self.skip_ws()
        if self.peek() == "]":
            self.pos += 1
            return arr
        while True:
            arr.append(self.parse_value())
            self.skip_ws()
            if self.peek() == ",":
                self.pos += 1
                continue
            self.expect("]")
            return arr

    def parse_object(self):
        self.expect("{")
        obj = {}
        self.skip_ws()
        if self.peek() == "}":
            self.pos += 1
            return obj
        while True:
            key = self.parse_string()
            self.skip_ws()
            self.expect(":")
            obj[key] = self.parse_value()
            self.skip_ws()
            if self.peek() == ",":
                self.pos += 1
                continue
            self.expect("}")
            return obj

    def parse_value(self):
        self.skip_ws()
        c = self.peek()
        if c == "{":
            return self.parse_object()
        if c == "[":
            return self.parse_array()
        if c == "\"":
            return self.parse_string()
        if c == "n":
            return self.parse_lit("null", None)
        return self.parse_number()


def parse_json(src):
    p = JsonParser(src)
    v = p.parse_value()
    p.skip_ws()
    if p.pos != len(src):
        p.fail("trailing content")
    return v


def count_nodes(v):
    if isinstance(v, list):
        n = 1
        for item in v:
            n += count_nodes(item)
        return n
    if isinstance(v, dict):
        n = 1
        for k in v.keys():
            n += count_nodes(v[k])
        return n
    return 1


t0 = time.perf_counter()

docs = []
i = 0
while i < doc_count:
    docs.append(json_stringify(gen_doc()))
    i += 1

node_acc = 0
roundtrip_ok = 0
p = 0
while p < passes:
    for text in docs:
        v = parse_json(text)
        if json_stringify(v) == text:
            roundtrip_ok += 1
        node_acc = (node_acc + count_nodes(v)) % 1000003
    p += 1

valid = 0
invalid = 0
i = 0
while i < snippets:
    text = docs[i % doc_count]
    probe = text[0:1 + rnd(len(text) // 2)]
    if rnd(3) == 0:
        probe = probe + "}"
    elif rnd(3) == 0:
        probe = probe + text[0]
    try:
        parse_json(probe)
        valid += 1
    except Exception:
        invalid += 1
    i += 1

dt = (time.perf_counter() - t0) * 1000.0

assert roundtrip_ok == doc_count * passes, "json_codec roundtrip drift"
assert roundtrip_ok == 2400, "json_codec roundtrip drift"
assert node_acc == 31480, "json_codec node_acc drift"
assert valid == 58, "json_codec valid drift"
assert invalid == 1942, "json_codec invalid drift"


print("value roundtrip_ok:", roundtrip_ok)
print("value node_acc:", node_acc)
print("value valid:", valid)
print("value invalid:", invalid)
print("bench-time:", dt, "ms")
