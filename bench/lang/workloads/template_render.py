"""template_render 参照端口(与 template_render.aria 同算法同规模):mustache 式模板编译成捕获
片段的闭包列表(文本/变量/section 三种段,section 递归编译),对 LCG 造的订单数据反复渲染;
变量段做 HTML 转义与缺失键兜底。"""
import time

orders_n = 900  # 规模:订单数
rounds = 20     # 规模:渲染轮数

state = 2024


def rnd(n):
    global state
    state = (state * 48271) % 2147483647
    return state % n


def esc_html(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def lookup(d, name):
    if name in d:
        return d[name]
    return None


def find_from(src, needle, from_):
    # str.find 带 start 即「在 src[from_:] 里找」,命中返回 from_ + 局部偏移,未命中 -1(对应 nil)。
    return src.find(needle, from_)


def render(segs, data):
    out = ""
    for seg in segs:
        out += seg(data)
    return out


# 段工厂:闭包捕获的是变量而非当轮值,直接在循环里写 lambda 会全部捕到最后一轮的值,
# 经工厂参数绑定当轮值。
def make_text(t):
    return lambda d: t


def make_var(name):
    def seg(d):
        v = lookup(d, name)
        if v is None:
            return ""
        return esc_html(str(v))
    return seg


def make_section(name, body):
    def seg(d):
        items = lookup(d, name)
        if items is None:
            return ""
        out = ""
        for item in items:
            out += render(body, item)
        return out
    return seg


def compile(src):
    segs = []
    pos = 0
    while True:
        open_ = find_from(src, "{{", pos)
        if open_ < 0:
            rest = src[pos:]
            if len(rest) > 0:
                segs.append(make_text(rest))
            break
        if open_ > pos:
            segs.append(make_text(src[pos:open_]))
        close = find_from(src, "}}", open_)
        tag = src[open_ + 2:close]
        if tag[0] == "#":
            name = tag[1:]
            endtag = "{{/" + name + "}}"
            end = find_from(src, endtag, close)
            body = compile(src[close + 2:end])
            segs.append(make_section(name, body))
            pos = end + len(endtag)
        else:
            segs.append(make_var(tag))
            pos = close + 2
    return segs


t0 = time.perf_counter()

item_names = ["widget", "gadget", "cable", "mount", "panel", "sensor"]
buyers = ["acme", "globex", "initech", "umbrella", "stark", "wayne"]
orders = []
i = 0
while i < orders_n:
    items = []
    n = 1 + rnd(4)
    j = 0
    while j < n:
        items.append({"name": item_names[rnd(len(item_names))], "qty": 1 + rnd(9), "price": 5 + rnd(200)})
        j += 1
    orders.append({"id": 1000 + i, "customer": buyers[rnd(len(buyers))], "items": items})
    i += 1

tpl_invoice = compile("Invoice #{{id}} -- {{customer}}\n{{#items}}  {{name}} x{{qty}} @ {{price}}\n{{/items}}")
tpl_receipt = compile("Receipt {{id}} ({{customer}}): {{#items}}{{qty}}x{{name}};{{/items}}")
tpl_summary = compile("Summary {{customer}} {{missing_key}} {{#items}}{{name}},{{/items}}")

total = 0
r = 0
while r < rounds:
    for o in orders:
        a = render(tpl_invoice, o)
        b = render(tpl_receipt, o)
        c = render(tpl_summary, o)
        total = (total + len(a) + len(b) + len(c)) % 1000003
    r += 1

assert total == 552394, "template_render checksum drift"

sample = len(render(tpl_invoice, orders[0]))
dt = (time.perf_counter() - t0) * 1000.0

print("value checksum:", total)
print("value sample:", sample)
print("bench-time:", dt, "ms")
