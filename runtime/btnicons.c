// The face button icons for [Controls] GameConfirm = South (btnicons.h), so every prompt shows the button that does
// it. Two textures draw them, both BC3, one level, block-linear (runtime/nvn/tegra.c):
// - The button font (menus and messages), GUI\02_common\font\button_festa_00_ID: towns use the copy in
//   loc\arc\resident.arc, quests the one in <language>\arc\quest_<language>.arc. Its top row holds the face buttons
//   left, top, bottom, right (the game's Y X B A; PlayStation icon mods' Square Triangle Cross Circle) in square
//   cells of 3/32 of the texture's width (24 pixels in the game's 256 x 256 one, 48 in HD mods' 512 x 512). With
//   the game's menus confirming with the bottom button, the bottom and the right cell trade places for good: the
//   patched archive is the source with that entry's new data appended (the other entries stay where they are), in
//   cache/btnicons/, made once per source file.
// - The field prompts' button (gather, carve, open a box...), one sprite for every prompt in the quest HUD texture
//   GUI\99_texture\HD_hud00_BM_NOMIP (quest_<language>.arc), in its row of L, the left, top, bottom and right face
//   buttons, and R. Which button a prompt takes depends on the prompt (runtime/hle/gameconfirm.c), so the bottom
//   and right sprites trade places in the renderer's copy of the texture while it should show the bottom button. The
//   game fills it with a GPU copy, so it is told as it is copied in, by the bytes of those two sprites as the archive
//   has them; their blocks are kept, and swapping them is two region uploads.
// - The title's "Press" button, baked into HD_title00 (title_<language>.arc): drawn as a shape in the same patched
//   archive, below.
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

