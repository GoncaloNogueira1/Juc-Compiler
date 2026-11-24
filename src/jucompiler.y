%{

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "ast.h"
#include "semantics.h"
#include "codegen.h"

int yylex(void);
void yyerror(char *s);

extern int line;
extern int col;
extern int tok_start_col;
extern char *yytext;

int mode = 0;

struct node *root = NULL;
int syntax_errors = 0;
int semantic_errors = 0;

char last_token_text[4096] = "";

/* %locations: @n -> line/col on the node. Errors, -s. Codegen only if semantics ok. */
static struct node *new_node_at(NodeType type, char *value, int line, int col) {
    struct node *n = new_node(type, value);
    set_node_loc(n, line, col);
    return n;
}

%}

%locations

%union {
    char *lexeme;
    struct node *node_ptr;
}

/* Tokens (no yylval text) */
%token CLASS PUBLIC STATIC VOID INT DOUBLE BOOL STRING
%token LBRACE RBRACE LPAR RPAR LSQ RSQ SEMICOLON COMMA DOTLENGTH
%token IF WHILE RETURN PRINT PARSEINT ARROW

/* Tokens carry lexeme in yylval */
%token <lexeme> IDENTIFIER NATURAL DECIMAL BOOLLIT STRLIT RESERVED

/* Precedence: lines lower in file bind tighter. %left/%right also declare tokens (no duplicate %token). */
%nonassoc LOWER_THAN_ELSE
%nonassoc ELSE
%right ASSIGN
%left OR
%left AND
%left XOR
%left EQ NE
%left LT GT LE GE
%left LSHIFT RSHIFT
%left PLUS MINUS
%left STAR DIV MOD
%right NOT UNARY_PLUS UNARY_MINUS
%left DOTLENGTH LPAR LSQ

/* $$ types for nonterminals */
%type <node_ptr> Program ProgramMember MemberRest FieldIdList VarIdList VarDecl
%type <node_ptr> Type FormalParams ParamList
%type <node_ptr> MethodBody MethodBodyItems
%type <node_ptr> Statement StatementBlock StatementListItems
%type <node_ptr> Expr ExprNoAssign AssignExpr ExprArgs ExprArgList

%%

Program:
    CLASS IDENTIFIER {
        root = new_node(Program, NULL);
        add_child(root, new_node_at(Identifier, $2, @2.first_line, @2.first_column));
        free($2);
    } LBRACE ProgramBody RBRACE {
        $$ = root;
    }
    ;

ProgramBody:
    /* empty */
  | ProgramBody ProgramMember
  | ProgramBody SEMICOLON
  ;

/*
 * FieldDecl and MethodDecl both start with "public static <type> <id>".
 * Choice is clear after <id>: '(' -> method, ',' or ';' -> field list. Handled in MemberRest.
 * void methods: separate rule (fields cannot be void).
 */
ProgramMember:
    PUBLIC STATIC Type IDENTIFIER MemberRest {
        if ($5->type == MethodDecl) {
            struct node *params = $5->children[0];
            struct node *body   = $5->children[1];

            struct node *hdr = new_node(MethodHeader, NULL);
            add_child(hdr, $3);
            add_child(hdr, new_node_at(Identifier, $4, @4.first_line, @4.first_column));
            free($4);
            add_child(hdr, params);

            struct node *md = new_node(MethodDecl, NULL);
            add_child(md, hdr);
            add_child(md, body);
            add_child(root, md);

            free($5->children);
            free($5->value);
            free($5);
        } else {
            struct node *fd = new_node(FieldDecl, NULL);
            add_child(fd, $3);
            add_child(fd, new_node_at(Identifier, $4, @4.first_line, @4.first_column));
            free($4);
            add_child(root, fd);

            /* Identificadores extra */
            int i;
            for (i = 0; i < $5->num_children; i++) {
                struct node *extra = new_node(FieldDecl, NULL);
                add_child(extra, new_node($3->type, $3->value));
                add_child(extra, new_node_at(Identifier, $5->children[i]->value, $5->children[i]->line, $5->children[i]->col));
                deallocate_node($5->children[i]);
                add_child(root, extra);
            }
            free($5->children);
            free($5->value);
            free($5);
        }
        $$ = NULL;
    }
  | PUBLIC STATIC VOID IDENTIFIER LPAR FormalParams RPAR MethodBody {
        struct node *hdr = new_node(MethodHeader, NULL);
        add_child(hdr, new_node(Void, NULL));
        add_child(hdr, new_node_at(Identifier, $4, @4.first_line, @4.first_column));
        free($4);
        add_child(hdr, $6);

        struct node *md = new_node(MethodDecl, NULL);
        add_child(md, hdr);
        add_child(md, $8);
        add_child(root, md);
        $$ = NULL;
    }
  | error SEMICOLON { $$ = NULL; }
  ;

