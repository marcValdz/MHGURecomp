// The face button icons for [Controls] GameConfirm = South (btnicons.h), so every prompt shows the button that does
// it, as the setting is saved. The icons are in three BC3 textures, one level, block-linear (runtime/nvn/tegra.c). The
// archives are read once, at start, as the mods left them: where each icon is and what it looks like with the button
// swapped (a "known" region of a texture). The game fills its textures with a copy it does not show again, so each is
// told as it is copied in (btnicons_copied): a texture whose region has the bytes of a known one is the renderer's
// copy to change. Showing the bottom button is then one region upload (renderer_upload_region), and back.
// - The button font (menus and messages), GUI\02_common\font\button_festa_00_ID: towns use the copy in
//   loc\arc\resident.arc, quests the one in <language>\arc\quest_<language>.arc. Its top row holds the face buttons
//   left, top, bottom, right (the game's Y X B A; PlayStation icon mods' Square Triangle Cross Circle) in square
//   cells of 3/32 of the texture's width (24 pixels in the game's 256 x 256 one, 48 in HD mods' 512 x 512). The
//   bottom and the right cell trade places.
// - The field prompts' button (gather, carve, open a box...), one sprite for every prompt in the quest HUD texture
//   GUI\99_texture\HD_hud00_BM_NOMIP (quest_<language>.arc), in its row of L, the left, top, bottom and right face
//   buttons, and R. Which button a prompt takes depends on the prompt (runtime/hle/gameconfirm.c), so the bottom
//   and right sprites trade places only while a prompt should show the bottom button.
// - The title's "Press" button, baked into HD_title00 (title_<language>.arc): drawn as a shape, below.
#include "btnicons.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <zlib.h>

#include "nvn/bcn.h"
#include "nvn/nvn.h"
#include "nvn/tegra.h"
#include "rt.h"

#define HEAD 12u
#define ENTRY 80u
#define SIZE_MASK 0x1FFFFFFFu
#define TEX_HEAD 0x18u
#define FONT "GUI\\02_common\\font\\button_festa_00_ID"
#define HUD "GUI\\99_texture\\HD_hud00_BM_NOMIP"

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = v, p[1] = v >> 8, p[2] = v >> 16, p[3] = v >> 24; }

static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    for (const uint8_t *q = p; n--; q++) h = (h ^ *q) * 1099511628211ull;
    return h;
}

static uint8_t *load(const char *path, long *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = *len > 0 ? malloc((size_t)*len) : NULL;
    if (buf && fread(buf, 1, (size_t)*len, f) != (size_t)*len) free(buf), buf = NULL;
    fclose(f);
    return buf;
}


// A texture's level 0 layout (block-linear), and a rectangle of it (pixels x, y, w, h: multiples of 4) as its blocks
// row after row, the way renderer_upload_region wants them
typedef struct { uint32_t w, h, bhl; } Surface;
static void region_blocks(const uint8_t *data, Surface s, uint32_t x, uint32_t y, uint32_t rw, uint32_t rh, uint8_t *buf) {
    for (uint32_t r = 0; r < rh / 4; r++)
        for (uint32_t c = 0; c < rw / 4; c++)
            memcpy(buf + ((size_t)r * (rw / 4) + c) * 16, data + tegra_block_offset(x / 4 + c, y / 4 + r, s.w / 4, s.h / 4, 16, s.bhl), 16);
}

// A .tex (BC3, one level) and its size; false for any other
static bool bc3_tex(const uint8_t *tex, uint32_t size, Surface *s) {
    if (size < TEX_HEAD) return false;
    uint32_t w2 = rd32(tex + 8), w = (w2 >> 6) & 0x1FFF, h = (w2 >> 19) & 0x1FFF;
    if (tex[0xd] != 0x17 || !w || w % 4 || h % 4 || size != TEX_HEAD + w * h) return false;
    *s = (Surface){w, h, tegra_default_block_height_log2(h / 4)};
    return true;
}

