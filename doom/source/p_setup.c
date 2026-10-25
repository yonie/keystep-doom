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
 *  Do all the WAD I/O, get map description,
 *  set up initial state and misc. LUTs.
 *
 * next-hack: heavily modified load functions to keep WAD-wise and level-wise
 * constant data in flash, saving tons of RAM.
 * KeyStep port: the converter already wrote every constant structure
 * (vertexes, line_t, side_t, seg_t, nodes, blockmap, reject, sector line
 * lists) in its runtime format into the blob, so they are used in place.
 * Only the mutable parts are built in the zone.
 *-----------------------------------------------------------------------------*/

#include "doomstat.h"
#include "m_bbox.h"
#include "g_game.h"
#include "w_wad.h"
#include "r_main.h"
#include "p_maputl.h"
#include "p_map.h"
#include "p_setup.h"
#include "p_spec.h"
#include "p_tick.h"
#include "p_enemy.h"
#include "s_sound.h"
#include "lprintf.h"
#include "global_data.h"

static void P_LoadVertexes(int lump)
{
    _g->numvertexes = W_LumpLength(lump) / sizeof(vertex_t);
    _g->vertexes = W_CacheLumpNum(lump);
}

static void P_LoadSegs(int lump)
{
    _g->numsegs = W_LumpLength(lump) / sizeof(seg_t);
    _g->segs = (const seg_t*) W_CacheLumpNum(lump);
}

static void P_LoadSubsectors(int lump)
{
    const mapsubsector_t *data;
    int i;

    _g->numsubsectors = W_LumpLength(lump) / sizeof(mapsubsector_t);
    _g->subsectors = Z_Calloc(_g->numsubsectors, sizeof(subsector_t), PU_LEVEL, 0);
    data = (const mapsubsector_t*) W_CacheLumpNum(lump);

    if ((!data) || (!_g->numsubsectors))
        I_Error("P_LoadSubsectors: no subsectors in level");

    for (i = 0; i < _g->numsubsectors; i++)
    {
        _g->subsectors[i].numlines = (unsigned short) SHORT(data[i].numsegs);
        _g->subsectors[i].firstline = (unsigned short) SHORT(data[i].firstseg);
    }
}

static void P_LoadSectors(int lump)
{
    const mapsector_t *data;
    int i;

    _g->numsectors = W_LumpLength(lump) / sizeof(mapsector_t);
    _g->sectors = Z_Calloc(_g->numsectors, sizeof(sector_t), PU_LEVEL, 0);
    data = W_CacheLumpNum(lump);

    for (i = 0; i < _g->numsectors; i++)
    {
        sector_t *ss = _g->sectors + i;
        const mapsector_t *ms = data + i;
        ss->floorheight = SHORT(ms->floorheight) << FRACBITS;
        ss->ceilingheight = SHORT(ms->ceilingheight) << FRACBITS;
        ss->floorpic = R_FlatNumForName(ms->floorpic);
        ss->ceilingpic = R_FlatNumForName(ms->ceilingpic);
        ss->lightlevel = SHORT(ms->lightlevel);
        ss->special = SHORT(ms->special);
        ss->oldspecial = SHORT(ms->special);
        ss->tag = SHORT(ms->tag);
        ss->thinglist_sptr = 0;
        ss->touching_thinglist_sptr = 0;            // phares 3/14/98
    }
}

static void P_LoadNodes(int lump)
{
    numnodes = W_LumpLength(lump) / sizeof(mapnode_t);
    nodes = W_CacheLumpNum(lump);
    if ((!nodes) || (!numnodes))
    {
        if (_g->numsubsectors != 1)
            I_Error("P_LoadNodes: no nodes in level");
    }
}

static void P_LoadThings(int lump)
{
    int i, numthings = W_LumpLength(lump) / sizeof(mapthing_t);
    const mapthing_t *data = W_CacheLumpNum(lump);

    if ((!data) || (!numthings))
        I_Error("P_LoadThings: no things in level");
    _g->totalstatic = 0;
    for (i = 0; i < numthings; i++)
    {
        mapthing_t mt;
        memcpy(&mt, data + i, sizeof(mt));
        mt.x = SHORT(mt.x);
        mt.y = SHORT(mt.y);
        mt.angle = SHORT(mt.angle);
        mt.type = SHORT(mt.type);
        mt.options = SHORT(mt.options);

        if (!P_IsDoomnumAllowed(mt.type))
            continue;

        P_SpawnMapThing(&mt);
    }
}

