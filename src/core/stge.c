#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "core/native.h"
#include "core/stge.h"
#include "core/vm.h"
#include "parser/parser.h"

Stge stge;

VmProg *stge_prog(void)
{
	return &stge.prog;
}

static VmProg *sc(void)
{
	return &stge.prog;
}

Obj *stge_obj(int id)
{
	VmProg *p = sc();

	if (p == NULL)
		return NULL;

	if (stge_is_type_ref(id))
	{
		int t = stge_type_index(id);

		if (t < 0 || t >= p->type_count || p->types == NULL)
			return NULL;
		return p->types[t];
	}

	if (id < 0 || id >= p->object_count || p->objs == NULL)
		return NULL;
	return p->objs[id];
}

Obj *stge_type(int type_index)
{
	VmProg *p = sc();

	if (p == NULL || type_index < 0 || type_index >= p->type_count || p->types == NULL)
		return NULL;
	return p->types[type_index];
}

static void spatial_remove(int id);

void *stge_alloc(size_t n)
{
	VmProg *s = sc();

	if (s == NULL)
		return NULL;

	return arena_alloc(&s->arena, n);
}

struct ArenaBlock
{
	ArenaBlock *next;
	size_t used;
	uint8_t data[STGE_ARENA_BLOCK];
};

void *arena_alloc(Arena *a, size_t n)
{
	n = (n + 7u) & ~(size_t)7u;
	if (n == 0)
		n = 8;

	if (a == NULL || n > STGE_ARENA_BLOCK)
		return NULL;

	if (a->curr == NULL || a->curr->used + n > STGE_ARENA_BLOCK)
	{
		ArenaBlock *b = malloc(sizeof *b);

		if (b == NULL)
			return NULL;

		b->next = NULL;
		b->used = 0;
		if (a->curr != NULL)
			a->curr->next = b;
		else
			a->head = b;
		a->curr = b;
	}

	{
		void *p = a->curr->data + a->curr->used;

		a->curr->used += n;
		return p;
	}
}

void arena_reset(Arena *a)
{
	ArenaBlock *b;

	if (a == NULL)
		return;

	b = a->head;
	while (b != NULL)
	{
		ArenaBlock *next = b->next;

		free(b);
		b = next;
	}

	a->head = NULL;
	a->curr = NULL;
}

bool vm_prog_init(VmProg *p)
{
	if (p == NULL)
		return false;

	memset(p, 0, sizeof *p);
	p->code = malloc(STGE_CODE_START);
	if (p->code == NULL)
		return false;

	p->code_cap = STGE_CODE_START;
	p->init_pc = -1;
	p->objs_cap = STGE_OBJS_INIT;
	p->objs = calloc((size_t)p->objs_cap, sizeof *p->objs);
	if (p->objs == NULL)
	{
		free(p->code);
		p->code = NULL;
		return false;
	}
	p->types_cap = STGE_OBJS_INIT;
	p->types = calloc((size_t)p->types_cap, sizeof *p->types);
	if (p->types == NULL)
	{
		free(p->objs);
		p->objs = NULL;
		free(p->code);
		p->code = NULL;
		return false;
	}
	{
		int i;

		for (i = 0; i < STGE_MAX_STRS; i++)
			p->wrap_id[i] = -1;
	}
	return true;
}

void vm_prog_reset(VmProg *p)
{
	int i;

	if (p == NULL)
		return;

	for (i = 0; i < p->object_count; i++)
	{
		if (p->objs[i] != NULL)
			obj_members_free(p->objs[i]);
		p->objs[i] = NULL;
	}
	for (i = 0; i < p->type_count; i++)
	{
		if (p->types[i] != NULL)
			obj_members_free(p->types[i]);
		p->types[i] = NULL;
	}

	arena_reset(&p->arena);
	p->object_count = 0;
	p->type_count = 0;
	{
		int i;

		for (i = 0; i < p->str_count; i++)
		{
			free(p->strs[i]);
			p->strs[i] = NULL;
		}
		p->str_count = 0;
		for (i = 0; i < STGE_MAX_STRS; i++)
			p->wrap_id[i] = -1;
	}
	p->code_len = 0;
	p->init_pc = -1;
	vm_script_locals_reset();
}

void vm_prog_shutdown(VmProg *p)
{
	if (p == NULL)
		return;

	vm_prog_reset(p);
	free(p->objs);
	p->objs = NULL;
	p->objs_cap = 0;
	free(p->types);
	p->types = NULL;
	p->types_cap = 0;
	free(p->code);
	p->code = NULL;
	p->code_cap = 0;
}

static const struct
{
	const char *name;
	Color color;
} named_colors[] = {
	{"default", COLOR_DEFAULT},
	{"black", COLOR_BLACK},
	{"red", COLOR_RED},
	{"green", COLOR_GREEN},
	{"yellow", COLOR_YELLOW},
	{"blue", COLOR_BLUE},
	{"magenta", COLOR_MAGENTA},
	{"cyan", COLOR_CYAN},
	{"white", COLOR_WHITE},
	{"lightblack", COLOR_LIGHTBLACK},
	{"lightred", COLOR_LIGHTRED},
	{"lightgreen", COLOR_LIGHTGREEN},
	{"lightyellow", COLOR_LIGHTYELLOW},
	{"lightblue", COLOR_LIGHTBLUE},
	{"lightmagenta", COLOR_LIGHTMAGENTA},
	{"lightcyan", COLOR_LIGHTCYAN},
	{"lightwhite", COLOR_LIGHTWHITE},
};

