#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/stge_string.h"

static StgeString stge_string_from_bytes(const char *text, size_t n)
{
	StgeString out;

	out.length = 0;
	out.buffer = NULL;
	if (n == 0)
		return out;

	out.buffer = malloc(n);
	if (out.buffer == NULL)
		return out;

	memcpy(out.buffer, text, n);
	out.length = (uint32_t)n;
	return out;
}

StgeString stge_string_new(void)
{
	StgeString out;

	out.length = 0;
	out.buffer = NULL;
	return out;
}

StgeString stge_string_new_from_cstr(const char *cstr)
{
	if (cstr == NULL)
		return stge_string_new();

	return stge_string_from_bytes(cstr, strlen(cstr));
}

StgeString stge_string_new_from_int(int32_t value)
{
	char buf[16];

	snprintf(buf, sizeof buf, "%d", (int)value);
	return stge_string_new_from_cstr(buf);
}

StgeString stge_string_new_from_uint(uint32_t value)
{
	char buf[16];

	snprintf(buf, sizeof buf, "%u", (unsigned)value);
	return stge_string_new_from_cstr(buf);
}

StgeString stge_string_new_from_float(float value)
{
	char buf[32];

	snprintf(buf, sizeof buf, "%g", (double)value);
	return stge_string_new_from_cstr(buf);
}

StgeString stge_string_new_from_double(double value)
{
	char buf[32];

	snprintf(buf, sizeof buf, "%g", value);
	return stge_string_new_from_cstr(buf);
}

StgeString stge_string_new_from_bool(bool value)
{
	return stge_string_new_from_cstr(value ? "true" : "false");
}

StgeString stge_string_new_from_char(char value)
{
	char buf[1];

	buf[0] = value;
	return stge_string_from_bytes(buf, 1);
}

StgeString stge_string_copy(const StgeString *str)
{
	if (str == NULL || str->buffer == NULL || str->length == 0)
		return stge_string_new();

	return stge_string_from_bytes((const char *)str->buffer, str->length);
}

void stge_string_append(StgeString *str, const char *cstr)
{
	size_t add;
	uint8_t *next;

	if (str == NULL || cstr == NULL || cstr[0] == '\0')
		return;

	add = strlen(cstr);
	next = realloc(str->buffer, (size_t)str->length + add);
	if (next == NULL)
		return;

	memcpy(next + str->length, cstr, add);
	str->buffer = next;
	str->length += (uint32_t)add;
}

void stge_string_append_char(StgeString *str, char c)
{
	char buf[2];

	buf[0] = c;
	buf[1] = '\0';
	stge_string_append(str, buf);
}
