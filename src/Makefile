CC = gcc
CFLAGS = -Wall -Wno-unused-function -lm

all: jucompiler

jucompiler: jucompiler.y jucompiler.l ast.c semantics.c codegen.c
	bison -d -o y.tab.c jucompiler.y
	flex jucompiler.l
	$(CC) -o jucompiler y.tab.c lex.yy.c ast.c semantics.c codegen.c $(CFLAGS)

clean:
	rm -f jucompiler y.tab.c y.tab.h lex.yy.c *.class *.j *.ll
