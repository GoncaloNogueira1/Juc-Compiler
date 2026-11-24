#ifndef AST_H
#define AST_H

typedef enum {
    Program,
    FieldDecl, VarDecl,
    MethodDecl, MethodHeader, MethodParams, ParamDecl, MethodBody,
    Block, If, While, Return, Call, Print, ParseArgs, Assign,
    Or, And, Xor, Eq, Ne, Lt, Gt, Le, Ge,
    Add, Sub, Mul, Div, Mod, Lshift, Rshift,
    Not, Minus, Plus, Length,
    Bool, BoolLit, Double, Decimal, Identifier, Int, Natural, StrLit,
    StringArray, Void,
    AUX /* internal lists; stripped before printing */
} NodeType;

typedef enum {
    SEM_UNDEF,
    SEM_BOOL,
    SEM_INT,
    SEM_DOUBLE,
    SEM_STRING,
    SEM_STRING_ARRAY,
    SEM_VOID
} SemType;

struct node {
    NodeType type;
    char *value;
    SemType sem_type;
    unsigned char annotate; /* set if -s should print a type on this node */
    char *type_note;        /* call id: which overload, e.g. (int,double) */
    int line;
    int col;
    struct node **children;
    int num_children;
    int cap_children;
};

struct node *new_node(NodeType type, char *value);
void set_node_loc(struct node *n, int line, int col);
void set_node_type(struct node *n, SemType t);
const char *sem_type_name(SemType t);
void add_child(struct node *parent, struct node *child);
void show_node(struct node *n, int depth);
void show_node_annotated(struct node *n, int depth);
void deallocate_node(struct node *n);
void remove_aux(struct node *n);

const char *node_type_name(NodeType t);

#endif