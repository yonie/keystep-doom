/*
 * r_main.h - KeyStep DOOM renderer interface (128x64, 1-bit dithered).
 *
 * The renderer (kd_render.c) is a vanilla-style BSP renderer working on the
 * data blob formats written by converter/kd_convert.py.
 *
 * Part of the KeyStep Pro DOOM port, GPLv2 (derived from DOOM / PrBoom /
 * GBADoom / next-hack's MCU ports).
 */
#ifndef __R_MAIN__
#define __R_MAIN__

#include "d_player.h"
#include "r_defs.h"

// BSP / geometry (kd_play.c)
extern short numnodes;
extern const mapnode_t *nodes;

angle_t R_PointToAngle2(fixed_t x1, fixed_t y1, fixed_t x2, fixed_t y2);
subsector_t* R_PointInSubsector(fixed_t x, fixed_t y);
int R_PointOnSide(fixed_t x, fixed_t y, const mapnode_t *node);

// texture / flat tables (kd_render.c), indices as stored in side_t / sector_t
extern fixed_t *textureheight;      // original (full resolution) texture heights, used by play code
extern short *texturetranslation;   // animated textures
extern short *flattranslation;      // animated flats

int R_CheckTextureNumForName(const char *name);   // -1 if missing
int R_TextureNumForName(const char *name);        // 0 if missing
int R_FlatNumForName(const char *name);           // 0 if missing (vanilla would error)
int R_CheckFlatNumForName(const char *name);      // -1 if missing

void R_Init(void);                           // after W_Init
void R_SetupLevel(void);                     // after P_SetupLevel
void R_RenderPlayerView(player_t *player);   // renders into the 1-bpp page buffer
extern uint8_t kd_framebuffer[1024];         // SSD1306 page format (byte = page*128 + x, bit = y & 7)

#endif
