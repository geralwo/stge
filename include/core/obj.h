#ifndef STGE_CORE_OBJ_H
#define STGE_CORE_OBJ_H

#include <stdbool.h>
#include <stdint.h>

#include "utils/dyn.h"

#define STGE_NAME_MAX 32

typedef struct
{
	char name[STGE_NAME_MAX];
	int pc;
	bool has_arg;
	int argc;
	int self_param; /* 0 if first param is bare type-name self, else -1 */
	bool is_extern;
	int native_id;
	char ret_type[STGE_NAME_MAX];
} ObjFn;

typedef struct
{
	int pc;
	int argc;
	bool has_arg;
	bool is_extern;
	int native_id;
} FuncEntry;

typedef enum
{
	MEMBER_INT,
	MEMBER_UINT,
	MEMBER_STRING,
	MEMBER_FLOAT,
	MEMBER_DOUBLE,
	MEMBER_BYTE,
	MEMBER_ARRAY,
	MEMBER_FUNCTION,
	MEMBER_OBJECT,
} MemberKind;

/* One named array owned by an obj. */
typedef struct
{
	char *key;
	MemberKind kind;
	uint32_t len;
	bool growable; /* T[]: data is Dyn* */
	bool host; /* data owned by engine; do not free */
	void *data;
} Member;

/* Open-addressed string map. The obj owns keys and arrays. */
typedef struct
{
	Member *slots;
	uint32_t cap;
	uint32_t count;
} MemberTable;

typedef struct
{
	int id;
	char name[STGE_NAME_MAX];
	MemberTable members;
	int type_id; /* index into VmProg.types[]; for type objs equals own index */
} Obj;

void obj_init(Obj *obj, int id, const char *name);
void obj_members_free(Obj *obj);

const Member *obj_member_get(const Obj *obj, const char *key);
bool obj_member_new(Obj *obj, const char *key, MemberKind kind, uint32_t len);
bool obj_member_bind(Obj *obj, const char *key, MemberKind kind, void *data, uint32_t len);
bool obj_member_new_dyn(Obj *obj, const char *key, MemberKind kind);
Dyn *obj_member_dyn(Obj *obj, const char *key);
uint32_t obj_mem_len(const Obj *obj, const char *key);
bool obj_dyn_add_i(Obj *obj, const char *key, int64_t value);

int32_t obj_mem_get_i(const Obj *obj, const char *key, uint32_t i);
uint32_t obj_mem_get_u(const Obj *obj, const char *key, uint32_t i);
void obj_mem_set_i(Obj *obj, const char *key, uint32_t i, int32_t value);
/* Missing name and i == 0 creates int(1). */
void obj_mem_set_i_auto(Obj *obj, const char *key, uint32_t i, int32_t value);

float obj_mem_get_f(const Obj *obj, const char *key, uint32_t i);
void obj_mem_set_f(Obj *obj, const char *key, uint32_t i, float value);
double obj_mem_get_d(const Obj *obj, const char *key, uint32_t i);
void obj_mem_set_d(Obj *obj, const char *key, uint32_t i, double value);

const char *obj_mem_str(const Obj *obj, const char *key);
const char *obj_mem_str_i(const Obj *obj, const char *key, uint32_t i);
bool obj_mem_set_str(Obj *obj, const char *key, const char *s);
bool obj_mem_set_str_i(Obj *obj, const char *key, uint32_t i, const char *s);

const FuncEntry *obj_fn_get(const Obj *obj, const char *name, int argc);
bool obj_fn_set(Obj *obj, const char *name, int pc, int argc, bool has_arg,
		bool is_extern, int native_id);

int32_t obj_obj_get(const Obj *obj, const char *key);
bool obj_obj_set(Obj *obj, const char *key, int32_t id);

/* Engine helper: missing visible counts as shown. */
bool obj_visible(const Obj *obj);

#endif /* STGE_CORE_OBJ_H */
