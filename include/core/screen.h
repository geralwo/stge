#ifndef STGE_CORE_SCREEN_H
#define STGE_CORE_SCREEN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/color.h"
#include "utils/vec.h"

typedef struct
{
	char ch;
	Color fg;
	Color bg;
} Pixel;

typedef struct
{
	Vec2i size;
	Pixel *screen_buffer;
} Screen;

/* Samples the window size once, hides the cursor and clears the terminal. */
bool screen_init(Screen *screen);
void screen_shutdown(Screen *screen);

Vec2i screen_size(const Screen *screen);
void screen_set_size(Screen *screen, Vec2i size);

Pixel *screen_buf(Screen *screen);
Pixel screen_get_pixel(const Screen *screen, Vec2i pos);
void screen_set_pixel(Screen *screen, Vec2i pos, Pixel pixel);

/* Resets every cell to a blank. */
void screen_clear(Screen *screen);
/* Writes one cell. Out of bounds writes are dropped. z is ignored; the later
   write wins. */
void screen_put(Screen *screen, int x, int y, char ch, Color fg, Color bg, int z);

/* Writes the frame to stdout, then clears the pixel buffer. */
void screen_flush(Screen *screen);

#endif /* STGE_CORE_SCREEN_H */
