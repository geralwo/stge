#include <stdlib.h>
#include <string.h>

#include "core/native.h"

typedef struct
{
	const char *name;
	StgeNativeFn fn;
} NativeEntry;

static NativeEntry *natives;
static int native_count;
static int natives_cap;
static const char **host_types;
static int host_type_count;
static int host_types_cap;

void stge_native_clear(void)
{
	free(natives);
	natives = NULL;
	native_count = 0;
	natives_cap = 0;
	free(host_types);
	host_types = NULL;
	host_type_count = 0;
	host_types_cap = 0;
}

static bool ensure_natives_cap(void)
{
	NativeEntry *grown;
	int ncap;

	if (native_count < natives_cap)
		return true;

	ncap = natives_cap > 0 ? natives_cap * 2 : 16;
	grown = realloc(natives, (size_t)ncap * sizeof *grown);
	if (grown == NULL)
		return false;
	memset(grown + natives_cap, 0, (size_t)(ncap - natives_cap) * sizeof *grown);
	natives = grown;
	natives_cap = ncap;
	return true;
}

static bool ensure_host_types_cap(void)
{
	const char **grown;
	int ncap;

	if (host_type_count < host_types_cap)
		return true;

	ncap = host_types_cap > 0 ? host_types_cap * 2 : 8;
	grown = realloc(host_types, (size_t)ncap * sizeof *grown);
	if (grown == NULL)
		return false;
	memset(grown + host_types_cap, 0,
	       (size_t)(ncap - host_types_cap) * sizeof *grown);
	host_types = grown;
	host_types_cap = ncap;
	return true;
}

bool stge_native_add(const char *name, StgeNativeFn fn)
{
	if (name == NULL || name[0] == '\0' || fn == NULL)
		return false;

	if (stge_native_find(name) >= 0)
		return false;

	if (!ensure_natives_cap())
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

int64_t stge_call_native(int native_id, int64_t *args, int argc)
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

	if (!ensure_host_types_cap())
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