// The archive entry whose name ends in `name` (after a '\', or all of it), decompressed; NULL: none
static uint8_t *arc_entry(const uint8_t *arc, long len, const char *name, uint8_t **entry, uint32_t *size) {
    if (len < (long)HEAD || memcmp(arc, "ARC", 4)) return NULL;
    int n = arc[6] | arc[7] << 8;
    size_t fl = strlen(name);
    for (int i = 0; i < n && (long)(HEAD + (uint32_t)(i + 1) * ENTRY) <= len; i++) {
        const uint8_t *p = arc + HEAD + (uint32_t)i * ENTRY;
        size_t nl = strnlen((const char *)p, 64);
        if (nl < fl || memcmp(p + nl - fl, name, fl) || (nl > fl && p[nl - fl - 1] != '\\')) continue;
        uint32_t csize = rd32(p + 68), off = rd32(p + 76);
        uLongf dsize = rd32(p + 72) & SIZE_MASK;
        uint8_t *out = malloc(dsize ? dsize : 1);
        if ((uint64_t)off + csize > (uint64_t)len || !out || uncompress(out, &dsize, arc + off, csize) != Z_OK) {
            free(out);
            return NULL;
        }
        if (entry) *entry = (uint8_t *)p;
        *size = (uint32_t)dsize;
        return out;
    }
    return NULL;
}

// ---- the title's prompt
// "Press <button>" at the title is a sprite in HD_title00: in HD UI mods' 2048 x 2048 texture a dark disc centered at
// (559, 1458) with the Circle's red ring (the game's own 1024 x 1024 one has a white disc and a letter, left as it is).
// The Cross is drawn over the ring as a shape, not a picture, and the blocks it touches are encoded again.
#define TITLE "GUI\\99_texture\\HD_title00_BM_NOMIP"
#define TITLE_W 2048u
#define DISC_X 559
#define DISC_Y 1458
#define TITLE_X0 ((DISC_X - 40) / 4 * 4)  // (the blocks around the disc's middle: the region this changes)
#define TITLE_Y0 ((DISC_Y - 40) / 4 * 4)
#define TITLE_N 84
#define DISC_ERASE 35.0f  // (the ring reaches 32 pixels from the center)
#define CROSS_LEN 32.2f   // (half the length of each stroke, to the middle of its square end)
#define CROSS_HALF 3.9f   // (half the stroke's width)
static const uint8_t DISC_COLOR[3] = {35, 34, 35}, CROSS_COLOR[3] = {140, 207, 255};

// One 4 x 4 block of RGBA texels -> BC3 (alpha: its range; colors: the ends of the box they span)
static void bc3_encode(const uint8_t px[16][4], uint8_t *b) {
    int amin = 255, amax = 0, lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0};
    for (int i = 0; i < 16; i++) {
        if (px[i][3] < amin) amin = px[i][3];
        if (px[i][3] > amax) amax = px[i][3];
        for (int c = 0; c < 3; c++) {
            if (px[i][c] < lo[c]) lo[c] = px[i][c];
            if (px[i][c] > hi[c]) hi[c] = px[i][c];
        }
    }
    uint64_t abits = 0;  // (8 values a0 > a1; with a0 == a1 every index is 0)
    for (int i = 0; i < 16 && amax > amin; i++) {
        int k = ((px[i][3] - amin) * 14 + (amax - amin)) / (2 * (amax - amin));  // 0 = amin .. 7 = amax
        abits |= (uint64_t)(k == 0 ? 1 : k == 7 ? 0 : 8 - k) << (3 * i);  // (index 0 = a0 = amax, 1 = a1 = amin)
    }
    b[0] = (uint8_t)amax, b[1] = (uint8_t)amin;
    for (int i = 0; i < 6; i++) b[2 + i] = (uint8_t)(abits >> (8 * i));
    int q[2][3], pal[4][3];
    for (int e = 0; e < 2; e++)
        for (int c = 0; c < 3; c++) {
            int v = e ? lo[c] : hi[c], m = c == 1 ? 63 : 31;
            q[e][c] = (v * m + 127) / 255;
        }
    uint32_t c0 = (uint32_t)q[0][0] << 11 | (uint32_t)q[0][1] << 5 | (uint32_t)q[0][2];
    uint32_t c1 = (uint32_t)q[1][0] << 11 | (uint32_t)q[1][1] << 5 | (uint32_t)q[1][2];
    if (c0 == c1) {  // (flat: index 0 is exact up to the 565 rounding)
        b[8] = (uint8_t)c0, b[9] = (uint8_t)(c0 >> 8), b[10] = (uint8_t)c1, b[11] = (uint8_t)(c1 >> 8);
        b[12] = b[13] = b[14] = b[15] = 0;
        return;
    }
    if (c0 < c1) { uint32_t t = c0; c0 = c1; c1 = t; }  // (c0 > c1: the four-color mode)
    int e0[3] = {(int)(c0 >> 11), (int)(c0 >> 5 & 63), (int)(c0 & 31)}, e1[3] = {(int)(c1 >> 11), (int)(c1 >> 5 & 63), (int)(c1 & 31)};
    for (int c = 0; c < 3; c++) {
        int m = c == 1 ? 63 : 31;
        pal[0][c] = e0[c] * 255 / m, pal[1][c] = e1[c] * 255 / m;
        pal[2][c] = (2 * pal[0][c] + pal[1][c]) / 3, pal[3][c] = (pal[0][c] + 2 * pal[1][c]) / 3;
    }
    uint32_t idx = 0;
    for (int i = 0; i < 16; i++) {
        int best = 0, bd = 1 << 30;
        for (int k = 0; k < 4; k++) {
            int d = 0;
            for (int c = 0; c < 3; c++) d += (px[i][c] - pal[k][c]) * (px[i][c] - pal[k][c]);
            if (d < bd) bd = d, best = k;
        }
        idx |= (uint32_t)best << (2 * i);
    }
    b[8] = (uint8_t)c0, b[9] = (uint8_t)(c0 >> 8), b[10] = (uint8_t)c1, b[11] = (uint8_t)(c1 >> 8);
    for (int i = 0; i < 4; i++) b[12 + i] = (uint8_t)(idx >> (8 * i));
}