bool color_parse(const char *s, Color *out)
{
	size_t i;

	if (s == NULL || out == NULL)
		return false;

	for (i = 0; i < sizeof named_colors / sizeof named_colors[0]; i++)
	{
		const char *a = s;
		const char *b = named_colors[i].name;

		while (*a != '\0' && *b != '\0')
		{
			char ca = *a;
			char cb = *b;

			if (ca >= 'A' && ca <= 'Z')
				ca = (char)(ca - 'A' + 'a');
			if (cb >= 'A' && cb <= 'Z')
				cb = (char)(cb - 'A' + 'a');
			if (ca != cb)
				break;
			a++;
			b++;
		}

		if (*a == '\0' && *b == '\0')
		{
			*out = named_colors[i].color;
			return true;
		}
	}

	return false;
}

Color color_named(const char *s)
{
	Color col = COLOR_DEFAULT;

	(void)color_parse(s, &col);
	return col;
}

int color_fg_code(Color color)
{
	int n = (int)color;

	if (color == COLOR_DEFAULT)
		return 39;

	if (n >= 8)
		return 90 + (n - 8);

	return 30 + n;
}

int color_bg_code(Color color)
{
	int n = (int)color;

	if (color == COLOR_DEFAULT)
		return 49;

	if (n >= 8)
		return 100 + (n - 8);

	return 40 + n;
}

static bool valid_id(int id)
{
	VmProg *p = sc();

	if (p == NULL)
		return false;

	if (stge_is_type_ref(id))
	{
		int t = stge_type_index(id);

		return t >= 0 && t < p->type_count && p->types != NULL && p->types[t] != NULL;
	}

	return id >= 0 && id < p->object_count && p->objs != NULL && p->objs[id] != NULL;
}

static bool valid_heap_id(int id)
{
	VmProg *p = sc();

	return p != NULL && id >= 0 && id < p->object_count && p->objs != NULL && p->objs[id] != NULL;
}

static const char *str_bytes_key(int id, const char *name, bool create)
{
	(void)id;
	(void)create;
	return name;
}

static bool is_pos_name(const char *name)
{
	return strcmp(name, "x") == 0 || strcmp(name, "y") == 0 || strcmp(name, "pos") == 0 || strcmp(name, "position") == 0;
}

static int link_of(int id, const char *name)
{
	if (!valid_id(id) || obj_member_get(stge_obj(id), name) == NULL)
		return -1;

	return (int)obj_mem_get_i(stge_obj(id), name, 0);
}

static void set_link(int id, const char *name, int value)
{
	if (valid_id(id))
		obj_mem_set_i_auto(stge_obj(id), name, 0, value);
}

int stge_parent(int id)
{
	return link_of(id, "parent");
}

static bool would_cycle(int parent, int child)
{
	int p = parent;
	int n = 0;

	while (p >= 0 && n++ < 1000000)
	{
		if (p == child)
			return true;

		p = link_of(p, "parent");
	}

	return false;
}

static bool ancestor_hidden(int id)
{
	int n = 0;

	while (valid_id(id) && n++ < 1000000)
	{
		if (!obj_visible(stge_obj(id)))
			return true;

		id = link_of(id, "parent");
	}

	return false;
}

static int tile_coord(int32_t v)
{
	if (v >= 0)
		return (int)(v / STGE_SPATIAL_TILE);

	return (int)((v - (STGE_SPATIAL_TILE - 1)) / STGE_SPATIAL_TILE);
}

static uint32_t tile_hash(int tx, int ty)
{
	uint32_t h = (uint32_t)tx * 73856093u ^ (uint32_t)ty * 19349663u;

	return h % STGE_SPATIAL_BUCKETS;
}

static bool spatial_ensure(int id)
{
	int need;

	if (id < 0)
		return false;
	need = id + 1;
	if (need <= stge.spatial_cap)
		return true;

	{
		int ncap = stge.spatial_cap > 0 ? stge.spatial_cap : STGE_OBJS_INIT;
		SpatialNode *nodes;
		bool *live;
		int i;

		while (ncap < need)
			ncap *= 2;
		nodes = realloc(stge.spatial_nodes, (size_t)ncap * sizeof *nodes);
		live = realloc(stge.spatial_live, (size_t)ncap * sizeof *live);
		if (nodes == NULL || live == NULL)
		{
			free(nodes);
			free(live);
			return false;
		}
		for (i = stge.spatial_cap; i < ncap; i++)
			live[i] = false;
		stge.spatial_nodes = nodes;
		stge.spatial_live = live;
		stge.spatial_cap = ncap;
	}
	return true;
}

static void spatial_init(void)
{
	int i;

	for (i = 0; i < STGE_SPATIAL_BUCKETS; i++)
		stge.spatial_heads[i] = -1;

	if (stge.spatial_live != NULL)
		memset(stge.spatial_live, 0, (size_t)stge.spatial_cap * sizeof *stge.spatial_live);
}

static void spatial_remove(int id)
{
	int b;
	int *walk;

	if (id < 0 || id >= stge.spatial_cap || !stge.spatial_live[id])
		return;

	b = (int)tile_hash(tile_coord(stge.spatial_nodes[id].wx), tile_coord(stge.spatial_nodes[id].wy));
	walk = &stge.spatial_heads[b];

	while (*walk >= 0)
	{
		if (*walk == id)
		{
			*walk = stge.spatial_nodes[id].next;
			stge.spatial_live[id] = false;
			return;
		}

		walk = &stge.spatial_nodes[*walk].next;
	}

	stge.spatial_live[id] = false;
}

/* Renderable objs stay off-stage until add_child. Container roots (no display) stay live. */
static bool orphan_instance(int id)
{
	if (!valid_heap_id(id) || link_of(id, "parent") >= 0)
		return false;

	return obj_mem_str(stge_obj(id), "display") != NULL;
}

static void spatial_put(int id)
{
	int b;
	int32_t x;
	int32_t y;

	if (!valid_heap_id(id) || !spatial_ensure(id))
		return;

	if (ancestor_hidden(id) || orphan_instance(id))
	{
		spatial_remove(id);
		return;
	}

	x = (int32_t)stge_world_x(id);
	y = (int32_t)stge_world_y(id);

	spatial_remove(id);

	stge.spatial_nodes[id].id = id;
	stge.spatial_nodes[id].wx = x;
	stge.spatial_nodes[id].wy = y;
	b = (int)tile_hash(tile_coord(x), tile_coord(y));
	stge.spatial_nodes[id].next = stge.spatial_heads[b];
	stge.spatial_heads[b] = id;
	stge.spatial_live[id] = true;
}

