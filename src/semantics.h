#ifndef SEMANTICS_H
#define SEMANTICS_H

#include "ast.h"

typedef enum {
    SYM_FIELD,
    SYM_METHOD,
    SYM_PARAM,
    SYM_LOCAL,
    SYM_RETURN
} SymbolKind;

typedef struct {
    SemType *types;
    int count;
} TypeList;

typedef struct symbol {
    char *name;
    SymbolKind kind;
    SemType type;          /* field type, var type, method return type, or return entry type */
    TypeList params;       /* only for methods */
    int line;
    int col;
    int is_param;          /* printing helper (Meta 3 wants "param") */
    struct symbol *next;
} Symbol;

typedef struct table {
    char *name;
    Symbol *head;
    Symbol *tail;
    struct table *next;
} SymbolTable;

SymbolTable *st_new(const char *name);
void st_free(SymbolTable *t);

Symbol *st_find(SymbolTable *t, const char *name);
Symbol *st_find_kind(SymbolTable *t, const char *name, SymbolKind kind);
Symbol *st_insert(SymbolTable *t, const char *name, SymbolKind kind, SemType type, int line, int col);
Symbol *st_insert_field(SymbolTable *t, const char *name, SemType type, int line, int col);
Symbol *st_insert_method(SymbolTable *t, const char *name, SemType ret_type, const SemType *param_types, int nparams, int line, int col);

void st_print_class(SymbolTable *class_table, const char *class_name);
void st_print_method(SymbolTable *method_table, const char *method_name, const SemType *param_types, int nparams);

int is_reserved_name(const char *name);

typedef struct method_tables {
    char *name;
    SemType *param_types;
    int nparams;
    SymbolTable *table;
    struct node *method_node;
    struct method_tables *next;
} MethodTables;

typedef struct semantic_result {
    int errors;
    SymbolTable *class_table;
    MethodTables *methods;
} SemanticResult;

SemanticResult *semantic_analyze(struct node *root);
void semantic_free(SemanticResult *r);

void semantic_print_tables(SemanticResult *r);

#endif