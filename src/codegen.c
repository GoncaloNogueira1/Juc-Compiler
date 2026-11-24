/*
  Authors:
    Gonçalo Nogueira
    João Moura
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "codegen.h"
#include "ast.h"
#include "semantics.h"

/* One stack frame: local allocas; String[] params store length + pointer (argv). */
typedef struct VMap {
    int n;
    char *nm[256];
    char sl[256][48];
    SemType ty[256];
    int isa[256];
    char Ls[256][48], Ps[256][48];
} VMap;

/* Temp registers (%t), labels, string globals (@.gs*). */
typedef struct CG {
    SemanticResult *sr;
    const char *cls;
    MethodTables *mt;
    int tmp, lbl, sg;
    char ustr[384][512];
    int uid[384];
    int nu;
    int str_sz[512];
    int cur_lbl;  /* 0 = entry block, n>0 = Ln block */
    int blk_open; /* 1 = current block needs a terminator */
} CG;

static int NT(CG *g) { return ++g->tmp; }
static int NL(CG *g) { return ++g->lbl; }

static void EL(CG *g, int n) { printf("L%d:\n", n); g->cur_lbl = n; g->blk_open = 1; }

/* Current block label for phi operands */
static void phi_pred(CG *g, char *buf, size_t sz) {
    if (g->cur_lbl == 0) snprintf(buf, sz, "%%entry");
    else snprintf(buf, sz, "%%L%d", g->cur_lbl);
}

static int emit_gep_global(CG *g, const char *glbl, int dim) {
    int t = NT(g);
    printf("  %%t%d = getelementptr inbounds [%d x i8], ptr %s, i32 0, i32 0\n", t, dim, glbl);
    return t;
}

static char SigC(SemType t) {
    switch (t) {
        case SEM_BOOL: return 'b';
        case SEM_INT: return 'i';
        case SEM_DOUBLE: return 'd';
        case SEM_STRING: return 's';
        case SEM_STRING_ARRAY: return 'a';
        default: return 'u';
    }
}

/* Name mangling: ju_Class_method_<param chars so overloads get different LLVM symbols */
static void Mang(char *b, size_t cap, const char *cl, const char *mn, SemType *pt, int np) {
    size_t p = 0;
    p += snprintf(b + p, cap - p, "ju_%s_%s", cl, mn);
    if (np > 0) {
        p += snprintf(b + p, cap - p, "_");
        for (int i = 0; i < np && p + 2 < cap; i++)
            p += snprintf(b + p, cap - p, "%c", SigC(pt[i]));
    }
}

static void SigSym(Symbol *s, char *o, size_t cap) {
    size_t p = 0;
    o[p++] = '(';
    for (int i = 0; i < s->params.count; i++) {
        if (i > 0 && p < cap) o[p++] = ',';
        const char *tn = sem_type_name(s->params.types[i]);
        size_t L = strlen(tn);
        if (p + L < cap) { memcpy(o + p, tn, L); p += L; }
    }
    if (p < cap) o[p++] = ')';
    o[p] = 0;
}

/* Find method: type_note from semantics must match symbol table signature string */
static Symbol *FindM(SemanticResult *sr, const char *nm, const char *note) {
    if (!note) return NULL;
    for (Symbol *s = sr->class_table->head; s; s = s->next) {
        if (s->kind != SYM_METHOD || strcmp(s->name, nm)) continue;
        char buf[512];
        SigSym(s, buf, sizeof buf);
        if (!strcmp(buf, note)) return s;
    }
    return NULL;
}

static SemType NdSem(NodeType t) {
    switch (t) {
        case Bool: return SEM_BOOL;
        case Int: return SEM_INT;
        case Double: return SEM_DOUBLE;
        case Void: return SEM_VOID;
        case StringArray: return SEM_STRING_ARRAY;
        default: return SEM_UNDEF;
    }
}

static const char *LT(SemType t) {
    switch (t) {
        case SEM_BOOL: return "i1";
        case SEM_INT: return "i32";
        case SEM_DOUBLE: return "double";
        case SEM_STRING: return "ptr";
        default: return "i32";
    }
}

static void VmClr(VMap *m) {
    for (int i = 0; i < m->n; i++) free(m->nm[i]);
    m->n = 0;
}

static void VmPut(VMap *m, const char *n, const char *s, SemType t, int sa, const char *Ls, const char *Ps) {
    if (m->n >= 256) return;
    m->nm[m->n] = strdup(n);
    if (!sa && s)
        strcpy(m->sl[m->n], s);
    else
        m->sl[m->n][0] = '\0';
    m->ty[m->n] = t;
    m->isa[m->n] = sa;
    if (sa) {
        strcpy(m->Ls[m->n], Ls);
        strcpy(m->Ps[m->n], Ps);
    }
    m->n++;
}

static int VmIx(VMap *stk, int dep, const char *name, int *lay) {
    for (int L = dep; L >= 0; L--) {
        VMap *m = stk + L;
        for (int i = 0; i < m->n; i++)
            if (!strcmp(m->nm[i], name)) {
                if (lay) *lay = L;
                return i;
            }
    }
    return -1;
}

static void strip_nat(const char *s, char *o, size_t c) {
    size_t j = 0;
    for (size_t i = 0; s[i] && j + 1 < c; i++)
        if (s[i] != '_') o[j++] = s[i];
    o[j] = 0;
}

static void strip_dec(const char *s, char *o, size_t c) { strip_nat(s, o, c); }

