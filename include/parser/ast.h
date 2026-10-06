#ifndef STGE_PARSER_AST_H
#define STGE_PARSER_AST_H

#include "core/stge.h"

/* Compile-time type/function records. A function value is a pointer (pc)
   into the static function table, not a copy of the body. */

typedef struct
{
	char *path;
} AstUse;

typedef struct
{
	char *type;
} AstFieldType;

#endif /* STGE_PARSER_AST_H */
