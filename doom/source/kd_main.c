/*
 * kd_main.c - KeyStep Pro DOOM: entry point and main loop.
 *
 * Boot: load the blob, start the demo lump (KDDEMO) on its map, and play it
 * in a loop. Without a demo lump the map is started on UV with an idle
 * player. Game logic runs in vanilla 35 Hz tics from kd_plat_ms(); a frame
 * is rendered after the tics that are due and handed to kd_plat_present().
 *
 * Also holds the stubs for the parts of DOOM this device does not have
 * (sound, error reporting).
 *
 * Part of the KeyStep Pro DOOM port, GPLv2.
 */
#include <stdarg.h>
#include <stdio.h>
#include "doomstat.h"
#include "d_items.h"
#include "d_net.h"
#include "g_game.h"
#include "p_setup.h"
#include "p_tick.h"
#include "r_main.h"
#include "s_sound.h"
#include "sounds.h"
#include "m_fixed.h"
#include "w_wad.h"
#include "z_zone.h"
#include "m_random.h"
#include "i_system.h"
#include "lprintf.h"
#include "global_data.h"
#include "kd_platform.h"

extern boolean kd_demo_finished;

// most tics run back to back before a frame is forced out
#define KD_MAX_CATCHUP_TICS 8

static uint32_t kd_basems;

// timing statistics, readable by a debugger (symbol kd_stats)
struct
{
    uint32_t frames, tics, render_ms, render_max_ms, tic_ms, present_ms, demo_loops, loop_leveltime, loop_kills, loop_secrets;
} kd_stats;

unsigned int I_GetTime(void)
{
    return (unsigned int) ((uint64_t) (kd_plat_ms() - kd_basems) * TICRATE / 1000);
}

void I_Error(const char *error, ...)
{
    kd_plat_log("I_Error: ");
    kd_plat_log(error);
    kd_plat_log("\n");
    for (;;)
        ;
}

// ---- sound: the KeyStep's metronome click, one click per significant sound
// pitch = timer prescaler for the platform's click tone (higher = lower note), ms = click length
static void kd_sound(fixed_t x, fixed_t y, boolean local, int sfx_id)
{
    int pitch, ms;
    switch (sfx_id)
    {
        case sfx_pistol:                                  pitch = 2;  ms = 14;  break;
        case sfx_shotgn:                                  pitch = 6;  ms = 40;  break;
        case sfx_punch:                                   pitch = 4;  ms = 10;  break;
        case sfx_barexp: case sfx_rxplod:                 pitch = 14; ms = 90;  break;
        case sfx_firsht:                                  pitch = 5;  ms = 18;  break;
        case sfx_firxpl:                                  pitch = 10; ms = 40;  break;
        case sfx_plpain:                                  pitch = 8;  ms = 30;  break;
        case sfx_pldeth:                                  pitch = 16; ms = 150; break;
        case sfx_podth1: case sfx_podth2: case sfx_podth3:
        case sfx_bgdth1: case sfx_bgdth2: case sfx_slop:  pitch = 10; ms = 50;  break;
        case sfx_doropn: case sfx_dorcls:
        case sfx_bdopn: case sfx_bdcls:                   pitch = 12; ms = 25;  break;
        case sfx_itemup: case sfx_wpnup: case sfx_getpow: pitch = 1;  ms = 8;   break;
        default: return;
    }
    if (!local && _g->player.mo)
    {   // vanilla's clipping distance: sounds farther than 1200 units are not heard
        fixed_t dx = D_abs(x - _g->player.mo->x) >> FRACBITS, dy = D_abs(y - _g->player.mo->y) >> FRACBITS;
        if (dx + dy - ((dx < dy ? dx : dy) >> 1) > 1200)
            return;
    }
    kd_plat_click(pitch, ms);
}

