#ifndef STGE_UTILS_VEC_H
#define STGE_UTILS_VEC_H

#include <stdbool.h>

typedef struct
{
	int x;
	int y;
} Vec2i;

typedef struct
{
	int x;
	int y;
	int z;
} Vec3i;

typedef struct
{
	float x;
	float y;
} Vec2;

typedef struct
{
	float x;
	float y;
	float z;
} Vec3;

#define VEC2I_ZERO ((Vec2i){0, 0})
#define VEC2I_ONE ((Vec2i){1, 1})
#define VEC2I_UP ((Vec2i){0, -1})
#define VEC2I_DOWN ((Vec2i){0, 1})
#define VEC2I_LEFT ((Vec2i){-1, 0})
#define VEC2I_RIGHT ((Vec2i){1, 0})

#define VEC3I_ZERO ((Vec3i){0, 0, 0})
#define VEC3I_UP ((Vec3i){0, -1, 0})
#define VEC3I_DOWN ((Vec3i){0, 1, 0})
#define VEC3I_LEFT ((Vec3i){-1, 0, 0})
#define VEC3I_RIGHT ((Vec3i){1, 0, 0})

static inline Vec2i vec2i(int x, int y)
{
	return (Vec2i){x, y};
}

static inline Vec3i vec3i(int x, int y, int z)
{
	return (Vec3i){x, y, z};
}

static inline Vec2 vec2(float x, float y)
{
	return (Vec2){x, y};
}

static inline Vec3 vec3(float x, float y, float z)
{
	return (Vec3){x, y, z};
}

static inline Vec2i vec2i_add(Vec2i a, Vec2i b)
{
	return (Vec2i){a.x + b.x, a.y + b.y};
}

static inline Vec3i vec3i_add(Vec3i a, Vec3i b)
{
	return (Vec3i){a.x + b.x, a.y + b.y, a.z + b.z};
}

static inline bool vec2i_eq(Vec2i a, Vec2i b)
{
	return a.x == b.x && a.y == b.y;
}

static inline bool vec3i_eq(Vec3i a, Vec3i b)
{
	return a.x == b.x && a.y == b.y && a.z == b.z;
}

/* true when v lies inside the [0,0]..[size.x-1, size.y-1] cell grid */
static inline bool vec2i_in_bounds(Vec2i v, Vec2i size)
{
	return v.x >= 0 && v.y >= 0 && v.x < size.x && v.y < size.y;
}

#endif /* STGE_UTILS_VEC_H */
