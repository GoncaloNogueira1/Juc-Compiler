#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>

#include "semantics.h"

static char *dupstr(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *d = (char *)malloc(n);
    memcpy(d, s, n);
    return d;
}

/* Linked lists + tail append: iteration order matches declaration order (reference -s output). */

int is_reserved_name(const char *name) {
    return name && strcmp(name, "_") == 0;
}

SymbolTable *st_new(const char *name) {
    SymbolTable *t = (SymbolTable *)calloc(1, sizeof(SymbolTable));
    t->name = dupstr(name);
    t->head = NULL;
    t->tail = NULL;
    t->next = NULL;
    return t;
}

static void free_symbol(Symbol *s) {
    if (!s) return;
    free(s->name);
    free(s->params.types);
    free(s);
}

void st_free(SymbolTable *t) {
    if (!t) return;
    Symbol *s = t->head;
    while (s) {
        Symbol *n = s->next;
        free_symbol(s);
        s = n;
    }
    free(t->name);
    free(t);
}

Symbol *st_find(SymbolTable *t, const char *name) {
    if (!t || !name) return NULL;
    for (Symbol *s = t->head; s; s = s->next) {
        if (strcmp(s->name, name) == 0) return s;
    }
    return NULL;
}

Symbol *st_find_kind(SymbolTable *t, const char *name, SymbolKind kind) {
    if (!t || !name) return NULL;
    for (Symbol *s = t->head; s; s = s->next) {
        if (s->kind == kind && strcmp(s->name, name) == 0) return s;
    }
    return NULL;
}

static Symbol *append(SymbolTable *t, Symbol *s) {
    if (!t || !s) return NULL;
    if (!t->head) t->head = s;
    else t->tail->next = s;
    t->tail = s;
    return s;
}

Symbol *st_insert(SymbolTable *t, const char *name, SymbolKind kind, SemType type, int line, int col) {
    if (!t || !name) return NULL;
    if (st_find(t, name)) return NULL;
    Symbol *s = (Symbol *)calloc(1, sizeof(Symbol));
    s->name = dupstr(name);
    s->kind = kind;
    s->type = type;
    s->line = line;
    s->col = col;
    s->is_param = (kind == SYM_PARAM);
    return append(t, s);
}

Symbol *st_insert_field(SymbolTable *t, const char *name, SemType type, int line, int col) {
    if (!t || !name) return NULL;
    /* Fields and methods are independent namespaces (Java-like). */
    if (st_find_kind(t, name, SYM_FIELD)) return NULL;
    Symbol *s = (Symbol *)calloc(1, sizeof(Symbol));
    s->name = dupstr(name);
    s->kind = SYM_FIELD;
    s->type = type;
    s->line = line;
    s->col = col;
    s->is_param = 0;
    return append(t, s);
}

Symbol *st_insert_method(SymbolTable *t, const char *name, SemType ret_type, const SemType *param_types, int nparams, int line, int col) {
    if (!t || !name) return NULL;
    /* Overloading is allowed: allow multiple methods with same name. */
    Symbol *s = (Symbol *)calloc(1, sizeof(Symbol));
    s->name = dupstr(name);
    s->kind = SYM_METHOD;
    s->type = ret_type;
    s->line = line;
    s->col = col;
    if (nparams > 0) {
        s->params.types = (SemType *)malloc(sizeof(SemType) * nparams);
        memcpy(s->params.types, param_types, sizeof(SemType) * nparams);
        s->params.count = nparams;
    } else {
        s->params.types = NULL;
        s->params.count = 0;
    }
    return append(t, s);
}

static void print_param_types(const SemType *types, int n) {
    printf("(");
    for (int i = 0; i < n; i++) {
        if (i > 0) printf(",");
        printf("%s", sem_type_name(types[i]));
    }
    printf(")");
}

void st_print_class(SymbolTable *class_table, const char *class_name) {
    printf("===== Class %s Symbol Table =====\n", class_name);
    for (Symbol *s = class_table->head; s; s = s->next) {
        if (s->kind == SYM_METHOD) {
            /* Exactly: Name\t(ParamTypes)\tType */
            printf("%s\t", s->name);
            print_param_types(s->params.types, s->params.count);
            printf("\t%s\n", sem_type_name(s->type));
        } else if (s->kind == SYM_FIELD) {
            /* Exactly: Name\t\tType */
            printf("%s\t\t%s\n", s->name, sem_type_name(s->type));
        }
    }
}

