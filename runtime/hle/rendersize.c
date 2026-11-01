// The 3D picture's size changed while the game runs ([Display] Resolution, RenderResolution, AspectRatio, Mode,
// [Upscaler] FSR, FSRQuality: config_live_sizes).
//
// The game's render system (f_dbca50's object, [[0x1a38db0]]) keeps two sizes: slot 0 (docked: g_render_size at
// start-up, gen.py PATCHES) and slot 1 (handheld). Each of its render targets (a "listener": array +0x44bae4, count
// +0x44bad8) is made with both: +0x1e8 its slot-0 size, +0x1f0 its slot-1 size (slot 0 x the ratio, f_d084a4), an NVN
// texture for each at the same storage (+0x40 slot 0, +0x100 slot 1, 0xc0 bytes each), and a child whose +0x50 / +0x58
// hold the same two sizes. Every frame f_dbe154 switches slots when the pending operation / performance modes say so
// (+0x44baf0 / +0x44baf4: 1 / 1 slot 0, 0 / 0 slot 1): the current size +0xb0 comes from the display-mode list entry
// of that slot (+0x44b150 + slot * 8 points at it: +0x50 / +0x54), and f_dbe8ac points each listener at that slot's
// texture and size. Nothing is allocated: the runtime makes an image per NVN texture, at its size.
//
// So a live size: the slot the game is not drawing with is rewritten (each listener's size in it, its texture there -
// its image made again at the next use -, its child's, the slot's display-mode entry), then the game switches to it;
// the next size goes into the other slot, and so on (each listener's own start-up size is kept here: slot 0 is
// rewritten too). A slot is rewritten only 3 frames after the game left it (frames already recorded may still draw
// into it). The size at +0x44bac8 (g_ui_size, the handheld size the slot-1 ratios come from) is left alone.
// A size larger than the start-up one works the same way (the images are the runtime's, made at each texture's size);
// the window textures follow it (window_follow).
// Hooked at f_dbe154's entry (the guest's frame, before the switch it makes) only while a size other than the start-up
// one is wanted or drawn at, and only once the render system was checked against these offsets (check); after it, g_render_size and
// g_render_aspect follow the size the game draws at (the renderer's checks, the cameras, fsr.c), g_ui_anchor_aspect
// UIAnchor on that picture (exec.c, per HUD draw).
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "rendersize.h"
#include "../config.h"
#include "../hle.h"
#include "../nvn/nvn.h"
#include "../platform.h"

#define FN_RS_FRAME     0xdbe154u   // the render system's per-frame step (r0: the render system)
#define G_RENDER_SYSTEM 0x1a38db0u  // -> -> the render system
#define RS_CUR          0xb0u       // the size drawn at now (w, h)
#define RS_SLOT1_MADE   0x44bab4u   // (u8) the slot-1 textures exist
#define RS_SLOT0        0x44bac0u   // slot 0's size (w, h): the start-up size
#define RS_INDEX        0x44bad0u   // the slot in use
#define RS_NLISTENERS   0x44bad8u
#define RS_LISTENERS    0x44bae4u
#define RS_PENDING      0x44baf0u   // operation mode, performance mode (-1: none)
#define RS_MODE_KEY     0x44b150u   // per slot (8 bytes): its display-mode list entry (+0x50 w, +0x54 h)
#define RS_NMODES       0x44b14cu   // the display-mode list: its count, its entries (0x5c bytes each, a name first)
#define RS_MODES        0x44554cu
#define L_CUR           0x1eu       // (u16 w, then the height's 14 bits at +0x20) the size in use
#define L_SIZE          0x1e8u      // + slot * 8: w, h
#define L_TEX           0x40u       // + slot * 0xc0: the NVN texture (its first 8 bytes: the runtime's NvnTexture)
#define L_KIDS2         0x204u      // (kind 2) children
#define L_KIDS5         0x208u      // (kind 5) children
#define L_NKIDS         0x20cu
#define K_SIZE          0x50u       // + slot * 8: w, h

static bool ok_addr(uint32_t a) { return a >= 0x1000u && !(a & 3); }

static uint32_t render_system(void) {
    uint32_t g = LD32(G_RENDER_SYSTEM);
    return ok_addr(g) && ok_addr(LD32(g)) ? LD32(g) : 0;
}

static int w0_, h0_;     // the start-up size (slot 0 as the game made it)
static float aspect0;    // and its shape
typedef struct { int w, h; float aspect; bool valid; } Slot;
static Slot slots[2];    // what each slot holds (slot 1: unknown until written here)
static int in_cur;       // frames since the game switched to the slot it draws with
static uint32_t idx_last = 2;
static bool used;                    // a size other than the start-up one was drawn at (since start-up)
static _Atomic bool settled = true;  // the game draws at the size wanted (the renderer's sizes may follow: fsr.c)

