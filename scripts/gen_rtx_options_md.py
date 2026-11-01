#!/usr/bin/env python3
"""Generates RtxOptions.md from the RTX_OPTION declarations in src/ and the NUMOS settings table.

Usage, from anywhere:
  python scripts/gen_rtx_options_md.py            # writes RtxOptions.md at the repo root
  python scripts/gen_rtx_options_md.py --check    # exits 1 when RtxOptions.md is out of date
  python scripts/gen_rtx_options_md.py --json out.json   # also dumps the parsed options

The output matches the table the runtime writes with DXVK_DOCUMENTATION_WRITE_RTX_OPTIONS_MD=1:
name, type, default, min, max and description, with simple and complex types in separate tables.
Defaults and limits are evaluated from the C++ source, so an expression the evaluator cannot
reduce is an error that names the option and the expression.
"""

import argparse
import json
import math
import os
import re
import struct
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_DIR = os.path.join(REPO_ROOT, "src")
NUMOS_FIELDS = os.path.join(REPO_ROOT, "submodules", "numos", "include", "numos", "settings_fields.inl")
NUMOS_MAX_MOONS = 4
EXTRA_HEADERS = [
  "include/vulkan/include/vulkan/vulkan_core.h",
  "submodules/nrc/Include/NrcStructures.h",
  "submodules/nrd/Include/NRDDescs.h",
]
KEYBIND_HEADER = os.path.join(SRC_DIR, "util", "util_keybind.h")

HEADER = """# RTX Options

This file contains a complete reference of all configurable RTX Options in RTX Remix.

For detailed documentation on the RtxOption system architecture, including layers, priorities, and how values are resolved, see [RemixConfig.md](documentation/RemixConfig.md).

This file is generated from the source by `scripts/gen_rtx_options_md.py`. Run `python scripts/gen_rtx_options_md.py` after adding or changing an RTX option, and commit the result.

"""

TABLE_HEADER = (
  "| RTX Option | Type | Default Value | Min Value | Max Value | Description |\n"
  "| :-- | :-: | :-: | :-: | :-: | :-- |\n"
)

INT_TYPES = {
  "int", "int8_t", "int16_t", "int32_t", "uint8_t", "uint16_t", "uint32_t", "size_t", "char", "uint",
  "unsigned", "unsigned int",
}
TYPE_NAMES = {
  "bool": "bool",
  "float": "float",
  "fast_unordered_set": "hash set",
  "std::vector<XXH64_hash_t>": "hash vector",
  "VirtualKeys": "virtual keys",
  "Vector2": "float2",
  "Vector3": "float3",
  "Vector2i": "int2",
  "Vector4": "float4",
  "std::string": "string",
}
LONG_TYPES = {"hash set", "hash vector", "virtual keys", "string"}
VECTOR_SIZES = {"float2": 2, "float3": 3, "int2": 2, "float4": 4}

MACRO_RE = re.compile(r"\bRTX_OPTION(_ARGS|_ENV|_FLAG|_FLAG_ENV|_FULL)?\s*\(")


class GenError(Exception):
  pass


# ---------------------------------------------------------------------------
# Source scanning
# ---------------------------------------------------------------------------

def read_text(path):
  with open(path, "rb") as f:
    return f.read().decode("utf-8", errors="replace")


def strip_comments(text):
  """Replaces comments with spaces, keeping string and character literals and line breaks."""
  out = []
  i = 0
  n = len(text)
  while i < n:
    c = text[i]
    if c == "/" and i + 1 < n and text[i + 1] == "/":
      j = text.find("\n", i)
      if j < 0:
        j = n
      out.append(" " * (j - i))
      i = j
    elif c == "/" and i + 1 < n and text[i + 1] == "*":
      j = text.find("*/", i + 2)
      j = n if j < 0 else j + 2
      out.append(re.sub(r"[^\n]", " ", text[i:j]))
      i = j
    elif c == 'R' and text.startswith('R"', i) and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == "_")):
      m = re.match(r'R"([^(\s]*)\(', text[i:])
      if m:
        end = text.find(")" + m.group(1) + '"', i)
        end = n if end < 0 else end + len(m.group(1)) + 2
        out.append(text[i:end])
        i = end
      else:
        out.append(c)
        i += 1
    elif c == '"' or (c == "'" and not (i > 0 and text[i - 1].isalnum())):
      j = i + 1
      while j < n and text[j] != c:
        if text[j] == "\\":
          j += 1
        elif text[j] == "\n":
          break
        j += 1
      out.append(text[i:j + 1])
      i = j + 1
    else:
      out.append(c)
      i += 1
  return "".join(out)