static void P_LoadLineDefs(int lump)
{
    int i;
    _g->numlines = W_LumpLength(lump) / sizeof(line_t);
    _g->lines = W_CacheLumpNum(lump);

    _g->linedata = Z_Calloc(_g->numlines, sizeof(linedata_t), PU_LEVEL, 0);

    for (i = 0; i < _g->numlines; i++)
    {
        _g->linedata[i].special = _g->lines[i].const_special;
    }
}

static void P_LoadSideDefs(int lump)
{
    // 2021-02-13 next-hack: side defs stay in ROM, only the texture offset
    // (which can change, scrolling walls) is copied to RAM.
    int i;
    _g->numsides = W_LumpLength(lump) / sizeof(side_t);
    _g->sides = (const side_t*) W_CacheLumpNum(lump);
    _g->textureoffsets = Z_Calloc(_g->numsides, sizeof(*_g->textureoffsets), PU_LEVEL, 0);
    for (i = 0; i < _g->numsides; i++)
        _g->textureoffsets[i] = _g->sides[i].textureoffset;

    // 2021-03-13: switches have changeable textures: lines with a special get
    // RAM copies of their side 0 textures.
    int l = 0;
    for (i = 0; i < _g->numlines; i++)
    {
        if (_g->lines[i].const_special)
            l++;
    }
    _g->linesChangeableTextureIndex = Z_Calloc(_g->numlines, sizeof(*_g->linesChangeableTextureIndex), PU_LEVEL, 0);
    _g->switch_texture_top = Z_Calloc(l, sizeof(*_g->switch_texture_top), PU_LEVEL, 0);
    _g->switch_texture_mid = Z_Calloc(l, sizeof(*_g->switch_texture_mid), PU_LEVEL, 0);
    _g->switch_texture_bot = Z_Calloc(l, sizeof(*_g->switch_texture_bot), PU_LEVEL, 0);
    l = 0;
    for (i = 0; i < _g->numlines; i++)
    {
        if (_g->lines[i].const_special)
        {
            _g->linesChangeableTextureIndex[i] = l;
            const side_t *sd = _g->sides + _g->lines[i].sidenum[0];
            _g->switch_texture_top[l] = sd->toptexture;
            _g->switch_texture_mid[l] = sd->midtexture;
            _g->switch_texture_bot[l] = sd->bottomtexture;
            l++;
        }
    }
}

static void P_LoadBlockMap(int lump)
{
    _g->blockmaplump = W_CacheLumpNum(lump);

    _g->bmaporgx = _g->blockmaplump[0] << FRACBITS;
    _g->bmaporgy = _g->blockmaplump[1] << FRACBITS;
    _g->bmapwidth = _g->blockmaplump[2];
    _g->bmapheight = _g->blockmaplump[3];

    // clear out mobj chains - CPhipps - use calloc
    _g->blocklinks_sptrs = Z_Calloc(_g->bmapwidth * _g->bmapheight, sizeof(*_g->blocklinks_sptrs), PU_LEVEL, 0);

    _g->blockmap = _g->blockmaplump + 4;
}

static void P_LoadReject(int lumpnum)
{
    _g->rejectmatrix = W_CacheLumpNum(lumpnum + ML_REJECT);
}