// Each target's (and child's) start-up size, by its address: the base the sizes of both slots are made from. A size
// at an address that is neither the base nor what was written there last is a new object there: based again.
#define BASES 2048
static struct { uint32_t addr; int w, h, written[2][2]; } bases[BASES];
static int nbases;
static uint32_t live_s;  // the render system (bases_compact: the targets alive now)
static bool alive(uint32_t addr) {
    uint32_t n = LD32(live_s + RS_NLISTENERS), arr = LD32(live_s + RS_LISTENERS);
    for (uint32_t i = 0; ok_addr(arr) && i < n && n <= 4096; i++) {
        uint32_t L = LD32(arr + 4 * i);
        if (L == addr) return true;
        if (!ok_addr(L)) continue;
        uint32_t nk = LD32(L + L_NKIDS);
        for (int k = 0; k < 2 && nk <= 256; k++) {
            uint32_t ka = LD32(L + (k ? L_KIDS5 : L_KIDS2));
            for (uint32_t j = 0; ok_addr(ka) && j < nk; j++)
                if (LD32(ka + 4 * j) == addr) return true;
        }
    }
    return false;
}
static void bases_compact(void) {  // (full: the entries of objects no longer there dropped)
    int k = 0;
    for (int i = 0; i < nbases; i++)
        if (alive(bases[i].addr)) bases[k++] = bases[i];
    nbases = k;
}
static int *base_of(uint32_t addr, int cw0, int ch0) {
    int i = 0;
    while (i < nbases && bases[i].addr != addr) i++;
    if (i == nbases) {
        if (nbases == BASES) bases_compact(), i = nbases;
        if (nbases == BASES) return NULL;
        nbases++;
        bases[i].addr = addr, bases[i].w = cw0, bases[i].h = ch0;
        memset(bases[i].written, 0, sizeof bases[i].written);
    } else if ((cw0 != bases[i].w || ch0 != bases[i].h) && (cw0 != bases[i].written[0][0] || ch0 != bases[i].written[0][1])) {
        bases[i].w = cw0, bases[i].h = ch0;  // (a new object at this address)
        memset(bases[i].written, 0, sizeof bases[i].written);
    }
    return &bases[i].w;
}
static void note_written(uint32_t addr, int slot, int w, int h) {
    for (int i = 0; i < nbases; i++)
        if (bases[i].addr == addr) { bases[i].written[slot][0] = w, bases[i].written[slot][1] = h; return; }
}

// A target's size for a 3D picture of W x H, from its start-up size bw x bh (f_d084a4's rounding for the others)
static void size_of(int bw, int bh, int W, int H, int *w, int *h) {
    if (W == w0_ && H == h0_) { *w = bw, *h = bh; return; }
    if (bw == w0_ && bh == h0_) { *w = W, *h = H; return; }
    *w = ((int)((float)bw * (float)W / (float)w0_) + 3) & ~3;
    *h = ((int)((float)bh * (float)H / (float)h0_) + 3) & ~3;
}

// Every target's size in slot as wanted for W x H (and, with fix, written so). false: one is not (a new target).
static bool slot_check(uint32_t S, int slot, int W, int H, bool fix) {
    uint32_t n = LD32(S + RS_NLISTENERS), arr = LD32(S + RS_LISTENERS);
    if (!ok_addr(arr) || n > 4096) return true;
    bool all = true;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t L = LD32(arr + 4 * i);
        if (!ok_addr(L)) continue;
        int *b = base_of(L, (int)LD32(L + L_SIZE), (int)LD32(L + L_SIZE + 4));
        if (!b) continue;
        int w, h;
        size_of(b[0], b[1], W, H, &w, &h);
        uint32_t at = L + L_SIZE + 8u * (uint32_t)slot;
        bool same = (int)LD32(at) == w && (int)LD32(at + 4) == h;
        all &= same;
        if (!fix || same) continue;
        ST32(at, w), ST32(at + 4, h);
        if (!slot) note_written(L, 0, w, h);
        NvnTexture *t = HOST(NvnTexture, L + L_TEX + 0xc0u * (uint32_t)slot);
        if (t && (t->width != (uint32_t)w || t->height != (uint32_t)h)) {
            backend_texture_destroyed(t);  // (deferred; its image is made again at the new size at its next use)
            t->width = (uint32_t)w, t->height = (uint32_t)h;
        }
        uint32_t nk = LD32(L + L_NKIDS);
        for (int k = 0; k < 2 && nk <= 256; k++) {
            uint32_t ka = LD32(L + (k ? L_KIDS5 : L_KIDS2));
            if (!ok_addr(ka)) continue;
            for (uint32_t j = 0; j < nk; j++) {
                uint32_t K = LD32(ka + 4 * j);
                if (!ok_addr(K)) continue;
                int *kb = base_of(K, (int)LD32(K + K_SIZE), (int)LD32(K + K_SIZE + 4));
                if (!kb) continue;
                int kw, kh;
                size_of(kb[0], kb[1], W, H, &kw, &kh);
                ST32(K + K_SIZE + 8u * (uint32_t)slot, kw), ST32(K + K_SIZE + 8u * (uint32_t)slot + 4, kh);
                if (!slot) note_written(K, 0, kw, kh);
            }
        }
    }
    return all;
}

