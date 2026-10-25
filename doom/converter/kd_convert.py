#!/usr/bin/env python3
"""
kd_convert.py - builds the KeyStep DOOM data blob from a DOOM IWAD.

Part of the KeyStep Pro DOOM port (GPLv2, derived from next-hack's MG21DOOM /
nRF52840 Doom, which derive from doomhack's GBADoom and PrBoom).

The blob is a tiny WAD-like container ("KDW1") that the engine reads in place
(memory mapped, const). It holds exactly one map, already in the runtime
formats the engine uses (line_t, seg_t, side_t, ...), the precomputed sector
line lists, one demo, and the graphics the map needs, converted to 4-bit
luminance at half resolution (the target is a 128x64 1-bit OLED).

Usage:
  python kd_convert.py --iwad doomu.wad --map E1M1 --demo e1m1.lmp --out kd_wad_e1m1.bin
"""
import argparse
import math
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.join(HERE, "..")

ML_THINGS, ML_LINEDEFS, ML_SIDEDEFS, ML_VERTEXES, ML_SEGS, ML_SSECTORS, ML_NODES, ML_SECTORS, ML_REJECT, ML_BLOCKMAP = range(1, 11)
NO_INDEX = 0xFFFF
ML_TWOSIDED = 4


# --------------------------------------------------------------------------
# WAD reading
# --------------------------------------------------------------------------
class Wad:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        ident, n, off = struct.unpack_from("<4sii", self.data, 0)
        self.lumps = []
        for i in range(n):
            pos, size, name = struct.unpack_from("<ii8s", self.data, off + 16 * i)
            self.lumps.append((name.split(b"\0")[0].decode("ascii").upper(), pos, size))
        self.names = [l[0] for l in self.lumps]

    def num(self, name, start=0):
        name = name.upper()
        for i in range(len(self.lumps) - 1, start - 1, -1):  # last one wins
            if self.lumps[i][0] == name:
                return i
        return -1

    def get(self, i):
        _, pos, size = self.lumps[i]
        return self.data[pos:pos + size]

    def getn(self, name):
        i = self.num(name)
        if i < 0:
            raise KeyError(name)
        return self.get(i)

    def range_between(self, start, end):
        a = self.names.index(start)
        b = self.names.index(end)
        return list(range(a + 1, b))


def name8(b):
    return b.split(b"\0")[0].decode("ascii", "replace").upper()


def s16(v):
    return struct.unpack("<h", struct.pack("<H", v & 0xFFFF))[0]


# --------------------------------------------------------------------------
# info.c / d_items.c parsing (to know which sprites the map can ever show)
# --------------------------------------------------------------------------
def parse_info():
    src = open(os.path.join(ENGINE, "source", "info.c"), encoding="latin-1").read()
    hdr = open(os.path.join(ENGINE, "include", "info.h"), encoding="latin-1").read()
    hdr = re.sub(r"/\*.*?\*/", "", hdr, flags=re.S)
    hdr = re.sub(r"//[^\n]*", "", hdr)
    hdr = re.sub(r"#ifdef\s+\w+.*?#endif", "", hdr, flags=re.S)

    def enum_body(tname):
        e = hdr.rindex("}", 0, hdr.index(tname) + 1)
        b = hdr.rindex("typedef enum", 0, e)
        return hdr[b:e]
    sprites = re.findall(r"\b(SPR_[A-Z0-9_]+)\b", enum_body("spritenum_t"))
    statenames = re.findall(r"\b(S_[A-Z0-9_]+)\b", enum_body("statenum_t"))
    sidx = {n: i for i, n in enumerate(statenames)}
    mtnames = re.findall(r"\b(MT_[A-Z0-9_]+)\b", enum_body("mobjtype_t"))
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    # states
    body = src[src.index("const state_t states[NUMSTATES]"):]
    body = body[:body.index("};")]
    states = []
    statenames_c = []
    for mm in re.finditer(r"\{\s*(SPR_[A-Z0-9_]+|0)\s*,\s*([0-9]+)\s*,\s*(-?[0-9]+)\s*,\s*([A-Za-z0-9_]+)\s*,\s*(S_[A-Z0-9_]+)[^\n]*//\s*(S_\w+)", body):
        states.append((mm.group(1), int(mm.group(2)), mm.group(5)))
        statenames_c.append(mm.group(6))
    statenames = statenames_c
    sidx = {n: i for i, n in enumerate(statenames)}
    assert len(states) == len(statenames), (len(states), len(statenames))
    # mobjinfo
    body = src[src.index("const mobjinfo_t mobjinfo[NUMMOBJTYPES]"):]
    blocks = re.findall(r"\{\s*//\s*(MT_[A-Z0-9_]+)(.*?)\n\}", body, re.S)
    mobjinfo = {}
    for name, blk in blocks:
        fields = {}
        for line in blk.split("\n"):
            mm = re.match(r"\s*(.*?),?\s*//\s*(\w+)", line)
            if mm:
                fields[mm.group(2)] = mm.group(1).strip().rstrip(",")
        mobjinfo[name] = fields
    return sprites, statenames, sidx, states, mtnames, mobjinfo