// How much of the pixel at (x, y) (4 x 4 samples) the Cross covers, 0 .. 16
static int cross_cover(int x, int y) {
    int n = 0;
    for (int sy = 0; sy < 4; sy++)
        for (int sx = 0; sx < 4; sx++) {
            float dx = (float)(x - DISC_X) + ((float)sx - 1.5f) / 4, dy = (float)(y - DISC_Y) + ((float)sy - 1.5f) / 4;
            float u = fabsf(dx + dy) * 0.70710678f, v = fabsf(dx - dy) * 0.70710678f;
            n += (u <= CROSS_LEN && v <= CROSS_HALF) || (v <= CROSS_LEN && u <= CROSS_HALF);
        }
    return n;
}

// The ring replaced by the Cross in a TITLE texture; false when it isn't laid out as above
static bool title_cross(uint8_t *tex, uint32_t size) {
    Surface s;
    if (!bc3_tex(tex, size, &s) || s.w != TITLE_W || s.h != TITLE_W) return false;
    uint8_t *data = tex + TEX_HEAD;
    uint32_t x0 = TITLE_X0, y0 = TITLE_Y0, n = TITLE_N;
    static uint8_t px[TITLE_N][TITLE_N][4];
    for (uint32_t by = 0; by < n / 4; by++)
        for (uint32_t bx = 0; bx < n / 4; bx++) {
            uint8_t blk[16], t[16][4];
            memcpy(blk, data + tegra_block_offset(x0 / 4 + bx, y0 / 4 + by, s.w / 4, s.h / 4, 16, s.bhl), 16);
            bcn_decode(BCN_BC3, blk, 4, 4, (uint8_t *)t);
            for (int i = 0; i < 16; i++) memcpy(px[by * 4 + (uint32_t)i / 4][bx * 4 + (uint32_t)i % 4], t[i], 4);
        }
    const uint8_t *ring = px[DISC_Y - 28 - y0][DISC_X - x0], *disc = px[DISC_Y - y0][DISC_X - 40 - x0];
    if (ring[0] < 200 || ring[1] > 140 || ring[2] > 140 || abs(disc[0] - DISC_COLOR[0]) > 8 || disc[3] != 255) return false;
    for (uint32_t by = 0; by < n / 4; by++)
        for (uint32_t bx = 0; bx < n / 4; bx++) {
            uint8_t t[16][4];
            for (int i = 0; i < 16; i++) {
                int x = (int)(x0 + bx * 4) + i % 4, y = (int)(y0 + by * 4) + i / 4;
                uint8_t *p = px[by * 4 + (uint32_t)i / 4][bx * 4 + (uint32_t)i % 4];
                float d = hypotf((float)(x - DISC_X), (float)(y - DISC_Y)), e = DISC_ERASE + 0.5f - d;
                e = e < 0 ? 0 : e > 1 ? 1 : e;
                int k = cross_cover(x, y);
                for (int c = 0; c < 3; c++) {
                    float v = (float)p[c] + ((float)DISC_COLOR[c] - (float)p[c]) * e;
                    v += ((float)CROSS_COLOR[c] - v) * (float)k / 16;
                    t[i][c] = (uint8_t)(v + 0.5f);
                }
                t[i][3] = p[3];
            }
            bc3_encode(t, data + tegra_block_offset(x0 / 4 + bx, y0 / 4 + by, s.w / 4, s.h / 4, 16, s.bhl));
        }
    return true;
}