static void spatial_put_tree(int id)
{
	int c;

	if (!valid_id(id))
		return;

	spatial_put(id);
	for (c = link_of(id, "child"); c >= 0; c = link_of(c, "sibling"))
		spatial_put_tree(c);
}

static void spatial_refresh(void)
{
	int i;

	if (sc() == NULL)
		return;

	for (i = 0; i < sc()->object_count; i++)
	{
		if (sc()->objs[i] != NULL)
			spatial_put(i);
	}
}

static void detach_child(int child)
{
	int parent;
	int first;
	int sib;
	int walk;

	parent = link_of(child, "parent");
	sib = link_of(child, "sibling");

	if (parent >= 0)
	{
		first = link_of(parent, "child");
		if (first == child)
		{
			set_link(parent, "child", sib);
		}
		else
		{
			walk = first;
			while (walk >= 0)
			{
				int next = link_of(walk, "sibling");

				if (next == child)
				{
					set_link(walk, "sibling", sib);
					break;
				}

				walk = next;
			}
		}
	}

	set_link(child, "parent", -1);
	set_link(child, "sibling", -1);
}

bool stge_init(void)
{
	memset(&stge, 0, sizeof stge);

	/* Screen and input stay off until stge_run so a parse error never
	   clears the terminal or enters raw mode. */
	if (!vm_prog_init(&stge.prog))
		return false;

	spatial_init();
	stge.tickrate = STGE_TICKRATE;
	stge.print_out = strdup("stge.log");
	if (stge.print_out == NULL)
	{
		vm_prog_shutdown(&stge.prog);
		return false;
	}
	stge.print_file = NULL;
	stge_native_clear();
	stge_std_register();
	stge_host_types_register();
	return true;
}

void stge_shutdown(void)
{
	vm_prog_shutdown(&stge.prog);
	free(stge.spatial_nodes);
	free(stge.spatial_live);
	stge.spatial_nodes = NULL;
	stge.spatial_live = NULL;
	stge.spatial_cap = 0;
	if (stge.print_file != NULL)
	{
		fclose(stge.print_file);
		stge.print_file = NULL;
	}
	free(stge.print_out);
	stge.print_out = NULL;
	input_shutdown(&stge.input);
	screen_shutdown(&stge.screen);
	stge_native_clear();
}

Vec2i stge_size(void)
{
	return screen_size(&stge.screen);
}

int stge_width(void)
{
	return stge_size().x;
}

int stge_height(void)
{
	return stge_size().y;
}

int32_t stge_cam_get(uint32_t i)
{
	if (i > 1)
		return 0;

	return stge.cam_pos[i];
}

void stge_cam_set(uint32_t i, int32_t value)
{
	if (i <= 1)
		stge.cam_pos[i] = value;
}

void stge_cam_center_on(int32_t x, int32_t y)
{
	stge.cam_pos[0] = x - stge_width() / 2;
	stge.cam_pos[1] = y - stge_height() / 2;
}

int32_t stge_tickrate(void)
{
	return stge.tickrate;
}

void stge_set_tickrate(int32_t rate)
{
	if (rate < 1)
		rate = 1;
	if (rate > 1000)
		rate = 1000;

	stge.tickrate = rate;
}

int32_t stge_rand(int32_t n)
{
	if (stge.rng == 0)
	{
		stge.rng = (uint32_t)time(NULL);
		if (stge.rng == 0)
			stge.rng = 1;
	}

	stge.rng = stge.rng * 1664525u + 1013904223u;
	if (n <= 0)
		return 0;

	return (int32_t)((stge.rng >> 16) % (uint32_t)n);
}

const char *stge_print_out(void)
{
	return stge.print_out != NULL ? stge.print_out : "";
}

void stge_set_print_out(const char *path)
{
	char *copy;

	if (path == NULL || path[0] == '\0')
		return;

	if (stge.print_out != NULL && strcmp(stge.print_out, path) == 0)
		return;

	copy = strdup(path);
	if (copy == NULL)
		return;

	if (stge.print_file != NULL)
	{
		fclose(stge.print_file);
		stge.print_file = NULL;
	}

	free(stge.print_out);
	stge.print_out = copy;
}

void stge_print(uint8_t kind, int64_t value)
{
	if (stge.print_file == NULL)
	{
		stge.print_file = fopen(stge.print_out, "w");
		if (stge.print_file == NULL)
			return;
	}

	if (kind == STGE_PRINT_STR)
	{
		const char *s = stge_str((int)value);

		fprintf(stge.print_file, "%s\n", s != NULL ? s : "");
	}
	else if (kind == STGE_PRINT_FLOAT)
	{
		uint32_t u = (uint32_t)value;
		float f;

		memcpy(&f, &u, sizeof f);
		fprintf(stge.print_file, "%g\n", (double)f);
	}
	else if (kind == STGE_PRINT_DOUBLE)
	{
		double d;

		memcpy(&d, &value, sizeof d);
		fprintf(stge.print_file, "%g\n", d);
	}
	else
		fprintf(stge.print_file, "%d\n", (int)value);

	fflush(stge.print_file);
}

bool stge_code_emit(const uint8_t *bytes, size_t n)
{
	VmProg *s = sc();

	if (n == 0)
		return true;

	if (s == NULL)
		return false;

	if (s->code_len + n > s->code_cap)
	{
		size_t cap = s->code_cap == 0 ? STGE_CODE_START : s->code_cap;
		uint8_t *grown;

		while (s->code_len + n > cap)
			cap *= 2;

		grown = realloc(s->code, cap);
		if (grown == NULL)
			return false;

		s->code = grown;
		s->code_cap = cap;
	}

	memcpy(s->code + s->code_len, bytes, n);
	s->code_len += n;
	return true;
}

