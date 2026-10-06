#ifndef STGE_CORE_INPUT_H
#define STGE_CORE_INPUT_H

#include <stdbool.h>
#include <stdint.h>
#include <termios.h>

#define KEY_MAX 512
#define INPUT_BUF 64
#define STGE_INPUT_PRESSED_BYTES ((KEY_MAX + 63) / 64 * (int)sizeof(uint64_t))

/* ASCII keys keep their byte values. The rest start at 256. */
typedef enum
{
	KEY_NONE = 0,
	KEY_TAB = 9,
	KEY_ENTER = 13,
	KEY_ESCAPE = 27,
	KEY_SPACE = 32,
	KEY_0 = '0',
	KEY_1 = '1',
	KEY_2 = '2',
	KEY_3 = '3',
	KEY_4 = '4',
	KEY_5 = '5',
	KEY_6 = '6',
	KEY_7 = '7',
	KEY_8 = '8',
	KEY_9 = '9',
	KEY_A = 'a',
	KEY_B = 'b',
	KEY_C = 'c',
	KEY_D = 'd',
	KEY_E = 'e',
	KEY_F = 'f',
	KEY_G = 'g',
	KEY_H = 'h',
	KEY_I = 'i',
	KEY_J = 'j',
	KEY_K = 'k',
	KEY_L = 'l',
	KEY_M = 'm',
	KEY_N = 'n',
	KEY_O = 'o',
	KEY_P = 'p',
	KEY_Q = 'q',
	KEY_R = 'r',
	KEY_S = 's',
	KEY_T = 't',
	KEY_U = 'u',
	KEY_V = 'v',
	KEY_W = 'w',
	KEY_X = 'x',
	KEY_Y = 'y',
	KEY_Z = 'z',
	KEY_BACKSPACE = 127,
	KEY_UP = 256,
	KEY_DOWN,
	KEY_LEFT,
	KEY_RIGHT,
	KEY_INSERT,
	KEY_DELETE,
	KEY_HOME,
	KEY_END,
	KEY_PAGEUP,
	KEY_PAGEDOWN,
	KEY_F1,
	KEY_F2,
	KEY_F3,
	KEY_F4,
	KEY_F5,
	KEY_F6,
	KEY_F7,
	KEY_F8,
	KEY_F9,
	KEY_F10,
	KEY_F11,
	KEY_F12
} Key;

typedef struct
{
	struct termios saved;
	bool saved_ok;
	int saved_flags;
	bool flags_ok;
	uint64_t pressed[(KEY_MAX + 63) / 64];
	uint8_t buf[INPUT_BUF];
	int buf_len;
} Input;

/* Puts the terminal in raw mode: no canonical line buffering, no echo, and
   reads that return immediately when nothing is waiting. */
bool input_init(Input *input);
void input_shutdown(Input *input);

/* Drain stdin. true if any key was seen this poll. */
bool input_poll(Input *input);

/* true if that key was decoded this poll. */
bool input_is_key_pressed(const Input *input, Key key);

#endif /* STGE_CORE_INPUT_H */
