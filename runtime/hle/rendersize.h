// The 3D picture's size changed while the game runs (runtime/hle/rendersize.c).
#pragma once
#include <stdbool.h>
#include <stdint.h>

void rendersize_init(void);    // the hook on the game's render system step, registered (set by rendersize_follow)
void rendersize_follow(void);  // the renderer, each present: hooked once a size other than the start-up one is wanted
// w x h is the start-up render size and another size has been drawn at since: a texture of it (the window texture)
// may hold a smaller picture in its top-left (the part a frame drew: nvn/vulkan/exec.c vk_drawn_part).
bool rendersize_startup(uint32_t w, uint32_t h);
struct NvnTexture;
bool rendersize_window(const struct NvnTexture *t);  // t is one of the game's window textures (the ones presented)
bool rendersize_switching(void);  // the size switched within the last few frames
// The window textures are larger than the start-up size w0 x h0 (a larger picture); the game's last draw into them
// covers at most that size (nvn/vulkan/exec.c stretches it over them)
bool rendersize_grown(uint32_t *w0, uint32_t *h0);
bool rendersize_settled(void);  // the game draws at the size wanted (no switch pending): the renderer's sizes follow
