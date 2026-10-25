"""Build a MIDI Control Center update file that turns a KeyStep Pro into a DOOM box.

  python tools/make_kspf.py [--out build/keystep-pro_DOOM.kspf]

Made from YOUR stock .kspf (never redistributed) and our build outputs:
  image 0 (main)  build/keystep_doom.bin             the DOOM-only application at 0x08008000
  image 1 (UI)    stock UI firmware + patches/ui.json the raw-pixel frame mode for the OLED
The KeyStep Pro Chroma images are left out on purpose (its main board is not what we traced), so MIDI
Control Center has nothing to put on a Chroma.

Image header (64 bytes, as in the stock file): VID/PID/index, u32 size at +5, byte +9 = payload checksum
(the payload's bytes plus it sum to 0 mod 256), byte +10 = 0x01, u32 base at +11, u32 limit at +15; the
last byte makes the 64 header bytes sum to 0 mod 256.  Both rules hold on all four stock images and are
asserted below.  The bootloader refuses an image whose payload checksum is wrong: it stays in update mode
(our first two hardware flashes copied stock's +9 byte unchanged and never started).
Getting back to stock: power on with a cable from Clock In to Reset Out (update mode), then update with
the stock .kspf in MIDI Control Center.
"""
import io
import json
import os
import struct
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kemu
import kspatch as P


def arg(name, default):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default


def header_from(stock_hdr, payload):
    h = bytearray(stock_hdr)
    size = len(payload)
    struct.pack_into('<I', h, 5, size)
    h[9] = (-sum(payload)) & 0xFF
    base, limit = struct.unpack_from('<II', h, 11)
    assert base + size - 1 <= limit, 'image past the header limit'
    h[63] = (-sum(h[:63])) & 0xFF
    return bytes(h)


def main():
    z = zipfile.ZipFile(P.KSPF)
    for n in z.namelist():                         # the stock headers must satisfy the rules we rely on
        if n.endswith('.bin'):
            b = z.read(n)
            assert sum(b[:64]) & 0xFF == 0, n
            assert (b[9] + sum(b[64:])) & 0xFF == 0, n
    info = json.loads(z.read('info.json'))
    main_name, ui_name = P.IMAGES['main'], P.IMAGES['ui']
    info['images'] = [im for im in info['images'] if im['file_name'] in (main_name, ui_name)]
    assert len(info['images']) == 2

    doom = open(os.path.join(kemu.REPO, 'build', 'keystep_doom.bin'), 'rb').read()
    main_hdr = header_from(z.read(main_name)[:64], doom)
    ui_base, ui_stock = P.stock('ui')
    ui_hdr_stock = z.read(ui_name)[:64]
    ui_limit = struct.unpack_from('<I', ui_hdr_stock, 15)[0]
    ui_img = P.apply_image(P.load(os.path.join(kemu.REPO, 'patches', 'ui.json')), ui_base, ui_stock, ui_limit)
    ui_hdr = header_from(ui_hdr_stock, ui_img)

    out = arg('--out', os.path.join(kemu.REPO, 'build', 'keystep-pro_DOOM.kspf'))
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as o:
        o.writestr('info.json', json.dumps(info, indent=4).replace('\n', '\r\n'))
        o.writestr(main_name, main_hdr + doom)
        o.writestr(ui_name, ui_hdr + ui_img)
    # read back and verify
    r = zipfile.ZipFile(out)
    for n, body in ((main_name, doom), (ui_name, ui_img)):
        b = r.read(n)
        assert sum(b[:64]) & 0xFF == 0 and b[64:] == body and struct.unpack_from('<I', b, 5)[0] == len(body)
        assert (b[9] + sum(body)) & 0xFF == 0
    print('%s: main %d B (DOOM), UI %d B (stock + %d-edit patch), headers check out' % (
        os.path.relpath(out, kemu.REPO), len(doom), len(ui_img), len(P.load(os.path.join(kemu.REPO, 'patches', 'ui.json'))['edits'])))


if __name__ == '__main__':
    main()
