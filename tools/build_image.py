"""Link the DOOM-only main MCU image: board layer + engine (or the test stand-in) + data blob.

  python tools/build_image.py [--wad build/kd_wad_e1m1.bin] [--lib build/doom_arm/libkddoom.a]

Writes build/keystep_doom.elf/.bin/.map and prints the flash and RAM budget against the real part
(480 KB from 0x08008000, 128 KB SRAM).
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kemu
import build as B

CFLAGS = ['-mcpu=cortex-m4', '-mthumb', '-mfloat-abi=soft', '-O2', '-ffreestanding', '-ffunction-sections',
          '-fdata-sections', '-Wall']
FLASH_LIMIT = 0x0807FFF0 - 0x08008000
RAM_LIMIT = 128 * 1024


def arg(name, default):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default


def main():
    out = os.path.join(kemu.REPO, 'build')
    os.makedirs(out, exist_ok=True)
    subprocess.run([sys.executable, os.path.join(kemu.REPO, 'tools', 'gen_pins.py')], check=True)
    srcs = [os.path.join(kemu.REPO, 'main', 'kd_board.c'), os.path.join(kemu.REPO, 'main', 'kd_intro.c')]
    objs, libs = [], []
    libs.append(arg('--lib', os.path.join(out, 'doom_arm', 'libkddoom.a')))
    wad = arg('--wad', os.path.join(out, 'kd_wad_e1m1.bin'))
    wobj = os.path.join(out, 'kd_wad.o')
    # the blob as symbol kd_wad[] in section .wad (read in place by the engine)
    B.run([B.tool('objcopy'), '-I', 'binary', '-O', 'elf32-littlearm', '-B', 'arm',
           '--rename-section', '.data=.wad,alloc,load,readonly,data,contents',
           '--redefine-sym', '_binary_%s_start=kd_wad' % wad.replace('\\', '_').replace('/', '_')
           .replace(':', '_').replace('.', '_').replace('-', '_'), wad, wobj])
    objs.append(wobj)
    elf = os.path.join(out, 'keystep_doom.elf')
    B.run([B.tool('gcc')] + CFLAGS + ['-nostartfiles', '-T', os.path.join(kemu.REPO, 'main', 'kd_board.ld'),
          '-Wl,--gc-sections', '-Wl,-Map=' + os.path.join(out, 'keystep_doom.map'),
          '-I', os.path.join(kemu.REPO, 'main')] + srcs + objs + libs +
          ['--specs=nano.specs', '-lc', '-lgcc', '-o', elf])
    B.run([B.tool('objcopy'), '-O', 'binary', '-R', '.bss', '-R', '._stack', elf,
           os.path.join(out, 'keystep_doom.bin')])
    sym = B.symbols(elf)
    flash = sym['_flash_end'] - 0x08008000
    ram = sym['_ebss'] - 0x20000000 + 6 * 1024
    wadsz = os.path.getsize(arg('--wad', os.path.join(out, 'kd_wad_e1m1.bin')))
    print('flash %d / %d bytes (%.1f%%), of which data blob %d' % (flash, FLASH_LIMIT, 100.0 * flash / FLASH_LIMIT, wadsz))
    print('RAM   %d / %d bytes (%.1f%%) incl. 6 KB stack' % (ram, RAM_LIMIT, 100.0 * ram / RAM_LIMIT))
    if flash > FLASH_LIMIT or ram > RAM_LIMIT:
        sys.exit('OVER BUDGET')


if __name__ == '__main__':
    main()