def scan_call(text, start):
  """Given the index just past '(', returns (args, end) with the top-level comma-separated args."""
  depth = 0
  args = []
  cur = start
  i = start
  n = len(text)
  while i < n:
    c = text[i]
    if c == "R" and text.startswith('R"', i) and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == "_")):
      raw = re.match(r'R"([^(\s]*)\(', text[i:])
      if raw:
        end = text.find(")" + raw.group(1) + '"', i)
        if end >= 0:
          i = end + len(raw.group(1)) + 2
          continue
    if c == '"' or (c == "'" and not (i > 0 and text[i - 1].isalnum())):
      j = i + 1
      while j < n and text[j] != c:
        if text[j] == "\\":
          j += 1
        j += 1
      i = j + 1
      continue
    if c in "([{":
      depth += 1
    elif c in ")]}":
      if depth == 0:
        args.append(text[cur:i].strip())
        return args, i + 1
      depth -= 1
    elif c == "," and depth == 0:
      args.append(text[cur:i].strip())
      cur = i + 1
    i += 1
  raise GenError("unterminated macro call")


def split_top(text, sep=","):
  args, _ = scan_call(text + ")", 0)
  return args


C_ESCAPES = {"n": "\n", "t": "\t", "r": "\r", "\\": "\\", '"': '"', "'": "'", "0": "\0", "a": "\a", "b": "\b",
             "f": "\f", "v": "\v", "?": "?"}


def unescape(body):
  out = []
  i = 0
  while i < len(body):
    c = body[i]
    if c == "\\" and i + 1 < len(body):
      e = body[i + 1]
      if e == "x":
        m = re.match(r"[0-9a-fA-F]+", body[i + 2:])
        out.append(chr(int(m.group(0), 16)))
        i += 2 + len(m.group(0))
        continue
      if e in "01234567":
        m = re.match(r"[0-7]{1,3}", body[i + 1:])
        out.append(chr(int(m.group(0), 8)))
        i += 1 + len(m.group(0))
        continue
      out.append(C_ESCAPES.get(e, e))
      i += 2
      continue
    out.append(c)
    i += 1
  return "".join(out)


STRING_PIECE_RE = re.compile(r'\s*(?:u8|L|u|U)?"((?:[^"\\]|\\.)*)"\s*', re.S)
RAW_STRING_RE = re.compile(r'\s*(?:u8|L|u|U)?R"([^(\s]*)\(')


def parse_string_concat(expr):
  """Parses adjacent string literals ("a" "b"). Returns None when the expression is anything else."""
  pos = 0
  pieces = []
  expr = expr.strip()
  if not expr:
    return ""
  while pos < len(expr):
    raw = RAW_STRING_RE.match(expr, pos)
    if raw:
      end = expr.find(")" + raw.group(1) + '"', raw.end())
      if end < 0:
        return None
      pieces.append(expr[raw.end():end])
      pos = end + len(raw.group(1)) + 2
      while pos < len(expr) and expr[pos].isspace():
        pos += 1
      continue
    m = STRING_PIECE_RE.match(expr, pos)
    if not m:
      return None
    pieces.append(unescape(m.group(1)))
    pos = m.end()
  return "".join(pieces)


# ---------------------------------------------------------------------------
# Constant and enum tables
# ---------------------------------------------------------------------------

class Symbols:
  def __init__(self):
    self.consts = {}      # short name -> list of expression text
    self.enums = {}       # (enum name, value name) -> int
    self.enum_values = {}  # value name -> list of ints, for unscoped enums
    self.enum_types = set()
    self.aliases = {}     # using Alias = Some::Type; -> "Type"
    self.keynames = {}    # VK symbol or char literal -> name

  def add_const(self, name, expr):
    self.consts.setdefault(name, []).append(expr)


