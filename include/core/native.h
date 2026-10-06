#ifndef STGE_CORE_NATIVE_H
#define STGE_CORE_NATIVE_H

#include <stdbool.h>
#include <stdint.h>

#define STGE_MAX_NATIVES 64
#define STGE_MAX_HOST_TYPES 16

typedef int32_t (*StgeNativeFn)(int32_t *args, int argc);

bool stge_native_add(const char *name, StgeNativeFn fn);
int stge_native_find(const char *name);
int32_t stge_call_native(int native_id, int32_t *args, int argc);
void stge_native_clear(void);

bool stge_host_type_add(const char *name);
int stge_host_type_find(const char *name);

void stge_std_register(void);
void stge_host_types_register(void);

#endif /* STGE_CORE_NATIVE_H */