int stge_intern_n(const char *s, size_t n)
{
	VmProg *scen;
	int i;
	char *copy;

	if (s == NULL)
		s = "";

	scen = sc();
	if (scen == NULL)
		return -1;

	for (i = 0; i < scen->str_count; i++)
	{
		if (strlen(scen->strs[i]) == n && memcmp(scen->strs[i], s, n) == 0)
			return i;
	}

	if (scen->str_count >= STGE_MAX_STRS)
		return -1;

	copy = malloc(n + 1);
	if (copy == NULL)
		return -1;

	memcpy(copy, s, n);
	copy[n] = '\0';
	scen->strs[scen->str_count] = copy;
	return scen->str_count++;
}

int stge_intern(const char *s)
{
	if (s == NULL)
		s = "";

	return stge_intern_n(s, strlen(s));
}

const char *stge_str(int idx)
{
	VmProg *s = sc();

	if (s == NULL || idx < 0 || idx >= s->str_count)
		return NULL;

	return s->strs[idx];
}

int stge_spawn(const char *name, const char *display)
{
	VmProg *s = sc();
	int n;
	Obj *obj;

	if (s == NULL || display == NULL)
		return -1;

	if (name == NULL)
		name = "";

	if (name[0] != '\0' && stge_find(name) >= 0)
		return -1;

	if (strlen(name) >= STGE_NAME_MAX)
		return -1;

	if (s->object_count >= s->objs_cap)
	{
		int ncap = s->objs_cap > 0 ? s->objs_cap * 2 : STGE_OBJS_INIT;
		Obj **grown = realloc(s->objs, (size_t)ncap * sizeof *grown);

		if (grown == NULL)
			return -1;
		memset(grown + s->objs_cap, 0, (size_t)(ncap - s->objs_cap) * sizeof *grown);
		s->objs = grown;
		s->objs_cap = ncap;
	}

	n = s->object_count;
	obj = arena_alloc(&s->arena, sizeof *obj);
	if (obj == NULL)
		return -1;

	obj_init(obj, n, name);
	s->objs[n] = obj;
	s->object_count = n + 1;

	if (display[0] != '\0')
	{
		if (!obj_mem_set_str(obj, "display", display))
		{
			obj_members_free(obj);
			s->objs[n] = NULL;
			return -1;
		}

		obj_mem_set_i_auto(obj, "width", 0, (int32_t)strlen(display));
		spatial_put(n);
		return n;
	}

	return n;
}

int stge_spawn_map(const char *name)
{
	return stge_spawn(name, "");
}

int stge_type_register(const char *name)
{
	VmProg *s = sc();
	int n;
	Obj *obj;

	if (s == NULL || name == NULL || name[0] == '\0')
		return -1;

	if (strlen(name) >= STGE_NAME_MAX)
		return -1;

	if (stge_find(name) >= 0)
		return -1;

	if (s->type_count >= s->types_cap)
	{
		int ncap = s->types_cap > 0 ? s->types_cap * 2 : STGE_OBJS_INIT;
		Obj **grown = realloc(s->types, (size_t)ncap * sizeof *grown);

		if (grown == NULL)
			return -1;
		memset(grown + s->types_cap, 0, (size_t)(ncap - s->types_cap) * sizeof *grown);
		s->types = grown;
		s->types_cap = ncap;
	}

	n = s->type_count;
	obj = arena_alloc(&s->arena, sizeof *obj);
	if (obj == NULL)
		return -1;

	obj_init(obj, n, name);
	obj->type_id = n;
	s->types[n] = obj;
	s->type_count = n + 1;
	return n;
}

static const uint8_t *string_obj_bytes(int id, uint32_t *n);

static int string_obj_from_bytes(const uint8_t *bytes, uint32_t n)
{
	VmProg *s = sc();
	int id;
	uint32_t i;

	id = stge_spawn_map("");
	if (id < 0)
	{
			return -1;
	}

	if (!stge_mem_new(id, "length", MEMBER_UINT, 1))
		return -1;

	stge_mem_set(id, "length", 0, (int32_t)n);
	if (!stge_mem_new_dyn(id, "buffer", MEMBER_BYTE))
		return -1;

	for (i = 0; i < n; i++)
	{
		if (!stge_dyn_add(id, "buffer", (int64_t)bytes[i]))
			return -1;
	}

	if (s != NULL)
	{
		int type = stge_find("String");

		if (type >= 0)
			s->objs[id]->type_id = type;
	}

	return id;
}

int stge_string_wrap(int intern_idx)
{
	VmProg *s = sc();
	const char *text;
	int id;
	uint32_t n;

	if (s == NULL || intern_idx < 0 || intern_idx >= s->str_count)
		return -1;

	if (s->wrap_id[intern_idx] >= 0)
	{
		int wid = s->wrap_id[intern_idx];

			return wid;
	}

	text = s->strs[intern_idx];
	if (text == NULL)
		text = "";

	n = (uint32_t)strlen(text);
	id = string_obj_from_bytes((const uint8_t *)text, n);
	if (id < 0)
		return -1;

	s->wrap_id[intern_idx] = id;
	return id;
}

void stge_string_clear(int id)
{
	Dyn *d;

	if (!valid_id(id))
		return;

	if (obj_member_get(stge_obj(id), "buffer") == NULL)
		stge_mem_new_dyn(id, "buffer", MEMBER_BYTE);
	if (obj_member_get(stge_obj(id), "length") == NULL)
		stge_mem_new(id, "length", MEMBER_UINT, 1);

	d = obj_member_dyn(stge_obj(id), "buffer");
	if (d != NULL)
		d->len = 0;
	stge_mem_set(id, "length", 0, 0);
}