// Each target points at the slot in use (one made since the switch may still have the other's size).
static bool targets_follow(uint32_t S, uint32_t idx) {
    uint32_t n = LD32(S + RS_NLISTENERS), arr = LD32(S + RS_LISTENERS);
    if (!ok_addr(arr) || n > 4096) return true;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t L = LD32(arr + 4 * i);
        if (!ok_addr(L)) continue;
        uint32_t w = LD32(L + L_SIZE + 8 * idx), h = LD32(L + L_SIZE + 8 * idx + 4);
        if (LD16(L + L_CUR) != (w & 0xffff) || (LD16(L + L_CUR + 2) & 0x3fff) != (h & 0x3fff)) return false;
    }
    return true;
}

// f_dbe154 finds a slot's entry by the name its key points at (strcmp), and the game names its entries after their
// size, one per name (f_dbc4a8): a start-up size equal to the handheld one (1280x720, g_ui_size: FSR Quality at
// 1920x1080) leaves slot 1 without a key, and no switch to it happens. The list's other entry is renamed and made its key.
static bool slot1_key(uint32_t S) {
    uint32_t k0 = LD32(S + RS_MODE_KEY), k1 = LD32(S + RS_MODE_KEY + 8), n = LD32(S + RS_NMODES);
    if (ok_addr(k1) && k1 != k0) return true;
    for (uint32_t i = 0; i < n && n <= 64; i++) {
        uint32_t e = S + RS_MODES + 0x5cu * i;
        if (e == k0) continue;
        static const char name[] = "rendersize";
        for (uint32_t j = 0; j < sizeof name; j++) ST8(e + j, (uint8_t)name[j]);
        ST32(S + RS_MODE_KEY + 8, e);
        LOG("[rendersize] slot 1 had no display mode of its own (the start-up size is the handheld one): entry %u\n", i);
        return true;
    }
    return false;
}

static void post(uint32_t S, int slot) {
    uint32_t mode = slot ? 0u : 1u;  // (1 / 1: slot 0, 0 / 0: slot 1)
    ST32(S + RS_PENDING, mode), ST32(S + RS_PENDING + 4, mode);
}

// The size wanted now (config_live_sizes)
static bool wanted(int rs[2], float *aspect) {
    int out[2];
    config_live_sizes(rs, out, aspect);
    return rs[0] > 0 && rs[1] > 0 && w0_;
}

// The window textures (the game's 3, presented in turn: nvnWindowInitialize) are not targets: made at the start-up
// size, the game draws a smaller picture into their top-left. For a larger picture they are made again at its size
// (their images at their next use), at the start-up size again once it fits.
static int pw_, ph_;  // their size now
static uint64_t switched_at;  // g_present_count at the last switch
static NvnTexture *window_tex(int i) {
    NvnWindow *win = g_nvn_window;
    uint32_t a = win && i < win->num ? (uint32_t)win->textures[i] : 0;
    return ok_addr(a) ? HOST(NvnTexture, a) : NULL;
}
static void window_follow(int w, int h) {
    if (w <= w0_ && h <= h0_) w = w0_, h = h0_;
    if (!g_nvn_window || (w == pw_ && h == ph_)) return;
    uint64_t t0 = plat_now_ns();
    for (int i = 0; i < g_nvn_window->num; i++) {
        NvnTexture *t = window_tex(i);
        if (!t || (t->width == (uint32_t)w && t->height == (uint32_t)h)) continue;
        backend_texture_destroyed(t);
        t->width = (uint32_t)w, t->height = (uint32_t)h;
    }
    LOG("[live] window textures %dx%d -> %dx%d (%.1f ms)\n", pw_, ph_, w, h, (plat_now_ns() - t0) / 1e6);
    pw_ = w, ph_ = h;
}

