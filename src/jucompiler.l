/*
  Authors:
    Gonçalo Nogueira
    João Moura
*/

%{
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "y.tab.h" 

int line = 1;
int col  = 1;
int tok_start_col = 1;

int print_tokens = 0;
extern int mode;
extern char last_token_text[];

extern YYLTYPE yylloc;

int comment_start_line = 0;
int comment_start_col  = 0;

int str_start_line = 0;
int str_start_col  = 0;
int str_error      = 0;

#define STR_BUFFER_SIZE 4096
char str_buffer[STR_BUFFER_SIZE];
int  str_len = 0;


static void str_reset(void) {
  str_len = 0;
  str_buffer[0] = '\0';
}

static void str_append(const char *s, int len) {
  if (len <= 0) return;
  if (str_len + len >= STR_BUFFER_SIZE) {
    len = STR_BUFFER_SIZE - 1 - str_len;
    if (len <= 0) return;
  }
  memcpy(str_buffer + str_len, s, len);
  str_len += len;
  str_buffer[str_len] = '\0';
}

#define YY_USER_ACTION                 \
  do {                                 \
    tok_start_col = col;               \
    yylloc.first_line = line;          \
    yylloc.first_column = tok_start_col; \
    col += yyleng;                     \
  } while (0);

%}

%x STRING_STATE
%x COMMENT_STATE

DIGIT        [0-9]
NONZERO      [1-9]
LETTER       [a-zA-Z_$]

NAT_BODY     {NONZERO}([0-9_]*[0-9])?
NATURAL      0|{NAT_BODY}

INTPART      {DIGIT}([0-9_]*[0-9])?
FRACT_PART   {DIGIT}([0-9_]*[0-9])?
EXP_NUM      {DIGIT}([0-9_]*[0-9])?
EXPONENT     [eE][+-]?{EXP_NUM}

DECIMAL      ({INTPART}"."({FRACT_PART})?{EXPONENT}?|"."{FRACT_PART}{EXPONENT}?|{INTPART}{EXPONENT})

WS           [ \t\f]+
NEWLINE      (\r\n|\r|\n)

/* yylloc fills token start for Bison @n. Longest match: keywords before IDENTIFIER. */

%%

"//"[^\r\n]*  { }

"/*"          {
                 comment_start_line = line;
                 comment_start_col  = tok_start_col;
                 BEGIN(COMMENT_STATE);
              }

<COMMENT_STATE>"*/" {
                 BEGIN(INITIAL);
              }

<COMMENT_STATE>{NEWLINE} {
                 line++;
                 col = 1;
              }

<COMMENT_STATE>.   { }

<COMMENT_STATE><<EOF>> {
                 printf("Line %d, col %d: unterminated comment\n",
                        comment_start_line, comment_start_col);
                 BEGIN(INITIAL);
              }

{WS}          { }

{NEWLINE}     {
                 line++;
                 col = 1;
              }

"\""          {
                 str_start_line = line;
                 str_start_col  = tok_start_col;
                 str_error      = 0;
                 str_reset();
                 BEGIN(STRING_STATE);
              }

<STRING_STATE>\" {
                 if (!str_error) {
                   if (mode == 1) {
                     int i;
                     printf("STRLIT(\"");
                     for (i = 0; i < str_len; i++) {
                       putchar(str_buffer[i]);
                     }
                    printf("\")\n");
                   }
                   line = str_start_line;
                   tok_start_col = str_start_col;
                   snprintf(last_token_text, 4096, "\"%s\"", str_buffer);
                   {
                     char tmp[STR_BUFFER_SIZE + 4];
                     snprintf(tmp, sizeof(tmp), "\"%s\"", str_buffer);
                     yylval.lexeme = strdup(tmp);
                   }
                   str_reset();
                   BEGIN(INITIAL);
                   return STRLIT;
                 }
                 str_reset();
                 BEGIN(INITIAL);
              }
<STRING_STATE>[^\\\"\n\r]+ {
                 str_append(yytext, yyleng);
              }
<STRING_STATE>\\[fnrt\"\\] {
                 str_append(yytext, yyleng);
              }