static void llvm_double_lit(const char *src, char *out, size_t cap) {
    char nb[256];
    int j = 0;
    for (size_t i = 0; src[i] && j < (int)sizeof(nb) - 1; i++)
        if (src[i] != '_') nb[j++] = src[i];
    nb[j] = '\0';
    if (nb[0] == '.') {
        char tmp[256];
        snprintf(tmp, sizeof tmp, "0%s", nb);
        strncpy(nb, tmp, sizeof nb);
        nb[sizeof(nb) - 1] = '\0';
    }
    char *exp = strpbrk(nb, "eE");
    char *dot = strchr(nb, '.');
    if (exp && !dot) {
        size_t el = (size_t)(exp - nb);
        size_t rest = strlen(exp);
        if (el + 2 + rest + 1 < sizeof(nb)) {
            memmove(nb + el + 2, nb + el, rest + 1);
            nb[el] = '.';
            nb[el + 1] = '0';
        }
    }
    snprintf(out, cap, "%s", nb);
}

/* Decode a quoted string literal value (as stored in StrLit node) to raw bytes.
 * \r at position 0 stays as \r; elsewhere becomes \n (to match Java semantics). */
static void str_in(const char *v, char *b, size_t cap) {
    size_t j = 0;
    if (!v || v[0] != '"') { b[0] = 0; return; }
    for (size_t i = 1; v[i] && v[i] != '"' && j + 1 < cap;) {
        if (v[i] == '\\' && v[i + 1]) {
            char e = v[i + 1];
            if (e == 'n') b[j++] = '\n';
            else if (e == 't') b[j++] = '\t';
            else if (e == 'r') {
                char ch = (j > 0) ? '\n' : '\r';
                b[j++] = ch;
            }
            else if (e == 'f') b[j++] = '\f';
            else if (e == '\\') b[j++] = '\\';
            else if (e == '"') b[j++] = '"';
            else { b[j++] = '\\'; b[j++] = e; }
            i += 2;
        } else
            b[j++] = v[i++];
    }
    b[j] = 0;
}

static void EmitStr(CG *g, const char *inner, int id) {
    size_t L = strlen(inner);
    int N = (int)L + 1;
    if (id >= 0 && id < 512) g->str_sz[id] = N;
    printf("@.gs%d = private unnamed_addr constant [%d x i8] c\"", id, N);
    for (size_t i = 0; i < L; i++) {
        unsigned char c = (unsigned char)inner[i];
        if (c == '\\') printf("\\5C");
        else if (c == '"') printf("\\22");
        else if (c == '\n') printf("\\0A");
        else if (c == '\t') printf("\\09");
        else if (c == '\r') printf("\\0D");
        else if (c >= 32 && c < 127) putchar(c);
        else printf("\\%02X", c);
    }
    printf("\\00\"\n");
}

static int StrUid(CG *g, const char *inner) {
    for (int i = 0; i < g->nu; i++)
        if (!strcmp(g->ustr[i], inner)) return g->uid[i];
    strncpy(g->ustr[g->nu], inner, 511);
    int id = ++g->sg;
    g->uid[g->nu] = id;
    EmitStr(g, inner, id);
    g->nu++;
    return id;
}

static void ScanStr(CG *g, struct node *n) {
    if (!n) return;
    if (n->type == StrLit) {
        char buf[4096];
        str_in(n->value, buf, sizeof buf);
        StrUid(g, buf);
    }
    for (int i = 0; i < n->num_children; i++) ScanStr(g, n->children[i]);
}

static const char *Fld(CG *g, const char *f) {
    static char b[160];
    snprintf(b, sizeof b, "@%s_%s", g->cls, f);
    return b;
}

static void RT(void) {
    printf("; jucompiler llvm\n");
    printf("declare i32 @printf(ptr, ...)\n");
    printf("declare i32 @atoi(ptr)\n");
    printf("declare i32 @strcmp(ptr, ptr)\n");
    printf("@.fmt_d = private unnamed_addr constant [3 x i8] c\"%%d\\00\"\n");
    printf("@.fmt_e = private unnamed_addr constant [6 x i8] c\"%%.16e\\00\"\n");
    printf("@.fmt_s = private unnamed_addr constant [3 x i8] c\"%%s\\00\"\n");
    printf("@.str_true = private unnamed_addr constant [5 x i8] c\"true\\00\"\n");
    printf("@.str_false = private unnamed_addr constant [6 x i8] c\"false\\00\"\n");
}

/* Class fields as LLVM globals (no objects, one class per file) */
static void Fields(CG *g, struct node *prog) {
    for (int i = 0; i < prog->num_children; i++) {
        struct node *c = prog->children[i];
        if (c->type != FieldDecl) continue;
        SemType ft = NdSem(c->children[0]->type);
        struct node *id = c->children[1];
        if (ft == SEM_BOOL) printf("%s = global i1 0\n", Fld(g, id->value));
        else if (ft == SEM_INT) printf("%s = global i32 0\n", Fld(g, id->value));
        else if (ft == SEM_DOUBLE) printf("%s = global double 0.0\n", Fld(g, id->value));
    }
}

static void Prom(SemType *a, SemType *b, SemType *res) {
    if (*a == SEM_DOUBLE || *b == SEM_DOUBLE) *res = SEM_DOUBLE;
    else *res = SEM_INT;
}

static void VmInit(VMap *m) { m->n = 0; }