static void before(uint32_t S) {
    if (!LD8(S + RS_SLOT1_MADE) || (int32_t)LD32(S + RS_PENDING) >= 0) return;
    if (!w0_) {
        w0_ = (int)LD32(S + RS_SLOT0), h0_ = (int)LD32(S + RS_SLOT0 + 4), aspect0 = g_render_aspect;
        pw_ = w0_, ph_ = h0_;
        slots[0] = (Slot){w0_, h0_, aspect0, true};
        LOG("[rendersize] the game's render system: start-up size %dx%d, %u targets\n", w0_, h0_, LD32(S + RS_NLISTENERS));
    }
    int rs[2];
    float aspect;
    if (!wanted(rs, &aspect)) return;
    uint32_t idx = LD32(S + RS_INDEX) ? 1 : 0, other = idx ^ 1;
    in_cur = idx == idx_last ? in_cur + 1 : 0;
    idx_last = idx;
    Slot want = {rs[0], rs[1], aspect, true};
    bool cur_ok = slots[idx].valid && slots[idx].w == want.w && slots[idx].h == want.h && slots[idx].aspect == aspect;
    if (cur_ok && (in_cur < 2 || (slot_check(S, (int)idx, want.w, want.h, false) && targets_follow(S, idx)))) {
        settled = true;
        return;
    }
    settled = false;
    // (the size in use is not the one wanted, or a target made since has another: the other slot gets it)
    bool other_ok = slots[other].valid && slots[other].w == want.w && slots[other].h == want.h &&
                    slots[other].aspect == aspect && slot_check(S, (int)other, want.w, want.h, false);
    if (!slot1_key(S)) return;
    if (!other_ok) {
        if (in_cur < 3) return;  // (the frames recorded before the last switch may still draw into the other slot)
        uint64_t t0 = plat_now_ns();
        slot_check(S, (int)other, want.w, want.h, true);
        uint32_t e = LD32(S + RS_MODE_KEY + 8 * other);  // (that slot's display-mode entry: the size the game takes)
        if (ok_addr(e)) ST32(e + 0x50, want.w), ST32(e + 0x54, want.h);
        slots[other] = want;
        LOG("[live] render size: slot %u made %dx%d (%.1f ms)%s\n", other, want.w, want.h, (plat_now_ns() - t0) / 1e6,
            cur_ok ? " (a target was not on the size in use)" : "");
    }
    post(S, (int)other);
}

// The HUD's width (exec.c): UIAnchor as saved now on the picture drawn now. After the game's step while hooked, and
// each present (rendersize_follow), so UIAnchor alone needs no hook.
static void hud_follow(void) {
    float ui = config_ui_aspect(config_live_ui_anchor(), g_render_aspect);
    if (ui != g_ui_anchor_aspect) {
        LOG("[live] HUD width %.3f -> %.3f\n", g_ui_anchor_aspect, ui);
        g_ui_anchor_aspect = ui;
    }
}

// After the game's step: the renderer and the cameras follow the size it draws at now, the HUD (exec.c) the width
// UIAnchor gives it on that picture.
static void after(uint32_t S) {
    uint32_t w = LD32(S + RS_CUR), h = LD32(S + RS_CUR + 4);
    if (w && h && (w != g_render_size[0] || h != g_render_size[1])) {
        uint32_t idx = LD32(S + RS_INDEX) ? 1 : 0;
        float aspect = slots[idx].valid ? slots[idx].aspect : g_render_aspect;
        LOG("[live] render size %ux%u -> %ux%u (shape %.3f)\n", g_render_size[0], g_render_size[1], w, h, aspect);
        g_render_aspect = aspect;
        g_render_size[0] = w, g_render_size[1] = h;
        used = true;
        switched_at = g_present_count;
        window_follow((int)w, (int)h);
    }
    hud_follow();
}

// The hook is set only while a size other than the start-up one is wanted or still drawn at (rendersize_follow), so
// a game whose sizes are never changed runs the render system as upstream's. Before the first write the render system
// is checked against what this file reads it as (check): anything else and sizes change at a restart only.
static uint32_t d_frame;     // the detour (registered once: rendersize_init)
static _Atomic bool hooked;
static bool off;             // the render system did not look as expected (logged): config_sizes_live(false)
static uint32_t start_w, start_h;
static float start_aspect;

