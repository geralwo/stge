# STGE

STGE is a small scripting language for terminal games. A game is a directory whose `main.stge` is the entry point. The compiler reads `.stge` files, emits bytecode, and the virtual machine runs that bytecode.

```
stge -game <dir>          # compile main.stge and run
stge -game <dir> -check   # compile only
```

Every value is an object. Source is a sequence of statements separated by newlines. `//` comments run to the end of the line. There are no semicolons and no block comments.

## Syntax

```
(* lexical *)
NAME       ::= letter { letter | digit | '_' }
INT        ::= digit { digit }
FLOAT      ::= digit { digit } '.' digit { digit }
CHAR       ::= "'" char "'"
STRING     ::= '"' { stringChar | escape | hole } '"'
escape     ::= '\' ( 'n' | 't' | 'r' | '\' | '"' | "'" | '$' | 'e' | 'x' hex hex )
hole       ::= '${' expr '}'
COMMENT    ::= '//' { char } EOL

(* file *)
file       ::= { useDecl | topStmt }

useDecl    ::= 'use' STRING EOL

topStmt    ::= typeBind
             | packedDecl
             | enumDecl
             | memberDef
             | slotDecl
             | stmt

typeBind   ::= qname '=' ( '{}' | '{' { stmt } '}' )

packedDecl ::= NAME '=' packedLayout [ componentBlock ]
packedLayout ::= '[' INT ']' | NAME '[' INT ']'
componentBlock ::= '{' { componentLine } '}'
componentLine ::= NAME '.' NAME '=' NAME '[' INT ']' EOL

enumDecl   ::= qname '=' 'enum' '{' { enumItem [ ',' ] } '}'
enumItem   ::= NAME [ '=' [ '-' ] INT ]

memberDef  ::= qname ( '.' | '::' ) NAME '=' ( funcDef | externDef | expr )
funcDef    ::= '(' [ param { ',' param } ] ')' [ '->' typeRef ] block
externDef  ::= 'extern' NAME '(' [ param { ',' param } ] ')' [ '->' typeRef ]
param      ::= [ typeRef ] NAME
typeRef    ::= qname [ '[' [ INT | expr ] ']' ]

slotDecl   ::= [ typeRef ] NAME '=' expr

stmt       ::= block
             | 'if' expr block [ 'else' ( 'if' expr block | block ) ]
             | 'while' expr block
             | 'return' [ expr ]
             | assign
             | expr

block      ::= '{' { stmt } '}'

assign     ::= lvalue ( '=' | '+=' | '-=' | '*=' | '/=' ) expr
lvalue     ::= path [ '[' expr ']' ]

(* expressions, tightest to loosest *)
expr       ::= orExpr
orExpr     ::= andExpr { 'or' andExpr }
andExpr    ::= cmpExpr { 'and' cmpExpr }
cmpExpr    ::= sumExpr [ ( '==' | '<' | '>' ) sumExpr ]
sumExpr    ::= unary { ( '+' | '-' ) unary }
unary      ::= ( '+' | '-' | 'not' ) unary | postfix
postfix    ::= primary { '.' NAME | '::' NAME | '(' [ expr { ',' expr } ] ')' | '[' expr ']' }
primary    ::= INT | FLOAT | CHAR | STRING | NAME | 'self' | '{}'
             | '(' expr ')'
             | 'color' '(' STRING ')'

qname      ::= NAME { '::' NAME }
path       ::= qname { '.' NAME }
```

`CHAR` is one raw character (no escape decoding). A `use` `STRING` may not contain `${`. There is no binary `*` / `/` / `!=` / `<=` / `>=` in `expr`.

## How a program runs

Compiling a file is two passes over the same source. The first pass records types, functions, and imports. The second pass emits code. A function can be called earlier in the file than it is written.

Top-level statements in a file become bytecode. When that bytecode runs is decided by the host program (the C runtime), not by STGE grammar.

`src/lang/core/core.stge` is included automatically unless the file already defines `int`, `String`, or `std`. `use` paths are resolved relative to the file that contains the `use`. A given path is processed at most once per compile pass (`use` is `#pragma once`-style: a second `use` of the same path is a no-op).

## Names

A name is letters, digits, and `_` (see Syntax).


| Form              | Meaning                                          |
| ----------------- | ------------------------------------------------ |
| `Name.field`      | Instance member or instance method.              |
| `Name::field`     | Static member or static function on the type.    |
| `Name::Inner.RED` | A nested static value, such as an enum constant. |
| `Name(args)`      | Call a function, or construct an object.         |


