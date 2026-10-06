# STGE (Scripting Game Engine)

A small scripting language for terminal games, compiled to bytecode and interpreted by a virtual machine.

## Overview

STGE is a game engine framework designed specifically for **terminal-based games**. It compiles `.stge` source files into lightweight bytecode that is executed by an in-process virtual machine (VM). The entire runtime system — compiler, VM, and built-in primitives — is written in C using only POSIX.10 standard libraries.

### Core Architecture

| Component | Description |
|-----------|-------------|
| **Compiler** | Parses `.stge` source, performs type checking, emits bytecode. Written in `src/parser/` and `src/core/`. |
| **Virtual Machine** | Executes compiled bytecode with heap, stack-based evaluation, function calls, and object model. Written in `src/core/`. |
| **Built-in Primitives** | Access to terminal I/O (`Terminal::print`, `Terminal::readline`, `Terminal::echo`), colors (`color("text color")`), and string/array operations. |
| **Host Runtime** | C program that loads the VM, compiles a game's `main.stge`, and runs it. See `src/main.c`. |

## Installation & Build

### Requirements

- **C17 compliant** GCC / Clang (≥ 9 for `-pedantic` support)
- A POSIX-compliant Unix-like system (Linux/macOS/BSD) — the C runtime is written for POSIX. The terminal primitives use standard `stdio.h`/`termios`.

### Build Commands

The project ships with a simple bash build script `compile.sh`:

```bash

# Build and run (compiles, links, executes)
./compile.sh run <game_directory> [extra_args]

# Build only — useful for checking syntax/type errors without executing
./compile.sh -game <dir> -check

# Add a source file to the build configuration
./compile.sh add src/some_file.c

# Remove a source file from the build configuration
./compile.sh remove src/old_file.c

# View current source list and compiler flags
./compile.sh sources
./compile.sh config

# Clean the build directory
./compile.sh clean
```

### Manual Build

If you prefer to compile manually:

```bash
mkdir -p build

gcc \
    -std=c17 \
    -Wall -Wextra -Werror -pedantic -O2 \
    -Iinclude \
    src/core/stge.c \
    src/core/native.c \
    src/core/obj.c \
    src/core/vm.c \
    src/core/input.c \
    src/core/screen.c \
    src/core/color.h \
    src/core/array.h \
    src/parser/parser.c \
    src/parser/ast.h \
    -o build/stge
```

### Running a Game

A **game** is simply a directory containing a `main.stge` entry file:

```bash
# Navigate into your game's directory and run it
cd my_game_dir && ./stge -game .

# Or from anywhere
./build/stge -game /path/to/my_game_dir "arg1" "arg2"
```

The host program (`stge`) loads `main.stge` from the specified game directory, compiles its bytecode, and starts execution. Any extra arguments are available as command-line parameters to the entry point.

## The STGE Language

### Basic Concepts

- **Every value is an object.** All values (integers, strings, vectors, custom types) live on a growable heap managed by the VM.
- **Statements** form the program structure; separated by newlines, no semicolons or block comments (`/* */`).
- **Comments** start with `//` and run to end-of-line.
- **Top-level code** becomes bytecode and runs at initialization (the entry point).

### Syntax Reference

The language grammar is fully described in [`language.md`](language.md) (included in the repository). Here are the key sections:

| Section | Description |
|---------|-------------|
| **Lexical** | `NAME`, `INT`, `FLOAT`, `CHAR` (`'a'`), `STRING` (`"hello"`), escape sequences (`\n`, `\t`, etc.), holes `${expr}` for interpolation. |
| **Type Declarations** | `Sprite = {}` defines a heap type; constructors via `Sprite(...)` or `Sprite::new(...)`. |
| **Packed Types** | Stack-inline storage: `vec2i = int[2] { vec2i.x = vec2i[0], vec2i.y = vec2i[1] }`. No runtime overhead. |
| **Functions & Methods** | `(params) -> type { stmts }`, static members with `Name::name`. |
| **Control Flow** | `if (cond) { ... } else { ... }`, `while (cond) { ... }`, `return` (with optional value). |
| **Operators** | Arithmetic (`+ - * / = += -=`), comparison, logical (`and`, `or`). |

### Example Game: Hello World

Create a directory and put the following as `main.stge`:

```stge
// A simple type definition
Player = {}
Player::new = (String name) -> Player {
  // Store the player's name
  Player.name = String(name)
}

// Entry point — runs automatically when bytecode starts
player = Player("Alice")
Terminal::print(player.name + "\n")
```

