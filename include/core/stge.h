#ifndef STGE_CORE_STGE_H
#define STGE_CORE_STGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "core/array.h"
#include "core/cstr.h"
#include "core/input.h"
#include "core/native.h"
#include "core/screen.h"
#include "core/stge_string.h"
#include "core/obj.h"
#include "utils/vec.h"

#define STGE_TICKRATE 60
#define STGE_SPATIAL_TILE 8
#define STGE_SPATIAL_BUCKETS 64
#define STGE_MAX_STRS 256
#define STGE_CODE_START 1024
#define STGE_ARENA_BLOCK 8192
#define STGE_OBJS_INIT 64

/* Type table ids are encoded as negative refs on the stack / in bytecode
   payloads so they never collide with heap instance ids (>= 0).
   -1 is reserved as "none"; type index 0 maps to -2. */
static inline int stge_type_ref(int type_index)
{
	return -type_index - 2;
}

static inline bool stge_is_type_ref(int id)
{
	return id <= -2;
}

static inline int stge_type_index(int type_ref)
{
	return -type_ref - 2;
}

typedef struct ArenaBlock ArenaBlock;

typedef struct
{
	ArenaBlock *head;
	ArenaBlock *curr;
} Arena;

/* Bytecode, interns, type table, and instance heap. */
typedef struct
{
	Arena arena;
	Obj **types;
	int type_count;
	int types_cap;
	Obj **objs; /* instance heap */
	int object_count;
	int objs_cap;
	char *strs[STGE_MAX_STRS];
	int wrap_id[STGE_MAX_STRS];
	int str_count;
	uint8_t *code;
	size_t code_len;
	size_t code_cap;
	int init_pc;
} VmProg;

void *arena_alloc(Arena *a, size_t n);
void arena_reset(Arena *a);

bool vm_prog_init(VmProg *p);
void vm_prog_reset(VmProg *p);
void vm_prog_shutdown(VmProg *p);

typedef struct SpatialNode
{
	int id;
	int32_t wx;
	int32_t wy;
	int next;
} SpatialNode;

typedef struct
{
	Screen screen;
	Input input;

	VmProg prog;

	int32_t cam_pos[2];
	int32_t tickrate;
	uint32_t rng;
	char *print_out;
	FILE *print_file;

	int spatial_heads[STGE_SPATIAL_BUCKETS];
	SpatialNode *spatial_nodes;
	bool *spatial_live;
	int spatial_cap;

	bool quit;
	int exit_code;
} Stge;

extern Stge stge;

bool stge_init(void);
void stge_shutdown(void);

VmProg *stge_prog(void);
Obj *stge_obj(int id);
Obj *stge_type(int type_index);
void *stge_alloc(size_t n);

Vec2i stge_size(void);
int stge_width(void);
int stge_height(void);

int32_t stge_cam_get(uint32_t i);
void stge_cam_set(uint32_t i, int32_t value);
void stge_cam_center_on(int32_t x, int32_t y);

int32_t stge_tickrate(void);
void stge_set_tickrate(int32_t rate);
int32_t stge_rand(int32_t n);

const char *stge_print_out(void);
void stge_set_print_out(const char *path);
void stge_print(uint8_t kind, int64_t value);

#define STGE_PRINT_INT 0
#define STGE_PRINT_STR 1
#define STGE_PRINT_FLOAT 2
#define STGE_PRINT_DOUBLE 3

bool stge_code_emit(const uint8_t *bytes, size_t n);

int stge_intern(const char *s);
int stge_intern_n(const char *s, size_t n);
const char *stge_str(int idx);

int stge_spawn(const char *name, const char *display);
int stge_spawn_map(const char *name);
/* Register a named type (no heap instance). Returns type index (>= 0), or -1. */
int stge_type_register(const char *name);
int stge_string_wrap(int intern_idx);
void stge_string_clear(int id);
bool stge_string_append_bytes(int id, const uint8_t *bytes, uint32_t n);
bool stge_string_append_obj(int dst, int src);
bool stge_string_set_from_int(int id, int32_t n);
void stge_write(int fd, const void *buf, uint32_t count);
const void *stge_mem_data(int id, const char *name);
bool stge_dyn_copy(int dst, const char *dname, int src, const char *sname);
bool stge_mem_copy(int dst, const char *dname, int src, const char *sname);
void stge_init_packed(int id, int type_id);
int stge_find(const char *name);
void stge_input_bind(void);
const FuncEntry *stge_fn_get(int id, const char *name, int argc);
bool stge_add_child(int parent, int child);
int stge_parent(int id);

bool stge_mem_new(int id, const char *name, MemberKind kind, uint32_t len);
bool stge_mem_new_dyn(int id, const char *name, MemberKind kind);
bool stge_dyn_add(int id, const char *name, int64_t value);
uint32_t stge_mem_len(int id, const char *name);
int32_t stge_mem_get(int id, const char *name, uint32_t i);
void stge_mem_set(int id, const char *name, uint32_t i, int32_t value);
float stge_mem_get_f(int id, const char *name, uint32_t i);
void stge_mem_set_f(int id, const char *name, uint32_t i, float value);
double stge_mem_get_d(int id, const char *name, uint32_t i);
void stge_mem_set_d(int id, const char *name, uint32_t i, double value);

void stge_set_display(int id, const char *display);
void stge_set_x(int id, int x);
void stge_set_y(int id, int y);
void stge_set_x_y(int id, int x, int y);
void stge_set_z(int id, int z);
void stge_set_fg(int id, Color color);
void stge_set_bg(int id, Color color);
void stge_set_visible(int id, bool visible);
void stge_center_x(int id);
void stge_center_y(int id);
void stge_center_xy(int id);
void stge_obj_set(int id, const char *name, int32_t value);
int stge_get_x(int id);
int stge_get_y(int id);
int stge_get_z(int id);
int stge_world_x(int id);
int stge_world_y(int id);
int stge_world_z(int id);
int stge_get_fg(int id);
int stge_get_bg(int id);
bool stge_get_visible(int id);
void stge_quit(int code);
int stge_exit_code(void);
void stge_poll_input(void);
void stge_present_frame(void);
void stge_wait_tick(void);

bool stge_load(const char *path);
bool stge_parse_failed(void);
void stge_clear(void);
void stge_render(void);
void stge_run(int64_t *tick);

#endif /* STGE_CORE_STGE_H */
