/*
 * kd_render.c - KeyStep Pro DOOM renderer: 128x64, 1-bit Atkinson dither.
 *
 * A vanilla DOOM style renderer (BSP walk with solid-seg clipping, wall
 * columns, sprites with drawseg clipping, masked mid textures, player
 * weapon sprites) working on the converted data blob:
 *  - textures, flats and sprites are 4-bit luminance at half resolution,
 *  - one light table (KDLIGHT: 34 colormaps x 16 codes -> 0..16) replaces
 *    the 34 palette colormaps,
 *  - pixels go into an 8-bit luminance buffer; at the end of the frame a
 *    contrast stretch (2nd..98th percentile, gamma 0.7) and Atkinson error
 *    diffusion produce the 1 kB SSD1306 page buffer.
 * Floors and ceilings are drawn per column while the walls are drawn (the
 * region vanilla would have marked in a visplane), so there are no
 * visplanes either.
 *
 * The renderer never writes game state (no validcount, no line flags), so
 * the frame rate cannot influence demo sync.
 *
 * Based on the DOOM source (id Software, GPL) as found in PrBoom / GBADoom /
 * next-hack's MCU ports. Part of the KeyStep Pro DOOM port, GPLv2.
 */
#include "doomstat.h"
#include "r_main.h"
#include "w_wad.h"
#include "z_zone.h"
#include "tables.h"
#include "m_bbox.h"
#include "p_pspr.h"
#include "lprintf.h"
#include "global_data.h"
#include <string.h>

#define SW SCREENWIDTH
#define SH SCREENHEIGHT
#define CENTERX (SW / 2)
#define CENTERY (SH / 2)
#define MAXDRAWSEGS 128
#define MAXOPENINGS (SW * 16)
#define MAXVISSPRITES 64
#define MAXSEGS 32
#define MINZ (FRACUNIT * 4)
#define HEIGHTBITS 12
#define HEIGHTUNIT (1 << HEIGHTBITS)
#define NOCOLUMN 0x7fff
// the vanilla light tables assume a 320 wide projection, ours is SW wide
#define LIGHTSCALE(s) ((int) (((uint32_t) (s) >> LIGHTSCALESHIFT) * 320 / SW))
// psprites: weapon bottom (sprite y 200) lands on the bottom row
#define PSP_CENTERY (SH - 100 * SW / 320)

fixed_t *textureheight;
short *texturetranslation;
short *flattranslation;
uint8_t kd_framebuffer[1024];
// 8-bit luminance frame; dithered into kd_framebuffer at the end of the frame
static uint8_t lumbuf[SH][SW];

// ---------------------------------------------------------------------------
// blob data
// ---------------------------------------------------------------------------
typedef struct
{
    int16_t w2, h2;         // half resolution size
    int16_t w, h;           // original size
    uint8_t flags;          // bit 0: masked (posts)
    uint8_t pad0;
    uint16_t pad1;
    uint32_t dataofs;       // relative to KDTEXDAT
} kd_texdir_t;

typedef struct
{
    int16_t lump[8];
    uint8_t flip;
    uint8_t rotate;
} kd_sprframe_t;

typedef struct
{
    int16_t w2, h2;         // half resolution size
    int16_t leftoffset, topoffset;      // full resolution
    uint16_t colofs[1];     // [w2], relative to the patch start
} kd_patch_t;

static const kd_texdir_t *texdir;
static const uint8_t *texdat;
static const char *texnames;
static const char *flatnames;
static const uint8_t *flatdat;
static const uint16_t *sprdef;      // first, count per sprite number
static const kd_sprframe_t *sprframes;
static const uint32_t *sprofs;
static const uint8_t *sprdat;
static const uint8_t *lighttab;     // [34][16] -> 0..16

static int find_name8(const char *table, int count, const char *name)
{
    char n8[8];
    memset(n8, 0, 8);
    for (int i = 0; i < 8 && name[i]; i++)
        n8[i] = (name[i] >= 'a' && name[i] <= 'z') ? name[i] - 32 : name[i];
    for (int i = 0; i < count; i++)
        if (!memcmp(table + 8 * i, n8, 8))
            return i;
    return -1;
}

int R_CheckTextureNumForName(const char *name)
{
    if (name[0] == '-')
        return 0;
    return find_name8(texnames, _g->numtextures, name);
}

int R_TextureNumForName(const char *name)
{
    int i = R_CheckTextureNumForName(name);
    return i < 0 ? 0 : i;
}

int R_CheckFlatNumForName(const char *name)
{
    return find_name8(flatnames, _g->numflats, name);
}

int R_FlatNumForName(const char *name)
{
    int i = find_name8(flatnames, _g->numflats, name);
    return i < 0 ? 0 : i;
}

static inline const kd_patch_t* sprite_patch(int lump)
{
    return (const kd_patch_t*) (sprdat + sprofs[lump]);
}

// ---------------------------------------------------------------------------
// view state
// ---------------------------------------------------------------------------
static fixed_t viewx, viewy, viewz;
static angle_t viewangle;
static fixed_t viewcos, viewsin;
static int extralight;
static int fixedcolormap;           // 0 = none, else colormap index
static const angle_t clipangle = 0x20080000;   // xtoviewangle[0] (gen_tables.py)
static const fixed_t centerxfrac = CENTERX << FRACBITS;
static const fixed_t centeryfrac = CENTERY << FRACBITS;
static const fixed_t projection = CENTERX << FRACBITS;
static const fixed_t pspritescale = FRACUNIT * SW / 320;
static const fixed_t pspriteiscale = FRACUNIT * 320 / SW;


// ---------------------------------------------------------------------------
// pixel output
// ---------------------------------------------------------------------------
static inline void put_pixel(int x, int y, int level)
{
    lumbuf[y][x] = level;
}

static inline const uint8_t* cmap_row(int cm)
{
    return lighttab + cm * 16;
}

// ---------------------------------------------------------------------------
// column drawing
// ---------------------------------------------------------------------------
typedef struct
{
    int x, yl, yh;
    fixed_t iscale;         // full resolution texels per pixel
    fixed_t texturemid;     // full resolution
    const uint8_t *light;   // 16 entries
} colvars_t;

// solid texture column: h2 half resolution texels, nibble packed, wraps vertically
static void draw_solid_column(const colvars_t *dc, const uint8_t *src, int h2)
{
    int y = dc->yl;
    if (y > dc->yh)
        return;
    fixed_t frac = dc->texturemid + (y - CENTERY) * dc->iscale;
    const uint8_t *light = dc->light;
    uint8_t *p = &lumbuf[y][dc->x];
    int pow2 = !(h2 & (h2 - 1));
    for (; y <= dc->yh; y++, frac += dc->iscale)
    {
        int t = frac >> (FRACBITS + 1);
        if (pow2)
            t &= h2 - 1;
        else
        {
            t %= h2;
            if (t < 0)
                t += h2;
        }
        int lum = (src[t >> 1] >> ((t & 1) << 2)) & 15;
        *p = light[lum];
        p += SW;
    }
}

