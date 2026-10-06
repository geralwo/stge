#ifndef STGE_CORE_VM_H
#define STGE_CORE_VM_H

#include <stdbool.h>
#include <stdint.h>

#include "core/stge.h"

typedef enum
{
	OP_HALT = 0,
	OP_PUSH,
	OP_PUSH_STR,
	OP_PUSH_SELF,
	OP_PUSH_NODE, /* fetch i32 type/heap ref */
	OP_POP,
	OP_JMP,
	OP_JZ,
	OP_NOT,
	OP_EQ,
	OP_LT,
	OP_GT,
	OP_AND,
	OP_OR,
	OP_ADD,
	OP_SUB,
	OP_MUL,
	OP_DIV,
	OP_SWAP,

	OP_QUIT,
	OP_IS_KEY,
	OP_FIND,
	OP_SPAWN,
	OP_SPAWN_MAP, /* pop name; container obj, no display */
	OP_STR, /* pop value, push interned string */
	OP_PUSH_ARG, /* fetch u16 index */
	OP_CENTER_X,
	OP_CENTER_Y,
	OP_CENTER_XY,
	OP_ADD_CHILD,
	OP_GET_WIDTH,
	OP_GET_HEIGHT,
	OP_GET_CAM, /* pop index */
	OP_SET_CAM, /* pop value, pop index */
	OP_CAM_CENTER_ON, /* pop y, pop x */
	OP_GET_TICKRATE,
	OP_SET_TICKRATE, /* pop rate */

	OP_NEW_INT,  /* pop n, pop name, pop id */
	OP_NEW_UINT, /* pop n, pop name, pop id */
	OP_NEW_STR,  /* pop n, pop name, pop id */
	OP_NEW_OBJ,  /* pop n, pop name, pop id; MEMBER_OBJECT */
	OP_NEW_ARRAY, /* pop n, pop name, pop id; MEMBER_ARRAY */
	OP_GET_MEM,     /* pop index, pop name, pop id */
	OP_SET_MEM,     /* pop value, pop index, pop name, pop id */
	OP_SET_OBJ,     /* pop value, pop index, pop name, pop id; MEMBER_OBJECT */
	OP_GET_MEM_STR, /* pop index, pop name, pop id; push interned string */
	OP_SET_MEM_STR, /* pop interned string, pop index, pop name, pop id */
	OP_SET_DISPLAY, /* pop str, pop id */
	OP_CALL, /* fetch u16 argc; pop pc, id, then argc args; push return */
	OP_CALL_NS, /* fetch u16 argc; pop pc, id, then argc args; push return */
	OP_RAND, /* pop n; push 0 .. n-1 */

	OP_NEW_FLOAT,
	OP_NEW_DOUBLE,
	OP_PUSH_F,
	OP_PUSH_D,
	OP_GET_MEM_F,
	OP_SET_MEM_F,
	OP_GET_MEM_D,
	OP_SET_MEM_D,
	OP_ADD_F,
	OP_SUB_F,
	OP_EQ_F,
	OP_LT_F,
	OP_GT_F,
	OP_ADD_D,
	OP_SUB_D,
	OP_EQ_D,
	OP_LT_D,
	OP_GT_D,
	OP_I2F,
	OP_I2D,
	OP_F2D,
	OP_F2I,
	OP_D2I,
	OP_D2F,
	OP_STR_F,
	OP_STR_D,
	OP_STR_OBJ, /* pop obj id; call id::to_str or default; push string */
	OP_STR_MEM, /* pop name, pop id; format that member; push string */
	OP_PRINT, /* pop interned string; write one line */
	OP_GET_PRINT_OUT,
	OP_SET_PRINT_OUT, /* pop interned path */

	OP_NEW_BYTE, /* pop n, pop name, pop id */
	OP_NEW_DYN,  /* pop name, pop id; fetch u8 MemberKind */
	OP_DYN_ADD,  /* pop value, pop name, pop id */
	OP_GET_LEN,  /* pop name, pop id; push len */
	OP_CALL_NEW, /* fetch u16 argc; pop pc, type_id, then argc args; spawn, run, push new id */
	OP_EXTERN,   /* fetch u8 native_id; fetch u16 argc; pop argc args */
	OP_STRING_WRAP, /* pop intern idx; push String object id */
	OP_GET_BUF,  /* pop name, pop id; push member data pointer */
	OP_DYN_COPY, /* pop src_name, src_id, dst_name, dst_id */
	OP_WRITE,    /* pop count, pop buf, pop fd; POSIX write */
	OP_GET_LOCAL, /* fetch u16 index; push locals[i] */
	OP_SET_LOCAL, /* pop value; fetch u16 index; locals[i] = value */
	OP_MEM_COPY, /* pop src_name, src_id, dst_name, dst_id; copy fixed member */
	OP_GET_SCREEN_SIZE, /* push width, then unused; host screen size */
	OP_GET_SCREEN_BUF, /* stub; push 0 */
	OP_POLL, /* input_poll */
	OP_RENDER, /* present if needed */
	OP_WAIT_TICK, /* sleep 1/tickrate; halt if quit */

	OP_COUNT
} Op;

typedef struct
{
	int64_t *stack;
	int sp;
	int stack_cap;
	int64_t *locals;
	int locals_cap;
	int64_t *args;
	int args_cap;
	int argc;
	int pc;
	int self;
	bool halted;
} Vm;

void vm_run(int pc, int self);

void vm_script_locals_reset(void);

#endif /* STGE_CORE_VM_H */