static void LoadVar(CG *g, VMap *stk, int dep, const char *name, char *out, size_t osz, SemType *ty) {
    int lay = 0;
    int ix = VmIx(stk, dep, name, &lay);
    if (ix < 0) {
        Symbol *f = st_find_kind(g->sr->class_table, name, SYM_FIELD);
        if (!f) {
            *ty = SEM_UNDEF;
            snprintf(out, osz, "undef");
            return;
        }
        *ty = f->type;
        int t = NT(g);
        snprintf(out, osz, "%%t%d", t);
        if (f->type == SEM_INT) printf("  %s = load i32, ptr %s\n", out, Fld(g, name));
        else if (f->type == SEM_DOUBLE) printf("  %s = load double, ptr %s\n", out, Fld(g, name));
        else if (f->type == SEM_BOOL) printf("  %s = load i1, ptr %s\n", out, Fld(g, name));
        else if (f->type == SEM_STRING) printf("  %s = load ptr, ptr %s\n", out, Fld(g, name));
        return;
    }
    if (stk[lay].isa[ix]) {
        *ty = SEM_STRING_ARRAY;
        snprintf(out, osz, "undef");
        return;
    }
    *ty = stk[lay].ty[ix];
    int t = NT(g);
    snprintf(out, osz, "%%t%d", t);
    const char *S = stk[lay].sl[ix];
    if (stk[lay].ty[ix] == SEM_INT) printf("  %s = load i32, ptr %s\n", out, S);
    else if (stk[lay].ty[ix] == SEM_DOUBLE) printf("  %s = load double, ptr %s\n", out, S);
    else if (stk[lay].ty[ix] == SEM_BOOL) printf("  %s = load i1, ptr %s\n", out, S);
    else if (stk[lay].ty[ix] == SEM_STRING) printf("  %s = load ptr, ptr %s\n", out, S);
}

static void SaLenPtr(CG *g, VMap *stk, int dep, const char *name, char *leno, char *ptro, size_t oz) {
    (void)g;
    int lay = 0;
    int ix = VmIx(stk, dep, name, &lay);
    if (ix < 0 || !stk[lay].isa[ix]) {
        snprintf(leno, oz, "0");
        snprintf(ptro, oz, "null");
        return;
    }
    int t1 = NT(g), t2 = NT(g);
    snprintf(leno, oz, "%%t%d", t1);
    snprintf(ptro, oz, "%%t%d", t2);
    printf("  %s = load i32, ptr %s\n", leno, stk[lay].Ls[ix]);
    printf("  %s = load ptr, ptr %s\n", ptro, stk[lay].Ps[ix]);
}

static void cg_lval(CG *g, VMap *stk, int dep, struct node *lhs, char *ptrn, size_t osz, SemType *pty);
static void cg_stmt(CG *g, VMap *stk, int *dep, struct node *s);
static void cg_body(CG *g, VMap *stk, int *dep, struct node *body);

