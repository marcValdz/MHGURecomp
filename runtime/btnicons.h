// [Controls] GameConfirm = South: the bottom and right face buttons' icons swapped in the game's button font (romfs.c).
#pragma once
#include <stdbool.h>
#include <stddef.h>

// Writes to a cache file (path in `out`) the archive `src` with the two icons swapped in its button font texture.
// False when the archive has no such texture, or one laid out in a way not known here: the caller keeps `src`.
bool btnicons_patch(const char *src, char *out, size_t outsz);
