/* mcc yacc: makes a parser in C from a grammar, in the manner of bison.
   LALR(1): the states are built from kernel items and the lookaheads found
   by passing them from item to item; the tables are written out plain, one
   row for each state. Not done: GLR, pure parsers, named references,
   verbose error messages, the trace that -t asks for. */
#include "mcc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the fixed parts of a parser ---- */
static const char *const skel_DECL[] = {
    "#ifndef YYDEBUG",
    "#define YYDEBUG 0",
    "#endif",
    "int yydebug;",
    "#define YYEMPTY (-2)",
    "#define YYEOF 0",
    "#define YYerror 256",
    "#define YYUNDEF 257",
    "#define YYACCEPT goto yyacceptlab",
    "#define YYABORT goto yyabortlab",
    "#define YYERROR goto yyerrorlab",
    "#define YYRECOVERING() (!!yyerrstatus)",
    "#define yyerrok (yyerrstatus = 0)",
    "#define yyclearin (yychar = YYEMPTY)",
    "#ifndef YYRHSLOC",
    "#define YYRHSLOC(Rhs, K) ((Rhs)[K])",
    "#endif",
    "#if YYLSP_NEEDED",
    "#ifndef YYLLOC_DEFAULT",
    "#define YYLLOC_DEFAULT(Current, Rhs, N) \\",
    "    do { \\",
    "        if (N) { \\",
    "            (Current).first_line = YYRHSLOC(Rhs, 1).first_line; \\",
    "            (Current).first_column = YYRHSLOC(Rhs, 1).first_column; \\",
    "            (Current).last_line = YYRHSLOC(Rhs, N).last_line; \\",
    "            (Current).last_column = YYRHSLOC(Rhs, N).last_column; \\",
    "        } else { \\",
    "            (Current).first_line = (Current).last_line = YYRHSLOC(Rhs, 0).last_line; \\",
    "            (Current).first_column = (Current).last_column = YYRHSLOC(Rhs, 0).last_column; \\",
    "        } \\",
    "    } while (0)",
    "#endif",
    "#define YYLOCARG(p) , p",
    "#else",
    "#define YYLOCARG(p)",
    "#endif",
    "int yychar, yynerrs;",
    "YYSTYPE yylval;",
    "#if YYLSP_NEEDED",
    "YYLTYPE yylloc;",
    "#endif",
    "int yyparse(void);",
    0};

static const char *const skel_PARSE[] = {
    "int yyparse(void)",
    "{",
    "    int yyn, yylen = 0, yytoken = 0, yyerrstatus = 0, yyresult = 0, yyrule;",
    "    size_t yycap = 200, yyused;",
    "    short *yyss = (short *)malloc(yycap * sizeof(short)), *yyssp = yyss;",
    "    YYSTYPE *yyvs = (YYSTYPE *)malloc(yycap * sizeof(YYSTYPE)), *yyvsp = yyvs;",
    "    YYSTYPE yyval;",
    "#if YYLSP_NEEDED",
    "    YYLTYPE *yyls = (YYLTYPE *)malloc(yycap * sizeof(YYLTYPE)), *yylsp = yyls;",
    "    YYLTYPE yyloc;",
    "#endif",
    "    yynerrs = 0;",
    "    yychar = YYEMPTY;",
    "    if (!yyss || !yyvs)",
    "        goto yynomem;",
    "#if YYLSP_NEEDED",
    "    if (!yyls)",
    "        goto yynomem;",
    "    *yylsp = yylloc;",
    "    yyloc = yylloc;",
    "#endif",
    "    *yyssp = 0;",
    "    memset(yyvsp, 0, sizeof *yyvsp);",
    "    memset(&yyval, 0, sizeof yyval);",
    "    for (;;) {",
    "        yyused = (size_t)(yyssp - yyss) + 1;",
    "        if (yyused + 2 >= yycap) {",
    "            yycap *= 2;",
    "            yyss = (short *)realloc(yyss, yycap * sizeof(short));",
    "            yyvs = (YYSTYPE *)realloc(yyvs, yycap * sizeof(YYSTYPE));",
    "            if (!yyss || !yyvs)",
    "                goto yynomem;",
    "            yyssp = yyss + yyused - 1;",
    "            yyvsp = yyvs + yyused - 1;",
    "#if YYLSP_NEEDED",
    "            yyls = (YYLTYPE *)realloc(yyls, yycap * sizeof(YYLTYPE));",
    "            if (!yyls)",
    "                goto yynomem;",
    "            yylsp = yyls + yyused - 1;",
    "#endif",
    "        }",
    "        yyn = 0;",
    "        if (yyneedla[*yyssp]) {",
    "            if (yychar == YYEMPTY)",
    "                yychar = yylex();",
    "            if (yychar <= 0)",
    "                yychar = YYEOF;",
    "            yytoken = yychar < YYNCODES ? yytranslate[yychar] : 2;",
    "            yyn = yytable[*yyssp][yytoken];",
    "        }",
    "        if (yyn == YYACCEPTACT)",
    "            YYACCEPT;",
    "        if (yyn > 0) {",
    "            *++yyssp = (short)yyn;",
    "            *++yyvsp = yylval;",
    "#if YYLSP_NEEDED",
    "            *++yylsp = yylloc;",
    "#endif",
    "            yychar = YYEMPTY;",
    "            if (yyerrstatus)",
    "                yyerrstatus--;",
    "            continue;",
    "        }",
    "        if (yyn == 0)",
    "            yyn = -yydefred[*yyssp];",
    "        if (yyn == 0 || yyn == YYERRACT)",
    "            goto yyerrlab;",
    "        yyrule = -yyn;",
    "        yylen = yyr2[yyrule];",
    "        yyval = yyvsp[1 - yylen];",
    "#if YYLSP_NEEDED",
    "        YYLLOC_DEFAULT(yyloc, (yylsp - yylen), yylen);",
    "#endif",
    "        if (0)",
    "            YYERROR;",
    "        if (0)",
    "            YYABORT;",
    "        switch (yyrule) {",
    0};