CONST_RE = re.compile(
  r"(?:^|[;{}\s])(?:inline\s+|static\s+|constexpr\s+|const\s+)+(?:[\w:<>]+\s+)?(?:const\s+)?(\w+)\s*(?:=\s*([^;]+?)|\{([^;]*?)\})\s*;",
  re.S)
ENUM_RE = re.compile(r"\benum\s+(class\s+|struct\s+)?(\w+)\s*(?::\s*[\w:]+\s*)?\{([^}]*)\}", re.S)
USING_RE = re.compile(r"\busing\s+(\w+)\s*=\s*([\w:]+)\s*;")
DEFINE_RE = re.compile(r"^\s*#\s*define\s+(\w+)\s+([^\n\\]+)$", re.M)


SCOPE_HEAD_RE = re.compile(r"\b(?:class|struct|union|namespace)\s+(?:\w+\s+)*?(\w+)\s*(?:final\s*)?(?::[^{;]*)?$")


def scope_map(text):
  """Returns a sorted list of (position, enclosing class or namespace name) changes for the text."""
  changes = [(0, None)]
  stack = []
  head_start = 0
  i = 0
  n = len(text)
  while i < n:
    c = text[i]
    if c == '"' or (c == "'" and not (i > 0 and text[i - 1].isalnum())):
      j = i + 1
      while j < n and text[j] != c and text[j] != "\n":
        if text[j] == "\\":
          j += 1
        j += 1
      i = j + 1
      continue
    if c == "{":
      m = SCOPE_HEAD_RE.search(text[head_start:i].strip())
      stack.append(m.group(1) if m and not re.search(r"\benum\b", text[head_start:i]) else None)
      named = [x for x in stack if x]
      changes.append((i + 1, named[-1] if named else None))
      head_start = i + 1
    elif c == "}":
      if stack:
        stack.pop()
      named = [x for x in stack if x]
      changes.append((i + 1, named[-1] if named else None))
      head_start = i + 1
    elif c == ";":
      head_start = i + 1
    i += 1
  return changes


def scope_at(changes, pos):
  lo, hi = 0, len(changes) - 1
  while lo < hi:
    mid = (lo + hi + 1) // 2
    if changes[mid][0] <= pos:
      lo = mid
    else:
      hi = mid - 1
  return changes[lo][1]


def collect_symbols(files):
  sym = Symbols()
  for path, text in files:
    scopes = scope_map(text)
    for m in ENUM_RE.finditer(text):
      enum_name = m.group(2)
      scoped = bool(m.group(1))
      outer = scope_at(scopes, m.start())
      sym.enum_types.add(enum_name)
      value = -1
      for entry in split_top(m.group(3)):
        entry = re.sub(r"#.*", "", entry).strip()
        if not entry:
          continue
        if "=" in entry:
          vname, vexpr = [s.strip() for s in entry.split("=", 1)]
          try:
            value = int(evaluate_number(vexpr, sym, enum_scope=enum_name))
          except GenError:
            continue
        else:
          vname = entry
          value += 1
        vname = vname.split()[0] if vname else vname
        sym.enums[(enum_name, vname)] = value
        if outer:
          sym.enums[(outer + "::" + enum_name, vname)] = value
        if not scoped:
          sym.enum_values.setdefault(vname, []).append(value)
    for m in USING_RE.finditer(text):
      sym.aliases[m.group(1)] = m.group(2)
    for m in CONST_RE.finditer(text):
      name = m.group(1)
      expr = m.group(2) if m.group(2) is not None else m.group(3)
      if expr is not None and name not in ("return",):
        sym.add_const(name, expr.strip())
    for m in DEFINE_RE.finditer(text):
      sym.add_const(m.group(1), m.group(2).strip())
  # Key names come from util_keybind.h: the first name of each Key{{"NAME", ...}, VK}.
  for m in re.finditer(r'Key\{\{"([^"]+)"[^}]*\},\s*([^}]+?)\}', read_text(KEYBIND_HEADER)):
    sym.keynames.setdefault(m.group(2).strip(), m.group(1))
  return sym


# ---------------------------------------------------------------------------
# Expression evaluation
# ---------------------------------------------------------------------------

class Vec(tuple):
  pass


