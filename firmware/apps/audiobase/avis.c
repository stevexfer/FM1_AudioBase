/* SPDX-License-Identifier: GPL-3.0-only */
/* AudioBase's visualizer (GLO), MOOD_FM1's audio visualizer (mood_avis.c): what is heard, as a picture that feeds on
 * itself.
 *
 * Each frame the picture before is drawn again turned and zoomed about its middle, a little darker, and the sound's
 * waveform is drawn over it as a shape with a symmetry (a ring of lobes, a star of scope lines, a phase plot turned
 * round the middle). So the shape leaves copies of itself, each turned and smaller (or larger) than the one before: a
 * spiral tunnel of the last second of sound. The zoom and the turn are not the same all over: they go from one value
 * in the middle to another at the rim, so the copies shear into whirls, or gather on a ring, or part from one. In
 * some scenes the picture is also folded on the way (mirrored in its axes, or in its diagonals too), which makes a
 * kaleidoscope of the same tunnel. The picture is one byte a pixel, shown through a palette that turns with the time.
 *
 * The sound moves all of it: the waveform is the shape; the bass pushes the zoom and swells the shape, the level
 * turns it faster and turns the colours, the treble shakes the turning; a beat (the bass well over its average)
 * flashes, jumps the colours, may turn the turning round, and, once a scene has had its time, brings the next scene.
 * In silence the shape is a slow wave of its own.
 *
 * Drawing: the picture is a disc in 90 x 90, shown as 180 x 180 (smoothed) in the middle of the screen, 22 frames a
 * second at the LCD's 12 MHz (its time on the wire). The canvas (ab_fw.h ui_fb) holds the picture
 * twice (the one before and the one being made) and two strips of 180 x 60 for the screen: a frame is three strips,
 * one a call. */
#include <stdint.h>
#include <string.h>

#include "avis.h"
#include "ab_fw.h"

#define FB 90                 /* the picture, squared */
#define W (2 * FB)            /* on the screen */
#define OX ((240 - W) / 2)
#define SH 60                 /* a strip's rows */
#define NS AB_SCOPE            /* the scope's samples */
#define M 96                  /* the shape's points: the newest 4 M samples */
#define MID ((FB - 1) * 32768) /* the middle, Q16 */
#define RIM ((FB - 1) * (FB - 1)) /* the disc's radius in half pixels, squared */
#define RINGS (RIM >> 7)      /* the zoom and the turn are set for each of these, by the radius squared */
#define FRAME_MS 45.0f        /* the scenes' numbers are for a frame of this long */

enum { RING, STAR, PHASE };
/* a scene: the shape, how many times it is drawn round the middle, and its size (1/64 of the disc's radius); the fold
   (0 none, 1 the axes, 2 the diagonals too); the zoom a frame in the middle and at the rim (1/256: under 0 the picture
   grows, a flight forward; over, it shrinks into its middle); the turn a frame there (1/4096 turns); how much of the
   light a frame keeps (x 1/256) */
typedef struct
{
    uint8_t shape, k, size, fold;
    int8_t zoom[2], turn[2];
    uint8_t keep;
} scene_t;
static const scene_t SCENE[] = {
    { RING, 1, 18, 0, { -18, -9 }, { 12, 12 }, 247 },    /* the tunnel: rings flying out, slowly turning */
    { RING, 6, 50, 0, { 14, 18 }, { 36, 36 }, 252 },     /* six lobes, each copy smaller and turned: a rose window */
    { STAR, 5, 62, 0, { 10, 14 }, { -100, -8 }, 252 },   /* a star winding into a whirl */
    { RING, 3, 18, 2, { -12, -12 }, { 60, 60 }, 246 },   /* the kaleidoscope */
    { PHASE, 4, 34, 1, { -16, 14 }, { 26, -26 }, 242 },  /* all of it gathers on a ring */
    { RING, 2, 34, 0, { 14, -12 }, { -34, 34 }, 249 },   /* and parts from one, into the middle and out */
    { STAR, 4, 44, 1, { -10, -6 }, { 44, 44 }, 246 },    /* blades thrown out */
    { RING, 2, 50, 1, { 22, 22 }, { 110, 110 }, 251 },   /* a fast whirl inward */
    { PHASE, 3, 46, 2, { 11, 11 }, { -30, -60 }, 251 },
};
#define SCENES ((int) (sizeof SCENE / sizeof SCENE[0]))
#define SCENE_MS 14000u       /* a scene's time before a beat may end it; twice that, it ends anyway */


static struct
{
    uint32_t last, scene_ms, rng;
    int strip, half, cur, scene, dir;
    float agc, bass, avg, level, treble, flash, psi, hue;
} A;
static uint16_t pal[256];