static const char *const skel_PARSE2[] = {
    "        default:",
    "            break;",
    "        }",
    "        yyvsp -= yylen;",
    "        yyssp -= yylen;",
    "        *++yyvsp = yyval;",
    "#if YYLSP_NEEDED",
    "        yylsp -= yylen;",
    "        *++yylsp = yyloc;",
    "#endif",
    "        yylen = 0;",
    "        yyn = yygoto[*yyssp][yyr1[yyrule]];",
    "        *++yyssp = (short)yyn;",
    "        continue;",
    "    yyerrorlab:",
    "        yyvsp -= yylen;",
    "        yyssp -= yylen;",
    "#if YYLSP_NEEDED",
    "        yylsp -= yylen;",
    "#endif",
    "        yylen = 0;",
    "        goto yyerrlab1;",
    "    yyerrlab:",
    "        if (!yyerrstatus) {",
    "            yynerrs++;",
    "            yyerror(\"syntax error\");",
    "        }",
    "        if (yyerrstatus == 3) {",
    "            if (yychar <= YYEOF) {",
    "                if (yychar == YYEOF)",
    "                    YYABORT;",
    "            } else {",
    "                yydestruct(yytoken, &yylval YYLOCARG(&yylloc));",
    "                yychar = YYEMPTY;",
    "            }",
    "        }",
    "    yyerrlab1:",
    "        yyerrstatus = 3;",
    "        for (;;) {",
    "            yyn = yytable[*yyssp][1];",
    "            if (yyn > 0 && yyn != YYACCEPTACT)",
    "                break;",
    "            if (yyssp == yyss)",
    "                YYABORT;",
    "            yydestruct(yystos[*yyssp], yyvsp YYLOCARG(yylsp));",
    "            yyvsp--;",
    "            yyssp--;",
    "#if YYLSP_NEEDED",
    "            yylsp--;",
    "#endif",
    "        }",
    "        *++yyssp = (short)yyn;",
    "        *++yyvsp = yylval;",
    "#if YYLSP_NEEDED",
    "        *++yylsp = yylloc;",
    "#endif",
    "    }",
    "yyacceptlab:",
    "    yyresult = 0;",
    "    goto yyreturn;",
    "yyabortlab:",
    "    yyresult = 1;",
    "    goto yyreturn;",
    "yynomem:",
    "    yyerror(\"memory exhausted\");",
    "    yyresult = 2;",
    "yyreturn:",
    "    if (yyss && yyvs) {",
    "        if (yychar != YYEMPTY && yychar > 0)",
    "            yydestruct(yychar < YYNCODES ? yytranslate[yychar] : 2, &yylval YYLOCARG(&yylloc));",
    "        yyvsp -= yylen;",
    "        yyssp -= yylen;",
    "#if YYLSP_NEEDED",
    "        yylsp -= yylen;",
    "#endif",
    "        while (yyssp != yyss) {",
    "            yydestruct(yystos[*yyssp], yyvsp YYLOCARG(yylsp));",
    "            yyvsp--;",
    "            yyssp--;",
    "#if YYLSP_NEEDED",
    "            yylsp--;",
    "#endif",
    "        }",
    "    }",
    "    free(yyss);",
    "    free(yyvs);",
    "#if YYLSP_NEEDED",
    "    free(yyls);",
    "#endif",
    "    return yyresult;",
    "}",
    0};

/* ---- the grammar ---- */

typedef struct Sym {
    char *name, *alias, *tag;
    int term;        /* a token */
    int code;        /* what the scanner returns for it */
    int prec, assoc; /* 0: none; assoc 'l' 'r' 'n' */
    int idx;         /* among the tokens, or among the others */
    int destructor;  /* + 1 */
    int nprods;
} Sym;

typedef struct Prod {
    int lhs, n, prec, line;
    int *rhs;
    char *action;
    int *ctx; /* an action in the middle of a rule sees the symbols before it */
    int nctx;
} Prod;

typedef struct Destructor {
    char *code;
} Destructor;

static Sym *syms;
static int nsyms, capsyms;
static Prod *prods;
static int nprods, capprods;
static Destructor *dtors;
static int ndtors;
static char **dtor_tags; /* destructors given for a type tag: pairs tag, index + 1 */
static int *dtor_tag_idx;
static int ndtor_tags;
static const char *yname;
static int yline;
static const char *yp;
static int nterm, nnt, next_code = 258, start_sym = -1, expect_sr = -1, want_locations, prec_level, nmid;
static Buf code_top, code_pre, code_post, code_req, code_union, code_tail;
static int seen_union;

static void yfail(const char *msg, const char *arg)
{
    fprintf(stderr, "%s:%d: ", yname, yline);
    fprintf(stderr, msg, arg);
    fprintf(stderr, "\n");
    exit(1);
}