`()` looks the path up. If the path is a function, it runs. If the path is a type, it constructs: `Sprite("@")` runs the `Sprite::new` whose parameter count matches.

Inside a method, the bare type name is the receiver when it appears as the **first** parameter written as only the type name — e.g. `(test, int a)` or `(int)`. In `player.move` with no such param, the name `player` still means the instance (via `self`). The name `self` is still accepted and means the same receiver.

`Name::` is the type, not an instance. `Type::new(...)` as a call is rejected. Construct with `Type(...)`.

## Types and objects

Heap types and packed (stack) types are different.

### Heap types

```
Sprite = {}
Sprite::new = (String s) -> Sprite {}
Game = {
  Game.player = Sprite("@")
  Game.greet = () {
    Terminal::print("hello\n")
  }
}

game = Game()
```

`Name = {}` **defines** a heap type (methods and members on the type). It does **not** allocate an instance. A brace list after `=` is the same definition with members written inline. Each line inside is an ordinary statement, almost always `Name.member = ...`. A top-level `Name = {}` (and packed / enum / extern) may be declared only once; later lines change it with `Name.member = ...` or `Name::...`, not another `Name =`.

`Name(...)` **allocates** one instance of that type on the growable heap and runs the matching constructor / `new`. Bind the result: `player = Sprite("@")`.

`{}` used as a value allocates an anonymous heap instance.

The host only runs top-level init bytecode. Any live instance must be constructed explicitly in that code (for example `game = Game()`).

### Packed types (stack values)

```
byte = [1]
int = byte[4]
uint = byte[4]
```

`Name = [n]` and `Name = Other[n]` register a **packed layout**. Values of that type are stack/inline storage (fixed bytes), not heap objects. `n` is at least 1. `Other` must already be a packed type (or a primitive layout such as `byte`).

Members on heap objects are named arrays. A scalar is an array of length 1. Reading `obj.field` reads index 0. `obj.field[i]` reads index `i`.

### Packed type grammar

Top-level only. One declaration introduces the type name; it is not an expression. Form is in Syntax (`packedDecl`). Each `componentLine` ends at the newline; commas are not used inside the block.

**Layout.** `Name = [n]` gives `Name` a single packed member (the type’s implicit storage) of length `n`. `Name = Other[n]` gives `Name` the same element layout as `Other`, with total length `n` (for example `int = byte[4]`). A value of a packed type is a **stack value**: fixed inline storage for that layout, not a heap map of separate fields.

**Component block (optional).** After `Name = Other[n]`, a `{ ... }` block may list **component aliases**. Each line has the form:

```
Name.component = Name[i]
```

- The `Name` on both sides must be the type being declared.
- `i` must satisfy `0 <= i < n`.
- Each `component` name may appear at most once; each index `i` may be bound at most once.
- The block is **compile-time metadata only**. It does not run at init and does not copy values. It tells the compiler that `value.component` and `value[i]` are the same storage.

Example (from `vec.stge`):

```
vec2i = int[2] {
  vec2i.x = vec2i[0]
  vec2i.y = vec2i[1]
}
```

After this, `v.x` and `v[0]` are equivalent for any `v` of type `vec2i`, and likewise `v.y` / `v[1]`.

**Not the same as** `Name = {}`**.** A brace list on a **heap** type (`Sprite = { ... }`) holds ordinary statements, usually default member stores on instances. A brace list on a **packed** type only declares index aliases; the only allowed lines are `Name.component = Name[i]`.

Built-in scalar constructors, used when storing a member:

```
obj.n = int(15)
obj.n = uint(1)
obj.b = byte(64)
obj.f = float(1.5)
obj.d = double(1.5)
obj.s = str("hi")
obj.p = vec2(1.0, 2.0)
obj.p = vec3i(1, 2, 3)
```

`int(n)`, `uint(n)`, and `byte(n)` used as expressions wrap that value and tag its type. `object` is the type that accepts any value.

## Vectors

`vec2i` and `vec3i` are integer vectors. `vec2` and `vec3` are float vectors. Components are `x`, `y`, and `z`.

Vectors are packed types with component aliases, not separate heap fields:

```
vec2i = int[2] {
  vec2i.x = vec2i[0]
  vec2i.y = vec2i[1]
}
```

Game code uses the short form:

```
pos.x
pos.y
pos[0]
```