static float fabs_(float x) { return x < 0.0f ? -x : x; }
/* the sine of an angle in turns (a parabola and a correction) */
static float sin_(float a)
{
    float y;
    a -= (float) (int) a;
    if (a < 0.0f)
        a += 1.0f;
    if (a > 0.5f)
        a -= 1.0f;
    y = 16.0f * a * (0.5f - fabs_(a));
    return y * (0.775f + 0.225f * fabs_(y));
}
static float cos_(float a) { return sin_(a + 0.25f); }

static uint8_t* fb(int i) { return (uint8_t*) ui_fb() + i * (FB * FB); }
static uint16_t* strip_px(int half) { return ui_fb() + FB * FB + half * (W * SH); }

static uint32_t rnd(void)
{
    A.rng ^= A.rng << 13;
    A.rng ^= A.rng >> 17;
    A.rng ^= A.rng << 5;
    return A.rng;
}

/* ---- the picture before, turned and zoomed (and folded), into the new one. a, b, keep: by the ring (the zoom times
   the turn's cosine and sine, Q14; the light kept, x 1/256) */
static void feed(const uint8_t* src, uint8_t* dst, const int16_t* a, const int16_t* b, const uint8_t* keep, int fold)
{
    for (int y = 0; y < FB; ++y)
        for (int x = 0; x < FB; ++x)
        {
            int hx = 2 * x - (FB - 1), hy = 2 * y - (FB - 1), v0 = 0;
            const int ring = (hx * hx + hy * hy) >> 7;
            if (ring < RINGS)
            {
                int u, v;
                if (fold)
                {
                    hx = hx < 0 ? -hx : hx;
                    hy = hy < 0 ? -hy : hy;
                    if (fold > 1 && hy > hx)
                    {
                        const int t = hx;
                        hx = hy;
                        hy = t;
                    }
                }
                u = MID + (a[ring] * hx - b[ring] * hy) * 2;
                v = MID + (b[ring] * hx + a[ring] * hy) * 2;
                if ((unsigned) u < (unsigned) ((FB - 1) << 16) && (unsigned) v < (unsigned) ((FB - 1) << 16))
                {
                    const uint8_t* p = src + (v >> 16) * FB + (u >> 16);
                    const int fx = (u >> 8) & 255, fy = (v >> 8) & 255;
                    const int top = p[0] * 256 + (p[1] - p[0]) * fx, bot = p[FB] * 256 + (p[FB + 1] - p[FB]) * fx;
                    v0 = (((top * 256 + (bot - top) * fy) >> 8) * keep[ring] >> 16) - 2; /* (the haze goes first) */
                    v0 = v0 < 0 ? 0 : v0;
                }
            }
            *dst++ = (uint8_t) v0;
        }
}

/* a line into the picture (Q16 ends) in half-pixel steps, each a dot shared among the four pixels round it: the
   brighter of it and what is there */
static void line(uint8_t* d, int x0, int y0, int x1, int y1, int c)
{
    int dx = x1 - x0, dy = y1 - y0, n = ((dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy)) >> 15;
    n = n < 1 ? 1 : n > 4 * FB ? 4 * FB : n;
    dx /= n;
    dy /= n;
    for (; n; --n, x0 += dx, y0 += dy)
    {
        const int hx = (x0 - MID) >> 15, hy = (y0 - MID) >> 15;
        if (hx * hx + hy * hy < RIM - 256) /* (in the disc: then in the picture too) */
        {
            uint8_t* p = d + (y0 >> 16) * FB + (x0 >> 16);
            const int fx = (x0 >> 8) & 255, fy = (y0 >> 8) & 255;
            for (int i = 0; i < 4; ++i)
            {
                int w = ((i & 1 ? fx : 256 - fx) * (i & 2 ? fy : 256 - fy)) >> 6; /* four times its share, 256 at most */
                w = (w > 256 ? 256 : w) * c >> 8;
                if (p[(i & 1) + (i >> 1) * FB] < w)
                    p[(i & 1) + (i >> 1) * FB] = (uint8_t) w;
            }
        }
    }
}

