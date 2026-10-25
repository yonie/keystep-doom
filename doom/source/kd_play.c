/*
 *  KeyStep Pro DOOM port: game-logic helpers that used to live in next-hack's
 *  r_fast_stuff.c (nRF52840 / MG21 Doom), moved here so the renderer can be
 *  replaced without touching play code.
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
 *  BSP point location, angles, line of sight, mobj state machine and the
 *  thinker loop. These must behave exactly like vanilla DOOM 1.9 so that
 *  demos stay in sync; differences from the PrBoom/GBA code are marked
 *  "KeyStep vanilla".
 */
#include "doomstat.h"
#include "r_main.h"
#include "p_map.h"
#include "p_maputl.h"
#include "p_setup.h"
#include "p_tick.h"
#include "p_spec.h"
#include "m_bbox.h"
#include "m_random.h"
#include "tables.h"
#include "global_data.h"

extern los_t los;
short numnodes;
const mapnode_t *nodes;

fixed_t CONSTFUNC FixedMul(fixed_t a, fixed_t b)
{
    return (fixed_t) ((int_64_t) a * b >> FRACBITS);
}

static CONSTFUNC int SlopeDiv(unsigned num, unsigned den)
{
    unsigned int ans;

    if (den < 512)
        return SLOPERANGE;

    ans = (num << 3) / (den >> 8);

    return ans <= SLOPERANGE ? ans : SLOPERANGE;
}

//
// R_PointOnSide
// Traverse BSP (sub) tree,
//  check point against partition plane.
// Returns side 0 (front) or 1 (back).
//
int PUREFUNC R_PointOnSide(fixed_t x, fixed_t y, const mapnode_t *node)
{
    fixed_t dx = (fixed_t) node->dx << FRACBITS;
    fixed_t dy = (fixed_t) node->dy << FRACBITS;

    fixed_t nx = (fixed_t) node->x << FRACBITS;
    fixed_t ny = (fixed_t) node->y << FRACBITS;

    if (!dx)
        return x <= nx ? node->dy > 0 : node->dy < 0;

    if (!dy)
        return y <= ny ? node->dx < 0 : node->dx > 0;

    x -= nx;
    y -= ny;

    // Try to quickly decide by looking at sign bits.
    if ((dy ^ dx ^ x ^ y) < 0)
        return (dy ^ x) < 0;  // (left is negative)

    return FixedMul(y, node->dx) >= FixedMul(node->dy, x);
}

subsector_t* R_PointInSubsector(fixed_t x, fixed_t y)
{
    int nodenum = numnodes - 1;

    // special case for trivial maps (single subsector, no nodes)
    if (numnodes == 0)
        return _g->subsectors;

    while (!(nodenum & NF_SUBSECTOR))
        nodenum = nodes[nodenum].children[R_PointOnSide(x, y, nodes + nodenum)];
    return &_g->subsectors[nodenum & ~NF_SUBSECTOR];
}

CONSTFUNC angle_t R_PointToAngle2(fixed_t vx, fixed_t vy, fixed_t x, fixed_t y)
{
    x -= vx;
    y -= vy;

    if ((!x) && (!y))
        return 0;

    if (x >= 0)
    {
        if (y >= 0)
        {
            if (x > y)
                return tantoangle[SlopeDiv(y, x)];              // octant 0
            else
                return ANG90 - 1 - tantoangle[SlopeDiv(x, y)];  // octant 1
        }
        else
        {
            y = -y;
            if (x > y)
                return -tantoangle[SlopeDiv(y, x)];             // octant 8
            else
                return ANG270 + tantoangle[SlopeDiv(x, y)];     // octant 7
        }
    }
    else
    {
        x = -x;
        if (y >= 0)
        {
            if (x > y)
                return ANG180 - 1 - tantoangle[SlopeDiv(y, x)]; // octant 3
            else
                return ANG90 + tantoangle[SlopeDiv(x, y)];      // octant 2
        }
        else
        {
            y = -y;
            if (x > y)
                return ANG180 + tantoangle[SlopeDiv(y, x)];     // octant 4
            else
                return ANG270 - 1 - tantoangle[SlopeDiv(x, y)]; // octant 5
        }
    }
}

