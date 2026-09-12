#!/usr/bin/env python3
"""
Keep win/LiFx.vcxproj (+ .filters) in sync with build_linux.sh.

The source list in build_linux.sh's build_lifx `srcs=( ... )` is the single
source of truth for what goes into the mod DLL. This script rewrites the
ClCompile/ClInclude ItemGroups of win/LiFx.vcxproj from it and regenerates
win/LiFx.vcxproj.filters with one filter per source folder.

Usage:
  python3 scripts/sync_vcxproj.py           # rewrite the project files
  python3 scripts/sync_vcxproj.py --check   # exit 1 if they have drifted

Output is deterministic (sorted items, uuid5 filter GUIDs), so re-running on
an in-sync tree is a no-op.
"""
import argparse
import os
import re
import sys
import uuid

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_SH = os.path.join(ROOT, "build_linux.sh")
VCXPROJ = os.path.join(ROOT, "win", "LiFx.vcxproj")
FILTERS = VCXPROJ + ".filters"

# Headers shown in the IDE: everything under these roots, plus cm_config.h.
HEADER_ROOTS = ["source/core", "source/server"]
EXTRA_HEADERS = ["source/cm_config.h"]
# Generated + gitignored (scripts/gen_baked_key.py); never list it.
GENERATED_HEADERS = {"source/core/crypto/lfxe_key_data.h"}

# Folder -> Solution Explorer filter name, where plain capitalisation is wrong.
FILTER_NAMES = {"api": "API", "ai": "AI", "netevent": "NetEvent"}

FILTER_NS = uuid.UUID("5f0c1f3e-9a61-4c3b-8d0e-6c1f4f1a2b7d")


def lifx_sources() -> list[str]:
    with open(BUILD_SH, encoding="utf-8") as f:
        text = f.read()
    m = re.search(r"build_lifx\(\)\s*\{.*?local srcs=\((.*?)\)", text, re.S)
    if not m:
        sys.exit("error: could not find build_lifx srcs=( ... ) in build_linux.sh")
    return sorted(line.strip() for line in m.group(1).splitlines() if line.strip())


def lifx_headers() -> list[str]:
    out = set(EXTRA_HEADERS)
    for root in HEADER_ROOTS:
        for dirpath, _, files in os.walk(os.path.join(ROOT, root)):
            for name in files:
                if name.endswith(".h"):
                    rel = os.path.relpath(os.path.join(dirpath, name), ROOT)
                    out.add(rel.replace(os.sep, "/"))
    return sorted(out - GENERATED_HEADERS)


def win_path(p: str) -> str:
    return "..\\" + p.replace("/", "\\")


def filter_of(p: str) -> str:
    """source/server/hooks/engine/x.cpp -> Server\\Hooks\\Engine"""
    parts = p.split("/")[1:-1]  # drop "source" and the filename
    return "\\".join(FILTER_NAMES.get(s, s.capitalize()) for s in parts)


def item_group(tag: str, paths: list[str]) -> str:
    lines = [f'    <{tag} Include="{win_path(p)}" />' for p in paths]
    return "  <ItemGroup>\n" + "\n".join(lines) + "\n  </ItemGroup>"


def render_vcxproj(current: str, srcs: list[str], hdrs: list[str]) -> str:
    groups = {
        "ClInclude": item_group("ClInclude", hdrs),
        "ClCompile": item_group("ClCompile", srcs),
    }
    for tag, new in groups.items():
        pat = re.compile(r"  <ItemGroup>\n(?:    <%s Include=\"[^\"]*\" />\n)+  </ItemGroup>" % tag)
        current, n = pat.subn(lambda _: new, current, count=1)
        if n != 1:
            sys.exit(f"error: could not find the {tag} ItemGroup in {VCXPROJ}")
    return current


def render_filters(srcs: list[str], hdrs: list[str]) -> str:
    filters = set()
    for p in srcs + hdrs:
        f = filter_of(p)
        while f:
            filters.add(f)
            f = f.rpartition("\\")[0]

    def items(tag: str, paths: list[str]) -> list[str]:
        out = []
        for p in paths:
            f = filter_of(p)
            if f:
                out += [f'    <{tag} Include="{win_path(p)}">',
                        f"      <Filter>{f}</Filter>",
                        f"    </{tag}>"]
            else:
                out.append(f'    <{tag} Include="{win_path(p)}" />')
        return out

    lines = ['<?xml version="1.0" encoding="utf-8"?>',
             '<Project ToolsVersion="4.0" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">',
             "  <ItemGroup>", *items("ClInclude", hdrs), "  </ItemGroup>",
             "  <ItemGroup>", *items("ClCompile", srcs), "  </ItemGroup>",
             "  <ItemGroup>"]
    for f in sorted(filters):
        lines += [f'    <Filter Include="{f}">',
                  f"      <UniqueIdentifier>{{{uuid.uuid5(FILTER_NS, f)}}}</UniqueIdentifier>",
                  "    </Filter>"]
    lines += ["  </ItemGroup>", "</Project>", ""]
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="report drift, don't write")
    args = ap.parse_args()

    srcs, hdrs = lifx_sources(), lifx_headers()
    missing = [p for p in srcs if not os.path.isfile(os.path.join(ROOT, p))]
    if missing:
        sys.exit("error: build_linux.sh lists missing files: " + ", ".join(missing))

    with open(VCXPROJ, encoding="utf-8", newline="") as f:
        vcx_old = f.read()
    try:
        with open(FILTERS, encoding="utf-8-sig", newline="") as f:
            flt_old = f.read()
    except FileNotFoundError:
        flt_old = ""

    vcx_new = render_vcxproj(vcx_old, srcs, hdrs)
    flt_new = render_filters(srcs, hdrs)
    drift = [p for p, old, new in ((VCXPROJ, vcx_old, vcx_new), (FILTERS, flt_old, flt_new))
             if old != new]

    if args.check:
        for p in drift:
            print(f"drift: {os.path.relpath(p, ROOT)} is out of sync with build_linux.sh", file=sys.stderr)
        if drift:
            print("       run: python3 scripts/sync_vcxproj.py", file=sys.stderr)
        return 1 if drift else 0

    # VS writes .filters with a BOM and .vcxproj without; keep it that way.
    for path, content, enc in ((VCXPROJ, vcx_new, "utf-8"), (FILTERS, flt_new, "utf-8-sig")):
        if path in drift:
            with open(path, "w", encoding=enc, newline="") as f:
                f.write(content)
            print(f"updated {os.path.relpath(path, ROOT)}")
    if not drift:
        print("already in sync")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