static void cg_expr(CG *g, VMap *stk, int dep, struct node *e, char *out, size_t osz, SemType *ty) {
    if (!e) {
        *ty = SEM_UNDEF;
        snprintf(out, osz, "undef");
        return;
    }
    switch (e->type) {
        case Natural: {
            char buf[128];
            strip_nat(e->value, buf, sizeof buf);
            int t = NT(g);
            snprintf(out, osz, "%%t%d", t);
            *ty = SEM_INT;
            printf("  %s = add i32 0, %s\n", out, buf);
            return;
        }
        case Decimal: {
            char raw[256];
            strip_dec(e->value, raw, sizeof raw);
            char buf[256];
            llvm_double_lit(raw, buf, sizeof buf);
            int t = NT(g);
            snprintf(out, osz, "%%t%d", t);
            *ty = SEM_DOUBLE;
            printf("  %s = fadd double 0.0, %s\n", out, buf);
            return;
        }
        case BoolLit: {
            int t = NT(g);
            snprintf(out, osz, "%%t%d", t);
            *ty = SEM_BOOL;
            if (!strcmp(e->value, "true"))
                printf("  %s = add i1 0, 1\n", out);
            else
                printf("  %s = add i1 0, 0\n", out);
            return;
        }
        case StrLit: {
            char inner[4096];
            str_in(e->value, inner, sizeof inner);
            int sid = StrUid(g, inner);
            int t = NT(g);
            snprintf(out, osz, "%%t%d", t);
            *ty = SEM_STRING;
            int N = (sid >= 0 && sid < 512) ? g->str_sz[sid] : (int)strlen(inner) + 1;
            printf("  %s = getelementptr inbounds [%d x i8], ptr @.gs%d, i32 0, i32 0\n", out, N, sid);
            return;
        }
        case Identifier:
            LoadVar(g, stk, dep, e->value, out, osz, ty);
            return;
        case Length: {
            char ln[64], pr[64];
            SaLenPtr(g, stk, dep, e->children[0]->value, ln, pr, sizeof ln);
            int t = NT(g);
            snprintf(out, osz, "%%t%d", t);
            *ty = SEM_INT;
            printf("  %s = add i32 %s, 0\n", out, ln);
            return;
        }
        default:
            break;
    }

    if (e->type == ParseArgs) {
        char ix[64];
        SemType it;
        cg_expr(g, stk, dep, e->children[1], ix, sizeof ix, &it);
        char ln[64], pr[64];
        SaLenPtr(g, stk, dep, e->children[0]->value, ln, pr, sizeof ln);
        int gep = NT(g), ld = NT(g), at = NT(g);
        snprintf(out, osz, "%%t%d", at);
        *ty = SEM_INT;
        printf("  %%t%d = getelementptr inbounds ptr, ptr %s, i32 %s\n", gep, pr, ix);
        printf("  %%t%d = load ptr, ptr %%t%d\n", ld, gep);
        printf("  %s = call i32 @atoi(ptr %%t%d)\n", out, ld);
        return;
    }

    if (e->type == Not) {
        char v[64];
        SemType vt;
        cg_expr(g, stk, dep, e->children[0], v, sizeof v, &vt);
        int t = NT(g);
        snprintf(out, osz, "%%t%d", t);
        *ty = SEM_BOOL;
        printf("  %s = xor i1 %s, true\n", out, v);
        return;
    }

    if (e->type == Minus || e->type == Plus) {
        char v[64];
        SemType vt;
        cg_expr(g, stk, dep, e->children[0], v, sizeof v, &vt);
        int t = NT(g);
        snprintf(out, osz, "%%t%d", t);
        *ty = vt;
        if (vt == SEM_INT && e->type == Minus)
            printf("  %s = sub i32 0, %s\n", out, v);
        else if (vt == SEM_DOUBLE && e->type == Minus)
            printf("  %s = fneg double %s\n", out, v);
        else if (vt == SEM_INT)
            printf("  %s = add i32 %s, 0\n", out, v);
        else
            printf("  %s = fadd double %s, 0.0\n", out, v);
        return;
    }

    if (e->type == Assign) {
        char rv[64];
        SemType rt;
        cg_expr(g, stk, dep, e->children[1], rv, sizeof rv, &rt);
        char ptr[64];
        SemType pt;
        cg_lval(g, stk, dep, e->children[0], ptr, sizeof ptr, &pt);
        if (pt == SEM_INT)
            printf("  store i32 %s, ptr %s\n", rv, ptr);
        else if (pt == SEM_DOUBLE) {
            if (rt == SEM_INT) {
                int c = NT(g);
                printf("  %%t%d = sitofp i32 %s to double\n", c, rv);
                printf("  store double %%t%d, ptr %s\n", c, ptr);
            } else
                printf("  store double %s, ptr %s\n", rv, ptr);
        } else if (pt == SEM_BOOL)
            printf("  store i1 %s, ptr %s\n", rv, ptr);
        else if (pt == SEM_STRING)
            printf("  store ptr %s, ptr %s\n", rv, ptr);
        snprintf(out, osz, "%s", rv);
        *ty = rt;
        return;
    }

    if (e->type == Call) {
        struct node *cid = e->children[0];
        Symbol *sym = FindM(g->sr, cid->value, cid->type_note);
        if (!sym) {
            *ty = SEM_UNDEF;
            snprintf(out, osz, "undef");
            return;
        }
        char fname[128];
        Mang(fname, sizeof fname, g->cls, cid->value, sym->params.types, sym->params.count);
        char argsbuf[4096];
        argsbuf[0] = 0;
        int off = 0;
        for (int i = 0; i < sym->params.count; i++) {
            char av[64];
            SemType at;
            cg_expr(g, stk, dep, e->children[i + 1], av, sizeof av, &at);
            SemType want = sym->params.types[i];
            if (want == SEM_DOUBLE && at == SEM_INT) {
                int c = NT(g);
                printf("  %%t%d = sitofp i32 %s to double\n", c, av);
                snprintf(av, sizeof av, "%%t%d", c);
                at = SEM_DOUBLE;
            }
            if (want == SEM_STRING_ARRAY) {
                struct node *argn = e->children[i + 1];
                if (argn->type != Identifier) {
                    *ty = SEM_UNDEF;
                    snprintf(out, osz, "undef");
                    return;
                }
                char ln[64], pr[64];
                SaLenPtr(g, stk, dep, argn->value, ln, pr, sizeof ln);
                off += snprintf(argsbuf + off, sizeof argsbuf - off, "%si32 %s, ptr %s", i ? ", " : "", ln, pr);
            } else {
                off += snprintf(argsbuf + off, sizeof argsbuf - off, "%s%s %s", i ? ", " : "", LT(want), av);
            }
        }
        *ty = sym->type;
        if (sym->type == SEM_VOID) {
            printf("  call void @%s(%s)\n", fname, argsbuf);
            snprintf(out, osz, "undef");
            return;
        }
        int t = NT(g);
        snprintf(out, osz, "%%t%d", t);
        printf("  %s = call %s @%s(%s)\n", out, LT(sym->type), fname, argsbuf);
        return;
    }

    if (e->type == Add || e->type == Sub || e->type == Mul) {
        char L[64], R[64];
        SemType lt, rt;
        cg_expr(g, stk, dep, e->children[0], L, sizeof L, &lt);
        cg_expr(g, stk, dep, e->children[1], R, sizeof R, &rt);
        SemType pr;
        Prom(&lt, &rt, &pr);
        int u = NT(g);
        snprintf(out, osz, "%%t%d", u);
        *ty = pr;
        if (pr == SEM_DOUBLE) {
            int Ld = lt == SEM_INT ? NT(g) : 0, Rd = rt == SEM_INT ? NT(g) : 0;
            char ls[64], rs[64];
            strcpy(ls, L);
            strcpy(rs, R);
            if (lt == SEM_INT) {
                printf("  %%t%d = sitofp i32 %s to double\n", Ld, L);
                snprintf(ls, sizeof ls, "%%t%d", Ld);
            }
            if (rt == SEM_INT) {
                printf("  %%t%d = sitofp i32 %s to double\n", Rd, R);
                snprintf(rs, sizeof rs, "%%t%d", Rd);
            }
            if (e->type == Add)
                printf("  %s = fadd double %s, %s\n", out, ls, rs);
            else if (e->type == Sub)
                printf("  %s = fsub double %s, %s\n", out, ls, rs);
            else
                printf("  %s = fmul double %s, %s\n", out, ls, rs);
        } else {
            if (e->type == Add)
                printf("  %s = add i32 %s, %s\n", out, L, R);
            else if (e->type == Sub)
                printf("  %s = sub i32 %s, %s\n", out, L, R);
            else
                printf("  %s = mul i32 %s, %s\n", out, L, R);
        }
        return;
    }
    if (e->type == Div) {
        char L[64], R[64];
        SemType lt, rt;
        cg_expr(g, stk, dep, e->children[0], L, sizeof L, &lt);
        cg_expr(g, stk, dep, e->children[1], R, sizeof R, &rt);
        SemType pr;
        Prom(&lt, &rt, &pr);
        int u = NT(g);
        snprintf(out, osz, "%%t%d", u);
        *ty = pr;
        if (pr == SEM_DOUBLE) {
            int Ld = lt == SEM_INT ? NT(g) : 0, Rd = rt == SEM_INT ? NT(g) : 0;
            char ls[64], rs[64];
            strcpy(ls, L);
            strcpy(rs, R);
            if (lt == SEM_INT) {
                printf("  %%t%d = sitofp i32 %s to double\n", Ld, L);
                snprintf(ls, sizeof ls, "%%t%d", Ld);
            }
            if (rt == SEM_INT) {
                printf("  %%t%d = sitofp i32 %s to double\n", Rd, R);
                snprintf(rs, sizeof rs, "%%t%d", Rd);
            }
            printf("  %s = fdiv double %s, %s\n", out, ls, rs);
        } else
            printf("  %s = sdiv i32 %s, %s\n", out, L, R);
        return;
    }
    if (e->type == Mod) {
        char L[64], R[64];
        SemType lt, rt;
        cg_expr(g, stk, dep, e->children[0], L, sizeof L, &lt);
        cg_expr(g, stk, dep, e->children[1], R, sizeof R, &rt);
        SemType pr;
        Prom(&lt, &rt, &pr);
        int u = NT(g);
        snprintf(out, osz, "%%t%d", u);
        *ty = pr;
        if (pr == SEM_DOUBLE) {
            int Ld = lt == SEM_INT ? NT(g) : 0, Rd = rt == SEM_INT ? NT(g) : 0;
            char ls[64], rs[64];
            strcpy(ls, L);
            strcpy(rs, R);
            if (lt == SEM_INT) {
                printf("  %%t%d = sitofp i32 %s to double\n", Ld, L);
                snprintf(ls, sizeof ls, "%%t%d", Ld);
            }
            if (rt == SEM_INT) {
                printf("  %%t%d = sitofp i32 %s to double\n", Rd, R);
                snprintf(rs, sizeof rs, "%%t%d", Rd);
            }
            printf("  %s = frem double %s, %s\n", out, ls, rs);
        } else
            printf("  %s = srem i32 %s, %s\n", out, L, R);
        return;
    }

    if (e->type == Lt || e->type == Gt || e->type == Le || e->type == Ge) {
        char L[64], R[64];
        SemType lt, rt;
        cg_expr(g, stk, dep, e->children[0], L, sizeof L, &lt);
        cg_expr(g, stk, dep, e->children[1], R, sizeof R, &rt);
        SemType pr;
        Prom(&lt, &rt, &pr);
        int u = NT(g);
        snprintf(out, osz, "%%t%d", u);
        *ty = SEM_BOOL;
        if (pr == SEM_DOUBLE) {
            int Ld = lt == SEM_INT ? NT(g) : 0, Rd = rt == SEM_INT ? NT(g) : 0;
            char ls[64], rs[64];
            strcpy(ls, L);
            strcpy(rs, R);
            if (lt == SEM_INT) {
                printf("  %%t%d = sitofp i32 %s to double\n", Ld, L);
                snprintf(ls, sizeof ls, "%%t%d", Ld);
            }
            if (rt == SEM_INT) {
                printf("  %%t%d = sitofp i32 %s to double\n", Rd, R);
                snprintf(rs, sizeof rs, "%%t%d", Rd);
            }
            const char *op = e->type == Lt ? "olt" : e->type == Gt ? "ogt" : e->type == Le ? "ole" : "oge";
            printf("  %s = fcmp %s double %s, %s\n", out, op, ls, rs);
        } else {
            const char *op = e->type == Lt ? "slt" : e->type == Gt ? "sgt" : e->type == Le ? "sle" : "sge";
            printf("  %s = icmp %s i32 %s, %s\n", out, op, L, R);
        }
        return;
    }

    if (e->type == Eq || e->type == Ne) {
        char L[64], R[64];
        SemType lt, rt;
        cg_expr(g, stk, dep, e->children[0], L, sizeof L, &lt);
        cg_expr(g, stk, dep, e->children[1], R, sizeof R, &rt);
        int u = NT(g);
        snprintf(out, osz, "%%t%d", u);
        *ty = SEM_BOOL;
        if (lt == SEM_BOOL && rt == SEM_BOOL) {
            if (e->type == Eq)
                printf("  %s = icmp eq i1 %s, %s\n", out, L, R);
            else
                printf("  %s = icmp ne i1 %s, %s\n", out, L, R);
            return;
        }
        if (lt == SEM_STRING && rt == SEM_STRING) {
            int c = NT(g);
            printf("  %%t%d = call i32 @strcmp(ptr %s, ptr %s)\n", c, L, R);
            if (e->type == Eq)
                printf("  %s = icmp eq i32 %%t%d, 0\n", out, c);
            else
                printf("  %s = icmp ne i32 %%t%d, 0\n", out, c);
        } else {
            SemType pr;
            Prom(&lt, &rt, &pr);
            if (pr == SEM_DOUBLE) {
                int Ld = lt == SEM_INT ? NT(g) : 0, Rd = rt == SEM_INT ? NT(g) : 0;
                char ls[64], rs[64];
                strcpy(ls, L);
                strcpy(rs, R);
                if (lt == SEM_INT) {
                    printf("  %%t%d = sitofp i32 %s to double\n", Ld, L);
                    snprintf(ls, sizeof ls, "%%t%d", Ld);
                }
                if (rt == SEM_INT) {
                    printf("  %%t%d = sitofp i32 %s to double\n", Rd, R);
                    snprintf(rs, sizeof rs, "%%t%d", Rd);
                }
                if (e->type == Eq)
                    printf("  %s = fcmp oeq double %s, %s\n", out, ls, rs);
                else
                    printf("  %s = fcmp one double %s, %s\n", out, ls, rs);
            } else {
                if (e->type == Eq)
                    printf("  %s = icmp eq i32 %s, %s\n", out, L, R);
                else
                    printf("  %s = icmp ne i32 %s, %s\n", out, L, R);
            }
        }
        return;
    }

    if (e->type == Xor) {
        char L[64], R[64];
        SemType lt, rt;
        cg_expr(g, stk, dep, e->children[0], L, sizeof L, &lt);
        cg_expr(g, stk, dep, e->children[1], R, sizeof R, &rt);
        int u = NT(g);
        snprintf(out, osz, "%%t%d", u);
        if (lt == SEM_BOOL && rt == SEM_BOOL) {
            *ty = SEM_BOOL;
            printf("  %s = xor i1 %s, %s\n", out, L, R);
        } else {
            *ty = SEM_INT;
            printf("  %s = xor i32 %s, %s\n", out, L, R);
        }
        return;
    }

    if (e->type == Lshift || e->type == Rshift) {
        char L[64], R[64];
        SemType lt, rt;
        cg_expr(g, stk, dep, e->children[0], L, sizeof L, &lt);
        cg_expr(g, stk, dep, e->children[1], R, sizeof R, &rt);
        int u = NT(g);
        snprintf(out, osz, "%%t%d", u);
        *ty = SEM_INT;
        if (e->type == Lshift)
            printf("  %s = shl i32 %s, %s\n", out, L, R);
        else
            printf("  %s = ashr i32 %s, %s\n", out, L, R);
        return;
    }

    /* Short-circuit && : skip rhs if lhs is false */
    if (e->type == And) {
        char lv[64]; SemType lt;
        cg_expr(g, stk, dep, e->children[0], lv, sizeof lv, &lt);
        int lr = NL(g), lm = NL(g), le = NL(g);
        printf("  br i1 %s, label %%L%d, label %%L%d\n", lv, lr, lm);
        EL(g, lr);
        char rv[64]; SemType rtt;
        cg_expr(g, stk, dep, e->children[1], rv, sizeof rv, &rtt);
        char rpred[48]; phi_pred(g, rpred, sizeof rpred);
        printf("  br label %%L%d\n", le);
        EL(g, lm);
        printf("  br label %%L%d\n", le);
        EL(g, le);
        int ld = NT(g);
        snprintf(out, osz, "%%t%d", ld);
        *ty = SEM_BOOL;
        printf("  %s = phi i1 [ 0, %%L%d ], [ %s, %s ]\n", out, lm, rv, rpred);
        return;
    }

    /* Short-circuit || : skip rhs if lhs is true */
    if (e->type == Or) {
        char lv[64]; SemType lt;
        cg_expr(g, stk, dep, e->children[0], lv, sizeof lv, &lt);
        int l_true = NL(g), l_rhs = NL(g), l_end = NL(g);
        printf("  br i1 %s, label %%L%d, label %%L%d\n", lv, l_true, l_rhs);
        EL(g, l_true);
        printf("  br label %%L%d\n", l_end);
        EL(g, l_rhs);
        char rv[64]; SemType rt;
        cg_expr(g, stk, dep, e->children[1], rv, sizeof rv, &rt);
        char rpred[48]; phi_pred(g, rpred, sizeof rpred);
        printf("  br label %%L%d\n", l_end);
        EL(g, l_end);
        int ld = NT(g);
        snprintf(out, osz, "%%t%d", ld);
        *ty = SEM_BOOL;
        printf("  %s = phi i1 [ 1, %%L%d ], [ %s, %s ]\n", out, l_true, rv, rpred);
        return;
    }

    *ty = SEM_UNDEF;
    snprintf(out, osz, "undef");
}

