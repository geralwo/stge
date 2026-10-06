#include <string.h>

#include "core/native.h"

typedef struct
{
	const char *name;
	StgeNativeFn fn;
} NativeEntry;

static NativeEntry natives[STGE_MAX_NATIVES];
static int native_count;
static const char *host_types[STGE_MAX_HOST_TYPES];
static int host_type_count;

void stge_native_clear(void)
{
	native_count = 0;
	host_type_count = 0;
	memset(natives, 0, sizeof natives);
	memset(host_types, 0, sizeof host_types);
}

bool stge_native_add(const char *name, StgeNativeFn fn)
{
	if (name == NULL || name[0] == '\0' || fn == NULL)
		return false;

	if (stge_native_find(name) >= 0)
		return false;

	if (native_count >= STGE_MAX_NATIVES)
		return false;

	natives[native_count].name = name;
	natives[native_count].fn = fn;
	native_count++;
	return true;
}

int stge_native_find(const char *name)
{
	int i;

	if (name == NULL)
		return -1;

	for (i = 0; i < native_count; i++)
	{
		if (strcmp(natives[i].name, name) == 0)
			return i;
	}

	return -1;
}

int32_t stge_call_native(int native_id, int32_t *args, int argc)
{
	if (native_id < 0 || native_id >= native_count || natives[native_id].fn == NULL)
		return 0;

	return natives[native_id].fn(args, argc);
}

bool stge_host_type_add(const char *name)
{
	if (name == NULL || name[0] == '\0')
		return false;

	if (stge_host_type_find(name) >= 0)
		return false;

	if (host_type_count >= STGE_MAX_HOST_TYPES)
		return false;

	host_types[host_type_count++] = name;
	return true;
}

int stge_host_type_find(const char *name)
{
	int i;

	if (name == NULL)
		return -1;

	for (i = 0; i < host_type_count; i++)
	{
		if (strcmp(host_types[i], name) == 0)
			return i;
	}

	return -1;
}

void stge_host_types_register(void)
{
	stge_host_type_add("stge_string");
	stge_host_type_add("stge_cstr");
	stge_host_type_add("stge_input");
	stge_host_type_add("Pixel");
	stge_host_type_add("Screen");
}