//
// P_IncValidCount: see global_data.h
//
void P_IncValidCount(void)
{
    if (++_g->validcount == 0)
    {
        for (int i = 0; i < _g->numlines; i++)
            _g->linedata[i].validcount = 0;
        for (int i = 0; i < _g->numsectors; i++)
            _g->sectors[i].validcount = 0;
        _g->validcount = 1;
    }
}

//
// P_DivlineSide
// Returns side 0 (front), 1 (back), or 2 (on).
//
// KeyStep vanilla: the horizontal-line case compares x with node->y, as in
// DOOM 1.9 (PrBoom keeps this bug for its vanilla compatibility levels).
static int P_DivlineSide(fixed_t x, fixed_t y, const divline_t *node)
{
    fixed_t left, right;
    if (!node->dx)
    {
        if (x == node->x)
            return 2;
        if (x <= node->x)
            return node->dy > 0;
        return node->dy < 0;
    }
    if (!node->dy)
    {
        if (x == node->y)
            return 2;
        if (y <= node->y)
            return node->dx < 0;
        return node->dx > 0;
    }
    right = ((y - node->y) >> FRACBITS) * (node->dx >> FRACBITS);
    left = ((x - node->x) >> FRACBITS) * (node->dy >> FRACBITS);
    if (right < left)
        return 0;
    if (left == right)
        return 2;
    return 1;
}

//
// P_CrossSubsector
// Returns true
//  if strace crosses the given subsector successfully.
//
// KeyStep vanilla: DOOM 1.9 order of tests (PrBoom's P_CrossSubsector_Doom).
static boolean P_CrossSubsector(int num)
{
    const seg_t *seg = _g->segs + _g->subsectors[num].firstline;
    int count;
    fixed_t opentop, openbottom;
    const sector_t *front, *back;

    for (count = _g->subsectors[num].numlines; --count >= 0; seg++)
    { // check lines
        int linenum = seg->linenum;
        const line_t *line = &_g->lines[linenum];
        divline_t divl;

        // allready checked other side?
        if (_g->linedata[linenum].validcount == (_g->validcount))
            continue;

        _g->linedata[linenum].validcount = _g->validcount;

        const vertex_t *v1 = &line->v1;
        const vertex_t *v2 = &line->v2;

        if (P_DivlineSide(v1->x, v1->y, &los.strace) == P_DivlineSide(v2->x, v2->y, &los.strace))
            continue;

        divl.dx = v2->x - (divl.x = v1->x);
        divl.dy = v2->y - (divl.y = v1->y);

        // line isn't crossed?
        if (P_DivlineSide(los.strace.x, los.strace.y, &divl) == P_DivlineSide(los.t2x, los.t2y, &divl))
            continue;

        // stop because it is not two sided anyway
        if (!(line->flags & ML_TWOSIDED))
            return false;

        // crosses a two sided line
        front = SG_FRONTSECTOR(seg);
        back = SG_BACKSECTOR(seg);

        // no wall to block sight with?
        if (front->floorheight == back->floorheight && front->ceilingheight == back->ceilingheight)
            continue;

        // possible occluder
        // because of ceiling height differences
        opentop = front->ceilingheight < back->ceilingheight ? front->ceilingheight : back->ceilingheight;

        // because of floor height differences
        openbottom = front->floorheight > back->floorheight ? front->floorheight : back->floorheight;

        // quick test for totally closed doors
        if (openbottom >= opentop)
            return false;               // stop

        fixed_t frac = P_InterceptVector2(&los.strace, &divl);

        if (front->floorheight != back->floorheight)
        {
            fixed_t slope = FixedDiv(openbottom - los.sightzstart, frac);
            if (slope > los.bottomslope)
                los.bottomslope = slope;
        }

        if (front->ceilingheight != back->ceilingheight)
        {
            fixed_t slope = FixedDiv(opentop - los.sightzstart, frac);
            if (slope < los.topslope)
                los.topslope = slope;
        }

        if (los.topslope <= los.bottomslope)
            return false;               // stop
    }
    // passed the subsector ok
    return true;
}