def to_f32(v):
  return struct.unpack("<f", struct.pack("<f", float(v)))[0]


def fmt_float(v):
  # std::ostream's default float formatting: %g with 6 significant digits.
  s = "%g" % to_f32(v)
  return s


def fmt_int(v):
  v = int(v) & 0xFFFFFFFF
  return str(v - (1 << 32) if v >= (1 << 31) else v)


BUILTIN_CONSTS = {
  "INT32_MAX": 2147483647, "INT32_MIN": -2147483648, "UINT32_MAX": 4294967295, "UINT8_MAX": 255,
  "UINT16_MAX": 65535, "INT_MAX": 2147483647, "UINT_MAX": 4294967295, "FLT_MAX": 3.402823466e+38,
  "FLT_MIN": 1.175494351e-38, "FLT_EPSILON": 1.192092896e-07, "M_PI": math.pi,
}
LIMITS = {
  "uint8_t": (0, 255), "int8_t": (-128, 127), "uint16_t": (0, 65535), "int16_t": (-32768, 32767),
  "uint32_t": (0, 4294967295), "unsigned": (0, 4294967295), "uint": (0, 4294967295),
  "int32_t": (-2147483648, 2147483647), "int": (-2147483648, 2147483647),
  "float": (-3.402823466e+38, 3.402823466e+38),
}


def numeric_limit(m):
  t = m.group(1).replace("std::", "")
  what = m.group(2)
  if t == "float":
    return {"max": "3.402823466e+38", "lowest": "-3.402823466e+38", "min": "1.175494351e-38",
            "infinity": "float('inf')"}[what]
  lo, hi = LIMITS[t]
  return str(hi if what == "max" else lo)


CAST_RE = re.compile(r"\b(?:static_cast|reinterpret_cast)\s*<[^<>]*(?:<[^<>]*>[^<>]*)*>")
C_CAST_RE = re.compile(r"\((?:const\s+)?(?:float|double|int|uint32_t|int32_t|uint8_t|uint16_t|size_t|uint|unsigned)\)")
NUM_RE = re.compile(r"(?<![\w.])((?:0[xX][0-9a-fA-F']+)|(?:\d[\d']*\.?\d*(?:[eE][+-]?\d+)?|\.\d+(?:[eE][+-]?\d+)?))([fFuUlL]*)\b")