MemberRest:
    FieldIdList SEMICOLON {
        $$ = $1;
    }
  | LPAR FormalParams RPAR MethodBody {
        $$ = new_node(MethodDecl, NULL);
        add_child($$, $2);
        add_child($$, $4);
    }
  ;

Type:
    BOOL   { $$ = new_node(Bool,   NULL); }
  | INT    { $$ = new_node(Int,    NULL); }
  | DOUBLE { $$ = new_node(Double, NULL); }
  ;

FieldIdList:
    /* empty */ { $$ = new_node(AUX, NULL); }
  | FieldIdList COMMA IDENTIFIER {
        struct node *id = new_node_at(AUX, $3, @3.first_line, @3.first_column);
        free($3);
        add_child($1, id);
        $$ = $1;
    }
  ;

VarIdList:
    /* empty */ { $$ = new_node(AUX, NULL); }
  | VarIdList COMMA IDENTIFIER {
        struct node *id = new_node_at(AUX, $3, @3.first_line, @3.first_column);
        free($3);
        add_child($1, id);
        $$ = $1;
    }
  ;

FormalParams:
    /* empty */ {
        $$ = new_node(MethodParams, NULL);
    }
  | STRING LSQ RSQ IDENTIFIER {
        $$ = new_node(MethodParams, NULL);
        struct node *pd = new_node(ParamDecl, NULL);
        add_child(pd, new_node(StringArray, NULL));
        add_child(pd, new_node_at(Identifier, $4, @4.first_line, @4.first_column));
        free($4);
        add_child($$, pd);
    }
  | ParamList {
        $$ = new_node(MethodParams, NULL);
        int i;
        for (i = 0; i < $1->num_children; i++)
            add_child($$, $1->children[i]);
        free($1->children);
        free($1->value);
        free($1);
    }
  ;

ParamList:
    Type IDENTIFIER {
        struct node *pd = new_node(ParamDecl, NULL);
        add_child(pd, $1);
        add_child(pd, new_node_at(Identifier, $2, @2.first_line, @2.first_column));
        free($2);
        $$ = new_node(AUX, NULL);
        add_child($$, pd);
    }
  | ParamList COMMA Type IDENTIFIER {
        struct node *pd = new_node(ParamDecl, NULL);
        add_child(pd, $3);
        add_child(pd, new_node_at(Identifier, $4, @4.first_line, @4.first_column));
        free($4);
        add_child($1, pd);
        $$ = $1;
    }
  ;

MethodBody:
    LBRACE MethodBodyItems RBRACE { $$ = $2; }
    ;

MethodBodyItems:
    /* empty */ {
        $$ = new_node(MethodBody, NULL);
    }
  | MethodBodyItems Statement {
        if ($2) {
            add_child($1, $2);
        }
        $$ = $1;
    }
  | MethodBodyItems VarDecl {
        struct node *aux = $2;
        if (aux) {
            int i;
            for (i = 0; i < aux->num_children; i++)
                add_child($1, aux->children[i]);
            free(aux->children);
            free(aux->value);
            free(aux);
        }
        $$ = $1;
    }
  ;

VarDecl:
    Type IDENTIFIER VarIdList SEMICOLON {
        $$ = new_node(AUX, NULL);

        struct node *vd = new_node(VarDecl, NULL);
        add_child(vd, $1);
        add_child(vd, new_node_at(Identifier, $2, @2.first_line, @2.first_column));
        free($2);
        add_child($$, vd);

        if ($3 && $3->num_children > 0) {
            int i;
            for (i = 0; i < $3->num_children; i++) {
                struct node *extra = new_node(VarDecl, NULL);
                add_child(extra, new_node($1->type, $1->value));
                add_child(extra, new_node_at(Identifier, $3->children[i]->value, $3->children[i]->line, $3->children[i]->col));
                deallocate_node($3->children[i]);
                add_child($$, extra);
            }
        }
        if ($3) {
            free($3->children);
            free($3->value);
            free($3);
        }
    }
    ;

