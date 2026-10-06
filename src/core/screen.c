#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "core/screen.h"

static size_t screen_count(const Screen *screen)
{
	if (screen == NULL || screen->size.x <= 0 || screen->size.y <= 0)
		return 0;

	return (size_t)screen->size.x * (size_t)screen->size.y;
}

static bool screen_ready(const Screen *screen)
{
	return screen != NULL && screen->screen_buffer != NULL && screen_count(screen) > 0;
}

static bool screen_alloc(Screen *screen, Vec2i size)
{
	Pixel *buf;
	size_t count;

	if (screen == NULL || size.x <= 0 || size.y <= 0)
		return false;

	count = (size_t)size.x * (size_t)size.y;
	buf = realloc(screen->screen_buffer, count * sizeof *buf);
	if (buf == NULL)
		return false;

	screen->screen_buffer = buf;
	screen->size = size;
	screen_clear(screen);
	return true;
}

bool screen_init(Screen *screen)
{
	struct winsize ws;
	Vec2i size;

	if (screen == NULL)
		return false;

	if (screen_ready(screen))
		return true;

	size = vec2i(80, 24);
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0)
		size = vec2i((int)ws.ws_col, (int)ws.ws_row);

	screen->size = VEC2I_ZERO;
	screen->screen_buffer = NULL;
	if (!screen_alloc(screen, size))
		return false;

	fputs("\x1b[?25l\x1b[2J\x1b[H", stdout);
	fflush(stdout);
	return true;
}

void screen_shutdown(Screen *screen)
{
	if (screen == NULL)
		return;

	if (screen_ready(screen))
	{
		fputs("\x1b[0m\x1b[?25h", stdout);
		fflush(stdout);
	}

	free(screen->screen_buffer);
	screen->screen_buffer = NULL;
	screen->size = VEC2I_ZERO;
}

Vec2i screen_size(const Screen *screen)
{
	if (screen == NULL)
		return VEC2I_ZERO;

	return screen->size;
}

void screen_set_size(Screen *screen, Vec2i size)
{
	if (screen == NULL || vec2i_eq(screen->size, size))
		return;

	(void)screen_alloc(screen, size);
}

Pixel *screen_buf(Screen *screen)
{
	if (!screen_ready(screen))
		return NULL;

	return screen->screen_buffer;
}

Pixel screen_get_pixel(const Screen *screen, Vec2i pos)
{
	Pixel blank = {' ', COLOR_DEFAULT, COLOR_DEFAULT};

	if (!screen_ready(screen) || !vec2i_in_bounds(pos, screen->size))
		return blank;

	return screen->screen_buffer[pos.y * screen->size.x + pos.x];
}

void screen_set_pixel(Screen *screen, Vec2i pos, Pixel pixel)
{
	screen_put(screen, pos.x, pos.y, pixel.ch, pixel.fg, pixel.bg, 0);
}

void screen_clear(Screen *screen)
{
	size_t i;
	size_t count;

	if (!screen_ready(screen))
		return;

	count = screen_count(screen);
	for (i = 0; i < count; i++)
	{
		screen->screen_buffer[i].ch = ' ';
		screen->screen_buffer[i].fg = COLOR_DEFAULT;
		screen->screen_buffer[i].bg = COLOR_DEFAULT;
	}
}

void screen_put(Screen *screen, int x, int y, char ch, Color fg, Color bg, int z)
{
	Pixel *cell;

	(void)z;
	if (!screen_ready(screen) || !vec2i_in_bounds(vec2i(x, y), screen->size))
		return;

	cell = &screen->screen_buffer[y * screen->size.x + x];
	cell->ch = ch;
	cell->fg = fg;
	cell->bg = bg;
}

void screen_flush(Screen *screen)
{
	int x;
	int y;
	Color fg = COLOR_DEFAULT;
	Color bg = COLOR_DEFAULT;
	size_t cap;
	size_t len = 0;
	char *out;
	ssize_t written;

	if (!screen_ready(screen))
		return;

	cap = screen_count(screen) * 12 + (size_t)screen->size.y * 16 + 64;
	out = malloc(cap);
	if (out == NULL)
		return;

	{
		const char *hide = "\x1b[?25l\x1b[0m";
		size_t n = strlen(hide);

		memcpy(out + len, hide, n);
		len += n;
	}

	for (y = 0; y < screen->size.y; y++)
	{
		char escape[32];
		int n;

		n = snprintf(escape, sizeof escape, "\x1b[%d;1H", y + 1);
		if (n > 0 && len + (size_t)n <= cap)
		{
			memcpy(out + len, escape, (size_t)n);
			len += (size_t)n;
		}

		for (x = 0; x < screen->size.x; x++)
		{
			const Pixel *cell = &screen->screen_buffer[y * screen->size.x + x];

			if (cell->fg != fg || cell->bg != bg)
			{
				n = snprintf(escape, sizeof escape, "\x1b[%d;%dm",
					     color_fg_code(cell->fg), color_bg_code(cell->bg));
				if (n > 0 && len + (size_t)n <= cap)
				{
					memcpy(out + len, escape, (size_t)n);
					len += (size_t)n;
				}

				fg = cell->fg;
				bg = cell->bg;
			}

			if (len < cap)
				out[len++] = cell->ch;
		}
	}

	{
		const char *reset = "\x1b[0m";
		size_t n = strlen(reset);

		if (len + n <= cap)
		{
			memcpy(out + len, reset, n);
			len += n;
		}
	}

	written = write(STDOUT_FILENO, out, len);
	(void)written;
	free(out);
	screen_clear(screen);
}