static void cg_lval(CG *g, VMap *stk, int dep, struct node *lhs, char *ptrn, size_t osz, SemType *pty) {
    if (lhs->type != Identifier) {
        *pty = SEM_UNDEF;
        snprintf(ptrn, osz, "null");
        return;
    }
    int lay = 0;
    int ix = VmIx(stk, dep, lhs->value, &lay);
    if (ix >= 0 && !stk[lay].isa[ix]) {
        *pty = stk[lay].ty[ix];
        snprintf(ptrn, osz, "%s", stk[lay].sl[ix]);
        return;
    }
    Symbol *f = st_find_kind(g->sr->class_table, lhs->value, SYM_FIELD);
    if (f) {
        *pty = f->type;
        snprintf(ptrn, osz, "%s", Fld(g, lhs->value));
        return;
    }
    *pty = SEM_UNDEF;
    snprintf(ptrn, osz, "null");
}

static void cg_print(CG *g, VMap *stk, int dep, struct node *stmt) {
    SemType pt = SEM_UNDEF;
    char val[64];
    cg_expr(g, stk, dep, stmt->children[0], val, sizeof val, &pt);
    if (pt == SEM_INT) {
        int fp = emit_gep_global(g, "@.fmt_d", 3);
        printf("  call i32 (ptr, ...) @printf(ptr %%t%d, i32 %s)\n", fp, val);
    } else if (pt == SEM_DOUBLE) {
        int fp = emit_gep_global(g, "@.fmt_e", 6);
        printf("  call i32 (ptr, ...) @printf(ptr %%t%d, double %s)\n", fp, val);
    } else if (pt == SEM_STRING) {
        int fp = emit_gep_global(g, "@.fmt_s", 3);
        printf("  call i32 (ptr, ...) @printf(ptr %%t%d, ptr %s)\n", fp, val);
    } else if (pt == SEM_BOOL) {
        int lt = NL(g), lf = NL(g), le = NL(g);
        printf("  br i1 %s, label %%L%d, label %%L%d\n", val, lt, lf);
        EL(g, lt);
        int fa = emit_gep_global(g, "@.fmt_s", 3);
        int sa = emit_gep_global(g, "@.str_true", 5);
        printf("  call i32 (ptr, ...) @printf(ptr %%t%d, ptr %%t%d)\n", fa, sa);
        printf("  br label %%L%d\n", le);
        EL(g, lf);
        fa = emit_gep_global(g, "@.fmt_s", 3);
        sa = emit_gep_global(g, "@.str_false", 6);
        printf("  call i32 (ptr, ...) @printf(ptr %%t%d, ptr %%t%d)\n", fa, sa);
        printf("  br label %%L%d\n", le);
        EL(g, le);
    }
}