//
// P_GroupLines
// Builds subsector sector numbers, sector line counts (the line lists
// themselves were baked by the converter, lump KDSECLIN, in the same
// order this function used to build them) and sector sound origins.
//
static void P_GroupLines(void)
{
    register const line_t *li;
    register sector_t *sector;
    int i, j;

    for (i = 0; i < _g->numsubsectors; i++)
    {
        const seg_t *seg = &_g->segs[_g->subsectors[i].firstline];
        _g->subsectors[i].sector = NULL;
        for (j = 0; j < _g->subsectors[i].numlines; j++)
        {
            if (seg->sidenum != NO_INDEX)
            {
                _g->subsectors[i].sector = &_g->sectors[_g->sides[seg->sidenum].sector_num];
                break;
            }
            seg++;
        }
        if (_g->subsectors[i].sector == NULL)
            I_Error("P_GroupLines: Subsector a part of no sector!\n");
    }

    // count number of lines in each sector
    for (i = 0, li = _g->lines; i < _g->numlines; i++, li++)
    {
        LN_FRONTSECTOR(li)->linecount++;
        if (LN_BACKSECTOR(li) && LN_BACKSECTOR(li) != LN_FRONTSECTOR(li))
            LN_BACKSECTOR(li)->linecount++;
    }

    int len;
    _g->seclines = W_LumpByName("KDSECLIN", &len);
    if (!_g->seclines)
        I_Error("P_GroupLines: no KDSECLIN");
    for (i = 0, sector = _g->sectors; i < _g->numsectors; i++, sector++)
        sector->lines_idx = _g->numsectors + _g->seclines[i];

    // calculate the box that contains each sector.
    // sound will be generated from the center of this box.
    for (i = 0, sector = _g->sectors; i < _g->numsectors; i++, sector++)
    {
        fixed_t bbox[4];
        M_ClearBox(bbox);

        for (int l = 0; l < sector->linecount; l++)
        {
            const line_t *pline = getSectorLineByIndex(sector, l);

            M_AddToBox(bbox, pline->v1.x, pline->v1.y);
            M_AddToBox(bbox, pline->v2.x, pline->v2.y);
        }
        sector->soundorg.x = bbox[BOXRIGHT] / 2 + bbox[BOXLEFT] / 2;
        sector->soundorg.y = bbox[BOXTOP] / 2 + bbox[BOXBOTTOM] / 2;
    }
}

void P_FreeLevelData()
{
    Z_FreeTags(PU_LEVEL, PU_PURGELEVEL - 1);

    Z_Free(_g->braintargets);
    _g->braintargets = NULL;
    _g->numbraintargets_alloc = _g->numbraintargets = 0;
}

//
// P_SetupLevel
//
void P_SetupLevel(int episode, int map, int playermask, skill_t skill)
{
    char lumpname[9];
    int lumpnum;
    (void) playermask;
    (void) skill;

    _g->totallive = _g->totalkills = _g->totalitems = _g->totalsecret = 0;

    _g->player.killcount = 0;
    _g->player.secretcount = 0;
    _g->player.itemcount = 0;

    // Initial height of PointOfView will be set by player think.
    _g->player.viewz = 1;

    S_Start();
    P_FreeLevelData();

    _g->skyflatnum = R_FlatNumForName("F_SKY1");

    P_InitThinkers();

    // find map name
    lumpname[0] = 'E';
    lumpname[1] = '0' + episode;
    lumpname[2] = 'M';
    lumpname[3] = '0' + map;
    lumpname[4] = 0;
    lumpnum = W_GetNumForName(lumpname);

    _g->leveltime = 0;
    _g->totallive = 0;

    P_LoadVertexes(lumpnum + ML_VERTEXES);
    P_LoadSectors(lumpnum + ML_SECTORS);
    P_LoadLineDefs(lumpnum + ML_LINEDEFS);
    P_LoadSideDefs(lumpnum + ML_SIDEDEFS);
    P_LoadBlockMap(lumpnum + ML_BLOCKMAP);
    P_LoadSubsectors(lumpnum + ML_SSECTORS);
    P_LoadNodes(lumpnum + ML_NODES);
    P_LoadSegs(lumpnum + ML_SEGS);
    P_GroupLines();
    P_LoadReject(lumpnum);

    memset(_g->playerstarts, 0, sizeof(_g->playerstarts));
    _g->player.mo = NULL;

    P_MapStart();

    P_LoadThings(lumpnum + ML_THINGS);

    if (_g->playeringame && !_g->player.mo)
        I_Error("P_SetupLevel: missing player start\n");

    // set up world state
    P_SpawnSpecials();

    P_MapEnd();

    P_InitPicAnims();

    // sky: DOOM determines the sky texture by episode
    {
        static const char skyname[4][5] = { "SKY1", "SKY2", "SKY3", "SKY4" };
        int e = episode >= 1 && episode <= 4 ? episode - 1 : 0;
        _g->skytexture = R_TextureNumForName(skyname[e]);
    }
    R_SetupLevel();
}

//
// P_Init
//
void P_Init(void)
{
    P_InitSwitchList();
}