bool stge_string_append_bytes(int id, const uint8_t *bytes, uint32_t n)
{
	uint32_t i;

	if (!valid_id(id))
		return false;

	if (obj_member_get(stge_obj(id), "buffer") == NULL
	    && !stge_mem_new_dyn(id, "buffer", MEMBER_BYTE))
		return false;
	if (obj_member_get(stge_obj(id), "length") == NULL
	    && !stge_mem_new(id, "length", MEMBER_UINT, 1))
		return false;

	for (i = 0; i < n; i++)
	{
		if (!stge_dyn_add(id, "buffer", (int64_t)(bytes != NULL ? bytes[i] : 0)))
			return false;
	}

	stge_mem_set(id, "length", 0, (int32_t)stge_mem_len(id, "buffer"));
	return true;
}

bool stge_string_append_obj(int dst, int src)
{
	uint32_t n = 0;
	const uint8_t *bytes = string_obj_bytes(src, &n);

	if (!valid_id(dst))
		return false;
	if (n == 0)
		return true;
	if (bytes == NULL)
		return false;
	return stge_string_append_bytes(dst, bytes, n);
}

bool stge_string_set_from_int(int id, int32_t n)
{
	StgeString s;
	bool ok;

	stge_string_clear(id);
	s = stge_string_new_from_int(n);
	ok = stge_string_append_bytes(id, s.buffer, s.length);
	free(s.buffer);
	return ok;
}

void stge_write(int fd, const void *buf, uint32_t count)
{
	if (buf == NULL || count == 0)
		return;

	(void)write(fd, buf, (size_t)count);
}

const void *stge_mem_data(int id, const char *name)
{
	const Member *m;

	if (!valid_id(id) || name == NULL)
		return NULL;

	name = str_bytes_key(id, name, false);
	m = obj_member_get(stge_obj(id), name);
	if (m == NULL || m->data == NULL)
		return NULL;

	if (m->growable)
		return ((const Dyn *)m->data)->data;

	return m->data;
}

bool stge_dyn_copy(int dst, const char *dname, int src, const char *sname)
{
	const Member *sm;
	Dyn *dd;
	uint32_t i;
	uint32_t n;

	if (!valid_id(dst) || !valid_id(src) || dname == NULL || sname == NULL)
		return false;

	sname = str_bytes_key(src, sname, false);
	dname = str_bytes_key(dst, dname, true);
	if (obj_member_get(stge_obj(dst), dname) == NULL
	    && !obj_member_new_dyn(stge_obj(dst), dname, MEMBER_BYTE))
		return false;

	sm = obj_member_get(stge_obj(src), sname);
	dd = obj_member_dyn(stge_obj(dst), dname);
	if (sm == NULL || sm->data == NULL || dd == NULL)
		return false;

	if (sm->growable)
	{
		const Dyn *sd = (const Dyn *)sm->data;

		n = dyn_len(sd);
		for (i = 0; i < n; i++)
		{
			const void *slot = dyn_at((Dyn *)sd, i);

			if (slot == NULL || !dyn_add(dd, slot))
				return false;
		}

		return true;
	}

	n = sm->len;
	if (sm->kind == MEMBER_BYTE)
	{
		const uint8_t *bytes = (const uint8_t *)sm->data;

		for (i = 0; i < n; i++)
		{
			if (!dyn_add(dd, &bytes[i]))
				return false;
		}

		return true;
	}

	return false;
}

bool stge_mem_copy(int dst, const char *dname, int src, const char *sname)
{
	const Member *sm;
	uint32_t i;
	uint32_t n;

	if (!valid_id(dst) || !valid_id(src) || dname == NULL || sname == NULL)
		return false;

	sm = obj_member_get(stge_obj(src), sname);
	if (sm == NULL || sm->growable)
		return false;

	n = sm->len;
	if (!stge_mem_new(dst, dname, sm->kind, n))
		return false;

	for (i = 0; i < n; i++)
	{
		if (sm->kind == MEMBER_FLOAT)
			stge_mem_set_f(dst, dname, i, stge_mem_get_f(src, sname, i));
		else if (sm->kind == MEMBER_DOUBLE)
			stge_mem_set_d(dst, dname, i, stge_mem_get_d(src, sname, i));
		else
			stge_mem_set(dst, dname, i, stge_mem_get(src, sname, i));
	}

	return true;
}

void stge_init_packed(int id, int type_id)
{
	Obj *type;
	Obj *inst;
	uint32_t i;
	int tindex;

	if (!valid_heap_id(id))
		return;

	tindex = stge_is_type_ref(type_id) ? stge_type_index(type_id) : type_id;
	type = stge_type(tindex);
	inst = stge_obj(id);
	if (type == NULL || inst == NULL || type->members.slots == NULL)
		return;

	inst->type_id = tindex;

	for (i = 0; i < type->members.cap; i++)
	{
		const Member *m = &type->members.slots[i];

		if (m->key == NULL || m->key[0] == '\0')
			continue;
		if (m->growable || m->host || m->kind == MEMBER_FUNCTION)
			continue;

		stge_mem_copy(id, m->key, stge_type_ref(tindex), m->key);
	}
}

int stge_find(const char *name)
{
	VmProg *s = sc();
	int i;

	if (s == NULL || name == NULL)
		return -1;

	for (i = 0; i < s->type_count; i++)
	{
		if (s->types[i] != NULL && strcmp(s->types[i]->name, name) == 0)
			return i;
	}

	return -1;
}

const FuncEntry *stge_fn_get(int id, const char *name, int argc)
{
	const FuncEntry *e;
	Obj *o;
	int type;

	if (!valid_id(id) || name == NULL)
		return NULL;

	o = stge_obj(id);
	if (o == NULL)
		return NULL;

	e = obj_fn_get(o, name, argc);
	if (e != NULL)
		return e;

	if (stge_is_type_ref(id))
		return NULL;

	type = o->type_id;
	if (type < 0 || stge_type(type) == NULL)
		return NULL;

	return obj_fn_get(stge_type(type), name, argc);
}

