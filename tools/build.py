"""Build the patches: assemble/compile our code, then write patches/<target>.json.

  python tools/build.py ui

Needs arm-none-eabi-gcc (xPack 14.2.1 is what this was made with); set ARM_GCC_BIN or put it on PATH.
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kemu
import kspatch as P

GCC_BIN = os.environ.get('ARM_GCC_BIN', os.path.expanduser('~/tools/xpack-arm-none-eabi-gcc-14.2.1-1.1/bin'))
# folder holding arm-none-eabi-gcc; tool() falls back to PATH when it is not there
BUILD = os.path.join(kemu.REPO, 'build')


def tool(name):
    p = os.path.join(GCC_BIN, 'arm-none-eabi-' + name + ('.exe' if os.name == 'nt' else ''))
    return p if os.path.exists(p) else 'arm-none-eabi-' + name


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit('FAILED: %s\n%s%s' % (' '.join(cmd), r.stdout, r.stderr))
    return r.stdout


def symbols(elf):
    out = {}
    for line in run([tool('nm'), elf]).splitlines():
        parts = line.split()
        if len(parts) == 3:
            out[parts[2]] = int(parts[0], 16)
    return out


def blob(elf, binp):
    run([tool('objcopy'), '-O', 'binary', elf, binp])
    with open(binp, 'rb') as f:
        return f.read()


def write_patch(target, code_addr, code, hooks):
    base, payload = P.stock(target)
    edits = [{'addr': hex(code_addr), 'new': code.hex(), 'what': 'our code'}]
    for site, dst, what in hooks:
        off = site - base
        edits.append({'addr': hex(site), 'new': P.bl(site, dst).hex(),
                      'old_sha256': P.sha(payload[off:off + 4]), 'what': what})
    patch = {'target': target, 'image': P.IMAGES[target], 'image_sha256': P.sha(payload),
             'license': 'MIT', 'edits': edits}
    path = os.path.join(kemu.REPO, 'patches', target + '.json')
    import json
    with open(path, 'w') as f:
        json.dump(patch, f, indent=1)
    print('%s: %d bytes of code at %s, %d hooks -> %s' % (target, len(code), hex(code_addr),
                                                          len(hooks), os.path.relpath(path, kemu.REPO)))
    return patch


def build_ui():
    os.makedirs(BUILD, exist_ok=True)
    src = os.path.join(kemu.REPO, 'ui', 'ui_patch.S')
    obj, elf = os.path.join(BUILD, 'ui_patch.o'), os.path.join(BUILD, 'ui_patch.elf')
    run([tool('gcc'), '-mcpu=cortex-m0', '-mthumb', '-c', src, '-o', obj])
    run([tool('ld'), '-T', os.path.join(kemu.REPO, 'ui', 'ui.ld'), obj, '-o', elf])
    sym = symbols(elf)
    code = blob(elf, os.path.join(BUILD, 'ui_patch.bin'))
    return write_patch('ui', 0x08030000, code, [
        (0x0800c7e0, sym['ui_rx_hook'], 'USART consumer: frame parser -> ui_rx_hook'),
        (0x0800f162, sym['ui_redraw_hook'], 'UI main loop: redraw dispatcher -> ui_redraw_hook'),
    ])


if __name__ == '__main__':
    build_ui()