// masked column: posts of (top, len, nibbles), clipped to [ceil+1, floor-1]
static fixed_t spryscale, sprtopscreen;
static void draw_masked_column(colvars_t *dc, const uint8_t *col, int ceilclip, int floorclip, boolean fuzz)
{

    while (*col != 0xff)
    {
        int top = col[0], len = col[1];
        const uint8_t *src = col + 2;
        fixed_t topscreen = sprtopscreen + spryscale * (top * 2);
        fixed_t bottomscreen = topscreen + spryscale * (len * 2);
        int yl = (topscreen + FRACUNIT - 1) >> FRACBITS;
        int yh = (bottomscreen - 1) >> FRACBITS;
        if (yh >= floorclip)
            yh = floorclip - 1;
        if (yl <= ceilclip)
            yl = ceilclip + 1;
        if (yl < 0)
            yl = 0;
        if (yh >= SH)
            yh = SH - 1;
        if (yl <= yh)
        {
            // full resolution texel position inside this post
            fixed_t frac = FixedMul((yl << FRACBITS) - topscreen, dc->iscale);
            for (int y = yl; y <= yh; y++, frac += dc->iscale)
            {
                int t = frac >> (FRACBITS + 1);
                if (t < 0)
                    t = 0;
                else if (t >= len)
                    t = len - 1;
                if (fuzz)
                {
                    // spectre / invisibility: darken a sparse pattern
                    if (((dc->x ^ y) & 1) == 0)
                        put_pixel(dc->x, y, 0);
                    continue;
                }
                int lum = (src[t >> 1] >> ((t & 1) << 2)) & 15;
                put_pixel(dc->x, y, dc->light[lum]);
            }
        }
        col += 2 + ((len + 1) >> 1);
    }
}

// ---------------------------------------------------------------------------
// textures
// ---------------------------------------------------------------------------
static inline int tex_column_index(const kd_texdir_t *t, int texcol)
{
    int c = texcol >> 1;
    int w2 = t->w2;
    if (!(w2 & (w2 - 1)))
        return c & (w2 - 1);
    c %= w2;
    return c < 0 ? c + w2 : c;
}

static void draw_wall_column(colvars_t *dc, int texnum, int texcol)
{
    const kd_texdir_t *t = &texdir[texnum];
    if (!t->w2)
        return;
    int c = tex_column_index(t, texcol);
    const uint8_t *base = texdat + t->dataofs;
    if (t->flags & 1)
    {
        // masked texture used as a wall: draw its posts (holes stay as they are)
        const uint16_t *colofs = (const uint16_t*) base;
        spryscale = FixedDiv(FRACUNIT, dc->iscale);
        sprtopscreen = centeryfrac - FixedMul(dc->texturemid, spryscale);
        draw_masked_column(dc, base + colofs[c], dc->yl - 1, dc->yh + 1, false);
    }
    else
    {
        draw_solid_column(dc, base + c * ((t->h2 + 1) >> 1), t->h2);
    }
}

// ---------------------------------------------------------------------------
// floors / ceilings / sky, one column piece at a time
// ---------------------------------------------------------------------------
typedef struct
{
    fixed_t height;
    int picnum;
    int lightlevel;
} planeinfo_t;

static planeinfo_t floorinfo, ceilinginfo;

static void draw_sky_column(int x, int yl, int yh)
{
    const kd_texdir_t *t = &texdir[_g->skytexture];
    if (!t->w2 || (t->flags & 1))
        return;
    int angle = (viewangle + xtoviewangle[x]) >> 22;      // ANGLETOSKYSHIFT, 1024 per turn
    int c = tex_column_index(t, angle);
    colvars_t dc;
    dc.x = x;
    dc.yl = yl;
    dc.yh = yh;
    dc.iscale = 2 * FRACUNIT;               // the 256 x 128 sky over a 90 degree view
    dc.texturemid = 64 * FRACUNIT;
    dc.light = cmap_row(fixedcolormap ? fixedcolormap : 0);
    const uint8_t *base = texdat + t->dataofs;
    draw_solid_column(&dc, base + c * ((t->h2 + 1) >> 1), t->h2);
}

static void draw_plane_column(const planeinfo_t *pl, int x, int yl, int yh)
{
    if (yl > yh)
        return;
    if (pl->picnum == _g->skyflatnum)
    {
        draw_sky_column(x, yl, yh);
        return;
    }
    int pic = flattranslation[pl->picnum];
    const uint8_t *flat = flatdat + pic * 512;
    fixed_t planeheight = D_abs(pl->height - viewz);
    angle_t angle = (viewangle + xtoviewangle[x]) >> ANGLETOFINESHIFT;
    fixed_t cosd = FixedMul(finecosine[angle], distscale[x]);
    fixed_t sind = FixedMul(finesine[angle], distscale[x]);
    int light = (pl->lightlevel >> LIGHTSEGSHIFT) + extralight;
    if (light >= LIGHTLEVELS)
        light = LIGHTLEVELS - 1;
    if (light < 0)
        light = 0;
    const uint8_t *zl = zlight[light];
    for (int y = yl; y <= yh; y++)
    {
        fixed_t distance = FixedMul(planeheight, yslope[y]);
        fixed_t xfrac = viewx + FixedMul(cosd, distance);
        fixed_t yfrac = -viewy - FixedMul(sind, distance);
        int fx = (xfrac >> (FRACBITS + 1)) & 31;
        int fy = (yfrac >> (FRACBITS + 1)) & 31;
        int lum = (flat[(fy << 4) + (fx >> 1)] >> ((fx & 1) << 2)) & 15;
        int cm;
        if (fixedcolormap)
            cm = fixedcolormap;
        else
        {
            unsigned index = (unsigned) distance >> LIGHTZSHIFT;
            if (index >= MAXLIGHTZ)
                index = MAXLIGHTZ - 1;
            cm = zl[index];
        }
        int level = lighttab[cm * 16 + lum];
        lumbuf[y][x] = level;
    }
}

// ---------------------------------------------------------------------------
// clipping state
// ---------------------------------------------------------------------------
typedef struct
{
    int first, last;
} cliprange_t;

typedef struct
{
    const seg_t *curline;
    fixed_t scale1, scale2, scalestep;
    fixed_t bsilheight, tsilheight;
    short *sprtopclip, *sprbottomclip;      // adjusted so [x1] is the first value
    short *maskedtexturecol;
    short x1, x2;
    uint8_t silhouette;
} drawseg_t;

typedef struct
{
    fixed_t gx, gy, gz, gzt;
    fixed_t startfrac, scale, xiscale, texturemid;
    short lump;
    short x1, x2;
    uint8_t cm;             // colormap, 255 = fuzz
    uint8_t psprite;
} vissprite_t;

static cliprange_t solidsegs[MAXSEGS], *newend;
static drawseg_t drawsegs[MAXDRAWSEGS], *ds_p;
static short openings[MAXOPENINGS], *lastopening;
static short floorclip[SW], ceilingclip[SW];
static short screenheightarray[SW], negonearray[SW];
static vissprite_t vissprites[MAXVISSPRITES], *vissprite_p;
static uint8_t sectordone[(1024 + 7) / 8];      // sprites already added (per frame)

static const seg_t *curline;
static const side_t *sidedef;
static const line_t *linedef;
static sector_t *frontsector, *backsector;
static angle_t rw_angle1;

