# Relatório final — Projeto de Compiladores (Juc)

Compilador desenvolvido com Flex/Bison e gerador LLVM.

## (i) Gramática reescrita

O analisador sintático é **LALR(1)** (Bison) alimentado pelo Flex. No `jucompiler.y` a gramática não segue a especificação Java à letra. O prefixo comum `public static <tipo> <id>` entra num único não-terminal, `ProgramMember`. A escolha entre campo e método fica para **`MemberRest`**: depois do identificador, `(` abre método (parâmetros e corpo); vírgula ou ponto e vírgula continuam uma lista de campos. Assim reduzimos conflitos entre declarações que partilham o mesmo início lexical. Métodos `void` têm produção separada (`PUBLIC STATIC VOID IDENTIFIER ...`), já que **campos não podem ser `void`** — o lookahead fica mais simples.

**Listas de identificadores** (`int a, b, c`) passam por `FieldIdList` e `VarIdList`. O parser expande essas listas em vários nós `FieldDecl` / `VarDecl` na AST, em vez de repetir o tipo em cada ramo da gramática.

Para expressões definimos **`Expr`**, **`ExprNoAssign`** e **`AssignExpr`**. A atribuição é `%right ASSIGN` e pode encadear (`a = b = c`), mas não deve aparecer como operando de chamadas ou de operadores onde o enunciado não a admite; **`ExprNoAssign`** cobre esses contextos. Chamadas e `Integer.parseInt` aparecem em produções de statement e de expressão; o lexer trata `IDENTIFIER` à parte das palavras-chave.

A **precedência** está declarada por níveis: `||`, `&&`, `^`, igualdades, relações, shifts, aditivos, multiplicativos. Os unários (`NOT`, `+`/`-` unários) ficam acima. **`.length`**, `(` e `[` entram como tokens de precedência para o postfix. O **else pendente** usa `%nonassoc LOWER_THAN_ELSE` e `%prec LOWER_THAN_ELSE` no `if` sem `else`, para o `else` se ligar ao `if` mais interior, como em Java.

**Recuperação sintática**: produções com `error` seguidas de `;` ou `)` fecham o erro e deixam o parser seguir (membros da classe, statements, expressões entre parênteses, listas de argumentos). Com **`%locations`**, cada nó criado com `new_node_at` guarda linha e coluna; isso sustenta o formato das mensagens semânticas pedido no enunciado.

## (ii) AST e tabelas de símbolos

A AST é representada por **nós com array dinâmico de filhos** (`struct node` com `type`, `value`, `children`, `line`, `col`). Cada valor de `NodeType` corresponde a uma construção da linguagem (`MethodDecl`, `Assign`, `Call`, operadores, etc.). Durante o parse usamos nós **`AUX`** para listas temporárias. Antes de imprimir com `-s`, **`remove_aux`** remove esses nós, de modo a só restarem expressões com tipo, como o output de referência exige.

Em expressões preenchemos **`sem_type`** e **`annotate`**. Nas chamadas, **`type_note`** guarda a assinatura concreta escolhida (por exemplo `(int,double)`). Sem isso o gerador não saberia qual overload LLVM chamar.

A semântica corre em **duas passagens**. Na primeira, **`pass_build_tables`** percorre o `Program` pela ordem de declaração: preenche a tabela da classe (campos e métodos, com vetor de tipos de parâmetros) e, por cada método, regista um **`MethodTables`** com **`SymbolTable`**, já com `return` e parâmetros. Na segunda, percorremos o corpo de cada método: **`VarDecl`** insere **locais** à medida que aparecem (**declaração antes de uso**); **`check_stmt`** e **`check_expr`** fazem o resto da verificação.

**`lookup_var`** olha primeiro para parâmetros e locais da tabela do método. Só depois consulta **campos** na tabela global. Nomes de métodos não entram nesta procura — variáveis e métodos ocupam espaços distintos no modelo do enunciado. Sobrecarga: primeiro igualdade estrita de tipos de parâmetros; se falhar, entra a regra de **compatibilidade**, desde que reste uma única candidata. Ambiguidade ou falta de método dispara os erros do enunciado; `Call` e o identificador ficam com **`undef`**.

Literais inteiros passam por **`check_number_bounds`**: `strtoul`, `INT_MAX`, `_` retirados antes do parse, tal como as regras do enunciado para **números fora de limites** e literais com underscores. Quando `^` ou `<<` / `>>` falham por tipos incompatíveis, reportamos o erro do catálogo e **ainda assim** anotamos o resultado como `int` ou `boolean` consoante os operandos. Isto corta cascatas de erros em `=` e em `while` que o repositório de testes não mostra; a lista de mensagens oficiais não foi alterada, só o pseudo-tipo que segue para o resto da árvore.

## (iii) Geração de código (LLVM IR)

**`codegen_program`** recebe a AST anotada e o **`SemanticResult`**. Mapeamos tipos Juc para **i1**, **i32** e **double**. **`System.out.print`** baixa a **`printf`** com os formatos `%d`, `%.16e` e `%s`; booleanos saem como `true`/`false`, conforme o enunciado. **`Integer.parseInt`** vira **`atoi`** sobre a string vinda de `args[i]`.

Símbolos LLVM para métodos são **mangled** (`ju_<Classe>_<método>_<letras de tipo>`), para overloads não colidirem só pelo nome curto. **`FindM`** compara o **`type_note`** do identificador na chamada com a assinatura formatada a partir da tabela da classe.

**`String[]`** (formal ou `main`) representa-se por **dois valores em i32**: comprimento e **ptr**. Assim **`.length`** e o arranque no `main` em C (`argc-1`, `argv+1`) partilham o mesmo layout. Locais e parâmetros usam **`alloca`**; um **`VMap`** por nível de frame associa nomes do Juc a nomes SSA (`%t…`, slots `alloca`).

**`&&`** e **`||`** não viram um único `and` / `or` sobre i1. Emitimos **branches**, etiquetas novas (`L…`) e no merge um **`phi i1`** com o predecessor certo. O resultado tem **curto-circuito**, como em Java. **`^`** usa `xor i1` ou `xor i32`; shifts usam `shl` e `ashr`. Só geramos IR sem erros lexicais, sintáticos ou semânticos e **sem** flags `-l`, `-t`, `-s`, `-e*`, tal como o enunciado descreve para o modo “compilar para stdout”.