class Evaluator:
  def __init__(self, sym, scope_hint=None, enum_scope=None):
    self.sym = sym
    self.scope_hint = scope_hint
    self.enum_scope = enum_scope
    self.depth = 0
    self.ambiguous = set()

  def lookup(self, qualified):
    parts = [p for p in qualified.split("::") if p]
    if parts[0] in self.sym.aliases:
      parts = self.sym.aliases[parts[0]].split("::") + parts[1:]
    name = parts[-1]
    if name in BUILTIN_CONSTS:
      return BUILTIN_CONSTS[name]
    # Function-style casts to an integer or enum type leave the value unchanged.
    if qualified.replace("std::", "") in INT_TYPES or name in self.sym.enum_types:
      return lambda v=0: v
    if len(parts) >= 3 and (parts[-3] + "::" + parts[-2], name) in self.sym.enums:
      return self.sym.enums[(parts[-3] + "::" + parts[-2], name)]
    if len(parts) >= 2 and (parts[-2], name) in self.sym.enums:
      return self.sym.enums[(parts[-2], name)]
    if self.enum_scope and (self.enum_scope, name) in self.sym.enums:
      return self.sym.enums[(self.enum_scope, name)]
    if name in self.sym.consts:
      errors = []
      if len(set(self.sym.consts[name])) > 1:
        self.ambiguous.add(name)
      for expr in self.sym.consts[name]:
        try:
          return self.eval(expr)
        except GenError as e:
          errors.append(str(e))
      raise GenError("cannot evaluate constant %s (%s)" % (qualified, "; ".join(errors)))
    if name in self.sym.enum_values and len(set(self.sym.enum_values[name])) == 1:
      return self.sym.enum_values[name][0]
    raise GenError("unknown name %s" % qualified)

  def eval(self, expr):
    self.depth += 1
    if self.depth > 40:
      raise GenError("recursion in %s" % expr)
    try:
      return self._eval(expr)
    finally:
      self.depth -= 1

  def _eval(self, expr):
    expr = expr.strip()
    if not expr:
      return Vec(())
    s = parse_string_concat(expr)
    if s is not None and expr.startswith(('"', 'u8"', 'R"')):
      return s
    if expr.startswith("std::string(") and expr.endswith(")"):
      return self.eval(expr[len("std::string("):-1])
    # Python-side constructors for the vector types and braced lists.
    py = expr
    py = CAST_RE.sub("", py)
    py = C_CAST_RE.sub("", py)
    py = re.sub(r"\bstd::numeric_limits<\s*([\w:]+)\s*>::(max|min|lowest|infinity)\(\)", numeric_limit, py)
    py = re.sub(r"\bstd::numeric_limits<\s*float\s*>::infinity\(\)", "float('inf')", py)
    py = re.sub(r"\bstd::numeric_limits<\s*(?:uint32_t|unsigned int|uint)\s*>::max\(\)", "4294967295", py)
    py = re.sub(r"\bstd::numeric_limits<\s*(?:int32_t|int)\s*>::max\(\)", "2147483647", py)
    py = re.sub(r"\bstd::numeric_limits<\s*(?:int32_t|int)\s*>::(?:min|lowest)\(\)", "-2147483648", py)
    py = re.sub(r"\bstd::numeric_limits<\s*float\s*>::(?:lowest)\(\)", "-3.402823466e+38", py)
    py = re.sub(r"\b(?:std::)?(?:sqrtf?|std::sqrt)\s*\(", "_sqrt(", py)
    py = re.sub(r"\b(?:numos::|dxvk::)?(?:Vector[234]i?|vec[234]|float[234]|Vector2i)\s*[({]", "_vec(", py)
    py = re.sub(r"\btrue\b", "True", py)
    py = re.sub(r"\bfalse\b", "False", py)
    py = py.replace("||", " or ").replace("&&", " and ")
    py = re.sub(r"!(?!=)", " not ", py)
    # Numbers: drop suffixes and digit separators. A float literal stays a Python float.
    py = NUM_RE.sub(lambda m: m.group(1).replace("'", ""), py)
    # Braced lists become calls of _vec, which also covers {} for an empty set.
    py = py.replace("{", "_vec(").replace("}", ")")
    names = {}

    def repl(m):
      q = m.group(0)
      if q in ("True", "False", "and", "or", "not", "_vec", "_sqrt", "float", "inf"):
        return q
      key = "_n%d" % len(names)
      names[key] = q
      return key

    py = re.sub(r"(?<![\w.'])(?:::)?[A-Za-z_]\w*(?:::[A-Za-z_~]\w*)*", repl, py)
    env = {"_vec": lambda *a: Vec(a), "_sqrt": math.sqrt, "float": float, "__builtins__": {}}
    for key, q in names.items():
      env[key] = self.lookup(q)
    try:
      return eval(py, env)
    except GenError:
      raise
    except Exception as e:
      raise GenError("cannot evaluate '%s' (%s)" % (expr, e))


def evaluate_number(expr, sym, enum_scope=None):
  v = Evaluator(sym, enum_scope=enum_scope).eval(expr)
  if isinstance(v, (bool, int, float)):
    return v
  raise GenError("not a number: %s" % expr)


# ---------------------------------------------------------------------------
# Option records
# ---------------------------------------------------------------------------

def type_string(ctype, sym):
  t = re.sub(r"\s+", " ", ctype.strip())
  t = re.sub(r"^(?:dxvk::|numos::)", "", t)
  if t in TYPE_NAMES:
    return TYPE_NAMES[t]
  # Integer types and enums are stored as int.
  return "int"


