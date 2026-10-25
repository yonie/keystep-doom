/**
 *  Doom Port to the nRF52840 by Nicola Wrachien (next-hack in the comments)
 *
 *  This port is based on the excellent doomhack's GBA Doom Port,
 *  with Kippykip additions.
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
 *  2026: trimmed for the KeyStep Pro port (demo playback only).
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
 *  The game loop: game state changes, demo playback, level start
 *  and exit, player reborn. Menus, savegames, intermission and finale are
 *  removed (the KeyStep port plays one level and one demo in a loop).
 *-----------------------------------------------------------------------------
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doomstat.h"
#include "d_net.h"
#include "m_random.h"
#include "p_setup.h"
#include "p_tick.h"
#include "p_map.h"
#include "w_wad.h"
#include "r_main.h"
#include "s_sound.h"
#include "sounds.h"
#include "p_inter.h"
#include "g_game.h"
#include "lprintf.h"
#include "i_system.h"
#include "global_data.h"
#include "z_bmalloc.h"
#include "i_spi_support.h"
#include "i_memory.h"
#include "kd_platform.h"

DECLARE_BLOCK_MEMORY_ALLOC_ZONE(secnodezone);

// set by G_CheckDemoStatus / G_DoCompleted, polled by kd_main.c
boolean kd_demo_finished;

static void G_DoLoadLevel(void)
{
    _g->gamestate = GS_LEVEL;

    if (_g->playeringame && _g->player.playerstate == PST_DEAD)
        _g->player.playerstate = PST_REBORN;

    memset(_g->player.frags, 0, sizeof(_g->player.frags));

    // initialize the msecnode_t freelist.                     phares 3/25/98
    NULL_BLOCK_MEMORY_ALLOC_ZONE(secnodezone);
    NULL_BLOCK_MEMORY_ALLOC_ZONE(mobjzone);
    NULL_BLOCK_MEMORY_ALLOC_ZONE(static_mobjzone);

    P_SetupLevel(_g->gameepisode, _g->gamemap, 0, _g->gameskill);

    _g->gameaction = ga_nothing;
    _g->special_event = 0;
    _g->paused = false;
}

//
// G_Ticker
//
void G_Ticker(void)
{
    P_MapStart();
    if (_g->playeringame && _g->player.playerstate == PST_REBORN)
        G_DoReborn(0);
    P_MapEnd();

    // do things to change the game state
    while (_g->gameaction != ga_nothing)
    {
        switch (_g->gameaction)
        {
            case ga_loadlevel:
                _g->player.playerstate = PST_REBORN;
                G_DoLoadLevel();
                break;
            case ga_playdemo:
                G_DoPlayDemo();
                break;
            case ga_completed:
                G_DoCompleted();
                break;
            default:
                _g->gameaction = ga_nothing;
                break;
        }
    }
    if (_g->playeringame)
    {
        ticcmd_t *cmd = &_g->player.cmd;
        memset(cmd, 0, sizeof *cmd);     // no local input on this device
        if (_g->demoplayback)
            G_ReadDemoTiccmd(cmd);
        if (_g->player.cmd.buttons & BT_SPECIAL)
        {
            // pause / save / load specials are ignored in this port
            _g->player.cmd.buttons = 0;
        }
    }

    // do main actions
    if (_g->gamestate == GS_LEVEL && !kd_demo_finished)
    {
        P_Ticker();
    }
}

//
// PLAYER STRUCTURE FUNCTIONS
// also see P_SpawnPlayer in P_Things
//

static void G_PlayerFinishLevel(int player)
{
    (void) player;
    player_t *p = &_g->player;
    memset(p->powers, 0, sizeof p->powers);
    memset(p->cards, 0, sizeof p->cards);
    p->mo = NULL;           // cph - this is allocated PU_LEVEL so it's gone
    p->extralight = 0;      // cancel gun flashes
    p->fixedcolormap = 0;   // cancel ir gogles
    p->damagecount = 0;     // no palette changes
    p->bonuscount = 0;
}

//
// G_PlayerReborn
// Called after a player dies
// almost everything is cleared and initialized
//
void G_PlayerReborn(int player)
{
    player_t *p;
    int i;
    int frags[MAXPLAYERS];
    int killcount;
    int itemcount;
    int secretcount;
    (void) player;

    memcpy(frags, _g->player.frags, sizeof frags);
    killcount = _g->player.killcount;
    itemcount = _g->player.itemcount;
    secretcount = _g->player.secretcount;

    p = &_g->player;

    {
        int cheats = p->cheats;
        memset(p, 0, sizeof(*p));
        p->cheats = cheats;
    }

    memcpy(_g->player.frags, frags, sizeof(_g->player.frags));
    _g->player.killcount = killcount;
    _g->player.itemcount = itemcount;
    _g->player.secretcount = secretcount;

    p->usedown = p->attackdown = true;  // don't do anything immediately
    p->playerstate = PST_LIVE;
    p->health = initial_health;
    p->readyweapon = p->pendingweapon = wp_pistol;
    p->weaponowned[wp_fist] = true;
    p->weaponowned[wp_pistol] = true;
    p->ammo[am_clip] = initial_bullets;

    for (i = 0; i < NUMAMMO; i++)
        p->maxammo[i] = maxammo[i];
}

void G_DoReborn(int playernum)
{
    (void) playernum;
    _g->gameaction = ga_loadlevel;      // reload the level from scratch
}

void G_ExitLevel(void)
{
    _g->secretexit = false;
    _g->gameaction = ga_completed;
}

void G_SecretExitLevel(void)
{
    _g->secretexit = true;
    _g->gameaction = ga_completed;
}

//
// G_DoCompleted: no intermission on this device, the demo loop restarts.
//
void G_DoCompleted(void)
{
    _g->gameaction = ga_nothing;
    if (_g->playeringame)
        G_PlayerFinishLevel(0);        // take away cards and stuff
    kd_demo_finished = true;
}

void G_ReloadDefaults(void)
{
    _g->demoplayback = false;
    _g->singledemo = false;
}

//
// G_InitNew
//
void G_InitNew(skill_t skill, int episode, int map)
{
    if (skill > sk_nightmare)
        skill = sk_nightmare;
    if (episode < 1)
        episode = 1;
    if (map < 1)
        map = 1;

    M_ClearRandom();

    _g->respawnmonsters = skill == sk_nightmare;

    _g->player.playerstate = PST_REBORN;

    _g->usergame = true;                // will be set false if a demo
    _g->paused = false;
    _g->gameepisode = episode;
    _g->gamemap = map;
    _g->gameskill = skill;

    _g->totalleveltimes = 0; // cph
    kd_demo_finished = false;
    G_DoLoadLevel();
}

//
// DEMO PLAYBACK
//

#define DEMOMARKER    0x80

void G_ReadDemoTiccmd(ticcmd_t *cmd)
{
    if (*_g->demo_p == DEMOMARKER)
        G_CheckDemoStatus();      // end of demo data stream
    else if (_g->demoplayback && _g->demo_p + (_g->longtics ? 5 : 4) > _g->demobuffer + _g->demolength)
    {
        lprintf(LO_WARN, "G_ReadDemoTiccmd: missing DEMOMARKER\n");
        G_CheckDemoStatus();
    }
    else
    {
        cmd->forwardmove = ((signed char) *_g->demo_p++);
        cmd->sidemove = ((signed char) *_g->demo_p++);
        if (!_g->longtics)
        {
            cmd->angleturn = ((unsigned char) *_g->demo_p++) << 8;
        }
        else
        {
            unsigned int lowbyte = (unsigned char) *_g->demo_p++;
            cmd->angleturn = (((signed int) (*_g->demo_p++)) << 8) + lowbyte;
        }
        cmd->buttons = (unsigned char) *_g->demo_p++;
    }
}

static const char *defdemoname;

void G_DeferedPlayDemo(const char *name)
{
    defdemoname = name;
    _g->gameaction = ga_playdemo;
}

static const byte* G_ReadDemoHeader(const byte *demo_p, size_t size)
{
    int skill, episode, map;
    const byte *header_p = demo_p;

    _g->basetic = _g->gametic;  // killough 9/29/98

    _g->demover = *demo_p++;
    _g->longtics = 0;

    // Only vanilla demos: versions up to 1.2 use a 7-byte header, 1.4 - 1.9 a 13-byte header,
    // 111 = longtics (Doom 1.91 / Chocolate)
    if (_g->demover <= 4)
    {
        skill = _g->demover;
        episode = *demo_p++;
        map = *demo_p++;
        _g->playeringame = *demo_p++;
        demo_p += 3;   // other players
    }
    else if (_g->demover >= 104 && _g->demover <= 111)
    {
        if (_g->demover >= 111)
            _g->longtics = 1;
        skill = *demo_p++;
        episode = *demo_p++;
        map = *demo_p++;
        demo_p++;       // deathmatch
        demo_p++;       // respawnparm
        demo_p++;       // fastparm
        demo_p++;       // nomonsters
        demo_p++;       // consoleplayer
        _g->playeringame = *demo_p++;
        demo_p += 3;    // other players
    }
    else
    {
        I_Error("G_ReadDemoHeader: Unknown demo format %d.", _g->demover);
        return NULL;
    }
    if ((size_t) (demo_p - header_p) > size)
        I_Error("G_ReadDemoHeader: wrong demo header\n");

    G_InitNew(skill, episode, map);
    _g->player.cheats = 0;
    return demo_p;
}

void G_DoPlayDemo(void)
{
    int demolumpnum = W_GetNumForName(defdemoname);
    _g->demobuffer = W_CacheLumpNum(demolumpnum);
    _g->demolength = W_LumpLength(demolumpnum);

    _g->demoplayback = true;
    _g->demo_p = G_ReadDemoHeader(_g->demobuffer, _g->demolength);

    _g->gameaction = ga_nothing;
    _g->usergame = false;
}

/* G_CheckDemoStatus
 * Called at the end of the demo: the main loop restarts it.
 */
boolean G_CheckDemoStatus(void)
{
    if (_g->demoplayback)
    {
        kd_demo_finished = true;
        return true;
    }
    return false;
}