// ---------------------------------------------------------------------------
// walls (r_segs.c)
// ---------------------------------------------------------------------------
static angle_t rw_normalangle, rw_centerangle;
static fixed_t rw_distance, rw_offset, rw_scale, rw_scalestep;
static int rw_x, rw_stopx;
static int worldtop, worldbottom, worldhigh, worldlow;
static fixed_t pixhigh, pixlow, pixhighstep, pixlowstep;
static fixed_t topfrac, topstep, bottomfrac, bottomstep;
static boolean segtextured, markfloor, markceiling, maskedtexture;
static int toptexture, bottomtexture, midtexture;
static fixed_t rw_midtexturemid, rw_toptexturemid, rw_bottomtexturemid;
static const uint8_t *walllights;
static short *maskedtexturecol;

static inline int side_top(const line_t *line, const side_t *side)
{
    if (!line->const_special || side == &_g->sides[line->sidenum[1]])
        return side->toptexture;
    return _g->switch_texture_top[_g->linesChangeableTextureIndex[line->lineno]];
}
static inline int side_mid(const line_t *line, const side_t *side)
{
    if (!line->const_special || side == &_g->sides[line->sidenum[1]])
        return side->midtexture;
    return _g->switch_texture_mid[_g->linesChangeableTextureIndex[line->lineno]];
}
static inline int side_bot(const line_t *line, const side_t *side)
{
    if (!line->const_special || side == &_g->sides[line->sidenum[1]])
        return side->bottomtexture;
    return _g->switch_texture_bot[_g->linesChangeableTextureIndex[line->lineno]];
}

static fixed_t R_PointToDist(fixed_t x, fixed_t y)
{
    fixed_t dx = D_abs(x - viewx);
    fixed_t dy = D_abs(y - viewy);
    if (dy > dx)
    {
        fixed_t t = dx;
        dx = dy;
        dy = t;
    }
    if (!dx)
        return 0;
    angle_t angle = (tantoangle[FixedDiv(dy, dx) >> DBITS] + ANG90) >> ANGLETOFINESHIFT;
    return FixedDiv(dx, finesine[angle]);
}

static fixed_t R_ScaleFromGlobalAngle(angle_t visangle)
{
    int anglea = ANG90 + (visangle - viewangle);
    int angleb = ANG90 + (visangle - rw_normalangle);
    int den = FixedMul(rw_distance, finesine[(angle_t) anglea >> ANGLETOFINESHIFT]);
    fixed_t num = FixedMul(projection, finesine[(angle_t) angleb >> ANGLETOFINESHIFT]);
    fixed_t scale;
    if (den > num >> 16)
    {
        scale = FixedDiv(num, den);
        if (scale > 64 * FRACUNIT)
            scale = 64 * FRACUNIT;
        else if (scale < 256)
            scale = 256;
    }
    else
        scale = 64 * FRACUNIT;
    return scale;
}

static inline const uint8_t* wall_light(fixed_t scale)
{
    if (fixedcolormap)
        return cmap_row(fixedcolormap);
    int index = LIGHTSCALE(scale);
    if (index >= MAXLIGHTSCALE)
        index = MAXLIGHTSCALE - 1;
    return cmap_row(walllights[index]);
}

static void R_RenderSegLoop(void)
{
    colvars_t dc;
    int texturecolumn = 0;
    for (; rw_x < rw_stopx; rw_x++)
    {
        int yl = (topfrac + HEIGHTUNIT - 1) >> HEIGHTBITS;
        if (yl < ceilingclip[rw_x] + 1)
            yl = ceilingclip[rw_x] + 1;

        if (markceiling)
        {
            int top = ceilingclip[rw_x] + 1;
            int bottom = yl - 1;
            if (bottom >= floorclip[rw_x])
                bottom = floorclip[rw_x] - 1;
            if (top <= bottom)
                draw_plane_column(&ceilinginfo, rw_x, top, bottom);
        }

        int yh = bottomfrac >> HEIGHTBITS;
        if (yh >= floorclip[rw_x])
            yh = floorclip[rw_x] - 1;

        if (markfloor)
        {
            int top = yh + 1;
            int bottom = floorclip[rw_x] - 1;
            if (top <= ceilingclip[rw_x])
                top = ceilingclip[rw_x] + 1;
            if (top <= bottom)
                draw_plane_column(&floorinfo, rw_x, top, bottom);
        }

        if (segtextured)
        {
            angle_t angle = (rw_centerangle + xtoviewangle[rw_x]) >> ANGLETOFINESHIFT;
            texturecolumn = (rw_offset - FixedMul(finetangent[angle], rw_distance)) >> FRACBITS;
            dc.light = wall_light(rw_scale);
            dc.x = rw_x;
            dc.iscale = 0xffffffffu / (unsigned) rw_scale;
        }

        if (midtexture)
        {
            dc.yl = yl;
            dc.yh = yh;
            dc.texturemid = rw_midtexturemid;
            draw_wall_column(&dc, midtexture, texturecolumn);
            ceilingclip[rw_x] = SH;
            floorclip[rw_x] = -1;
        }
        else
        {
            if (toptexture)
            {
                int mid = pixhigh >> HEIGHTBITS;
                pixhigh += pixhighstep;
                if (mid >= floorclip[rw_x])
                    mid = floorclip[rw_x] - 1;
                if (mid >= yl)
                {
                    dc.yl = yl;
                    dc.yh = mid;
                    dc.texturemid = rw_toptexturemid;
                    draw_wall_column(&dc, toptexture, texturecolumn);
                    ceilingclip[rw_x] = mid;
                }
                else
                    ceilingclip[rw_x] = yl - 1;
            }
            else if (markceiling)
                ceilingclip[rw_x] = yl - 1;

            if (bottomtexture)
            {
                int mid = (pixlow + HEIGHTUNIT - 1) >> HEIGHTBITS;
                pixlow += pixlowstep;
                if (mid <= ceilingclip[rw_x])
                    mid = ceilingclip[rw_x] + 1;
                if (mid <= yh)
                {
                    dc.yl = mid;
                    dc.yh = yh;
                    dc.texturemid = rw_bottomtexturemid;
                    draw_wall_column(&dc, bottomtexture, texturecolumn);
                    floorclip[rw_x] = mid;
                }
                else
                    floorclip[rw_x] = yh + 1;
            }
            else if (markfloor)
                floorclip[rw_x] = yh + 1;

            if (maskedtexture)
                maskedtexturecol[rw_x] = texturecolumn;
        }
        rw_scale += rw_scalestep;
        topfrac += topstep;
        bottomfrac += bottomstep;
    }
}

