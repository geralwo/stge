#define _DEFAULT_SOURCE

#include <stdlib.h>
#include <string.h>

#include "core/stge.h"
#include "core/obj.h"

static char *dup_str(const char *s)
{
	size_t n;
	char *out;

	if (s == NULL)
		s = "";

	n = strlen(s) + 1;
	out = stge_alloc(n);
	if (out == NULL)
		return NULL;

	memcpy(out, s, n);
	return out;
}

static uint32_t hash_str(const char *s)
{
	uint32_t h = 5381;
	unsigned char c;

	while ((c = (unsigned char)*s++) != '\0')
		h = h * 33u + c;

	return h;
}

static size_t elem_size(MemberKind kind)
{
	if (kind == MEMBER_STRING)
		return sizeof(char *);
	if (kind == MEMBER_FLOAT)
		return sizeof(float);
	if (kind == MEMBER_DOUBLE)
		return sizeof(double);
	if (kind == MEMBER_BYTE)
		return 1;
	if (kind == MEMBER_FUNCTION)
		return sizeof(FuncEntry);

	return sizeof(int32_t);
}

static bool kind_is_i32(MemberKind kind)
{
	return kind == MEMBER_INT || kind == MEMBER_OBJECT || kind == MEMBER_ARRAY;
}

static void member_clear(Member *m)
{
	memset(m, 0, sizeof *m);
}

void obj_init(Obj *obj, int id, const char *name)
{
	memset(obj, 0, sizeof *obj);
	obj->id = id;
	obj->type_id = -1;

	if (name != NULL)
	{
		size_t n = strlen(name);

		if (n >= STGE_NAME_MAX)
			n = STGE_NAME_MAX - 1;

		memcpy(obj->name, name, n);
		obj->name[n] = '\0';
	}
}

void obj_members_free(Obj *obj)
{
	uint32_t i;

	if (obj->members.slots == NULL)
		return;

	for (i = 0; i < obj->members.cap; i++)
	{
		Member *m = &obj->members.slots[i];

		if (m->kind == MEMBER_STRING && m->data != NULL && !m->host)
		{
			uint32_t j;
			char **arr = (char **)m->data;

			for (j = 0; j < m->len; j++)
			{
				free(arr[j]);
				arr[j] = NULL;
			}
		}

		if (m->growable && m->data != NULL)
		{
			dyn_free((Dyn *)m->data);
			free(m->data);
		}

		member_clear(m);
	}

	obj->members.slots = NULL;
	obj->members.cap = 0;
	obj->members.count = 0;
}

static Member *find_slot(const MemberTable *t, const char *key, bool for_insert)
{
	uint32_t mask;
	uint32_t i;
	Member *empty = NULL;

	if (t->slots == NULL || t->cap == 0)
		return NULL;

	mask = t->cap - 1;
	i = hash_str(key) & mask;

	for (;;)
	{
		Member *m = &t->slots[i];

		if (m->key == NULL)
			return for_insert ? (empty != NULL ? empty : m) : empty;

		if (strcmp(m->key, key) == 0)
			return m;

		i = (i + 1) & mask;
	}
}

static bool table_grow(MemberTable *t)
{
	Member *old;
	uint32_t old_cap;
	uint32_t i;
	uint32_t cap = t->cap == 0 ? 8 : t->cap * 2;

	old = t->slots;
	old_cap = t->cap;

	t->slots = stge_alloc(cap * sizeof *t->slots);
	if (t->slots == NULL)
	{
		t->slots = old;
		return false;
	}

	memset(t->slots, 0, cap * sizeof *t->slots);
	t->cap = cap;
	t->count = 0;

	for (i = 0; i < old_cap; i++)
	{
		if (old[i].key != NULL)
		{
			Member *dst = find_slot(t, old[i].key, true);

			*dst = old[i];
			t->count++;
		}
	}

	return true;
}

const Member *obj_member_get(const Obj *obj, const char *key)
{
	Member *m;

	if (obj == NULL || key == NULL)
		return NULL;

	m = find_slot(&obj->members, key, false);
	return m;
}

