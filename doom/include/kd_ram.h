/*
 * kd_ram.h - the one RAM block that holds the game globals and the zone heap.
 *
 * Everything that is ever referenced by a 16-bit "short pointer"
 * (mobjs, thinkers, sectors, subsectors, msecnodes, the player) lives in
 * here, so a short pointer ((addr - base) / 4) reaches all of it.
 * On the board the base is the start of SRAM (0x20000000); on the host
 * build it is the address of kd_ram itself.
 *
 * KD_ZONE_SIZE is the zone heap. It is sized so that bss + data + stack fit in
 * 128 kB on the board (see doom/README.md for the RAM map).
 *
 * Part of the KeyStep Pro DOOM port, GPLv2.
 */
#ifndef KD_RAM_H
#define KD_RAM_H
#include <stdint.h>
#include "global_data.h"

#ifndef KD_ZONE_SIZE
#define KD_ZONE_SIZE (72 * 1024)
#endif

typedef struct
{
    uint32_t guard[4];      // keeps every object away from short pointer 0 (= NULL)
    globals_t global;
    uint8_t zone[KD_ZONE_SIZE] __attribute__((aligned(8)));
} kd_ram_t;

extern kd_ram_t kd_ram;

#endif