static void R_StoreWallRange(int start, int stop)
{
    if (ds_p == &drawsegs[MAXDRAWSEGS])
        return;
    if (lastopening + 3 * SW > &openings[MAXOPENINGS])
        return;     // out of openings: drop the seg rather than overflow

    sidedef = &_g->sides[curline->sidenum];
    linedef = &_g->lines[curline->linenum];

    rw_normalangle = curline->angle + ANG90;
    angle_t offsetangle = D_abs((int) (rw_normalangle - rw_angle1));
    if (offsetangle > ANG90)
        offsetangle = ANG90;
    angle_t distangle = ANG90 - offsetangle;
    fixed_t hyp = R_PointToDist(curline->v1.x, curline->v1.y);
    fixed_t sineval = finesine[distangle >> ANGLETOFINESHIFT];
    rw_distance = FixedMul(hyp, sineval);

    ds_p->x1 = rw_x = start;
    ds_p->x2 = stop;
    ds_p->curline = curline;
    rw_stopx = stop + 1;

    ds_p->scale1 = rw_scale = R_ScaleFromGlobalAngle(viewangle + xtoviewangle[start]);
    if (stop > start)
    {
        ds_p->scale2 = R_ScaleFromGlobalAngle(viewangle + xtoviewangle[stop]);
        ds_p->scalestep = rw_scalestep = (ds_p->scale2 - rw_scale) / (stop - start);
    }
    else
    {
        ds_p->scale2 = ds_p->scale1;
        ds_p->scalestep = rw_scalestep = 0;
    }

    worldtop = frontsector->ceilingheight - viewz;
    worldbottom = frontsector->floorheight - viewz;

    midtexture = toptexture = bottomtexture = maskedtexture = 0;
    ds_p->maskedtexturecol = NULL;

    if (!backsector)
    {
        int mt = side_mid(linedef, sidedef);
        midtexture = texturetranslation[mt];
        markfloor = markceiling = true;
        if (linedef->flags & ML_DONTPEGBOTTOM)
            rw_midtexturemid = frontsector->floorheight + textureheight[mt] - viewz;
        else
            rw_midtexturemid = worldtop;
        rw_midtexturemid += sidedef->rowoffset << FRACBITS;

        ds_p->silhouette = SIL_BOTH;
        ds_p->sprtopclip = screenheightarray;
        ds_p->sprbottomclip = negonearray;
        ds_p->bsilheight = INT_MAX;
        ds_p->tsilheight = INT_MIN;
    }
    else
    {
        ds_p->sprtopclip = ds_p->sprbottomclip = NULL;
        ds_p->silhouette = 0;

        if (frontsector->floorheight > backsector->floorheight)
        {
            ds_p->silhouette = SIL_BOTTOM;
            ds_p->bsilheight = frontsector->floorheight;
        }
        else if (backsector->floorheight > viewz)
        {
            ds_p->silhouette = SIL_BOTTOM;
            ds_p->bsilheight = INT_MAX;
        }

        if (frontsector->ceilingheight < backsector->ceilingheight)
        {
            ds_p->silhouette |= SIL_TOP;
            ds_p->tsilheight = frontsector->ceilingheight;
        }
        else if (backsector->ceilingheight < viewz)
        {
            ds_p->silhouette |= SIL_TOP;
            ds_p->tsilheight = INT_MIN;
        }

        if (backsector->ceilingheight <= frontsector->floorheight)
        {
            ds_p->sprbottomclip = negonearray;
            ds_p->bsilheight = INT_MAX;
            ds_p->silhouette |= SIL_BOTTOM;
        }

        if (backsector->floorheight >= frontsector->ceilingheight)
        {
            ds_p->sprtopclip = screenheightarray;
            ds_p->tsilheight = INT_MIN;
            ds_p->silhouette |= SIL_TOP;
        }

        worldhigh = backsector->ceilingheight - viewz;
        worldlow = backsector->floorheight - viewz;

        // hack to allow height changes in outdoor areas
        if (frontsector->ceilingpic == _g->skyflatnum && backsector->ceilingpic == _g->skyflatnum)
            worldtop = worldhigh;

        markfloor = worldlow != worldbottom || backsector->floorpic != frontsector->floorpic || backsector->lightlevel != frontsector->lightlevel;
        markceiling = worldhigh != worldtop || backsector->ceilingpic != frontsector->ceilingpic || backsector->lightlevel != frontsector->lightlevel;

        if (backsector->ceilingheight <= frontsector->floorheight || backsector->floorheight >= frontsector->ceilingheight)
            markceiling = markfloor = true;     // closed door

        if (worldhigh < worldtop)
        {
            int tt = side_top(linedef, sidedef);
            toptexture = texturetranslation[tt];
            if (linedef->flags & ML_DONTPEGTOP)
                rw_toptexturemid = worldtop;
            else
                rw_toptexturemid = backsector->ceilingheight + textureheight[tt] - viewz;
        }
        if (worldlow > worldbottom)
        {
            bottomtexture = texturetranslation[side_bot(linedef, sidedef)];
            if (linedef->flags & ML_DONTPEGBOTTOM)
                rw_bottomtexturemid = worldtop;
            else
                rw_bottomtexturemid = worldlow;
        }
        rw_toptexturemid += sidedef->rowoffset << FRACBITS;
        rw_bottomtexturemid += sidedef->rowoffset << FRACBITS;

        if (side_mid(linedef, sidedef))
        {
            maskedtexture = true;
            ds_p->maskedtexturecol = maskedtexturecol = lastopening - rw_x;
            lastopening += rw_stopx - rw_x;
        }
    }

    segtextured = midtexture | toptexture | bottomtexture | maskedtexture;

    if (segtextured)
    {
        offsetangle = rw_normalangle - rw_angle1;
        if (offsetangle > ANG180)
            offsetangle = -offsetangle;
        if (offsetangle > ANG90)
            offsetangle = ANG90;
        sineval = finesine[offsetangle >> ANGLETOFINESHIFT];
        rw_offset = FixedMul(hyp, sineval);
        if (rw_normalangle - rw_angle1 < ANG180)
            rw_offset = -rw_offset;
        rw_offset += (_g->textureoffsets[curline->sidenum] << FRACBITS) + curline->offset;
        rw_centerangle = ANG90 + viewangle - rw_normalangle;

        int lightnum = (frontsector->lightlevel >> LIGHTSEGSHIFT) + extralight;
        if (curline->v1.y == curline->v2.y)
            lightnum--;
        else if (curline->v1.x == curline->v2.x)
            lightnum++;
        if (lightnum < 0)
            lightnum = 0;
        if (lightnum >= LIGHTLEVELS)
            lightnum = LIGHTLEVELS - 1;
        walllights = scalelight[lightnum];
    }

    // a floor / ceiling plane on the wrong side of the view plane is invisible
    if (frontsector->floorheight >= viewz)
        markfloor = false;
    if (frontsector->ceilingheight <= viewz && frontsector->ceilingpic != _g->skyflatnum)
        markceiling = false;

    worldtop >>= 4;
    worldbottom >>= 4;

    topstep = -FixedMul(rw_scalestep, worldtop);
    topfrac = (centeryfrac >> 4) - FixedMul(worldtop, rw_scale);

    bottomstep = -FixedMul(rw_scalestep, worldbottom);
    bottomfrac = (centeryfrac >> 4) - FixedMul(worldbottom, rw_scale);

    if (backsector)
    {
        worldhigh >>= 4;
        worldlow >>= 4;
        if (worldhigh < worldtop)
        {
            pixhigh = (centeryfrac >> 4) - FixedMul(worldhigh, rw_scale);
            pixhighstep = -FixedMul(rw_scalestep, worldhigh);
        }
        if (worldlow > worldbottom)
        {
            pixlow = (centeryfrac >> 4) - FixedMul(worldlow, rw_scale);
            pixlowstep = -FixedMul(rw_scalestep, worldlow);
        }
    }

    R_RenderSegLoop();

    // save sprite clipping info
    if (((ds_p->silhouette & SIL_TOP) || maskedtexture) && !ds_p->sprtopclip)
    {
        memcpy(lastopening, ceilingclip + start, sizeof(short) * (rw_stopx - start));
        ds_p->sprtopclip = lastopening - start;
        lastopening += rw_stopx - start;
    }
    if (((ds_p->silhouette & SIL_BOTTOM) || maskedtexture) && !ds_p->sprbottomclip)
    {
        memcpy(lastopening, floorclip + start, sizeof(short) * (rw_stopx - start));
        ds_p->sprbottomclip = lastopening - start;
        lastopening += rw_stopx - start;
    }
    if (maskedtexture && !(ds_p->silhouette & SIL_TOP))
    {
        ds_p->silhouette |= SIL_TOP;
        ds_p->tsilheight = INT_MIN;
    }
    if (maskedtexture && !(ds_p->silhouette & SIL_BOTTOM))
    {
        ds_p->silhouette |= SIL_BOTTOM;
        ds_p->bsilheight = INT_MAX;
    }
    ds_p++;
}