// ---- what is known, and which of the renderer's textures show it
enum { KIND_MENU, KIND_HUD };  // the font and the title follow GameConfirm; the prompts' button also the prompt shown
typedef struct {
    int kind;
    uint32_t w, h, x, y, rw, rh;  // the texture's size, and the region (pixels)
    uint64_t hash;                // of the region's blocks as the archive has them
    uint8_t *orig, *alt;          // those blocks, and with the bottom button
} Known;
typedef struct { uint32_t w, bottom, right, y, size; } HudLayout;  // (by the HUD texture's width: the game's 512 x 512
static const HudLayout hud_layouts[] = {{512, 96, 128, 408, 32}, {2048, 400, 532, 1648, 100}};  // and HD mods' 2048)
#define MAX_KNOWN 48
#define MAX_SEEN 16
static Known g_known[MAX_KNOWN];  // (learnt at start)
static int g_nknown;
static struct { void *t; const Known *k; bool swapped; } g_seen[MAX_SEEN];  // the renderer's textures, and what they show
static volatile bool g_want[2];  // the bottom button where each kind is drawn
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void known_add(int kind, Surface s, uint32_t x, uint32_t y, uint32_t rw, uint32_t rh, uint8_t *orig, uint8_t *alt) {
    Known k = {kind, s.w, s.h, x, y, rw, rh, fnv(14695981039346656037ull, orig, (size_t)rw * rh), orig, alt};
    pthread_mutex_lock(&g_lock);
    bool keep = g_nknown < MAX_KNOWN;
    for (int i = 0; keep && i < g_nknown; i++)
        keep = !(g_known[i].kind == kind && g_known[i].w == s.w && g_known[i].h == s.h && g_known[i].x == x &&
                 g_known[i].y == y && g_known[i].hash == k.hash);
    if (keep) g_known[g_nknown++] = k;
    pthread_mutex_unlock(&g_lock);
    if (!keep) free(orig), free(alt);
}

int btnicons_known(void) { return g_nknown; }

void btnicons_learn(const char *src) {
    long len = 0;
    uint32_t size = 0;
    uint8_t *arc = load(src, &len), *tex;
    Surface s;
    if (!arc) return;
    if ((tex = arc_entry(arc, len, FONT, NULL, &size))) {
        if (bc3_tex(tex, size, &s) && s.w == s.h && s.w % 128 == 0) {
            uint32_t cell = s.w * 3 / 32, row = cell * 4;  // (a cell's blocks in a row of them: bytes)
            size_t n = (size_t)2 * cell * cell;
            uint8_t *o = malloc(n), *a = malloc(n);
            if (o && a) {
                region_blocks(tex + TEX_HEAD, s, 2 * cell, 0, 2 * cell, cell, o);  // (the bottom and the right cell)
                for (uint32_t r = 0; r < cell / 4; r++) {
                    memcpy(a + (size_t)r * 2 * row, o + (size_t)r * 2 * row + row, row);
                    memcpy(a + (size_t)r * 2 * row + row, o + (size_t)r * 2 * row, row);
                }
                known_add(KIND_MENU, s, 2 * cell, 0, 2 * cell, cell, o, a);
            } else {
                free(o), free(a);
            }
        } else {
            LOG("[romfs] %s: its button icons aren't laid out as known: not swapped\n", src);
        }
        free(tex);
    }
    if ((tex = arc_entry(arc, len, HUD, NULL, &size))) {
        const HudLayout *l = NULL;
        if (bc3_tex(tex, size, &s))
            for (size_t i = 0; i < sizeof hud_layouts / sizeof *hud_layouts; i++)
                if (hud_layouts[i].w == s.w && s.h == s.w) l = &hud_layouts[i];
        if (l) {
            uint32_t rw = l->right + l->size - l->bottom, rowb = rw * 4, sq = l->size * 4, off = (l->right - l->bottom) * 4;
            size_t n = (size_t)rw * l->size;
            uint8_t *o = malloc(n), *a = malloc(n);
            if (o && a) {
                region_blocks(tex + TEX_HEAD, s, l->bottom, l->y, rw, l->size, o);
                memcpy(a, o, n);
                for (uint32_t r = 0; r < l->size / 4; r++) {  // (the two sprites trade places inside it)
                    memcpy(a + (size_t)r * rowb, o + (size_t)r * rowb + off, sq);
                    memcpy(a + (size_t)r * rowb + off, o + (size_t)r * rowb, sq);
                }
                known_add(KIND_HUD, s, l->bottom, l->y, rw, l->size, o, a);
            } else {
                free(o), free(a);
            }
        } else {
            LOG("[romfs] %s: its HUD's buttons aren't laid out as known: the prompts' button stays as it is\n", src);
        }
        free(tex);
    }
    if ((tex = arc_entry(arc, len, TITLE, NULL, &size))) {
        size_t n = (size_t)TITLE_N * TITLE_N;
        uint8_t *o = malloc(n), *a = malloc(n);
        if (o && a && bc3_tex(tex, size, &s) && s.w == TITLE_W && s.h == TITLE_W) {  // (the game's own 1024 x 1024: a letter, as is)
            region_blocks(tex + TEX_HEAD, s, TITLE_X0, TITLE_Y0, TITLE_N, TITLE_N, o);
            if (title_cross(tex, size)) {
                region_blocks(tex + TEX_HEAD, s, TITLE_X0, TITLE_Y0, TITLE_N, TITLE_N, a);
                known_add(KIND_MENU, s, TITLE_X0, TITLE_Y0, TITLE_N, TITLE_N, o, a);
                o = a = NULL;
            }
        }
        free(o), free(a), free(tex);
    }
    free(arc);
}

