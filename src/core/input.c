#define _DEFAULT_SOURCE

#include <ctype.h>
#include <fcntl.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "core/input.h"

bool input_init(Input *input)
{
	struct termios raw;
	int flags;

	memset(input, 0, sizeof *input);

	if (tcgetattr(STDIN_FILENO, &input->saved) == 0)
	{
		raw = input->saved;
		raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
		raw.c_cc[VMIN] = 0;
		raw.c_cc[VTIME] = 0;

		if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0)
			input->saved_ok = true;
	}

	/* Also needed when stdin is a pipe rather than a terminal, where the
	   termios settings above do not apply and a read would block. */
	flags = fcntl(STDIN_FILENO, F_GETFL);
	if (flags != -1 && fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) == 0)
	{
		input->saved_flags = flags;
		input->flags_ok = true;
	}

	return true;
}

void input_shutdown(Input *input)
{
	if (input->saved_ok)
	{
		tcsetattr(STDIN_FILENO, TCSAFLUSH, &input->saved);
		input->saved_ok = false;
	}

	if (input->flags_ok)
	{
		fcntl(STDIN_FILENO, F_SETFL, input->saved_flags);
		input->flags_ok = false;
	}
}

static void pressed_set(Input *input, Key key)
{
	unsigned u = (unsigned)key;

	if (u == 0 || u >= KEY_MAX)
		return;

	input->pressed[u / 64] |= (uint64_t)1 << (u % 64);
}

static bool pressed_has(const Input *input, unsigned u)
{
	if (u >= KEY_MAX)
		return false;

	return (input->pressed[u / 64] >> (u % 64)) & 1u;
}

static Key map_byte(unsigned char ch)
{
	if (ch == '\r' || ch == '\n')
		return KEY_ENTER;
	if (ch == '\t')
		return KEY_TAB;
	if (ch == ' ')
		return KEY_SPACE;
	if (ch == 8 || ch == 127)
		return KEY_BACKSPACE;
	if (ch >= 'A' && ch <= 'Z')
		return (Key)(ch - 'A' + 'a');

	return (Key)ch;
}

static Key csi_tilde(int num)
{
	switch (num)
	{
	case 1:
		return KEY_HOME;
	case 2:
		return KEY_INSERT;
	case 3:
		return KEY_DELETE;
	case 4:
		return KEY_END;
	case 5:
		return KEY_PAGEUP;
	case 6:
		return KEY_PAGEDOWN;
	case 11:
		return KEY_F1;
	case 12:
		return KEY_F2;
	case 13:
		return KEY_F3;
	case 14:
		return KEY_F4;
	case 15:
		return KEY_F5;
	case 17:
		return KEY_F6;
	case 18:
		return KEY_F7;
	case 19:
		return KEY_F8;
	case 20:
		return KEY_F9;
	case 21:
		return KEY_F10;
	case 23:
		return KEY_F11;
	case 24:
		return KEY_F12;
	default:
		return KEY_NONE;
	}
}

static Key csi_final(char c)
{
	switch (c)
	{
	case 'A':
		return KEY_UP;
	case 'B':
		return KEY_DOWN;
	case 'C':
		return KEY_RIGHT;
	case 'D':
		return KEY_LEFT;
	case 'H':
		return KEY_HOME;
	case 'F':
		return KEY_END;
	default:
		return KEY_NONE;
	}
}

/* Returns bytes consumed, or 0 if the sequence is incomplete. *out is
   KEY_NONE when the bytes are a complete unknown sequence (drop). */
static int parse_key(const uint8_t *b, int n, Key *out)
{
	int i;
	int num;

	*out = KEY_NONE;
	if (n <= 0)
		return 0;

	if (b[0] != 0x1b)
	{
		*out = map_byte(b[0]);
		return 1;
	}

	if (n == 1)
		return 0;

	if (b[1] == '[')
	{
		num = 0;
		for (i = 2; i < n; i++)
		{
			char c = (char)b[i];

			if (c >= '0' && c <= '9')
			{
				num = num * 10 + (c - '0');
				continue;
			}

			if (c == ';')
			{
				num = 0;
				continue;
			}

			if (c == '~')
				*out = csi_tilde(num);
			else
				*out = csi_final(c);

			return i + 1;
		}

		return 0;
	}

	if (b[1] == 'O')
	{
		if (n < 3)
			return 0;

		switch ((char)b[2])
		{
		case 'P':
			*out = KEY_F1;
			break;
		case 'Q':
			*out = KEY_F2;
			break;
		case 'R':
			*out = KEY_F3;
			break;
		case 'S':
			*out = KEY_F4;
			break;
		default:
			*out = KEY_NONE;
			break;
		}

		return 3;
	}

	*out = KEY_ESCAPE;
	return 1;
}

static void decode_buf(Input *input, bool flush)
{
	int used = 0;

	while (used < input->buf_len)
	{
		Key key;
		int n = parse_key(input->buf + used, input->buf_len - used, &key);

		if (n == 0)
		{
			if (!flush)
				break;

			if (input->buf[used] == 0x1b)
				pressed_set(input, KEY_ESCAPE);
			used = input->buf_len;
			break;
		}

		if (key != KEY_NONE)
			pressed_set(input, key);
		used += n;
	}

	if (used > 0 && used < input->buf_len)
		memmove(input->buf, input->buf + used, (size_t)(input->buf_len - used));
	input->buf_len -= used;
	if (input->buf_len < 0)
		input->buf_len = 0;
}

static bool pressed_any(const Input *input)
{
	size_t i;

	for (i = 0; i < sizeof input->pressed / sizeof input->pressed[0]; i++)
	{
		if (input->pressed[i] != 0)
			return true;
	}

	return false;
}

bool input_poll(Input *input)
{
	uint8_t chunk[32];
	ssize_t n;

	memset(input->pressed, 0, sizeof input->pressed);

	for (;;)
	{
		n = read(STDIN_FILENO, chunk, sizeof chunk);
		if (n <= 0)
			break;

		if (input->buf_len + (int)n > INPUT_BUF)
			n = INPUT_BUF - input->buf_len;
		if (n <= 0)
		{
			decode_buf(input, true);
			if (input->buf_len >= INPUT_BUF)
				input->buf_len = 0;
			continue;
		}

		memcpy(input->buf + input->buf_len, chunk, (size_t)n);
		input->buf_len += (int)n;
		decode_buf(input, false);
	}

	decode_buf(input, true);
	return pressed_any(input);
}

bool input_is_key_pressed(const Input *input, Key key)
{
	unsigned u;

	if (input == NULL)
		return false;

	if (key == KEY_ENTER)
		return pressed_has(input, (unsigned)KEY_ENTER)
		    || pressed_has(input, (unsigned)'\r')
		    || pressed_has(input, (unsigned)'\n');

	u = (unsigned)key;
	if (u < 256 && isalpha((unsigned char)u))
		u = (unsigned)tolower((unsigned char)u);

	if (pressed_has(input, u))
		return true;

	if (u >= 'a' && u <= 'z' && pressed_has(input, u - 'a' + 'A'))
		return true;

	return false;
}
