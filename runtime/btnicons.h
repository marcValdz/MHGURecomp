// [Controls] GameConfirm = South: the bottom and right face buttons' icons (btnicons.c).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Writes to a cache file (path in `out`) the archive `src` with the two icons swapped in its button font texture.
// False when the archive has no such texture, or one laid out in a way not known here: the caller keeps `src`.
bool btnicons_patch(const char *src, char *out, size_t outsz);

// The field prompts' button (one sprite for every prompt, in the quest HUD texture): what it looks like in the
// archive `src` (romfs.c, at start); a BC3 texture's level 0 as the game copies it in, blocks row after row, which may
// be that texture (nvn/vulkan/exec.c, render thread); a texture going away (nvn/objects.c); and which button it shows,
// set every frame from the game's thread (runtime/hle/gameconfirm.c). The textures are NvnTexture *.
void btnicons_learn_hud(const char *src);
void btnicons_copied(void *texture, uint8_t *blocks, uint32_t w, uint32_t h, uint32_t format);
void btnicons_texture_gone(void *texture);
void btnicons_hud_bottom(bool bottom);
