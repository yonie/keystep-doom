/* The takeover intro, before DOOM boots (MIT): about six seconds of C64-demo style on every output the
 * KeyStep Pro has.
 *
 *   OLED   1. hacker terminal: "OVERRIDE KEYSTEP PRO" typed over glitch noise, blinking cursor
 *          2. raster bars behind a sine-wobbling KEYSTEP DOOM logo, scroller along the bottom
 *          3. DOOM's own screen melt into black, then the game
 *   LEDs   rainbow chase on the 16 RGB steps, a colour wave over the 39 key LEDs, cycling track colour
 *          bars, bouncing touch-strip bars, spinning encoder-ring dots; all cleared at the end
 *
 * LED packets go through the stock UI firmware's own handlers:
 * group 2 = knob ring dot (pos, pos), group 3 idx k = step k RGB, group 4 idx k = key LED k RGB,
 * group 5 idx 0..11 = track LEDs RGB, group 7 = strips, group 3/4/5 batches 0x50.. = colour + mask.
 */
#include <stdint.h>

void kd_plat_present(const uint8_t fb[1024]);
uint32_t kd_plat_ms(void);
void kd_led(uint8_t group, uint8_t idx, uint8_t a, uint8_t b, uint8_t c, uint8_t d);
void kd_hud_resend(void);

#define FPS_MS 50
#define T_TERM 32                                  /* frames: terminal */
#define T_DEMO 116                                 /* frames: logo, bars, scroller */
#define T_MELT 22                                  /* frames: melt */

static uint8_t fb[1024];
static uint32_t rng = 0x1D4C3B2Au;

static uint32_t rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

static const int8_t sine64[64] = {                 /* sin * 31, one period */
    0, 3, 6, 9, 12, 15, 17, 20, 22, 24, 26, 27, 29, 30, 30, 31, 31, 31, 30, 30, 29, 27, 26, 24, 22, 20,
    17, 15, 12, 9, 6, 3, 0, -3, -6, -9, -12, -15, -17, -20, -22, -24, -26, -27, -29, -30, -30, -31, -31,
    -31, -30, -30, -29, -27, -26, -24, -22, -20, -17, -15, -12, -9, -6, -3 };

static const uint8_t font5x7[26][5] = {          /* A..Z, column-major, LSB = top row */
    {0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},{0x7F,0x41,0x41,0x22,0x1C},
    {0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},{0x3E,0x41,0x49,0x49,0x7A},{0x7F,0x08,0x08,0x08,0x7F},
    {0x00,0x41,0x7F,0x41,0x00},{0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},{0x7F,0x09,0x09,0x09,0x06},
    {0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7F,0x01,0x01},
    {0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},{0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},
};
static const uint8_t glyph_gt[5] = {0x41, 0x22, 0x14, 0x08, 0x00};   /* '>' */
static const uint8_t glyph_dot[5] = {0x00, 0x60, 0x60, 0x00, 0x00};  /* '.' */
static const uint8_t glyph_dash[5] = {0x08, 0x08, 0x08, 0x08, 0x08}; /* '-' */

static void px(int x, int y)
{
    if (x >= 0 && x < 128 && y >= 0 && y < 64)
        fb[(y >> 3) * 128 + x] |= (uint8_t)(1u << (y & 7));
}

static void unpx(int x, int y)
{
    if (x >= 0 && x < 128 && y >= 0 && y < 64)
        fb[(y >> 3) * 128 + x] &= (uint8_t)~(1u << (y & 7));
}

static const uint8_t *glyph(char ch)
{
    if (ch >= 'A' && ch <= 'Z') return font5x7[ch - 'A'];
    if (ch == '>') return glyph_gt;
    if (ch == '.') return glyph_dot;
    if (ch == '-') return glyph_dash;
    return 0;
}

/* draw a character; clear=1 knocks it out of what is already there (text over bars) */
static void ch(char c, int x, int y, int sc, int clear)
{
    const uint8_t *g = glyph(c);
    if (!g) return;
    for (int cx = 0; cx < 5; cx++)
        for (int r = 0; r < 7; r++)
            if (g[cx] >> r & 1)
                for (int dx = 0; dx < sc; dx++)
                    for (int dy = 0; dy < sc; dy++) {
                        if (clear) unpx(x + cx * sc + dx, y + r * sc + dy);
                        else px(x + cx * sc + dx, y + r * sc + dy);
                    }
}

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

/* ---- LEDs ------------------------------------------------------------------------------------- */