bool stge_add_child(int parent, int child)
{
	int old;

	if (!valid_heap_id(parent) || !valid_heap_id(child) || parent == child || would_cycle(parent, child))
		return false;

	detach_child(child);
	old = link_of(parent, "child");
	set_link(child, "sibling", old);
	set_link(parent, "child", child);
	set_link(child, "parent", parent);
	spatial_put_tree(child);
	return true;
}

bool stge_mem_new(int id, const char *name, MemberKind kind, uint32_t len)
{
	if (!valid_id(id) || name == NULL)
		return false;

	return obj_member_new(stge_obj(id), name, kind, len);
}

bool stge_mem_new_dyn(int id, const char *name, MemberKind kind)
{
	if (!valid_id(id) || name == NULL)
		return false;

	name = str_bytes_key(id, name, true);
	return obj_member_new_dyn(stge_obj(id), name, kind);
}

bool stge_dyn_add(int id, const char *name, int64_t value)
{
	if (!valid_id(id) || name == NULL)
		return false;

	name = str_bytes_key(id, name, true);
	if (obj_member_get(stge_obj(id), name) == NULL
	    && !obj_member_new_dyn(stge_obj(id), name, MEMBER_BYTE))
		return false;

	return obj_dyn_add_i(stge_obj(id), name, value);
}

uint32_t stge_mem_len(int id, const char *name)
{
	if (!valid_id(id) || name == NULL)
		return 0;

	name = str_bytes_key(id, name, false);
	return obj_mem_len(stge_obj(id), name);
}

static Vec2i vec2i_from_obj(int id)
{
	const Obj *obj;
	const char *tname;
	const Member *m;

	if (!valid_id(id))
		return VEC2I_ZERO;

	obj = stge_obj(id);
	if (obj->type_id >= 0 && obj->type_id < sc()->object_count)
	{
		tname = stge_obj(obj->type_id)->name;
		m = obj_member_get(obj, tname);
		if (m != NULL && m->len >= 2)
			return vec2i((int)stge_mem_get(id, tname, 0),
				     (int)stge_mem_get(id, tname, 1));
	}

	m = obj_member_get(obj, "pos");
	if (m != NULL && m->len >= 2)
		return vec2i((int)stge_mem_get(id, "pos", 0),
			     (int)stge_mem_get(id, "pos", 1));

	return VEC2I_ZERO;
}

static Pixel pixel_from_obj(int id)
{
	Pixel p = {' ', COLOR_DEFAULT, COLOR_DEFAULT};
	const Obj *obj;

	if (!valid_id(id))
		return p;

	obj = stge_obj(id);
	if (obj_member_get(obj, "char") != NULL)
		p.ch = (char)stge_mem_get(id, "char", 0);
	else if (obj_member_get(obj, "ch") != NULL)
		p.ch = (char)stge_mem_get(id, "ch", 0);
	if (obj_member_get(obj, "fg") != NULL)
		p.fg = (Color)stge_mem_get(id, "fg", 0);
	if (obj_member_get(obj, "bg") != NULL)
		p.bg = (Color)stge_mem_get(id, "bg", 0);
	return p;
}

static int pixel_to_obj(Pixel p)
{
	int id = stge_spawn_map("");
	int type = stge_find("Terminal::Screen::Pixel");

	if (id < 0)
		return -1;

	if (type >= 0)
		stge_obj(id)->type_id = type;

	stge_mem_set(id, "char", 0, (int32_t)(uint8_t)p.ch);
	stge_mem_set(id, "fg", 0, (int32_t)p.fg);
	stge_mem_set(id, "bg", 0, (int32_t)p.bg);
	return id;
}

static int32_t screen_flush_native(int32_t *args, int argc){
	(void)args;
	(void)argc;

	screen_flush(&stge.screen);
	return 0;
}

static int32_t screen_clear_native(int32_t *args, int argc){
	(void)args;
	(void)argc;

	screen_clear(&stge.screen);
	return 0;
}

static int32_t screen_size_native(int32_t *args, int argc){
	(void)args;
	(void)argc;

	Vec2i size = screen_size(&stge.screen);

	return (int32_t)((uint32_t)size.x | ((uint32_t)size.y << 16));
}

static int32_t screen_buf_native(int32_t *args, int argc){
	(void)args;
	(void)argc;

	return (int32_t)(uintptr_t)screen_buf(&stge.screen);
}

static int32_t screen_get_pixel_native(int32_t *args, int argc){
	int32_t pos = (argc > 0) ? args[0] : 0;

	return (int32_t)pixel_to_obj(screen_get_pixel(&stge.screen, vec2i_from_obj(pos)));
}

static int32_t screen_set_pixel_native(int32_t *args, int argc){
	int32_t pos = (argc > 0) ? args[0] : 0;
	int32_t pixel = (argc > 1) ? args[1] : 0;

	screen_set_pixel(&stge.screen, vec2i_from_obj(pos), pixel_from_obj(pixel));
	return 0;
}

static int32_t stge_string_append_char_native(int32_t *args, int argc){
	int32_t id = (argc > 0) ? args[0] : 0;
	int32_t ch = (argc > 1) ? args[1] : 0;

	if (!valid_id(id))
		return 0;
	if (obj_member_get(stge_obj(id), "buffer") == NULL)
		stge_mem_new_dyn(id, "buffer", MEMBER_BYTE);
	stge_dyn_add(id, "buffer", (int64_t)(uint8_t)ch);
	stge_mem_set(id, "length", 0, (int32_t)stge_mem_len(id, "buffer"));
	return id;
}

static int32_t stge_print_native(int32_t *args, int argc){
	int32_t a = (argc > 0) ? args[0] : 0;

	stge_print(STGE_PRINT_STR, a);
	return 0;
}

