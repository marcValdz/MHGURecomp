// [Controls] GameConfirm = South: what the game's buttons become (runtime/hle/gameconfirm.c), as newer Monster Hunter
// games lay them out: the bottom button confirms, talks and opens boxes, the right one cancels, gathers and carves. In
// the game's pad words the right face button (the Switch's A) is "decide" and the bottom one (B) "cancel" / roll.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define GAME_BTN_DECIDE 0x2000u  // right face button
#define GAME_BTN_CANCEL 0x4000u  // bottom face button
#define GAME_BTN_FACE (GAME_BTN_DECIDE | GAME_BTN_CANCEL)

// What the hunter gets: as pressed; the two swapped (towns: the bottom button talks and opens boxes, the right one
// rolls); or at a box's prompt in a quest, the bottom button opens it and the right one does nothing
enum { FIELD_AS_IS, FIELD_SWAP, FIELD_BOX };

// The quest prompts (the hunter's +0x32b4, 1..35) that are facilities, boxes and talking, done with the bottom
// button; the others (gather, carve, fish, climb...) keep the right one. Seen: 5 the supply box, 9 the delivery box,
// 8 the camp's Felyne, 17 the camp's bed, 14 a ballista, 16 a siege weapon's mount. By their HUD icon (not seen yet):
// 33 the supply box's, and 12, 26-29, 31, 32, 34 the siege weapons' (16's).
#define GAME_BOX_PROMPTS (1ull << 5 | 1ull << 8 | 1ull << 9 | 1ull << 12 | 1ull << 14 | 1ull << 16 | 1ull << 17 | \
                          1ull << 26 | 1ull << 27 | 1ull << 28 | 1ull << 29 | 1ull << 31 | 1ull << 32 | 1ull << 33 | \
                          1ull << 34)
static inline bool game_confirm_box_prompt(uint32_t prompt) {
    return prompt < 64 && (GAME_BOX_PROMPTS >> prompt & 1);
}

static inline uint32_t game_confirm_swap(uint32_t b) {
    return (b & ~GAME_BTN_FACE) | (b & GAME_BTN_DECIDE ? GAME_BTN_CANCEL : 0) | (b & GAME_BTN_CANCEL ? GAME_BTN_DECIDE : 0);
}

static inline int game_confirm_field_mode(bool town, uint32_t prompt) {
    return town ? FIELD_SWAP : game_confirm_box_prompt(prompt) ? FIELD_BOX : FIELD_AS_IS;
}

static inline uint32_t game_confirm_field(uint32_t b, int mode) {
    if (mode == FIELD_SWAP) return game_confirm_swap(b);
    if (mode == FIELD_BOX) return (b & ~GAME_BTN_FACE) | (b & GAME_BTN_CANCEL ? GAME_BTN_DECIDE : 0);
    return b;
}