void S_Init(int sfxVolume, int musicVolume) { (void) sfxVolume; (void) musicVolume; }
void S_Stop(void) { }
void S_Start(void) { }
void S_StartSound(mobj_t *origin, int sfx_id)
{
    if (!origin) kd_sound(0, 0, true, sfx_id);
    else kd_sound(origin->x, origin->y, origin == _g->player.mo, sfx_id);
}
void S_StartSound2(degenmobj_t *origin, int sfx_id)
{
    if (origin) kd_sound(origin->x, origin->y, false, sfx_id);
}
void S_StopSound(void *origin) { (void) origin; }
void S_StartMusic(int music_id) { (void) music_id; }
void S_ChangeMusic(int music_id, int looping) { (void) music_id; (void) looping; }
void S_StopMusic(void) { }
void S_PauseSound(void) { }
void S_ResumeSound(void) { }
void S_UpdateSounds(void *listener) { (void) listener; }
void S_SetMusicVolume(int volume) { (void) volume; }
void S_SetSfxVolume(int volume) { (void) volume; }

static void kd_start(void)
{
    kd_demo_finished = false;
    if (W_CheckNumForName("KDDEMO") >= 0)
    {
        G_DeferedPlayDemo("KDDEMO");
    }
    else
    {
        // no demo: E1M1 on UV, consoleplayer gets empty ticcmds
        G_ReloadDefaults();
        _g->playeringame = true;
        G_InitNew(sk_hard, 1, 1);
    }
}

void kd_doom_main(void)
{
    kd_plat_init();
    InitGlobals();
    Z_Init();
    W_Init();
    R_Init();
    P_Init();
    kd_plat_log("KeyStep DOOM: start\n");

    kd_start();
    kd_basems = kd_plat_ms();
    _g->gametic = 0;
    int levelstart = -1;

    for (;;)
    {
        int now = I_GetTime();
        int ran = 0;
        uint32_t t0 = kd_plat_ms();
        while (_g->gametic < now || levelstart < 0)
        {
            G_Ticker();
            _g->gametic++;
            ran++;
            if (levelstart < 0 && _g->gamestate == GS_LEVEL)
            {
                // level load time does not count: restart the clock at the first tic
                levelstart = _g->gametic;
                kd_basems = kd_plat_ms() - (uint32_t) ((uint64_t) _g->gametic * 1000 / TICRATE);
                now = I_GetTime();
            }
            if (kd_demo_finished)
            {
                kd_stats.demo_loops++;
                kd_stats.loop_leveltime = _g->leveltime;
                kd_stats.loop_kills = _g->player.killcount;
                kd_stats.loop_secrets = _g->player.secretcount;
                // demo over (or level exited): start again
                kd_start();
                levelstart = -1;
            }
            if (ran >= KD_MAX_CATCHUP_TICS)
            {
                // too slow: drop real time instead of spiralling
                kd_basems = kd_plat_ms() - (uint32_t) ((uint64_t) _g->gametic * 1000 / TICRATE);
                break;
            }
        }
        uint32_t t1 = kd_plat_ms();
        if (_g->gamestate == GS_LEVEL && _g->player.mo)
        {
            const player_t *pl = &_g->player;
            int at = weaponinfo[pl->readyweapon].ammo;
            kd_plat_hud(pl->health, pl->armorpoints, at == am_noammo ? -1 : pl->ammo[at],
                        at == am_noammo ? -1 : pl->maxammo[at]);
            R_RenderPlayerView(&_g->player);
        }
        uint32_t t2 = kd_plat_ms();
        kd_plat_present(kd_framebuffer);
        uint32_t t3 = kd_plat_ms();
        kd_stats.frames++;
        kd_stats.tics += ran;
        kd_stats.tic_ms += t1 - t0;
        kd_stats.render_ms += t2 - t1;
        if (t2 - t1 > kd_stats.render_max_ms)
            kd_stats.render_max_ms = t2 - t1;
        kd_stats.present_ms += t3 - t2;
    }
}