Build and run:

```bash
./compile.sh init
./compile.sh add src/main.c
./compile.sh run .
# Output: Alice
```

### Example Game: Number Guessing

```stge
// Simple game with typed variables and control flow
GuessGame = {}
GuessGame::new = () {
  this.target = uint(42)
  this.guesses = int(0)
}

game = GuessGame()

while (true) {
  Terminal::echo("Guess the number: ")
  guess_str = Terminal::readline()
  
  guess = int(guess_str)
  game.guesses++
  
  if (guess == game.target) {
    Terminal::print("Correct! It took " + String(game.guesses) + " guesses.")
    return
  } else if (guess < game.target) {
    Terminal::print("Too low!\n")
  } else {
    Terminal::print("Too high!\n")
  }
}
```

Run: `./build/stge -game .`

### Game Example from the Repo (`script_test/main.stge`)

The project includes a built-in test game at `script_test/`. Run it to verify everything works:

```bash
./compile.sh run script_test
# Output: :: script_test ok
```

## Running Tests

```bash
./compile.sh test
```

This builds the runtime, loads `main.stge` from `script_test/`, and executes its embedded assertions. If all checks pass, output will include `:: script_test ok`.

## Project Structure

```
stge/                          # Root project directory
├── compile.sh                  # Build script (bash)
├── language.md                 # Full language specification & documentation
├── .gitignore                  # Git ignore rules
├── .sources                    # List of C source files to include in build
└── src/                         # C source code
│   ├── main.c                   # Host runtime: loads, compiles, runs STGE programs
│   ├── core/
│   │   ├── stge.c               # VM initialization and shutdown
│   │   ├── native.c             # Platform-specific I/O (Terminal::print etc.)
│   │   ├── obj.c                # Heap object management, memory allocator
│   │   ├── vm.c                 # Virtual machine: bytecode execution engine
│   │   ├── input.c              # Terminal read/write primitives
│   │   ├── screen.c             # Screen/terminal manipulation helpers
│   │   ├── color.h              # Color constants and helpers
│   │   ├── array.h              # Array type implementation
│   │   ├── cstr.h               # String utilities
│   │   └── vm.h                 # VM API definitions (heap, stack, bytecode ops)
│   ├── parser/
│   │   ├── parser.c             # C17 lexer + parser → AST
│   │   ├── ast.h                # Abstract syntax tree node types
│   │   └── name_pool.h          # Name resolution during compilation
│   └── utils/
│       └── ...                  # Utility helpers, error handling
├── include/                     # C header files (shared between src and test)
│   ├── core/                    # Core VM modules (obj.h, vm.h, etc.)
│   ├── parser/                  # Parser module headers (parser.h, ast.h)
│   └── utils/                   # Utility module headers
└── script_test/                 # Self-test game with embedded assertions
    ├── main.stge                # Test program verifying VM functionality
    └── test_object.stge         # Example: defines a simple Object type and tests it
```

## Development Guide

### Adding a New Source File

1. Write your C source in `src/` (e.g., `src/core/newmodule.c`)
2. Add its path to `.sources`:
   ```bash
   ./compile.sh add src/newmodule.c
   ```
3. If the module has headers, ensure they're under `include/` with matching `#include "..."` paths.

### Adding a New Language Feature

1. Update the grammar in [`language.md`](language.md) to document changes.
2. Extend AST node types in `src/parser/ast.h`.
3. Add lexer rules and parser rules in `src/parser/parser.c`.
4. Implement bytecode generation in the compiler (`core/stge.c` or dedicated codegen files).
5. Implement VM execution semantics (bytecode ops) in `src/core/vm.c`.

### Adding a Built-in Primitive

1. Define the function prototype and C implementation in `include/core/...h` / `...c`.
2. Register it as a method on a built-in type (e.g., `Terminal::print`, `Color::red`) in `src/core/stge.c`.

### Extending with External Libraries

The build command is straightforward — just add files and libraries to the compile script:

```bash
# Example: adding OpenSSL for crypto primitives
./compile.sh add src/crypto.c -lssl -lcrypto
```

## License

This project is licensed under the [MIT License](https://opensource.org/licenses/MIT).

## Contact / Community

Feel free to explore the source code at [`github.com`](https://github.com/geralwo/stge) or ask questions — STGE is designed for both learning and production use.

---