static void cg_stmt(CG *g, VMap *stk, int *dep, struct node *s) {
    if (!s) return;
    switch (s->type) {
        case VarDecl: {
            SemType vt = NdSem(s->children[0]->type);
            struct node *id = s->children[1];
            int al = NT(g);
            char slot[48];
            snprintf(slot, sizeof slot, "%%v%d", al);
            printf("  %s = alloca %s\n", slot, LT(vt));
            if (vt == SEM_INT)
                printf("  store i32 0, ptr %s\n", slot);
            else if (vt == SEM_DOUBLE)
                printf("  store double 0.0, ptr %s\n", slot);
            else if (vt == SEM_BOOL)
                printf("  store i1 0, ptr %s\n", slot);
            else if (vt == SEM_STRING)
                printf("  store ptr null, ptr %s\n", slot);
            VmPut(stk + *dep, id->value, slot, vt, 0, NULL, NULL);
            return;
        }
        case Block:
        case MethodBody:
            (*dep)++;
            VmInit(stk + *dep);
            for (int i = 0; i < s->num_children; i++) {
                struct node *c = s->children[i];
                if (c->type == VarDecl)
                    cg_stmt(g, stk, dep, c);
            }
            for (int i = 0; i < s->num_children; i++) {
                struct node *c = s->children[i];
                if (c->type != VarDecl)
                    cg_stmt(g, stk, dep, c);
            }
            VmClr(stk + *dep);
            (*dep)--;
            return;
        case If: {
            char cv[64];
            SemType ct;
            cg_expr(g, stk, *dep, s->children[0], cv, sizeof cv, &ct);
            int th = NL(g), el = NL(g), mg = NL(g);
            int hasElse = s->num_children > 2 && s->children[2] != NULL &&
                          !(s->children[2]->type == Block && s->children[2]->num_children == 0);
            if (hasElse)
                printf("  br i1 %s, label %%L%d, label %%L%d\n", cv, th, el);
            else
                printf("  br i1 %s, label %%L%d, label %%L%d\n", cv, th, mg);
            EL(g, th);
            cg_stmt(g, stk, dep, s->children[1]);
            printf("  br label %%L%d\n", mg);
            if (hasElse) {
                EL(g, el);
                cg_stmt(g, stk, dep, s->children[2]);
                printf("  br label %%L%d\n", mg);
            }
            EL(g, mg);
            return;
        }
        case While: {
            int head = NL(g), body = NL(g), end = NL(g);
            printf("  br label %%L%d\n", head);
            EL(g, head);
            char cv[64];
            SemType ct;
            cg_expr(g, stk, *dep, s->children[0], cv, sizeof cv, &ct);
            printf("  br i1 %s, label %%L%d, label %%L%d\n", cv, body, end);
            EL(g, body);
            cg_stmt(g, stk, dep, s->children[1]);
            printf("  br label %%L%d\n", head);
            EL(g, end);
            return;
        }
        case Return: {
            Symbol *ret = st_find(g->mt->table, "return");
            SemType rt = ret ? ret->type : SEM_VOID;
            if (s->num_children == 0) {
                if (rt == SEM_VOID)
                    printf("  ret void\n");
                g->blk_open = 0;
                return;
            }
            char rv[64];
            SemType vt;
            cg_expr(g, stk, *dep, s->children[0], rv, sizeof rv, &vt);
            if (rt == SEM_INT)
                printf("  ret i32 %s\n", rv);
            else if (rt == SEM_DOUBLE) {
                if (vt == SEM_INT) {
                    int c = NT(g);
                    printf("  %%t%d = sitofp i32 %s to double\n", c, rv);
                    printf("  ret double %%t%d\n", c);
                } else
                    printf("  ret double %s\n", rv);
            } else if (rt == SEM_BOOL)
                printf("  ret i1 %s\n", rv);
            else if (rt == SEM_STRING)
                printf("  ret ptr %s\n", rv);
            g->blk_open = 0;
            return;
        }
        case Print:
            cg_print(g, stk, *dep, s);
            return;
        case Assign:
        case Call:
        case ParseArgs: {
            char tmp[64];
            SemType tt;
            cg_expr(g, stk, *dep, s, tmp, sizeof tmp, &tt);
            return;
        }
        default:
            return;
    }
}

