/*
 * kd_platform.h - interface between the KeyStep DOOM engine and the platform.
 * The board implementation lives in main/ (platform worker), the host one in
 * doom/host/host_main.c.
 *
 * GPLv2 (part of the DOOM port in this directory).
 */
#ifndef KD_PLATFORM_H
#define KD_PLATFORM_H
#include <stdint.h>

void     kd_plat_init(void);                     // clocks, timebase, display link
uint32_t kd_plat_ms(void);                       // milliseconds since boot
void     kd_plat_present(const uint8_t fb[1024]);// one 1-bpp page-format frame (blocks while the previous is still sending)
void     kd_plat_log(const char *s);             // debug text (host: stdout; board: may be a no-op)
// player status once per rendered frame, for the panel LEDs (ammo/maxammo = -1: weapon without ammo)
void     kd_plat_hud(int health, int armor, int ammo, int maxammo);
// one metronome click: pitch = tone timer prescaler (1 high .. 16 low), ms = length
void     kd_plat_click(int pitch, int ms);
extern const uint8_t kd_wad[];                   // the converted data blob (board: linked in flash, section .wad)

// engine entry point, never returns
void kd_doom_main(void);


#endif