/* the waveform (n: M + 16 points, -1..1) as the scene's shape, k times round the middle, r pixels wide */
static void shape(uint8_t* d, const float* n, const scene_t* s, float r, int c)
{
    for (int k = 0; k < s->k; ++k)
    {
        const float t0 = A.psi + (float) k / (float) s->k;
        const float c0 = cos_(t0), s0 = sin_(t0);
        const float cd = cos_(1.0f / (float) (s->k * M)), sd = sin_(1.0f / (float) (s->k * M));
        float cc = c0, ss = s0;
        int px = 0, py = 0;
        for (int i = 0; i <= M; ++i)
        {
            const float f = (float) i / (float) M;
            float x, y;
            if (s->shape == RING)
            {
                /* a lobe: the waveform along its arc, backwards in every other one (they meet), nothing at its ends */
                float q = r * (1.0f + 2.4f * f * (1.0f - f) * n[(k & 1) ? M - i : i]);
                const float t = cc * cd - ss * sd;
                q = q > FB / 2 - 2.0f ? FB / 2 - 2.0f : q; /* (flat against the rim rather than through it) */
                x = q * cc;
                y = q * ss;
                ss = ss * cd + cc * sd;
                cc = t;
            }
            else if (s->shape == STAR)
            {
                /* an arm: a scope line from the middle out, wider the further out */
                const float q = (r > FB / 2 - 2.0f ? FB / 2 - 2.0f : r) * f, o = r * 0.6f * f * n[i];
                x = q * c0 - o * s0;
                y = q * s0 + o * c0;
            }
            else
            {
                /* the sound against itself a millisecond later, turned */
                const float u = r * 1.2f * n[i], v = r * 1.2f * n[i + 12];
                x = u * c0 - v * s0;
                y = u * s0 + v * c0;
            }
            {
                const int qx = MID + (int) (x * 65536.0f), qy = MID + (int) (y * 65536.0f);
                if (i)
                    line(d, px, py, qx, qy, c);
                px = qx;
                py = qy;
            }
        }
    }
}

/* the palette: dark to a colour to white, the colour going round with the brightness and with A.hue */
static void colours(int flash)
{
    for (int i = 0; i < 256; ++i)
    {
        /* the hue: six sixths of 256, red to yellow to green to cyan to blue to magenta */
        const int hue = ((int) A.hue + i * 4) % 1536, six = hue >> 8, f = hue & 255;
        const int white = i > 200 ? (i - 200) * 4 : 0;
        int v = i + ((i * (255 - i)) >> 9) + (i > 8 ? flash : 0), c[3];
        v = v > 255 ? 255 : v;
        c[0] = c[1] = c[2] = 0;
        c[((six + 1) >> 1) % 3] = v;                                  /* the full one, and the one coming or going */
        c[(six & 1 ? six >> 1 : (six >> 1) + 1) % 3] = (v * (six & 1 ? 255 - f : f)) >> 8;
        for (int k = 0; k < 3; ++k)
            c[k] += ((255 - c[k]) * white) >> 8;
        {
            const uint16_t p = UI_RGB(c[0], c[1], c[2]);
            pal[i] = (uint16_t) (p >> 8 | p << 8);
        }
    }
}

