#!/usr/bin/env python3
import os, sys, glob

args = sys.argv[1:]
if "--version" in args:
    print("1.8.1")
    sys.exit(0)

pkgs = [a for a in args if not a.startswith("-")]
if not pkgs:
    sys.exit(0)

libdir = os.environ.get("PKG_CONFIG_LIBDIR", "")
path = os.environ.get("PKG_CONFIG_PATH", "")
search_dirs = [d for d in (libdir.split(":") + path.split(":")) if d] + [
    "/usr/lib/x86_64-linux-gnu/pkgconfig",
    "/usr/share/pkgconfig",
]

def find_pc(pkg):
    for d in search_dirs:
        p = os.path.join(d, f"{pkg}.pc")
        if os.path.isfile(p):
            return p
    return None

def parse_pc(path):
    vars_map = {}
    fields = {}
    with open(path, "r", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if "=" in line and (":" not in line or line.index("=") < line.index(":")):
                k, v = line.split("=", 1)
                for vk, vv in vars_map.items():
                    v = v.replace(f"${{{vk}}}", vv)
                vars_map[k.strip()] = v.strip()
            elif ":" in line:
                k, v = line.split(":", 1)
                for vk, vv in vars_map.items():
                    v = v.replace(f"${{{vk}}}", vv)
                fields[k.strip().lower()] = v.strip()
    return vars_map, fields

pc_files = []
for pkg in pkgs:
    p = find_pc(pkg)
    if not p:
        sys.exit(1)
    pc_files.append((pkg, p, *parse_pc(p)))

if "--exists" in args or "--atleast-version" in args or "--exact-version" in args or "--max-version" in args:
    sys.exit(0)

if "--modversion" in args:
    for _, _, _, fields in pc_files:
        print(fields.get("version", "1.0.0"))
    sys.exit(0)

if "--variable" in args:
    idx = args.index("--variable")
    vname = args[idx + 1] if idx + 1 < len(args) else ""
    for _, _, vmap, _ in pc_files:
        print(vmap.get(vname, ""))
    sys.exit(0)

for a in args:
    if a.startswith("--variable="):
        vname = a.split("=", 1)[1]
        for _, _, vmap, _ in pc_files:
            print(vmap.get(vname, ""))
        sys.exit(0)

out = []
if "--cflags" in args or "--cflags-only-I" in args:
    for _, _, _, fields in pc_files:
        if fields.get("cflags"):
            out.append(fields["cflags"])
if "--libs" in args or "--libs-only-l" in args or "--libs-only-L" in args:
    for _, _, _, fields in pc_files:
        if fields.get("libs"):
            out.append(fields["libs"])
print(" ".join(out))