// ---------------------------------------------------------------------------
// solid segs (r_bsp.c)
// ---------------------------------------------------------------------------
static void R_ClipSolidWallSegment(int first, int last)
{
    cliprange_t *next, *start;

    start = solidsegs;
    while (start->last < first - 1)
        start++;

    if (first < start->first)
    {
        if (last < start->first - 1)
        {
            // post is entirely visible (above start), so insert a new clippost
            R_StoreWallRange(first, last);
            if (newend >= &solidsegs[MAXSEGS])
                return;
            next = newend;
            newend++;
            while (next != start)
            {
                *next = *(next - 1);
                next--;
            }
            next->first = first;
            next->last = last;
            return;
        }
        // there is a fragment above *start
        R_StoreWallRange(first, start->first - 1);
        start->first = first;
    }

    if (last <= start->last)
        return;

    next = start;
    while (last >= (next + 1)->first - 1)
    {
        R_StoreWallRange(next->last + 1, (next + 1)->first - 1);
        next++;
        if (last <= next->last)
        {
            start->last = next->last;
            goto crunch;
        }
    }
    R_StoreWallRange(next->last + 1, last);
    start->last = last;

crunch:
    if (next == start)
        return;
    while (next++ != newend)
        *++start = *next;
    newend = start + 1;
}

static void R_ClipPassWallSegment(int first, int last)
{
    cliprange_t *start = solidsegs;
    while (start->last < first - 1)
        start++;

    if (first < start->first)
    {
        if (last < start->first - 1)
        {
            R_StoreWallRange(first, last);
            return;
        }
        R_StoreWallRange(first, start->first - 1);
    }

    if (last <= start->last)
        return;

    while (last >= (start + 1)->first - 1)
    {
        R_StoreWallRange(start->last + 1, (start + 1)->first - 1);
        start++;
        if (last <= start->last)
            return;
    }
    R_StoreWallRange(start->last + 1, last);
}

static angle_t R_PointToAngle(fixed_t x, fixed_t y)
{
    return R_PointToAngle2(viewx, viewy, x, y);
}

static void R_AddLine(const seg_t *line)
{
    int x1, x2;
    angle_t angle1, angle2, span, tspan;

    curline = line;
    angle1 = R_PointToAngle(line->v1.x, line->v1.y);
    angle2 = R_PointToAngle(line->v2.x, line->v2.y);

    span = angle1 - angle2;
    if (span >= ANG180)
        return;

    rw_angle1 = angle1;
    angle1 -= viewangle;
    angle2 -= viewangle;

    tspan = angle1 + clipangle;
    if (tspan > 2 * clipangle)
    {
        tspan -= 2 * clipangle;
        if (tspan >= span)
            return;
        angle1 = clipangle;
    }
    tspan = clipangle - angle2;
    if (tspan > 2 * clipangle)
    {
        tspan -= 2 * clipangle;
        if (tspan >= span)
            return;
        angle2 = -clipangle;
    }

    angle1 = (angle1 + ANG90) >> ANGLETOFINESHIFT;
    angle2 = (angle2 + ANG90) >> ANGLETOFINESHIFT;
    x1 = viewangletox[angle1];
    x2 = viewangletox[angle2];

    if (x1 >= x2)
        return;

    backsector = SG_BACKSECTOR(line);

    if (!backsector)
        goto clipsolid;

    if (backsector->ceilingheight <= frontsector->floorheight || backsector->floorheight >= frontsector->ceilingheight)
        goto clipsolid;

    if (backsector->ceilingheight != frontsector->ceilingheight || backsector->floorheight != frontsector->floorheight)
        goto clippass;

    // reject empty lines used for triggers and special events
    if (backsector->ceilingpic == frontsector->ceilingpic && backsector->floorpic == frontsector->floorpic
            && backsector->lightlevel == frontsector->lightlevel && _g->sides[line->sidenum].midtexture == 0)
        return;

clippass:
    R_ClipPassWallSegment(x1, x2 - 1);
    return;

clipsolid:
    R_ClipSolidWallSegment(x1, x2 - 1);
}

static const int checkcoord[12][4] =
{
    { 3, 0, 2, 1 },
    { 3, 0, 2, 0 },
    { 3, 1, 2, 0 },
    { 0 },
    { 2, 0, 2, 1 },
    { 0, 0, 0, 0 },
    { 3, 1, 3, 0 },
    { 0 },
    { 2, 0, 3, 1 },
    { 2, 1, 3, 1 },
    { 2, 1, 3, 0 }
};

static boolean R_CheckBBox(const short *bspcoord)
{
    int boxx, boxy, boxpos;
    fixed_t x1, y1, x2, y2;
    angle_t angle1, angle2, span, tspan;
    cliprange_t *start;
    int sx1, sx2;

    if (viewx <= ((fixed_t) bspcoord[BOXLEFT] << FRACBITS))
        boxx = 0;
    else if (viewx < ((fixed_t) bspcoord[BOXRIGHT] << FRACBITS))
        boxx = 1;
    else
        boxx = 2;

    if (viewy >= ((fixed_t) bspcoord[BOXTOP] << FRACBITS))
        boxy = 0;
    else if (viewy > ((fixed_t) bspcoord[BOXBOTTOM] << FRACBITS))
        boxy = 1;
    else
        boxy = 2;

    boxpos = (boxy << 2) + boxx;
    if (boxpos == 5)
        return true;

    x1 = (fixed_t) bspcoord[checkcoord[boxpos][0]] << FRACBITS;
    y1 = (fixed_t) bspcoord[checkcoord[boxpos][1]] << FRACBITS;
    x2 = (fixed_t) bspcoord[checkcoord[boxpos][2]] << FRACBITS;
    y2 = (fixed_t) bspcoord[checkcoord[boxpos][3]] << FRACBITS;

    angle1 = R_PointToAngle(x1, y1) - viewangle;
    angle2 = R_PointToAngle(x2, y2) - viewangle;

    span = angle1 - angle2;
    if (span >= ANG180)
        return true;

    tspan = angle1 + clipangle;
    if (tspan > 2 * clipangle)
    {
        tspan -= 2 * clipangle;
        if (tspan >= span)
            return false;
        angle1 = clipangle;
    }
    tspan = clipangle - angle2;
    if (tspan > 2 * clipangle)
    {
        tspan -= 2 * clipangle;
        if (tspan >= span)
            return false;
        angle2 = -clipangle;
    }

    angle1 = (angle1 + ANG90) >> ANGLETOFINESHIFT;
    angle2 = (angle2 + ANG90) >> ANGLETOFINESHIFT;
    sx1 = viewangletox[angle1];
    sx2 = viewangletox[angle2];

    if (sx1 == sx2)
        return false;
    sx2--;

    start = solidsegs;
    while (start->last < sx2)
        start++;

    if (sx1 >= start->first && sx2 <= start->last)
        return false;   // the clippost contains the new span

    return true;
}