<STRING_STATE>\\\r\n {
                 printf("Line %d, col %d: invalid escape sequence (\\)\n",
                        str_start_line, tok_start_col);
                 printf("Line %d, col %d: unterminated string literal\n",
                        str_start_line, str_start_col);
                 line++;
                 col = 1;
                 str_reset();
                 BEGIN(INITIAL);
              }
<STRING_STATE>\\\n {
                 printf("Line %d, col %d: invalid escape sequence (\\)\n",
                        str_start_line, tok_start_col);
                 printf("Line %d, col %d: unterminated string literal\n",
                        str_start_line, str_start_col);
                 line++;
                 col = 1;
                 str_reset();
                 BEGIN(INITIAL);
              }
<STRING_STATE>\\\r {
                 printf("Line %d, col %d: invalid escape sequence (\\)\n",
                        str_start_line, tok_start_col);
                 printf("Line %d, col %d: unterminated string literal\n",
                        str_start_line, str_start_col);
                 line++;
                 col = 1;
                 str_reset();
                 BEGIN(INITIAL);
              }
<STRING_STATE>\\[^fnrt\"\\\n\r] {
                 printf("Line %d, col %d: invalid escape sequence (%s)\n",
                        str_start_line, tok_start_col, yytext);
                 str_error = 1;
                 str_append(yytext, yyleng);
              }
<STRING_STATE>{NEWLINE} {
                 printf("Line %d, col %d: unterminated string literal\n",
                        str_start_line, str_start_col);
                 line++;
                 col = 1;
                 str_reset();
                 BEGIN(INITIAL);
              }

<STRING_STATE><<EOF>> {
                 printf("Line %d, col %d: unterminated string literal\n",
                        str_start_line, str_start_col);
                 str_reset();
                 BEGIN(INITIAL);
              }

"System.out.print" {
  if (mode == 1) printf("PRINT\n");
  return PRINT;
}

"Integer.parseInt" {
  if (mode == 1) printf("PARSEINT\n");
  return PARSEINT;
}

".length" {
  if (mode == 1) printf("DOTLENGTH\n");
  return DOTLENGTH;
}

"class"   { if (mode == 1) printf("CLASS\n");        return CLASS; }
"public"  { if (mode == 1) printf("PUBLIC\n");       return PUBLIC; }
"static"  { if (mode == 1) printf("STATIC\n");       return STATIC; }
"int"     { if (mode == 1) printf("INT\n");          return INT; }
"double"  { if (mode == 1) printf("DOUBLE\n");       return DOUBLE; }
"boolean" { if (mode == 1) printf("BOOL\n");         return BOOL; }
"String"  { if (mode == 1) printf("STRING\n");       return STRING; }
"void"    { if (mode == 1) printf("VOID\n");         return VOID; }
"if"      { if (mode == 1) printf("IF\n");           return IF; }
"else"    { if (mode == 1) printf("ELSE\n");         return ELSE; }
"while"   { if (mode == 1) printf("WHILE\n");        return WHILE; }
"return"  { if (mode == 1) printf("RETURN\n");       return RETURN; }
"true"    { if (mode == 1) printf("BOOLLIT(true)\n");  yylval.lexeme = strdup(yytext); return BOOLLIT; }
"false"   { if (mode == 1) printf("BOOLLIT(false)\n"); yylval.lexeme = strdup(yytext); return BOOLLIT; }

"<<"       { if (mode == 1) printf("LSHIFT\n");    return LSHIFT; }
">>"       { if (mode == 1) printf("RSHIFT\n");    return RSHIFT; }
"->"       { if (mode == 1) printf("ARROW\n");     return ARROW; }
"&&"       { if (mode == 1) printf("AND\n");       return AND; }
"||"       { if (mode == 1) printf("OR\n");        return OR; }
"=="       { if (mode == 1) printf("EQ\n");        return EQ; }
"!="       { if (mode == 1) printf("NE\n");        return NE; }
">="       { if (mode == 1) printf("GE\n");        return GE; }
"<="       { if (mode == 1) printf("LE\n");        return LE; }