bool obj_member_new(Obj *obj, const char *key, MemberKind kind, uint32_t len)
{
	Member *m;
	void *data;
	char *owned = NULL;

	if (obj == NULL || key == NULL)
		return false;

	if (obj->members.cap == 0 || (obj->members.count + 1) * 4 >= obj->members.cap * 3)
	{
		if (!table_grow(&obj->members))
			return false;
	}

	m = find_slot(&obj->members, key, true);
	if (m == NULL)
		return false;

	if (len == 0)
		data = NULL;
	else
	{
		data = stge_alloc(len * elem_size(kind));
		if (data == NULL)
			return false;
		memset(data, 0, len * elem_size(kind));
	}

	if (m->key == NULL)
	{
		owned = dup_str(key);
		if (owned == NULL)
			return false;

		m->key = owned;
		obj->members.count++;
	}

	m->kind = kind;
	m->len = len;
	m->growable = false;
	m->host = false;
	m->data = data;
	return true;
}

bool obj_member_bind(Obj *obj, const char *key, MemberKind kind, void *data, uint32_t len)
{
	Member *m;
	char *owned = NULL;

	if (obj == NULL || key == NULL || data == NULL || len == 0)
		return false;

	if (obj->members.cap == 0 || (obj->members.count + 1) * 4 >= obj->members.cap * 3)
	{
		if (!table_grow(&obj->members))
			return false;
	}

	m = find_slot(&obj->members, key, true);
	if (m == NULL)
		return false;

	if (m->key == NULL)
	{
		owned = dup_str(key);
		if (owned == NULL)
			return false;

		m->key = owned;
		obj->members.count++;
	}
	else if (m->growable && m->data != NULL)
	{
		dyn_free((Dyn *)m->data);
		free(m->data);
	}

	m->kind = kind;
	m->len = len;
	m->growable = false;
	m->host = true;
	m->data = data;
	return true;
}

bool obj_member_new_dyn(Obj *obj, const char *key, MemberKind kind)
{
	Member *m;
	Dyn *d;
	char *owned = NULL;

	if (obj == NULL || key == NULL)
		return false;

	if (obj->members.cap == 0 || (obj->members.count + 1) * 4 >= obj->members.cap * 3)
	{
		if (!table_grow(&obj->members))
			return false;
	}

	m = find_slot(&obj->members, key, true);
	if (m == NULL)
		return false;

	d = malloc(sizeof *d);
	if (d == NULL)
		return false;

	dyn_init(d, (uint32_t)elem_size(kind));

	if (m->key == NULL)
	{
		owned = dup_str(key);
		if (owned == NULL)
		{
			dyn_free(d);
			free(d);
			return false;
		}

		m->key = owned;
		obj->members.count++;
	}
	else if (m->growable && m->data != NULL)
	{
		dyn_free((Dyn *)m->data);
		free(m->data);
	}

	m->kind = kind;
	m->len = 0;
	m->growable = true;
	m->host = false;
	m->data = d;
	return true;
}

Dyn *obj_member_dyn(Obj *obj, const char *key)
{
	Member *m = obj == NULL ? NULL : find_slot(&obj->members, key, false);

	if (m == NULL || !m->growable)
		return NULL;

	return (Dyn *)m->data;
}

uint32_t obj_mem_len(const Obj *obj, const char *key)
{
	const Member *m = obj_member_get(obj, key);

	if (m == NULL)
		return 0;

	if (m->growable && m->data != NULL)
		return dyn_len((const Dyn *)m->data);

	return m->len;
}

bool obj_dyn_add_i(Obj *obj, const char *key, int64_t value)
{
	Dyn *d = obj_member_dyn(obj, key);
	const Member *m = obj_member_get(obj, key);
	int32_t i32;
	uint8_t b;

	if (d == NULL || m == NULL)
		return false;

	if (m->kind == MEMBER_BYTE)
	{
		b = (uint8_t)value;
		return dyn_add(d, &b);
	}

	if (m->kind == MEMBER_FLOAT)
	{
		float f = (float)value;

		return dyn_add(d, &f);
	}

	i32 = (int32_t)value;
	return dyn_add(d, &i32);
}

