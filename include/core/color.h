#ifndef STGE_CORE_COLOR_H
#define STGE_CORE_COLOR_H

#include <stdbool.h>

/* Terminal colors. COLOR_DEFAULT leaves the terminal's own choice alone.
   COLOR_LIGHT* are the bright/aixterm set (90–97 / 100–107). */
typedef enum
{
	COLOR_DEFAULT = -1,
	COLOR_BLACK = 0,
	COLOR_RED,
	COLOR_GREEN,
	COLOR_YELLOW,
	COLOR_BLUE,
	COLOR_MAGENTA,
	COLOR_CYAN,
	COLOR_WHITE,
	COLOR_LIGHTBLACK,
	COLOR_LIGHTRED,
	COLOR_LIGHTGREEN,
	COLOR_LIGHTYELLOW,
	COLOR_LIGHTBLUE,
	COLOR_LIGHTMAGENTA,
	COLOR_LIGHTCYAN,
	COLOR_LIGHTWHITE
} Color;

/* "blue", "lightblue", "default". Returns false if the name is unknown. */
bool color_parse(const char *s, Color *out);
Color color_named(const char *s);
int color_fg_code(Color color);
int color_bg_code(Color color);

#endif /* STGE_CORE_COLOR_H */
