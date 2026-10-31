// [Controls] GameConfirm = South: what the game's buttons become (runtime/hle/gameconfirm.c). In the game's pad words
// the right face button (the Switch's A) is "decide" and the bottom one (B) "cancel" / roll.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define GAME_BTN_DECIDE 0x2000u  // right face button
#define GAME_BTN_CANCEL 0x4000u  // bottom face button
#define GAME_BTN_FACE (GAME_BTN_DECIDE | GAME_BTN_CANCEL)

enum { FIELD_AS_IS, FIELD_SWAP, FIELD_INTERACT };  // what the hunter gets: as pressed, the two swapped, bottom interacts

static inline uint32_t game_confirm_swap(uint32_t b) {
    return (b & ~GAME_BTN_FACE) | (b & GAME_BTN_DECIDE ? GAME_BTN_CANCEL : 0) | (b & GAME_BTN_CANCEL ? GAME_BTN_DECIDE : 0);
}

// Towns swap the two (the bottom button talks and opens boxes, the right one rolls); quests, while a prompt is shown,
// make the bottom button interact too, so nothing rolls under a prompt.
static inline int game_confirm_field_mode(bool town, bool prompt) {
    return town ? FIELD_SWAP : prompt ? FIELD_INTERACT : FIELD_AS_IS;
}

static inline uint32_t game_confirm_field(uint32_t b, int mode) {
    if (mode == FIELD_SWAP) return game_confirm_swap(b);
    if (mode == FIELD_INTERACT && (b & GAME_BTN_CANCEL)) return (b & ~GAME_BTN_CANCEL) | GAME_BTN_DECIDE;
    return b;
}
