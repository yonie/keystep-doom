/**
 *  Doom Port to the nRF52840 by Nicola Wrachien (next-hack in the comments)
 *
 *  This port is based on the excellent doomhack's GBA Doom Port,
 *  with Kippykip additions.
 *  
 *  Several data structures and functions have been optimized 
 *  to fit in only 256kB RAM of nRF52840 (GBA has 384 kB RAM). 
 *  Z-Depth Light has been restored with almost no RAM consumption!
 *  Tons of speed optimizations have been done, and the game now
 *  runs extremely fast, despite the much higher 3D resolution with
 *  respect to GBA.
 *
 *  PrBoom: a Doom port merged with LxDoom and LSDLDoom
 *  based on BOOM, a modified and improved DOOM engine
 *  Copyright (C) 1999 by
 *  id Software, Chi Hoang, Lee Killough, Jim Flynn, Rand Phares, Ty Halderman
 *  Copyright (C) 1999-2000 by
 *  Jess Haas, Nicolas Kalkhof, Colin Phipps, Florian Schulze
 *  Copyright 2005, 2006 by
 *  Florian Schulze, Colin Phipps, Neil Stevens, Andrey Budko
 *  Copyright (C) 2021 Nicola Wrachien (next-hack in the comments)
 *  on nRF52840 port.
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 *  02111-1307, USA.
 *
 * DESCRIPTION:
 * global data collected by doomhack.
 * Modified by next-hack. KeyStep port: only the game state is left here
 * (no menus, HUD, status bar, automap, intermission, sound, renderer).
 */
#ifndef GLOBAL_DATA_H
#define GLOBAL_DATA_H

#include "doomstat.h"
#include "doomdef.h"
#include "m_fixed.h"
#include "g_game.h"
#include "r_defs.h"
#include "p_spec.h"
#include "p_enemy.h"
#include "p_map.h"
#include "p_maputl.h"
#include "p_mobj.h"
#include "p_tick.h"
#include "r_main.h"
#include "w_wad.h"
#include <stdint.h>

extern intercept_t intercepts[MAXINTERCEPTS];
extern boolean interceptIsALine[MAXINTERCEPTS];
extern intercept_t *intercept_p;