`src/lang/core/vec.stge` defines `vec2i` this way. It is not pulled in by `core.stge`, so a game that uses it `use`s that file (or a file that does). `vec2i()` and `vec2i(x, y)` are the constructors. `+=`, `-=`, `*=`, and `/=` on a `vec2i` call `vec2i::+=` and the matching operators, which update both components. `vec2i::to_str` formats a vector as `"x, y"`.

The same pattern applies to other vector sizes when they are added (`vec3i = int[3] { ... }`, and float layouts over `float[n]`).

## Functions

A function is a member of a type.

```
Sprite.draw = () {
  Terminal::print(Sprite.display)
}

int::to_str = extern stge_int_to_str(int n) -> String

vec2i::new = (int x, int y) -> vec2i {
  vec2i.x = x
  vec2i.y = y
}
```

The parameter list is `(Type name, Type name)` or, untyped, `(name, name)`. Untyped parameters are not checked. The first typed parameter is also the receiver name for field lookup inside the body.

`-> Type` is the return type. Leave `->` off when the function returns nothing. `-> Type[n]` and `-> Type[]` are accepted as array return types (`[]` means unknown length; see Arrays).

`return` leaves the function. `return expr` returns that value. A bare `return`, and falling off the end, returns `0`.

The same function name can be defined more than once when the parameter counts differ. `int::new` has both an `int` and a `uint` overload. A call must match one arity.

`extern` binds a static function to a C native. It is only legal in files under `src/lang/`. The native is named by its C symbol; arity is the parameter list (same as a normal call):

```
std::write = extern stge_write(int fd, byte[] buf, uint count)
```



## Slots

Inside a function, a bare assignment creates a slot. A slot is a name that lives for that function call. It is not a member of the receiver.

```
i = int(0)
p = Terminal::Screen::Pixel()
String s = obj.to_str()
```

`name = expr` stores the value in a new slot. `Type name = expr` does the same and checks the right-hand side. `object name = expr` accepts any type. Any other type must match the value's type.

Later lines in the same function read and write that slot, including `+=`, `-=`, `*=`, and `/=`. Parameters occupy slots of their own.

At file scope, `name = expr` (when the right-hand side is not `{}`, an enum, `extern`, or a packed layout) is a slot in the script's init code. A value that should stay on an object is written `object.field = expr`.

## Statements

A statement ends at a newline. Blank lines are ignored. A `{ ... }` block is itself a statement, and the statements inside it are also newline-separated.

```
if Input.is_key_pressed(KEY_UP) == 1 {
  player.vel += vec2i(0, -1)
}
else if Input.is_key_pressed(KEY_DOWN) == 1 {
  player.vel += vec2i(0, 1)
}
else {
  player.vel = vec2i(0, 0)
}

while i < s.length {
  i += 1
}
```

`if` and `while` take an integer. `0` is false. Anything else is true. `else` may sit on the following line. `else if` is an `else` whose body is another `if`.

Assignment:

```
obj.field = expr
obj.field[i] = expr
obj.field += expr
name = expr
name.x += 1
```

`+=`, `-=`, `*=`, and `/=` read the current value, apply the operator, and store it back. On `int`, `uint`, and `byte` this is integer arithmetic. On `vec2i` it calls the type's operator.

A call used as a statement runs and drops its result:

```
Terminal::print("hello\n")
player.move()
```



## Expressions

From tightest to loosest:

1. Values, calls, member access, indexing, parentheses, unary `+`, `-`, and `not`.
2. `+` and `-`.
3. `==`, `<`, `>`.
4. `and`, `or`.

```
1 + 2
-n
not done
a == b
i < s.length
key == 1 and index > 0
while not quit { ... }
```

`and` and `or` always evaluate both sides, then combine them as integers. `not` flips integer truthiness (`0` → `1`, anything else → `0`).

`+` on a `String` concatenates. The right-hand side is turned into a `String` first.

There is no `*`, `/`, `!=`, `<=`, or `>=` in expressions. Multiplication and division exist only as `*=` and `/=`.

Numbers are decimal. `15` is an int. `1.5` is a float when it fits in a float, otherwise a double. `'A'` is a byte, the character's code, and the quotes must hold exactly one character. Single-quoted char literals are raw: no escape decoding.

A double-quoted literal is a `String`:

```
"hello\n"
"\e[31m"
"\x1b[0m"
"${name} ${n}\n"
```

Escapes in double-quoted strings are `\n`, `\t`, `\r`, `\\`, `\"`, `\'`, `\$`, `\e` (ESC, `0x1B`), and `\xHH` (exactly two hex digits). `${expr}` is replaced by that expression converted to a `String`. Holes can nest, up to 8.

