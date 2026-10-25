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
 *  KeyStep port: the data blob ("KDW1", built by converter/kd_convert.py)
 *  is read in place through plain const pointers. Directory entries are
 *  {name[8], int filepos, int size}, filepos relative to the blob start.
 *-----------------------------------------------------------------------------*/
#ifndef __W_WAD__
#define __W_WAD__

#include "doomtype.h"
#include <stdint.h>
#include "r_defs.h"
#include "i_memory.h"

typedef struct
{
    char name[8];
    int filepos;
    int size;
} kd_lump_t;

typedef struct
{
    char magic[4];      // "KDW1"
    int numlumps;
    int dirofs;
    int totalsize;
} kd_wadheader_t;

void W_Init(void);
int PUREFUNC W_CheckNumForName(const char *name);
int PUREFUNC W_GetNumForName(const char *name);
const char* PUREFUNC W_GetNameForNum(int lump);
int PUREFUNC W_LumpLength(int lump);
const void* W_CacheLumpNum(int lump);
#define W_CacheLumpName(name) W_CacheLumpNum(W_GetNumForName(name))
// returns the lump data for a name (NULL if missing), optional length
const void* W_LumpByName(const char *name, int *length);
#endif