def parse_weapons():
    src = open(os.path.join(ENGINE, "source", "d_items.c"), encoding="latin-1").read()
    body = src[src.index("weaponinfo[NUMWEAPONS]"):]
    ws = re.findall(r"\{\s*//\s*([a-z0-9 ]+)\s*\n\s*(am_\w+),\s*(S_\w+),\s*(S_\w+),\s*(S_\w+),\s*(S_\w+),\s*(S_\w+)", body)
    return [(w[0].strip(), w[2:]) for w in ws]


# spawned in code, not reachable through mobjinfo state fields
CODE_SPAWNS = {
    "MT_POSSESSED": ["MT_CLIP"],
    "MT_SHOTGUY": ["MT_SHOTGUN"],
    "MT_CHAINGUY": ["MT_CHAINGUN"],
    "MT_TROOP": ["MT_TROOPSHOT"],
    "MT_HEAD": ["MT_HEADSHOT"],
    "MT_BRUISER": ["MT_BRUISERSHOT"],
    "MT_KNIGHT": ["MT_BRUISERSHOT"],
    "MT_SKULL": [],
    "MT_CYBORG": ["MT_ROCKET"],
    "MT_SPIDER": [],
}
ALWAYS_TYPES = ["MT_PLAYER", "MT_PUFF", "MT_BLOOD", "MT_TFOG", "MT_IFOG"]
# weapon pickups -> weapon index in weaponinfo, plus its missile
WEAPON_THINGS = {2001: (2, None), 2002: (3, None), 2003: (4, "MT_ROCKET"),
                 2004: (5, "MT_PLASMA"), 2006: (6, "MT_BFG"), 2005: (7, None)}


def needed_sprites(map_things, skill_bit):
    sprites, statenames, sidx, states, mtnames, mobjinfo = parse_info()
    weapons = parse_weapons()
    ednum = {}
    for mt, f in mobjinfo.items():
        try:
            ednum[int(f.get("doomednum", "-1"))] = mt
        except ValueError:
            pass
    types = set(ALWAYS_TYPES)
    weap = {0, 1}  # fist, pistol
    for t, opt in map_things:
        if t in (1, 2, 3, 4, 11):
            continue
        if not (opt & skill_bit):
            continue
        if opt & 16:  # not in single player
            continue
        if t in WEAPON_THINGS:
            w, missile = WEAPON_THINGS[t]
            weap.add(w)
            if missile:
                types.add(missile)
        if t in ednum:
            types.add(ednum[t])
    # code spawns (closure)
    changed = True
    while changed:
        changed = False
        for t in list(types):
            for s in CODE_SPAWNS.get(t, []):
                if s not in types:
                    types.add(s)
                    changed = True
    stfields = ["spawnstate", "seestate", "painstate", "meleestate", "missilestate", "deathstate", "xdeathstate", "raisestate"]
    todo = []
    for t in types:
        f = mobjinfo[t]
        for k in stfields:
            v = f.get(k, "S_NULL")
            if v.startswith("S_") and v != "S_NULL":
                todo.append(v)
    for w in weap:
        todo.extend(s for s in weapons[w][1] if s != "S_NULL")
    seen = set()
    while todo:
        s = todo.pop()
        if s in seen or s == "S_NULL":
            continue
        seen.add(s)
        todo.append(states[sidx[s]][2])
    spr = set(states[sidx[s]][0] for s in seen)
    return sprites, sorted(spr), sorted(types), sorted(weap)