boolean P_CrossBSPNode(int bspnum)
{
    while (!(bspnum & NF_SUBSECTOR))
    {
        const mapnode_t *bsp = nodes + bspnum;

        divline_t dl;
        dl.x = ((fixed_t) bsp->x << FRACBITS);
        dl.y = ((fixed_t) bsp->y << FRACBITS);
        dl.dx = ((fixed_t) bsp->dx << FRACBITS);
        dl.dy = ((fixed_t) bsp->dy << FRACBITS);

        int side, side2;
        side = P_DivlineSide(los.strace.x, los.strace.y, &dl);
        if (side == 2)
            side = 0;   // an "on" is considered to be on the front side
        side2 = P_DivlineSide(los.t2x, los.t2y, &dl);

        if (side == side2)
            bspnum = bsp->children[side]; // doesn't touch the other side
        else if (!P_CrossBSPNode(bsp->children[side]))
            return 0;  // cross the starting side
        else
            bspnum = bsp->children[side ^ 1];  // cross the ending side
    }
    return P_CrossSubsector(bspnum == -1 ? 0 : bspnum & ~NF_SUBSECTOR);
}

//
// P_MobjThinker
//

void P_NightmareRespawn(mobj_t *mobj);
void P_XYMovement(mobj_t *mo);
void P_ZMovement(mobj_t *mo);

//
// P_SetMobjState
// Returns true if the mobj is still present.
//
boolean P_SetMobjState(mobj_t *mobj, statenum_t state)
{
    const state_t *st;

    do
    {
        if (state == S_NULL)
        {
            mobj->state_idx = state;
            P_RemoveMobj(mobj);
            return false;
        }
        st = &states[state];
        mobj->state_idx = state;
        mobj->tics = st->tics;
        mobj->sprite = st->sprite;
        mobj->frame = st->frame;

        // Modified handling.
        // Call action functions when the state is set
        if (st->action)
            st->action(mobj);

        state = st->nextstate;

    } while (!mobj->tics);

    return true;
}

void P_MobjThinker(mobj_t *mobj)
{
    // momentum movement
    if (mobj->momx || mobj->momy || (mobj->flags & MF_SKULLFLY))
    {
        P_XYMovement(mobj);
        if (mobj->thinker.function != P_MobjThinker) // cph - Must've been removed
            return;       // killough - mobj was removed
    }

    if (mobj->z != mobj->floorz || mobj->momz)
    {
        P_ZMovement(mobj);
        if (mobj->thinker.function != P_MobjThinker) // cph - Must've been removed
            return;       // killough - mobj was removed
    }

    // cycle through states,
    // calling action functions at transitions

    if (mobj->tics != -1)
    {
        mobj->tics--;

        // you can cycle through multiple states in a tic

        if (!mobj->tics)
            if (!P_SetMobjState(mobj, getMobjState(mobj)->nextstate))
                return;     // freed itself
    }
    else
    {
        // check for nightmare respawn
        if (!(mobj->flags & MF_COUNTKILL))
            return;

        if (!_g->respawnmonsters)
            return;

        mobj->movecount++;

        if (mobj->movecount < 12 * 35)
            return;

        if (_g->leveltime & 31)
            return;

        if (P_Random() > 4)
            return;

        P_NightmareRespawn(mobj);
    }
}

void P_RemoveThinkerDelayed(thinker_t *thinker);
void P_RemoveStaticThinkerDelayed(thinker_t *thinker);

//
// P_RunThinkers
//
// KeyStep vanilla: like DOOM 1.9, the next thinker is read after the
// current one ran, so thinkers spawned by the last one in the list still
// think in this tic. Removed thinkers are unlinked first.
void P_RunThinkers(void)
{
    thinker_t *th = getThinkerNext(&thinkercap);
    thinker_t *th_end = &thinkercap;

    while (th != th_end)
    {
        thinker_t *th_next;
        if (th->function == (think_t) P_RemoveThinkerDelayed || th->function == (think_t) P_RemoveStaticThinkerDelayed)
        {
            th_next = getThinkerNext(th);
            th->function(th);
        }
        else
        {
            if (th->function)
                th->function(th);
            th_next = getThinkerNext(th);
        }
        th = th_next;
    }
}