Statement:
    StatementBlock { $$ = $1; }
  /* %prec: dangling else -> inner if */
  | IF LPAR Expr RPAR Statement %prec LOWER_THAN_ELSE {
        $$ = new_node_at(If, NULL, @1.first_line, @1.first_column);
        add_child($$, $3);
        add_child($$, $5 ? $5 : new_node(Block, NULL));
        add_child($$, new_node(Block, NULL));
    }
  | IF LPAR Expr RPAR Statement ELSE Statement {
        $$ = new_node_at(If, NULL, @1.first_line, @1.first_column);
        add_child($$, $3);
        add_child($$, $5 ? $5 : new_node(Block, NULL));
        add_child($$, $7 ? $7 : new_node(Block, NULL));
    }
  | WHILE LPAR Expr RPAR Statement {
        $$ = new_node_at(While, NULL, @1.first_line, @1.first_column);
        add_child($$, $3);
        add_child($$, $5 ? $5 : new_node(Block, NULL));
    }
  | RETURN SEMICOLON {
        $$ = new_node_at(Return, NULL, @1.first_line, @1.first_column);
    }
  | RETURN Expr SEMICOLON {
        $$ = new_node_at(Return, NULL, @1.first_line, @1.first_column);
        add_child($$, $2);
    }
  | PRINT LPAR Expr RPAR SEMICOLON {
        $$ = new_node_at(Print, NULL, @1.first_line, @1.first_column);
        add_child($$, $3);
    }
  | PRINT LPAR STRLIT RPAR SEMICOLON {
        $$ = new_node_at(Print, NULL, @1.first_line, @1.first_column);
        add_child($$, new_node_at(StrLit, $3, @3.first_line, @3.first_column));
        free($3);
    }
  | IDENTIFIER ASSIGN Expr SEMICOLON {
        $$ = new_node_at(Assign, NULL, @2.first_line, @2.first_column);
        add_child($$, new_node_at(Identifier, $1, @1.first_line, @1.first_column));
        free($1);
        add_child($$, $3);
    }
  | IDENTIFIER LPAR ExprArgs RPAR SEMICOLON {
        $$ = new_node_at(Call, NULL, @1.first_line, @1.first_column);
        add_child($$, new_node_at(Identifier, $1, @1.first_line, @1.first_column));
        free($1);
        if ($3) {
            int i;
            for (i = 0; i < $3->num_children; i++)
                add_child($$, $3->children[i]);
            free($3->children);
            free($3->value);
            free($3);
        }
    }
  | IDENTIFIER LPAR error RPAR SEMICOLON {
        $$ = new_node_at(Call, NULL, @1.first_line, @1.first_column);
        add_child($$, new_node_at(Identifier, $1, @1.first_line, @1.first_column));
        free($1);
        syntax_errors++;
    }
  | PARSEINT LPAR IDENTIFIER LSQ Expr RSQ RPAR SEMICOLON {
        $$ = new_node_at(ParseArgs, NULL, @1.first_line, @1.first_column);
        add_child($$, new_node_at(Identifier, $3, @3.first_line, @3.first_column));
        free($3);
        add_child($$, $5);
    }
  | PARSEINT LPAR error RPAR SEMICOLON {
        $$ = new_node_at(ParseArgs, NULL, @1.first_line, @1.first_column);
        syntax_errors++;
    }
  | SEMICOLON { $$ = NULL; }
  | error SEMICOLON { $$ = NULL; syntax_errors++; }
  ;

StatementBlock:
    LBRACE StatementListItems RBRACE {
        struct node *blk = $2;
        if (blk->num_children == 0) {
            free(blk->children);
            free(blk->value);
            free(blk);
            $$ = NULL;
        } else if (blk->num_children == 1) {
            struct node *single = blk->children[0];
            free(blk->children);
            free(blk->value);
            free(blk);
            $$ = single;
        } else {
            blk->type = Block;
            $$ = blk;
        }
    }
    ;

StatementListItems:
    /* empty */ { $$ = new_node(AUX, NULL); }
  | StatementListItems Statement {
        if ($2) add_child($1, $2);
        $$ = $1;
    }
  ;