/* ---- a frame: the sound, the scene's motion, the new picture */
static void update(uint32_t now)
{
    const int16_t* s = (const int16_t*) fb(A.cur ^ 1); /* (the scope, where the new picture is yet to be made) */
    float n[M + 16], lp = 0.0f, bass = 0.0f, treble = 0.0f, level = 0.0f, k;
    const scene_t* sc = &SCENE[A.scene];
    int peak = 0, beat;

    k = (float) (now - A.last) / FRAME_MS;
    k = k < 0.25f ? 0.25f : k > 2.0f ? 2.0f : k;
    A.last = now;

    ab_scope_read((int16_t*) fb(A.cur ^ 1), NS);
    for (int i = 0; i < NS; ++i)
    {
        const float x = (float) s[i] * (1.0f / 32768.0f);
        const int a = s[i] < 0 ? -s[i] : s[i];
        peak = a > peak ? a : peak;
        treble += fabs_(x - lp) * (i ? 1.0f : 0.0f);
        lp += (x - lp) * 0.03f;                                      /* under 230 Hz */
        bass += fabs_(lp);
        level += fabs_(x);
    }
    bass *= 4.0f / NS;
    level *= 2.0f / NS;
    treble *= 2.0f / NS;
    A.agc = A.agc * 0.97f > (float) peak ? A.agc * 0.97f : (float) peak;
    {
        /* the shape's size: by this frame's peak, down to a third of the loudest of late (the quiet between two
           beats is still a shape) */
        float g = (float) peak > A.agc * 0.35f ? (float) peak : A.agc * 0.35f;
        g = 1.0f / (g < 500.0f ? 500.0f : g);
        for (int i = 0; i < M + 16; ++i)
        {
            const int16_t* p = s + NS - 4 * (M + 16) + 4 * i;
            n[i] = (float) (p[0] + p[1] + p[2] + p[3]) * 0.25f * g;
        }
    }
    beat = bass > 0.04f && bass > A.avg * 1.5f && A.bass <= A.avg * 1.5f;
    A.avg += (bass - A.avg) * 0.08f;
    A.bass = bass;
    A.level += (level - A.level) * 0.3f;
    A.treble += (treble - A.treble) * 0.4f;
    A.flash *= 0.7f;
    if (beat)
    {
        A.flash = 1.0f;
        A.hue += 150.0f;
        if (!(rnd() & 3u))
            A.dir = -A.dir;
    }
    if ((beat && now - A.scene_ms > SCENE_MS) || now - A.scene_ms > 2u * SCENE_MS)
    {
        A.scene = (A.scene + 1 + (int) (rnd() % 3u)) % SCENES;
        A.scene_ms = now;
        sc = &SCENE[A.scene];
    }

    /* in silence: a wave of its own */
    if (A.agc < 500.0f)
        for (int i = 0; i < M + 16; ++i)
            n[i] += (1.0f - A.agc * (1.0f / 500.0f)) * 0.4f * sin_(A.psi * 3.0f + (float) i * (3.0f / M));

    A.psi += (0.004f + 0.02f * A.level) * k * (float) A.dir;
    A.psi -= (float) (int) A.psi;
    A.hue += (3.0f + 60.0f * A.level) * k;
    if (A.hue >= 1536.0f)
        A.hue -= 1536.0f;

    {
        /* the bass pushes the zoom the way it goes; the level and the treble the turn */
        const float push = k * (1.0f + 6.0f * (bass > 0.5f ? 0.5f : bass)) * (1.0f / 256.0f);
        const float spin = k * (1.0f + 3.0f * A.level) * (float) A.dir * (1.0f / 4096.0f);
        const float shake = A.treble > 0.02f ? ((float) (rnd() & 255u) - 127.5f) * A.treble * (1.0f / 2048.0f) : 0.0f;
        int keep = 256 - (int) ((float) (256 - sc->keep) * k);
        const uint8_t* src = fb(A.cur);
        uint8_t* dst = fb(A.cur ^ 1);
        int16_t a[RINGS], b[RINGS];
        uint8_t kp[RINGS];
        keep = keep < 128 ? 128 : keep > 255 ? 255 : keep;
        for (int i = 0; i < RINGS; ++i)
        {
            const float f = (float) i * (1.0f / (RINGS - 1));
            float zoom = ((float) sc->zoom[0] + (float) (sc->zoom[1] - sc->zoom[0]) * f) * push;
            const float turn = ((float) sc->turn[0] + (float) (sc->turn[1] - sc->turn[0]) * f) * spin + shake;
            zoom = 1.0f + (zoom < -0.4f ? -0.4f : zoom > 0.4f ? 0.4f : zoom);
            a[i] = (int16_t) (zoom * cos_(turn) * 16384.0f);
            b[i] = (int16_t) (zoom * sin_(turn) * 16384.0f);
            kp[i] = (uint8_t) (keep * (RINGS - i > 6 ? 6 : RINGS - i) / 6); /* a soft rim */
        }
        feed(src, dst, a, b, kp, sc->fold);
        shape(dst, n, sc, (float) sc->size * ((FB / 2) / 64.0f) * (1.0f + (bass > 0.5f ? 0.5f : bass)),
              200 + (int) (55.0f * A.flash));
        A.cur ^= 1;
    }
    colours((int) (A.flash * 70.0f));
}

/* a strip of the screen from the picture: each pixel 2 x 2, the ones between two (or four) the mean of them */
static void show(uint16_t* out, int y0)
{
    const uint8_t* p = fb(A.cur);
    for (int y = y0; y < y0 + SH; ++y)
    {
        const uint8_t* a = p + (y >> 1) * FB;
        const uint8_t* b = (y & 1) && (y >> 1) < FB - 1 ? a + FB : a;
        for (int x = 0; x < FB; ++x)
        {
            const int x1 = x < FB - 1 ? x + 1 : x, v = a[x] + b[x];
            *out++ = pal[v >> 1];
            *out++ = pal[(v + a[x1] + b[x1]) >> 2];
        }
    }
}

void avis_start(uint32_t now)
{
    memset(&A, 0, sizeof A);
    A.last = A.scene_ms = now;
    A.rng = (now * 2654435761u) | 1u;
    rnd();
    A.hue = (float) (rnd() % 1536u);
    A.scene = (int) ((rnd() >> 8) % (uint32_t) SCENES);
    A.dir = 1;
    ui_lcd_sync(); /* (the transfer running may read the canvas) */
    memset(ui_fb(), 0, 2 * FB * FB);
    ui_lcd_fill(0, 0, 240, 240, 0);
}

uint32_t avis_frames; /* the frames drawn (a bench reads it from the RAM) */

void avis_frame(uint32_t now)
{
    if (!A.strip)
    {
        ++avis_frames;
        update(now);
    }
    A.half ^= 1;
    show(strip_px(A.half), A.strip * SH);
    ui_lcd_blit(OX, (uint32_t) (OX + A.strip * SH), W, SH, strip_px(A.half));
    A.strip = (A.strip + 1) % (W / SH);
}
