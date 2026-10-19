#!/usr/bin/env python3
"""Pack a bank (or a Sound) and every YAML next to it into one self-contained document (a `library:` section),
for places without files — the browser worklet gets this one string.

  python3 tools/fa_pack.py presets/looks.yaml > looks.pack.yaml

Includes every *.yaml in the bank's directory and its macros/ subdirectory (not only the ones referenced — they
are small, and a look added later then needs no repack logic). Built-in macros (macros/ at the repo root) are
compiled into the library and need no packing.
"""
import glob
import os
import sys


def main():
    root = sys.argv[1]
    d = os.path.dirname(os.path.abspath(root))
    text = open(root, encoding="utf-8").read().rstrip("\n") + "\n"
    if "\nlibrary:" in text:
        sys.exit("already packed")
    out = [text, "library:\n"]
    files = sorted(glob.glob(os.path.join(d, "*.yaml")) + glob.glob(os.path.join(d, "macros", "*.yaml")))
    for f in files:
        if os.path.abspath(f) == os.path.abspath(root):
            continue
        name = os.path.splitext(os.path.basename(f))[0]
        body = open(f, encoding="utf-8").read().rstrip("\n").split("\n")
        out.append(f"  {name}:\n")
        out.extend(("    " + line).rstrip() + "\n" for line in body)
    sys.stdout.write("".join(out))


if __name__ == "__main__":
    main()