def format_value(tstr, value, sym, expr):
  if tstr == "bool":
    if isinstance(value, Vec) and len(value) == 1:
      value = value[0]
    return "True" if value else "False"
  if tstr == "int":
    if isinstance(value, Vec) and len(value) == 1:
      value = value[0]
    if isinstance(value, Vec) and len(value) == 0:
      value = 0
    return fmt_int(value)
  if tstr == "float":
    if isinstance(value, Vec) and len(value) == 1:
      value = value[0]
    if isinstance(value, Vec) and len(value) == 0:
      value = 0.0
    return fmt_float(value)
  if tstr in VECTOR_SIZES:
    n = VECTOR_SIZES[tstr]
    if not isinstance(value, Vec):
      value = Vec([value] * n)
    elif len(value) == 1 and isinstance(value[0], Vec):
      value = value[0]
    if len(value) == 0:
      value = Vec([0] * n)
    if len(value) == 1:
      value = Vec(list(value) * n)
    if len(value) != n:
      raise GenError("wrong component count in %s" % expr)
    f = fmt_int if tstr == "int2" else fmt_float
    return ", ".join(f(c) for c in value)
  if tstr == "string":
    if isinstance(value, Vec) and len(value) == 0:
      return ""
    if isinstance(value, Vec) and len(value) == 1:
      value = value[0]
    if not isinstance(value, str):
      raise GenError("string option with a non-string default %s" % expr)
    return value
  if tstr == "hash set":
    values = flatten(value)
    return ", ".join("0x%016X" % (int(v) & 0xFFFFFFFFFFFFFFFF) for v in values)
  if tstr == "hash vector":
    values = flatten(value)
    return ", ".join("0x%016X" % (int(v) & 0xFFFFFFFFFFFFFFFF) for v in values)
  raise GenError("cannot format %s" % tstr)


def flatten(v):
  if isinstance(v, Vec):
    out = []
    for c in v:
      out.extend(flatten(c))
    return out
  return [v]


def virtual_keys_string(expr, sym, ev, seen=None):
  seen = seen or set()
  expr = expr.strip()
  keys = re.findall(r"VirtualKey\s*\{\s*([^}]+?)\s*\}", expr)
  if not keys:
    name = expr.split("::")[-1]
    if name in sym.consts and name not in seen:
      seen.add(name)
      return virtual_keys_string(sym.consts[name][0], sym, ev, seen)
    if expr in ("{}", ""):
      return ""
    raise GenError("cannot read virtual keys %s" % expr)
  names = []
  for k in keys:
    if k in sym.keynames:
      names.append(sym.keynames[k])
    else:
      raise GenError("unknown key %s" % k)
  return ",".join(names)


def markdown_escape(text):
  out = []
  for c in text:
    if c == "<":
      out.append("\\<")
    elif c == ">":
      out.append("\\>")
    elif c == "\n":
      out.append("<br>")
    elif c in "\\`*_{}[]()#+-.!|":
      out.append("\\" + c)
    else:
      out.append(c)
  return "".join(out)


ARGS_FIELD_RE = re.compile(r"\bargs\.(\w+)\s*(\|?=)\s*")


def parse_option_args(text):
  """Parses `args.x = y` statements from the variadic part of RTX_OPTION_ARGS / RTX_OPTION_FULL."""
  fields = {}
  for stmt in split_statements(text):
    m = ARGS_FIELD_RE.match(stmt.strip())
    if m:
      fields[m.group(1)] = stmt.strip()[m.end():].strip()
  return fields


def split_statements(text):
  out = []
  depth = 0
  cur = []
  i = 0
  while i < len(text):
    c = text[i]
    if c in "([{":
      depth += 1
    elif c in ")]}":
      depth -= 1
    if (c == ";" or c == ",") and depth == 0:
      out.append("".join(cur))
      cur = []
    else:
      cur.append(c)
    i += 1
  out.append("".join(cur))
  return out


def expand_stringify(expr, sym):
  """Replaces STRINGIFY(MACRO) with the macro's text as a string literal, as the preprocessor does."""
  def repl(m):
    name = m.group(1)
    text = sym.consts[name][0] if name in sym.consts else name
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'
  return re.sub(r"\bSTRINGIFY\s*\(\s*(\w+)\s*\)", repl, expr)