// A square of BC3 blocks (pixels x, y, size: multiples of 4) of a w x h level 0: its hash, or swapped with another
typedef struct { uint32_t w, h, bhl; } Surface;
static uint64_t rect_hash(const uint8_t *data, Surface s, uint32_t x, uint32_t y, uint32_t size) {
    uint64_t h = 14695981039346656037ull;
    for (uint32_t by = y / 4; by < (y + size) / 4; by++)
        for (uint32_t bx = x / 4; bx < (x + size) / 4; bx++)
            h = fnv(h, data + tegra_block_offset(bx, by, s.w / 4, s.h / 4, 16, s.bhl), 16);
    return h;
}
static void rect_swap(uint8_t *data, Surface s, uint32_t xa, uint32_t xb, uint32_t y, uint32_t size) {
    for (uint32_t by = y / 4; by < (y + size) / 4; by++)
        for (uint32_t i = 0; i < size / 4; i++) {
            uint8_t *a = data + tegra_block_offset(xa / 4 + i, by, s.w / 4, s.h / 4, 16, s.bhl);
            uint8_t *b = data + tegra_block_offset(xb / 4 + i, by, s.w / 4, s.h / 4, 16, s.bhl), t[16];
            memcpy(t, a, 16), memcpy(a, b, 16), memcpy(b, t, 16);
        }
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
    uint32_t x0 = (DISC_X - 40) / 4 * 4, y0 = (DISC_Y - 40) / 4 * 4, n = 84;  // (the blocks around the disc's middle)
    static uint8_t px[84][84][4];
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

// An archive being rebuilt: the source with changed entries' data appended (the other entries stay where they are)
typedef struct { uint8_t *img; size_t len; } NewArc;
static bool arc_put(NewArc *a, const uint8_t *arc, const uint8_t *e, const uint8_t *data, uint32_t size) {
    uLongf plen = compressBound(size);
    uint8_t *packed = malloc(plen);
    bool ok = packed && compress2(packed, &plen, data, size, Z_DEFAULT_COMPRESSION) == Z_OK && a->len + plen <= 0x7FFFFFFFu;
    uint8_t *img = ok ? realloc(a->img, a->len + plen) : NULL;
    if (img) {
        a->img = img;
        memcpy(img + a->len, packed, plen);
        uint8_t *ne = img + (e - arc);
        wr32(ne + 68, (uint32_t)plen), wr32(ne + 72, (rd32(e + 72) & ~SIZE_MASK) | size), wr32(ne + 76, (uint32_t)a->len);
        a->len += plen;
    }
    free(packed);
    return img != NULL;
}

bool btnicons_patch(const char *src, char *out, size_t outsz) {
    struct stat st;
    if (stat(src, &st)) return false;
    long long v[2] = {(long long)st.st_size, (long long)st.st_mtime};
    uint64_t h = fnv(14695981039346656037ull, "2", 1);  // (patch version)
    h = fnv(fnv(h, src, strlen(src)), v, sizeof v);
    snprintf(out, outsz, "cache/btnicons/%016llx.arc", (unsigned long long)h);
    if (!stat(out, &st) && st.st_size > 0) return true;  // (made on an earlier start)

    long len = 0;
    uint32_t size = 0;
    uint8_t *arc = load(src, &len), *e = NULL, *tex = NULL;
    NewArc a = {arc ? malloc((size_t)len) : NULL, (size_t)len};
    bool ok = false;
    if (!a.img) goto done;
    memcpy(a.img, arc, (size_t)len);
    int changed = 0;
    Surface s;
    if ((tex = arc_entry(arc, len, FONT, &e, &size))) {
        if (!bc3_tex(tex, size, &s) || s.w != s.h || s.w % 128) {
            LOG("[romfs] %s: its button icons aren't laid out as known: not swapped\n", src);
        } else {
            uint32_t cell = s.w * 3 / 32;
            rect_swap(tex + TEX_HEAD, s, 2 * cell, 3 * cell, 0, cell);
            if (!arc_put(&a, arc, e, tex, size)) goto done;
            changed++;
        }
        free(tex);
    }
    if ((tex = arc_entry(arc, len, TITLE, &e, &size))) {
        if (title_cross(tex, size)) {  // (the game's own 1024 x 1024 one: its letter stays)
            if (!arc_put(&a, arc, e, tex, size)) goto done;
            changed++;
        }
        free(tex), tex = NULL;
    }
    if (!changed) goto done;

    mkdir("cache", 0755);
    mkdir("cache/btnicons", 0755);
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.tmp", out);
    FILE *f = fopen(tmp, "wb");
    if (!f) goto done;
    ok = fwrite(a.img, 1, a.len, f) == a.len;
    ok = fclose(f) == 0 && ok;
    if (ok) ok = rename(tmp, out) == 0;
    if (!ok) remove(tmp);
    else LOG("[romfs] button icons swapped: %s\n", src);
done:
    free(arc), free(tex), free(a.img);
    return ok;
}

// ---- the field prompts' button
// Where the bottom and right face buttons are in the HUD texture, by its width: the game's 512 x 512 one and HD
// mods' 2048 x 2048 (squares around each sprite, on 4-pixel blocks)
typedef struct { uint32_t w, bottom, right, y, size; } HudLayout;
static const HudLayout hud_layouts[] = {{512, 96, 128, 408, 32}, {2048, 400, 532, 1648, 100}};
#define MAX_KNOWN 8
static struct { const HudLayout *l; uint32_t h; uint64_t bottom, right; } g_known[MAX_KNOWN];  // (learnt at start)
static int g_nknown;
// The texture, as copied in: its two sprites' blocks, and whether the renderer's copy has them swapped
static struct { void *t; const HudLayout *l; uint8_t *bottom, *right; bool swapped; } g_hud;
static volatile bool g_want;  // the bottom button's sprite where the prompts' button is drawn
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

// A square of blocks (pixels, multiples of 4) of a level 0 whose blocks are row after row: hashed as rect_hash does,
// or copied out of / into a buffer of its own rows
static uint64_t lin_hash(const uint8_t *lin, uint32_t w, uint32_t x, uint32_t y, uint32_t size) {
    uint64_t h = 14695981039346656037ull;
    for (uint32_t by = y / 4; by < (y + size) / 4; by++) h = fnv(h, lin + ((size_t)by * (w / 4) + x / 4) * 16, size * 4);
    return h;
}
static void lin_copy(uint8_t *lin, uint32_t w, uint32_t x, uint32_t y, uint32_t size, uint8_t *buf, bool out) {
    for (uint32_t i = 0; i < size / 4; i++) {
        uint8_t *row = lin + ((size_t)(y / 4 + i) * (w / 4) + x / 4) * 16, *b = buf + (size_t)i * size * 4;
        if (out) memcpy(b, row, size * 4);
        else memcpy(row, b, size * 4);
    }
}

void btnicons_learn_hud(const char *src) {
    long len = 0;
    uint32_t size = 0;
    uint8_t *arc = load(src, &len), *tex = arc ? arc_entry(arc, len, HUD, NULL, &size) : NULL;
    Surface s;
    const HudLayout *l = NULL;
    if (tex && bc3_tex(tex, size, &s))
        for (size_t i = 0; i < sizeof hud_layouts / sizeof *hud_layouts; i++)
            if (hud_layouts[i].w == s.w && s.h == s.w) l = &hud_layouts[i];
    if (l) {
        uint64_t b = rect_hash(tex + TEX_HEAD, s, l->bottom, l->y, l->size), r = rect_hash(tex + TEX_HEAD, s, l->right, l->y, l->size);
        pthread_mutex_lock(&g_lock);
        bool seen = false;
        for (int i = 0; i < g_nknown; i++) seen |= g_known[i].bottom == b && g_known[i].right == r;
        if (!seen && g_nknown < MAX_KNOWN) g_known[g_nknown++] = (typeof(g_known[0])){l, s.h, b, r};
        pthread_mutex_unlock(&g_lock);
    } else if (tex) {
        LOG("[romfs] %s: its HUD's buttons aren't laid out as known: the prompts' button stays as it is\n", src);
    }
    free(arc), free(tex);
}

void btnicons_copied(void *t, uint8_t *blocks, uint32_t w, uint32_t h, uint32_t format) {
    if ((format != 0x44 && format != 0x48) || !g_nknown) return;  // (BC3)
    for (int k = 0; k < g_nknown; k++) {
        const HudLayout *l = g_known[k].l;
        if (l->w != w || g_known[k].h != h || lin_hash(blocks, w, l->bottom, l->y, l->size) != g_known[k].bottom) continue;
        size_t n = (size_t)l->size * l->size;  // (BC3: a byte a pixel)
        pthread_mutex_lock(&g_lock);
        if (g_hud.t != t || g_hud.l != l) {
            static bool logged;
            if (!logged) LOG("[gameconfirm] the prompts' button found in the %ux%u HUD texture\n", w, h), logged = true;
            free(g_hud.bottom), free(g_hud.right);
            g_hud = (typeof(g_hud)){t, l, malloc(n), malloc(n), false};
        }
        if (g_hud.bottom && g_hud.right) {
            lin_copy(blocks, w, l->bottom, l->y, l->size, g_hud.bottom, true);
            lin_copy(blocks, w, l->right, l->y, l->size, g_hud.right, true);
            g_hud.swapped = g_want;
            if (g_want) {  // (copied in while the bottom button is wanted: in already swapped)
                lin_copy(blocks, w, l->bottom, l->y, l->size, g_hud.right, false);
                lin_copy(blocks, w, l->right, l->y, l->size, g_hud.bottom, false);
            }
        }
        pthread_mutex_unlock(&g_lock);
        return;
    }
}

void btnicons_texture_gone(void *t) {
    if (g_hud.t != t) return;
    pthread_mutex_lock(&g_lock);
    if (g_hud.t == t) {
        free(g_hud.bottom), free(g_hud.right);
        g_hud = (typeof(g_hud)){0};
    }
    pthread_mutex_unlock(&g_lock);
}

void btnicons_hud_bottom(bool bottom) {
    g_want = bottom;
    if (!g_hud.t || g_hud.swapped == bottom) return;
    pthread_mutex_lock(&g_lock);
    if (g_hud.t && g_hud.bottom && g_hud.right && g_hud.swapped != bottom) {
        const HudLayout *l = g_hud.l;
        size_t n = (size_t)l->size * l->size;
        renderer_upload_region(g_hud.t, l->bottom, l->y, l->size, l->size, bottom ? g_hud.right : g_hud.bottom, n);
        renderer_upload_region(g_hud.t, l->right, l->y, l->size, l->size, bottom ? g_hud.bottom : g_hud.right, n);
        g_hud.swapped = bottom;
    }
    pthread_mutex_unlock(&g_lock);
}
