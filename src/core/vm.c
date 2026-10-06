#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "core/vm.h"

static int64_t vm_call(int pc, int self, const int64_t *args, int argc);

/* Script slots are shared across nested vm_call frames so file-scope names
   (e.g. test_quit) stay visible inside methods. */
static int64_t *script_locals;
static int script_locals_cap;

static void script_locals_reset(void)
{
	free(script_locals);
	script_locals = NULL;
	script_locals_cap = 0;
}

void vm_script_locals_reset(void)
{
	script_locals_reset();
}

static bool locals_grow(Vm *vm, int need)
{
	int cap;
	int64_t *grown;
	int i;

	(void)vm;

	if (need <= script_locals_cap)
		return true;

	cap = script_locals_cap > 0 ? script_locals_cap : 16;
	while (cap < need)
	{
		if (cap > 1 << 28)
			return false;
		cap *= 2;
	}

	grown = realloc(script_locals, (size_t)cap * sizeof *grown);
	if (grown == NULL)
		return false;

	for (i = script_locals_cap; i < cap; i++)
		grown[i] = 0;
	script_locals = grown;
	script_locals_cap = cap;
	return true;
}

static void vm_fail(Vm *vm, const char *msg)
{
	fprintf(stderr, "vm: %s at pc %d\n", msg, vm->pc);
	vm->halted = true;
}

static void require_member(int id, const char *key)
{
	VmProg *s = stge_prog();
	const Member *m;

	if (s == NULL || key == NULL || stge_obj(id) == NULL)
		return;

	m = obj_member_get(stge_obj(id), key);
	if (m != NULL)
		return;

	fprintf(stderr, "stge: undefined member '%s'\n", key);
	abort();
}

static void mem_bounds_or_crash(int id, const char *key, int32_t idx)
{
	VmProg *s = stge_prog();
	const Member *m;
	uint32_t len;

	if (s == NULL || key == NULL || stge_obj(id) == NULL)
		return;

	m = obj_member_get(stge_obj(id), key);
	if (m == NULL)
		return;

	if (!m->growable && m->len <= 1)
		return;

	len = obj_mem_len(stge_obj(id), key);
	if (idx < 0 || (uint32_t)idx >= len)
	{
		fprintf(stderr, "stge: index %d out of range for array[%u]\n",
			(int)idx, (unsigned)len);
		abort();
	}
}

static bool stack_grow(Vm *vm, int need)
{
	int cap;
	int64_t *grown;

	if (need <= vm->stack_cap)
		return true;

	cap = vm->stack_cap > 0 ? vm->stack_cap : 64;
	while (cap < need)
	{
		if (cap > 1 << 28)
			return false;
		cap *= 2;
	}

	grown = realloc(vm->stack, (size_t)cap * sizeof *grown);
	if (grown == NULL)
		return false;

	vm->stack = grown;
	vm->stack_cap = cap;
	return true;
}

static void stack_push(Vm *vm, int64_t value)
{
	if (!stack_grow(vm, vm->sp + 1))
	{
		vm_fail(vm, "stack overflow");
		return;
	}

	vm->stack[vm->sp++] = value;
}

static int64_t stack_pop(Vm *vm)
{
	if (vm->sp <= 0)
	{
		vm_fail(vm, "stack underflow");
		return 0;
	}

	return vm->stack[--vm->sp];
}

static int32_t stack_pop_i(Vm *vm)
{
	return (int32_t)stack_pop(vm);
}

static int64_t fbits(float f)
{
	uint32_t u;

	memcpy(&u, &f, sizeof u);
	return (int64_t)u;
}

static float ffrom(int64_t v)
{
	uint32_t u = (uint32_t)v;
	float f;

	memcpy(&f, &u, sizeof f);
	return f;
}

static int64_t dbits(double d)
{
	int64_t u;

	memcpy(&u, &d, sizeof u);
	return u;
}

static double dfrom(int64_t v)
{
	double d;

	memcpy(&d, &v, sizeof d);
	return d;
}

static int intern_num(Vm *vm, const char *s)
{
	int idx = stge_intern(s);

	if (idx < 0)
		vm_fail(vm, "str: intern failed");

	return idx;
}