`color("red")` pushes a color constant. Names match the `COLOR_*` values (`red`, `lightblue`, `default`, and the rest).

Comparing a byte with a one-character string compares the character code: `buf[i] == "\n"`.

## Arrays

```
obj.cells = int[8]
obj.buf = byte[]
Pixel[n]
```

`T[n]` is a fixed array of length `n`. The length can be an expression. `T[]` is an array of unknown length (growable at run time). `[*]` is not valid syntax. Known element types are `byte`, `int`, `uint`, `float`, `double`, and `str`. Any other name is an array of object ids.

```
obj.buf[i]
obj.buf.len
obj.buf.append(byte(65))
obj.buf.add(byte(65))
```

`append` and `add` push one value onto a growable array. `String.buffer` is a growable `byte` array, so `s.buffer.append(ch)` and `s.buffer[i]` are the usual way to read and write string bytes. `s.length` is the string's length; appending to `buffer` does not by itself change `length`.

## Enums

```
Terminal::Color = enum {
  DEFAULT = -1
  BLACK
  RED
  GREEN
}
```

The first constant is `0` unless a line says `NAME = integer`. Later names count up from the previous value. A leading minus is allowed (`-1`). A trailing comma is allowed.

Read a constant with a dot: `Terminal::Color.RED`. The value is an int, and its type tag is the enum, so a parameter typed `Terminal::Color` accepts it.

## Imports

```
use "scripts/sprite.stge"
use "vec.stge"
```

`use` takes a plain string. Interpolation is not allowed. Absolute paths are used as written. Relative paths are tried next to the current file, then as given. The same resolved path is only loaded once in a pass; repeating `use` for it is ignored (like `#pragma once`).

## Strings and printing

`String` is a heap object:

```
String = {
  String.length = uint(0)
  String.buffer = byte[]
}
```

`String::concat(a, b)` joins two strings. `"a" + b` uses it. `value.to_str()` and `Type::to_str(value)` produce a `String`. `int`, `uint`, `byte`, and `vec2i` each define `to_str`. `Terminal::print` writes the string's bytes to stdout.

```
Terminal::print("hello\n")
Terminal::print(int::to_str(n))
Terminal::printc(obj, Terminal::Color.RED, Terminal::Color.BLACK)
Terminal::print_at(0, 0, "hi")
```

`printc` and `print_at` are plain script: they emit ANSI CSI (SGR for color, CUP for cursor). Coordinates for `print_at` are 0-based. Use `"\e[..."` or `"\x1b[..."` for ESC.

`Terminal::stdin`, `Terminal::stdout`, and `Terminal::stderr` are `0`, `1`, and `2`. `std::write(fd, buf, count)` is the raw write.

## What the standard library defines

Loaded through `core.stge`:


| File            | Defines                                                       |
| --------------- | ------------------------------------------------------------- |
| `number.stge`   | `byte`, `int`, `uint`, `int64`, `uint64` (ctors + `to_str`)   |
| `object.stge`   | `object`, `object.type`, `object.id`                          |
| `input.stge`    | `Input`, `Input::poll`, `Input::is_key_pressed`, `Input::Key` |
| `std.stge`      | `std::write`                                                  |
| `string.stge`   | `String`, `String::concat`, `to_str`                          |
| `terminal.stge` | `Terminal`, `Terminal::Color`, `print`, `printc`, `print_at`  |
| `vec.stge`      | `vec2i` packed type and operators                             |


Input lives in `input.stge` only. Bind `Input.keys` from the host after load.

A parameter that is **only the type name** (and must be first) is the receiver / self. Other parameters come from the call parentheses:

```
test = {}
test.to_abc = (test, int a) -> String {
  return "abc${int::to_str(a)}"
}

int.to_str = (int) -> String {
  return int::to_str(int)
}
```

`obj.to_abc(5)` binds `test = obj` and `a = 5`. `1337.to_str()` and `n.to_str()` are the same for an int-typed value (a numeric literal just needs the `.` after the number token).

## Limits

Tables that used to be fixed-size (namespaces, packed types, natives, string
interns, `use` lists, call patches, field metadata, and so on) grow as needed.
Object and function names are heap/arena strings (no 32-character truncate).

`use` has no count limit: each path is recorded and skipped if seen again.
Include nesting via `load` / nested include is limited only by **cycle
detection** (re-entering a path already on the include stack fails). `${}`
interpolation depth and dot-chain field paths also grow with use.

