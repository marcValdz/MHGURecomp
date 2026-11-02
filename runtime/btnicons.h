// [Controls] GameConfirm = South: the bottom and right face buttons' icons (btnicons.c).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// What the button icons look like in the archive `src` (romfs.c, at start, as the mods left it): the button font
// (menus, messages), the quest HUD's prompt button and the title's "Press" button, each with the bottom button's
// version. How many regions are known after that: btnicons_known().
void btnicons_learn(const char *src);
int btnicons_known(void);

// The renderer's copies of those textures: a BC3 texture's level 0 as the game copies it in, blocks row after row,
// which may be one (nvn/vulkan/exec.c, render thread); a texture going away (nvn/objects.c); and which button each
// shows, set every frame from the game's thread (runtime/hle/gameconfirm.c): the font and the title, and the prompts'
// button. The textures are NvnTexture *.
void btnicons_copied(void *texture, uint8_t *blocks, uint32_t w, uint32_t h, uint32_t format);
void btnicons_texture_gone(void *texture);
void btnicons_set(bool menus, bool prompts);