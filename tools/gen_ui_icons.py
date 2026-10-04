#!/usr/bin/env python3
"""Builds engine_assets/assets/dfe/ui/icons.png (16x16 cells) and icons.json. Deterministic, stdlib only."""
import json, os, struct, zlib

OUT = os.path.join(os.path.dirname(__file__), "..", "engine_assets", "assets", "dfe", "ui")
PAL = {".": (0, 0, 0, 0), "k": (30, 18, 18, 255), "r": (210, 40, 40, 255), "R": (150, 20, 30, 255), "w": (255, 190, 190, 255),
       "g": (120, 120, 120, 255), "o": (200, 130, 50, 255), "O": (140, 80, 30, 255), "y": (240, 210, 90, 255),
       "s": (190, 200, 210, 255), "S": (110, 120, 135, 255), "b": (70, 110, 230, 255), "B": (40, 60, 150, 255),
       "c": (180, 220, 255, 255), "l": (130, 230, 60, 255), "L": (60, 150, 30, 255), "m": (255, 0, 255, 255)}

HEART = ["................", "..kkkk....kkkk..", ".krrrrk..krrrrk.", "krwwrrrkkrrrrRRk", "krwrrrrrrrrrrRRk", "krrrrrrrrrrrrRRk",
         "krrrrrrrrrrrrRRk", ".krrrrrrrrrrRRk.", "..krrrrrrrrRRk..", "...krrrrrrRRk...", "....krrrrRRk....", ".....krrRRk.....",
         "......krRk......", ".......kk.......", "................", "................"]
HEART_HALF = [row[:8] + "".join("g" if ch in "rwR" else ("k" if ch == "k" else ".") for ch in row[8:]) for row in HEART]
HUNGER = ["................", "......kk........", ".....kook.......", "....koyyok......", "...koyyyyok.....", "..koyyyyyyok....", "..koyyyyyyOk....",
          "..koyyyyyOOk....", "...koyyyyOOk....", "...kkoyyOOk.....", "..kooOkOOkk.....", ".kooOk.kk.......", ".kOOk...........", "..kk............",
          "................", "................"]
ARMOR = ["................", "..kkkkkkkkkkkk..", ".ksssssssssssSk.", ".kswssssssssSSk.", ".ksssssssssSSSk.", ".ksssssssssSSSk.", ".ksssssssssSSSk.",
         "..ksssssssSSSk..", "..kssssssSSSk...", "...ksssssSSSk...", "....kssssSSk....", ".....kssSSk.....", "......kSSk......", ".......kk.......",
         "................", "................"]
STAMINA = ["................", "........kkk.....", ".......kllk.....", "......kllLk.....", ".....kllLk......", "....kllLLk......", "...kllllllllk...",
           "...kkkkklLLk....", "......kllLk.....", ".....kllLk......", "....kllLk.......", "...kllLk........", "...kLLk.........", "....kk..........",
           "................", "................"]
MAGICKA = ["................", ".......kk.......", "......kbbk......", ".....kbcbbk.....", ".....kbcbbk.....", "....kbcbbbBk....", "....kbbbbbBk....",
           "...kbbbbbbBBk...", "...kbbbbbbBBk...", "...kbbbbbBBBk...", "....kbbbbBBk....", ".....kBBBBk.....", "......kkkk......", "................",
           "................", "................"]
XP_ORB = ["................", ".....kkkkkk.....", "...kkllllllkk...", "..kllwlllllllk..", "..klwllllllLLk..", ".kllllllllllLLk.", ".kllllllllllLLk.",
          ".kllllllllllLLk.", ".kllllllllllLLk.", ".kllllllllLLLLk.", "..kllllllLLLLk..", "..kkLLLLLLLLkk..", "...kkkLLLLkkk...", ".....kkkkkk.....",
          "................", "................"]
MISSING = ["".join("m" if (x // 8 + y // 8) % 2 == 0 else "k" for x in range(16)) for y in range(16)]

ICONS = [("heart", HEART), ("heart_half", HEART_HALF), ("hunger", HUNGER), ("armor", ARMOR), ("stamina", STAMINA),
         ("magicka", MAGICKA), ("xp_orb", XP_ORB), ("missing", MISSING)]

def png(width, height, rows):
    raw = b"".join(b"\x00" + bytes(v for px in row for v in px) for row in rows)
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))

def main():
    for name, art in ICONS:
        assert len(art) == 16 and all(len(r) == 16 for r in art), name
    rows = [[PAL[ch] for _, art in ICONS for ch in art[y]] for y in range(16)]
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, "icons.png"), "wb") as f:
        f.write(png(16 * len(ICONS), 16, rows))
    with open(os.path.join(OUT, "icons.json"), "w") as f:
        json.dump({"cell": 16, "icons": {n: [i, 0] for i, (n, _) in enumerate(ICONS)}}, f, indent=2)
        f.write("\n")

main()
