// [Controls] GameConfirm = South: the game's menus confirm with the bottom face button and go back with the right one,
// as newer Monster Hunter games do; in towns the bottom button also talks and opens boxes (the two swap there), and in
// quests it also interacts while the hunter has a prompt on screen (gather, carve, supply box). Ported from the
// confirm-on-x Forge plugin (github.com/lick-mee/mhgu-confirm-on-x, MIT), which found the game code below.
//
// sFestaPad keeps, besides each pad's raw state the hunter reads, a menu input object: every frame it takes the pad's
// held buttons through a getter (FN_MENU_BUTTONS) and menus test its decide / cancel bits. So swapping those two in the
// getter's result moves confirm / cancel in menus only. The hunter's buttons are changed where every pad's state is
// read (FN_PAD_READ), and the menu getter puts back the buttons actually pressed (swapped), whatever the hunter got.
// The hunter's mode only changes while neither face button is held, so a press that starts a gather can't turn into
// a roll when the prompt goes away under it. The button icons are swapped in the game's files (runtime/btnicons.c).
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "../config.h"
#include "../game_confirm.h"
#include "../hle.h"
#include "../mods.h"

// Game code and data (main, v1.4.0; runtime addresses)
#define FN_MENU_BUTTONS 0x5de4f8u   // (menu input, pad's held buttons) -> them with the left stick folded into the D-pad
#define FN_PAD_READ     0xdac204u   // (pad, ?, ?, state *): a pad's raw state; state[0] its buttons' low word
#define FN_MY_PLAYER    0x476278u   // (sPlayer) -> our hunter
#define G_SPLAYER       0x1a85b58u
#define G_SQUEST        0x1a8ad90u  // sQuest *: +0x1c the quest timer, running only on a quest (0 in towns)
#define PLAYER_PROMPT   0x32b4u     // hunter: the interaction prompt shown (1..35, 0 none; quests only)
#define PADS            8

static bool sane(uint32_t a) { return a >= 0x08000000u && a < 0xF0000000u && !(a & 3); }

static int g_mode = FIELD_AS_IS, g_quest = -1;
static uint64_t g_prompt_at;  // the hunter's prompt is read at most every 4 ms (a frame's pads read together)
static bool g_prompt;
static struct { uint32_t pad, pressed, sent; } g_pads[PADS];  // what each pad had and what the hunter got

static bool active(void) {
    return config_live_game_confirm() == GAME_CONFIRM_SOUTH && config_live_face_buttons() == FACE_POSITION;
}

static bool prompt_shown(Cpu *c) {
    uint64_t now = plat_now_ns();
    if (now - g_prompt_at < 4000000) return g_prompt;
    g_prompt_at = now, g_prompt = false;
    uint32_t players = LD32(G_SPLAYER);
    if (!sane(players)) return false;
    uint32_t saved[16];
    memcpy(saved, c->r, sizeof saved);
    c->r[0] = players;
    guest_call(c, FN_MY_PLAYER);
    uint32_t me = c->r[0];
    memcpy(c->r, saved, sizeof saved);
    return g_prompt = sane(me) && LD32(me + PLAYER_PROMPT) != 0;
}

static int field_mode(Cpu *c) {
    uint32_t q = LD32(G_SQUEST);
    int quest = sane(q) && LD32(q + 0x1c) != 0;  // (no sQuest yet: a town)
    if (quest != g_quest) LOG("[gameconfirm] %s\n", quest ? "quest: the bottom button interacts at prompts" : "town: face buttons swapped");
    g_quest = quest;
    return game_confirm_field_mode(!quest, quest && prompt_shown(c));
}

static void pad_read(Cpu *c) {
    uint32_t pad = c->r[0], state = c->r[3];
    fnhook_call_original(c, FN_PAD_READ);
    if (!sane(state)) return;
    uint32_t pressed = LD32(state), sent = pressed;
    if (active()) {
        uint32_t r0 = c->r[0];
        if (!(pressed & GAME_BTN_FACE)) g_mode = field_mode(c);
        c->r[0] = r0;
        sent = game_confirm_field(pressed, g_mode);
        ST32(state, sent);
    }
    for (int i = 0; i < PADS; i++)
        if (g_pads[i].pad == pad || !g_pads[i].pad) {
            g_pads[i].pad = pad, g_pads[i].pressed = pressed, g_pads[i].sent = sent;
            break;
        }
}

static void menu_buttons(Cpu *c) {
    uint32_t held = c->r[1];
    fnhook_call_original(c, FN_MENU_BUTTONS);
    if (!active()) return;
    uint32_t b = c->r[0], pressed = b & GAME_BTN_FACE;
    for (int i = 0; i < PADS && g_pads[i].pad; i++)
        if (!((g_pads[i].sent ^ held) & 0xffff)) {  // (the pad these buttons came from: what was really pressed)
            pressed = g_pads[i].pressed & GAME_BTN_FACE;
            break;
        }
    c->r[0] = (b & ~GAME_BTN_FACE) | game_confirm_swap(pressed);
}

void gameconfirm_init(void) {
    if (!fnhook_set(FN_PAD_READ, hle_register("gameconfirm:pad_read", pad_read)) ||
        !fnhook_set(FN_MENU_BUTTONS, hle_register("gameconfirm:menu_buttons", menu_buttons))) {
        LOG("[gameconfirm] the game's pad code wasn't found: GameConfirm does nothing\n");
        return;
    }
    const char *layers[64];
    int n = mods_romfs_layers(layers, 64);
    for (int i = 0; i < n; i++) {
        char path[1100];
        struct stat st;
        snprintf(path, sizeof path, "%s/nativeNX/plugins/confirm_on_x.nro", layers[i]);
        if (!stat(path, &st))
            LOG("[gameconfirm] the confirm-on-x mod is installed (%s): with GameConfirm = South both swap the buttons, "
                "so they swap back; remove one\n", layers[i]);
    }
    LOG("[gameconfirm] GameConfirm = %s\n", config_live_game_confirm() == GAME_CONFIRM_SOUTH ? "South" : "Original");
}