static void cg_body(CG *g, VMap *stk, int *dep, struct node *body) {
    if (!body) return;
    for (int i = 0; i < body->num_children; i++) {
        struct node *c = body->children[i];
        if (c->type == VarDecl)
            cg_stmt(g, stk, dep, c);
    }
    for (int i = 0; i < body->num_children; i++) {
        struct node *c = body->children[i];
        if (c->type != VarDecl)
            cg_stmt(g, stk, dep, c);
    }
}

static int tree_has_return(struct node *n) {
    if (!n) return 0;
    if (n->type == Return) return 1;
    for (int i = 0; i < n->num_children; i++)
        if (tree_has_return(n->children[i])) return 1;
    return 0;
}

static void emit_method(CG *g, MethodTables *mt) {
    g->mt = mt;
    g->tmp = 0;
    g->lbl = 0;
    struct node *md = mt->method_node;
    struct node *hdr = md->children[0];
    struct node *ret = hdr->children[0];
    struct node *mid = hdr->children[1];
    struct node *params = hdr->children[2];
    struct node *body = md->children[1];
    SemType rtt = NdSem(ret->type);

    char fname[160];
    Mang(fname, sizeof fname, g->cls, mid->value, mt->param_types, mt->nparams);

    if (rtt == SEM_VOID)
        printf("define void @%s(", fname);
    else
        printf("define %s @%s(", LT(rtt), fname);

    VMap stk[32];
    int dep = 0;
    VmInit(stk);

    int pc = 0;
    for (int i = 0; i < params->num_children; i++) {
        struct node *pd = params->children[i];
        struct node *pty = pd->children[0];
        SemType pt = NdSem(pty->type);
        if (pc++) printf(", ");
        /* String[] formal = i32 length + ptr (matches argv and .length) */
        if (pt == SEM_STRING_ARRAY) {
            printf("i32 %%p%d_len, ptr %%p%d_ptr", i, i);
        } else {
            printf("%s %%p%d", LT(pt), i);
        }
    }
    printf(") {\nentry:\n");
    g->cur_lbl = 0;
    g->blk_open = 1;

    pc = 0;
    for (int i = 0; i < params->num_children; i++) {
        struct node *pd = params->children[i];
        struct node *pty = pd->children[0];
        struct node *pid = pd->children[1];
        SemType pt = NdSem(pty->type);
        if (pt == SEM_STRING_ARRAY) {
            char lslot[48], pslot[48];
            int a = NT(g), b = NT(g);
            snprintf(lslot, sizeof lslot, "%%v%d", a);
            snprintf(pslot, sizeof pslot, "%%v%d", b);
            printf("  %s = alloca i32\n", lslot);
            printf("  %s = alloca ptr\n", pslot);
            printf("  store i32 %%p%d_len, ptr %s\n", i, lslot);
            printf("  store ptr %%p%d_ptr, ptr %s\n", i, pslot);
            VmPut(stk + dep, pid->value, NULL, SEM_STRING_ARRAY, 1, lslot, pslot);
        } else {
            char slot[48];
            int a = NT(g);
            snprintf(slot, sizeof slot, "%%v%d", a);
            printf("  %s = alloca %s\n", slot, LT(pt));
            printf("  store %s %%p%d, ptr %s\n", LT(pt), i, slot);
            VmPut(stk + dep, pid->value, slot, pt, 0, NULL, NULL);
        }
        pc++;
    }

    cg_body(g, stk, &dep, body);

    if (g->blk_open) {
        if (rtt == SEM_VOID)
            printf("  ret void\n");
        else
            printf("  unreachable\n");
    }

    printf("}\n");
    VmClr(stk);
}