static int32_t stge_write_native(int32_t *args, int argc){
	int32_t a = (argc > 0) ? args[0] : 0;
	int32_t b = (argc > 1) ? args[1] : 0;
	int32_t c = (argc > 2) ? args[2] : 0;

	stge_write((int)a, (const void *)(uintptr_t)(uint32_t)b, (uint32_t)c);
	return 0;
}

static const uint8_t *string_obj_bytes(int id, uint32_t *n)
{
	const void *data;

	*n = 0;
	if (!valid_id(id))
		return NULL;

	*n = stge_mem_len(id, "buffer");
	if (*n == 0)
		return NULL;

	data = stge_mem_data(id, "buffer");
	return (const uint8_t *)data;
}

static int32_t stge_string_new_native(int32_t *args, int argc){
	(void)args;
	(void)argc;

	return string_obj_from_bytes(NULL, 0);
}

static int32_t stge_string_concat_native(int32_t *args, int argc){
	int32_t a = (argc > 0) ? args[0] : 0;
	int32_t b = (argc > 1) ? args[1] : 0;

	int id;

	id = string_obj_from_bytes(NULL, 0);
	if (id < 0)
		return -1;
	if (!stge_string_append_obj(id, a))
		return -1;
	if (!stge_string_append_obj(id, b))
		return -1;
	return id;
}

static int32_t stge_string_clear_native(int32_t *args, int argc){
	int32_t id = (argc > 0) ? args[0] : 0;

	stge_string_clear(id);
	return id;
}

static int32_t stge_string_append_native(int32_t *args, int argc){
	int32_t dst = (argc > 0) ? args[0] : 0;
	int32_t src = (argc > 1) ? args[1] : 0;

	if (!stge_string_append_obj(dst, src))
		return -1;
	return dst;
}

static int32_t stge_string_append_int_native(int32_t *args, int argc){
	int32_t dst = (argc > 0) ? args[0] : 0;
	int32_t n = (argc > 1) ? args[1] : 0;

	StgeString s;
	bool ok;

	s = stge_string_new_from_int(n);
	ok = stge_string_append_bytes(dst, s.buffer, s.length);
	free(s.buffer);
	return ok ? dst : -1;
}

static int32_t stge_int_to_str_native(int32_t *args, int argc){
	int32_t n = (argc > 0) ? args[0] : 0;

	int id;

	id = string_obj_from_bytes(NULL, 0);
	if (id < 0)
		return -1;
	if (!stge_string_set_from_int(id, n))
		return -1;
	return id;
}

static int32_t stge_input_poll_native(int32_t *args, int argc){
	(void)args;
	(void)argc;


	return input_poll(&stge.input) ? 1 : 0;
}

static int32_t stge_is_key_pressed_native(int32_t *args, int argc){
	int32_t a = (argc > 0) ? args[0] : 0;


	/* Read the bitmap from the last Input::poll / OP_POLL; do not poll here
	   or we clear keys before the caller can see them. */
	return input_is_key_pressed(&stge.input, (Key)a) ? 1 : 0;
}

void stge_input_bind(void)
{
	int t = stge_find("Input");

	if (t < 0)
		return;

	obj_member_bind(stge_type(t), "keys", MEMBER_BYTE,
			stge.input.pressed, (uint32_t)STGE_INPUT_PRESSED_BYTES);
}

void stge_std_register(void)
{
	stge_native_add("stge_input_poll", stge_input_poll_native);
	stge_native_add("stge_is_key_pressed", stge_is_key_pressed_native);
	stge_native_add("stge_print", stge_print_native);
	stge_native_add("stge_write", stge_write_native);
	stge_native_add("stge_int_to_str", stge_int_to_str_native);
	stge_native_add("screen_flush", screen_flush_native);
	stge_native_add("screen_clear", screen_clear_native);
	stge_native_add("screen_size", screen_size_native);
	stge_native_add("screen_buf", screen_buf_native);
	stge_native_add("screen_get_pixel", screen_get_pixel_native);
	stge_native_add("screen_set_pixel", screen_set_pixel_native);
	stge_native_add("stge_string_append_char", stge_string_append_char_native);
	stge_native_add("stge_string_concat", stge_string_concat_native);
	stge_native_add("stge_string_new", stge_string_new_native);
	stge_native_add("stge_string_clear", stge_string_clear_native);
	stge_native_add("stge_string_append", stge_string_append_native);
	stge_native_add("stge_string_append_int", stge_string_append_int_native);
}

int32_t stge_mem_get(int id, const char *name, uint32_t i)
{
	if (!valid_id(id) || name == NULL)
		return 0;

	name = str_bytes_key(id, name, false);
	return obj_mem_get_i(stge_obj(id), name, i);
}

void stge_obj_set(int id, const char *name, int32_t value)
{
	if (!valid_id(id) || name == NULL)
		return;

	name = str_bytes_key(id, name, false);
	(void)obj_obj_set(stge_obj(id), name, value);
}

void stge_mem_set(int id, const char *name, uint32_t i, int32_t value)
{
	int32_t old = 0;
	const Member *m;

	if (!valid_id(id) || name == NULL)
		return;

	name = str_bytes_key(id, name, false);
	m = obj_member_get(stge_obj(id), name);
	if (m != NULL && i < m->len)
		old = obj_mem_get_i(stge_obj(id), name, i);

	obj_mem_set_i_auto(stge_obj(id), name, i, value);

	(void)old;
	if (is_pos_name(name) || strcmp(name, "visible") == 0)
		spatial_put_tree(id);
}

float stge_mem_get_f(int id, const char *name, uint32_t i)
{
	if (!valid_id(id) || name == NULL)
		return 0.0f;

	return obj_mem_get_f(stge_obj(id), name, i);
}

void stge_mem_set_f(int id, const char *name, uint32_t i, float value)
{
	if (!valid_id(id) || name == NULL)
		return;

	obj_mem_set_f(stge_obj(id), name, i, value);
}