static char *ydup(const char *s, size_t n)
{
    char *p = (char *)xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

static int sym_find(const char *name)
{
    int k;
    for (k = 0; k < nsyms; k++)
        if (strcmp(syms[k].name, name) == 0 || (syms[k].alias && strcmp(syms[k].alias, name) == 0))
            return k;
    return -1;
}

static int sym_get(const char *name, int term)
{
    int k = sym_find(name);
    if (k >= 0)
        return k;
    if (nsyms == capsyms) {
        capsyms = capsyms ? 2 * capsyms : 128;
        syms = (Sym *)xrealloc(syms, (size_t)capsyms * sizeof(Sym));
    }
    memset(&syms[nsyms], 0, sizeof(Sym));
    syms[nsyms].name = ydup(name, strlen(name));
    syms[nsyms].term = term;
    syms[nsyms].code = -1;
    return nsyms++;
}

/* ---- reading the grammar file ---- */

enum { Y_EOF, Y_ID, Y_CHAR, Y_STR, Y_TAG, Y_CODE, Y_DIR, Y_NUM, Y_PUNCT, Y_MARK, Y_PROLOGUE };

static int ytok;
static char *ytext; /* the name, the literal with its quotes, the tag, the code without its braces, the directive */

static void skip_space(void)
{
    for (;;) {
        if (*yp == '\n') {
            yline++;
            yp++;
        } else if (*yp == ' ' || *yp == '\t' || *yp == '\r' || *yp == '\f') {
            yp++;
        } else if (yp[0] == '/' && yp[1] == '*') {
            const char *e = strstr(yp + 2, "*/");
            if (!e)
                yfail("unterminated comment", 0);
            for (; yp < e + 2; yp++)
                if (*yp == '\n')
                    yline++;
        } else if (yp[0] == '/' && yp[1] == '/') {
            while (*yp && *yp != '\n')
                yp++;
        } else {
            break;
        }
    }
}

/* yp is at an opening brace: returns the place of the closing one. */
static const char *code_end(const char *s)
{
    int depth = 0;
    for (; *s; s++) {
        if (*s == '\n')
            yline++;
        if (*s == '"' || *s == '\'') {
            int q = *s;
            for (s++; *s && *s != q; s++) {
                if (*s == '\\' && s[1])
                    s++;
                if (*s == '\n')
                    yline++;
            }
            if (!*s)
                break;
        } else if (s[0] == '/' && s[1] == '*') {
            const char *e = strstr(s + 2, "*/");
            if (!e)
                break;
            for (; s < e + 1; s++)
                if (*s == '\n')
                    yline++;
        } else if (s[0] == '/' && s[1] == '/') {
            while (s[1] && s[1] != '\n')
                s++;
        } else if (*s == '{') {
            depth++;
        } else if (*s == '}' && --depth == 0) {
            return s;
        }
    }
    yfail("unterminated code in braces", 0);
    return s;
}

static int is_idc(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.'; }

static void ynext(void)
{
    const char *s;
    skip_space();
    s = yp;
    free(ytext);
    ytext = 0;
    if (!*yp) {
        ytok = Y_EOF;
    } else if (yp[0] == '%' && yp[1] == '%') {
        yp += 2;
        ytok = Y_MARK;
    } else if (yp[0] == '%' && yp[1] == '{') {
        const char *e = strstr(yp, "%}");
        if (!e)
            yfail("unterminated %%{", 0);
        ytext = ydup(yp + 2, (size_t)(e - yp - 2));
        for (; yp < e; yp++)
            if (*yp == '\n')
                yline++;
        yp = e + 2;
        ytok = Y_PROLOGUE;
    } else if (*yp == '%') {
        yp++;
        while (is_idc((u8)*yp) || *yp == '-')
            yp++;
        ytext = ydup(s, (size_t)(yp - s));
        ytok = Y_DIR;
    } else if (*yp == '{') {
        const char *e = code_end(yp);
        ytext = ydup(yp + 1, (size_t)(e - yp - 1));
        yp = e + 1;
        ytok = Y_CODE;
    } else if (*yp == '<') {
        const char *e = strchr(yp, '>');
        if (!e)
            yfail("unterminated <tag>", 0);
        ytext = ydup(yp + 1, (size_t)(e - yp - 1));
        yp = e + 1;
        ytok = Y_TAG;
    } else if (*yp == '\'' || *yp == '"') {
        int q = *yp;
        for (yp++; *yp && *yp != q; yp++)
            if (*yp == '\\' && yp[1])
                yp++;
        if (!*yp)
            yfail("unterminated literal", 0);
        yp++;
        ytext = ydup(s, (size_t)(yp - s));
        ytok = q == '"' ? Y_STR : Y_CHAR;
    } else if (*yp >= '0' && *yp <= '9') {
        while (is_idc((u8)*yp))
            yp++;
        ytext = ydup(s, (size_t)(yp - s));
        ytok = Y_NUM;
    } else if (is_idc((u8)*yp)) {
        while (is_idc((u8)*yp))
            yp++;
        ytext = ydup(s, (size_t)(yp - s));
        ytok = Y_ID;
    } else {
        ytext = ydup(yp, 1);
        yp++;
        ytok = Y_PUNCT;
    }
}

static int char_value(const char *lit)
{
    if (lit[1] != '\\')
        return (u8)lit[1];
    switch (lit[2]) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case '0': return 0;
    case 'a': return 7;
    case 'b': return '\b';
    case 'f': return '\f';
    case 'v': return '\v';
    }
    return (u8)lit[2];
}

/* The symbol the current token names (a name or a literal), made if new. */
static int tok_sym(int term)
{
    int k;
    if (ytok == Y_CHAR) {
        k = sym_get(ytext, 1);
        syms[k].code = char_value(ytext);
        return k;
    }
    if (ytok == Y_STR) {
        k = sym_find(ytext);
        if (k < 0) {
            k = sym_get(ytext, 1);
            syms[k].code = next_code++;
        }
        return k;
    }
    k = sym_find(ytext);
    if (k < 0) {
        k = sym_get(ytext, term);
        if (term)
            syms[k].code = next_code++;
    } else if (term && !syms[k].term) {
        syms[k].term = 1;
        syms[k].code = next_code++;
    }
    return k;
}

static Prod *new_prod(int lhs)
{
    Prod *p;
    if (nprods == capprods) {
        capprods = capprods ? 2 * capprods : 128;
        prods = (Prod *)xrealloc(prods, (size_t)capprods * sizeof(Prod));
    }
    p = &prods[nprods++];
    memset(p, 0, sizeof *p);
    p->lhs = lhs;
    p->line = yline;
    return p;
}

static void declarations(void)
{
    for (;;) {
        if (ytok == Y_EOF)
            yfail("no rules", 0);
        if (ytok == Y_MARK)
            return;
        if (ytok == Y_PROLOGUE) {
            Buf *b = seen_union ? &code_post : &code_pre;
            buf_put(b, ytext, strlen(ytext));
            buf_u8(b, '\n');
            ynext();
            continue;
        }
        if (ytok != Y_DIR)
            yfail("unexpected %s among the declarations", ytext);
        if (strcmp(ytext, "%union") == 0) {
            ynext();
            if (ytok == Y_ID)
                ynext();
            if (ytok != Y_CODE)
                yfail("%%union needs braces", 0);
            buf_put(&code_union, ytext, strlen(ytext));
            seen_union = 1;
            ynext();
        } else if (strcmp(ytext, "%token") == 0 || strcmp(ytext, "%left") == 0 || strcmp(ytext, "%right") == 0 ||
                   strcmp(ytext, "%nonassoc") == 0 || strcmp(ytext, "%type") == 0 || strcmp(ytext, "%precedence") == 0 ||
                   strcmp(ytext, "%nterm") == 0) {
            int kind = ytext[1], is_type = strcmp(ytext, "%type") == 0 || strcmp(ytext, "%nterm") == 0, last = -1;
            int is_token = strcmp(ytext, "%token") == 0;
            char *tag = 0;
            if (!is_type && !is_token)
                prec_level++;
            ynext();
            for (;;) {
                if (ytok == Y_TAG) {
                    tag = ydup(ytext, strlen(ytext));
                    ynext();
                } else if (ytok == Y_ID || ytok == Y_CHAR || (ytok == Y_STR && (is_type || last < 0 || !is_token))) {
                    last = tok_sym(!is_type);
                    if (tag)
                        syms[last].tag = tag;
                    if (!is_type && !is_token) {
                        syms[last].prec = prec_level;
                        syms[last].assoc = kind == 'p' ? 'p' : kind;
                    }
                    ynext();
                } else if (ytok == Y_NUM && last >= 0) {
                    syms[last].code = atoi(ytext);
                    ynext();
                } else if (ytok == Y_STR && last >= 0) {
                    syms[last].alias = ydup(ytext, strlen(ytext));
                    ynext();
                } else {
                    break;
                }
            }
        } else if (strcmp(ytext, "%start") == 0) {
            ynext();
            start_sym = tok_sym(0);
            ynext();
        } else if (strcmp(ytext, "%expect") == 0 || strcmp(ytext, "%expect-rr") == 0) {
            int rr = ytext[7] == '-';
            ynext();
            if (!rr)
                expect_sr = atoi(ytext);
            ynext();
        } else if (strcmp(ytext, "%locations") == 0) {
            want_locations = 1;
            ynext();
        } else if (strcmp(ytext, "%destructor") == 0 || strcmp(ytext, "%printer") == 0) {
            int keep = ytext[1] == 'd', idx = 0;
            ynext();
            if (ytok != Y_CODE)
                yfail("code expected", 0);
            if (keep) {
                dtors = (Destructor *)xrealloc(dtors, (size_t)(ndtors + 1) * sizeof(Destructor));
                dtors[ndtors].code = ydup(ytext, strlen(ytext));
                idx = ++ndtors;
            }
            ynext();
            while (ytok == Y_ID || ytok == Y_CHAR || ytok == Y_STR || ytok == Y_TAG) {
                if (ytok == Y_TAG) {
                    if (keep) {
                        dtor_tags = (char **)xrealloc(dtor_tags, (size_t)(ndtor_tags + 1) * sizeof(char *));
                        dtor_tag_idx = (int *)xrealloc(dtor_tag_idx, (size_t)(ndtor_tags + 1) * sizeof(int));
                        dtor_tags[ndtor_tags] = ydup(ytext, strlen(ytext));
                        dtor_tag_idx[ndtor_tags++] = idx;
                    }
                } else if (keep) {
                    int k = sym_find(ytext);
                    if (k < 0)
                        k = sym_get(ytext, 0);
                    syms[k].destructor = idx;
                }
                ynext();
            }
        } else if (strcmp(ytext, "%code") == 0) {
            Buf *b = &code_post;
            ynext();
            if (ytok == Y_ID) {
                if (strcmp(ytext, "requires") == 0 || strcmp(ytext, "provides") == 0)
                    b = &code_req;
                else if (strcmp(ytext, "top") == 0)
                    b = &code_top;
                ynext();
            }
            if (ytok != Y_CODE)
                yfail("code expected", 0);
            buf_put(b, ytext, strlen(ytext));
            buf_u8(b, '\n');
            ynext();
        } else if (strcmp(ytext, "%define") == 0) {
            int line;
            ynext();
            line = yline;
            ynext();
            if (yline == line && (ytok == Y_ID || ytok == Y_STR || ytok == Y_CODE || ytok == Y_NUM))
                ynext();
        } else if (strcmp(ytext, "%debug") == 0 || strcmp(ytext, "%error-verbose") == 0 || strcmp(ytext, "%verbose") == 0 ||
                   strcmp(ytext, "%defines") == 0 || strcmp(ytext, "%token-table") == 0 || strcmp(ytext, "%no-lines") == 0 ||
                   strcmp(ytext, "%pure-parser") == 0 || strcmp(ytext, "%yacc") == 0) {
            if (strcmp(ytext, "%pure-parser") == 0)
                yfail("pure parsers are not supported", 0);
            ynext();
        } else {
            yfail("unknown directive %s", ytext);
        }
    }
}

static void grammar(void)
{
    char num[32];
    while (ytok != Y_EOF && ytok != Y_MARK) {
        int lhs, *rhs = 0, n = 0, cap = 0, prec = 0;
        char *action = 0;
        if (ytok != Y_ID)
            yfail("a rule must start with a name, not %s", ytext);
        lhs = tok_sym(0);
        if (syms[lhs].term)
            yfail("%s is a token and cannot have rules", syms[lhs].name);
        if (start_sym < 0)
            start_sym = lhs;
        ynext();
        if (ytok != Y_PUNCT || ytext[0] != ':')
            yfail("':' expected after %s", syms[lhs].name);
        ynext();
        for (;;) {
            if (ytok == Y_ID || ytok == Y_CHAR || ytok == Y_STR) {
                const char *save_p;
                int save_line, k, is_lhs = 0;
                if (ytok == Y_ID) {
                    /* a name followed by a colon starts the next rule (a missing semicolon) */
                    save_p = yp;
                    save_line = yline;
                    skip_space();
                    is_lhs = *yp == ':';
                    yp = save_p;
                    yline = save_line;
                }
                if (is_lhs)
                    goto end_alt;
                if (action) {
                    /* the action seen was in the middle: it becomes a rule of its own for a made-up name */
                    Prod *mp;
                    int ms;
                    sprintf(num, "$@%d", ++nmid);
                    ms = sym_get(num, 0);
                    mp = new_prod(ms);
                    mp->action = action;
                    mp->ctx = (int *)xmalloc((size_t)(n + 1) * sizeof(int));
                    memcpy(mp->ctx, rhs, (size_t)n * sizeof(int));
                    mp->nctx = n;
                    action = 0;
                    if (n == cap)
                        rhs = (int *)xrealloc(rhs, (size_t)(cap = cap ? 2 * cap : 8) * sizeof(int));
                    rhs[n++] = ms;
                }
                k = tok_sym(ytok != Y_ID);
                if (n == cap)
                    rhs = (int *)xrealloc(rhs, (size_t)(cap = cap ? 2 * cap : 8) * sizeof(int));
                rhs[n++] = k;
                ynext();
            } else if (ytok == Y_CODE) {
                if (action) {
                    Prod *mp;
                    int ms;
                    sprintf(num, "$@%d", ++nmid);
                    ms = sym_get(num, 0);
                    mp = new_prod(ms);
                    mp->action = action;
                    mp->ctx = (int *)xmalloc((size_t)(n + 1) * sizeof(int));
                    memcpy(mp->ctx, rhs, (size_t)n * sizeof(int));
                    mp->nctx = n;
                    if (n == cap)
                        rhs = (int *)xrealloc(rhs, (size_t)(cap = cap ? 2 * cap : 8) * sizeof(int));
                    rhs[n++] = ms;
                }
                action = ydup(ytext, strlen(ytext));
                ynext();
            } else if (ytok == Y_DIR && strcmp(ytext, "%prec") == 0) {
                ynext();
                prec = tok_sym(1) + 1;
                ynext();
            } else if (ytok == Y_DIR && strcmp(ytext, "%empty") == 0) {
                ynext();
            } else if (ytok == Y_PUNCT && (ytext[0] == '|' || ytext[0] == ';')) {
                Prod *p;
                int more;
            end_alt:
                more = ytok == Y_PUNCT && ytext[0] == '|';
                p = new_prod(lhs);
                p->rhs = rhs;
                p->n = n;
                p->prec = prec;
                p->action = action;
                rhs = 0;
                n = cap = 0;
                prec = 0;
                action = 0;
                if (ytok == Y_PUNCT)
                    ynext();
                if (!more && ytok == Y_PUNCT && ytext[0] == '|') {
                    /* "; |" goes on with the same name, as bison has it */
                    more = 1;
                    ynext();
                }
                if (!more)
                    break;
            } else if (ytok == Y_EOF || ytok == Y_MARK) {
                goto end_alt;
            } else {
                yfail("unexpected %s in a rule", ytext);
            }
        }
    }
}

/* ---- the automaton ---- */

static int *pstart;    /* the first item of each rule; an item is a rule and a place in it */
static int *item_prod, *item_sym, nitems;
static int **nt_prods; /* the rules of each symbol */
static u8 *nullable;
static u32 **first;
static int lawords;

typedef struct State {
    int *kernel, nk;
    int *tsym, *tto, ntrans;
    int slot; /* where the lookaheads of its kernel items start */
    int access;
} State;

static State *states;
static int nstates, capstates;
static u32 *la;       /* a set of tokens for each kernel item of each state */
static int nslots;
static int *link_from, *link_to, nlinks, caplinks;

static int *cl_items, ncl, *cl_slot;
static u32 *cl_la;

#define BIT(set, k) ((set)[(k) >> 5] >> ((k) & 31) & 1)
#define SETBIT(set, k) ((set)[(k) >> 5] |= 1U << ((k) & 31))

static int or_into(u32 *dst, const u32 *src)
{
    int k, changed = 0;
    for (k = 0; k < lawords; k++) {
        u32 v = dst[k] | src[k];
        if (v != dst[k]) {
            dst[k] = v;
            changed = 1;
        }
    }
    return changed;
}

static void cl_reset(void)
{
    int k;
    for (k = 0; k < ncl; k++)
        cl_slot[cl_items[k]] = -1;
    ncl = 0;
}

static int cl_add(int item, const u32 *set)
{
    int s = cl_slot[item];
    if (s < 0) {
        s = cl_slot[item] = ncl;
        cl_items[ncl++] = item;
        memset(cl_la + (size_t)s * (size_t)lawords, 0, (size_t)lawords * 4);
        or_into(cl_la + (size_t)s * (size_t)lawords, set);
        return 1;
    }
    return or_into(cl_la + (size_t)s * (size_t)lawords, set);
}

/* Completes the set of items with those their lookaheads allow. */
static void closure1(void)
{
    u32 *f = (u32 *)xmalloc((size_t)lawords * 4);
    int changed = 1, i, j;
    while (changed) {
        changed = 0;
        for (i = 0; i < ncl; i++) {
            int it = cl_items[i], x = item_sym[it], **pp, *p;
            if (x < 0 || syms[x].term)
                continue;
            memset(f, 0, (size_t)lawords * 4);
            for (j = it + 1;; j++) {
                int s = item_sym[j];
                if (s < 0) {
                    or_into(f, cl_la + (size_t)i * (size_t)lawords);
                    break;
                }
                if (syms[s].term) {
                    SETBIT(f, syms[s].idx);
                    break;
                }
                or_into(f, first[s]);
                if (!nullable[s])
                    break;
            }
            pp = &nt_prods[x];
            for (p = *pp; *p >= 0; p++)
                changed |= cl_add(pstart[*p], f);
        }
    }
    free(f);
}

static int state_find(int *kernel, int nk)
{
    int k;
    for (k = 0; k < nstates; k++)
        if (states[k].nk == nk && memcmp(states[k].kernel, kernel, (size_t)nk * sizeof(int)) == 0)
            return k;
    if (nstates == capstates) {
        capstates = capstates ? 2 * capstates : 256;
        states = (State *)xrealloc(states, (size_t)capstates * sizeof(State));
    }
    memset(&states[nstates], 0, sizeof(State));
    states[nstates].kernel = (int *)xmalloc((size_t)nk * sizeof(int));
    memcpy(states[nstates].kernel, kernel, (size_t)nk * sizeof(int));
    states[nstates].nk = nk;
    return nstates++;
}

static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

static int kernel_index(State *s, int item)
{
    int k;
    for (k = 0; k < s->nk; k++)
        if (s->kernel[k] == item)
            return k;
    die("mcc yacc: lost an item");
    return 0;
}

static Buf yo;

static void y(const char *s) { buf_put(&yo, s, strlen(s)); }

static void ynum(long v)
{
    char num[24];
    sprintf(num, "%ld", v);
    y(num);
}

static void ylines(const char *const *lines)
{
    for (; *lines; lines++) {
        y(*lines);
        y("\n");
    }
}

static const char *tag_of(int sym) { return sym >= 0 ? syms[sym].tag : 0; }

/* Writes an action with its $ and @ references spelled out. rhs/n: the symbols before the action. */
static void put_action(const char *code, int lhs, const int *rhs, int n, int in_destructor)
{
    const char *s;
    for (s = code; *s; s++) {
        if (*s == '"' || *s == '\'') {
            int q = *s;
            const char *b = s;
            for (s++; *s && *s != q; s++)
                if (*s == '\\' && s[1])
                    s++;
            buf_put(&yo, b, (size_t)(s - b) + (*s ? 1 : 0));
            if (!*s)
                break;
        } else if (s[0] == '/' && s[1] == '*') {
            const char *e = strstr(s + 2, "*/");
            e = e ? e + 1 : s + strlen(s) - 1;
            buf_put(&yo, s, (size_t)(e - s) + 1);
            s = e;
        } else if (*s == '$' || *s == '@') {
            const char *b = s, *tag = 0;
            char *given = 0;
            int at = *s == '@', neg = 0, v = 0;
            s++;
            if (!at && *s == '<') {
                const char *e = strchr(s, '>');
                if (!e)
                    yfail("unterminated <tag> in an action", 0);
                given = ydup(s + 1, (size_t)(e - s - 1));
                s = e + 1;
            }
            if (*s == '$') {
                if (at)
                    y(in_destructor ? "(*yylocationp)" : "(yyloc)");
                else {
                    tag = given ? given : tag_of(lhs);
                    y(in_destructor ? "((*yyvaluep)" : "(yyval");
                    if (tag) {
                        y(".");
                        y(tag);
                    }
                    y(")");
                }
            } else if ((*s >= '0' && *s <= '9') || (*s == '-' && s[1] >= '0' && s[1] <= '9')) {
                if (*s == '-')
                    neg = 1, s++;
                while (*s >= '0' && *s <= '9')
                    v = v * 10 + *s++ - '0';
                s--;
                if (neg)
                    v = -v;
                if (v > n)
                    yfail("an action refers to a symbol past its place", 0);
                y(at ? "(yylsp[" : "(yyvsp[");
                ynum(v - n);
                y("]");
                if (!at) {
                    tag = given ? given : v >= 1 ? tag_of(rhs[v - 1]) : 0;
                    if (tag) {
                        y(".");
                        y(tag);
                    }
                }
                y(")");
            } else {
                buf_put(&yo, b, (size_t)(s - b) + 1);
            }
            free(given);
        } else {
            buf_u8(&yo, (u8)*s);
        }
    }
}

static void put_tokens(Buf *b)
{
    Buf save = yo;
    int k;
    yo = *b;
    y("#ifndef YYTOKENTYPE\n#define YYTOKENTYPE\nenum yytokentype {\n    YYEMPTY_TOKEN_ = -2");
    for (k = 0; k < nsyms; k++)
        if (syms[k].term && syms[k].code >= 256 && syms[k].name[0] != '"' && syms[k].name[0] != '$' && strcmp(syms[k].name, "error") != 0) {
            y(",\n    ");
            y(syms[k].name);
            y(" = ");
            ynum(syms[k].code);
        }
    y("\n};\n#endif\n");
    y("#if !defined YYSTYPE && !defined YYSTYPE_IS_DECLARED\n");
    if (code_union.len) {
        y("union YYSTYPE {");
        buf_put(&yo, code_union.p, code_union.len);
        y("};\ntypedef union YYSTYPE YYSTYPE;\n");
    } else {
        y("typedef int YYSTYPE;\n");
    }
    y("#define YYSTYPE_IS_DECLARED 1\n#endif\n");
    if (want_locations) {
        y("#if !defined YYLTYPE && !defined YYLTYPE_IS_DECLARED\n");
        y("typedef struct YYLTYPE {\n    int first_line, first_column, last_line, last_column;\n} YYLTYPE;\n");
        y("#define YYLTYPE_IS_DECLARED 1\n#endif\n");
    }
    *b = yo;
    yo = save;
}

int yacc_main(int argc, char **argv)
{
    const char *out_name = 0, *def_name = 0;
    char *src;
    size_t len;
    int k, i, j, changed, sr = 0, rr = 0;
    short *table, *gototab, *defred;
    u8 *needla;
    Buf hdr;
    memset(&hdr, 0, sizeof hdr);
    for (k = 0; k < argc; k++) {
        if (strcmp(argv[k], "-o") == 0 && k + 1 < argc)
            out_name = argv[++k];
        else if (strncmp(argv[k], "-o", 2) == 0)
            out_name = argv[k] + 2;
        else if (strncmp(argv[k], "--output=", 9) == 0)
            out_name = argv[k] + 9;
        else if (strncmp(argv[k], "--defines=", 10) == 0)
            def_name = argv[k] + 10;
        else if (strcmp(argv[k], "-d") == 0 || strcmp(argv[k], "--defines") == 0)
            def_name = "";
        else if (strcmp(argv[k], "--version") == 0 || strcmp(argv[k], "-V") == 0) {
            puts("bison (GNU Bison) 3.8.2 (mcc yacc)");
            return 0;
        } else if (argv[k][0] != '-')
            yname = argv[k];
    }
    if (!yname)
        die("usage: mcc yacc [-o out.c] [--defines=out.h] file.y");
    if (!out_name)
        out_name = "y.tab.c";
    if (def_name && !*def_name) {
        char *d = ydup(out_name, strlen(out_name));
        if (strlen(d) > 2 && strcmp(d + strlen(d) - 2, ".c") == 0)
            d[strlen(d) - 1] = 'h';
        def_name = d;
    }
    src = (char *)os_read_file(yname, &len);
    if (!src)
        die("cannot read %s", yname);
    src = ydup(src, len);
    yp = src;
    yline = 1;
    sym_get("$end", 1);
    syms[0].code = 0;
    sym_get("error", 1);
    syms[1].code = 256;
    sym_get("$undefined", 1);
    syms[2].code = 257;
    ynext();
    declarations();
    ynext();
    {
        /* the rule that accepts comes first, so space for it is made before the others are read */
        new_prod(sym_get("$accept", 0));
    }
    grammar();
    if (ytok == Y_MARK)
        buf_put(&code_tail, yp, strlen(yp));
    if (start_sym < 0)
        yfail("no rules", 0);
    prods[0].rhs = (int *)xmalloc(2 * sizeof(int));
    prods[0].rhs[0] = start_sym;
    prods[0].rhs[1] = 0;
    prods[0].n = 2;

    /* number the symbols of each kind */
    for (k = 0; k < nsyms; k++) {
        if (syms[k].term)
            syms[k].idx = nterm++;
        else
            syms[k].idx = nnt++;
    }
    for (k = 0; k < nprods; k++)
        syms[prods[k].lhs].nprods++;
    for (k = 0; k < nsyms; k++)
        if (!syms[k].term && !syms[k].nprods) {
            fprintf(stderr, "%s: the symbol %s is used but has no rules and is not a token\n", yname, syms[k].name);
            exit(1);
        }
    /* a destructor given for a type goes to the symbols of that type that have none of their own */
    for (k = 0; k < nsyms; k++)
        for (i = 0; i < ndtor_tags && !syms[k].destructor; i++)
            if ((syms[k].tag && (strcmp(dtor_tags[i], syms[k].tag) == 0 || strcmp(dtor_tags[i], "*") == 0)) ||
                (!syms[k].tag && !*dtor_tags[i]))
                syms[k].destructor = dtor_tag_idx[i];

    /* the items */
    pstart = (int *)xmalloc((size_t)(nprods + 1) * sizeof(int));
    for (k = 0; k < nprods; k++) {
        pstart[k] = nitems;
        nitems += prods[k].n + 1;
    }
    item_prod = (int *)xmalloc((size_t)(nitems + 1) * sizeof(int));
    item_sym = (int *)xmalloc((size_t)(nitems + 1) * sizeof(int));
    for (k = 0; k < nprods; k++)
        for (i = 0; i <= prods[k].n; i++) {
            item_prod[pstart[k] + i] = k;
            item_sym[pstart[k] + i] = i < prods[k].n ? prods[k].rhs[i] : -1;
        }
    nt_prods = (int **)xmalloc((size_t)nsyms * sizeof(int *));
    for (k = 0; k < nsyms; k++) {
        nt_prods[k] = (int *)xmalloc((size_t)(syms[k].nprods + 1) * sizeof(int));
        syms[k].nprods = 0;
    }
    for (k = 0; k < nprods; k++)
        nt_prods[prods[k].lhs][syms[prods[k].lhs].nprods++] = k;
    for (k = 0; k < nsyms; k++)
        nt_prods[k][syms[k].nprods] = -1;

    /* what can be empty, and what each symbol can start with */
    lawords = (nterm + 1 + 31) / 32;
    nullable = (u8 *)xmalloc((size_t)nsyms);
    memset(nullable, 0, (size_t)nsyms);
    first = (u32 **)xmalloc((size_t)nsyms * sizeof(u32 *));
    for (k = 0; k < nsyms; k++) {
        first[k] = (u32 *)xmalloc((size_t)lawords * 4);
        memset(first[k], 0, (size_t)lawords * 4);
    }
    do {
        changed = 0;
        for (k = 0; k < nprods; k++) {
            Prod *p = &prods[k];
            for (i = 0; i < p->n; i++) {
                int s = p->rhs[i];
                if (syms[s].term) {
                    if (!BIT(first[p->lhs], syms[s].idx)) {
                        SETBIT(first[p->lhs], syms[s].idx);
                        changed = 1;
                    }
                    break;
                }
                changed |= or_into(first[p->lhs], first[s]);
                if (!nullable[s])
                    break;
            }
            if (i == p->n && !nullable[p->lhs]) {
                nullable[p->lhs] = 1;
                changed = 1;
            }
        }
    } while (changed);

    /* the states, by their kernel items */
    cl_items = (int *)xmalloc((size_t)(nitems + 1) * sizeof(int));
    cl_slot = (int *)xmalloc((size_t)(nitems + 1) * sizeof(int));
    cl_la = (u32 *)xmalloc((size_t)(nitems + 1) * (size_t)lawords * 4);
    for (k = 0; k <= nitems; k++)
        cl_slot[k] = -1;
    {
        int first_item = 0, *next = (int *)xmalloc((size_t)(nitems + 1) * sizeof(int));
        u32 *none = (u32 *)xmalloc((size_t)lawords * 4);
        memset(none, 0, (size_t)lawords * 4);
        state_find(&first_item, 1);
        for (k = 0; k < nstates; k++) {
            int x;
            cl_reset();
            for (i = 0; i < states[k].nk; i++)
                cl_add(states[k].kernel[i], none);
            closure1();
            for (x = 0; x < nsyms; x++) {
                int nn = 0, to;
                for (i = 0; i < ncl; i++)
                    if (item_sym[cl_items[i]] == x)
                        next[nn++] = cl_items[i] + 1;
                if (!nn || x == 0)
                    continue;
                qsort(next, (size_t)nn, sizeof(int), cmp_int);
                to = state_find(next, nn);
                states[to].access = x;
                states[k].tsym = (int *)xrealloc(states[k].tsym, (size_t)(states[k].ntrans + 1) * sizeof(int));
                states[k].tto = (int *)xrealloc(states[k].tto, (size_t)(states[k].ntrans + 1) * sizeof(int));
                states[k].tsym[states[k].ntrans] = x;
                states[k].tto[states[k].ntrans++] = to;
            }
        }
        free(next);

        /* the lookaheads: which arise on the spot, and which are passed along from item to item */
        for (k = 0; k < nstates; k++) {
            states[k].slot = nslots;
            nslots += states[k].nk;
        }
        la = (u32 *)xmalloc((size_t)nslots * (size_t)lawords * 4);
        memset(la, 0, (size_t)nslots * (size_t)lawords * 4);
        for (k = 0; k < nstates; k++)
            for (i = 0; i < states[k].nk; i++) {
                u32 *dummy = (u32 *)xmalloc((size_t)lawords * 4);
                memset(dummy, 0, (size_t)lawords * 4);
                SETBIT(dummy, nterm);
                cl_reset();
                cl_add(states[k].kernel[i], dummy);
                closure1();
                free(dummy);
                for (j = 0; j < ncl; j++) {
                    int it = cl_items[j], x = item_sym[it], t, to = -1, slot;
                    u32 *l = cl_la + (size_t)j * (size_t)lawords;
                    if (x <= 0)
                        continue;
                    for (t = 0; t < states[k].ntrans; t++)
                        if (states[k].tsym[t] == x)
                            to = states[k].tto[t];
                    slot = states[to].slot + kernel_index(&states[to], it + 1);
                    if (BIT(l, nterm)) {
                        if (nlinks == caplinks) {
                            caplinks = caplinks ? 2 * caplinks : 1024;
                            link_from = (int *)xrealloc(link_from, (size_t)caplinks * sizeof(int));
                            link_to = (int *)xrealloc(link_to, (size_t)caplinks * sizeof(int));
                        }
                        link_from[nlinks] = states[k].slot + i;
                        link_to[nlinks++] = slot;
                        l[nterm >> 5] &= ~(1U << (nterm & 31));
                        or_into(la + (size_t)slot * (size_t)lawords, l);
                        SETBIT(l, nterm);
                    } else {
                        or_into(la + (size_t)slot * (size_t)lawords, l);
                    }
                }
            }
        do {
            changed = 0;
            for (k = 0; k < nlinks; k++)
                changed |= or_into(la + (size_t)link_to[k] * (size_t)lawords, la + (size_t)link_from[k] * (size_t)lawords);
        } while (changed);
        free(none);
    }

    /* the tables */
#define ACT_ACCEPT 32767
#define ACT_ERROR (-32767)
    table = (short *)xmalloc((size_t)nstates * (size_t)nterm * sizeof(short));
    gototab = (short *)xmalloc((size_t)nstates * (size_t)nnt * sizeof(short));
    defred = (short *)xmalloc((size_t)nstates * sizeof(short));
    needla = (u8 *)xmalloc((size_t)nstates);
    memset(table, 0, (size_t)nstates * (size_t)nterm * sizeof(short));
    memset(gototab, 0, (size_t)nstates * (size_t)nnt * sizeof(short));
    for (k = 0; k < nstates; k++) {
        short *row = table + (size_t)k * (size_t)nterm;
        int *count = (int *)xmalloc((size_t)(nprods + 1) * sizeof(int)), best = 0;
        for (i = 0; i < states[k].ntrans; i++) {
            int x = states[k].tsym[i];
            if (syms[x].term)
                row[syms[x].idx] = (short)states[k].tto[i];
            else
                gototab[(size_t)k * (size_t)nnt + (size_t)syms[x].idx] = (short)states[k].tto[i];
        }
        cl_reset();
        for (i = 0; i < states[k].nk; i++)
            cl_add(states[k].kernel[i], la + (size_t)(states[k].slot + i) * (size_t)lawords);
        closure1();
        for (i = 0; i < ncl; i++) {
            int it = cl_items[i], p = item_prod[it], t;
            u32 *l = cl_la + (size_t)i * (size_t)lawords;
            if (p == 0 && it == pstart[0] + 1) {
                row[0] = ACT_ACCEPT;
                continue;
            }
            if (item_sym[it] >= 0)
                continue;
            for (t = 0; t < nterm; t++) {
                int pprec = 0, tprec = 0, tassoc = 0, s;
                if (!BIT(l, t))
                    continue;
                if (row[t] == 0) {
                    row[t] = (short)-p;
                    continue;
                }
                if (row[t] == ACT_ERROR)
                    continue;
                if (row[t] < 0) {
                    /* two rules could be reduced: the earlier one is */
                    rr++;
                    if (p < -row[t])
                        row[t] = (short)-p;
                    continue;
                }
                /* shift or reduce: precedence decides, if both sides have one */
                if (prods[p].prec)
                    pprec = syms[prods[p].prec - 1].prec;
                else
                    for (j = prods[p].n - 1; j >= 0; j--)
                        if (syms[prods[p].rhs[j]].term) {
                            pprec = syms[prods[p].rhs[j]].prec;
                            break;
                        }
                for (s = 0; s < nsyms; s++)
                    if (syms[s].term && syms[s].idx == t) {
                        tprec = syms[s].prec;
                        tassoc = syms[s].assoc;
                    }
                if (!pprec || !tprec) {
                    sr++;
                    continue;
                }
                if (pprec > tprec || (pprec == tprec && tassoc == 'l'))
                    row[t] = (short)-p;
                else if (pprec == tprec && tassoc == 'n')
                    row[t] = ACT_ERROR;
            }
        }
        /* the rule reduced on most tokens becomes what the state does when nothing else is said */
        memset(count, 0, (size_t)(nprods + 1) * sizeof(int));
        for (i = 0; i < nterm; i++)
            if (row[i] < 0 && row[i] != ACT_ERROR)
                count[-row[i]]++;
        for (i = 1; i < nprods; i++)
            if (count[i] > count[best])
                best = i;
        defred[k] = (short)best;
        needla[k] = 0;
        for (i = 0; i < nterm; i++) {
            if (best && row[i] == -best)
                row[i] = 0;
            if (row[i])
                needla[k] = 1;
        }
        if (!best)
            needla[k] = 1;
        free(count);
    }
    if (rr || (sr && sr != expect_sr) || (!sr && expect_sr > 0))
        fprintf(stderr, "%s: warning: %d shift/reduce and %d reduce/reduce conflicts\n", yname, sr, rr);

    /* the header */
    if (code_req.len)
        buf_put(&hdr, code_req.p, code_req.len);
    put_tokens(&hdr);
    if (def_name) {
        Buf h;
        Buf save = yo;
        memset(&h, 0, sizeof h);
        yo = h;
        y("/* Tokens and types of the parser made by mcc yacc. */\n#ifndef YY_YY_PARSER_HEADER\n#define YY_YY_PARSER_HEADER\n");
        y("#ifndef YYDEBUG\n#define YYDEBUG 0\n#endif\n#if YYDEBUG\nextern int yydebug;\n#endif\n");
        buf_put(&yo, hdr.p, hdr.len);
        y("extern YYSTYPE yylval;\n");
        if (want_locations)
            y("extern YYLTYPE yylloc;\n");
        y("int yyparse(void);\n#endif\n");
        os_write_file(def_name, yo.p, yo.len, 0);
        buf_free(&yo);
        yo = save;
    }

    /* the parser */
    y("/* A parser generated by mcc yacc. */\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n#define YYBISON 30802\n");
    buf_put(&yo, code_top.p, code_top.len);
    buf_put(&yo, code_pre.p, code_pre.len);
    buf_put(&yo, hdr.p, hdr.len);
    buf_put(&yo, code_post.p, code_post.len);
    y("#define YYLSP_NEEDED ");
    ynum(want_locations);
    y("\n");
    ylines(skel_DECL);
    y("#define YYACCEPTACT 32767\n#define YYERRACT (-32767)\n#define YYNCODES ");
    {
        int maxcode = 0, *tr;
        for (k = 0; k < nsyms; k++)
            if (syms[k].term && syms[k].code > maxcode)
                maxcode = syms[k].code;
        ynum(maxcode + 1);
        tr = (int *)xmalloc((size_t)(maxcode + 1) * sizeof(int));
        for (i = 0; i <= maxcode; i++)
            tr[i] = 2;
        for (k = 0; k < nsyms; k++)
            if (syms[k].term && syms[k].code >= 0)
                tr[syms[k].code] = syms[k].idx;
        y("\nstatic const short yytranslate[] = {");
        for (i = 0; i <= maxcode; i++) {
            y(i % 20 ? " " : "\n    ");
            ynum(tr[i]);
            y(",");
        }
        y("\n};\n");
        free(tr);
    }
    y("static const short yytable[");
    ynum(nstates);
    y("][");
    ynum(nterm);
    y("] = {\n");
    for (k = 0; k < nstates; k++) {
        y("    {");
        for (i = 0; i < nterm; i++) {
            ynum(table[(size_t)k * (size_t)nterm + (size_t)i]);
            y(i + 1 < nterm ? "," : "");
        }
        y("},\n");
    }
    y("};\nstatic const short yygoto[");
    ynum(nstates);
    y("][");
    ynum(nnt);
    y("] = {\n");
    for (k = 0; k < nstates; k++) {
        y("    {");
        for (i = 0; i < nnt; i++) {
            ynum(gototab[(size_t)k * (size_t)nnt + (size_t)i]);
            y(i + 1 < nnt ? "," : "");
        }
        y("},\n");
    }
    y("};\nstatic const short yydefred[] = {");
    for (k = 0; k < nstates; k++) {
        y(k % 20 ? " " : "\n    ");
        ynum(defred[k]);
        y(",");
    }
    y("\n};\nstatic const unsigned char yyneedla[] = {");
    for (k = 0; k < nstates; k++) {
        y(k % 30 ? " " : "\n    ");
        ynum(needla[k]);
        y(",");
    }
    y("\n};\nstatic const short yystos[] = {");
    for (k = 0; k < nstates; k++) {
        int a = states[k].access;
        y(k % 20 ? " " : "\n    ");
        ynum(syms[a].term ? syms[a].idx : nterm + syms[a].idx);
        y(",");
    }
    y("\n};\nstatic const short yyr1[] = {");
    for (k = 0; k < nprods; k++) {
        y(k % 20 ? " " : "\n    ");
        ynum(syms[prods[k].lhs].idx);
        y(",");
    }
    y("\n};\nstatic const short yyr2[] = {");
    for (k = 0; k < nprods; k++) {
        y(k % 20 ? " " : "\n    ");
        ynum(k ? prods[k].n : 1);
        y(",");
    }
    y("\n};\n");
    y("static void yydestruct(int yykind, YYSTYPE *yyvaluep YYLOCARG(YYLTYPE *yylocationp))\n{\n    (void)yyvaluep;\n");
    if (want_locations)
        y("    (void)yylocationp;\n");
    y("    switch (yykind) {\n");
    for (k = 0; k < nsyms; k++)
        if (syms[k].destructor) {
            y("    case ");
            ynum(syms[k].term ? syms[k].idx : nterm + syms[k].idx);
            y(": {");
            put_action(dtors[syms[k].destructor - 1].code, k, 0, 0, 1);
            y("} break;\n");
        }
    y("    default:\n        break;\n    }\n}\n");
    ylines(skel_PARSE);
    for (k = 1; k < nprods; k++) {
        Prod *p = &prods[k];
        if (!p->action)
            continue;
        y("        case ");
        ynum(k);
        y(": {");
        if (p->ctx)
            put_action(p->action, -1, p->ctx, p->nctx, 0);
        else
            put_action(p->action, p->lhs, p->rhs, p->n, 0);
        y("} break;\n");
    }
    ylines(skel_PARSE2);
    buf_put(&yo, code_tail.p, code_tail.len);
    os_write_file(out_name, yo.p, yo.len, 0);
    return 0;
}
