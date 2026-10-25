# DOOM engine for the KeyStep Pro

The real DOOM engine, running as the only firmware on the KeyStep Pro main MCU (Cortex-M4, 480 KB flash, 128 KB RAM). It renders E1M1 on the 128x64 OLED and loops a vanilla v1.9 demo.

This directory is GPLv2 (see `LICENSE`). It derives from next-hack's MCU DOOM ports by Nicola Wrachien: MG21DOOM (the IKEA TRADFRI lamp port) and its successor, nRF52840 Doom. Those are based on doomhack's GBADoom, which is based on PrBoom and id Software's DOOM. The file headers keep their copyright notices.

## What changed from next-hack's port

- **Data read in place.** The game data is one const blob, `kd_wad[]`, read through plain pointers. There is no SPI flash, no flash writes and no level cache.
- **Offline converter.** `converter/kd_convert.py` writes the map in the engine's runtime formats: `line_t`, `seg_t`, `side_t`, nodes, blockmap, reject and the sector line lists. It also keeps only the textures, flats and sprites the map can show. Graphics are 4-bit luminance at half resolution, with one light table instead of 34 colormaps.
- **New renderer.** `source/kd_render.c` is a vanilla-style renderer at 128x64 that renders 8-bit luminance into an 8 KB buffer. Each frame then gets a contrast stretch (2nd..98th percentile, gamma 0.7) and Atkinson error diffusion into the 1 KB SSD1306 page buffer. It has no visplanes: floors and ceilings are drawn per column. It never writes game state.
- **Memory.** A single 128 KB RAM region holds the 16-bit pointers (`include/i_memory.h`). The game globals and the zone share one block, `kd_ram` (`include/kd_ram.h`).
- **Removed:** sound, music, menus, status bar, automap, intermission, finale, savegames, network and input.
- **Vanilla demo sync restored.** The GBA/PrBoom changes that broke demo sync are undone in:
  - `P_LookForPlayers` (lastlook walk over 4 slots, no threshold)
  - the thinker loop
  - line-of-sight checks
  - the 16-bit validcount wrap
  - `MAXSPECIALCROSS`

  `source/kd_play.c` holds the BSP, angle, sight and thinker code.

## Build

```
python doom/build.py data   # build/kd_wad_e1m1.bin from build/doomu.wad (+ build/e1m1.lmp)
python doom/build.py arm    # build/doom_arm/libkddoom.a + a size test link
```

`converter/gen_tables.py` regenerates `source/kd_rtables.c`, which holds the view-size tables.

## Board integration

- **Entry point.** `void kd_doom_main(void)` never returns. The platform implements `include/kd_platform.h`.
- **Linker.** The blob goes in a section `.wad` as `kd_wad[]`, 4-byte aligned. Standard `.text`, `.rodata`, `.data` and `.bss` sections. Stack 6 KB.
- **No heap.** There is no malloc, printf or floating point. Only `memcpy`, `memset`, `memcmp` and libgcc division are used.
- **RAM.** About 94 KB of `.bss`:
  - `kd_ram`: 74.6 KB, of which the zone is 72 KB. Peak zone use on the E1M1 demo is 24 KB.
  - renderer: about 13 KB (drawsegs, openings, vissprites, clip arrays).
  - frame buffers: 8 KB luminance + 1 KB page buffer.
