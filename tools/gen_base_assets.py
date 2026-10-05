"""Generates the base mod's block textures and block definitions procedurally.

Everything is derived from a fixed seed so the output is reproducible. Run from the repository root:
    python3 tools/gen_base_assets.py
"""
import json
import math
import os
import random

from PIL import Image

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "mods", "base")
TEX = os.path.join(ROOT, "assets", "base", "textures", "block")
BLK = os.path.join(ROOT, "data", "base", "blocks")
SIZE = 16



def water_level(x, y, f, size, frames):
    """Water brightness in 0..1. Integer wave counts per tile and per loop make it periodic in x, y and f."""
    tau = 2 * math.pi
    p = f / frames
    a = math.sin(tau * (2 * x / size + p)) * math.cos(tau * (3 * y / size + p))
    b = math.sin(tau * (3 * x / size - p)) * math.cos(tau * (x / size + 2 * y / size + 2 * p))
    return 0.5 + 0.2 * a + 0.07 * b

def clamp(v):
    return max(0, min(255, int(v)))


def tint(c, k):
    return tuple(clamp(x * k) for x in c[:3]) + ((c[3],) if len(c) > 3 else (255,))


def noisy(base, spread, seed, alpha=255):
    rng = random.Random(seed)
    img = Image.new("RGBA", (SIZE, SIZE))
    for y in range(SIZE):
        for x in range(SIZE):
            k = 1.0 + (rng.random() - 0.5) * spread
            img.putpixel((x, y), (clamp(base[0] * k), clamp(base[1] * k), clamp(base[2] * k), alpha))
    return img


def speckle(img, color, count, seed, size=1):
    rng = random.Random(seed)
    for _ in range(count):
        cx, cy = rng.randrange(SIZE), rng.randrange(SIZE)
        for dy in range(size):
            for dx in range(size):
                img.putpixel(((cx + dx) % SIZE, (cy + dy) % SIZE), color + (255,))
    return img


def save(img, name):
    os.makedirs(TEX, exist_ok=True)
    img.save(os.path.join(TEX, name + ".png"))


def strip(frames, name, fps):
    out = Image.new("RGBA", (SIZE, SIZE * len(frames)))
    for i, f in enumerate(frames):
        out.paste(f, (0, i * SIZE))
    save(out, name)
    with open(os.path.join(TEX, name + ".json"), "w") as fh:
        json.dump({"fps": fps}, fh)


def block(name, **kw):
    os.makedirs(BLK, exist_ok=True)
    with open(os.path.join(BLK, name + ".json"), "w") as fh:
        json.dump(kw, fh, indent=2)
        fh.write("\n")


def all_tex(name):
    return {"all": "base:block/" + name}


