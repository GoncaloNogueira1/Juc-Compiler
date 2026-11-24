#ifndef CODEGEN_H
#define CODEGEN_H

#include "ast.h"
#include "semantics.h"

/* Print LLVM IR to stdout. Overload picked in semantics (see type_note on call ids). */
void codegen_program(struct node *root, SemanticResult *sr);

#endif