int32_t obj_mem_get_i(const Obj *obj, const char *key, uint32_t i)
{
	const Member *m = obj_member_get(obj, key);

	if (m == NULL && key != NULL && strcmp(key, "pos") == 0)
		return obj_mem_get_i(obj, i == 0 ? "x" : "y", 0);

	if (m == NULL || m->data == NULL)
		return 0;

	if (m->growable)
	{
		const void *slot = dyn_at((Dyn *)m->data, i);

		if (slot == NULL)
			return 0;
		if (m->kind == MEMBER_BYTE)
			return *(const uint8_t *)slot;
		if (m->kind == MEMBER_UINT)
			return (int32_t)*(const uint32_t *)slot;
		return *(const int32_t *)slot;
	}

	if (i >= m->len)
		return 0;

	if (m->kind == MEMBER_BYTE)
		return ((const uint8_t *)m->data)[i];

	if (m->kind == MEMBER_UINT)
		return (int32_t)((const uint32_t *)m->data)[i];

	if (kind_is_i32(m->kind))
		return ((const int32_t *)m->data)[i];

	return 0;
}

uint32_t obj_mem_get_u(const Obj *obj, const char *key, uint32_t i)
{
	return (uint32_t)obj_mem_get_i(obj, key, i);
}

void obj_mem_set_i(Obj *obj, const char *key, uint32_t i, int32_t value)
{
	Member *m;

	if (obj == NULL)
		return;

	m = find_slot(&obj->members, key, false);
	if (m == NULL || m->data == NULL)
		return;

	if (m->growable)
	{
		void *slot = dyn_at((Dyn *)m->data, i);

		if (slot == NULL)
			return;
		if (m->kind == MEMBER_BYTE)
			*(uint8_t *)slot = (uint8_t)value;
		else if (m->kind == MEMBER_UINT)
			*(uint32_t *)slot = (uint32_t)value;
		else
			*(int32_t *)slot = value;
		return;
	}

	if (i >= m->len)
		return;

	if (m->kind == MEMBER_BYTE)
		((uint8_t *)m->data)[i] = (uint8_t)value;
	else if (m->kind == MEMBER_UINT)
		((uint32_t *)m->data)[i] = (uint32_t)value;
	else if (kind_is_i32(m->kind))
		((int32_t *)m->data)[i] = value;
}

void obj_mem_set_i_auto(Obj *obj, const char *key, uint32_t i, int32_t value)
{
	const Member *m = obj_member_get(obj, key);

	if (m == NULL)
	{
		if (i != 0)
			return;

		if (!obj_member_new(obj, key, MEMBER_INT, 1))
			return;
	}

	obj_mem_set_i(obj, key, i, value);
}

float obj_mem_get_f(const Obj *obj, const char *key, uint32_t i)
{
	const Member *m = obj_member_get(obj, key);

	if (m == NULL || i >= m->len || m->data == NULL || m->kind != MEMBER_FLOAT)
		return 0.0f;

	return ((const float *)m->data)[i];
}

void obj_mem_set_f(Obj *obj, const char *key, uint32_t i, float value)
{
	Member *m;

	if (obj == NULL)
		return;

	m = find_slot(&obj->members, key, false);
	if (m == NULL || i >= m->len || m->data == NULL || m->kind != MEMBER_FLOAT)
		return;

	((float *)m->data)[i] = value;
}

double obj_mem_get_d(const Obj *obj, const char *key, uint32_t i)
{
	const Member *m = obj_member_get(obj, key);

	if (m == NULL || i >= m->len || m->data == NULL || m->kind != MEMBER_DOUBLE)
		return 0.0;

	return ((const double *)m->data)[i];
}

void obj_mem_set_d(Obj *obj, const char *key, uint32_t i, double value)
{
	Member *m;

	if (obj == NULL)
		return;

	m = find_slot(&obj->members, key, false);
	if (m == NULL || i >= m->len || m->data == NULL || m->kind != MEMBER_DOUBLE)
		return;

	((double *)m->data)[i] = value;
}