static uint8_t fetch_u8(Vm *vm)
{
	VmProg *s = stge_prog();

	if (s == NULL || vm->pc < 0 || (size_t)vm->pc >= s->code_len)
	{
		vm_fail(vm, "pc past code");
		return OP_HALT;
	}

	return s->code[vm->pc++];
}

static uint16_t fetch_u16(Vm *vm)
{
	uint16_t lo = fetch_u8(vm);
	uint16_t hi = fetch_u8(vm);

	return (uint16_t)(lo | (hi << 8));
}

static int16_t fetch_i16(Vm *vm)
{
	return (int16_t)fetch_u16(vm);
}

static int32_t fetch_i32(Vm *vm)
{
	uint32_t b0 = fetch_u8(vm);
	uint32_t b1 = fetch_u8(vm);
	uint32_t b2 = fetch_u8(vm);
	uint32_t b3 = fetch_u8(vm);

	return (int32_t)(b0 | (b1 << 8) | (b2 << 16) | (b3 << 24));
}

static uint32_t fetch_u32(Vm *vm)
{
	return (uint32_t)fetch_i32(vm);
}

static int64_t fetch_i64(Vm *vm)
{
	uint64_t lo = fetch_u32(vm);
	uint64_t hi = fetch_u32(vm);

	return (int64_t)(lo | (hi << 32));
}

void vm_run(int pc, int self)
{
	(void)vm_call(pc, self, NULL, 0);
}

static int obj_str_pc(int id)
{
	const FuncEntry *e;

	e = stge_fn_get(id, "to_str", 0);
	if (e != NULL && e->pc >= 0)
		return e->pc;

	e = stge_fn_get(id, "#to_str", 0);
	if (e != NULL && e->pc >= 0)
		return e->pc;

	return -1;
}

static int intern_or_fail(Vm *vm, const char *s)
{
	int idx = stge_intern(s != NULL ? s : "");

	if (idx < 0)
		vm_fail(vm, "str: intern failed");

	return idx;
}

static int default_obj_str(Vm *vm, int id)
{
	VmProg *s = stge_prog();
	const char *disp;

	if (s == NULL || id < 0 || id >= s->object_count)
		return intern_or_fail(vm, "");

	disp = obj_mem_str(stge_obj(id), "display");
	if (disp != NULL && disp[0] != '\0')
		return intern_or_fail(vm, disp);

	return intern_or_fail(vm, stge_obj(id)->name);
}