def make_textures():
    stone = noisy((125, 125, 128), 0.22, 1)
    speckle(stone, (100, 100, 104), 18, 2)
    save(stone, "stone")
    deep = noisy((70, 70, 78), 0.25, 3)
    speckle(deep, (50, 50, 58), 20, 4)
    save(deep, "deep_stone")
    save(noisy((121, 85, 58), 0.25, 5), "dirt")
    gtop = noisy((200, 200, 200), 0.18, 6)
    save(gtop, "grass_top")
    side = noisy((121, 85, 58), 0.25, 7)
    rng = random.Random(8)
    for x in range(SIZE):
        depth = 3 + rng.randrange(3)
        for y in range(depth):
            k = 1.0 + (rng.random() - 0.5) * 0.2
            side.putpixel((x, y), (clamp(95 * k), clamp(160 * k), clamp(60 * k), 255))
    save(side, "grass_side")
    sand = noisy((222, 207, 150), 0.12, 9)
    save(sand, "sand")
    save(speckle(noisy((214, 196, 140), 0.10, 10), (190, 172, 118), 10, 11), "sandstone_top")
    ss = noisy((214, 196, 140), 0.10, 12)
    for y in (4, 5, 11):
        for x in range(SIZE):
            ss.putpixel((x, y), (196, 178, 124, 255))
    save(ss, "sandstone_side")
    gr = noisy((135, 130, 128), 0.45, 13)
    speckle(gr, (95, 92, 92), 24, 14)
    speckle(gr, (170, 165, 160), 16, 15)
    save(gr, "gravel")
    cob = noisy((118, 118, 120), 0.18, 16)
    for (x0, y0, w, h) in [(0, 0, 8, 5), (8, 0, 8, 5), (0, 5, 5, 6), (5, 5, 6, 6), (11, 5, 5, 6), (0, 11, 8, 5), (8, 11, 8, 5)]:
        for x in range(x0, x0 + w):
            for y in range(y0, y0 + h):
                if x in (x0, x0 + w - 1) or y in (y0, y0 + h - 1):
                    cob.putpixel((x, y), (78, 78, 80, 255))
    save(cob, "cobblestone")
    save(speckle(noisy((244, 247, 250), 0.05, 17), (225, 232, 240), 12, 18), "snow")
    ice = noisy((150, 190, 235), 0.1, 19, alpha=190)
    save(ice, "ice")
    save(speckle(noisy((82, 64, 50), 0.2, 20), (60, 46, 36), 14, 21), "mud")
    coarse = noisy((112, 80, 56), 0.28, 30)
    speckle(coarse, (150, 140, 130), 22, 31)
    speckle(coarse, (84, 60, 42), 16, 32)
    save(coarse, "coarse_dirt")
    pod_top = speckle(noisy((96, 66, 38), 0.22, 33), (70, 48, 28), 20, 34)
    speckle(pod_top, (128, 92, 52), 10, 35)
    save(pod_top, "podzol_top")
    pod_side = noisy((121, 85, 58), 0.25, 36)
    rng = random.Random(37)
    for x in range(SIZE):
        for y in range(3 + rng.randrange(3)):
            k = 1.0 + (rng.random() - 0.5) * 0.25
            pod_side.putpixel((x, y), (clamp(96 * k), clamp(66 * k), clamp(38 * k), 255))
    save(pod_side, "podzol_side")
    save(speckle(noisy((196, 112, 58), 0.14, 38), (170, 90, 44), 14, 39), "red_sand")
    save(speckle(noisy((158, 164, 178), 0.08, 40), (140, 146, 160), 12, 41), "clay")
    save(speckle(noisy((138, 138, 136), 0.16, 42), (112, 112, 112), 18, 43), "andesite")
    gran = speckle(noisy((152, 108, 94), 0.18, 44), (120, 80, 70), 16, 45)
    save(speckle(gran, (186, 146, 130), 12, 46), "granite")
    moss = noisy((84, 122, 46), 0.22, 47)
    save(speckle(moss, (60, 96, 34), 18, 48), "moss_block")
    log_side = noisy((102, 80, 50), 0.25, 22)
    for x in range(0, SIZE, 4):
        for y in range(SIZE):
            log_side.putpixel((x, y), (74, 58, 36, 255))
    save(log_side, "log_side")
    top = noisy((160, 130, 82), 0.12, 23)
    for r in (3, 6):
        for a in range(0, 360, 6):
            px = int(7.5 + r * math.cos(math.radians(a)))
            py = int(7.5 + r * math.sin(math.radians(a)))
            top.putpixel((px, py), (120, 94, 58, 255))
    save(top, "log_top")
    leaves = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    rng = random.Random(24)
    for y in range(SIZE):
        for x in range(SIZE):
            if rng.random() < 0.78:
                k = 0.7 + rng.random() * 0.5
                leaves.putpixel((x, y), (clamp(140 * k), clamp(190 * k), clamp(130 * k), 255))
    save(leaves, "leaves")
    planks = noisy((176, 140, 88), 0.1, 25)
    for y in (0, 5, 10, 15):
        for x in range(SIZE):
            planks.putpixel((x, y), (130, 100, 60, 255))
    save(planks, "planks")
    glass = Image.new("RGBA", (SIZE, SIZE), (200, 230, 240, 70))
    for i in range(SIZE):
        for p in ((i, 0), (i, SIZE - 1), (0, i), (SIZE - 1, i)):
            glass.putpixel(p, (220, 240, 250, 210))
    glass.putpixel((3, 3), (255, 255, 255, 230))
    save(glass, "glass")

    water = []
    for f in range(4):
        img = Image.new("RGBA", (SIZE, SIZE))
        for y in range(SIZE):
            for x in range(SIZE):
                v = water_level(x, y, f, SIZE, 4)
                img.putpixel((x, y), (clamp(210 * v + 40), clamp(225 * v + 30), clamp(255 * v), 230))
        water.append(img)
    strip(water, "water", 3)
    lava = []
    for f in range(4):
        img = Image.new("RGBA", (SIZE, SIZE))
        rng = random.Random(30 + f)
        for y in range(SIZE):
            for x in range(SIZE):
                v = 0.6 + 0.4 * math.sin((x * 0.7 + y * 0.5) + f * 1.6) * rng.random()
                img.putpixel((x, y), (clamp(255 * v), clamp(110 * v), clamp(20 * v), 255))
        lava.append(img)
    strip(lava, "lava", 2)

    for name, col, seed in [("coal_ore", (30, 30, 34), 40), ("iron_ore", (205, 160, 130), 41), ("gold_ore", (245, 205, 70), 42), ("diamond_ore", (90, 235, 230), 43)]:
        ore = stone.copy()
        speckle(ore, col, 9, seed, size=2)
        save(ore, name)

    for name, col in [("crystal_red", (230, 40, 50)), ("crystal_blue", (60, 110, 255)), ("crystal_green", (60, 230, 110)), ("crystal_amber", (255, 190, 50))]:
        img = noisy(tint(col + (255,), 0.55), 0.3, sum(map(ord, name)))
        for (x, y) in [(3, 3), (4, 3), (9, 6), (10, 6), (5, 11), (12, 12), (7, 2), (13, 9)]:
            img.putpixel((x, y), col + (255,))
            img.putpixel((x + 1 if x < 15 else x, y), tint(col + (255,), 1.2))
        save(img, name)
    lamp = noisy((255, 214, 130), 0.14, 50)
    for i in range(SIZE):
        for p in ((i, 0), (i, SIZE - 1), (0, i), (SIZE - 1, i)):
            lamp.putpixel(p, (170, 120, 60, 255))
    save(lamp, "lantern")

    def plant(name, seed, fn):
        img = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
        fn(img, random.Random(seed))
        save(img, name)

    def grass_fn(img, rng):
        for x in range(1, SIZE, 2):
            h = 6 + rng.randrange(8)
            for y in range(SIZE - h, SIZE):
                k = 0.75 + 0.25 * (y - (SIZE - h)) / h + rng.random() * 0.1
                img.putpixel((x, y), (clamp(210 * k), clamp(230 * k), clamp(210 * k), 255))

    plant("tall_grass", 60, grass_fn)

    def flower(c1):
        def fn(img, rng):
            for y in range(7, SIZE):
                img.putpixel((8, y), (70, 150, 60, 255))
            img.putpixel((7, 11), (70, 150, 60, 255))
            img.putpixel((9, 9), (70, 150, 60, 255))
            for dx, dy in [(0, 0), (1, 0), (-1, 0), (0, 1), (0, -1)]:
                img.putpixel((8 + dx, 5 + dy), c1 + (255,))
            img.putpixel((8, 5), (250, 230, 120, 255))
        return fn

    plant("flower_red", 61, flower((220, 40, 50)))
    plant("flower_yellow", 62, flower((250, 215, 50)))

    def dead_fn(img, rng):
        for (x0, y0, x1, y1) in [(8, 15, 8, 8), (8, 11, 4, 7), (8, 10, 12, 6), (4, 7, 3, 4), (12, 6, 13, 3)]:
            n = max(abs(x1 - x0), abs(y1 - y0)) + 1
            for i in range(n):
                t = i / max(n - 1, 1)
                img.putpixel((int(x0 + (x1 - x0) * t), int(y0 + (y1 - y0) * t)), (120, 88, 52, 255))

    plant("dead_bush", 63, dead_fn)

    def mushroom_fn(img, rng):
        for y in range(9, SIZE):
            img.putpixel((8, y), (225, 215, 195, 255))
        for x in range(4, 13):
            for y in range(5, 9):
                if (x - 8) ** 2 / 20.0 + (y - 8) ** 2 / 9.0 < 1.0:
                    img.putpixel((x, y), (190, 50, 45, 255))
        img.putpixel((6, 6), (240, 240, 235, 255))
        img.putpixel((10, 7), (240, 240, 235, 255))

    plant("mushroom", 64, mushroom_fn)