const char *obj_mem_str_i(const Obj *obj, const char *key, uint32_t i)
{
	const Member *m = obj_member_get(obj, key);
	char **arr;

	if (m == NULL || m->kind != MEMBER_STRING || m->data == NULL || i >= m->len)
		return NULL;

	arr = (char **)m->data;
	return arr[i] != NULL ? arr[i] : "";
}

const char *obj_mem_str(const Obj *obj, const char *key)
{
	return obj_mem_str_i(obj, key, 0);
}

bool obj_mem_set_str_i(Obj *obj, const char *key, uint32_t i, const char *s)
{
	char *copy;
	char **arr;
	const Member *m;

	if (s == NULL)
		s = "";

	m = obj_member_get(obj, key);
	if (m == NULL || m->kind != MEMBER_STRING)
	{
		if (i != 0)
			return false;
		if (!obj_member_new(obj, key, MEMBER_STRING, 1))
			return false;
	}

	m = obj_member_get(obj, key);
	if (m == NULL || m->data == NULL || i >= m->len)
		return false;

	copy = strdup(s);
	if (copy == NULL)
		return false;

	arr = (char **)m->data;
	free(arr[i]);
	arr[i] = copy;
	return true;
}

bool obj_mem_set_str(Obj *obj, const char *key, const char *s)
{
	return obj_mem_set_str_i(obj, key, 0, s);
}

bool obj_visible(const Obj *obj)
{
	const Member *m = obj_member_get(obj, "visible");

	if (m == NULL)
		return true;

	return obj_mem_get_i(obj, "visible", 0) != 0;
}

const FuncEntry *obj_fn_get(const Obj *obj, const char *name, int argc)
{
	const Member *m = obj_member_get(obj, name);
	const FuncEntry *ents;
	uint32_t i;

	if (m == NULL || m->kind != MEMBER_FUNCTION || m->data == NULL)
		return NULL;

	ents = (const FuncEntry *)m->data;
	for (i = 0; i < m->len; i++)
	{
		if (ents[i].argc == argc)
			return &ents[i];
	}

	return NULL;
}

bool obj_fn_set(Obj *obj, const char *name, int pc, int argc, bool has_arg,
		bool is_extern, int native_id)
{
	Member *m;
	FuncEntry *ents;
	FuncEntry *grown;
	FuncEntry one;
	uint32_t i;
	uint32_t n;

	if (obj == NULL || name == NULL)
		return false;

	one.pc = pc;
	one.argc = argc;
	one.has_arg = has_arg;
	one.is_extern = is_extern;
	one.native_id = native_id;

	m = find_slot(&obj->members, name, false);
	if (m == NULL || m->kind != MEMBER_FUNCTION)
	{
		if (!obj_member_new(obj, name, MEMBER_FUNCTION, 1))
			return false;
		m = find_slot(&obj->members, name, false);
		if (m == NULL || m->data == NULL)
			return false;
		((FuncEntry *)m->data)[0] = one;
		return true;
	}

	ents = (FuncEntry *)m->data;
	n = m->len;
	for (i = 0; i < n; i++)
	{
		if (ents[i].argc == argc)
		{
			ents[i] = one;
			return true;
		}
	}

	grown = stge_alloc((n + 1) * sizeof *grown);
	if (grown == NULL)
		return false;
	if (ents != NULL && n > 0)
		memcpy(grown, ents, n * sizeof *grown);
	grown[n] = one;
	m->data = grown;
	m->len = n + 1;
	return true;
}

int32_t obj_obj_get(const Obj *obj, const char *key)
{
	const Member *m = obj_member_get(obj, key);

	if (m == NULL || m->kind != MEMBER_OBJECT)
		return -1;

	return obj_mem_get_i(obj, key, 0);
}

bool obj_obj_set(Obj *obj, const char *key, int32_t id)
{
	const Member *m = obj_member_get(obj, key);

	if (m == NULL || m->kind != MEMBER_OBJECT)
	{
		if (!obj_member_new(obj, key, MEMBER_OBJECT, 1))
			return false;
	}

	obj_mem_set_i(obj, key, 0, id);
	return true;
}