"+"        { if (mode == 1) printf("PLUS\n");      return PLUS; }
"-"        { if (mode == 1) printf("MINUS\n");     return MINUS; }
"*"        { if (mode == 1) printf("STAR\n");      return STAR; }
"/"        { if (mode == 1) printf("DIV\n");       return DIV; }
"%"        { if (mode == 1) printf("MOD\n");       return MOD; }
"="        { if (mode == 1) printf("ASSIGN\n");    return ASSIGN; }
">"        { if (mode == 1) printf("GT\n");        return GT; }
"<"        { if (mode == 1) printf("LT\n");        return LT; }
"!"        { if (mode == 1) printf("NOT\n");       return NOT; }
"^"        { if (mode == 1) printf("XOR\n");       return XOR; }

"{"        { if (mode == 1) printf("LBRACE\n");    return LBRACE; }
"}"        { if (mode == 1) printf("RBRACE\n");    return RBRACE; }
"("        { if (mode == 1) printf("LPAR\n");      return LPAR; }
")"        { if (mode == 1) printf("RPAR\n");      return RPAR; }
"["        { if (mode == 1) printf("LSQ\n");       return LSQ; }
"]"        { if (mode == 1) printf("RSQ\n");       return RSQ; }
","        { if (mode == 1) printf("COMMA\n");     return COMMA; }
";"        { if (mode == 1) printf("SEMICOLON\n"); return SEMICOLON; }

"++"       { if (mode == 1) printf("RESERVED(++)\n");   yylval.lexeme = strdup("++");      return RESERVED; }
"--"       { if (mode == 1) printf("RESERVED(--)\n");   yylval.lexeme = strdup("--");      return RESERVED; }
"null"     { if (mode == 1) printf("RESERVED(null)\n"); yylval.lexeme = strdup(yytext);    return RESERVED; }
"Integer"  { if (mode == 1) printf("RESERVED(Integer)\n"); yylval.lexeme = strdup(yytext); return RESERVED; }
"System"   { if (mode == 1) printf("RESERVED(System)\n");  yylval.lexeme = strdup(yytext); return RESERVED; }

