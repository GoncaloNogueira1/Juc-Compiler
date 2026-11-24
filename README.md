# Juc Compiler

A compiler for Juc, a subset of Java, that generates LLVM IR. Built with Flex and Bison for the Compilers course (BSc in Informatics Engineering, University of Coimbra).

**Authors:** Gonçalo Nogueira, João Moura

## Pipeline

1. **Lexical analysis** (`jucompiler.l`, Flex): tokens, literals with underscores, comments and lexical errors
2. **Parsing** (`jucompiler.y`, Bison LALR(1)): grammar rewritten to avoid conflicts between fields and methods, explicit operator precedence, dangling-else resolution and error recovery
3. **AST** (`ast.c`): every node keeps its line and column for error reporting
4. **Semantic analysis** (`semantics.c`): two passes; the first builds the class and method symbol tables, the second type-checks statements and expressions, resolves overloaded methods and checks integer literal bounds
5. **Code generation** (`codegen.c`): LLVM IR with name mangling for overloads, short-circuit `&&` / `||` with branches and `phi` nodes, `printf` for output and `atoi` for `Integer.parseInt`

## Building

Requires GCC, Flex and Bison.

```bash
cd src
make
```

## Usage

```bash
./jucompiler < program.java > program.ll   # generate LLVM IR
lli program.ll                             # run it
```

Flags to stop at an earlier stage:

| Flag | Output |
| --- | --- |
| `-l` | tokens |
| `-t` | syntax tree |
| `-s` | symbol tables and annotated tree |
| `-e1`, `-e2`, `-e3` | lexical, syntax or semantic errors only |

Design notes (in Portuguese): [`docs/design-notes-pt.md`](docs/design-notes-pt.md).