/* ASSIGN vs '(' vs plain id: next token disambiguates */
Expr:
    Expr OR     Expr { $$ = new_node_at(Or,     NULL, @2.first_line, @2.first_column); add_child($$,$1); add_child($$,$3); }
  | Expr AND    Expr { $$ = new_node_at(And,    NULL, @2.first_line, @2.first_column); add_child($$,$1); add_child($$,$3); }
  | Expr XOR    Expr { $$ = new_node_at(Xor,    NULL, @2.first_line, @2.first_column); add_child($$,$1); add_child($$,$3); }
  | Expr EQ     Expr { $$ = new_node_at(Eq,     NULL, @2.first_line, @2.first_column); add_child($$,$1); add_child($$,$3); }
  | Expr NE     Expr { $$ = new_node_at(Ne,     NULL, @2.first_line, @2.first_column); add_child($$,$1); add_child($$,$3); }
  | Expr LT     Expr { $$ = new_node_at(Lt,     NULL, @2.first_line, @2.first_column); add_child($$,$1); add_child($$,$3); }
  | Expr GT     Expr { $$ = new_node_at(Gt,     NULL, @2.first_line, @2.first_column); add_child($$,$1); add_child($$,$3); }
  | Expr LE     Expr { $$ = new_node_at(Le,     NULL, @2.first_line, @2.first_column); add_child($$,$1); add_child($$,$3); }
  | Expr GE     Expr { $$ = new_node_at(Ge,     NULL, @2.first_line, @2.first_column); add_child($$,$1); add_child($$,$3); }
  | Expr LSHIFT Expr { $$ = new_node_at(Lshift, NULL, @2.first_line, @2.first_column); add_child($$,$1); add_child($$,$3); }
  | Expr RSHIFT Expr { $$ = new_node_at(Rshift, NULL, @2.first_line, @2.first_column); add_child($$,$1); add_child($$,$3); }
  | Expr PLUS   Expr { $$ = new_node_at(Add,    NULL, @2.first_line, @2.first_column); add_child($$,$1); add_child($$,$3); }
  | Expr MINUS  Expr { $$ = new_node_at(Sub,    NULL, @2.first_line, @2.first_column); add_child($$,$1); add_child($$,$3); }
  | Expr STAR   Expr { $$ = new_node_at(Mul,    NULL, @2.first_line, @2.first_column); add_child($$,$1); add_child($$,$3); }
  | Expr DIV    Expr { $$ = new_node_at(Div,    NULL, @2.first_line, @2.first_column); add_child($$,$1); add_child($$,$3); }
  | Expr MOD    Expr { $$ = new_node_at(Mod,    NULL, @2.first_line, @2.first_column); add_child($$,$1); add_child($$,$3); }
  | Expr DOTLENGTH {
        $$ = new_node_at(Length, NULL, @2.first_line, @2.first_column);
        add_child($$, $1);
    }
  | MINUS Expr %prec UNARY_MINUS { $$ = new_node_at(Minus, NULL, @1.first_line, @1.first_column); add_child($$,$2); }
  | PLUS  Expr %prec UNARY_PLUS  { $$ = new_node_at(Plus,  NULL, @1.first_line, @1.first_column); add_child($$,$2); }
  | NOT   Expr                   { $$ = new_node_at(Not,   NULL, @1.first_line, @1.first_column); add_child($$,$2); }
  | LPAR Expr RPAR  { $$ = $2; }
  | LPAR error RPAR { $$ = NULL; syntax_errors++; }
  | AssignExpr { $$ = $1; }
  | IDENTIFIER LPAR ExprArgs RPAR {
        $$ = new_node_at(Call, NULL, @1.first_line, @1.first_column);
        add_child($$, new_node_at(Identifier, $1, @1.first_line, @1.first_column));
        free($1);
        if ($3) {
            int i;
            for (i = 0; i < $3->num_children; i++)
                add_child($$, $3->children[i]);
            free($3->children);
            free($3->value);
            free($3);
        }
    }
  | IDENTIFIER LPAR error RPAR {
        $$ = new_node_at(Call, NULL, @1.first_line, @1.first_column);
        add_child($$, new_node_at(Identifier, $1, @1.first_line, @1.first_column));
        free($1);
        syntax_errors++;
    }
  | PARSEINT LPAR IDENTIFIER LSQ Expr RSQ RPAR {
        $$ = new_node_at(ParseArgs, NULL, @1.first_line, @1.first_column);
        add_child($$, new_node_at(Identifier, $3, @3.first_line, @3.first_column));
        free($3);
        add_child($$, $5);
    }
  | PARSEINT LPAR error RPAR {
        $$ = new_node_at(ParseArgs, NULL, @1.first_line, @1.first_column);
        syntax_errors++;
    }
  | NATURAL    { $$ = new_node_at(Natural,    $1, @1.first_line, @1.first_column); free($1); }
  | DECIMAL    { $$ = new_node_at(Decimal,    $1, @1.first_line, @1.first_column); free($1); }
  | BOOLLIT    { $$ = new_node_at(BoolLit,   $1, @1.first_line, @1.first_column); free($1); }
  | IDENTIFIER { $$ = new_node_at(Identifier, $1, @1.first_line, @1.first_column); free($1); }
  ;

