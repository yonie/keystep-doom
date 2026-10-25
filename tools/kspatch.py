"""Patch records for the KeyStep Pro: our bytes only, applied to the user's own stock image.

A patch file (patches/*.json) is
  {"target": "ui" | "main", "image": <name inside the .kspf>, "image_sha256": <stock payload hash>,
   "edits": [{"addr": "0x...", "new": "<hex>", "old_sha256": "<hash of the stock bytes replaced>"}]}
No stock firmware byte is stored: the hashes only let apply() refuse a different firmware version.
"""
import hashlib
import json
import struct
import zipfile

KSPF = (r'C:\ProgramData\Arturia\MIDI Control Center\Firmware'
        r'\keystep-pro_Firmware_Update_2.5.20.0.kspf')
IMAGES = {'main': 'keysteppro_main__fw2_5_20_2085__2024_03_04.bin',
          'ui': 'keysteppro_ui__fw0_0_1_1823__2024_03_04.bin'}


def sha(b):
    return hashlib.sha256(bytes(b)).hexdigest()


def stock(target, kspf=KSPF):
    """(base, payload) of the user's own stock image."""
    b = zipfile.ZipFile(kspf).read(IMAGES[target])
    size = struct.unpack_from('<I', b, 5)[0]
    base = struct.unpack_from('<I', b, 11)[0]
    return base, b[64:64 + size]


def bl(src, dst):
    """Thumb-2 BL at `src` to `dst` (both byte addresses), as 4 little-endian bytes."""
    off = (dst & ~1) - (src + 4)
    assert -(1 << 24) <= off < (1 << 24) and off % 2 == 0, hex(off)
    s = (off >> 24) & 1
    i1, i2 = (off >> 23) & 1, (off >> 22) & 1
    j1, j2 = (1 - i1) ^ s, (1 - i2) ^ s
    hi = 0xF000 | (s << 10) | ((off >> 12) & 0x3FF)
    lo = 0xD000 | (j1 << 13) | (j2 << 11) | ((off >> 1) & 0x7FF)
    return struct.pack('<HH', hi, lo)


def load(path):
    with open(path) as f:
        return json.load(f)


def check(patch, base, payload):
    if sha(payload) != patch['image_sha256']:
        raise ValueError('stock image does not match the one this patch was made for')
    for e in patch['edits']:
        a, new = int(e['addr'], 16), bytes.fromhex(e['new'])
        off = a - base
        if 'old_sha256' in e:
            if not (0 <= off and off + len(new) <= len(payload)):
                raise ValueError('edit %s is inside the image but out of range' % e['addr'])
            if sha(payload[off:off + len(new)]) != e['old_sha256']:
                raise ValueError('stock bytes at %s differ' % e['addr'])


def apply_image(patch, base, payload, limit):
    """Return a new payload with the patch applied (extended with 0xFF up to the highest edit)."""
    check(patch, base, payload)
    out = bytearray(payload)
    for e in patch['edits']:
        a, new = int(e['addr'], 16), bytes.fromhex(e['new'])
        off = a - base
        assert a + len(new) - 1 <= limit, 'edit past the image limit'
        if off + len(new) > len(out):
            out.extend(b'\xff' * (off + len(new) - len(out)))
        out[off:off + len(new)] = new
    return bytes(out)