// Why the render system is not what the offsets above describe (NULL: it is): its start-up size is the one the game
// was given, the size in use is slot 0's, each target's slot-0 size is its texture's (the runtime's own record of it),
// and slot 0's display-mode entry has that size.
static const char *check(uint32_t S) {
    uint32_t w = LD32(S + RS_SLOT0), h = LD32(S + RS_SLOT0 + 4), idx = LD32(S + RS_INDEX);
    if (w != start_w || h != start_h) return "its slot 0 is not the start-up size";
    if (idx > 1) return "its slot index is neither 0 nor 1";
    if (idx == 0 && (LD32(S + RS_CUR) != w || LD32(S + RS_CUR + 4) != h)) return "the size in use is not slot 0's";
    uint32_t n = LD32(S + RS_NLISTENERS), arr = LD32(S + RS_LISTENERS);
    if (!ok_addr(arr) || !n || n > 4096) return "its render target list";
    for (uint32_t i = 0; i < n; i++) {
        uint32_t L = LD32(arr + 4 * i);
        if (!ok_addr(L)) return "a render target's address";
        uint32_t tw = LD32(L + L_SIZE), th = LD32(L + L_SIZE + 4);
        if (!tw || !th || tw > 16384 || th > 16384) return "a render target's size";
        NvnTexture *t = HOST(NvnTexture, L + L_TEX);
        if (t && (t->width != tw || t->height != th)) return "a render target's size is not its texture's";
    }
    uint32_t e = LD32(S + RS_MODE_KEY);
    if (!ok_addr(e) || LD32(e + 0x50) != w || LD32(e + 0x54) != h) return "its slot-0 display mode";
    return NULL;
}

// After the game's step at the start-up size again, on slot 0, nothing else wanted: unhooked (rendersize_follow sets
// it again when another size is wanted).
static void maybe_unhook(uint32_t S) {
    int rs[2];
    float aspect;
    if (!wanted(rs, &aspect) || (uint32_t)rs[0] != start_w || (uint32_t)rs[1] != start_h || aspect != start_aspect) return;
    if (LD32(S + RS_INDEX) || in_cur < 3 || !settled || g_render_size[0] != start_w || g_render_size[1] != start_h) return;
    hooked = false;
    fnhook_set(FN_RS_FRAME, 0);
    LOG("[rendersize] back at the start-up size: unhooked\n");
}

static void rs_frame(Cpu *c) {
    uint32_t S = render_system();
    live_s = S;
    if (S) before(S);
    fnhook_call_original(c, FN_RS_FRAME);
    if (S) after(S), maybe_unhook(S);
}

void rendersize_init(void) {
    d_frame = hle_register("rendersize:frame", rs_frame);
    start_w = g_render_size[0], start_h = g_render_size[1], start_aspect = g_render_aspect;
}

void rendersize_follow(void) {
    hud_follow();
    if (off || hooked) return;
    int rs[2], out[2];
    float aspect;
    config_live_sizes(rs, out, &aspect);
    if (rs[0] <= 0 || rs[1] <= 0 || ((uint32_t)rs[0] == start_w && (uint32_t)rs[1] == start_h && aspect == start_aspect))
        return;
    uint32_t S = render_system();
    if (!S || !LD8(S + RS_SLOT1_MADE)) return;  // (not made yet: the next frame)
    const char *why = check(S);
    if (why) {
        off = true;
        config_sizes_live(false);
        LOG("[rendersize] the game's render system is not as expected (%s): the render size changes at the next start\n", why);
        return;
    }
    hooked = true;
    fnhook_set(FN_RS_FRAME, d_frame);
    LOG("[rendersize] %dx%d wanted: the render system's step hooked\n", rs[0], rs[1]);
}

bool rendersize_startup(uint32_t w, uint32_t h) { return used && (int)w == pw_ && (int)h == ph_; }
bool rendersize_grown(uint32_t *w0, uint32_t *h0) {
    *w0 = (uint32_t)w0_, *h0 = (uint32_t)h0_;
    return pw_ > w0_ || ph_ > h0_;
}
bool rendersize_switching(void) {
    uint64_t n = g_present_count;
    return used && (n > switched_at ? n - switched_at : switched_at - n) <= 8;
}
bool rendersize_window(const NvnTexture *t) {
    if (!hooked && !used) return false;  // (sizes never changed: the renderer's window-texture paths stay out)
    for (int i = 0; g_nvn_window && i < g_nvn_window->num; i++)
        if (window_tex(i) == t) return true;
    return false;
}
bool rendersize_settled(void) {
    int rs[2];
    float aspect;
    if (!wanted(rs, &aspect)) return true;  // (before the render system: the start-up size)
    return settled && (uint32_t)rs[0] == g_render_size[0] && (uint32_t)rs[1] == g_render_size[1];
}
