#!/usr/bin/env python3
"""Write hash.txt for generation epoch directories.  Usage: epoch_hash.py <epoch_dir>...
Hash layout matches src/epoch.c: for the six files in byte order, name NUL, length (LE64), contents."""
import hashlib, struct, sys, pathlib

FILES = ["biomes.json", "caves.json", "features.json", "noise.json", "structures.json", "surface.json"]

def epoch_hash(d):
    h = hashlib.sha256()
    for n in FILES:
        b = (pathlib.Path(d) / n).read_bytes()
        h.update(n.encode() + b"\0" + struct.pack("<Q", len(b)) + b)
    return h.hexdigest()

if __name__ == "__main__":
    for d in sys.argv[1:]:
        hx = epoch_hash(d)
        (pathlib.Path(d) / "hash.txt").write_text(hx + "\n")
        print(d, hx)
