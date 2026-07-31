#!/usr/bin/env python3
"""Approximates the Arduino IDE's .ino -> .cpp step.

The behaviour that matters here: the IDE scans the sketch for top-level
function definitions, generates a prototype for each, and injects the
whole block immediately before the FIRST function definition in the file.
Any type named in a signature must therefore already be declared at that
point, or the generated prototypes fail to compile.
"""
import re
import sys

src = open(sys.argv[1]).read()
lines = src.split("\n")

# Top-level function definition: starts at column 0, has a parameter list,
# and opens a brace. Deliberately ignores struct/class/enum and statements.
start_re = re.compile(
    r"^(static\s+|inline\s+)*[A-Za-z_][\w:*&<>\s]*[\s*&]+\**(\w+)\s*\("
)
skip_kw = ("struct", "class", "enum", "return", "if", "for", "while",
           "switch", "typedef", "namespace", "extern", "#")

protos = []
first_def_line = None
i = 0
while i < len(lines):
    line = lines[i]
    m = start_re.match(line)
    if m and not line.lstrip().startswith(skip_kw) and \
       not any(line.startswith(k) for k in skip_kw):
        # accumulate until the parameter list closes
        chunk = line
        j = i
        while chunk.count("(") > chunk.count(")") and j + 1 < len(lines):
            j += 1
            chunk += " " + lines[j].strip()
        # a definition ends in '{'; a declaration/statement does not
        tail = chunk[chunk.rfind(")") + 1:].strip()
        if tail.startswith("{") or tail == "{":
            sig = chunk[:chunk.rfind(")") + 1].strip()
            sig = re.sub(r"\s+", " ", sig)
            protos.append(sig + ";")
            if first_def_line is None:
                first_def_line = i
        i = j + 1
        continue
    i += 1

if first_def_line is None:
    sys.exit("no function definitions found")

block = ["", "// ==== Arduino IDE generated function prototypes ===="] \
        + protos + ["// ==== end generated prototypes ====", ""]
out = lines[:first_def_line] + block + lines[first_def_line:]
open(sys.argv[2], "w").write("\n".join(out))
print(f"injected {len(protos)} prototypes at line {first_def_line + 1}")
