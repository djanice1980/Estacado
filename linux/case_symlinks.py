#!/usr/bin/env python3
"""Add case-variant symlinks to the xwin sysroot for every #include / #pragma
comment(lib) spelling used by the given source trees that only matches an SDK
file case-insensitively (Windows sources are written for a case-insensitive
filesystem)."""
import os
import re
import sys

# Usage: case_symlinks.py <xwin sysroot> <source tree>...
sysroot = os.path.abspath(sys.argv[1])
inc_dirs = [os.path.join(sysroot, p) for p in (
    "crt/include", "sdk/include/ucrt", "sdk/include/um", "sdk/include/shared",
    "sdk/include/winrt", "sdk/include/cppwinrt")]
lib_dirs = [os.path.join(sysroot, p) for p in (
    "crt/lib/x86_64", "sdk/lib/um/x86_64", "sdk/lib/ucrt/x86_64")]

include_re = re.compile(rb'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.M)
lib_re = re.compile(rb'#\s*pragma\s+comment\s*\(\s*lib\s*,\s*"([^"]+)"', re.I)


def index(dirs):
    idx = {}
    for d in dirs:
        for root, _, files in os.walk(d):
            for f in files:
                rel = os.path.relpath(os.path.join(root, f), d)
                idx.setdefault(rel.lower(), []).append((d, rel))
    return idx


def fix(names, dirs, idx, kind):
    added = 0
    for name in sorted(names):
        name = name.replace("\\", "/")
        if any(os.path.exists(os.path.join(d, name)) for d in dirs):
            continue
        hits = idx.get(name.lower())
        if not hits:
            continue
        base, rel = hits[0]
        link = os.path.join(base, name)
        os.makedirs(os.path.dirname(link), exist_ok=True)
        target = os.path.relpath(os.path.join(base, rel), os.path.dirname(link))
        if not os.path.lexists(link):
            os.symlink(target, link)
            added += 1
            print(f"{kind}: {name} -> {rel}")
    return added


includes, libs = set(), set()
for tree in sys.argv[2:]:
    for root, _, files in os.walk(tree):
        for f in files:
            if not f.endswith((".h", ".hpp", ".c", ".cc", ".cpp", ".inl", ".inc", ".txt", ".cmake")):
                continue
            try:
                data = open(os.path.join(root, f), "rb").read()
            except OSError:
                continue
            includes.update(m.decode("latin-1") for m in include_re.findall(data))
            libs.update(m.decode("latin-1") for m in lib_re.findall(data))
            if f == "CMakeLists.txt" or f.endswith(".cmake"):
                for m in re.findall(rb"\b([A-Za-z0-9_]+)\.lib\b", data):
                    libs.add(m.decode() + ".lib")
libs = {l if l.lower().endswith(".lib") else l + ".lib" for l in libs}

n = fix(includes, inc_dirs, index(inc_dirs), "include")
n += fix(libs, lib_dirs, index(lib_dirs), "lib")
print(f"{n} symlinks added")