def parse_rtx_options(files, sym):
  options = []
  for path, text in files:
    rel = os.path.relpath(path, REPO_ROOT).replace("\\", "/")
    for m in MACRO_RE.finditer(text):
      line_start = text.rfind("\n", 0, m.start()) + 1
      if re.match(r"\s*#\s*define", text[line_start:m.start()]):
        continue
      # A macro continuation line of a #define also counts as a definition.
      prev = text[:line_start].rstrip("\n")
      if prev.endswith("\\"):
        continue
      line = text.count("\n", 0, m.start()) + 1
      kind = m.group(1) or ""
      try:
        args, _ = scan_call(text, m.end())
      except GenError as e:
        raise GenError("%s:%d: %s" % (rel, line, e))
      if kind == "":
        layout = ("category", "type", "name", "value", "description")
      elif kind == "_ARGS":
        layout = ("category", "type", "name", "value", "description", "*")
      elif kind == "_ENV":
        layout = ("category", "type", "name", "value", "environment", "description")
      elif kind == "_FLAG":
        layout = ("category", "type", "name", "value", "flags", "description")
      elif kind == "_FLAG_ENV":
        layout = ("category", "type", "name", "value", "flags", "environment", "description")
      else:
        layout = ("category", "type", "name", "value", "environmentUnused", "flagsUnused", "description", "*")
      fixed = [k for k in layout if k != "*"]
      if len(args) < len(fixed) or ("*" not in layout and len(args) != len(fixed)):
        raise GenError("%s:%d: RTX_OPTION%s has %d arguments" % (rel, line, kind, len(args)))
      rec = dict(zip(fixed, args[:len(fixed)]))
      extra = ", ".join(args[len(fixed):])
      fields = parse_option_args(extra) if "*" in layout else {}
      if "environment" in rec:
        fields.setdefault("environment", rec["environment"])
      category = parse_string_concat(rec["category"])
      if category is None:
        raise GenError("%s:%d: category is not a string literal" % (rel, line))
      description = parse_string_concat(expand_stringify(rec["description"], sym))
      if description is None:
        raise GenError("%s:%d: description of %s is not a string literal: %s" % (rel, line, rec["name"], rec["description"]))
      options.append({
        "name": category + "." + rec["name"].strip(),
        "ctype": rec["type"],
        "value": rec["value"],
        "min": fields.get("minValue"),
        "max": fields.get("maxValue"),
        "environment": fields.get("environment"),
        "description": description,
        "file": rel,
        "line": line,
      })
  return options


def parse_numos_options():
  if not os.path.exists(NUMOS_FIELDS):
    raise GenError("missing %s; initialise the numos submodule" % NUMOS_FIELDS)
  text = strip_comments(read_text(NUMOS_FIELDS))
  rel = os.path.relpath(NUMOS_FIELDS, REPO_ROOT).replace("\\", "/")
  options = []
  for m in re.finditer(r"\bNUMOS_(MOON_)?SETTING\s*\(", text):
    line_start = text.rfind("\n", 0, m.start()) + 1
    if re.match(r"\s*#", text[line_start:m.start()]):
      continue
    line = text.count("\n", 0, m.start()) + 1
    args, _ = scan_call(text, m.end())
    if len(args) != 8:
      raise GenError("%s:%d: NUMOS setting with %d arguments" % (rel, line, len(args)))
    ctype, _, value, key, mn, mx, _, desc = args
    key = parse_string_concat(key)
    description = parse_string_concat(desc) or ""
    keys = [key] if not m.group(1) else ["rtx.atmosphere.moon%d.%s" % (i, key) for i in range(NUMOS_MAX_MOONS)]
    for k in keys:
      options.append({
        "name": k,
        "ctype": ctype,
        "value": value,
        "min": None if mn.strip() == "NUMOS_NO_MIN" or ctype.strip() == "bool" else mn,
        "max": None if mx.strip() == "NUMOS_NO_MAX" or ctype.strip() == "bool" else mx,
        "environment": None,
        "description": description,
        "file": rel,
        "line": line,
      })
  return options