ExprNoAssign:
    ExprNoAssign OR     ExprNoAssign { $$ = new_node_at(Or,     NULL, @2.first_line, @2.first_column); add_child($$,$1); add_child($$,$3); }
  | ExprNoAssign AND    ExprNoAssign { $$ = new_node_at(And,    NULL, @2.first_line, @2.first_column); add_child($$,$1); add_child($$,$3); }
  | ExprNoAssign XOR    ExprNoAssign { $$ = new_node_at(Xor,    NULL, @2.first_line, @2.first_column); add_child($$,$1); add_child($$,$3); }
  | ExprNoAssign EQ     ExprNoAssign { $$ = new_node_at(Eq,     NULL, @2.first_line, @2.first_column); add_child($$,$1); add_child($$,$3); }
  | ExprNoAssign NE     ExprNoAssign { $$ = new_node_at(Ne,     NULL, @2.first_line, @2.first_column); add_child($$,$1); add_child($$,$3); }
  | ExprNoAssign LT     ExprNoAssign { $$ = new_node_at(Lt,     NULL, @2.first_line, @2.first_column); add_child($$,$1); add_child($$,$3); }
  | ExprNoAssign GT     ExprNoAssign { $$ = new_node_at(Gt,     NULL, @2.first_line, @2.first_column); add_child($$,$1); add_child($$,$3); }
  | ExprNoAssign LE     ExprNoAssign { $$ = new_node_at(Le,     NULL, @2.first_line, @2.first_column); add_child($$,$1); add_child($$,$3); }
  | ExprNoAssign GE     ExprNoAssign { $$ = new_node_at(Ge,     NULL, @2.first_line, @2.first_column); add_child($$,$1); add_child($$,$3); }
  | ExprNoAssign LSHIFT ExprNoAssign { $$ = new_node_at(Lshift, NULL, @2.first_line, @2.first_column); add_child($$,$1); add_child($$,$3); }
  | ExprNoAssign RSHIFT ExprNoAssign { $$ = new_node_at(Rshift, NULL, @2.first_line, @2.first_column); add_child($$,$1); add_child($$,$3); }
  | ExprNoAssign PLUS   ExprNoAssign { $$ = new_node_at(Add,    NULL, @2.first_line, @2.first_column); add_child($$,$1); add_child($$,$3); }
  | ExprNoAssign MINUS  ExprNoAssign { $$ = new_node_at(Sub,    NULL, @2.first_line, @2.first_column); add_child($$,$1); add_child($$,$3); }
  | ExprNoAssign STAR   ExprNoAssign { $$ = new_node_at(Mul,    NULL, @2.first_line, @2.first_column); add_child($$,$1); add_child($$,$3); }
  | ExprNoAssign DIV    ExprNoAssign { $$ = new_node_at(Div,    NULL, @2.first_line, @2.first_column); add_child($$,$1); add_child($$,$3); }
  | ExprNoAssign MOD    ExprNoAssign { $$ = new_node_at(Mod,    NULL, @2.first_line, @2.first_column); add_child($$,$1); add_child($$,$3); }
  | ExprNoAssign DOTLENGTH { $$ = new_node_at(Length, NULL, @2.first_line, @2.first_column); add_child($$, $1); }
  | MINUS ExprNoAssign %prec UNARY_MINUS { $$ = new_node_at(Minus, NULL, @1.first_line, @1.first_column); add_child($$,$2); }
  | PLUS  ExprNoAssign %prec UNARY_PLUS  { $$ = new_node_at(Plus,  NULL, @1.first_line, @1.first_column); add_child($$,$2); }
  | NOT   ExprNoAssign                   { $$ = new_node_at(Not,   NULL, @1.first_line, @1.first_column); add_child($$,$2); }
  | LPAR AssignExpr RPAR   { $$ = $2; }
  | LPAR ExprNoAssign RPAR { $$ = $2; }
  | LPAR error RPAR        { $$ = NULL; syntax_errors++; }
  | IDENTIFIER LPAR ExprArgs RPAR {
        $$ = new_node_at(Call, NULL, @1.first_line, @1.first_column);
        add_child($$, new_node_at(Identifier, $1, @1.first_line, @1.first_column));
        free($1);
        if ($3) {
            int i;
            for (i = 0; i < $3->num_children; i++)
                add_child($$, $3->children[i]);
            free($3->children);
            free($3->value);
            free($3);
        }
    }
  | IDENTIFIER LPAR error RPAR {
        $$ = new_node(Call, NULL);
        add_child($$, new_node(Identifier, $1));
        free($1);
        syntax_errors++;
    }
  | PARSEINT LPAR IDENTIFIER LSQ ExprNoAssign RSQ RPAR {
        $$ = new_node_at(ParseArgs, NULL, @1.first_line, @1.first_column);
        add_child($$, new_node_at(Identifier, $3, @3.first_line, @3.first_column));
        free($3);
        add_child($$, $5);
    }
  | PARSEINT LPAR error RPAR { $$ = new_node_at(ParseArgs, NULL, @1.first_line, @1.first_column); syntax_errors++; }
  | NATURAL    { $$ = new_node_at(Natural,    $1, @1.first_line, @1.first_column); free($1); }
  | DECIMAL    { $$ = new_node_at(Decimal,    $1, @1.first_line, @1.first_column); free($1); }
  | BOOLLIT    { $$ = new_node_at(BoolLit,   $1, @1.first_line, @1.first_column); free($1); }
  | IDENTIFIER { $$ = new_node_at(Identifier, $1, @1.first_line, @1.first_column); free($1); }
  ;