def make_blocks():
    def cube(name, textures=None, **kw):
        block(name, textures=textures or all_tex(name), **kw)

    cube("stone", hardness=1.5, tool="pick", drops="base:cobblestone", sound="stone")
    cube("deep_stone", hardness=3.0, tool="pick", sound="stone")
    cube("dirt", hardness=0.5, tool="shovel", sound="dirt")
    block("grass_block", textures={"up": "base:block/grass_top", "down": "base:block/dirt", "side": "base:block/grass_side"},
          tint="grass", tint_faces=["up"], hardness=0.6, tool="shovel", drops="base:dirt", sound="grass", random_tick=True)
    cube("sand", hardness=0.5, tool="shovel", sound="sand")
    block("sandstone", textures={"up": "base:block/sandstone_top", "down": "base:block/sandstone_top", "side": "base:block/sandstone_side"},
          hardness=0.8, tool="pick", sound="stone")
    cube("gravel", hardness=0.6, tool="shovel", sound="gravel")
    cube("cobblestone", hardness=2.0, tool="pick", sound="stone")
    cube("snow", hardness=0.2, tool="shovel", sound="snow")
    cube("ice", layer="translucent", light={"opacity": 1}, hardness=0.5, sound="glass", friction=0.2)
    cube("mud", hardness=0.5, tool="shovel", sound="mud")
    cube("coarse_dirt", hardness=0.5, tool="shovel", sound="dirt")
    block("podzol", textures={"up": "base:block/podzol_top", "down": "base:block/dirt", "side": "base:block/podzol_side"},
          hardness=0.5, tool="shovel", drops="base:dirt", sound="dirt")
    cube("red_sand", hardness=0.5, tool="shovel", sound="sand")
    cube("clay", hardness=0.6, tool="shovel", sound="dirt")
    cube("andesite", hardness=1.5, tool="pick", sound="stone")
    cube("granite", hardness=1.5, tool="pick", sound="stone")
    cube("moss_block", hardness=0.3, tool="shovel", sound="grass")
    block("log", textures={"up": "base:block/log_top", "down": "base:block/log_top", "side": "base:block/log_side"}, hardness=2.0, tool="axe", sound="wood")
    block("leaves", textures=all_tex("leaves"), layer="cutout", tint="foliage", wind=True, light={"opacity": 1}, hardness=0.2, sound="grass")
    cube("planks", hardness=2.0, tool="axe", sound="wood")
    cube("glass", layer="translucent", light={"opacity": 0}, hardness=0.3, sound="glass")
    block("water", shape="fluid", layer="translucent", tint="water", textures=all_tex("water"), solid=False, hardness=-1,
          light={"opacity": 2}, fluid={"viscosity": 5, "group": "water"}, item=False, sound="water")
    block("lava", shape="fluid", layer="opaque", textures=all_tex("lava"), solid=False, hardness=-1,
          light={"opacity": 15, "emit": [15, 8, 2]}, fluid={"viscosity": 30, "group": "lava"}, item=False, sound="lava")
    for ore, tool in [("coal_ore", "pick"), ("iron_ore", "pick"), ("gold_ore", "pick"), ("diamond_ore", "pick")]:
        cube(ore, hardness=3.0, tool=tool, sound="stone")
    glow = {"crystal_red": [15, 3, 3], "crystal_blue": [3, 6, 15], "crystal_green": [3, 15, 6], "crystal_amber": [15, 11, 3]}
    for name, emit in glow.items():
        cube(name, light={"opacity": 15, "emit": emit}, hardness=1.0, tool="pick", sound="glass")
    cube("lantern", light={"opacity": 15, "emit": [15, 12, 7]}, hardness=0.5, sound="glass")
    block("tall_grass", shape="cross", layer="cutout", tint="grass", wind=True, textures=all_tex("tall_grass"), hardness=0.0, sound="grass")
    block("flower_red", shape="cross", layer="cutout", wind=True, textures=all_tex("flower_red"), hardness=0.0, sound="grass")
    block("flower_yellow", shape="cross", layer="cutout", wind=True, textures=all_tex("flower_yellow"), hardness=0.0, sound="grass")
    block("dead_bush", shape="cross", layer="cutout", wind=True, textures=all_tex("dead_bush"), hardness=0.0, sound="grass")
    block("mushroom", shape="cross", layer="cutout", textures=all_tex("mushroom"), light={"emit": [2, 1, 3]}, hardness=0.0, sound="grass")