static void hue(int h, uint8_t *r, uint8_t *g, uint8_t *b)   /* h 0..191 -> 0..0x7F RGB */
{
    h %= 192;
    int s = h % 64, up = s * 2, dn = 0x7F - s * 2;
    if (h < 64) { *r = (uint8_t)dn; *g = (uint8_t)up; *b = 0; }
    else if (h < 128) { *r = 0; *g = (uint8_t)dn; *b = (uint8_t)up; }
    else { *r = (uint8_t)up; *g = 0; *b = (uint8_t)dn; }
}

static void leds(int f)
{
    uint8_t r, g, b;
    for (int k = 0; k < 16; k++) {                 /* steps: rainbow chase with a white head */
        if (k == (f % 16)) { r = g = b = 0x7F; }
        else hue(f * 9 + k * 12, &r, &g, &b);
        kd_led(3, (uint8_t)k, r, g, b, 0);
    }
    for (int k = (f & 1); k < 39; k += 2) {        /* keys: a wave, half of them per frame */
        int v = sine64[(k * 3 + f * 4) & 63] + 31;  /* 0..62 */
        hue(k * 5 + f * 6, &r, &g, &b);
        kd_led(4, (uint8_t)k, (uint8_t)(r * v / 62), (uint8_t)(g * v / 62), (uint8_t)(b * v / 62), 0);
    }
    if ((f & 3) == 0)
        for (int k = 0; k < 12; k++) {             /* track LEDs */
            hue(f * 4 + k * 16, &r, &g, &b);
            kd_led(5, (uint8_t)k, r, g, b, 0);
        }
    int a = sine64[(f * 3) & 63] + 31, c = sine64[(f * 5 + 16) & 63] + 31;
    kd_led(7, 0, 0, (uint8_t)(a * 200 / 62), 0, 0);          /* mod strip: bouncing bar */
    kd_led(7, 1, 100, (uint8_t)(c * 200 / 62), 0, 0);        /* pitch strip: swinging from the centre */
    for (int k = 0; k < 5; k++) {                  /* encoder rings: dots spinning, each its own speed */
        uint8_t pos = (uint8_t)((f * (3 + k) * 4 + k * 25) & 127);
        kd_led(2, (uint8_t)k, pos, pos, 0, 0);
    }
}

static void leds_off(void)
{
    for (int bank = 0x50; bank <= 0x54; bank++) kd_led(4, (uint8_t)bank, 0, 0, 0, 0xFF);
    for (int k = 0; k < 12; k++) kd_led(5, (uint8_t)k, 0, 0, 0, 0);
    kd_led(3, 0x50, 0, 0, 0, 0xFF);
    kd_led(3, 0x51, 0, 0, 0, 0xFF);
    for (int k = 0; k < 5; k++) kd_led(2, (uint8_t)k, 0, 0, 0, 0);
}

/* ---- OLED --------------------------------------------------------------------------------------- */

static void terminal(int f)
{
    static const char *line1 = ">OVERRIDE";
    static const char *line2 = "KEYSTEP PRO";
    for (int i = 0; i < 1024; i++) fb[i] = 0;
    int glitch = f < 10 ? 40 : 6;                  /* noise lines, heavy at first */
    for (int i = 0; i < glitch; i++) {
        int y = (int)(rnd() % 64), x0 = (int)(rnd() % 128), w = 4 + (int)(rnd() % 40);
        for (int x = x0; x < x0 + w; x++) if (rnd() & 1) px(x, y);
    }
    int typed = f - 4;                             /* characters revealed so far */
    int n1 = slen(line1), x = 4;
    for (int i = 0; i < n1 && i < typed; i++, x += 12) ch(line1[i], x, 8, 2, 0);
    int x2 = 4, k = typed - n1;
    for (int i = 0; i < slen(line2) && i < k; i++, x2 += 12) ch(line2[i], x2, 28, 2, 0);
    if (k > slen(line2) && f > 26) {               /* status line */
        static const char *ok = "ACCESS GRANTED";
        int xx = (128 - slen(ok) * 6) / 2;
        for (int i = 0; ok[i]; i++, xx += 6) ch(ok[i], xx, 52, 1, 0);
    }
    if ((f >> 2) & 1) {                            /* blinking block cursor */
        int cx = k <= 0 ? 4 + (typed < 0 ? 0 : typed) * 12 : x2, cy = k <= 0 ? 8 : 28;
        for (int dx = 0; dx < 10; dx++) for (int dy = 0; dy < 14; dy++) px(cx + dx, cy + dy);
    }
}