void st_print_method(SymbolTable *method_table, const char *method_name, const SemType *param_types, int nparams) {
    printf("===== Method %s", method_name);
    print_param_types(param_types, nparams);
    printf(" Symbol Table =====\n");
    for (Symbol *s = method_table->head; s; s = s->next) {
        if (s->kind == SYM_RETURN) {
            /* Exactly: return\t\tType */
            printf("return\t\t%s\n", sem_type_name(s->type));
        } else {
            /* Exactly: Name\t\tType[\tparam] */
            printf("%s\t\t%s%s\n",
                s->name,
                sem_type_name(s->type),
                s->is_param ? "\tparam" : "");
        }
    }
}

/*
 * Pass 1: class table + one method table per declaration (fields vs methods namespaces).
 * Pass 2: linear body walk — locals inserted at VarDecl then statements checked (declare-before-use).
 * SEM_UNDEF absorbs failures so later nodes still get checked.
 */

static void sem_error(SemanticResult *r, int line, int col, const char *fmt, const char *tok, const char *type1, const char *type2) {
    r->errors++;
    printf("Line %d, col %d: ", line, col);
    if (type1 && type2) printf(fmt, tok, type1, type2);
    else if (type1)     printf(fmt, tok, type1);
    else                printf(fmt, tok);
    putchar('\n');
}

static void sem_op_types(SemanticResult *r, int line, int col, const char *op, SemType a, SemType b) {
    sem_error(r, line, col, "Operator %s cannot be applied to types %s, %s", op, sem_type_name(a), sem_type_name(b));
}

static void sem_op_type(SemanticResult *r, int line, int col, const char *op, SemType a) {
    sem_error(r, line, col, "Operator %s cannot be applied to type %s", op, sem_type_name(a), NULL);
}

static const char *method_name_only(const char *s, char *buf, size_t bufsz) {
    if (!s || bufsz == 0) return "";
    size_t i = 0;
    while (s[i] && s[i] != '(' && i + 1 < bufsz) {
        buf[i] = s[i];
        i++;
    }
    buf[i] = '\0';
    return buf;
}

static SemType node_decl_type_to_sem(NodeType t) {
    switch (t) {
        case Bool:        return SEM_BOOL;
        case Int:         return SEM_INT;
        case Double:      return SEM_DOUBLE;
        case Void:        return SEM_VOID;
        case StringArray: return SEM_STRING_ARRAY;
        default:          return SEM_UNDEF;
    }
}

static MethodTables *mt_append(MethodTables **head, MethodTables **tail, MethodTables *m) {
    if (!*head) *head = m;
    else (*tail)->next = m;
    *tail = m;
    return m;
}

static void mt_free(MethodTables *m) {
    while (m) {
        MethodTables *n = m->next;
        free(m->name);
        free(m->param_types);
        st_free(m->table);
        free(m);
        m = n;
    }
}

/* Forward declarations */
static void pass_build_tables(SemanticResult *r, struct node *root);
static SemType check_expr(SemanticResult *r, MethodTables *m, struct node *expr);
static void check_stmt(SemanticResult *r, MethodTables *m, struct node *stmt);
static Symbol *lookup_var(SemanticResult *r, MethodTables *m, const char *name);

