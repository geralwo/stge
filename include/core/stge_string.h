#ifndef STGE_CORE_STGE_STRING_H
#define STGE_CORE_STGE_STRING_H

#include <stdbool.h>
#include <stdint.h>

/* Host type for `extern stge_string`. Not a script global. */
typedef struct
{
	uint32_t length;
	uint8_t *buffer;
} StgeString;

StgeString stge_string_new();
StgeString stge_string_new_from_cstr(const char *cstr);
StgeString stge_string_new_from_int(int32_t value);
StgeString stge_string_new_from_uint(uint32_t value);
StgeString stge_string_new_from_int64(int64_t value);
StgeString stge_string_new_from_uint64(uint64_t value);
StgeString stge_string_new_from_float(float value);
StgeString stge_string_new_from_double(double value);
StgeString stge_string_new_from_bool(bool value);
StgeString stge_string_new_from_char(char value);
StgeString stge_string_copy(const StgeString *str);
void stge_string_append(StgeString *str, const char *cstr);
void stge_string_append_char(StgeString *str, char c);

#endif /* STGE_CORE_STGE_STRING_H */