AssignExpr:
    IDENTIFIER ASSIGN AssignExpr {
        $$ = new_node_at(Assign, NULL, @2.first_line, @2.first_column);
        add_child($$, new_node_at(Identifier, $1, @1.first_line, @1.first_column));
        free($1);
        add_child($$, $3);
    }
  | IDENTIFIER ASSIGN ExprNoAssign {
        $$ = new_node_at(Assign, NULL, @2.first_line, @2.first_column);
        add_child($$, new_node_at(Identifier, $1, @1.first_line, @1.first_column));
        free($1);
        add_child($$, $3);
    }
  ;

ExprArgs:
    /* empty */ { $$ = NULL; }
  | ExprArgList { $$ = $1; }
  ;

ExprArgList:
    Expr {
        $$ = new_node(AUX, NULL);
        add_child($$, $1);
    }
  | ExprArgList COMMA Expr {
        add_child($1, $3);
        $$ = $1;
    }
  ;

%%

void yyerror(char *s) {
    if (mode != 1 && mode != 3) {
        int report_col = (yytext && yytext[0] != '\0') ? tok_start_col : col;
        const char *txt = (yytext && yytext[0] == '"' && yytext[1] == '\0' && last_token_text[0])
                          ? last_token_text : (yytext ? yytext : "");
        printf("Line %d, col %d: %s: %s\n", line, report_col, s, txt);
    }
    last_token_text[0] = '\0';
    syntax_errors++;
}

int main(int argc, char **argv) {
    if (argc > 1) {
        if      (strcmp(argv[1], "-l")  == 0) mode = 1;
        else if (strcmp(argv[1], "-t")  == 0) mode = 2;
        else if (strcmp(argv[1], "-e1") == 0) mode = 3;
        else if (strcmp(argv[1], "-e2") == 0) mode = 4;
        else if (strcmp(argv[1], "-s")  == 0) mode = 5;
        else if (strcmp(argv[1], "-e3") == 0) mode = 6;
    }

    if (mode == 1 || mode == 3) {
        while (yylex());
        return 0;
    }

    yyparse();

    if (mode == 2) {
        if (syntax_errors == 0 && root != NULL)
            show_node(root, 0);
        if (root) deallocate_node(root);
    } else if (mode == 5 || mode == 6 || mode == 0) {
        if (syntax_errors == 0 && root != NULL) {
            SemanticResult *sr = semantic_analyze(root);
            semantic_errors = sr ? sr->errors : 0;

            if (mode == 5) {
                semantic_print_tables(sr);
                if (syntax_errors == 0)
                    show_node_annotated(root, 0);
            } else if (mode == 0 && semantic_errors == 0) {
                codegen_program(root, sr);
            }

            semantic_free(sr);
        }
        if (root) deallocate_node(root);
    } else if (mode == 4) {
        if (root) deallocate_node(root);
    }

    return 0;
}