double stge_mem_get_d(int id, const char *name, uint32_t i)
{
	if (!valid_id(id) || name == NULL)
		return 0.0;

	return obj_mem_get_d(stge_obj(id), name, i);
}

void stge_mem_set_d(int id, const char *name, uint32_t i, double value)
{
	if (!valid_id(id) || name == NULL)
		return;

	obj_mem_set_d(stge_obj(id), name, i, value);
}

void stge_set_display(int id, const char *display)
{
	if (!valid_id(id))
		return;

	if (display == NULL)
		display = "";

	if (obj_mem_set_str(stge_obj(id), "display", display))
		obj_mem_set_i_auto(stge_obj(id), "width", 0, (int32_t)strlen(display));
}

void stge_set_x(int id, int x)
{
	stge_mem_set(id, "x", 0, x);
}

void stge_set_y(int id, int y)
{
	stge_mem_set(id, "y", 0, y);
}

void stge_set_x_y(int id, int x, int y)
{
	stge_set_x(id, x);
	stge_set_y(id, y);
}

void stge_set_z(int id, int z)
{
	stge_mem_set(id, "z", 0, z);
}

void stge_set_fg(int id, Color color)
{
	stge_mem_set(id, "fg", 0, (int32_t)color);
}

void stge_set_bg(int id, Color color)
{
	stge_mem_set(id, "bg", 0, (int32_t)color);
}

void stge_set_visible(int id, bool visible)
{
	stge_mem_set(id, "visible", 0, visible ? 1 : 0);
}

void stge_center_x(int id)
{
	const char *display;
	Vec2i size;

	if (!valid_id(id))
		return;

	display = obj_mem_str(stge_obj(id), "display");
	size = stge_size();
	stge_set_x(id, size.x / 2 - (display != NULL ? (int)strlen(display) : 0) / 2);
}

void stge_center_y(int id)
{
	if (valid_id(id))
		stge_set_y(id, stge_size().y / 2);
}

void stge_center_xy(int id)
{
	stge_center_x(id);
	stge_center_y(id);
}

static int pos_follow(int id, uint32_t axis, const char *scalar)
{
	const Obj *obj;
	const Member *m;
	int pid;
	const char *comp;

	if (!valid_id(id))
		return 0;

	obj = stge_obj(id);
	m = obj_member_get(obj, "pos");
	if (m == NULL)
		m = obj_member_get(obj, "position");

	if (m != NULL && m->len == 1)
	{
		pid = (int)obj_mem_get_i(obj, m->key, 0);
		comp = axis == 0 ? "x" : (axis == 1 ? "y" : "z");
		if (valid_id(pid) && obj_member_get(stge_obj(pid), comp) != NULL)
			return (int)obj_mem_get_i(stge_obj(pid), comp, 0);
	}

	if (m != NULL && m->len > axis)
		return (int)obj_mem_get_i(obj, m->key, axis);

	return (int)stge_mem_get(id, scalar, 0);
}

int stge_get_x(int id)
{
	return pos_follow(id, 0, "x");
}

int stge_get_y(int id)
{
	return pos_follow(id, 1, "y");
}

int stge_get_z(int id)
{
	return pos_follow(id, 2, "z");
}

int stge_world_x(int id)
{
	int x = 0;
	int n = 0;

	while (valid_id(id) && n++ < 1000000)
	{
		x += stge_get_x(id);
		id = link_of(id, "parent");
	}

	return x;
}

int stge_world_y(int id)
{
	int y = 0;
	int n = 0;

	while (valid_id(id) && n++ < 1000000)
	{
		y += stge_get_y(id);
		id = link_of(id, "parent");
	}

	return y;
}

int stge_world_z(int id)
{
	int z = 0;
	int n = 0;

	while (valid_id(id) && n++ < 1000000)
	{
		z += stge_get_z(id);
		id = link_of(id, "parent");
	}

	return z;
}

int stge_get_fg(int id)
{
	return valid_id(id) ? (int)stge_mem_get(id, "fg", 0) : 0;
}

int stge_get_bg(int id)
{
	return valid_id(id) ? (int)stge_mem_get(id, "bg", 0) : 0;
}

bool stge_get_visible(int id)
{
	return valid_id(id) && obj_visible(stge_obj(id));
}

void stge_quit(int code)
{
	stge.quit = true;
	stge.exit_code = code;
}

int stge_exit_code(void)
{
	return stge.exit_code;
}

bool stge_load(const char *path)
{
	vm_prog_reset(&stge.prog);
	spatial_init();

	if (!parser_compile(path))
	{
		vm_prog_reset(&stge.prog);
		return false;
	}

	stge_input_bind();
	stge.cam_pos[0] = 0;
	stge.cam_pos[1] = 0;
	return true;
}

bool stge_parse_failed(void)
{
	return parser_had_error();
}

void stge_clear(void)
{
	vm_prog_reset(&stge.prog);
	stge.cam_pos[0] = 0;
	stge.cam_pos[1] = 0;
	spatial_init();
}

void stge_render(void)
{
	screen_flush(&stge.screen);
}

static int64_t *run_tick;

void stge_poll_input(void)
{
	input_poll(&stge.input);
}

void stge_present_frame(void)
{
	spatial_refresh();
	stge_render();
}

void stge_wait_tick(void)
{
	int32_t rate = stge.tickrate < 1 ? 1 : stge.tickrate;
	struct timespec interval;

	interval.tv_sec = 0;
	interval.tv_nsec = (long)(1000000000L / rate);
	nanosleep(&interval, NULL);
}

void stge_run(int64_t *tick)
{
	VmProg *s = sc();

	if (!screen_init(&stge.screen))
	{
		stge_quit(1);
		return;
	}
	if (!input_init(&stge.input))
	{
		stge_quit(1);
		return;
	}

	run_tick = tick;
	if (s != NULL && s->code != NULL && s->code_len > 0)
		vm_run(s->init_pc >= 0 ? s->init_pc : 0, -1);
	run_tick = NULL;
}
