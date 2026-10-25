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
 *  KeyStep port: lump access into the const data blob kd_wad[].
 *-----------------------------------------------------------------------------*/
#include "doomstat.h"
#include "doomtype.h"
#include "i_system.h"
#include "w_wad.h"
#include "lprintf.h"
#include "global_data.h"
#include "kd_platform.h"
#include <string.h>

static const kd_lump_t *kd_dir;

static const kd_wadheader_t* hdr(void)
{
    return (const kd_wadheader_t*) kd_wad;
}

void W_Init(void)
{
    if (memcmp(hdr()->magic, "KDW1", 4))
        I_Error("W_Init: bad data blob");
    _g->numlumps = hdr()->numlumps;
    kd_dir = (const kd_lump_t*) (kd_wad + hdr()->dirofs);
}

int PUREFUNC W_CheckNumForName(const char *name)
{
    char n8[8];
    int i;
    memset(n8, 0, 8);
    for (i = 0; i < 8 && name[i]; i++)
        n8[i] = (name[i] >= 'a' && name[i] <= 'z') ? name[i] - 32 : name[i];
    for (i = _g->numlumps - 1; i >= 0; i--)
        if (!memcmp(kd_dir[i].name, n8, 8))
            return i;
    return -1;
}

int PUREFUNC W_GetNumForName(const char *name)
{
    int i = W_CheckNumForName(name);
    if (i == -1)
        I_Error("W_GetNumForName: %.8s not found", name);
    return i;
}

const char* PUREFUNC W_GetNameForNum(int lump)
{
    if (lump < 0 || lump >= _g->numlumps)
        return NULL;
    return kd_dir[lump].name;
}

int PUREFUNC W_LumpLength(int lump)
{
    if (lump < 0 || lump >= _g->numlumps)
        I_Error("W_LumpLength: %i >= numlumps", lump);
    return kd_dir[lump].size;
}

const void* W_CacheLumpNum(int lump)
{
    if (lump < 0 || lump >= _g->numlumps)
        I_Error("W_CacheLumpNum: %i >= numlumps", lump);
    return kd_wad + kd_dir[lump].filepos;
}

const void* W_LumpByName(const char *name, int *length)
{
    int i = W_CheckNumForName(name);
    if (i < 0)
    {
        if (length)
            *length = 0;
        return NULL;
    }
    if (length)
        *length = kd_dir[i].size;
    return W_CacheLumpNum(i);
}