/* Emit globals, methods, then C main() that calls ju_*_main(argc-1, argv+1) */
void codegen_program(struct node *root, SemanticResult *sr) {
    if (!root || root->type != Program) return;
    CG g;
    memset(&g, 0, sizeof g);
    g.sr = sr;
    g.cls = root->children[0]->value;
    RT();
    ScanStr(&g, root);
    Fields(&g, root);
    for (MethodTables *m = sr->methods; m; m = m->next)
        emit_method(&g, m);

    MethodTables *entry = NULL;
    for (MethodTables *m = sr->methods; m; m = m->next) {
        if (strcmp(m->name, "main") == 0 && m->nparams == 1 && m->param_types[0] == SEM_STRING_ARRAY) {
            entry = m;
            break;
        }
    }
    if (!entry) return;

    char wname[128];
    Mang(wname, sizeof wname, g.cls, "main", entry->param_types, entry->nparams);
    printf("define i32 @main(i32 %%argc, ptr %%argv) {\nentry:\n");
    printf("  %%mw1 = add i32 0, 1\n");
    printf("  %%mw2 = sub i32 %%argc, %%mw1\n");
    printf("  %%mw3 = getelementptr inbounds ptr, ptr %%argv, i32 1\n");
    printf("  call void @%s(i32 %%mw2, ptr %%mw3)\n", wname);
    printf("  ret i32 0\n}\n");
}