# --------------------------------------------------------------------------
# luminance
# --------------------------------------------------------------------------
def palette_lum(wad):
    pal = wad.getn("PLAYPAL")[:768]
    out = []
    for i in range(256):
        r, g, b = pal[3 * i], pal[3 * i + 1], pal[3 * i + 2]
        out.append((0.299 * r + 0.587 * g + 0.114 * b) / 255.0)
    return out


def enc4(l):
    """linear luminance 0..1 -> 4-bit code (sqrt encoded: more steps in the darks)"""
    return max(0, min(15, int(round(15.0 * math.sqrt(max(0.0, l))))))


def dec4(c):
    return (c / 15.0) ** 2


# --------------------------------------------------------------------------
# patches
# --------------------------------------------------------------------------
def patch_to_grid(data):
    """returns w, h, leftoff, topoff, grid[x][y] = palette index or None"""
    w, h, lo, to = struct.unpack_from("<hhhh", data, 0)
    grid = [[None] * h for _ in range(w)]
    for x in range(w):
        ofs = struct.unpack_from("<I", data, 8 + 4 * x)[0] & 0xFFFFFF
        while True:
            top = data[ofs]
            if top == 0xFF:
                break
            ln = data[ofs + 1]
            for i in range(ln):
                y = top + i
                if 0 <= y < h:
                    grid[x][y] = data[ofs + 3 + i]
            ofs += ln + 4
    return w, h, lo, to, grid


def downsample(grid, w, h, lum, factor=2):
    """grid of palette indices / None -> grid of linear luminance / None, size ceil(w/f) x ceil(h/f)"""
    w2 = (w + factor - 1) // factor
    h2 = (h + factor - 1) // factor
    out = [[None] * h2 for _ in range(w2)]
    for x2 in range(w2):
        for y2 in range(h2):
            vals = []
            tot = 0
            for dx in range(factor):
                for dy in range(factor):
                    x, y = x2 * factor + dx, y2 * factor + dy
                    if x < w and y < h:
                        tot += 1
                        p = grid[x][y]
                        if p is not None:
                            vals.append(lum[p])
            if vals and 2 * len(vals) >= tot:
                out[x2][y2] = sum(vals) / len(vals)
    return w2, h2, out


def pack_posts(lgrid, w, h):
    """masked column format: per column posts of (top u8, len u8, nibbles), 0xFF ends.
    returns (columnofs list relative to start of column data, bytes)"""
    cols = []
    blob = bytearray()
    for x in range(w):
        cols.append(len(blob))
        y = 0
        col = lgrid[x]
        while y < h:
            if col[y] is None:
                y += 1
                continue
            y0 = y
            while y < h and col[y] is not None and y - y0 < 254:
                y += 1
            run = [enc4(v) for v in col[y0:y]]
            blob.append(y0)
            blob.append(len(run))
            for i in range(0, len(run), 2):
                lo = run[i]
                hi = run[i + 1] if i + 1 < len(run) else 0
                blob.append(lo | (hi << 4))
        blob.append(0xFF)
    return cols, bytes(blob)