def resolve(options, sym, warnings):
  rows = []
  errors = []
  for o in options:
    ev = Evaluator(sym)
    try:
      tstr = type_string(o["ctype"], sym)
      if tstr == "virtual keys":
        default = virtual_keys_string(o["value"], sym, ev)
      else:
        default = format_value(tstr, ev.eval(o["value"]), sym, o["value"])
      mn = format_value(tstr, ev.eval(o["min"]), sym, o["min"]) if o["min"] else ""
      mx = format_value(tstr, ev.eval(o["max"]), sym, o["max"]) if o["max"] else ""
      env = None
      if o["environment"]:
        env = parse_string_concat(o["environment"])
        if env is None:
          env = str(ev.eval(o["environment"]))
      if ev.ambiguous:
        warnings.append("%s:%d: %s: constant with several definitions: %s" % (
          o["file"], o["line"], o["name"], ", ".join(sorted(ev.ambiguous))))
      rows.append(dict(o, type=tstr, default=default, minText=mn, maxText=mx, environment=env))
    except GenError as e:
      errors.append("%s:%d: %s: %s" % (o["file"], o["line"], o["name"], e))
  return rows, errors


def render(rows):
  # The registry keeps the first registration of a name. RTX_OPTION declarations register before
  # the NUMOS table, which skips names an option already owns.
  by_name = {}
  for r in rows:
    by_name.setdefault(r["name"], r)
  ordered = sorted(by_name.values(), key=lambda r: r["name"].encode("utf-8"))
  out = [HEADER]
  for title, long_types in (("Simple Types", False), ("Complex Types", True)):
    out.append("## %s\n" % title)
    out.append(TABLE_HEADER)
    for r in ordered:
      if (r["type"] in LONG_TYPES) != long_types:
        continue
      desc = r["description"]
      if r["environment"]:
        desc = (desc + "\n" if desc else "") + "Environment variable: " + r["environment"]
      out.append("|%s|%s|%s|%s|%s|%s|\n" % (r["name"], r["type"], r["default"], r["minText"], r["maxText"],
                                          markdown_escape(desc)))
    if not long_types:
      out.append("\n")
  return "".join(out)


def load_extra_headers():
  """Headers outside src/ whose enums or constants option defaults use."""
  files = []
  for rel in EXTRA_HEADERS:
    path = os.path.join(REPO_ROOT, rel)
    if not os.path.exists(path):
      raise GenError("missing %s; initialise the submodules and run a build once to fetch the packman dependencies" % rel)
    files.append((path, strip_comments(read_text(path))))
  return files


def load_sources():
  files = []
  for root, _, names in os.walk(SRC_DIR):
    for name in sorted(names):
      if name.endswith((".h", ".cpp", ".hpp", ".inl")):
        path = os.path.join(root, name)
        files.append((path, strip_comments(read_text(path))))
  return files


def main():
  parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
  parser.add_argument("--output", default=os.path.join(REPO_ROOT, "RtxOptions.md"))
  parser.add_argument("--check", action="store_true", help="exit 1 when the output file is out of date")
  parser.add_argument("--json", help="also write the parsed options to this JSON file")
  parser.add_argument("--verbose", action="store_true", help="list defaults that use a constant defined more than once")
  args = parser.parse_args()

  files = load_sources()
  numos_text = strip_comments(read_text(NUMOS_FIELDS)) if os.path.exists(NUMOS_FIELDS) else ""
  sym = collect_symbols(files + load_extra_headers() + [(NUMOS_FIELDS, numos_text)])
  options = parse_rtx_options(files, sym) + parse_numos_options()
  warnings = []
  rows, errors = resolve(options, sym, warnings)
  if args.verbose:
    for w in warnings:
      print("warning: " + w, file=sys.stderr)
  if errors:
    for e in errors:
      print("error: " + e, file=sys.stderr)
    return 1

  text = render(rows)
  if args.json:
    with open(args.json, "w", encoding="utf-8") as f:
      json.dump(rows, f, indent=1)
  if args.check:
    current = read_text(args.output) if os.path.exists(args.output) else ""
    if current.replace("\r\n", "\n") != text:
      print("%s is out of date; run python scripts/gen_rtx_options_md.py" % args.output, file=sys.stderr)
      return 1
    return 0
  # Keep the existing file's line endings.
  newline = "\n"
  if os.path.exists(args.output):
    with open(args.output, "rb") as f:
      if b"\r\n" in f.read(4096):
        newline = "\r\n"
  with open(args.output, "w", encoding="utf-8", newline=newline) as f:
    f.write(text)
  print("wrote %s (%d options)" % (args.output, len({r["name"] for r in rows})))
  return 0


if __name__ == "__main__":
  sys.exit(main())