// ---------------------------------------------------------------------------
// sprites (r_things.c)
// ---------------------------------------------------------------------------
static const uint8_t *spritelights;

static void R_ProjectSprite(mobj_t *thing)
{
    fixed_t tr_x = thing->x - viewx;
    fixed_t tr_y = thing->y - viewy;
    fixed_t gxt = FixedMul(tr_x, viewcos);
    fixed_t gyt = -FixedMul(tr_y, viewsin);
    fixed_t tz = gxt - gyt;

    if (tz < MINZ)
        return;

    fixed_t xscale = FixedDiv(projection, tz);
    gxt = -FixedMul(tr_x, viewsin);
    gyt = FixedMul(tr_y, viewcos);
    fixed_t tx = -(gyt + gxt);

    if (D_abs(tx) > (tz << 2))
        return;

    if (thing->sprite >= NUMSPRITES)
        return;
    int first = sprdef[thing->sprite * 2], count = sprdef[thing->sprite * 2 + 1];
    int frame = thing->frame & FF_FRAMEMASK;
    if (frame >= count)
        return;     // sprite not in the blob
    const kd_sprframe_t *sprframe = &sprframes[first + frame];

    int lump, flip;
    if (sprframe->rotate)
    {
        angle_t ang = R_PointToAngle(thing->x, thing->y);
        angle_t mangle = (thing->flags & MF_STATIC) ? 0 : thing->angle;
        unsigned rot = (ang - mangle + (unsigned) (ANG45 / 2) * 9) >> 29;
        lump = sprframe->lump[rot];
        flip = (sprframe->flip >> rot) & 1;
    }
    else
    {
        lump = sprframe->lump[0];
        flip = sprframe->flip & 1;
    }

    const kd_patch_t *patch = sprite_patch(lump);
    fixed_t width = (fixed_t) (patch->w2 * 2) << FRACBITS;
    tx -= (fixed_t) patch->leftoffset << FRACBITS;
    int x1 = (centerxfrac + FixedMul(tx, xscale)) >> FRACBITS;
    if (x1 > SW - 1)
        return;
    tx += width;
    int x2 = ((centerxfrac + FixedMul(tx, xscale)) >> FRACBITS) - 1;
    if (x2 < 0)
        return;
    if (vissprite_p == &vissprites[MAXVISSPRITES])
        return;

    vissprite_t *vis = vissprite_p++;
    vis->psprite = 0;
    vis->scale = xscale;
    vis->gx = thing->x;
    vis->gy = thing->y;
    vis->gz = thing->z;
    vis->gzt = thing->z + ((fixed_t) patch->topoffset << FRACBITS);
    vis->texturemid = vis->gzt - viewz;
    vis->x1 = x1 < 0 ? 0 : x1;
    vis->x2 = x2 >= SW ? SW - 1 : x2;
    fixed_t iscale = FixedDiv(FRACUNIT, xscale);
    if (flip)
    {
        vis->startfrac = width - 1;
        vis->xiscale = -iscale;
    }
    else
    {
        vis->startfrac = 0;
        vis->xiscale = iscale;
    }
    if (vis->x1 > x1)
        vis->startfrac += vis->xiscale * (vis->x1 - x1);
    vis->lump = lump;

    if (thing->flags & MF_SHADOW)
        vis->cm = 255;
    else if (fixedcolormap)
        vis->cm = fixedcolormap;
    else if (thing->frame & FF_FULLBRIGHT)
        vis->cm = 0;
    else
    {
        int index = LIGHTSCALE(xscale);
        if (index >= MAXLIGHTSCALE)
            index = MAXLIGHTSCALE - 1;
        vis->cm = spritelights[index];
    }
}

static void R_AddSprites(sector_t *sec)
{
    int secnum = sec - _g->sectors;
    if (sectordone[secnum >> 3] & (1 << (secnum & 7)))
        return;
    sectordone[secnum >> 3] |= 1 << (secnum & 7);

    int lightnum = (sec->lightlevel >> LIGHTSEGSHIFT) + extralight;
    if (lightnum < 0)
        lightnum = 0;
    if (lightnum >= LIGHTLEVELS)
        lightnum = LIGHTLEVELS - 1;
    spritelights = scalelight[lightnum];

    for (mobj_t *thing = getSectorThingList(sec); thing; thing = getSNext(thing))
        R_ProjectSprite(thing);
}

static void R_DrawVisSprite(const vissprite_t *vis, const short *clipbot, const short *cliptop, fixed_t centery_frac)
{
    const kd_patch_t *patch = sprite_patch(vis->lump);
    colvars_t dc;
    dc.texturemid = vis->texturemid;
    dc.light = cmap_row(vis->cm == 255 ? 0 : vis->cm);
    fixed_t frac = vis->startfrac;
    spryscale = vis->scale;
    sprtopscreen = centery_frac - FixedMul(dc.texturemid, spryscale);
    // vertical texel step = 1 / vertical scale (same as the horizontal scale here)
    dc.iscale = FixedDiv(FRACUNIT, spryscale);
    for (dc.x = vis->x1; dc.x <= vis->x2; dc.x++, frac += vis->xiscale)
    {
        int texturecolumn = (frac >> FRACBITS) >> 1;
        if (texturecolumn < 0 || texturecolumn >= patch->w2)
            continue;
        const uint8_t *col = (const uint8_t*) patch + patch->colofs[texturecolumn];
        // sprites use the column y relative to their own center: shift by the
        // difference between the sprite center and the view center
        draw_masked_column(&dc, col, cliptop[dc.x], clipbot[dc.x], vis->cm == 255);
    }
}

static int R_PointOnSegSide(fixed_t x, fixed_t y, const seg_t *line)
{
    fixed_t lx = line->v1.x;
    fixed_t ly = line->v1.y;
    fixed_t ldx = line->v2.x - lx;
    fixed_t ldy = line->v2.y - ly;

    if (!ldx)
        return x <= lx ? ldy > 0 : ldy < 0;
    if (!ldy)
        return y <= ly ? ldx < 0 : ldx > 0;

    x -= lx;
    y -= ly;

    if ((ldy ^ ldx ^ x ^ y) < 0)
        return (ldy ^ x) < 0;

    return FixedMul(y, ldx >> FRACBITS) >= FixedMul(ldy >> FRACBITS, x);
}