static int64_t vm_call(int pc, int self, const int64_t *args, int argc)
{
	Vm vm;
	int64_t ret = 0;

	if (pc < 0)
		return 0;

	memset(&vm, 0, sizeof vm);
	vm.pc = pc;
	vm.self = self;
	vm.argc = argc;
	if (args != NULL && argc > 0)
	{
		vm.args = malloc((size_t)argc * sizeof *vm.args);
		if (vm.args == NULL)
			return 0;
		memcpy(vm.args, args, (size_t)argc * sizeof *vm.args);
		vm.args_cap = argc;
	}

	while (!vm.halted)
	{
		uint8_t op = fetch_u8(&vm);

		if (vm.halted)
			break;

		switch (op)
		{
		case OP_HALT:
			if (vm.sp > 0)
				ret = stack_pop(&vm);
			vm.halted = true;
			break;

		case OP_PUSH:
			stack_push(&vm, fetch_i32(&vm));
			break;

		case OP_PUSH_STR:
			stack_push(&vm, fetch_u16(&vm));
			break;

		case OP_PUSH_SELF:
			stack_push(&vm, vm.self);
			break;

		case OP_PUSH_NODE:
			stack_push(&vm, fetch_i32(&vm));
			break;

		case OP_POP:
			stack_pop(&vm);
			break;

		case OP_JMP:
			vm.pc += fetch_i16(&vm);
			break;

		case OP_JZ:
		{
			int16_t off = fetch_i16(&vm);

			if (stack_pop(&vm) == 0)
				vm.pc += off;
			break;
		}

		case OP_NOT:
			stack_push(&vm, stack_pop(&vm) == 0 ? 1 : 0);
			break;

		case OP_EQ:
		{
			int32_t b = stack_pop(&vm);
			int32_t a = stack_pop(&vm);

			stack_push(&vm, a == b ? 1 : 0);
			break;
		}

		case OP_LT:
		{
			int32_t b = stack_pop(&vm);
			int32_t a = stack_pop(&vm);

			stack_push(&vm, a < b ? 1 : 0);
			break;
		}

		case OP_GT:
		{
			int32_t b = stack_pop(&vm);
			int32_t a = stack_pop(&vm);

			stack_push(&vm, a > b ? 1 : 0);
			break;
		}

		case OP_AND:
		{
			int32_t b = stack_pop(&vm);
			int32_t a = stack_pop(&vm);

			stack_push(&vm, (a != 0 && b != 0) ? 1 : 0);
			break;
		}

		case OP_OR:
		{
			int32_t b = stack_pop(&vm);
			int32_t a = stack_pop(&vm);

			stack_push(&vm, (a != 0 || b != 0) ? 1 : 0);
			break;
		}

		case OP_ADD:
		{
			int32_t b = stack_pop(&vm);
			int32_t a = stack_pop(&vm);

			stack_push(&vm, a + b);
			break;
		}

		case OP_SUB:
		{
			int32_t b = stack_pop(&vm);
			int32_t a = stack_pop(&vm);

			stack_push(&vm, a - b);
			break;
		}

		case OP_MUL:
		{
			int32_t b = stack_pop(&vm);
			int32_t a = stack_pop(&vm);

			stack_push(&vm, a * b);
			break;
		}

		case OP_DIV:
		{
			int32_t b = stack_pop(&vm);
			int32_t a = stack_pop(&vm);

			if (b == 0)
			{
				vm_fail(&vm, "divide by zero");
				break;
			}

			stack_push(&vm, a / b);
			break;
		}

		case OP_SWAP:
		{
			int64_t b = stack_pop(&vm);
			int64_t a = stack_pop(&vm);

			stack_push(&vm, b);
			stack_push(&vm, a);
			break;
		}

		case OP_QUIT:
			stge_quit(stack_pop_i(&vm));
			stack_push(&vm, 0);
			vm.halted = true;
			break;

		case OP_IS_KEY:
		{
			Key key = (Key)stack_pop(&vm);

			stack_push(&vm, input_is_key_pressed(&stge.input, key) ? 1 : 0);
			break;
		}

		case OP_FIND:
		{
			int t = stge_find(stge_str(stack_pop(&vm)));

			stack_push(&vm, t >= 0 ? stge_type_ref(t) : (int64_t)INT32_MIN);
			break;
		}

		case OP_SPAWN:
		{
			int32_t display = stack_pop(&vm);
			int32_t name = stack_pop(&vm);

			stack_push(&vm, stge_spawn(stge_str(name), stge_str(display)));
			break;
		}

		case OP_SPAWN_MAP:
			stack_push(&vm, stge_spawn_map(stge_str(stack_pop(&vm))));
			break;

		case OP_STR:
		{
			char buf[32];

			snprintf(buf, sizeof buf, "%d", stack_pop_i(&vm));
			stack_push(&vm, intern_num(&vm, buf));
			break;
		}

		case OP_PUSH_ARG:
		{
			uint16_t i = fetch_u16(&vm);

			if ((int)i >= vm.argc || vm.args == NULL)
			{
				vm_fail(&vm, "bad arg");
				break;
			}

			stack_push(&vm, vm.args[i]);
			break;
		}


		case OP_CENTER_X:
			stge_center_x(stack_pop(&vm));
			stack_push(&vm, 0);
			break;

		case OP_CENTER_Y:
			stge_center_y(stack_pop(&vm));
			stack_push(&vm, 0);
			break;

		case OP_CENTER_XY:
			stge_center_xy(stack_pop(&vm));
			stack_push(&vm, 0);
			break;

		case OP_ADD_CHILD:
		{
			int32_t child = stack_pop(&vm);
			int32_t parent = stack_pop(&vm);

			stge_add_child(parent, child);
			stack_push(&vm, 0);
			break;
		}

		case OP_GET_WIDTH:
			stack_push(&vm, stge_width());
			break;

		case OP_GET_HEIGHT:
			stack_push(&vm, stge_height());
			break;

		case OP_GET_CAM:
			stack_push(&vm, stge_cam_get((uint32_t)stack_pop(&vm)));
			break;

		case OP_SET_CAM:
		{
			int32_t value = stack_pop(&vm);
			int32_t idx = stack_pop(&vm);

			stge_cam_set((uint32_t)idx, value);
			stack_push(&vm, 0);
			break;
		}

		case OP_CAM_CENTER_ON:
		{
			int32_t y = stack_pop(&vm);
			int32_t x = stack_pop(&vm);

			stge_cam_center_on(x, y);
			stack_push(&vm, 0);
			break;
		}

		case OP_GET_TICKRATE:
			stack_push(&vm, stge_tickrate());
			break;

		case OP_SET_TICKRATE:
			stge_set_tickrate(stack_pop(&vm));
			stack_push(&vm, 0);
			break;

		case OP_NEW_INT:
		case OP_NEW_UINT:
		case OP_NEW_STR:
		case OP_NEW_BYTE:
		case OP_NEW_OBJ:
		case OP_NEW_ARRAY:
		{
			int32_t n = stack_pop(&vm);
			int32_t name = stack_pop(&vm);
			int32_t id = stack_pop(&vm);
			MemberKind kind = MEMBER_INT;

			if (op == OP_NEW_UINT)
				kind = MEMBER_UINT;
			else if (op == OP_NEW_STR)
				kind = MEMBER_STRING;
			else if (op == OP_NEW_BYTE)
				kind = MEMBER_BYTE;
			else if (op == OP_NEW_OBJ)
				kind = MEMBER_OBJECT;
			else if (op == OP_NEW_ARRAY)
				kind = MEMBER_ARRAY;

			if (n < 0)
				n = 0;

			stge_mem_new(id, stge_str(name), kind, (uint32_t)n);
			stack_push(&vm, 0);
			break;
		}

		case OP_GET_MEM:
		{
			int32_t idx = stack_pop(&vm);
			int32_t name = stack_pop(&vm);
			int32_t id = stack_pop(&vm);
			const char *key = stge_str(name);

			require_member(id, key);
			mem_bounds_or_crash(id, key, idx);
			stack_push(&vm, stge_mem_get(id, key, (uint32_t)idx));
			break;
		}

		case OP_SET_MEM:
		{
			int32_t value = stack_pop(&vm);
			int32_t idx = stack_pop(&vm);
			int32_t name = stack_pop(&vm);
			int32_t id = stack_pop(&vm);
			const char *key = stge_str(name);

			mem_bounds_or_crash(id, key, idx);
			stge_mem_set(id, key, (uint32_t)idx, value);
			stack_push(&vm, 0);
			break;
		}

		case OP_SET_OBJ:
		{
			int32_t value = stack_pop(&vm);
			int32_t idx = stack_pop(&vm);
			int32_t name = stack_pop(&vm);
			int32_t id = stack_pop(&vm);
			const char *key = stge_str(name);
			VmProg *s = stge_prog();
			const Member *m;

			if (s != NULL && stge_obj(id) != NULL)
			{
				m = obj_member_get(stge_obj(id), key);
				if (m != NULL && m->kind == MEMBER_ARRAY)
					stge_mem_set(id, key, (uint32_t)(idx < 0 ? 0 : idx), value);
				else
					stge_obj_set(id, key, value);
			}
			stack_push(&vm, 0);
			break;
		}

		case OP_GET_MEM_STR:
		{
			int32_t idx = stack_pop_i(&vm);
			int32_t name = stack_pop_i(&vm);
			int32_t id = stack_pop_i(&vm);
			VmProg *s = stge_prog();
			const char *text = "";

			if (s != NULL && stge_obj(id) != NULL)
			{
				const char *key = stge_str(name);

				require_member(id, key);
				{
					const char *got = obj_mem_str_i(stge_obj(id), key, (uint32_t)idx);

					if (got != NULL)
						text = got;
				}
			}

			stack_push(&vm, intern_or_fail(&vm, text));
			break;
		}

		case OP_SET_MEM_STR:
		{
			int32_t value = stack_pop_i(&vm);
			int32_t idx = stack_pop_i(&vm);
			int32_t name = stack_pop_i(&vm);
			int32_t id = stack_pop_i(&vm);
			VmProg *s = stge_prog();

			if (s != NULL && id >= 0 && id < s->object_count)
				obj_mem_set_str_i(stge_obj(id), stge_str(name), (uint32_t)idx, stge_str(value));

			stack_push(&vm, 0);
			break;
		}

		case OP_SET_DISPLAY:
		{
			int32_t str = stack_pop(&vm);
			int32_t id = stack_pop(&vm);

			stge_set_display(id, stge_str(str));
			stack_push(&vm, 0);
			break;
		}

		case OP_CALL:
		case OP_CALL_NS:
		{
			int32_t fn_pc = stack_pop_i(&vm);
			int32_t id = stack_pop_i(&vm);
			uint16_t argc = fetch_u16(&vm);
			int64_t *call_args = NULL;
			int i;

			if (argc > 0)
			{
				call_args = malloc((size_t)argc * sizeof *call_args);
				if (call_args == NULL)
				{
					vm_fail(&vm, "out of memory");
					break;
				}
				for (i = (int)argc - 1; i >= 0; i--)
					call_args[i] = stack_pop(&vm);
			}

			stack_push(&vm, (int32_t)vm_call(fn_pc, id, call_args, (int)argc));
			free(call_args);
			break;
		}

		case OP_RAND:
			stack_push(&vm, stge_rand(stack_pop_i(&vm)));
			break;

		case OP_NEW_FLOAT:
		case OP_NEW_DOUBLE:
		{
			int32_t n = stack_pop_i(&vm);
			int32_t name = stack_pop_i(&vm);
			int32_t id = stack_pop_i(&vm);
			MemberKind kind = op == OP_NEW_FLOAT ? MEMBER_FLOAT : MEMBER_DOUBLE;

			if (n < 0)
				n = 0;

			stge_mem_new(id, stge_str(name), kind, (uint32_t)n);
			stack_push(&vm, 0);
			break;
		}

		case OP_PUSH_F:
			stack_push(&vm, (int64_t)fetch_u32(&vm));
			break;

		case OP_PUSH_D:
			stack_push(&vm, fetch_i64(&vm));
			break;

		case OP_GET_MEM_F:
		{
			int32_t idx = stack_pop_i(&vm);
			int32_t name = stack_pop_i(&vm);
			int32_t id = stack_pop_i(&vm);

			stack_push(&vm, fbits(stge_mem_get_f(id, stge_str(name), (uint32_t)idx)));
			break;
		}

		case OP_SET_MEM_F:
		{
			float value = ffrom(stack_pop(&vm));
			int32_t idx = stack_pop_i(&vm);
			int32_t name = stack_pop_i(&vm);
			int32_t id = stack_pop_i(&vm);

			stge_mem_set_f(id, stge_str(name), (uint32_t)idx, value);
			stack_push(&vm, 0);
			break;
		}

		case OP_GET_MEM_D:
		{
			int32_t idx = stack_pop_i(&vm);
			int32_t name = stack_pop_i(&vm);
			int32_t id = stack_pop_i(&vm);

			stack_push(&vm, dbits(stge_mem_get_d(id, stge_str(name), (uint32_t)idx)));
			break;
		}

		case OP_SET_MEM_D:
		{
			double value = dfrom(stack_pop(&vm));
			int32_t idx = stack_pop_i(&vm);
			int32_t name = stack_pop_i(&vm);
			int32_t id = stack_pop_i(&vm);

			stge_mem_set_d(id, stge_str(name), (uint32_t)idx, value);
			stack_push(&vm, 0);
			break;
		}

		case OP_ADD_F:
		{
			float b = ffrom(stack_pop(&vm));
			float a = ffrom(stack_pop(&vm));

			stack_push(&vm, fbits(a + b));
			break;
		}

		case OP_SUB_F:
		{
			float b = ffrom(stack_pop(&vm));
			float a = ffrom(stack_pop(&vm));

			stack_push(&vm, fbits(a - b));
			break;
		}

		case OP_EQ_F:
		{
			float b = ffrom(stack_pop(&vm));
			float a = ffrom(stack_pop(&vm));

			stack_push(&vm, a == b ? 1 : 0);
			break;
		}

		case OP_LT_F:
		{
			float b = ffrom(stack_pop(&vm));
			float a = ffrom(stack_pop(&vm));

			stack_push(&vm, a < b ? 1 : 0);
			break;
		}

		case OP_GT_F:
		{
			float b = ffrom(stack_pop(&vm));
			float a = ffrom(stack_pop(&vm));

			stack_push(&vm, a > b ? 1 : 0);
			break;
		}

		case OP_ADD_D:
		{
			double b = dfrom(stack_pop(&vm));
			double a = dfrom(stack_pop(&vm));

			stack_push(&vm, dbits(a + b));
			break;
		}

		case OP_SUB_D:
		{
			double b = dfrom(stack_pop(&vm));
			double a = dfrom(stack_pop(&vm));

			stack_push(&vm, dbits(a - b));
			break;
		}

		case OP_EQ_D:
		{
			double b = dfrom(stack_pop(&vm));
			double a = dfrom(stack_pop(&vm));

			stack_push(&vm, a == b ? 1 : 0);
			break;
		}

		case OP_LT_D:
		{
			double b = dfrom(stack_pop(&vm));
			double a = dfrom(stack_pop(&vm));

			stack_push(&vm, a < b ? 1 : 0);
			break;
		}

		case OP_GT_D:
		{
			double b = dfrom(stack_pop(&vm));
			double a = dfrom(stack_pop(&vm));

			stack_push(&vm, a > b ? 1 : 0);
			break;
		}

		case OP_I2F:
			stack_push(&vm, fbits((float)stack_pop_i(&vm)));
			break;

		case OP_I2D:
			stack_push(&vm, dbits((double)stack_pop_i(&vm)));
			break;

		case OP_F2D:
			stack_push(&vm, dbits((double)ffrom(stack_pop(&vm))));
			break;

		case OP_F2I:
			stack_push(&vm, (int64_t)(int32_t)ffrom(stack_pop(&vm)));
			break;

		case OP_D2I:
			stack_push(&vm, (int64_t)(int32_t)dfrom(stack_pop(&vm)));
			break;

		case OP_D2F:
			stack_push(&vm, fbits((float)dfrom(stack_pop(&vm))));
			break;

		case OP_STR_F:
		{
			char buf[32];

			snprintf(buf, sizeof buf, "%g", (double)ffrom(stack_pop(&vm)));
			stack_push(&vm, intern_num(&vm, buf));
			break;
		}

		case OP_STR_D:
		{
			char buf[32];

			snprintf(buf, sizeof buf, "%g", dfrom(stack_pop(&vm)));
			stack_push(&vm, intern_num(&vm, buf));
			break;
		}

		case OP_STR_OBJ:
		{
			int32_t id = stack_pop_i(&vm);
			int pc = obj_str_pc(id);

			if (pc >= 0)
				stack_push(&vm, vm_call(pc, id, NULL, 0));
			else
				stack_push(&vm, default_obj_str(&vm, id));
			break;
		}

		case OP_STR_MEM:
		{
			int32_t name = stack_pop_i(&vm);
			int32_t id = stack_pop_i(&vm);
			VmProg *s = stge_prog();
			const Member *m;
			char buf[64];

			if (s == NULL || id < 0 || id >= s->object_count)
			{
				stack_push(&vm, intern_or_fail(&vm, ""));
				break;
			}

			m = obj_member_get(stge_obj(id), stge_str(name));
			if (m == NULL || m->data == NULL || m->len == 0)
			{
				stack_push(&vm, intern_or_fail(&vm, ""));
				break;
			}

			if (m->kind == MEMBER_STRING)
			{
				const char *str = obj_mem_str(stge_obj(id), stge_str(name));

				stack_push(&vm, intern_or_fail(&vm, str != NULL ? str : ""));
				break;
			}

			if (m->kind == MEMBER_FLOAT && m->len >= 3)
				snprintf(buf, sizeof buf, "%g, %g, %g",
					 (double)((const float *)m->data)[0],
					 (double)((const float *)m->data)[1],
					 (double)((const float *)m->data)[2]);
			else if (m->kind == MEMBER_FLOAT && m->len >= 2)
				snprintf(buf, sizeof buf, "%g, %g",
					 (double)((const float *)m->data)[0],
					 (double)((const float *)m->data)[1]);
			else if (m->kind == MEMBER_FLOAT)
				snprintf(buf, sizeof buf, "%g", (double)((const float *)m->data)[0]);
			else if (m->kind == MEMBER_DOUBLE)
				snprintf(buf, sizeof buf, "%g", ((const double *)m->data)[0]);
			else if (m->len >= 3)
				snprintf(buf, sizeof buf, "%d, %d, %d",
					 obj_mem_get_i(stge_obj(id), stge_str(name), 0),
					 obj_mem_get_i(stge_obj(id), stge_str(name), 1),
					 obj_mem_get_i(stge_obj(id), stge_str(name), 2));
			else if (m->len >= 2)
				snprintf(buf, sizeof buf, "%d, %d",
					 obj_mem_get_i(stge_obj(id), stge_str(name), 0),
					 obj_mem_get_i(stge_obj(id), stge_str(name), 1));
			else
				snprintf(buf, sizeof buf, "%d", obj_mem_get_i(stge_obj(id), stge_str(name), 0));

			stack_push(&vm, intern_or_fail(&vm, buf));
			break;
		}

		case OP_PRINT:
		{
			int64_t value = stack_pop(&vm);

			stge_print(STGE_PRINT_STR, value);
			stack_push(&vm, 0);
			break;
		}

		case OP_GET_PRINT_OUT:
		{
			int idx = stge_intern(stge_print_out());

			if (idx < 0)
			{
				vm_fail(&vm, "print_out: intern failed");
				break;
			}

			stack_push(&vm, idx);
			break;
		}

		case OP_SET_PRINT_OUT:
			stge_set_print_out(stge_str(stack_pop_i(&vm)));
			stack_push(&vm, 0);
			break;

		case OP_NEW_DYN:
		{
			int32_t name = stack_pop_i(&vm);
			int32_t id = stack_pop_i(&vm);
			MemberKind kind = (MemberKind)fetch_u8(&vm);

			stge_mem_new_dyn(id, stge_str(name), kind);
			stack_push(&vm, 0);
			break;
		}

		case OP_DYN_ADD:
		{
			int64_t value = stack_pop(&vm);
			int32_t name = stack_pop_i(&vm);
			int32_t id = stack_pop_i(&vm);

			stge_dyn_add(id, stge_str(name), value);
			stack_push(&vm, 0);
			break;
		}

		case OP_GET_LEN:
		{
			int32_t name = stack_pop_i(&vm);
			int32_t id = stack_pop_i(&vm);
			const char *key = stge_str(name);

			require_member(id, key);
			stack_push(&vm, (int64_t)stge_mem_len(id, key));
			break;
		}

		case OP_CALL_NEW:
		{
			int32_t fn_pc = stack_pop_i(&vm);
			int32_t type_id = stack_pop_i(&vm);
			uint16_t argc = fetch_u16(&vm);
			int64_t *call_args = NULL;
			int i;
			int nid;

			if (argc > 0)
			{
				call_args = malloc((size_t)argc * sizeof *call_args);
				if (call_args == NULL)
				{
					vm_fail(&vm, "out of memory");
					break;
				}
				for (i = (int)argc - 1; i >= 0; i--)
					call_args[i] = stack_pop(&vm);
			}

			nid = stge_spawn_map("");

			if (nid >= 0)
			{
				int tindex = stge_is_type_ref(type_id) ? stge_type_index(type_id)
					: type_id;

				if (stge_type(tindex) != NULL)
					stge_init_packed(nid, tindex);
			}

			(void)vm_call(fn_pc, nid, call_args, (int)argc);
			free(call_args);
			stack_push(&vm, nid);
			break;
		}

		case OP_EXTERN:
		{
			int native = fetch_u8(&vm);
			uint16_t argc = fetch_u16(&vm);
			int32_t *nargs = NULL;
			int i;
			int32_t result;

			if (argc > 0)
			{
				nargs = malloc((size_t)argc * sizeof *nargs);
				if (nargs == NULL)
				{
					vm_fail(&vm, "out of memory");
					break;
				}
				for (i = (int)argc - 1; i >= 0; i--)
					nargs[i] = stack_pop_i(&vm);
			}

			result = stge_call_native(native, nargs, (int)argc);
			free(nargs);
			stack_push(&vm, result);
			break;
		}

		case OP_STRING_WRAP:
			stack_push(&vm, stge_string_wrap(stack_pop_i(&vm)));
			break;

		case OP_GET_BUF:
		{
			int32_t name = stack_pop_i(&vm);
			int32_t id = stack_pop_i(&vm);
			const char *key = stge_str(name);
			const void *data;

			require_member(id, key);
			data = stge_mem_data(id, key);
			stack_push(&vm, (int64_t)(uintptr_t)data);
			break;
		}

		case OP_DYN_COPY:
		{
			int32_t src_name = stack_pop_i(&vm);
			int32_t src_id = stack_pop_i(&vm);
			int32_t dst_name = stack_pop_i(&vm);
			int32_t dst_id = stack_pop_i(&vm);

			stge_dyn_copy(dst_id, stge_str(dst_name), src_id, stge_str(src_name));
			stack_push(&vm, 0);
			break;
		}

		case OP_WRITE:
		{
			uint32_t count = (uint32_t)stack_pop(&vm);
			int64_t buf = stack_pop(&vm);
			int fd = (int)stack_pop(&vm);

			stge_write(fd, (const void *)(uintptr_t)buf, count);
			stack_push(&vm, 0);
			break;
		}

		case OP_GET_LOCAL:
		{
			uint16_t i = fetch_u16(&vm);

			if (!locals_grow(&vm, (int)i + 1))
			{
				vm_fail(&vm, "out of memory");
				break;
			}

			stack_push(&vm, script_locals[i]);
			break;
		}

		case OP_SET_LOCAL:
		{
			uint16_t i = fetch_u16(&vm);
			int64_t v = stack_pop(&vm);

			if (!locals_grow(&vm, (int)i + 1))
			{
				vm_fail(&vm, "out of memory");
				break;
			}

			script_locals[i] = v;
			stack_push(&vm, 0);
			break;
		}

		case OP_MEM_COPY:
		{
			int32_t src_name = stack_pop_i(&vm);
			int32_t src_id = stack_pop_i(&vm);
			int32_t dst_name = stack_pop_i(&vm);
			int32_t dst_id = stack_pop_i(&vm);

			stge_mem_copy(dst_id, stge_str(dst_name), src_id, stge_str(src_name));
			stack_push(&vm, dst_id);
			break;
		}

		case OP_GET_SCREEN_SIZE:
			stack_push(&vm, ((int64_t)(uint32_t)stge_height() << 32)
					| (uint32_t)stge_width());
			break;

		case OP_GET_SCREEN_BUF:
			stack_push(&vm, (int64_t)(uintptr_t)screen_buf(&stge.screen));
			break;

		case OP_POLL:
			stge_poll_input();
			break;

		case OP_RENDER:
			stge_present_frame();
			break;

		case OP_WAIT_TICK:
			if (stge.quit)
			{
				vm.halted = true;
				break;
			}

			stge_wait_tick();
			if (stge.quit)
				vm.halted = true;
			break;

		default:
			vm_fail(&vm, "unknown opcode");
			break;
		}
	}

	free(vm.stack);
	free(vm.args);
	return ret;
}