typedef struct globals_t
{
//******************************************************************************
//g_game.c
//******************************************************************************
    const byte *demobuffer; /* cph - only used for playback */
    int demolength; // check for overrun (missing DEMOMARKER)
    const byte *demo_p;

    player_t player;

    int totalleveltimes;      // CPhipps - total time for all completed levels
    int longtics;
    int gametic;
    int basetic; /* killough 9/29/98: for demo sync */
    short totalkills, totallive, totalitems, totalsecret;    // for intermission
    short totalstatic;

    byte gameaction;
    byte gamestate;
    byte gameskill;

    uint8_t gameepisode;
    uint8_t gamemap;

    uint8_t demover;
    byte special_event;

    boolean secretexit;
    boolean respawnmonsters;
    boolean paused;
    boolean usergame;      // ok to save / end game
    boolean playeringame;
    boolean demoplayback;
    boolean singledemo;
    boolean menuactive;    // always false here

// Game Mode - identify IWAD as shareware, retail etc.
    byte gamemode;
    byte gamemission;

//******************************************************************************
//m_random.c
//******************************************************************************
    uint8_t rndindex;
    uint8_t prndindex;

//******************************************************************************
//p_ceiling.c
//******************************************************************************
    ceilinglist_t *activeceilings;

//******************************************************************************
//p_enemy.c
//******************************************************************************
    mobj_t *current_actor;
    fixed_t dropoff_deltax, dropoff_deltay, floorz;
    mobj_t *corpsehit;
    fixed_t viletryx;
    fixed_t viletryy;
    mobj_t **braintargets;
    short numbraintargets_alloc;
    short numbraintargets;
    brain_t brain;   // killough 3/26/98: global state of boss brain

//******************************************************************************
//p_map.c
//******************************************************************************
    uint_64_t aim_flags_mask;

    mobj_t *tmthing;
    fixed_t tmx;
    fixed_t tmy;

    int pe_x; // Pain Elemental position for Lost Soul checks // phares
    int pe_y; // Pain Elemental position for Lost Soul checks // phares
    int ls_x; // Lost Soul position for Lost Soul checks      // phares
    int ls_y; // Lost Soul position for Lost Soul checks      // phares

    fixed_t tmbbox[4];  // bounding box for line intersection checks
    fixed_t tmfloorz;   // floor you would hit if free to fall
    fixed_t tmceilingz; // ceiling of sector you are in
    fixed_t tmdropoffz; // dropoff on other side of line you are crossing

    const line_t *ceilingline;
    const line_t *blockline; /* killough 8/11/98: blocking linedef */
    const line_t *floorline; /* killough 8/1/98: Highest touched floor */
    int tmunstuck; /* killough 8/1/98: whether to allow unsticking */

    const line_t *spechit[8];                // vanilla MAXSPECIALCROSS = 8

    msecnode_t *sector_list;                             // phares 3/16/98

    fixed_t bestslidefrac;
    const line_t *bestslideline;
    mobj_t *slidemo;
    fixed_t tmxmove;
    fixed_t tmymove;

    mobj_t *linetarget; // who got hit (or NULL)
    mobj_t *shootthing;
    fixed_t shootz;
    int la_damage;
    fixed_t attackrange;
    fixed_t aimslope;
    fixed_t topslope;
    fixed_t bottomslope;

    mobj_t *bombsource, *bombspot;
    int bombdamage;

    mobj_t *usething;

    short numspechit;
    boolean floatok;
    boolean felldown;
    boolean crushchange, nofit;
    boolean telefrag; /* killough 8/9/98: whether to telefrag at exit */

//******************************************************************************
//p_maputl.c
//******************************************************************************
    fixed_t opentop;
    fixed_t openbottom;
    fixed_t openrange;
    fixed_t lowfloor;
    sector_t *openfrontsector; // made global                    // phares
    sector_t *openbacksector;  // made global
    divline_t trace;

//******************************************************************************
//p_pspr.c
//******************************************************************************
    fixed_t bulletslope;

//******************************************************************************
//p_setup.c
//******************************************************************************
    const vertex_t *vertexes;
    const seg_t *segs;
    sector_t *sectors;
    subsector_t *subsectors;

    short numvertexes;
    short numsegs;
    short numsectors;
    short numsubsectors;
    short numsides;
    short numlines;

    const line_t *lines;
    linedata_t *linedata;

    const side_t *sides;
    short *textureoffsets; // 2021-02-13 next-hack: only the (changeable) texture offset is in RAM
    uint16_t *linesChangeableTextureIndex;
    uint16_t *switch_texture_top;
    uint16_t *switch_texture_mid;
    uint16_t *switch_texture_bot;
    const uint16_t *seclines;   // KeyStep: baked sector line lists (blob lump KDSECLIN)

    short bmapwidth, bmapheight;  // size in mapblocks
    const short *blockmap;              // was short -- killough
    const short *blockmaplump;          // was short -- killough
    fixed_t bmaporgx, bmaporgy;     // origin of block map
    unsigned short *blocklinks_sptrs;
    const byte *rejectmatrix; // cph - const*

    mapthing_t playerstarts[MAXPLAYERS];

//******************************************************************************
//p_spec.c
//******************************************************************************
    anim_t *lastanim;
    anim_t anims[MAXANIMS];
    pusher_t *tmpusher; // pusher structure for blockmap searches

//******************************************************************************
//p_switch.c
//******************************************************************************
    button_t buttonlist[MAXBUTTONS];
    short numswitches;                           // killough

//******************************************************************************
//p_tick.c
//******************************************************************************
    int leveltime; // tics in game play for par
    thinker_t thinkerclasscap[th_all + 1];

//******************************************************************************
//r_data.c
//******************************************************************************
    short numflats;
    short numtextures;

//******************************************************************************
//p_user.c
//******************************************************************************
    boolean onground; // whether player is on ground or in air

//******************************************************************************
//validcount
//******************************************************************************
    unsigned short validcount;         // increment every time a check is made

    short skyflatnum;
    short skytexture;
    short numlumps;
} globals_t;

void InitGlobals();

extern globals_t *_g;

static inline const line_t* getSectorLineByIndex(sector_t *psec, int i)
{
    return &_g->lines[_g->seclines[psec->lines_idx + i]];
}

// validcount is 16 bits in RAM; on wrap every stamp is cleared so a stale
// stamp can never equal the new count (same behaviour as vanilla's int).
void P_IncValidCount(void);

#endif // GLOBAL_DATA_H