static void R_RenderMaskedSegRange(drawseg_t *ds, int x1, int x2)
{
    curline = ds->curline;
    frontsector = SG_FRONTSECTOR(curline);
    backsector = SG_BACKSECTOR(curline);
    if (!backsector)
        return;
    const line_t *ld = &_g->lines[curline->linenum];
    const side_t *sd = &_g->sides[curline->sidenum];
    int mt = side_mid(ld, sd);
    int texnum = texturetranslation[mt];
    const kd_texdir_t *t = &texdir[texnum];
    if (!t->w2)
        return;

    int lightnum = (frontsector->lightlevel >> LIGHTSEGSHIFT) + extralight;
    if (curline->v1.y == curline->v2.y)
        lightnum--;
    else if (curline->v1.x == curline->v2.x)
        lightnum++;
    if (lightnum < 0)
        lightnum = 0;
    if (lightnum >= LIGHTLEVELS)
        lightnum = LIGHTLEVELS - 1;
    walllights = scalelight[lightnum];

    short *mtc = ds->maskedtexturecol;
    fixed_t step = ds->scalestep;
    spryscale = ds->scale1 + (x1 - ds->x1) * step;

    fixed_t texturemid;
    if (ld->flags & ML_DONTPEGBOTTOM)
    {
        texturemid = frontsector->floorheight > backsector->floorheight ? frontsector->floorheight : backsector->floorheight;
        texturemid = texturemid + textureheight[mt] - viewz;
    }
    else
    {
        texturemid = frontsector->ceilingheight < backsector->ceilingheight ? frontsector->ceilingheight : backsector->ceilingheight;
        texturemid = texturemid - viewz;
    }
    texturemid += sd->rowoffset << FRACBITS;

    const uint8_t *base = texdat + t->dataofs;
    colvars_t dc;
    for (dc.x = x1; dc.x <= x2; dc.x++, spryscale += step)
    {
        if (mtc[dc.x] == NOCOLUMN)
            continue;
        dc.light = wall_light(spryscale);
        dc.texturemid = texturemid;
        sprtopscreen = centeryfrac - FixedMul(texturemid, spryscale);
        dc.iscale = 0xffffffffu / (unsigned) spryscale;
        int c = tex_column_index(t, mtc[dc.x]);
        if (t->flags & 1)
        {
            const uint16_t *colofs = (const uint16_t*) base;
            draw_masked_column(&dc, base + colofs[c], ds->sprtopclip[dc.x], ds->sprbottomclip[dc.x], false);
        }
        else
        {
            // solid texture on a two sided line: one post, no vertical tiling (as vanilla)
            int yl = (sprtopscreen + FRACUNIT - 1) >> FRACBITS;
            int yh = (sprtopscreen + spryscale * t->h - 1) >> FRACBITS;
            if (yl <= ds->sprtopclip[dc.x])
                yl = ds->sprtopclip[dc.x] + 1;
            if (yh >= ds->sprbottomclip[dc.x])
                yh = ds->sprbottomclip[dc.x] - 1;
            if (yl < 0)
                yl = 0;
            if (yh >= SH)
                yh = SH - 1;
            dc.yl = yl;
            dc.yh = yh;
            draw_solid_column(&dc, base + c * ((t->h2 + 1) >> 1), t->h2);
        }
        mtc[dc.x] = NOCOLUMN;
    }
}

static void R_DrawSprite(vissprite_t *spr)
{
    short clipbot[SW], cliptop[SW];
    int x;

    for (x = spr->x1; x <= spr->x2; x++)
        clipbot[x] = cliptop[x] = -2;

    for (drawseg_t *ds = ds_p - 1; ds >= drawsegs; ds--)
    {
        if (ds->x1 > spr->x2 || ds->x2 < spr->x1 || (!ds->silhouette && !ds->maskedtexturecol))
            continue;

        int r1 = ds->x1 < spr->x1 ? spr->x1 : ds->x1;
        int r2 = ds->x2 > spr->x2 ? spr->x2 : ds->x2;
        fixed_t lowscale, scale;
        if (ds->scale1 > ds->scale2)
        {
            lowscale = ds->scale2;
            scale = ds->scale1;
        }
        else
        {
            lowscale = ds->scale1;
            scale = ds->scale2;
        }

        if (scale < spr->scale || (lowscale < spr->scale && !R_PointOnSegSide(spr->gx, spr->gy, ds->curline)))
        {
            if (ds->maskedtexturecol)
                R_RenderMaskedSegRange(ds, r1, r2);
            continue;
        }

        int silhouette = ds->silhouette;
        if (spr->gz >= ds->bsilheight)
            silhouette &= ~SIL_BOTTOM;
        if (spr->gzt <= ds->tsilheight)
            silhouette &= ~SIL_TOP;

        if (silhouette & SIL_BOTTOM)
            for (x = r1; x <= r2; x++)
                if (clipbot[x] == -2)
                    clipbot[x] = ds->sprbottomclip[x];
        if (silhouette & SIL_TOP)
            for (x = r1; x <= r2; x++)
                if (cliptop[x] == -2)
                    cliptop[x] = ds->sprtopclip[x];
    }

    for (x = spr->x1; x <= spr->x2; x++)
    {
        if (clipbot[x] == -2)
            clipbot[x] = SH;
        if (cliptop[x] == -2)
            cliptop[x] = -1;
    }
    R_DrawVisSprite(spr, clipbot, cliptop, centeryfrac);
}

static void R_DrawPSprite(const pspdef_t *psp, int lightnum)
{
    const state_t *st = psp->state;
    if (st->sprite >= NUMSPRITES)
        return;
    int first = sprdef[st->sprite * 2], count = sprdef[st->sprite * 2 + 1];
    int frame = st->frame & FF_FRAMEMASK;
    if (frame >= count)
        return;
    const kd_sprframe_t *sprframe = &sprframes[first + frame];
    int lump = sprframe->lump[0];
    int flip = sprframe->flip & 1;
    const kd_patch_t *patch = sprite_patch(lump);
    fixed_t width = (fixed_t) (patch->w2 * 2) << FRACBITS;

    fixed_t tx = psp->sx - 160 * FRACUNIT;
    tx -= (fixed_t) patch->leftoffset << FRACBITS;
    int x1 = (centerxfrac + FixedMul(tx, pspritescale)) >> FRACBITS;
    if (x1 > SW - 1)
        return;
    tx += width;
    int x2 = ((centerxfrac + FixedMul(tx, pspritescale)) >> FRACBITS) - 1;
    if (x2 < 0)
        return;

    vissprite_t vis;
    vis.psprite = 1;
    vis.texturemid = (100 << FRACBITS) + FRACUNIT / 2 - (psp->sy - ((fixed_t) patch->topoffset << FRACBITS));
    vis.x1 = x1 < 0 ? 0 : x1;
    vis.x2 = x2 >= SW ? SW - 1 : x2;
    vis.scale = pspritescale;
    if (flip)
    {
        vis.xiscale = -pspriteiscale;
        vis.startfrac = width - 1;
    }
    else
    {
        vis.xiscale = pspriteiscale;
        vis.startfrac = 0;
    }
    if (vis.x1 > x1)
        vis.startfrac += vis.xiscale * (vis.x1 - x1);
    vis.lump = lump;

    player_t *pl = &_g->player;
    if (pl->powers[pw_invisibility] > 4 * 32 || pl->powers[pw_invisibility] & 8)
        vis.cm = 255;
    else if (fixedcolormap)
        vis.cm = fixedcolormap;
    else if (st->frame & FF_FULLBRIGHT)
        vis.cm = 0;
    else
        vis.cm = scalelight[lightnum][MAXLIGHTSCALE - 1];
    R_DrawVisSprite(&vis, screenheightarray, negonearray, PSP_CENTERY << FRACBITS);
}

