#ifndef STGE_PARSER_PARSER_H
#define STGE_PARSER_PARSER_H

#include "core/stge.h"

/* Compiles a .stge script into the VM program image and runs its init.
   1-arg load("path") is a compile-time include / trait import.

   Meaning rules:
    Every value is an object. Heap Type: Name = {} / Name = { members }.
    Packed Type: Name = T[n] or Name = [n]. Members attach to the Type.
    :: is static/type, . is instance. () looks up the path: function runs,
    object constructs (Name(args) runs matching ::new). No self keyword.
    Inside Name.fn, bare Name is the receiver. Name:: is static.
    object accepts any value. Typed params and Type name = rhs must match.
    Bare assign in a function is a slot. Omit -> when the function returns
    nothing. "#" is a String. ${expr} inside "..." inserts expr as a String.
    'x' is a byte. if/while take int (0 is false).
   Parser tokenizes, parses grammar, and emits GET/SET/CALL/EXTERN. */
bool parser_compile(const char *path);
bool parser_load(const char *path);
bool parser_had_error(void);

#endif /* STGE_PARSER_PARSER_H */