static int params_equal(const SemType *a, const SemType *b, int n) {
    for (int i = 0; i < n; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

static void format_method_sig(char *buf, size_t bufsz, const char *name, const SemType *pt, int nparams) {
    /* Used in error messages: must match Meta 3's expected formatting. */
    if (!buf || bufsz == 0) return;
    size_t pos = 0;
    if (!name) name = "";
    size_t nl = strlen(name);
    if (nl >= bufsz) nl = bufsz - 1;
    memcpy(buf, name, nl);
    pos = nl;
    if (pos + 1 < bufsz) buf[pos++] = '(';
    for (int i = 0; i < nparams && pos + 1 < bufsz; i++) {
        if (i > 0 && pos + 1 < bufsz) buf[pos++] = ',';
        const char *tn = sem_type_name(pt[i]);
        size_t tl = strlen(tn);
        if (pos + tl >= bufsz) tl = bufsz - 1 - pos;
        memcpy(buf + pos, tn, tl);
        pos += tl;
    }
    if (pos + 1 < bufsz) buf[pos++] = ')';
    buf[pos] = '\0';
}

static SemType rhs_type_hint(struct node *rhs) {
    if (!rhs) return SEM_UNDEF;
    if (rhs->sem_type != SEM_UNDEF) return rhs->sem_type;
    switch (rhs->type) {
        /* Operators with fixed result type regardless of operand issues */
        case Length:
        case ParseArgs:
        case Lshift:
        case Rshift:
        case Xor:
            return SEM_INT;
        case Not:
        case And:
        case Or:
        case Eq:
        case Ne:
        case Lt:
        case Le:
        case Gt:
        case Ge:
            return SEM_BOOL;
        default:
            return SEM_UNDEF;
    }
}

static int sem_is_numeric(SemType t) {
    return t == SEM_INT || t == SEM_DOUBLE;
}

static int sem_is_compatible(SemType from, SemType to) {
    /* undef is never compatible — drives cascade of operator/= errors. */
    if (from == SEM_UNDEF || to == SEM_UNDEF) return 0;
    if (from == to) return 1;
    if (from == SEM_INT && to == SEM_DOUBLE) return 1;
    return 0;
}

static SemType sem_promote_numeric(SemType a, SemType b) {
    /* Numeric operators promote int+double to double (Java-like widening). */
    if (a == SEM_DOUBLE || b == SEM_DOUBLE) return SEM_DOUBLE;
    if (a == SEM_INT && b == SEM_INT) return SEM_INT;
    return SEM_UNDEF;
}

SemanticResult *semantic_analyze(struct node *root) {
    SemanticResult *r = (SemanticResult *)calloc(1, sizeof(SemanticResult));
    r->errors = 0;
    const char *class_name = "";
    if (root && root->type == Program && root->num_children > 0 && root->children[0] && root->children[0]->type == Identifier && root->children[0]->value)
        class_name = root->children[0]->value;
    r->class_table = st_new(class_name);
    r->methods = NULL;
    pass_build_tables(r, root);
    for (MethodTables *m = r->methods; m; m = m->next) {
        struct node *body = m->method_node->children[1]; /* MethodBody */
        for (int i = 0; i < body->num_children; i++) {
            struct node *child = body->children[i];
            if (!child) continue;
            if (child->type == VarDecl) {
                struct node *vtype = child->children[0];
                struct node *vid   = child->children[1];
                SemType vt = node_decl_type_to_sem(vtype->type);
                if (is_reserved_name(vid->value)) {
                    sem_error(r, vid->line, vid->col, "Symbol %s is reserved", vid->value, NULL, NULL);
                } else if (!st_insert(m->table, vid->value, SYM_LOCAL, vt, vid->line, vid->col)) {
                    sem_error(r, vid->line, vid->col, "Symbol %s already defined", vid->value, NULL, NULL);
                }
                continue;
            }
            check_stmt(r, m, child);
        }
    }
    return r;
}

void semantic_free(SemanticResult *r) {
    if (!r) return;
    st_free(r->class_table);
    mt_free(r->methods);
    free(r);
}

static void add_method_tables(SemanticResult *r, struct node *method_decl) {
    /* MethodDecl -> MethodHeader, MethodBody */
    struct node *hdr    = method_decl->children[0];
    struct node *ret    = hdr->children[0];
    struct node *id     = hdr->children[1];
    struct node *params = hdr->children[2];

    SemType ret_type = node_decl_type_to_sem(ret->type);

    int nparams = params->num_children;
    SemType *pt = NULL;
    if (nparams > 0) {
        pt = (SemType *)malloc(sizeof(SemType) * nparams);
        for (int i = 0; i < nparams; i++) {
            struct node *pd    = params->children[i]; /* ParamDecl */
            struct node *ptype = pd->children[0];
            pt[i] = node_decl_type_to_sem(ptype->type);
        }
    }

    /* Reserved name `_` cannot be used as a method name.
     * Check params first (for errors), then emit the method name error. */
    if (is_reserved_name(id->value)) {
        for (int i = 0; i < nparams; i++) {
            struct node *pd  = params->children[i];
            struct node *pid = pd->children[1];
            if (is_reserved_name(pid->value)) {
                sem_error(r, pid->line, pid->col, "Symbol %s is reserved", pid->value, NULL, NULL);
            } else {
                for (int j = 0; j < i; j++) {
                    struct node *pdj = params->children[j];
                    struct node *pjd = pdj->children[1];
                    if (pjd->value && pid->value && strcmp(pjd->value, pid->value) == 0) {
                        sem_error(r, pid->line, pid->col, "Symbol %s already defined", pid->value, NULL, NULL);
                        break;
                    }
                }
            }
        }
        sem_error(r, id->line, id->col, "Symbol %s is reserved", id->value, NULL, NULL);
        free(pt);
        return;
    }

    /* Duplicate method (same name + same param types) should not create duplicate output. */
    int is_dup_method = 0;
    for (Symbol *s = r->class_table->head; s; s = s->next) {
        if (s->kind != SYM_METHOD) continue;
        if (strcmp(s->name, id->value) != 0) continue;
        if (s->params.count != nparams) continue;
        if (params_equal(s->params.types, pt, nparams)) {
            is_dup_method = 1;
            break;
        }
    }
    if (is_dup_method) {
        /* Even for duplicate methods, parameter declarations are still checked. */
        for (int i = 0; i < nparams; i++) {
            struct node *pd  = params->children[i];
            struct node *pid = pd->children[1];
            if (is_reserved_name(pid->value)) {
                sem_error(r, pid->line, pid->col, "Symbol %s is reserved", pid->value, NULL, NULL);
                continue;
            }
            for (int j = 0; j < i; j++) {
                struct node *pdj = params->children[j];
                struct node *pjd = pdj->children[1];
                if (pjd && pid && pjd->value && pid->value && strcmp(pjd->value, pid->value) == 0) {
                    sem_error(r, pid->line, pid->col, "Symbol %s already defined", pid->value, NULL, NULL);
                    break;
                }
            }
        }
        char sigbuf[512];
        format_method_sig(sigbuf, sizeof(sigbuf), id->value, pt, nparams);
        sem_error(r, id->line, id->col, "Symbol %s already defined", sigbuf, NULL, NULL);
        free(pt);
        return; /* do not create method entry or method table output */
    }

    /* Insert method signature (overloading allowed) */
    st_insert_method(r->class_table, id->value, ret_type, pt, nparams, id->line, id->col);

    /* Create method table */
    MethodTables *mt = (MethodTables *)calloc(1, sizeof(MethodTables));
    mt->name         = dupstr(id->value);
    mt->param_types  = pt;
    mt->nparams      = nparams;
    mt->method_node  = method_decl;
    mt->table        = st_new("method");
    mt->next         = NULL;

    /* return entry */
    st_insert(mt->table, "return", SYM_RETURN, ret_type, id->line, id->col);

    /* params */
    for (int i = 0; i < nparams; i++) {
        struct node *pd    = params->children[i];
        struct node *pid   = pd->children[1];
        SemType ptype = pt[i];
        if (is_reserved_name(pid->value)) {
            sem_error(r, pid->line, pid->col, "Symbol %s is reserved", pid->value, NULL, NULL);
        } else if (!st_insert(mt->table, pid->value, SYM_PARAM, ptype, pid->line, pid->col)) {
            sem_error(r, pid->line, pid->col, "Symbol %s already defined", pid->value, NULL, NULL);
        }
    }

    /* locals are inserted during type-check traversal to enforce declaration-before-use */

    /* Append in declaration order */
    MethodTables *tail = r->methods;
    if (!tail) {
        r->methods = mt;
    } else {
        while (tail->next) tail = tail->next;
        tail->next = mt;
    }
}

static void pass_build_tables(SemanticResult *r, struct node *root) {
    if (!root || root->type != Program) return;
    /* Program children: Identifier(className), then FieldDecl/MethodDecl.
     * We keep insertion order to match the reference output. */
    for (int i = 0; i < root->num_children; i++) {
        struct node *c = root->children[i];
        if (c->type == FieldDecl) {
            struct node *fid = c->children[1];
            SemType ft = node_decl_type_to_sem(c->children[0]->type);
            if (is_reserved_name(fid->value)) {
                sem_error(r, fid->line, fid->col, "Symbol %s is reserved", fid->value, NULL, NULL);
            } else if (!st_insert_field(r->class_table, fid->value, ft, fid->line, fid->col)) {
                sem_error(r, fid->line, fid->col, "Symbol %s already defined", fid->value, NULL, NULL);
            }
        } else if (c->type == MethodDecl) {
            add_method_tables(r, c);
        }
    }
}

static Symbol *lookup_var(SemanticResult *r, MethodTables *m, const char *name) {
    if (!name) return NULL;
    /* method scope: params + locals */
    Symbol *s = st_find(m->table, name);
    if (s && (s->kind == SYM_PARAM || s->kind == SYM_LOCAL)) return s;
    /* class scope: only fields (methods live in a separate namespace) */
    Symbol *f = st_find_kind(r->class_table, name, SYM_FIELD);
    if (f && f->kind == SYM_FIELD) return f;
    return NULL;
}

static SemType check_identifier_expr(SemanticResult *r, MethodTables *m, struct node *id) {
    if (is_reserved_name(id->value)) {
        sem_error(r, id->line, id->col, "Symbol %s is reserved", id->value, NULL, NULL);
        id->sem_type = SEM_UNDEF;
        id->annotate = 1;
        return SEM_UNDEF;
    }
    Symbol *sym = lookup_var(r, m, id->value);
    id->annotate = 1;
    if (!sym) {
        sem_error(r, id->line, id->col, "Cannot find symbol %s", id->value, NULL, NULL);
        id->sem_type = SEM_UNDEF;
        return SEM_UNDEF;
    }
    id->sem_type = sym->type;
    return sym->type;
}

static SemType check_number_bounds(SemanticResult *r, struct node *nat) {
    /* NATURAL may include '_' */
    const char *s = nat->value ? nat->value : "";
    char buf[128];
    int j = 0;
    for (int i = 0; s[i] && j < (int)sizeof(buf) - 1; i++) {
        if (s[i] != '_') buf[j++] = s[i];
    }
    buf[j] = '\0';

    /* Meta3 tests: out of bounds if more than 10 digits, or exactly 2147483648 */
    if (strlen(buf) > 10 || (strlen(buf) == 10 && strcmp(buf, "2147483648") >= 0)) {
        sem_error(r, nat->line, nat->col, "Number %s out of bounds", nat->value ? nat->value : buf, NULL, NULL);
        /* Meta 3 policy (per tests): still treat the literal as int for annotation/propagation,
         * even though it is semantically invalid. */
        nat->sem_type = SEM_INT;
        nat->annotate = 1;
        return SEM_INT;
    }
    nat->sem_type = SEM_INT;
    nat->annotate = 1;
    return SEM_INT;
}

static SemType check_decimal_bounds(SemanticResult *r, struct node *dec) {
    const char *s = dec->value ? dec->value : "";
    char buf[256];
    int j = 0;
    for (int i = 0; s[i] && j < (int)sizeof(buf) - 1; i++) {
        if (s[i] != '_') buf[j++] = s[i];
    }
    buf[j] = '\0';

    errno = 0;
    double val = strtod(buf, NULL);
    /* ERANGE: overflow to inf or underflow to 0 is an error; tiny subnormals are ok */
    if (errno == ERANGE && (val == 0.0 || isinf(val))) {
        sem_error(r, dec->line, dec->col, "Number %s out of bounds", dec->value ? dec->value : buf, NULL, NULL);
    }

    dec->sem_type = SEM_DOUBLE;
    dec->annotate = 1;
    return SEM_DOUBLE;
}

static SemType check_expr(SemanticResult *r, MethodTables *m, struct node *expr) {
    if (!expr) return SEM_UNDEF;

    switch (expr->type) {
        case Identifier:
            return check_identifier_expr(r, m, expr);
        case Natural:
            return check_number_bounds(r, expr);
        case Decimal:
            return check_decimal_bounds(r, expr);
        case BoolLit:
            expr->sem_type = SEM_BOOL;
            expr->annotate = 1;
            return SEM_BOOL;
        case StrLit:
            expr->sem_type = SEM_STRING;
            expr->annotate = 1;
            return SEM_STRING;

        case Length: {
            SemType t0 = check_expr(r, m, expr->children[0]);
            expr->annotate = 1;
            if (t0 != SEM_STRING_ARRAY) {
                sem_op_type(r, expr->line, expr->col, ".length", t0);
            }
            expr->sem_type = SEM_INT;
            return SEM_INT;
        }

        case Not: {
            SemType t = check_expr(r, m, expr->children[0]);
            expr->annotate = 1;
            /* Always cascade undef: report error whenever operand isn't boolean.
             * Result type is boolean regardless. */
            if (t != SEM_BOOL) {
                sem_op_type(r, expr->line, expr->col, "!", t);
            }
            expr->sem_type = SEM_BOOL;
            return SEM_BOOL;
        }

        case Minus:
        case Plus: {
            SemType t = check_expr(r, m, expr->children[0]);
            expr->annotate = 1;
            const char *op = (expr->type == Minus) ? "-" : "+";
            /* Always cascade undef: report error whenever operand isn't numeric.
             * Result type is undef so subsequent ops can also cascade. */
            if (!sem_is_numeric(t)) {
                sem_op_type(r, expr->line, expr->col, op, t);
                expr->sem_type = SEM_UNDEF;
                return SEM_UNDEF;
            }
            expr->sem_type = t;
            return t;
        }

        case Add: case Sub: case Mul: case Div: case Mod: {
            SemType l  = check_expr(r, m, expr->children[0]);
            SemType rr = check_expr(r, m, expr->children[1]);
            expr->annotate = 1;
            const char *op = (expr->type == Add) ? "+" :
                             (expr->type == Sub) ? "-" :
                             (expr->type == Mul) ? "*" :
                             (expr->type == Div) ? "/" : "%";

            if (expr->type == Mod) {
                /* %: numeric promotion when both operands numeric; otherwise error + undef */
                if (sem_is_numeric(l) && sem_is_numeric(rr)) {
                    expr->sem_type = sem_promote_numeric(l, rr);
                    return expr->sem_type;
                }
                sem_op_types(r, expr->line, expr->col, op, l, rr);
                expr->sem_type = SEM_UNDEF;
                return SEM_UNDEF;
            }

            /* + - * / : if operands are not both numeric, report error and yield undef */
            if (!sem_is_numeric(l) || !sem_is_numeric(rr)) {
                sem_op_types(r, expr->line, expr->col, op, l, rr);
                expr->sem_type = SEM_UNDEF;
                return SEM_UNDEF;
            }

            expr->sem_type = sem_promote_numeric(l, rr);
            return expr->sem_type;
        }

        case Lt: case Le: case Gt: case Ge: {
            SemType l  = check_expr(r, m, expr->children[0]);
            SemType rr = check_expr(r, m, expr->children[1]);
            expr->annotate = 1;
            const char *op = (expr->type == Lt) ? "<"  :
                             (expr->type == Le) ? "<=" :
                             (expr->type == Gt) ? ">"  : ">=";
            /* Always cascade undef: report error whenever operands aren't both numeric.
             * Result type is boolean regardless. */
            if (!sem_is_numeric(l) || !sem_is_numeric(rr)) {
                sem_op_types(r, expr->line, expr->col, op, l, rr);
            }
            expr->sem_type = SEM_BOOL;
            return SEM_BOOL;
        }

        case Eq: case Ne: {
            SemType l  = check_expr(r, m, expr->children[0]);
            SemType rr = check_expr(r, m, expr->children[1]);
            expr->annotate = 1;
            const char *op = (expr->type == Eq) ? "==" : "!=";
            if (l != SEM_UNDEF && rr != SEM_UNDEF &&
                ((l == rr && l != SEM_STRING_ARRAY) || (sem_is_numeric(l) && sem_is_numeric(rr)))) {
                expr->sem_type = SEM_BOOL;
                return SEM_BOOL;
            }
            sem_op_types(r, expr->line, expr->col, op, l, rr);
            /* Result type is boolean even if operands are invalid/undef. */
            expr->sem_type = SEM_BOOL;
            return SEM_BOOL;
        }

        case And: case Or: case Xor: {
            SemType l  = check_expr(r, m, expr->children[0]);
            SemType rr = check_expr(r, m, expr->children[1]);
            expr->annotate = 1;
            if (expr->type == Xor) {
                const char *op = "^";
                if (l == SEM_BOOL && rr == SEM_BOOL) {
                    expr->sem_type = SEM_BOOL;
                    return SEM_BOOL;
                }
                if (l == SEM_INT && rr == SEM_INT) {
                    expr->sem_type = SEM_INT;
                    return SEM_INT;
                }
                sem_op_types(r, expr->line, expr->col, op, l, rr);
                /* Recover type so Assign/while do not cascade extra "= ... undef" / "undef in while". */
                expr->sem_type = (sem_is_numeric(l) && sem_is_numeric(rr)) ? SEM_INT : SEM_BOOL;
                return expr->sem_type;
            } else {
                const char *op = (expr->type == And) ? "&&" : "||";
                if (l != SEM_BOOL || rr != SEM_BOOL) sem_op_types(r, expr->line, expr->col, op, l, rr);
                /* Result type is boolean even if operands are invalid/undef. */
                expr->sem_type = SEM_BOOL;
                return SEM_BOOL;
            }
        }

        case Lshift: case Rshift: {
            SemType l  = check_expr(r, m, expr->children[0]);
            SemType rr = check_expr(r, m, expr->children[1]);
            expr->annotate = 1;
            const char *op = (expr->type == Lshift) ? "<<" : ">>";
            if (l != SEM_INT || rr != SEM_INT) {
                sem_op_types(r, expr->line, expr->col, op, l, rr);
                expr->sem_type = SEM_INT;
                return SEM_INT;
            }
            expr->sem_type = SEM_INT;
            return SEM_INT;
        }

        case Assign: {
            struct node *lhs = expr->children[0];
            struct node *rhs = expr->children[1];
            expr->annotate = 1;
            lhs->annotate  = 1;
            /* Check LHS first to preserve left-to-right error order. */
            SemType lt;
            if (is_reserved_name(lhs->value)) {
                sem_error(r, lhs->line, lhs->col, "Symbol %s is reserved", lhs->value, NULL, NULL);
                lhs->sem_type = SEM_UNDEF;
                lt = SEM_UNDEF;
            } else {
                Symbol *sym = lookup_var(r, m, lhs->value);
                if (!sym) {
                    sem_error(r, lhs->line, lhs->col, "Cannot find symbol %s", lhs->value, NULL, NULL);
                    lhs->sem_type  = SEM_UNDEF;
                    lt = SEM_UNDEF;
                } else {
                    lhs->sem_type = sym->type;
                    lt = sym->type;
                }
            }
            /* Then evaluate RHS. */
            SemType rt = check_expr(r, m, rhs);
            expr->sem_type = lt;
            if (lt == SEM_STRING_ARRAY || rt == SEM_STRING_ARRAY) {
                sem_op_types(r, expr->line, expr->col, "=", lt, rt);
            } else if (!sem_is_compatible(rt, lt)) {
                sem_op_types(r, expr->line, expr->col, "=", lt, rt);
            }
            return lt;
        }

        case ParseArgs: {
            /* ParseArgs(Identifier(args), indexExpr) => int, but args must be String[] */
            struct node *arr = expr->children[0];
            struct node *idx = expr->children[1];
            SemType at = check_expr(r, m, arr);
            SemType it = check_expr(r, m, idx);
            expr->annotate = 1;
            /* Targeted rule (meta3/invalid_parse_int): parseInt reports as a binary operator:
             * Operator Integer.parseInt cannot be applied to types <arrayType>, <indexType> */
            if (at != SEM_STRING_ARRAY || it != SEM_INT) {
                sem_op_types(r, expr->line, expr->col, "Integer.parseInt", at, it);
            }
            expr->sem_type = SEM_INT;
            return SEM_INT;
        }

        case Call:
            /* Resolve overloaded calls (Meta 3 simplified rules):
             * - exact match on parameter types wins
             * - otherwise, a single compatible match wins (int -> double widening)
             * - multiple compatible matches => ambiguous
             * - no compatible match => cannot find symbol
             *
             * On failure/ambiguity, the Call and its Identifier are typed as 'undef' so later
             * checks can continue and still report follow-up errors. */
            expr->annotate = 1;
            if (expr->num_children == 0 || !expr->children[0]) {
                expr->sem_type = SEM_UNDEF;
                return SEM_UNDEF;
            }
            {
                struct node *id = expr->children[0];
                id->annotate = 1;

                int nargs = expr->num_children - 1;
                SemType *actuals = NULL;
                if (nargs > 0) {
                    actuals = (SemType *)malloc(sizeof(SemType) * nargs);
                    for (int i = 0; i < nargs; i++)
                        actuals[i] = check_expr(r, m, expr->children[i + 1]);
                }

                /* gather candidates (methods with same name) */
                int cand_count = 0;
                for (Symbol *s = r->class_table->head; s; s = s->next)
                    if (s->kind == SYM_METHOD && strcmp(s->name, id->value) == 0)
                        cand_count++;

                Symbol **cands = NULL;
                if (cand_count > 0) {
                    cands = (Symbol **)malloc(sizeof(Symbol *) * cand_count);
                    int k = 0;
                    for (Symbol *s = r->class_table->head; s; s = s->next)
                        if (s->kind == SYM_METHOD && strcmp(s->name, id->value) == 0)
                            cands[k++] = s;
                }

                Symbol *exact      = NULL;
                int     exact_hits = 0;
                for (int i = 0; i < cand_count; i++) {
                    Symbol *s = cands[i];
                    if (s->params.count != nargs) continue;
                    int ok = 1;
                    for (int a = 0; a < nargs; a++) {
                        if (actuals[a] == SEM_UNDEF)              { ok = 0; break; }
                        if (actuals[a] != s->params.types[a])     { ok = 0; break; }
                    }
                    if (ok) { exact = s; exact_hits++; }
                }
                if (exact_hits == 1) {
                    expr->sem_type = exact->type;
                    id->sem_type   = exact->type;
                    /* annotate identifier with the selected signature */
                    free(id->type_note);
                    {
                        int n = exact->params.count;
                        size_t cap = 2; /* () */
                        for (int p = 0; p < n; p++) cap += strlen(sem_type_name(exact->params.types[p])) + 1;
                        char *sig = (char *)malloc(cap + 1);
                        size_t pos = 0;
                        sig[pos++] = '(';
                        for (int p = 0; p < n; p++) {
                            if (p > 0) sig[pos++] = ',';
                            const char *tn = sem_type_name(exact->params.types[p]);
                            size_t len = strlen(tn);
                            memcpy(sig + pos, tn, len);
                            pos += len;
                        }
                        sig[pos++] = ')';
                        sig[pos] = '\0';
                        id->type_note = sig;
                    }
                    free(actuals);
                    free(cands);
                    return expr->sem_type;
                }

                /* compatible match */
                Symbol *best      = NULL;
                int     best_hits = 0;
                for (int i = 0; i < cand_count; i++) {
                    Symbol *s = cands[i];
                    if (s->params.count != nargs) continue;
                    int ok = 1;
                    for (int a = 0; a < nargs; a++) {
                        if (actuals[a] == SEM_UNDEF)                              { ok = 0; break; }
                        if (!sem_is_compatible(actuals[a], s->params.types[a]))   { ok = 0; break; }
                    }
                    if (ok) { best = s; best_hits++; }
                }

                if (best_hits == 1) {
                    expr->sem_type = best->type;
                    id->sem_type   = best->type;
                    free(id->type_note);
                    {
                        int n = best->params.count;
                        size_t cap = 2;
                        for (int p = 0; p < n; p++) cap += strlen(sem_type_name(best->params.types[p])) + 1;
                        char *sig = (char *)malloc(cap + 1);
                        size_t pos = 0;
                        sig[pos++] = '(';
                        for (int p = 0; p < n; p++) {
                            if (p > 0) sig[pos++] = ',';
                            const char *tn = sem_type_name(best->params.types[p]);
                            size_t len = strlen(tn);
                            memcpy(sig + pos, tn, len);
                            pos += len;
                        }
                        sig[pos++] = ')';
                        sig[pos] = '\0';
                        id->type_note = sig;
                    }
                } else if (best_hits > 1) {
                    char sigbuf[512];
                    format_method_sig(sigbuf, sizeof(sigbuf), id->value, actuals, nargs);
                    sem_error(r, id->line, id->col, "Reference to method %s is ambiguous", sigbuf, NULL, NULL);
                    expr->sem_type = SEM_UNDEF;
                    id->sem_type   = SEM_UNDEF;
                    free(id->type_note);
                    id->type_note = NULL;
                } else {
                    char sigbuf[512];
                    format_method_sig(sigbuf, sizeof(sigbuf), id->value, actuals, nargs);
                    sem_error(r, id->line, id->col, "Cannot find symbol %s", sigbuf, NULL, NULL);
                    expr->sem_type = SEM_UNDEF;
                    id->sem_type   = SEM_UNDEF;
                    free(id->type_note);
                    id->type_note = NULL;
                }

                free(actuals);
                free(cands);
                return expr->sem_type;
            }

        default:
            /* Not an expression */
            return SEM_UNDEF;
    }
}

static void check_stmt(SemanticResult *r, MethodTables *m, struct node *stmt) {
    if (!stmt) return;
    switch (stmt->type) {
        case If: {
            SemType ct = check_expr(r, m, stmt->children[0]);
            check_stmt(r, m, stmt->children[1]);
            check_stmt(r, m, stmt->children[2]);
            if (ct != SEM_BOOL) {
                sem_error(r, stmt->children[0]->line, stmt->children[0]->col, "Incompatible type %s in if statement", sem_type_name(ct), NULL, NULL);
            }
            break;
        }
        case While: {
            SemType ct = check_expr(r, m, stmt->children[0]);
            check_stmt(r, m, stmt->children[1]);
            if (ct != SEM_BOOL) {
                sem_error(r, stmt->children[0]->line, stmt->children[0]->col, "Incompatible type %s in while statement", sem_type_name(ct), NULL, NULL);
            }
            break;
        }
        case Block:
        case MethodBody:
            for (int i = 0; i < stmt->num_children; i++) {
                struct node *c = stmt->children[i];
                if (!c) continue;
                if (c->type == VarDecl) continue;
                check_stmt(r, m, c);
            }
            break;
        case Return: {
            SemType expected = st_find(m->table, "return")->type;
            if (stmt->num_children == 0) {
                if (expected != SEM_VOID && expected != SEM_UNDEF) {
                    sem_error(r, stmt->line, stmt->col, "Incompatible type void in return statement", "", NULL, NULL);
                }
            } else {
                SemType rt = check_expr(r, m, stmt->children[0]);
                if (rt == SEM_VOID) {
                    sem_error(r, stmt->children[0]->line, stmt->children[0]->col, "Incompatible type void in return statement", "", NULL, NULL);
                    break;
                }
                if (expected != SEM_UNDEF && !sem_is_compatible(rt, expected)) {
                    sem_error(r, stmt->children[0]->line, stmt->children[0]->col, "Incompatible type %s in return statement", sem_type_name(rt), NULL, NULL);
                }
            }
            break;
        }
        case Print: {
            if (stmt->num_children > 0) {
                SemType pt = check_expr(r, m, stmt->children[0]);
                if (!(pt == SEM_BOOL || pt == SEM_INT || pt == SEM_DOUBLE || pt == SEM_STRING)) {
                    sem_error(r, stmt->children[0]->line, stmt->children[0]->col, "Incompatible type %s in System.out.print statement", sem_type_name(pt), NULL, NULL);
                }
            }
            break;
        }
        case Assign:
        case Call:
        case ParseArgs:
            (void)check_expr(r, m, stmt);
            break;
        default:
            /* ignore */
            break;
    }
}

void semantic_print_tables(SemanticResult *r) {
    if (!r || !r->class_table) return;
    st_print_class(r->class_table, r->class_table->name ? r->class_table->name : "");
    printf("\n");
    for (MethodTables *m = r->methods; m; m = m->next) {
        st_print_method(m->table, m->name, m->param_types, m->nparams);
        printf("\n");
    }
}