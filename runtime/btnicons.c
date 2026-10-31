// The face button icons for [Controls] GameConfirm = South (btnicons.h): with the game's menus confirming with the
// bottom button, the icons of the bottom and the right button trade places, so every prompt (menus and the field's
// talk / gather prompts alike) shows the button that does it. The prompts are characters of the button font, drawn
// from the texture GUI\02_common\font\button_festa_00_ID: towns use the copy in loc\arc\resident.arc, quests the one
// in <language>\arc\quest_<language>.arc. Its top row holds the face buttons left, top, bottom, right (the game's
// Y X B A; PlayStation icon mods' Square Triangle Cross Circle) in square cells of 3/32 of the texture's width (24
// pixels in the game's 256 x 256 one, 48 in HD mods' 512 x 512). BC3, one level, block-linear (runtime/nvn/tegra.c).
// The patched archive is the source with that entry's new data appended (the other entries stay where they are), in
// cache/btnicons/, made once per source file.
#include "btnicons.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <zlib.h>

#include "nvn/tegra.h"
#include "rt.h"

#define HEAD 12u
#define ENTRY 80u
#define SIZE_MASK 0x1FFFFFFFu
#define TEX_HEAD 0x18u
#define FONT "GUI\\02_common\\font\\button_festa_00_ID"

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

// Swaps the bottom and right buttons' cells of a .tex (BC3, square, one level); false for any other layout.
static bool swap_cells(uint8_t *tex, uint32_t size) {
    if (size < TEX_HEAD) return false;
    uint32_t w2 = rd32(tex + 8), w = (w2 >> 6) & 0x1FFF, h = (w2 >> 19) & 0x1FFF, wb = w / 4, hb = h / 4;
    if (tex[0xd] != 0x17 || w != h || !w || w % 128 || size != TEX_HEAD + w * h) return false;
    uint32_t bhl = tegra_default_block_height_log2(hb), cell = w * 3 / 32 / 4;  // (the cell in 4 x 4 blocks)
    uint8_t *lin = malloc((size_t)wb * hb * 16);
    if (!lin) return false;
    tegra_deswizzle(tex + TEX_HEAD, lin, wb, hb, 1, 16, bhl, 0);
    for (uint32_t y = 0; y < cell; y++)
        for (uint32_t x = 0; x < cell; x++) {
            uint8_t *a = lin + ((size_t)y * wb + 2 * cell + x) * 16, *b = lin + ((size_t)y * wb + 3 * cell + x) * 16, t[16];
            memcpy(t, a, 16), memcpy(a, b, 16), memcpy(b, t, 16);
        }
    tegra_swizzle(lin, tex + TEX_HEAD, wb, hb, 1, 16, bhl, 0);
    free(lin);
    return true;
}

bool btnicons_patch(const char *src, char *out, size_t outsz) {
    struct stat st;
    if (stat(src, &st)) return false;
    long long v[2] = {(long long)st.st_size, (long long)st.st_mtime};
    uint64_t h = fnv(14695981039346656037ull, "1", 1);  // (patch version)
    h = fnv(fnv(h, src, strlen(src)), v, sizeof v);
    snprintf(out, outsz, "cache/btnicons/%016llx.arc", (unsigned long long)h);
    if (!stat(out, &st) && st.st_size > 0) return true;  // (made on an earlier start)

    long len = 0;
    uint8_t *arc = load(src, &len), *tex = NULL, *packed = NULL, *img = NULL;
    bool ok = false;
    if (!arc || len < (long)HEAD || memcmp(arc, "ARC", 4)) goto done;
    int n = arc[6] | arc[7] << 8;
    uint8_t *e = NULL;
    for (int i = 0; i < n && (long)(HEAD + (uint32_t)(i + 1) * ENTRY) <= len; i++) {
        uint8_t *p = arc + HEAD + (uint32_t)i * ENTRY;
        size_t nl = strnlen((const char *)p, 64), fl = sizeof FONT - 1;
        if (nl >= fl && !memcmp(p + nl - fl, FONT, fl) && (nl == fl || p[nl - fl - 1] == '\\')) e = p;
    }
    if (!e) goto done;
    uint32_t csize = rd32(e + 68), sf = rd32(e + 72), off = rd32(e + 76);
    uLongf size = sf & SIZE_MASK;
    if ((uint64_t)off + csize > (uint64_t)len || !(tex = malloc(size)) ||
        uncompress(tex, &size, arc + off, csize) != Z_OK || !swap_cells(tex, (uint32_t)size)) {
        LOG("[romfs] %s: its button icons aren't laid out as known: not swapped\n", src);
        goto done;
    }
    uLongf plen = compressBound(size);
    if (!(packed = malloc(plen)) || compress2(packed, &plen, tex, size, Z_DEFAULT_COMPRESSION) != Z_OK) goto done;
    if ((uint64_t)len + plen > 0x7FFFFFFFu || !(img = malloc((size_t)len + plen))) goto done;
    memcpy(img, arc, (size_t)len);
    memcpy(img + len, packed, plen);
    uint8_t *ne = img + (e - arc);
    wr32(ne + 68, (uint32_t)plen), wr32(ne + 72, (sf & ~SIZE_MASK) | (uint32_t)size), wr32(ne + 76, (uint32_t)len);

    mkdir("cache", 0755);
    mkdir("cache/btnicons", 0755);
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.tmp", out);
    FILE *f = fopen(tmp, "wb");
    if (!f) goto done;
    size_t total = (size_t)len + plen;
    ok = fwrite(img, 1, total, f) == total;
    ok = fclose(f) == 0 && ok;
    if (ok) ok = rename(tmp, out) == 0;
    if (!ok) remove(tmp);
    else LOG("[romfs] button icons swapped: %s\n", src);
done:
    free(arc), free(tex), free(packed), free(img);
    return ok;
}