def make_worldgen():
    path = os.path.join(ROOT, "data", "base", "worldgen")
    os.makedirs(path, exist_ok=True)
    cfg = {
        "sea_level": 62,
        "deep_level": 0,
        "blocks": {
            "stone": "base:stone", "deep_stone": "base:deep_stone", "dirt": "base:dirt", "grass": "base:grass_block",
            "sand": "base:sand", "sandstone": "base:sandstone", "gravel": "base:gravel", "snow": "base:snow",
            "mud": "base:mud", "water": "base:water",
            "coarse_dirt": "base:coarse_dirt", "podzol": "base:podzol", "red_sand": "base:red_sand", "clay": "base:clay",
            "andesite": "base:andesite", "granite": "base:granite", "moss": "base:moss_block",
        },
        "far_colors": {
            "ocean": [40, 80, 150], "beach": [222, 207, 150], "desert": [222, 200, 140], "tundra": [235, 240, 245],
            "swamp": [80, 100, 60], "forest": [70, 130, 55], "plains": [110, 165, 70], "mountain": [130, 130, 134],
        },
    }
    with open(os.path.join(path, "default.json"), "w") as fh:
        json.dump(cfg, fh, indent=2)
        fh.write("\n")


# PBR companion maps. Both are pure functions of the host PNG: no randomness at all, so regenerating them from the
# same colour texture always gives byte-identical files. The engine loads <name>_n.png and <name>_r.png beside
# <name>.png; see "PBR maps" in docs/MODDING.md for the conventions.
PBR_BLOCKS = {
    # name: (roughness, normal strength, height gain). Roughness follows the real surface: 0 mirror, 1 matte.
    # Normal strength scales the luminance gradient (rougher, chunkier material = stronger); height gain scales how
    # far the relief swings around the neutral 0.5 and so how deep the contact shadow and parallax read.
    # rock
    "stone": (0.85, 2.0, 1.5), "deep_stone": (0.9, 2.2, 1.6), "cobblestone": (0.92, 3.0, 2.4), "limestone": (0.8, 1.6, 1.2),
    "andesite": (0.82, 1.6, 1.2), "granite": (0.62, 1.4, 1.0), "tuff": (0.9, 2.2, 1.6),
    "mossy_cobble_cold": (0.93, 3.0, 2.4), "mossy_cobble_warm": (0.93, 3.0, 2.4),
    # ores: rock matrix, metallic flecks are a touch glossier
    "coal_ore": (0.85, 2.0, 1.5), "iron_ore": (0.75, 2.0, 1.5), "gold_ore": (0.6, 2.0, 1.5), "diamond_ore": (0.5, 2.0, 1.5),
    # gems
    "crystal_amber": (0.2, 1.0, 0.8), "crystal_blue": (0.18, 1.0, 0.8), "crystal_green": (0.2, 1.0, 0.8), "crystal_red": (0.2, 1.0, 0.8),
    # soil and loose ground
    "dirt": (0.95, 1.8, 1.4), "coarse_dirt": (0.97, 2.8, 2.2), "gravel": (0.95, 3.2, 2.6), "mud": (0.45, 1.2, 1.0),
    "peat": (0.95, 1.6, 1.2), "podzol_top": (0.95, 1.8, 1.4), "podzol_side": (0.95, 1.8, 1.4), "permafrost": (0.8, 1.6, 1.2),
    "clay": (0.7, 1.0, 0.8), "moss_block": (0.98, 2.4, 1.8),
    # sand and sandstone
    "sand": (0.9, 1.6, 1.2), "red_sand": (0.9, 1.6, 1.2), "sandstone_side": (0.88, 1.8, 1.4), "sandstone_top": (0.88, 1.4, 1.1),
    "red_sandstone": (0.88, 1.8, 1.4),
    # fired clay
    "terracotta_orange": (0.65, 1.2, 0.9), "terracotta_red": (0.65, 1.2, 0.9), "terracotta_white": (0.6, 1.2, 0.9),
    # wood: bark is rough and grooved, cut ends and planks are smoother
    "log_side": (0.9, 3.0, 2.4), "birch_log_side": (0.85, 2.4, 1.8), "spruce_log_side": (0.92, 3.0, 2.4), "jungle_log_side": (0.92, 3.0, 2.4),
    "acacia_log_side": (0.9, 2.6, 2.0), "darkoak_log_side": (0.92, 3.0, 2.4), "cherry_log_side": (0.85, 2.4, 1.8), "mangrove_log_side": (0.92, 3.0, 2.4),
    "log_top": (0.75, 2.0, 1.4), "birch_log_top": (0.75, 2.0, 1.4), "spruce_log_top": (0.75, 2.0, 1.4), "jungle_log_top": (0.75, 2.0, 1.4),
    "acacia_log_top": (0.75, 2.0, 1.4), "darkoak_log_top": (0.75, 2.0, 1.4), "cherry_log_top": (0.75, 2.0, 1.4), "mangrove_log_top": (0.75, 2.0, 1.4),
    "planks": (0.65, 1.6, 1.0),
    # turf and snow
    "grass_top": (0.92, 1.8, 1.4), "grass_side": (0.92, 1.8, 1.4), "dry_grass_top": (0.92, 1.8, 1.4), "dry_grass_side": (0.92, 1.8, 1.4),
    "snow": (0.7, 1.0, 0.8),
}


