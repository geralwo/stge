#ifndef STGE_PARSER_AST_H
#define STGE_PARSER_AST_H

#include "core/stge.h"

/* Compile-time type/function records. A function value is a pointer (pc)
   into the static function table, not a copy of the body. */

#define STGE_MAX_USE 16

typedef struct
{
	char *path;
} AstUse;

typedef struct
{
	char type[STGE_NAME_MAX];
} AstFieldType;

#endif /* STGE_PARSER_AST_H */
