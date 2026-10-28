"""A minimal S-expression reader and writer for KiCad files."""
import re

_TOKEN = re.compile(r'\s*(?:(\()|(\))|("(?:[^"\\]|\\.)*")|([^\s()"]+))')


class Sym(str):
    """An unquoted atom (keyword or number), as opposed to a quoted string."""


_ESC = {"n": "\n", "t": "\t", '"': '"', "\\": "\\"}


def _unescape(s):
    return re.sub(r"\\(.)", lambda m: _ESC.get(m.group(1), m.group(1)), s)


def parse(text):
    stack, cur = [], []
    pos = 0
    while True:
        m = _TOKEN.match(text, pos)
        if not m or m.end() == pos:
            break
        pos = m.end()
        if m.group(1):
            stack.append(cur)
            cur = []
        elif m.group(2):
            done = cur
            cur = stack.pop()
            cur.append(done)
        elif m.group(3) is not None:
            cur.append(_unescape(m.group(3)[1:-1]))
        else:
            cur.append(Sym(m.group(4)))
    return cur[0] if len(cur) == 1 else cur


def quote(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n") + '"'


def dump(node, indent=0):
    if isinstance(node, list):
        if not node:
            return "()"
        flat = all(not isinstance(x, list) for x in node)
        if flat:
            return "(" + " ".join(dump(x) for x in node) + ")"
        head = []
        i = 0
        while i < len(node) and not isinstance(node[i], list):
            head.append(dump(node[i]))
            i += 1
        pad = "\t" * (indent + 1)
        body = "".join("\n" + pad + dump(x, indent + 1) for x in node[i:])
        return "(" + " ".join(head) + body + "\n" + "\t" * indent + ")"
    if isinstance(node, Sym):
        return str(node)
    if isinstance(node, (int, float)):
        return fmt_num(node)
    return quote(node)


def fmt_num(v):
    if isinstance(v, int):
        return str(v)
    s = f"{v:.4f}".rstrip("0").rstrip(".")
    return "0" if s in ("-0", "") else s


def find(node, key):
    return [x for x in node if isinstance(x, list) and x and x[0] == key]


def first(node, key):
    r = find(node, key)
    return r[0] if r else None