static void demo(int f)
{
    static const uint8_t bayer[4][4] = { {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5} };
    static const char *scroll =
        "      GREETINGS FROM THE KEYSTEP PRO - THIS MIDI CONTROLLER HAS BEEN TAKEN OVER - "
        "REAL DOOM ENGINE - 480 KB FLASH - 128 KB RAM - RIP AND TEAR ...      ";
    for (int i = 0; i < 1024; i++) fb[i] = 0;
    /* raster bars: three soft bands sliding on sines, ordered-dither shading */
    for (int bar = 0; bar < 3; bar++) {
        int cy = 22 + sine64[(f * (2 + bar) + bar * 21) & 63] * 20 / 31;
        for (int y = cy - 6; y <= cy + 6; y++) {
            int d = y - cy; if (d < 0) d = -d;
            int lvl = 15 - d * 2;
            for (int x = 0; x < 128; x++)
                if (y >= 0 && y < 50 && bayer[y & 3][x & 3] < lvl) px(x, y);
        }
    }
    /* logo: KEYSTEP / DOOM knocked out of the bars, each column wobbling on a sine */
    static const char *l1 = "KEYSTEP", *l2 = "DOOM";
    int w1 = slen(l1) * 12 - 2, w2 = slen(l2) * 18 - 3;
    for (int i = 0; l1[i]; i++) {
        int x = (128 - w1) / 2 + i * 12;
        int dy = sine64[(i * 9 + f * 5) & 63] / 8;
        for (int dx = -2; dx <= 2; dx++)            /* a 2 px black outline keeps it readable over the bars */
            for (int ddy = -2; ddy <= 2; ddy++) ch(l1[i], x + dx, 2 + dy + ddy, 2, 1);
        ch(l1[i], x, 2 + dy, 2, 0);
    }
    for (int i = 0; l2[i]; i++) {
        int x = (128 - w2) / 2 + i * 18;
        int dy = sine64[(i * 11 + f * 6 + 20) & 63] / 7;
        for (int dx = -2; dx <= 2; dx++)
            for (int ddy = -2; ddy <= 2; ddy++) ch(l2[i], x + dx, 21 + dy + ddy, 3, 1);
        ch(l2[i], x, 21 + dy, 3, 0);
    }
    /* scroller on the bottom line, 3 px per frame */
    int off = f * 3, sl = slen(scroll);
    for (int x = -(off % 6), i = off / 6; x < 128; x += 6, i++)
        ch(scroll[i % sl], x, 55, 1, 0);
    for (int x = 0; x < 128; x++) px(x, 52);      /* a rule above the scroller */
}

static int16_t melt_y[128];

static void melt_init(void)
{
    melt_y[0] = -(int16_t)(rnd() % 16);            /* DOOM's wipe: each column starts a little later */
    for (int x = 1; x < 128; x++) {
        int v = melt_y[x - 1] + (int)(rnd() % 3) - 1;
        if (v > 0) v = 0;
        if (v < -15) v = -15;
        melt_y[x] = (int16_t)v;
    }
}

static void melt(void)
{
    static uint8_t col[64];
    for (int x = 0; x < 128; x++) {
        int dy = melt_y[x] < 0 ? 0 : (melt_y[x] < 16 ? melt_y[x] + 1 : 8);
        melt_y[x] = (int16_t)(melt_y[x] + (melt_y[x] < 0 ? 1 : dy));
        int sh = melt_y[x] < 0 ? 0 : melt_y[x];
        if (sh <= 0) continue;
        for (int y = 0; y < 64; y++) col[y] = (fb[(y >> 3) * 128 + x] >> (y & 7)) & 1;
        for (int y = 63; y >= 0; y--) {
            int src = y - sh;
            if (src >= 0 && col[src]) px(x, y); else unpx(x, y);
        }
    }
}

void kd_intro(void)
{
    for (int f = 0; f < T_TERM + T_DEMO + T_MELT; f++) {
        uint32_t t0 = kd_plat_ms();
        if (f < T_TERM) terminal(f);
        else if (f < T_TERM + T_DEMO) demo(f - T_TERM);
        else {
            if (f == T_TERM + T_DEMO) melt_init();
            melt();
        }
        if (f < T_TERM + T_DEMO) leds(f);
        else if (f == T_TERM + T_DEMO) leds_off();
        kd_plat_present(fb);
        while (kd_plat_ms() - t0 < FPS_MS)
            ;
    }
    for (int i = 0; i < 1024; i++) fb[i] = 0;
    kd_plat_present(fb);
    kd_hud_resend();
}