static void R_DrawMasked(void)
{
    // sort vissprites back to front (insertion sort, few sprites)
    int n = vissprite_p - vissprites;
    for (int i = 1; i < n; i++)
    {
        vissprite_t v = vissprites[i];
        int j = i - 1;
        while (j >= 0 && vissprites[j].scale > v.scale)
        {
            vissprites[j + 1] = vissprites[j];
            j--;
        }
        vissprites[j + 1] = v;
    }
    for (int i = 0; i < n; i++)
        R_DrawSprite(&vissprites[i]);

    // render any remaining masked mid textures
    for (drawseg_t *ds = ds_p - 1; ds >= drawsegs; ds--)
        if (ds->maskedtexturecol)
            R_RenderMaskedSegRange(ds, ds->x1, ds->x2);

    // player weapon
    player_t *pl = &_g->player;
    int lightnum = (getMobjSubsector(pl->mo)->sector->lightlevel >> LIGHTSEGSHIFT) + extralight;
    if (lightnum < 0)
        lightnum = 0;
    if (lightnum >= LIGHTLEVELS)
        lightnum = LIGHTLEVELS - 1;
    for (int i = 0; i < NUMPSPRITES; i++)
        if (pl->psprites[i].state)
            R_DrawPSprite(&pl->psprites[i], lightnum);
}

// ---------------------------------------------------------------------------
// BSP walk
// ---------------------------------------------------------------------------
static void R_Subsector(int num)
{
    subsector_t *sub = &_g->subsectors[num];
    frontsector = sub->sector;
    int count = sub->numlines;
    const seg_t *line = &_g->segs[sub->firstline];

    floorinfo.height = frontsector->floorheight;
    floorinfo.picnum = frontsector->floorpic;
    floorinfo.lightlevel = frontsector->lightlevel;
    ceilinginfo.height = frontsector->ceilingheight;
    ceilinginfo.picnum = frontsector->ceilingpic;
    ceilinginfo.lightlevel = frontsector->lightlevel;

    R_AddSprites(frontsector);

    while (count--)
        R_AddLine(line++);
}

static void R_RenderBSPNode(int bspnum)
{
    while (!(bspnum & NF_SUBSECTOR))
    {
        const mapnode_t *bsp = &nodes[bspnum];
        int side = R_PointOnSide(viewx, viewy, bsp);
        R_RenderBSPNode(bsp->children[side]);
        if (!R_CheckBBox(bsp->bbox[side ^ 1]))
            return;
        bspnum = bsp->children[side ^ 1];
    }
    R_Subsector(bspnum == -1 ? 0 : bspnum & ~NF_SUBSECTOR);
}

// ---------------------------------------------------------------------------
// public
// ---------------------------------------------------------------------------
void R_Init(void)
{
    int len;
    texdir = W_LumpByName("KDTEXDIR", &len);
    _g->numtextures = len / sizeof(kd_texdir_t);
    texnames = W_LumpByName("KDTEXNAM", NULL);
    texdat = W_LumpByName("KDTEXDAT", NULL);
    flatnames = W_LumpByName("KDFLTNAM", &len);
    _g->numflats = len / 8;
    flatdat = W_LumpByName("KDFLTDAT", NULL);
    sprdef = W_LumpByName("KDSPRDEF", NULL);
    sprframes = W_LumpByName("KDSPRFRM", NULL);
    sprofs = W_LumpByName("KDSPROFS", NULL);
    sprdat = W_LumpByName("KDSPRDAT", NULL);
    lighttab = W_LumpByName("KDLIGHT", NULL);
    if (!texdir || !texdat || !flatdat || !sprdef || !lighttab)
        I_Error("R_Init: blob is missing graphics");

    textureheight = Z_Malloc(_g->numtextures * sizeof(*textureheight), PU_STATIC, 0);
    texturetranslation = Z_Malloc((_g->numtextures + 1) * sizeof(*texturetranslation), PU_STATIC, 0);
    flattranslation = Z_Malloc((_g->numflats + 1) * sizeof(*flattranslation), PU_STATIC, 0);
    for (int i = 0; i < _g->numtextures; i++)
    {
        textureheight[i] = texdir[i].h << FRACBITS;
        texturetranslation[i] = i;
    }
    for (int i = 0; i < _g->numflats; i++)
        flattranslation[i] = i;
    for (int i = 0; i < SW; i++)
    {
        screenheightarray[i] = SH;
        negonearray[i] = -1;
    }
}

void R_SetupLevel(void)
{
}

//
// Frame output: per-frame contrast stretch (2nd..98th percentile -> 0..255,
// then gamma 0.7 via kd_gamma07[]) and Atkinson error diffusion into the
// 1-bpp SSD1306 page buffer.
//
static void R_DitherFrame(void)
{
    static uint16_t hist[256];
    static uint8_t lut[256];
    static int16_t err[3][SW + 4];      // rows y, y+1, y+2 (index x + 2)
    const int total = SW * SH;
    int i, lo = 0, hi = 255, acc;

    memset(hist, 0, sizeof(hist));
    const uint8_t *q = &lumbuf[0][0];
    for (i = 0; i < total; i++)
        hist[q[i]]++;
    for (acc = 0, i = 0; i < 256; i++)
    {
        acc += hist[i];
        if (acc > total * 2 / 100)
        {
            lo = i;
            break;
        }
    }
    for (acc = 0, i = 255; i >= 0; i--)
    {
        acc += hist[i];
        if (acc > total * 2 / 100)
        {
            hi = i;
            break;
        }
    }
    if (hi <= lo)
        hi = lo + 1;
    for (i = 0; i < 256; i++)
    {
        int v = i <= lo ? 0 : i >= hi ? 255 : (i - lo) * 255 / (hi - lo);
        lut[i] = kd_gamma07[v];
    }

    memset(err, 0, sizeof(err));
    memset(kd_framebuffer, 0, sizeof(kd_framebuffer));
    for (int y = 0; y < SH; y++)
    {
        int16_t *e0 = err[y % 3], *e1 = err[(y + 1) % 3], *e2 = err[(y + 2) % 3];
        uint8_t *src = lumbuf[y];
        uint8_t *dst = &kd_framebuffer[(y >> 3) << 7];
        uint8_t m = 1 << (y & 7);
        for (int x = 0; x < SW; x++)
        {
            int v = lut[src[x]] + e0[x + 2];
            int out = v >= 128 ? 255 : 0;
            if (out)
                dst[x] |= m;
            int e = (v - out) / 8;
            e0[x + 3] += e;
            e0[x + 4] += e;
            e1[x + 1] += e;
            e1[x + 2] += e;
            e1[x + 3] += e;
            e2[x + 2] += e;
        }
        memset(e0, 0, sizeof(err[0]));
    }
}

void R_RenderPlayerView(player_t *player)
{
    mobj_t *mo = player->mo;
    viewx = mo->x;
    viewy = mo->y;
    viewz = player->viewz;
    viewangle = mo->angle;
    extralight = player->extralight;
    viewsin = finesine[viewangle >> ANGLETOFINESHIFT];
    viewcos = finecosine[viewangle >> ANGLETOFINESHIFT];
    fixedcolormap = player->fixedcolormap;

    solidsegs[0].first = -0x7fffffff;
    solidsegs[0].last = -1;
    solidsegs[1].first = SW;
    solidsegs[1].last = 0x7fffffff;
    newend = solidsegs + 2;
    ds_p = drawsegs;
    lastopening = openings;
    vissprite_p = vissprites;
    for (int i = 0; i < SW; i++)
    {
        floorclip[i] = SH;
        ceilingclip[i] = -1;
    }
    memset(sectordone, 0, sizeof(sectordone));

    R_RenderBSPNode(numnodes - 1);
    R_DrawMasked();
    R_DitherFrame();
}