def pack_solid(lgrid, w, h):
    """solid column format: column-major, (h+1)//2 bytes per column, low nibble = even row"""
    blob = bytearray()
    for x in range(w):
        col = lgrid[x]
        for y in range(0, h, 2):
            a = enc4(col[y] if col[y] is not None else 0.0)
            b = enc4(col[y + 1] if y + 1 < h and col[y + 1] is not None else 0.0) if y + 1 < h else 0
            blob.append(a | (b << 4))
    return bytes(blob)


# --------------------------------------------------------------------------
# textures
# --------------------------------------------------------------------------
def load_textures(wad):
    pn = wad.getn("PNAMES")
    npn = struct.unpack_from("<i", pn, 0)[0]
    pnames = [name8(pn[4 + 8 * i:12 + 8 * i]) for i in range(npn)]
    texs = []
    for tl in ("TEXTURE1", "TEXTURE2"):
        if wad.num(tl) < 0:
            continue
        d = wad.getn(tl)
        n = struct.unpack_from("<i", d, 0)[0]
        for i in range(n):
            ofs = struct.unpack_from("<i", d, 4 + 4 * i)[0]
            nm = name8(d[ofs:ofs + 8])
            masked, w, h, _, pc = struct.unpack_from("<ihhih", d, ofs + 8)
            patches = []
            for p in range(pc):
                ox, oy, pi, _, _ = struct.unpack_from("<hhhhh", d, ofs + 22 + 10 * p)
                patches.append((ox, oy, pnames[pi]))
            texs.append((nm, w, h, patches))
    return texs


def compose_texture(wad, tex):
    nm, w, h, patches = tex
    grid = [[None] * h for _ in range(w)]
    for ox, oy, pname in patches:
        pw, ph, _, _, pg = patch_to_grid(wad.getn(pname))
        for x in range(pw):
            tx = ox + x
            if tx < 0 or tx >= w:
                continue
            # vanilla R_DrawColumnInCache: posts clipped to texture height
            for y in range(ph):
                v = pg[x][y]
                ty = oy + y
                if v is not None and 0 <= ty < h:
                    grid[tx][ty] = v
    return grid


TEX_ANIMS = [("BLODGR1", "BLODGR4"), ("SLADRIP1", "SLADRIP3"), ("BLODRIP1", "BLODRIP4"), ("FIREWALA", "FIREWALL"),
             ("GSTFONT1", "GSTFONT3"), ("FIRELAV3", "FIRELAVA"), ("FIREMAG1", "FIREMAG3"), ("FIREBLU1", "FIREBLU2"),
             ("ROCKRED1", "ROCKRED3"), ("BFALL1", "BFALL4"), ("SFALL1", "SFALL4"), ("WFALL1", "WFALL4"), ("DBRAIN1", "DBRAIN4")]
FLAT_ANIMS = [("NUKAGE1", "NUKAGE3"), ("FWATER1", "FWATER4"), ("SWATER1", "SWATER4"), ("LAVA1", "LAVA4"), ("BLOOD1", "BLOOD3"),
              ("RROCK05", "RROCK08"), ("SLIME01", "SLIME04"), ("SLIME05", "SLIME08"), ("SLIME09", "SLIME12")]


# --------------------------------------------------------------------------
# helpers to build structs
# --------------------------------------------------------------------------
def fixed_div(a, b):
    # m_fixed.h FixedDiv (only sign matters for slopetype)
    if (abs(a) >> 14) >= abs(b):
        return -0x80000000 if (a ^ b) < 0 else 0x7FFFFFFF
    return int((a << 16) / b)


class Blob:
    def __init__(self):
        self.lumps = []  # (name, bytes)

    def add(self, name, data):
        assert len(name) <= 8
        self.lumps.append((name, bytes(data)))

    def build(self):
        hdr_size = 16
        dir_entries = len(self.lumps)
        out = bytearray(hdr_size)
        offs = []
        for name, data in self.lumps:
            while len(out) % 4:
                out.append(0)
            offs.append(len(out))
            out += data
        while len(out) % 4:
            out.append(0)
        dirofs = len(out)
        for (name, data), o in zip(self.lumps, offs):
            out += name.encode("ascii").ljust(8, b"\0")
            out += struct.pack("<ii", o, len(data))
        struct.pack_into("<4siii", out, 0, b"KDW1", dir_entries, dirofs, len(out))
        return bytes(out)


