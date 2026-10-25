#!/usr/bin/env python3
"""
doom/build.py - builds the KeyStep DOOM engine.

  python doom/build.py data     # converter: build/kd_wad_e1m1.bin from build/doomu.wad (+ build/e1m1.lmp)
  python doom/build.py arm      # build/doom_arm/libkddoom.a + size report
  python doom/build.py all

Every compiler process is started at below-normal priority (Windows).
Part of the KeyStep Pro DOOM port, GPLv2.
"""
import glob
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOOM = os.path.join(ROOT, "doom")
BUILD = os.path.join(ROOT, "build")
TOOLS = os.environ.get("KD_TOOLS", os.path.expanduser("~/tools"))   # where the ARM compiler lives
ARM_BIN = os.environ.get("KD_ARM_BIN", os.path.join(TOOLS, "xpack-arm-none-eabi-gcc-14.2.1-1.1", "bin"))
ARM_CC = os.path.join(ARM_BIN, "arm-none-eabi-gcc.exe" if os.name == "nt" else "arm-none-eabi-gcc")
if not os.path.exists(ARM_CC):          # not in ~/tools: take the toolchain from PATH
    ARM_BIN = ""
    ARM_CC = "arm-none-eabi-gcc"
ARM_AR = os.path.join(ARM_BIN, "arm-none-eabi-ar.exe" if os.name == "nt" else "arm-none-eabi-ar") if ARM_BIN else "arm-none-eabi-ar"
ARM_SIZE = os.path.join(ARM_BIN, "arm-none-eabi-size.exe" if os.name == "nt" else "arm-none-eabi-size")
ARM_NM = os.path.join(ARM_BIN, "arm-none-eabi-nm.exe" if os.name == "nt" else "arm-none-eabi-nm")

BELOW_NORMAL = 0x00004000

COMMON = ["-std=gnu11", "-Wall", "-Wno-unused-variable", "-Wno-unused-but-set-variable", "-Wno-unused-function",
          "-Wno-missing-braces", "-Wno-char-subscripts", "-Wno-parentheses", "-Wno-pointer-sign",
          "-fno-strict-aliasing", "-fwrapv", "-I" + os.path.join(DOOM, "include")]


def run(cmd, **kw):
    flags = BELOW_NORMAL if os.name == "nt" else 0
    if os.name != "nt":
        cmd = ["nice", "-n", "10"] + cmd
    r = subprocess.run(cmd, creationflags=flags, **kw)
    if r.returncode:
        sys.exit("command failed: %s" % " ".join(cmd[:3]))
    return r


def sources():
    return sorted(glob.glob(os.path.join(DOOM, "source", "*.c")))


def compile_parallel(cc, flags, srcs, objdir):
    os.makedirs(objdir, exist_ok=True)
    procs = []
    objs = []
    failed = []
    flagsnt = BELOW_NORMAL if os.name == "nt" else 0
    for s in srcs:
        o = os.path.join(objdir, os.path.splitext(os.path.basename(s))[0] + ".o")
        objs.append(o)
        if os.path.exists(o) and os.path.getmtime(o) > max([os.path.getmtime(s)] + [os.path.getmtime(h) for h in glob.glob(os.path.join(DOOM, "include", "*.h"))]):
            continue
        cmd = [cc] + flags + ["-c", s, "-o", o]
        if os.name != "nt":
            cmd = ["nice", "-n", "10"] + cmd
        procs.append((s, subprocess.Popen(cmd, creationflags=flagsnt)))
        if len(procs) >= 8:
            s0, p0 = procs.pop(0)
            if p0.wait():
                failed.append(s0)
    for s0, p0 in procs:
        if p0.wait():
            failed.append(s0)
    if failed:
        sys.exit("compile failed: " + " ".join(os.path.basename(f) for f in failed))
    return objs


def build_data():
    wad = os.path.join(BUILD, "doomu.wad")
    demo = os.path.join(BUILD, "e1m1.lmp")
    out = os.path.join(BUILD, "kd_wad_e1m1.bin")
    cmd = [sys.executable, os.path.join(DOOM, "converter", "kd_convert.py"), "--iwad", wad, "--map", "E1M1", "--out", out]
    if os.path.exists(demo):
        cmd += ["--demo", demo]
    run(cmd)


ARM_FLAGS = ["-mcpu=cortex-m4", "-mthumb", "-mfloat-abi=soft", "-O2", "-ffunction-sections", "-fdata-sections", "-g"]


def build_arm():
    objdir = os.path.join(BUILD, "doom_arm", "obj")
    flags = COMMON + ARM_FLAGS
    objs = compile_parallel(ARM_CC, flags, sources(), objdir)
    lib = os.path.join(BUILD, "doom_arm", "libkddoom.a")
    if os.path.exists(lib):
        os.remove(lib)
    run([ARM_AR, "rcs", lib] + objs)
    print("arm:", lib)
    # size report: link a throwaway image with a dummy platform to see what survives --gc-sections
    test_c = os.path.join(BUILD, "doom_arm", "sizetest_plat.c")
    with open(test_c, "w") as f:
        f.write('#include <stdint.h>\n'
                'const uint8_t kd_wad[4] __attribute__((section(".wad"))) = {0};\n'
                'void kd_plat_init(void){}\nuint32_t kd_plat_ms(void){return 0;}\n'
                'void kd_plat_present(const uint8_t fb[1024]){(void)fb;}\nvoid kd_plat_log(const char*s){(void)s;}\n'
                'void kd_plat_hud(int h,int a,int m,int x){(void)h;(void)a;(void)m;(void)x;}\n'
                'void kd_plat_click(int p,int m){(void)p;(void)m;}\n'
                'void kd_doom_main(void);\nvoid Reset_Handler(void){kd_doom_main();}\n'
                '__attribute__((section(".vectors"))) const void* vectors[2] = {(void*)0x20020000, (void*)Reset_Handler};\n')
    ld = os.path.join(BUILD, "doom_arm", "sizetest.ld")
    with open(ld, "w") as f:
        f.write("""MEMORY { FLASH (rx) : ORIGIN = 0x08008000, LENGTH = 491504
RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 128K }
ENTRY(Reset_Handler)
SECTIONS {
 .vectors : { KEEP(*(.vectors)) } > FLASH
 .text : { *(.text*) *(.rodata*) } > FLASH
 .wad : { KEEP(*(.wad)) } > FLASH
 .data : { *(.data*) } > RAM AT > FLASH
 .bss (NOLOAD) : { *(.bss*) *(COMMON) } > RAM
}
""")
    elf = os.path.join(BUILD, "doom_arm", "sizetest.elf")
    run([ARM_CC] + ARM_FLAGS + ["-nostartfiles", "-Wl,--gc-sections", "-T", ld, "-o", elf, test_c, lib, "-Wl,-Map=" + elf[:-4] + ".map",
                                "--specs=nano.specs", "--specs=nosys.specs", "-lm"])
    run([ARM_SIZE, "-A", elf])


def main():
    what = sys.argv[1:] or ["all"]
    for w in what:
        if w in ("data", "all"):
            build_data()
        if w in ("arm", "all"):
            build_arm()


if __name__ == "__main__":
    main()