void btnicons_copied(void *t, uint8_t *blocks, uint32_t w, uint32_t h, uint32_t format) {
    if ((format != 0x44 && format != 0x48) || !g_nknown) return;  // (BC3)
    for (int i = 0; i < g_nknown; i++) {
        const Known *k = &g_known[i];
        if (k->w != w || k->h != h) continue;
        uint64_t hh = 14695981039346656037ull;
        for (uint32_t r = 0; r < k->rh / 4; r++) hh = fnv(hh, blocks + ((size_t)(k->y / 4 + r) * (w / 4) + k->x / 4) * 16, (size_t)k->rw * 4);
        if (hh != k->hash) continue;
        pthread_mutex_lock(&g_lock);
        int slot = -1;
        for (int j = 0; j < MAX_SEEN; j++)
            if (g_seen[j].t == t && g_seen[j].k == k) slot = j;
        for (int j = 0; j < MAX_SEEN && slot < 0; j++)
            if (!g_seen[j].t) slot = j;
        if (slot >= 0) {
            static bool logged[2];
            if (!logged[k->kind]) LOG("[gameconfirm] the %s found in a %ux%u texture\n", k->kind == KIND_HUD ? "prompts' button" : "button icons", w, h), logged[k->kind] = true;
            g_seen[slot].k = k, g_seen[slot].swapped = g_want[k->kind], g_seen[slot].t = t;
            if (g_want[k->kind])  // (copied in while the bottom button is wanted: in already swapped)
                for (uint32_t r = 0; r < k->rh / 4; r++)
                    memcpy(blocks + ((size_t)(k->y / 4 + r) * (w / 4) + k->x / 4) * 16, k->alt + (size_t)r * k->rw * 4, (size_t)k->rw * 4);
        }
        pthread_mutex_unlock(&g_lock);
        return;
    }
}

void btnicons_texture_gone(void *t) {
    pthread_mutex_lock(&g_lock);
    for (int j = 0; j < MAX_SEEN; j++)
        if (g_seen[j].t == t) g_seen[j].t = NULL;
    pthread_mutex_unlock(&g_lock);
}

void btnicons_set(bool menus, bool hud) {
    g_want[KIND_MENU] = menus, g_want[KIND_HUD] = hud;
    bool todo = false;
    for (int j = 0; j < MAX_SEEN; j++) todo |= g_seen[j].t && g_seen[j].k && g_seen[j].swapped != g_want[g_seen[j].k->kind];
    if (!todo) return;
    pthread_mutex_lock(&g_lock);
    for (int j = 0; j < MAX_SEEN; j++) {
        if (!g_seen[j].t || !g_seen[j].k) continue;
        const Known *k = g_seen[j].k;
        bool want = g_want[k->kind];
        if (g_seen[j].swapped == want) continue;
        renderer_upload_region(g_seen[j].t, k->x, k->y, k->rw, k->rh, want ? k->alt : k->orig, (size_t)k->rw * k->rh);
        g_seen[j].swapped = want;
    }
    pthread_mutex_unlock(&g_lock);
}