# --------------------------------------------------------------------------
# main conversion
# --------------------------------------------------------------------------
def convert(args):
    wad = Wad(args.iwad)
    lum = palette_lum(wad)
    blob = Blob()
    report = []

    mapnum = wad.num(args.map)
    if mapnum < 0:
        sys.exit("map %s not found" % args.map)
    L = lambda k: wad.get(mapnum + k)

    # --- textures used by the map
    texs = load_textures(wad)
    texnames = [t[0] for t in texs]
    tindex = {n: i for i, n in enumerate(texnames)}
    sides_raw = L(ML_SIDEDEFS)
    nsides = len(sides_raw) // 30
    used_tex = set()
    for i in range(nsides):
        for k in (4, 12, 20):
            n = name8(sides_raw[i * 30 + k:i * 30 + k + 8])
            if n in tindex:
                used_tex.add(n)
    # switch partners
    for n in list(used_tex):
        if n.startswith("SW1") and ("SW2" + n[3:]) in tindex:
            used_tex.add("SW2" + n[3:])
        if n.startswith("SW2") and ("SW1" + n[3:]) in tindex:
            used_tex.add("SW1" + n[3:])
    # anim ranges (all or nothing, in original order)
    for a, b in TEX_ANIMS:
        if a in tindex and b in tindex:
            rng = texnames[tindex[a]:tindex[b] + 1]
            if any(r in used_tex for r in rng):
                used_tex.update(rng)
    sky = {1: "SKY1", 2: "SKY2", 3: "SKY3", 4: "SKY4"}.get(int(args.map[1]) if args.map.startswith("E") else 1, "SKY1")
    used_tex.add(sky)
    kept = [t for t in texs if t[0] in used_tex]  # original order (keeps anim ranges contiguous)
    # index 0 = "no texture"; its height is original texture 0's height (vanilla textureheight[0] quirk)
    kept_names = ["-"] + [t[0] for t in kept]
    knum = {n: i for i, n in enumerate(kept_names)}

    texdir = bytearray()
    texdat = bytearray()
    texnamesb = bytearray()
    # entry: int16 w2, int16 h2, int16 origw, int16 origh, uint8 flags, uint8 widthmaskbits, uint16 pad, uint32 dataofs(relative to KDTEXDAT)
    texdir += struct.pack("<hhhhBBHI", 0, 0, texs[0][1], texs[0][2], 0, 0, 0, 0)
    texnamesb += b"-".ljust(8, b"\0")
    tex_bytes = 0
    for t in kept:
        nm, w, h, patches = t
        grid = compose_texture(wad, t)
        w2, h2, lg = downsample(grid, w, h, lum)
        masked = any(v is None for col in lg for v in col)
        flags = 1 if masked else 0
        while len(texdat) % 2:
            texdat.append(0)
        ofs = len(texdat)
        if masked:
            cols, data = pack_posts(lg, w2, h2)
            hdr = bytearray()
            base = 2 * w2
            for c in cols:
                hdr += struct.pack("<H", base + c)
            texdat += hdr + data
        else:
            texdat += pack_solid(lg, w2, h2)
        texdir += struct.pack("<hhhhBBHI", w2, h2, w, h, flags, 0, 0, ofs)
        texnamesb += nm.encode().ljust(8, b"\0")
    blob.add("KDTEXDIR", texdir)
    blob.add("KDTEXNAM", texnamesb)
    blob.add("KDTEXDAT", texdat)
    report.append("textures: %d kept, %d bytes" % (len(kept), len(texdat)))

    # --- flats
    flatnums = wad.range_between("F_START", "F_END")
    flatnames_all = [wad.names[i] for i in flatnums if wad.lumps[i][2] == 4096]
    flatlumps = {wad.names[i]: i for i in flatnums if wad.lumps[i][2] == 4096}
    secs_raw = L(ML_SECTORS)
    nsecs = len(secs_raw) // 26
    used_flat = set()
    for i in range(nsecs):
        used_flat.add(name8(secs_raw[i * 26 + 4:i * 26 + 12]))
        used_flat.add(name8(secs_raw[i * 26 + 12:i * 26 + 20]))
    for a, b in FLAT_ANIMS:
        if a in flatlumps and b in flatlumps:
            rng = flatnames_all[flatnames_all.index(a):flatnames_all.index(b) + 1]
            if any(r in used_flat for r in rng):
                used_flat.update(rng)
    used_flat.add("F_SKY1")
    kept_flats = [n for n in flatnames_all if n in used_flat]
    flatdat = bytearray()
    flatnamesb = bytearray()
    for n in kept_flats:
        d = wad.get(flatlumps[n])
        grid = [[d[y * 64 + x] for y in range(64)] for x in range(64)]
        w2, h2, lg = downsample(grid, 64, 64, lum)
        # row-major 32x32, 2 px per byte (low nibble = even x)
        for y in range(32):
            for x in range(0, 32, 2):
                flatdat.append(enc4(lg[x][y]) | (enc4(lg[x + 1][y]) << 4))
        flatnamesb += n.encode().ljust(8, b"\0")
    blob.add("KDFLTNAM", flatnamesb)
    blob.add("KDFLTDAT", flatdat)
    report.append("flats: %d kept, %d bytes" % (len(kept_flats), len(flatdat)))

    # --- level
    things = L(ML_THINGS)
    skill_bit = {1: 1, 2: 1, 3: 2, 4: 4, 5: 4}[args.skill]
    map_things = [(struct.unpack_from("<h", things, i + 6)[0], struct.unpack_from("<h", things, i + 8)[0]) for i in range(0, len(things), 10)]
    vraw = L(ML_VERTEXES)
    verts = [struct.unpack_from("<hh", vraw, i) for i in range(0, len(vraw), 4)]
    vertex_t = b"".join(struct.pack("<ii", x << 16, y << 16) for x, y in verts)
    lraw = L(ML_LINEDEFS)
    nlines = len(lraw) // 14
    ldefs = [struct.unpack_from("<HHHhhHH", lraw, i * 14) for i in range(nlines)]
    sides = []
    for i in range(nsides):
        xo, yo = struct.unpack_from("<hh", sides_raw, i * 30)
        tn = [name8(sides_raw[i * 30 + k:i * 30 + k + 8]) for k in (4, 12, 20)]
        sec = struct.unpack_from("<h", sides_raw, i * 30 + 28)[0]
        sides.append((xo, yo, knum.get(tn[0], 0), knum.get(tn[1], 0), knum.get(tn[2], 0), sec))
    side_t = b"".join(struct.pack("<hhhhhh", *s) for s in sides)
    line_t = bytearray()
    for i, (v1, v2, flags, special, tag, s0, s1) in enumerate(ldefs):
        x1, y1 = verts[v1][0] << 16, verts[v1][1] << 16
        x2, y2 = verts[v2][0] << 16, verts[v2][1] << 16
        dx, dy = x2 - x1, y2 - y1
        if dx == 0:
            st = 1  # ST_VERTICAL
        elif dy == 0:
            st = 0  # ST_HORIZONTAL
        else:
            st = 2 if fixed_div(dy, dx) > 0 else 3
        bbox = [0] * 4  # BOXTOP, BOXBOTTOM, BOXLEFT, BOXRIGHT
        bbox[0] = max(y1, y2)
        bbox[1] = min(y1, y2)
        bbox[2] = min(x1, x2)
        bbox[3] = max(x1, x2)
        line_t += struct.pack("<iiiiIiiHHiiiiHhhh", x1, y1, x2, y2, i, dx, dy, s0, s1, bbox[0], bbox[1], bbox[2], bbox[3], flags, special, tag, st)
    sraw = L(ML_SEGS)
    seg_t = bytearray()
    for i in range(0, len(sraw), 12):
        v1, v2, ang, ld, side, ofs = struct.unpack_from("<HHhHhh", sraw, i)
        l = ldefs[ld]
        sidenum = l[5 + side]
        front = sides[sidenum][5] if sidenum != NO_INDEX else NO_INDEX
        back = NO_INDEX
        if l[2] & ML_TWOSIDED:
            os_ = l[5 + (side ^ 1)]
            if os_ != NO_INDEX:
                back = sides[os_][5]
        seg_t += struct.pack("<iiiiiIHHHH", verts[v1][0] << 16, verts[v1][1] << 16, verts[v2][0] << 16, verts[v2][1] << 16,
                             ofs << 16, (ang << 16) & 0xFFFFFFFF, sidenum, ld, front, back)

    # sector line lists, same order as P_GroupLines would build them
    seclines = [[] for _ in range(nsecs)]
    for i, (v1, v2, flags, special, tag, s0, s1) in enumerate(ldefs):
        fs = sides[s0][5]
        seclines[fs].append(i)
        if s1 != NO_INDEX:
            bs = sides[s1][5]
            if bs != fs:
                seclines[bs].append(i)
    sl = bytearray()
    starts = []
    pos = 0
    for s in seclines:
        starts.append(pos)
        pos += len(s)
    for st in starts:
        sl += struct.pack("<H", st)
    for s in seclines:
        for li in s:
            sl += struct.pack("<H", li)

    blob.add(args.map, b"")
    blob.add("THINGS", things)
    blob.add("LINEDEFS", line_t)
    blob.add("SIDEDEFS", side_t)
    blob.add("VERTEXES", vertex_t)
    blob.add("SEGS", seg_t)
    blob.add("SSECTORS", L(ML_SSECTORS))
    blob.add("NODES", L(ML_NODES))
    blob.add("SECTORS", secs_raw)
    blob.add("REJECT", L(ML_REJECT))
    blob.add("BLOCKMAP", L(ML_BLOCKMAP))
    blob.add("KDSECLIN", sl)
    lvl = sum(len(x[1]) for x in blob.lumps[-12:])
    report.append("level %s: %d bytes (lines %d, sides %d, segs %d, sectors %d)" % (args.map, lvl, nlines, nsides, len(sraw) // 12, nsecs))

    # --- demo
    if args.demo and os.path.exists(args.demo):
        d = open(args.demo, "rb").read()
        blob.add("KDDEMO", d)
        report.append("demo: %s, %d bytes, version %d skill %d E%dM%d" % (os.path.basename(args.demo), len(d), d[0], d[1] + 1, d[2], d[3]))
    else:
        report.append("demo: none (player stands still)")

    # --- sprites
    sprnames, spr_needed, types, weap = needed_sprites(map_things, skill_bit)
    report.append("sprite types: %d, sprites: %s" % (len(types), " ".join(s[4:] for s in spr_needed)))
    sstart = wad.names.index("S_START")
    send = wad.names.index("S_END")
    spr_lumps = {}
    for i in range(sstart + 1, send):
        spr_lumps[wad.names[i]] = i
    patches = []        # list of (lumpname)
    patch_index = {}
    frames = []         # spriteframe records
    sprdef = bytearray()
    for sname in sprnames:
        pre = sname[4:]
        if sname not in spr_needed:
            sprdef += struct.pack("<HH", 0, 0)
            continue
        # vanilla R_InitSpriteDefs
        sprtemp = {}
        maxframe = -1
        for ln, li in spr_lumps.items():
            if not ln.startswith(pre) or len(ln) < 6:
                continue
            for (fc, rc, flip) in ((ln[4], ln[5], False),) + (((ln[6], ln[7], True),) if len(ln) >= 8 else ()):
                frame = ord(fc) - ord("A")
                rot = ord(rc) - ord("0")
                if ln not in patch_index:
                    patch_index[ln] = len(patches)
                    patches.append(ln)
                pidx = patch_index[ln]
                fr = sprtemp.setdefault(frame, {"rotate": -1, "lump": [-1] * 8, "flip": 0})
                maxframe = max(maxframe, frame)
                if rot == 0:
                    fr["rotate"] = 0
                    for r in range(8):
                        fr["lump"][r] = pidx
                        fr["flip"] = (fr["flip"] | (1 << r)) if flip else (fr["flip"] & ~(1 << r))
                else:
                    fr["rotate"] = 1
                    fr["lump"][rot - 1] = pidx
                    fr["flip"] = (fr["flip"] | (1 << (rot - 1))) if flip else (fr["flip"] & ~(1 << (rot - 1)))
        first = len(frames)
        for f in range(maxframe + 1):
            fr = sprtemp.get(f, {"rotate": 0, "lump": [0] * 8, "flip": 0})
            frames.append(fr)
        sprdef += struct.pack("<HH", first, maxframe + 1)
    frdata = bytearray()
    for fr in frames:
        frdata += struct.pack("<8hBB", *[max(0, x) for x in fr["lump"]], fr["flip"] & 0xFF, 1 if fr["rotate"] == 1 else 0)
    # sprite patches: header int16 w2,h2,leftoff2,topoff2 then uint16 columnofs[w2] (relative to patch start), posts
    sprdat = bytearray()
    sprofs = bytearray()
    for pn in patches:
        w, h, lo, to, grid = patch_to_grid(wad.get(spr_lumps[pn]))
        w2, h2, lg = downsample(grid, w, h, lum)
        cols, data = pack_posts(lg, w2, h2)
        while len(sprdat) % 2:
            sprdat.append(0)
        sprofs += struct.pack("<I", len(sprdat))
        hdr = struct.pack("<hhhh", w2, h2, lo, to)  # offsets kept in full-res units
        base = 8 + 2 * w2
        sprdat += hdr + b"".join(struct.pack("<H", base + c) for c in cols) + data
    blob.add("KDSPRDEF", sprdef)
    blob.add("KDSPRFRM", frdata)
    blob.add("KDSPROFS", sprofs)
    blob.add("KDSPRDAT", sprdat)
    report.append("sprites: %d patches, %d bytes" % (len(patches), len(sprdat) + len(sprofs) + len(frdata) + len(sprdef)))

    # --- light table: 34 colormaps x 16 codes -> output luminance 0..255
    cm = wad.getn("COLORMAP")
    light = bytearray()
    gain = args.gain
    gamma = args.gamma
    for m in range(34):
        for c in range(16):
            l = dec4(c)
            if m < 32:
                # vanilla colormaps fade linearly towards black: level m keeps (32-m)/32
                v = l * (32 - m) / 32.0
            elif m == 32:  # invulnerability: inverse grey
                v = 1.0 - l
            else:
                v = 0.0
            v = min(1.0, gain * v) ** gamma
            v = min(1.0, max(0.0, 0.5 + (v - 0.5) * args.contrast))
            light.append(max(0, min(255, int(v * 255 + 0.5))))
    blob.add("KDLIGHT", light)

    out = blob.build()
    open(args.out, "wb").write(out)
    report.append("blob: %s, %d bytes" % (args.out, len(out)))
    print("\n".join(report))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iwad", required=True)
    ap.add_argument("--map", default="E1M1")
    ap.add_argument("--demo", default=None)
    ap.add_argument("--skill", type=int, default=4, help="1..5, decides which things' sprites are kept")
    ap.add_argument("--gain", type=float, default=2.0)
    ap.add_argument("--gamma", type=float, default=0.7)
    ap.add_argument("--contrast", type=float, default=1.4)
    ap.add_argument("--out", required=True)
    convert(ap.parse_args())


if __name__ == "__main__":
    main()