"abstract"     { if (mode == 1) printf("RESERVED(abstract)\n");     yylval.lexeme = strdup(yytext); return RESERVED; }
"assert"       { if (mode == 1) printf("RESERVED(assert)\n");       yylval.lexeme = strdup(yytext); return RESERVED; }
"break"        { if (mode == 1) printf("RESERVED(break)\n");        yylval.lexeme = strdup(yytext); return RESERVED; }
"byte"         { if (mode == 1) printf("RESERVED(byte)\n");         yylval.lexeme = strdup(yytext); return RESERVED; }
"case"         { if (mode == 1) printf("RESERVED(case)\n");         yylval.lexeme = strdup(yytext); return RESERVED; }
"catch"        { if (mode == 1) printf("RESERVED(catch)\n");        yylval.lexeme = strdup(yytext); return RESERVED; }
"char"         { if (mode == 1) printf("RESERVED(char)\n");         yylval.lexeme = strdup(yytext); return RESERVED; }
"const"        { if (mode == 1) printf("RESERVED(const)\n");        yylval.lexeme = strdup(yytext); return RESERVED; }
"continue"     { if (mode == 1) printf("RESERVED(continue)\n");     yylval.lexeme = strdup(yytext); return RESERVED; }
"default"      { if (mode == 1) printf("RESERVED(default)\n");      yylval.lexeme = strdup(yytext); return RESERVED; }
"do"           { if (mode == 1) printf("RESERVED(do)\n");           yylval.lexeme = strdup(yytext); return RESERVED; }
"enum"         { if (mode == 1) printf("RESERVED(enum)\n");         yylval.lexeme = strdup(yytext); return RESERVED; }
"extends"      { if (mode == 1) printf("RESERVED(extends)\n");      yylval.lexeme = strdup(yytext); return RESERVED; }
"final"        { if (mode == 1) printf("RESERVED(final)\n");        yylval.lexeme = strdup(yytext); return RESERVED; }
"finally"      { if (mode == 1) printf("RESERVED(finally)\n");      yylval.lexeme = strdup(yytext); return RESERVED; }
"float"        { if (mode == 1) printf("RESERVED(float)\n");        yylval.lexeme = strdup(yytext); return RESERVED; }
"for"          { if (mode == 1) printf("RESERVED(for)\n");          yylval.lexeme = strdup(yytext); return RESERVED; }
"goto"         { if (mode == 1) printf("RESERVED(goto)\n");         yylval.lexeme = strdup(yytext); return RESERVED; }
"implements"   { if (mode == 1) printf("RESERVED(implements)\n");   yylval.lexeme = strdup(yytext); return RESERVED; }
"import"       { if (mode == 1) printf("RESERVED(import)\n");       yylval.lexeme = strdup(yytext); return RESERVED; }
"instanceof"   { if (mode == 1) printf("RESERVED(instanceof)\n");   yylval.lexeme = strdup(yytext); return RESERVED; }
"interface"    { if (mode == 1) printf("RESERVED(interface)\n");    yylval.lexeme = strdup(yytext); return RESERVED; }
"long"         { if (mode == 1) printf("RESERVED(long)\n");         yylval.lexeme = strdup(yytext); return RESERVED; }
"native"       { if (mode == 1) printf("RESERVED(native)\n");       yylval.lexeme = strdup(yytext); return RESERVED; }
"new"          { if (mode == 1) printf("RESERVED(new)\n");          yylval.lexeme = strdup(yytext); return RESERVED; }
"package"      { if (mode == 1) printf("RESERVED(package)\n");      yylval.lexeme = strdup(yytext); return RESERVED; }
"private"      { if (mode == 1) printf("RESERVED(private)\n");      yylval.lexeme = strdup(yytext); return RESERVED; }
"protected"    { if (mode == 1) printf("RESERVED(protected)\n");    yylval.lexeme = strdup(yytext); return RESERVED; }
"short"        { if (mode == 1) printf("RESERVED(short)\n");        yylval.lexeme = strdup(yytext); return RESERVED; }
"strictfp"     { if (mode == 1) printf("RESERVED(strictfp)\n");     yylval.lexeme = strdup(yytext); return RESERVED; }
"super"        { if (mode == 1) printf("RESERVED(super)\n");        yylval.lexeme = strdup(yytext); return RESERVED; }
"switch"       { if (mode == 1) printf("RESERVED(switch)\n");       yylval.lexeme = strdup(yytext); return RESERVED; }
"synchronized" { if (mode == 1) printf("RESERVED(synchronized)\n"); yylval.lexeme = strdup(yytext); return RESERVED; }
"this"         { if (mode == 1) printf("RESERVED(this)\n");         yylval.lexeme = strdup(yytext); return RESERVED; }
"throw"        { if (mode == 1) printf("RESERVED(throw)\n");        yylval.lexeme = strdup(yytext); return RESERVED; }
"throws"       { if (mode == 1) printf("RESERVED(throws)\n");       yylval.lexeme = strdup(yytext); return RESERVED; }
"transient"    { if (mode == 1) printf("RESERVED(transient)\n");    yylval.lexeme = strdup(yytext); return RESERVED; }
"try"          { if (mode == 1) printf("RESERVED(try)\n");          yylval.lexeme = strdup(yytext); return RESERVED; }
"volatile"     { if (mode == 1) printf("RESERVED(volatile)\n");     yylval.lexeme = strdup(yytext); return RESERVED; }

{DECIMAL} {
  if (mode == 1) printf("DECIMAL(%s)\n", yytext);
  yylval.lexeme = strdup(yytext);
  return DECIMAL;
}

{NATURAL} {
  if (mode == 1) printf("NATURAL(%s)\n", yytext);
  yylval.lexeme = strdup(yytext);
  return NATURAL;
}

{LETTER}({LETTER}|{DIGIT})* {
  if (mode == 1) printf("IDENTIFIER(%s)\n", yytext);
  yylval.lexeme = strdup(yytext);
  return IDENTIFIER;
}

.            {
  printf("Line %d, col %d: illegal character (%s)\n",
         line, tok_start_col, yytext);
}

%%

int yywrap(void) {
  return 1;
}
