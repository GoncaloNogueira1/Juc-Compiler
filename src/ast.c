#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ast.h"

#define INIT_CAP 4

struct node *new_node(NodeType type, char *value) {
    struct node *n = malloc(sizeof(struct node));
    n->type = type;
    n->value = value ? strdup(value) : NULL;
    n->sem_type = SEM_UNDEF;
    n->annotate = 0;
    n->type_note = NULL;
    n->line = 0;
    n->col = 0;
    n->children = NULL;
    n->num_children = 0;
    n->cap_children = 0;
    return n;
}

void set_node_loc(struct node *n, int line, int col) {
    if (!n) return;
    n->line = line;
    n->col = col;
}

void set_node_type(struct node *n, SemType t) {
    if (!n) return;
    n->sem_type = t;
}

const char *sem_type_name(SemType t) {
    switch (t) {
        case SEM_BOOL:         return "boolean";
        case SEM_INT:          return "int";
        case SEM_DOUBLE:       return "double";
        case SEM_STRING:       return "String";
        case SEM_STRING_ARRAY: return "String[]";
        case SEM_VOID:         return "void";
        case SEM_UNDEF:        return "undef";
        default:               return "undef";
    }
}

void add_child(struct node *parent, struct node *child) {
    if (!parent || !child) return;
    if (parent->num_children == parent->cap_children) {
        int newcap = parent->cap_children == 0 ? INIT_CAP : parent->cap_children * 2;
        parent->children = realloc(parent->children, newcap * sizeof(struct node *));
        parent->cap_children = newcap;
    }
    parent->children[parent->num_children++] = child;
}

const char *node_type_name(NodeType t) {
    switch (t) {
        case Program:     return "Program";
        case FieldDecl:   return "FieldDecl";
        case VarDecl:     return "VarDecl";
        case MethodDecl:  return "MethodDecl";
        case MethodHeader:return "MethodHeader";
        case MethodParams:return "MethodParams";
        case ParamDecl:   return "ParamDecl";
        case MethodBody:  return "MethodBody";
        case Block:       return "Block";
        case If:          return "If";
        case While:       return "While";
        case Return:      return "Return";
        case Call:        return "Call";
        case Print:       return "Print";
        case ParseArgs:   return "ParseArgs";
        case Assign:      return "Assign";
        case Or:          return "Or";
        case And:         return "And";
        case Xor:         return "Xor";
        case Eq:          return "Eq";
        case Ne:          return "Ne";
        case Lt:          return "Lt";
        case Gt:          return "Gt";
        case Le:          return "Le";
        case Ge:          return "Ge";
        case Add:         return "Add";
        case Sub:         return "Sub";
        case Mul:         return "Mul";
        case Div:         return "Div";
        case Mod:         return "Mod";
        case Lshift:      return "Lshift";
        case Rshift:      return "Rshift";
        case Not:         return "Not";
        case Minus:       return "Minus";
        case Plus:        return "Plus";
        case Length:      return "Length";
        case Bool:        return "Bool";
        case BoolLit:     return "BoolLit";
        case Double:      return "Double";
        case Decimal:     return "Decimal";
        case Identifier:  return "Identifier";
        case Int:         return "Int";
        case Natural:     return "Natural";
        case StrLit:      return "StrLit";
        case StringArray: return "StringArray";
        case Void:        return "Void";
        case AUX:         return "AUX";
        default:          return "Unknown";
    }
}

void show_node(struct node *n, int depth) {
    if (!n) return;
    int i;
    for (i = 0; i < depth * 2; i++) putchar('.');
    if (n->value)
        printf("%s(%s)\n", node_type_name(n->type), n->value);
    else
        printf("%s\n", node_type_name(n->type));
    for (i = 0; i < n->num_children; i++)
        show_node(n->children[i], depth + 1);
}

void show_node_annotated(struct node *n, int depth) {
    if (!n) return;
    int i;
    for (i = 0; i < depth * 2; i++) putchar('.');
    if (n->value)
        printf("%s(%s)", node_type_name(n->type), n->value);
    else
        printf("%s", node_type_name(n->type));

    if (n->annotate) {
        printf(" - %s", n->type_note ? n->type_note : sem_type_name(n->sem_type));
    }
    putchar('\n');

    for (i = 0; i < n->num_children; i++)
        show_node_annotated(n->children[i], depth + 1);
}

void deallocate_node(struct node *n) {
    if (!n) return;
    int i;
    for (i = 0; i < n->num_children; i++)
        deallocate_node(n->children[i]);
    free(n->children);
    free(n->value);
    free(n->type_note);
    free(n);
}

void remove_aux(struct node *n) {
    if (!n) return;
    int i, j;
    for (i = 0; i < n->num_children; i++)
        if (n->children[i] && n->children[i]->type != AUX)
            remove_aux(n->children[i]);

    int new_count = 0;
    for (i = 0; i < n->num_children; i++)
        if (n->children[i] && n->children[i]->type == AUX)
            new_count += n->children[i]->num_children;
        else if (n->children[i])
            new_count++;

    struct node **new_children = malloc(new_count * sizeof(struct node *));
    int idx = 0;
    for (i = 0; i < n->num_children; i++) {
        if (n->children[i] && n->children[i]->type == AUX) {
            struct node *aux = n->children[i];
            for (j = 0; j < aux->num_children; j++) {
                new_children[idx++] = aux->children[j];
                remove_aux(aux->children[j]);
            }
            free(aux->children);
            free(aux->value);
            free(aux);
        } else if (n->children[i]) {
            new_children[idx++] = n->children[i];
        }
    }
    free(n->children);
    n->children = new_children;
    n->num_children = idx;
    n->cap_children = new_count;
}