def _luminance_blurred(img):
    """Luminance of every texel, blurred with a 3x3 tent filter. Edges wrap so tiles stay seamless."""
    img = img.convert("RGBA")
    w, h = img.size
    px = img.load()
    lum = [[(0.299 * px[x, y][0] + 0.587 * px[x, y][1] + 0.114 * px[x, y][2]) / 255.0 for x in range(w)] for y in range(h)]
    k = ((1, 2, 1), (2, 4, 2), (1, 2, 1))
    out = [[0.0] * w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            acc = 0.0
            for j in range(3):
                for i in range(3):
                    acc += k[j][i] * lum[(y + j - 1) % h][(x + i - 1) % w]
            out[y][x] = acc / 16.0
    return out


def make_normal(host_png, strength=2.0):
    """Tangent-space normal map from a colour texture: Sobel gradient of blurred luminance, OpenGL convention
    (red = +u to the right, green = up in the image, blue = out of the surface)."""
    lum = _luminance_blurred(Image.open(host_png))
    h, w = len(lum), len(lum[0])
    out = Image.new("RGB", (w, h))
    o = out.load()
    for y in range(h):
        for x in range(w):
            def L(dx, dy):
                return lum[(y + dy) % h][(x + dx) % w]
            gx = (L(1, -1) + 2 * L(1, 0) + L(1, 1)) - (L(-1, -1) + 2 * L(-1, 0) + L(-1, 1))
            gy = (L(-1, 1) + 2 * L(0, 1) + L(1, 1)) - (L(-1, -1) + 2 * L(0, -1) + L(1, -1))  # downwards in the image
            nx, ny, nz = -gx * strength, gy * strength, 1.0
            inv = 1.0 / math.sqrt(nx * nx + ny * ny + nz * nz)
            o[x, y] = tuple(int(round((c * inv * 0.5 + 0.5) * 255)) for c in (nx, ny, nz))
    return out


def make_roughness_height(host_png, roughness, height, gain=1.0):
    """Red: roughness, a little rougher in the crevices. Green: height, blurred luminance re-centred on the
    given bias so a block's mean depth stays where the caller put it."""
    lum = _luminance_blurred(Image.open(host_png))
    h, w = len(lum), len(lum[0])
    mean = sum(sum(r) for r in lum) / (w * h)
    out = Image.new("RGB", (w, h))
    o = out.load()
    for y in range(h):
        for x in range(w):
            d = lum[y][x] - mean
            o[x, y] = (clamp(int(round((roughness - d * 0.3) * 255))), clamp(int(round((height + d * 1.5 * gain) * 255))), 0)
    return out


def make_pbr_maps():
    for name, (rough, nstrength, gain) in PBR_BLOCKS.items():
        host = os.path.join(TEX, name + ".png")
        if not os.path.exists(host):
            continue
        make_normal(host, nstrength).save(os.path.join(TEX, name + "_n.png"))
        make_roughness_height(host, rough, 0.5, gain).save(os.path.join(TEX, name + "_r.png"))


def stylize_blocks():
    """Paints the PBR_BLOCKS tiles (tools/stylize_textures.py). Run once on freshly generated textures."""
    import stylize_textures
    for name in PBR_BLOCKS:
        host = os.path.join(TEX, name + ".png")
        if os.path.exists(host):
            stylize_textures.stylize_file(host, host, stylize_textures.guess_class(name))


if __name__ == "__main__":
    import sys
    if "--pbr-only" in sys.argv:
        # Rebuilds only the PBR companions from the colour PNGs already on disk.
        make_pbr_maps()
        print("pbr maps written")
        sys.exit(0)
    make_textures()
    make_pbr_maps()
    make_blocks()
    make_worldgen()
    print("base assets written")
