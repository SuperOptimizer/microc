/* mcc make: a make in the manner of GNU make, enough of it to build large
   projects written for that program (functions, pattern rules with chains,
   target-specific variables, VPATH, included makefiles that are themselves
   remade, parallel jobs shared with the makes it starts). There are no
   built-in rules. */
#include "mcc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- variables ---- */

enum { O_UNDEF, O_DEFAULT, O_ENV, O_FILE, O_CMDLINE, O_OVERRIDE, O_AUTO };

typedef struct Var {
    const char *name;
    char *value;
    u8 recursive, origin, exported; /* exported: 1 yes, 2 no, 0 unsaid */
    u8 busy;
    struct Var *next;
} Var;

/* A variable set for one target (or for the targets a pattern matches). */
typedef struct TVar {
    const char *name;
    char *value;
    int op; /* '=' ':' '+' '?' */
    struct TVar *next;
} TVar;

typedef struct Recipe {
    Buf text;
    const char *file;
    int line;
    int used; /* some line was given */
} Recipe;

typedef struct Target Target;
typedef struct Dep {
    Target *t;
    int order_only;
    struct Dep *next;
} Dep;

enum { S_NEW, S_DEPS, S_RUNNING, S_DONE };

struct Target {
    const char *name;
    const char *path; /* where the file was found: the name, or under a VPATH directory */
    Dep *deps, **deps_tail, *cursor;
    Recipe *recipe;
    TVar *vars, **vars_tail;
    const char *stem;
    char *second; /* prerequisites still to be expanded, when the target is considered */
    Target *also_of; /* made by the recipe of that target */
    Target *parent;  /* on whose behalf it is being made: its variables are inherited */
    i64 mtime;
    u8 mtime_known, phony, precious, is_target, mentioned, state, failed, searching, double_colon, ran, tried, visiting, ignore;
    long walk;
    i64 old_mtime;
    char **env;
    char **cmds;
    int ncmds, cmd_i, pid, has_token;
    Target *next;
};

typedef struct PatRule {
    char **targets;
    int ntargets;
    char **deps;
    int ndeps, norder; /* the order-only ones come last */
    int second;        /* the prerequisites are expanded again when the rule is tried */
    Recipe *recipe;
    struct PatRule *next;
} PatRule;

typedef struct PatVar {
    const char *pattern;
    TVar *v;
    struct PatVar *next;
} PatVar;

typedef struct Include {
    const char *name;
    int optional, found;
    struct Include *next;
} Include;

#define VHASH 4096
static Var *vars[VHASH];
static Target *targets[VHASH];
static PatRule *pat_rules, **pat_tail = &pat_rules;
static PatVar *pat_vars, **pat_vars_tail = &pat_vars;
static Include *includes, **includes_tail = &includes;
static Target *scope;        /* the target whose variables are in force */
static Target *default_goal;
static long var_generation;  /* counts changes, for what is cached from variables */

static int opt_keep_going, opt_silent, opt_dry, opt_always, opt_print_dir = -1, opt_jobs = 1, opt_ignore;
static int second_expansion, delete_on_error, export_all, make_level;
static const char *self_path; /* how to run this program again */
static char **self_argv, **self_envp;
static const char *include_dirs[32];
static int ninclude_dirs;
static Buf overrides; /* VAR=value words of the command line */
static const char *cur_file = "make";
static int cur_line;

static void cleanup(void);

static void fatal(const char *fmt, const char *a, const char *b)
{
    fflush(stdout);
    if (cur_line)
        fprintf(stderr, "%s:%d: *** ", cur_file, cur_line);
    else if (make_level)
        fprintf(stderr, "make[%d]: *** ", make_level);
    else
        fprintf(stderr, "make: *** ");
    fprintf(stderr, fmt, a, b);
    fprintf(stderr, ".  Stop.\n");
    cleanup();
    os_exit(2);
}

static u32 hash_str(const char *s, size_t n)
{
    u32 h = 2166136261U;
    while (n--)
        h = (h ^ (u8)*s++) * 16777619U;
    return h;
}

static char *save(const char *s, size_t n)
{
    char *p = (char *)xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

static char *save_str(const char *s) { return save(s, strlen(s)); }

static Var *var_find(const char *name, size_t n)
{
    Var *v;
    for (v = vars[hash_str(name, n) % VHASH]; v; v = v->next)
        if (strncmp(v->name, name, n) == 0 && !v->name[n])
            return v;
    return 0;
}

static Var *var_get(const char *name, size_t n)
{
    Var *v = var_find(name, n);
    u32 h;
    if (v)
        return v;
    h = hash_str(name, n) % VHASH;
    v = (Var *)xmalloc(sizeof(Var));
    memset(v, 0, sizeof *v);
    v->name = save(name, n);
    v->value = save_str("");
    v->next = vars[h];
    vars[h] = v;
    return v;
}

/* Sets a variable unless something of higher standing set it before. */
static Var *var_set(const char *name, const char *value, int recursive, int origin)
{
    Var *v = var_get(name, strlen(name));
    if ((v->origin == O_CMDLINE && origin != O_CMDLINE && origin != O_OVERRIDE) ||
        (v->origin == O_OVERRIDE && origin != O_OVERRIDE && origin != O_CMDLINE && origin != O_AUTO))
        return v;
    if (v->origin == O_ENV && origin == O_FILE)
        v->exported = v->exported ? v->exported : 1; /* what came from the environment goes back to it */
    free(v->value);
    v->value = save_str(value);
    v->recursive = (u8)recursive;
    v->origin = (u8)origin;
    var_generation++;
    return v;
}

static const char *var_value(const char *name)
{
    Var *v = var_find(name, strlen(name));
    return v ? v->value : "";
}

/* ---- words ---- */

static int is_space(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

/* The next word of *s, or null; *len gets its length. */
static const char *next_word(const char **s, size_t *len)
{
    const char *p = *s, *w;
    while (is_space((u8)*p))
        p++;
    if (!*p)
        return 0;
    w = p;
    while (*p && !is_space((u8)*p))
        p++;
    *len = (size_t)(p - w);
    *s = p;
    return w;
}

static void put_word(Buf *out, size_t start, const char *w, size_t n)
{
    if (out->len > start)
        buf_u8(out, ' ');
    buf_put(out, w, n);
}

static void put_str(Buf *out, const char *s) { buf_put(out, s, strlen(s)); }

static char *finish(Buf *b)
{
    buf_u8(b, 0);
    return (char *)b->p;
}

static char *strip(char *s)
{
    char *e;
    while (is_space((u8)*s))
        s++;
    e = s + strlen(s);
    while (e > s && is_space((u8)e[-1]))
        e--;
    *e = 0;
    return s;
}

/* Does the word match the pattern (one %)? The stem is given back. */
static int pat_match(const char *pat, const char *w, size_t wn, const char **stem, size_t *stem_len)
{
    const char *pc = strchr(pat, '%');
    size_t pre, suf;
    if (!pc)
        return strlen(pat) == wn && memcmp(pat, w, wn) == 0 && (*stem = w, *stem_len = 0, 1);
    pre = (size_t)(pc - pat);
    suf = strlen(pc + 1);
    if (wn < pre + suf || memcmp(pat, w, pre) != 0 || memcmp(pc + 1, w + wn - suf, suf) != 0)
        return 0;
    *stem = w + pre;
    *stem_len = wn - pre - suf;
    return 1;
}

static void pat_subst(Buf *out, const char *repl, const char *stem, size_t stem_len)
{
    const char *pc = strchr(repl, '%');
    if (!pc) {
        put_str(out, repl);
        return;
    }
    buf_put(out, repl, (size_t)(pc - repl));
    buf_put(out, stem, stem_len);
    put_str(out, pc + 1);
}

/* ---- expansion ---- */

static void expand(Buf *out, const char *s);
static void eval_text(char *text, const char *file, int line);
static Target *target_get(const char *name);
static void auto_var(Buf *out, int c, int part);

static char *xexpand(const char *s)
{
    Buf b;
    memset(&b, 0, sizeof b);
    expand(&b, s);
    return finish(&b);
}

/* arguments of the user function being called: $(0), $(1), ... */
static char **call_args;
static int ncall_args;

static int tvar_value(Buf *out, Target *t, const char *name, size_t n);

static void var_expand(Buf *out, const char *name, size_t n)
{
    Var *v;
    if (n == 1 || (n == 2 && (name[1] == 'D' || name[1] == 'F'))) {
        if (strchr("@<^+?*|%", name[0]) && scope) {
            auto_var(out, name[0], n == 2 ? name[1] : 0);
            return;
        }
        if (name[0] >= '0' && name[0] <= '9' && n == 1 && call_args) {
            if (name[0] - '0' < ncall_args)
                put_str(out, call_args[name[0] - '0']);
            return;
        }
    } else if (call_args && name[0] >= '1' && name[0] <= '9') {
        size_t k;
        int idx = 0;
        for (k = 0; k < n && name[k] >= '0' && name[k] <= '9'; k++)
            idx = idx * 10 + (name[k] - '0');
        if (k == n) {
            if (idx < ncall_args)
                put_str(out, call_args[idx]);
            return;
        }
    }
    if (scope && tvar_value(out, scope, name, n))
        return;
    v = var_find(name, n);
    if (!v)
        return;
    if (!v->recursive) {
        put_str(out, v->value);
        return;
    }
    if (v->busy)
        fatal("Recursive variable '%s' references itself (eventually)", v->name, 0);
    v->busy = 1;
    expand(out, v->value);
    v->busy = 0;
}

/* The value a target gives a variable, if it or what it inherits from says anything. */
static int tvar_value(Buf *out, Target *t, const char *name, size_t n)
{
    TVar *list[64], *tv;
    int nlist = 0, k, first = 0, base_done = 0;
    Target *p;
    PatVar *pv;
    size_t start = out->len;
    /* innermost last: what the ancestors say, then the patterns, then the target itself */
    for (p = t; p && nlist < 60; p = p->parent) {
        int from = nlist, to;
        for (pv = pat_vars; pv && nlist < 60; pv = pv->next) {
            const char *stem;
            size_t sl;
            if (strncmp(pv->v->name, name, n) == 0 && !pv->v->name[n] && pat_match(pv->pattern, p->name, strlen(p->name), &stem, &sl))
                list[nlist++] = pv->v;
        }
        for (tv = p->vars; tv && nlist < 60; tv = tv->next)
            if (strncmp(tv->name, name, n) == 0 && !tv->name[n])
                list[nlist++] = tv;
        /* this level was collected in order; the levels are from the inside out, so turn each around */
        for (to = nlist - 1; from < to; from++, to--) {
            tv = list[from];
            list[from] = list[to];
            list[to] = tv;
        }
    }
    if (!nlist)
        return 0;
    /* list[0] is the innermost, latest setting: find where the value starts */
    for (first = 0; first < nlist; first++)
        if (list[first]->op == '=' || list[first]->op == ':')
            break;
    {
        Target *saved = scope;
        Var *v = var_find(name, n);
        if (v && v->origin == O_CMDLINE) /* the command line wins over what makefiles say */
            return 0;
        if (first == nlist) { /* only additions: they add to the global value */
            first = nlist - 1;
            if (list[first]->op == '?' && v && v->origin != O_UNDEF)
                ;
            scope = 0;
            if (v) {
                if (v->recursive)
                    expand(out, v->value);
                else
                    put_str(out, v->value);
            }
            scope = saved;
            base_done = v && v->origin != O_UNDEF;
            first = nlist;
        }
        for (k = first < nlist ? first : nlist - 1; k >= 0; k--) {
            tv = list[k];
            if (tv->op == '?') {
                if (base_done || out->len > start)
                    continue;
                expand(out, tv->value);
            } else if (tv->op == '+') {
                if (out->len > start)
                    buf_u8(out, ' ');
                expand(out, tv->value);
            } else if (tv->op == ':') {
                out->len = start;
                put_str(out, tv->value);
            } else {
                out->len = start;
                expand(out, tv->value);
            }
            base_done = 1;
        }
    }
    return 1;
}

static const char *match_paren(const char *s, int open, int close)
{
    /* s is just past the opening one; returns the matching closer */
    int depth = 1;
    for (; *s; s++) {
        if (*s == open)
            depth++;
        else if (*s == close && --depth == 0)
            return s;
    }
    return 0;
}

static int call_function(Buf *out, const char *name, size_t nlen, const char *args, const char *end, int open, int close);

static void expand(Buf *out, const char *s)
{
    for (;;) {
        const char *d = strchr(s, '$');
        if (!d) {
            put_str(out, s);
            return;
        }
        buf_put(out, s, (size_t)(d - s));
        s = d + 1;
        if (*s == '$') {
            buf_u8(out, '$');
            s++;
        } else if (*s == '(' || *s == '{') {
            int open = *s, close = open == '(' ? ')' : '}';
            const char *e = match_paren(s + 1, open, close), *p, *colon = 0;
            char *name = 0;
            size_t nlen;
            if (!e)
                fatal("unterminated variable reference", 0, 0);
            s++;
            for (p = s; p < e && !is_space((u8)*p) && *p != '$' && *p != ','; p++)
                ;
            if (p < e && is_space((u8)*p) && call_function(out, s, (size_t)(p - s), p + 1, e, open, close)) {
                s = e + 1;
                continue;
            }
            /* a variable; its name may itself hold references, and a substitution may follow */
            nlen = (size_t)(e - s);
            if (memchr(s, '$', nlen)) {
                char *raw = save(s, nlen);
                name = xexpand(raw);
                free(raw);
                s = name;
                nlen = strlen(name);
            }
            {
                int depth = 0;
                for (p = s; p < s + nlen; p++) {
                    if (*p == '(' || *p == '{')
                        depth++;
                    else if (*p == ')' || *p == '}')
                        depth--;
                    else if (*p == ':' && !depth && memchr(p, '=', (size_t)(s + nlen - p))) {
                        colon = p;
                        break;
                    }
                }
            }
            if (colon) {
                /* $(VAR:a=b) */
                const char *eq = (const char *)memchr(colon, '=', (size_t)(s + nlen - colon)), *w;
                char *from = save(colon + 1, (size_t)(eq - colon - 1)), *to = save(eq + 1, (size_t)(s + nlen - eq - 1));
                Buf val;
                const char *vp;
                size_t wl, start = out->len;
                memset(&val, 0, sizeof val);
                var_expand(&val, s, (size_t)(colon - s));
                vp = finish(&val);
                if (!strchr(from, '%')) {
                    char *f2 = (char *)xmalloc(strlen(from) + 2), *t2 = (char *)xmalloc(strlen(to) + 2);
                    sprintf(f2, "%%%s", from);
                    sprintf(t2, "%%%s", to);
                    free(from);
                    free(to);
                    from = f2;
                    to = t2;
                }
                while ((w = next_word(&vp, &wl)) != 0) {
                    const char *stem;
                    size_t sl;
                    if (out->len > start)
                        buf_u8(out, ' ');
                    if (pat_match(from, w, wl, &stem, &sl))
                        pat_subst(out, to, stem, sl);
                    else
                        buf_put(out, w, wl);
                }
                free(from);
                free(to);
                buf_free(&val);
            } else {
                var_expand(out, s, nlen);
            }
            free(name);
            s = e + 1;
        } else if (*s) {
            var_expand(out, s, 1);
            s++;
        } else {
            buf_u8(out, '$');
        }
    }
}

/* ---- functions ---- */

static const char *dep_path(Target *t) { return t->path ? t->path : t->name; }

static void put_part(Buf *out, size_t start, const char *w, int part)
{
    const char *slash = strrchr(w, '/');
    if (out->len > start)
        buf_u8(out, ' ');
    if (!part)
        put_str(out, w);
    else if (part == 'F')
        put_str(out, slash ? slash + 1 : w);
    else if (!slash)
        buf_u8(out, '.');
    else if (slash == w)
        buf_u8(out, '/');
    else
        buf_put(out, w, (size_t)(slash - w));
}

static i64 target_mtime(Target *t);

static void auto_var(Buf *out, int c, int part)
{
    Target *t = scope;
    size_t start = out->len;
    Dep *d, *e;
    switch (c) {
    case '@':
        put_part(out, start, t->name, part);
        break;
    case '*':
        if (t->stem)
            put_part(out, start, t->stem, part);
        break;
    case '<':
        for (d = t->deps; d; d = d->next)
            if (!d->order_only) {
                put_part(out, start, dep_path(d->t), part);
                break;
            }
        break;
    case '^': case '+': case '|': case '?':
        for (d = t->deps; d; d = d->next) {
            if ((c == '|') != (d->order_only != 0))
                continue;
            if (c == '^' || c == '|') { /* each once */
                for (e = t->deps; e != d; e = e->next)
                    if (e->t == d->t && (e->order_only != 0) == (d->order_only != 0))
                        break;
                if (e != d)
                    continue;
            }
            if (c == '?' && target_mtime(t) && target_mtime(d->t) && target_mtime(d->t) <= target_mtime(t))
                continue;
            put_part(out, start, dep_path(d->t), part);
        }
        break;
    }
}

typedef struct Names {
    char **v;
    u32 n;
} Names;

static void names_add(Names *l, char *s)
{
    if ((l->n & (l->n + 1)) == 0)
        l->v = (char **)xrealloc(l->v, (2 * (size_t)l->n + 2) * sizeof(char *));
    l->v[l->n++] = s;
}

static int cmp_names(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static int glob_one(const char *p, const char *s)
{
    for (;; p++, s++) {
        if (*p == '*') {
            while (*p == '*')
                p++;
            if (!*p)
                return 1;
            for (; *s; s++)
                if (glob_one(p, s))
                    return 1;
            return 0;
        }
        if (*p == '[' && *s) {
            const char *q = p + 1;
            int neg = *q == '!' || *q == '^', hit = 0;
            if (neg)
                q++;
            for (; *q && (*q != ']' || q == p + 1 + neg); q++) {
                if (q[1] == '-' && q[2] && q[2] != ']') {
                    if (*s >= q[0] && *s <= q[2])
                        hit = 1;
                    q += 2;
                } else if (*q == *s)
                    hit = 1;
            }
            if (!*q || hit == neg)
                return 0;
            p = q;
            continue;
        }
        if (!*s || (*p != '?' && *p != *s))
            return !*p && !*s;
    }
}

typedef struct GlobCtx {
    const char *dir, *pat, *rest;
    Names *out;
} GlobCtx;

static void glob_into(const char *dir, const char *pat, Names *out);

static void glob_entry(const char *name, void *vctx)
{
    GlobCtx *g = (GlobCtx *)vctx;
    char *path;
    if ((name[0] == '.' && g->pat[0] != '.') || !glob_one(g->pat, name))
        return;
    path = (char *)xmalloc(strlen(g->dir) + strlen(name) + 2);
    sprintf(path, "%s%s", g->dir, name);
    if (g->rest) {
        char *sub = (char *)xmalloc(strlen(path) + 2);
        int is_dir = 0;
        if (os_mtime(path, &is_dir) && is_dir) {
            sprintf(sub, "%s/", path);
            glob_into(sub, g->rest, g->out);
        }
        free(sub);
        free(path);
    } else {
        names_add(g->out, path);
    }
}

/* Files matching pat, taken to lie under dir (a prefix ending in a slash, or empty). */
static void glob_into(const char *dir, const char *pat, Names *out)
{
    const char *slash = strchr(pat, '/');
    char *comp = slash ? save(pat, (size_t)(slash - pat)) : save_str(pat);
    const char *rest = slash ? slash + 1 : 0;
    while (rest && *rest == '/')
        rest++;
    if (rest && !*rest)
        rest = 0;
    if (!strpbrk(comp, "*?[")) {
        char *path = (char *)xmalloc(strlen(dir) + strlen(comp) + 2);
        sprintf(path, "%s%s%s", dir, comp, rest || (slash && !rest) ? "/" : "");
        if (rest)
            glob_into(path, rest, out);
        else if (os_exists(path))
            names_add(out, path), path = 0;
        free(path);
    } else {
        GlobCtx g;
        g.dir = dir;
        g.pat = comp;
        g.rest = rest;
        g.out = out;
        os_list_dir(*dir ? dir : ".", glob_entry, &g);
    }
    free(comp);
}

static void glob_words(Buf *out, size_t start, const char *pats, int keep_unmatched)
{
    const char *w;
    size_t wl;
    while ((w = next_word(&pats, &wl)) != 0) {
        char *pat = save(w, wl);
        Names found;
        u32 k;
        found.v = 0;
        found.n = 0;
        if (!strpbrk(pat, "*?[")) {
            if (keep_unmatched || os_exists(pat))
                put_word(out, start, pat, wl);
            free(pat);
            continue;
        }
        if (pat[0] == '/')
            glob_into("/", pat + 1, &found);
        else
            glob_into("", pat, &found);
        if (found.n > 1)
            qsort(found.v, found.n, sizeof(char *), cmp_names);
        for (k = 0; k < found.n; k++) {
            put_word(out, start, found.v[k], strlen(found.v[k]));
            free(found.v[k]);
        }
        if (!found.n && keep_unmatched)
            put_word(out, start, pat, wl);
        free(found.v);
        free(pat);
    }
}

static char *cwd;

/* The path made absolute and tidied, without looking at the file system. */
static void abs_path(Buf *out, const char *w, size_t wl)
{
    Buf b;
    const char *p, *e;
    size_t base;
    memset(&b, 0, sizeof b);
    if (w[0] != '/') {
        put_str(&b, cwd);
        buf_u8(&b, '/');
    }
    buf_put(&b, w, wl);
    buf_u8(&b, 0);
    base = out->len;
    for (p = (const char *)b.p; *p;) {
        while (*p == '/')
            p++;
        e = p;
        while (*e && *e != '/')
            e++;
        if (e == p)
            break;
        if (e - p == 2 && p[0] == '.' && p[1] == '.') {
            while (out->len > base && out->p[out->len - 1] != '/')
                out->len--;
            if (out->len > base)
                out->len--;
        } else if (!(e - p == 1 && p[0] == '.')) {
            buf_u8(out, '/');
            buf_put(out, p, (size_t)(e - p));
        }
        p = e;
    }
    if (out->len == base)
        buf_u8(out, '/');
    buf_free(&b);
}

static const char *shell_path(void);
static char **make_env(int for_recipe);

static void run_shell(Buf *out, const char *cmd)
{
    int fd[2], id, code = 0;
    char *argv[4];
    size_t start = out->len, k;
    if (os_pipe(fd) != 0)
        fatal("cannot create a pipe", 0, 0);
    argv[0] = (char *)shell_path();
    argv[1] = (char *)"-c";
    argv[2] = (char *)cmd;
    argv[3] = 0;
    id = os_run(argv, make_env(0), fd[1], fd[0]);
    os_close(fd[1]);
    for (;;) {
        long n;
        buf_reserve(out, 4096);
        n = os_read(fd[0], out->p + out->len, 4096);
        if (n <= 0)
            break;
        out->len += (size_t)n;
    }
    os_close(fd[0]);
    code = os_wait(id);
    {
        char num[16];
        sprintf(num, "%d", code);
        var_set(".SHELLSTATUS", num, 0, O_OVERRIDE);
    }
    while (out->len > start && out->p[out->len - 1] == '\n')
        out->len--;
    for (k = start; k < out->len; k++)
        if (out->p[k] == '\n' || out->p[k] == '\r')
            out->p[k] = ' ';
}

enum { F_SUBST, F_PATSUBST, F_STRIP, F_FINDSTRING, F_FILTER, F_FILTER_OUT, F_SORT, F_WORD, F_WORDLIST, F_WORDS,
       F_FIRSTWORD, F_LASTWORD, F_DIR, F_NOTDIR, F_SUFFIX, F_BASENAME, F_ADDSUFFIX, F_ADDPREFIX, F_JOIN, F_WILDCARD,
       F_REALPATH, F_ABSPATH, F_IF, F_OR, F_AND, F_FOREACH, F_CALL, F_VALUE, F_EVAL, F_ORIGIN, F_FLAVOR, F_SHELL,
       F_ERROR, F_WARNING, F_INFO, F_FILE };

static const struct {
    const char *name;
    int id, max; /* max arguments; 0: any number */
    int lazy;
} funcs[] = {
    {"subst", F_SUBST, 3, 0}, {"patsubst", F_PATSUBST, 3, 0}, {"strip", F_STRIP, 1, 0},
    {"findstring", F_FINDSTRING, 2, 0}, {"filter", F_FILTER, 2, 0}, {"filter-out", F_FILTER_OUT, 2, 0},
    {"sort", F_SORT, 1, 0}, {"word", F_WORD, 2, 0}, {"wordlist", F_WORDLIST, 3, 0}, {"words", F_WORDS, 1, 0},
    {"firstword", F_FIRSTWORD, 1, 0}, {"lastword", F_LASTWORD, 1, 0}, {"dir", F_DIR, 1, 0},
    {"notdir", F_NOTDIR, 1, 0}, {"suffix", F_SUFFIX, 1, 0}, {"basename", F_BASENAME, 1, 0},
    {"addsuffix", F_ADDSUFFIX, 2, 0}, {"addprefix", F_ADDPREFIX, 2, 0}, {"join", F_JOIN, 2, 0},
    {"wildcard", F_WILDCARD, 1, 0}, {"realpath", F_REALPATH, 1, 0}, {"abspath", F_ABSPATH, 1, 0},
    {"if", F_IF, 3, 1}, {"or", F_OR, 0, 1}, {"and", F_AND, 0, 1}, {"foreach", F_FOREACH, 3, 1},
    {"call", F_CALL, 0, 0}, {"value", F_VALUE, 1, 0}, {"eval", F_EVAL, 1, 0}, {"origin", F_ORIGIN, 1, 0},
    {"flavor", F_FLAVOR, 1, 0}, {"shell", F_SHELL, 1, 0}, {"error", F_ERROR, 1, 0}, {"warning", F_WARNING, 1, 0},
    {"info", F_INFO, 1, 0}, {"file", F_FILE, 2, 0}};
#define NFUNCS (sizeof funcs / sizeof funcs[0])

static int find_func(const char *name, size_t n)
{
    u32 k;
    for (k = 0; k < NFUNCS; k++)
        if (strlen(funcs[k].name) == n && memcmp(funcs[k].name, name, n) == 0)
            return (int)k;
    return -1;
}

static int filter_hit(const char *pats, const char *w, size_t wl)
{
    const char *p;
    size_t pl;
    while ((p = next_word(&pats, &pl)) != 0) {
        const char *pc = (const char *)memchr(p, '%', pl);
        if (!pc) {
            if (pl == wl && memcmp(p, w, wl) == 0)
                return 1;
        } else {
            size_t pre = (size_t)(pc - p), suf = pl - pre - 1;
            if (wl >= pre + suf && memcmp(p, w, pre) == 0 && memcmp(pc + 1, w + wl - suf, suf) == 0)
                return 1;
        }
    }
    return 0;
}

static void do_function(Buf *out, int id, char **a, int n);

static int call_function(Buf *out, const char *name, size_t nlen, const char *args, const char *end, int open, int close)
{
    int f = find_func(name, nlen), n = 0, k, depth = 0;
    char *argv[64];
    const char *p, *start;
    if (f < 0)
        return 0;
    while (args < end && is_space((u8)*args))
        args++;
    /* split at the commas, as far as the function takes arguments */
    start = args;
    for (p = args; p <= end; p++) {
        if (p < end && *p == '$' && p + 1 < end && (p[1] == '(' || p[1] == '{')) {
            const char *m = match_paren(p + 2, p[1], p[1] == '(' ? ')' : '}');
            if (m && m < end) {
                p = m;
                continue;
            }
        }
        if (p < end && *p == open)
            depth++;
        else if (p < end && *p == close)
            depth--;
        else if (p == end || (*p == ',' && !depth && (!funcs[f].max || n + 1 < funcs[f].max))) {
            if (n == 63)
                fatal("too many arguments to function '%s'", funcs[f].name, 0);
            argv[n++] = save(start, (size_t)(p - start));
            start = p + 1;
        }
    }
    if (!funcs[f].lazy)
        for (k = 0; k < n; k++) {
            char *x = xexpand(argv[k]);
            free(argv[k]);
            argv[k] = x;
        }
    argv[n] = 0;
    do_function(out, funcs[f].id, argv, n);
    for (k = 0; k < n; k++)
        free(argv[k]);
    return 1;
}

static void do_function(Buf *out, int id, char **a, int n)
{
    size_t start = out->len, wl;
    const char *w, *s = n ? a[0] : "";
    char *x;
    int k;
    Var *v;
#define NEED(c) if (n < (c)) fatal("insufficient number of arguments to a function", 0, 0)
    switch (id) {
    case F_SUBST: {
        size_t fl;
        NEED(3);
        fl = strlen(a[0]);
        s = a[2];
        if (!fl) {
            put_str(out, s);
            put_str(out, a[1]);
            break;
        }
        for (;;) {
            const char *hit = strstr(s, a[0]);
            if (!hit) {
                put_str(out, s);
                break;
            }
            buf_put(out, s, (size_t)(hit - s));
            put_str(out, a[1]);
            s = hit + fl;
        }
        break;
    }
    case F_PATSUBST:
        NEED(3);
        s = a[2];
        while ((w = next_word(&s, &wl)) != 0) {
            const char *stem;
            size_t sl;
            if (out->len > start)
                buf_u8(out, ' ');
            if (pat_match(a[0], w, wl, &stem, &sl))
                pat_subst(out, a[1], stem, sl);
            else
                buf_put(out, w, wl);
        }
        break;
    case F_STRIP:
        while ((w = next_word(&s, &wl)) != 0)
            put_word(out, start, w, wl);
        break;
    case F_FINDSTRING:
        NEED(2);
        if (strstr(a[1], a[0]))
            put_str(out, a[0]);
        break;
    case F_FILTER:
    case F_FILTER_OUT:
        NEED(2);
        s = a[1];
        while ((w = next_word(&s, &wl)) != 0)
            if (filter_hit(a[0], w, wl) == (id == F_FILTER))
                put_word(out, start, w, wl);
        break;
    case F_SORT: {
        Names l;
        u32 i;
        l.v = 0;
        l.n = 0;
        while ((w = next_word(&s, &wl)) != 0)
            names_add(&l, save(w, wl));
        if (l.n > 1)
            qsort(l.v, l.n, sizeof(char *), cmp_names);
        for (i = 0; i < l.n; i++) {
            if (!i || strcmp(l.v[i], l.v[i - 1]) != 0)
                put_word(out, start, l.v[i], strlen(l.v[i]));
        }
        for (i = 0; i < l.n; i++)
            free(l.v[i]);
        free(l.v);
        break;
    }
    case F_WORD:
    case F_WORDLIST: {
        long from, to, i = 0;
        NEED(id == F_WORD ? 2 : 3);
        from = atol(a[0]);
        to = id == F_WORD ? from : atol(a[1]);
        if (from < 1)
            fatal("first argument to 'word' function must be greater than 0", 0, 0);
        s = a[id == F_WORD ? 1 : 2];
        while ((w = next_word(&s, &wl)) != 0)
            if (++i >= from && i <= to)
                put_word(out, start, w, wl);
        break;
    }
    case F_WORDS: {
        long count = 0;
        char num[24];
        while (next_word(&s, &wl))
            count++;
        sprintf(num, "%ld", count);
        put_str(out, num);
        break;
    }
    case F_FIRSTWORD:
        if ((w = next_word(&s, &wl)) != 0)
            buf_put(out, w, wl);
        break;
    case F_LASTWORD: {
        const char *last = 0;
        size_t ll = 0;
        while ((w = next_word(&s, &wl)) != 0)
            last = w, ll = wl;
        if (last)
            buf_put(out, last, ll);
        break;
    }
    case F_DIR:
    case F_NOTDIR:
    case F_SUFFIX:
    case F_BASENAME:
        while ((w = next_word(&s, &wl)) != 0) {
            const char *slash = 0, *dot = 0, *q;
            for (q = w; q < w + wl; q++) {
                if (*q == '/')
                    slash = q, dot = 0;
                else if (*q == '.')
                    dot = q;
            }
            if (id == F_DIR) {
                if (slash)
                    put_word(out, start, w, (size_t)(slash - w + 1));
                else
                    put_word(out, start, "./", 2);
            } else if (id == F_NOTDIR) {
                if (!slash)
                    put_word(out, start, w, wl);
                else if (slash + 1 < w + wl)
                    put_word(out, start, slash + 1, (size_t)(w + wl - slash - 1));
            } else if (id == F_SUFFIX) {
                if (dot)
                    put_word(out, start, dot, (size_t)(w + wl - dot));
            } else {
                put_word(out, start, w, dot ? (size_t)(dot - w) : wl);
            }
        }
        break;
    case F_ADDSUFFIX:
    case F_ADDPREFIX:
        NEED(2);
        s = a[1];
        while ((w = next_word(&s, &wl)) != 0) {
            if (out->len > start)
                buf_u8(out, ' ');
            if (id == F_ADDPREFIX)
                put_str(out, a[0]);
            buf_put(out, w, wl);
            if (id == F_ADDSUFFIX)
                put_str(out, a[0]);
        }
        break;
    case F_JOIN: {
        const char *t, *w2;
        size_t l2;
        NEED(2);
        t = a[1];
        for (;;) {
            w = next_word(&s, &wl);
            w2 = next_word(&t, &l2);
            if (!w && !w2)
                break;
            if (out->len > start)
                buf_u8(out, ' ');
            if (w)
                buf_put(out, w, wl);
            if (w2)
                buf_put(out, w2, l2);
        }
        break;
    }
    case F_WILDCARD:
        glob_words(out, start, s, 0);
        break;
    case F_REALPATH:
    case F_ABSPATH:
        while ((w = next_word(&s, &wl)) != 0) {
            size_t at = out->len;
            if (out->len > start)
                buf_u8(out, ' ');
            abs_path(out, w, wl);
            if (id == F_REALPATH) {
                buf_u8(out, 0);
                out->len--;
                if (!os_exists((const char *)out->p + (out->len > at && at > start ? at + 1 : at)))
                    out->len = at;
            }
        }
        break;
    case F_IF:
        NEED(2);
        x = xexpand(a[0]);
        if (*strip(x))
            expand(out, a[1]);
        else if (n > 2)
            expand(out, a[2]);
        free(x);
        break;
    case F_OR:
        for (k = 0; k < n; k++) {
            x = xexpand(a[k]);
            if (*strip(x)) {
                put_str(out, strip(x));
                free(x);
                break;
            }
            free(x);
        }
        break;
    case F_AND:
        for (k = 0; k < n; k++) {
            x = xexpand(a[k]);
            if (!*strip(x)) {
                free(x);
                break;
            }
            if (k == n - 1)
                put_str(out, strip(x));
            free(x);
        }
        break;
    case F_FOREACH: {
        char *name, *list, *old_value;
        Var *fv;
        int old_rec, old_origin;
        NEED(3);
        name = xexpand(a[0]);
        list = xexpand(a[1]);
        fv = var_get(strip(name), strlen(strip(name)));
        old_value = fv->value;
        old_rec = fv->recursive;
        old_origin = fv->origin;
        fv->value = 0;
        s = list;
        while ((w = next_word(&s, &wl)) != 0) {
            free(fv->value);
            fv->value = save(w, wl);
            fv->recursive = 0;
            fv->origin = O_AUTO;
            if (out->len > start)
                buf_u8(out, ' ');
            expand(out, a[2]);
        }
        free(fv->value);
        fv->value = old_value;
        fv->recursive = (u8)old_rec;
        fv->origin = (u8)old_origin;
        free(name);
        free(list);
        break;
    }
    case F_CALL: {
        char **saved = call_args, *fname;
        int nsaved = ncall_args, f;
        NEED(1);
        fname = strip(a[0]);
        f = find_func(fname, strlen(fname));
        if (f >= 0) {
            do_function(out, funcs[f].id, a + 1, n - 1);
            break;
        }
        v = var_find(fname, strlen(fname));
        if (!v)
            break;
        call_args = a;
        ncall_args = n;
        if (v->recursive)
            expand(out, v->value);
        else
            put_str(out, v->value);
        call_args = saved;
        ncall_args = nsaved;
        break;
    }
    case F_VALUE:
        v = var_find(strip(a[0]), strlen(strip(a[0])));
        if (v)
            put_str(out, v->value);
        break;
    case F_EVAL: {
        const char *f = cur_file;
        int l = cur_line;
        eval_text(save_str(a[0]), cur_file, cur_line);
        cur_file = f;
        cur_line = l;
        break;
    }
    case F_ORIGIN: {
        static const char *const names[] = {"undefined", "default", "environment", "file", "command line", "override", "automatic"};
        s = strip(a[0]);
        if (strlen(s) == 1 && strchr("@<^+?*|%", *s))
            put_str(out, "automatic");
        else {
            v = var_find(s, strlen(s));
            put_str(out, names[v ? v->origin : O_UNDEF]);
        }
        break;
    }
    case F_FLAVOR:
        v = var_find(strip(a[0]), strlen(strip(a[0])));
        put_str(out, !v || v->origin == O_UNDEF ? "undefined" : v->recursive ? "recursive" : "simple");
        break;
    case F_SHELL:
        run_shell(out, s);
        break;
    case F_ERROR:
        fatal("%s", s, 0);
        break;
    case F_WARNING:
        fflush(stdout);
        fprintf(stderr, "%s:%d: %s\n", cur_file, cur_line, s);
        break;
    case F_INFO:
        puts(s);
        fflush(stdout);
        break;
    case F_FILE: {
        /* $(file >name,text) and $(file >>name,text) */
        const char *nm = s;
        int append = 0;
        while (*nm == '>')
            nm++, append++;
        if (append) {
            FILE *f;
            x = save_str(nm);
            f = fopen(strip(x), append > 1 ? "a" : "w");
            if (!f)
                fatal("cannot write %s", strip(x), 0);
            if (n > 1) {
                fputs(a[1], f);
                if (!*a[1] || a[1][strlen(a[1]) - 1] != '\n')
                    fputc('\n', f);
            }
            fclose(f);
            free(x);
        }
        break;
    }
    }
#undef NEED
}

/* ---- targets ---- */

static const char *tidy(const char *name)
{
    while (name[0] == '.' && name[1] == '/' && name[2]) {
        name += 2;
        while (*name == '/' && name[1])
            name++;
    }
    return name;
}

static Target *target_find(const char *name)
{
    Target *t;
    name = tidy(name);
    for (t = targets[hash_str(name, strlen(name)) % VHASH]; t; t = t->next)
        if (strcmp(t->name, name) == 0)
            return t;
    return 0;
}

static Target *target_get(const char *name)
{
    Target *t = target_find(name);
    u32 h;
    if (t)
        return t;
    name = tidy(name);
    h = hash_str(name, strlen(name)) % VHASH;
    t = (Target *)xmalloc(sizeof(Target));
    memset(t, 0, sizeof *t);
    t->name = save_str(name);
    t->deps_tail = &t->deps;
    t->vars_tail = &t->vars;
    t->next = targets[h];
    targets[h] = t;
    return t;
}

/* When the file was last changed, looking in the VPATH directories too; 0 if there is none. */
static i64 find_file(const char *name, const char **path)
{
    static char *vpath;
    static long vpath_gen = -1;
    i64 m = os_mtime(name, 0);
    const char *p, *e;
    if (m || name[0] == '/')
        return m;
    if (vpath_gen != var_generation) {
        free(vpath);
        vpath = xexpand("$(VPATH)");
        vpath_gen = var_generation;
    }
    for (p = vpath; *p; p = e) {
        char *full;
        while (*p == ':' || is_space((u8)*p))
            p++;
        e = p;
        while (*e && *e != ':' && !is_space((u8)*e))
            e++;
        if (e == p)
            break;
        full = (char *)xmalloc((size_t)(e - p) + strlen(name) + 2);
        sprintf(full, "%.*s/%s", (int)(e - p), p, name);
        m = os_mtime(full, 0);
        if (m) {
            if (path)
                *path = full;
            else
                free(full);
            return m;
        }
        free(full);
    }
    return 0;
}

static i64 target_mtime(Target *t)
{
    if (t->phony)
        return 0;
    if (!t->mtime_known) {
        t->path = 0;
        t->mtime = find_file(t->name, &t->path);
        t->mtime_known = 1;
    }
    return t->mtime;
}

static Dep *new_dep(const char *name, int order_only)
{
    Dep *d = (Dep *)xmalloc(sizeof(Dep));
    d->t = target_get(name);
    d->t->mentioned = 1;
    d->order_only = order_only;
    d->next = 0;
    return d;
}

static void add_dep(Target *t, const char *name, int order_only)
{
    Dep *d = new_dep(name, order_only);
    *t->deps_tail = d;
    t->deps_tail = &d->next;
}

static void add_deps_text(Target *t, const char *text, int order_only)
{
    const char *w;
    size_t wl;
    while ((w = next_word(&text, &wl)) != 0) {
        char *n = save(w, wl);
        if (strpbrk(n, "*?[")) {
            Buf b;
            const char *s, *g;
            size_t gl;
            memset(&b, 0, sizeof b);
            glob_words(&b, 0, n, 1);
            s = finish(&b);
            while ((g = next_word(&s, &gl)) != 0) {
                char *gn = save(g, gl);
                add_dep(t, gn, order_only);
                free(gn);
            }
            buf_free(&b);
        } else {
            add_dep(t, n, order_only);
        }
        free(n);
    }
}

/* ---- reading makefiles ---- */

typedef struct Reader {
    Target **targets;
    Dep ***slots; /* where the prerequisites this rule gave each target start */
    int n, cap;
    Recipe *recipe;
    int dc;
} Reader;

static int not_parallel;

/* The next word, where a reference with spaces inside it counts as part of the word. */
static const char *next_nest(const char **s, size_t *len)
{
    const char *p = *s, *w;
    while (is_space((u8)*p))
        p++;
    if (!*p)
        return 0;
    w = p;
    while (*p && !is_space((u8)*p)) {
        if (*p == '$' && (p[1] == '(' || p[1] == '{')) {
            const char *m = match_paren(p + 2, p[1], p[1] == '(' ? ')' : '}');
            if (!m) {
                p += strlen(p);
                break;
            }
            p = m + 1;
        } else {
            p++;
        }
    }
    *len = (size_t)(p - w);
    *s = p;
    return w;
}

/* The first of the given characters that is not inside a reference. */
static char *find_top(char *s, const char *set)
{
    for (; *s; s++) {
        if (*s == '$') {
            if (s[1] == '(' || s[1] == '{') {
                const char *m = match_paren(s + 2, s[1], s[1] == '(' ? ')' : '}');
                if (!m)
                    return 0;
                s = (char *)m;
            } else if (s[1]) {
                s++;
            }
        } else if (strchr(set, *s)) {
            return s;
        }
    }
    return 0;
}

/* Is this "name op value"? */
static int find_assign(const char *s, const char **name_end, int *op, const char **value)
{
    const char *p = s;
    int ws = 0;
    while (is_space((u8)*p))
        p++;
    s = p;
    for (; *p; p++) {
        if (is_space((u8)*p)) {
            ws = 1;
            continue;
        }
        if (*p == '=') {
            *op = '=';
            *value = p + 1;
            break;
        }
        if (*p == ':') {
            if (p[1] == '=')
                *value = p + 2;
            else if (p[1] == ':' && p[2] == '=')
                *value = p + 3;
            else
                return 0;
            *op = ':';
            break;
        }
        if ((*p == '+' || *p == '?' || *p == '!') && p[1] == '=') {
            *op = *p;
            *value = p + 2;
            break;
        }
        if (ws || *p == ';')
            return 0;
        if (*p == '$') {
            if (p[1] == '(' || p[1] == '{') {
                const char *m = match_paren(p + 2, p[1], p[1] == '(' ? ')' : '}');
                if (!m)
                    return 0;
                p = m;
            } else if (p[1]) {
                p++;
            }
        }
    }
    if (!*p || p == s)
        return 0;
    *name_end = p;
    while (is_space((u8)**value))
        (*value)++;
    return 1;
}

static int var_blocked(Var *v, int origin)
{
    return v && ((v->origin == O_CMDLINE && origin != O_CMDLINE && origin != O_OVERRIDE) ||
                 (v->origin == O_OVERRIDE && origin != O_OVERRIDE && origin != O_CMDLINE && origin != O_AUTO));
}

static void do_assign(const char *name, int op, const char *value, int origin)
{
    Var *v = var_find(name, strlen(name));
    int defined = v && v->origin != O_UNDEF;
    char *x;
    if (var_blocked(v, origin))
        return;
    switch (op) {
    case '=':
        var_set(name, value, 1, origin);
        break;
    case ':':
        x = xexpand(value);
        var_set(name, x, 0, origin);
        free(x);
        break;
    case '?':
        if (!defined)
            var_set(name, value, 1, origin);
        break;
    case '!': {
        Buf b;
        memset(&b, 0, sizeof b);
        x = xexpand(value);
        run_shell(&b, x);
        free(x);
        var_set(name, finish(&b), 1, origin);
        buf_free(&b);
        break;
    }
    case '+':
        if (!defined) {
            var_set(name, value, 1, origin);
        } else {
            Buf b;
            int rec = v->recursive;
            memset(&b, 0, sizeof b);
            put_str(&b, v->value);
            if (b.len)
                buf_u8(&b, ' ');
            if (rec)
                put_str(&b, value);
            else
                expand(&b, value);
            var_set(name, finish(&b), rec, v->origin == O_DEFAULT || v->origin == O_ENV ? origin : v->origin);
            buf_free(&b);
        }
        break;
    }
}

/* s if it starts with the word, moved past it and the blanks after it; else null. */
static char *kw(char *s, const char *word)
{
    size_t n = strlen(word);
    if (strncmp(s, word, n) != 0 || (s[n] && !is_space((u8)s[n])))
        return 0;
    s += n;
    while (is_space((u8)*s))
        s++;
    return s;
}

static Recipe *new_recipe(void)
{
    Recipe *r = (Recipe *)xmalloc(sizeof(Recipe));
    memset(r, 0, sizeof *r);
    r->file = cur_file;
    r->line = cur_line;
    return r;
}

static void close_rule(Reader *r)
{
    int k;
    if (r->recipe && r->recipe->used) {
        for (k = 0; k < r->n; k++) {
            Target *t = r->targets[k];
            Dep **slot = r->slots[k];
            if (t->recipe && t->recipe != r->recipe && r->dc) {
                buf_put(&t->recipe->text, r->recipe->text.p, r->recipe->text.len);
            } else {
                if (t->recipe && t->recipe != r->recipe && t->recipe->text.len) {
                    fflush(stdout);
                    fprintf(stderr, "%s:%d: warning: overriding recipe for target '%s'\n", r->recipe->file, r->recipe->line, t->name);
                }
                t->recipe = r->recipe;
            }
            /* the prerequisites of the rule with the recipe come first */
            if (slot != &t->deps && *slot) {
                Dep *first = *slot, *last = first;
                while (last->next)
                    last = last->next;
                *slot = 0;
                last->next = t->deps;
                t->deps = first;
                t->deps_tail = slot;
            }
        }
    }
    r->n = 0;
    r->recipe = 0;
}

static void reader_add(Reader *r, Target *t)
{
    if (r->n == r->cap) {
        r->cap = r->cap ? 2 * r->cap : 8;
        r->targets = (Target **)xrealloc(r->targets, (size_t)r->cap * sizeof(Target *));
        r->slots = (Dep ***)xrealloc(r->slots, (size_t)r->cap * sizeof(Dep **));
    }
    r->targets[r->n] = t;
    r->slots[r->n++] = t->deps_tail;
}

static void recipe_line(Recipe *rc, const char *s)
{
    while (is_space((u8)*s))
        s++;
    put_str(&rc->text, s);
    buf_u8(&rc->text, '\n');
    rc->used = 1;
}

/* Puts the stem in for the % of each word. */
static char *subst_words(const char *text, const char *stem, size_t sl)
{
    Buf b;
    const char *w;
    size_t wl;
    memset(&b, 0, sizeof b);
    while ((w = next_nest(&text, &wl)) != 0) {
        char *word = save(w, wl);
        if (b.len)
            buf_u8(&b, ' ');
        pat_subst(&b, word, stem, sl);
        free(word);
    }
    return finish(&b);
}

static void add_tvar(Target *t, const char *pattern, const char *name, int op, const char *value)
{
    TVar *tv = (TVar *)xmalloc(sizeof(TVar));
    tv->name = save_str(name);
    tv->op = op;
    tv->value = op == ':' ? xexpand(value) : save_str(value);
    tv->next = 0;
    if (pattern) {
        PatVar *pv = (PatVar *)xmalloc(sizeof(PatVar));
        pv->pattern = save_str(pattern);
        pv->v = tv;
        pv->next = 0;
        *pat_vars_tail = pv;
        pat_vars_tail = &pv->next;
    } else {
        *t->vars_tail = tv;
        t->vars_tail = &tv->next;
    }
    var_generation++;
}

static void do_rule(Reader *r, char *line)
{
    char *colon = find_top(line, ":"), *tg, *rest, *recipe_text = 0, *x = 0, *deps, *semi, *bar, *tpat = 0, *c2;
    const char *order = "", *s, *w;
    size_t wl;
    int expanded = 0, dc = 0, first = 1;
    close_rule(r);
    if (!colon) {
        x = xexpand(line);
        if (!*strip(x)) {
            free(x);
            return;
        }
        colon = strchr(x, ':');
        if (!colon)
            fatal("missing separator", 0, 0);
        expanded = 1;
        *colon = 0;
        tg = save_str(x);
    } else {
        *colon = 0;
        tg = xexpand(line);
    }
    rest = colon + 1;
    if (*rest == ':')
        dc = 1, rest++;
    if (!expanded) {
        char *a = rest, *b;
        const char *ne, *val;
        int op, origin = O_FILE;
        for (;;) {
            while (is_space((u8)*a))
                a++;
            if ((b = kw(a, "override")) != 0)
                origin = O_OVERRIDE;
            else if ((b = kw(a, "export")) == 0 && (b = kw(a, "private")) == 0)
                break;
            a = b;
        }
        if (find_assign(a, &ne, &op, &val)) {
            char *raw = save(a, (size_t)(ne - a)), *name = xexpand(raw), *nm = strip(name);
            (void)origin;
            s = tg;
            while ((w = next_word(&s, &wl)) != 0) {
                char *tn = save(w, wl);
                if (strchr(tn, '%'))
                    add_tvar(0, tn, nm, op, val);
                else
                    add_tvar(target_get(tn), 0, nm, op, val);
                free(tn);
            }
            free(raw);
            free(name);
            free(tg);
            return;
        }
        semi = find_top(rest, ";");
    } else {
        semi = strchr(rest, ';');
    }
    if (semi) {
        *semi = 0;
        recipe_text = semi + 1;
    }
    deps = expanded ? save_str(rest) : xexpand(rest);
    {
        char *base = deps;
        c2 = find_top(deps, ":");
        if (c2) {
            *c2 = 0;
            tpat = strip(deps);
            deps = c2 + 1;
        }
        bar = find_top(deps, "|");
        if (bar) {
            *bar = 0;
            order = bar + 1;
        }
        /* the special targets */
        s = tg;
        w = next_word(&s, &wl);
        if (!w) { /* a rule for no targets says nothing, and its recipe goes nowhere */
            r->recipe = new_recipe();
            free(base);
            free(tg);
            free(x);
            return;
        }
        if (w[0] == '.' && wl > 1 && w[1] >= 'A' && w[1] <= 'Z' && !memchr(w, '/', wl)) {
            char *sp = save(w, wl);
            int known = 1, bare = !*strip(deps);
            if (strcmp(sp, ".PHONY") == 0) {
                s = deps;
                while ((w = next_word(&s, &wl)) != 0) {
                    char *n = save(w, wl);
                    target_get(n)->phony = 1;
                    free(n);
                }
            } else if (strcmp(sp, ".SECONDEXPANSION") == 0)
                second_expansion = 1;
            else if (strcmp(sp, ".DELETE_ON_ERROR") == 0)
                delete_on_error = 1;
            else if (strcmp(sp, ".EXPORT_ALL_VARIABLES") == 0)
                export_all = 1;
            else if (strcmp(sp, ".NOTPARALLEL") == 0)
                not_parallel = 1;
            else if (strcmp(sp, ".SILENT") == 0)
                opt_silent |= bare;
            else if (strcmp(sp, ".IGNORE") == 0)
                opt_ignore |= bare;
            else if (strcmp(sp, ".PRECIOUS") != 0 && strcmp(sp, ".SECONDARY") != 0 && strcmp(sp, ".INTERMEDIATE") != 0 &&
                     strcmp(sp, ".SUFFIXES") != 0 && strcmp(sp, ".NOTINTERMEDIATE") != 0 && strcmp(sp, ".POSIX") != 0 &&
                     strcmp(sp, ".ONESHELL") != 0 && strcmp(sp, ".LOW_RESOLUTION_TIME") != 0)
                known = 0;
            free(sp);
            if (known) {
                free(base);
                free(tg);
                free(x);
                return;
            }
        }
        r->recipe = new_recipe();
        r->dc = dc;
        s = tg;
        w = next_word(&s, &wl);
        if (memchr(w, '%', wl) && !tpat) {
            /* a pattern rule */
            PatRule *pr = (PatRule *)xmalloc(sizeof(PatRule)), *old;
            Names tl, dl;
            const char *part;
            int k;
            memset(pr, 0, sizeof *pr);
            tl.v = dl.v = 0;
            tl.n = dl.n = 0;
            for (; w; w = next_word(&s, &wl)) {
                char *full = save(w, wl);
                names_add(&tl, save_str(tidy(full)));
                free(full);
            }
            for (k = 0; k < 2; k++) {
                part = k ? order : deps;
                while ((w = next_nest(&part, &wl)) != 0) {
                    char *dn;
                    while (wl > 2 && w[0] == '.' && w[1] == '/')
                        w += 2, wl -= 2;
                    dn = save(w, wl);
                    if (strpbrk(dn, "*?[") && !strchr(dn, '%')) {
                        /* file name patterns stand for the files there are when the rule is read */
                        Buf gb;
                        const char *gs, *g;
                        size_t gl;
                        memset(&gb, 0, sizeof gb);
                        glob_words(&gb, 0, dn, 1);
                        gs = finish(&gb);
                        while ((g = next_word(&gs, &gl)) != 0) {
                            names_add(&dl, save(g, gl));
                            pr->norder += k;
                        }
                        buf_free(&gb);
                        free(dn);
                        continue;
                    }
                    names_add(&dl, dn);
                    pr->norder += k;
                }
            }
            pr->targets = tl.v;
            pr->ntargets = (int)tl.n;
            pr->deps = dl.v;
            pr->ndeps = (int)dl.n;
            pr->recipe = r->recipe;
            pr->second = second_expansion;
            /* a rule with the same targets and prerequisites takes the place of the earlier one */
            for (old = pat_rules; old; old = old->next) {
                if (old->ntargets != pr->ntargets || old->ndeps != pr->ndeps || old->norder != pr->norder)
                    continue;
                for (k = 0; k < pr->ntargets && strcmp(old->targets[k], pr->targets[k]) == 0; k++)
                    ;
                if (k < pr->ntargets)
                    continue;
                for (k = 0; k < pr->ndeps && strcmp(old->deps[k], pr->deps[k]) == 0; k++)
                    ;
                if (k == pr->ndeps)
                    break;
            }
            if (old) {
                old->recipe = pr->recipe;
                old->second = pr->second;
            } else {
                *pat_tail = pr;
                pat_tail = &pr->next;
            }
        } else {
            for (; w; w = next_word(&s, &wl)) {
                char *tn = save(w, wl), *dn = 0, *on = 0;
                const char *dtext = deps, *otext = order;
                Target *t = target_get(tn);
                t->is_target = 1;
                reader_add(r, t);
                if (first && !default_goal && (tn[0] != '.' || strchr(tn, '/')))
                    default_goal = t;
                first = 0;
                if (tpat) {
                    const char *stem;
                    size_t sl;
                    if (!pat_match(tpat, t->name, strlen(t->name), &stem, &sl))
                        fatal("target '%s' doesn't match the target pattern", tn, 0);
                    t->stem = save(stem, sl);
                    dtext = dn = subst_words(deps, t->stem, sl);
                    otext = on = subst_words(order, t->stem, sl);
                }
                if (second_expansion && (strchr(dtext, '$') || strchr(otext, '$'))) {
                    Buf b;
                    memset(&b, 0, sizeof b);
                    if (t->second) {
                        put_str(&b, t->second);
                        buf_u8(&b, '\n');
                    }
                    put_str(&b, dtext);
                    put_str(&b, " | ");
                    put_str(&b, otext);
                    free(t->second);
                    t->second = finish(&b);
                } else {
                    add_deps_text(t, dtext, 0);
                    add_deps_text(t, otext, 1);
                }
                free(dn);
                free(on);
                free(tn);
            }
        }
        if (recipe_text)
            recipe_line(r->recipe, recipe_text);
        free(base);
    }
    free(tg);
    free(x);
}

static int cond_eval(char *s)
{
    char *a, *b, *xa, *xb;
    int neg = 0, r;
    if ((a = kw(s, "ifdef")) != 0 || ((a = kw(s, "ifndef")) != 0 && (neg = 1) != 0)) {
        Var *v;
        xa = xexpand(a);
        b = strip(xa);
        v = var_find(b, strlen(b));
        r = v && v->origin != O_UNDEF && *v->value;
        free(xa);
        return r != neg;
    }
    if ((a = kw(s, "ifeq")) == 0) {
        if ((a = kw(s, "ifneq")) == 0)
            fatal("invalid conditional", 0, 0);
        neg = 1;
    }
    if (*a == '(') {
        int depth = 0;
        char *p, *close = strrchr(a, ')');
        if (!close)
            fatal("invalid syntax in conditional", 0, 0);
        *close = 0;
        for (p = ++a; *p; p++) {
            if (*p == '(' || *p == '{')
                depth++;
            else if (*p == ')' || *p == '}')
                depth--;
            else if (*p == ',' && depth <= 0)
                break;
        }
        if (!*p)
            fatal("invalid syntax in conditional", 0, 0);
        b = p + 1;
        while (p > a && is_space((u8)p[-1]))
            p--;
        *p = 0;
        while (is_space((u8)*b))
            b++;
    } else {
        int q = *a;
        char *e = q == '"' || q == '\'' ? strchr(a + 1, q) : 0;
        if (!e)
            fatal("invalid syntax in conditional", 0, 0);
        *e = 0;
        a++;
        b = e + 1;
        while (is_space((u8)*b))
            b++;
        q = *b;
        e = q == '"' || q == '\'' ? strchr(b + 1, q) : 0;
        if (!e)
            fatal("invalid syntax in conditional", 0, 0);
        *e = 0;
        b++;
    }
    xa = xexpand(a);
    xb = xexpand(b);
    r = strcmp(xa, xb) == 0;
    free(xa);
    free(xb);
    return r != neg;
}

static void logical_line(char **pp, Buf *lb, int *line)
{
    char *p = *pp;
    lb->len = 0;
    for (;;) {
        char *e = strchr(p, '\n'), *end = e ? e : p + strlen(p), *q = end;
        int bs = 0;
        if (q > p && q[-1] == '\r')
            q--;
        while (q - bs > p && q[-1 - bs] == '\\')
            bs++;
        (*line)++;
        if (e && (bs & 1)) {
            q--;
            while (q > p && is_space((u8)q[-1]))
                q--;
            buf_put(lb, p, (size_t)(q - p));
            buf_u8(lb, ' ');
            p = e + 1;
            while (*p == ' ' || *p == '\t')
                p++;
            continue;
        }
        buf_put(lb, p, (size_t)(q - p));
        p = e ? e + 1 : end;
        break;
    }
    buf_u8(lb, 0);
    *pp = p;
}

static void strip_comment(char *s)
{
    char *d = s;
    for (; *s; s++) {
        if (*s == '\\' && s[1] == '#') {
            *d++ = '#';
            s++;
        } else if (*s == '#') {
            break;
        } else {
            *d++ = *s;
        }
    }
    *d = 0;
}

static void read_makefile(const char *name, int optional, int top);

static void mark_export(const char *names, int how)
{
    const char *w;
    size_t wl;
    while ((w = next_word(&names, &wl)) != 0)
        var_get(w, wl)->exported = (u8)how;
}

static void eval_text(char *text, const char *file, int line)
{
    Reader r;
    Buf lb;
    char *p = text;
    u8 active[64], taken[64];
    int depth = 0, ignoring = 0;
    const char *old_file = cur_file;
    int old_line = cur_line;
    Target *old_scope = scope;
    memset(&r, 0, sizeof r);
    memset(&lb, 0, sizeof lb);
    scope = 0;
    line--;
    while (*p) {
        char *s, *a;
        const char *ne, *val;
        int op, origin = O_FILE, exp = 0;
        if (*p == '\t' && r.recipe) {
            /* a line of the recipe, kept as it is */
            Recipe *rc = r.recipe;
            p++;
            for (;;) {
                char *e = strchr(p, '\n'), *end = e ? e : p + strlen(p), *q = end;
                int bs = 0;
                while (q - bs > p && q[-1 - bs] == '\\')
                    bs++;
                line++;
                if (!ignoring) {
                    buf_put(&rc->text, p, (size_t)(end - p));
                    buf_u8(&rc->text, '\n');
                    rc->used = 1;
                }
                p = e ? e + 1 : end;
                if (!e || !(bs & 1))
                    break;
            }
            continue;
        }
        cur_file = file;
        cur_line = line + 1;
        logical_line(&p, &lb, &line);
        strip_comment((char *)lb.p);
        s = strip((char *)lb.p);
        if (!*s)
            continue;
        if (kw(s, "ifdef") || kw(s, "ifndef") || kw(s, "ifeq") || kw(s, "ifneq")) {
            if (depth == 64)
                fatal("conditionals nested too deeply", 0, 0);
            if (ignoring) {
                active[depth] = 0;
                taken[depth] = 1;
            } else {
                active[depth] = taken[depth] = (u8)cond_eval(s);
            }
            depth++;
            ignoring = !active[depth - 1];
            continue;
        }
        if ((a = kw(s, "else")) != 0) {
            if (!depth)
                fatal("extraneous 'else'", 0, 0);
            if (*a) {
                if (taken[depth - 1])
                    active[depth - 1] = 0;
                else
                    active[depth - 1] = taken[depth - 1] = (u8)cond_eval(a);
            } else {
                active[depth - 1] = !taken[depth - 1];
                taken[depth - 1] = 1;
            }
            ignoring = !active[depth - 1];
            continue;
        }
        if (kw(s, "endif")) {
            if (!depth)
                fatal("extraneous 'endif'", 0, 0);
            depth--;
            ignoring = depth && !active[depth - 1];
            continue;
        }
        if (ignoring)
            continue;
        /* anything but a conditional ends the rule before it; do_rule does so itself, after looking at the line */
        if (find_top(s, ":") == 0 || find_assign(s, &ne, &op, &val) || kw(s, "define") || kw(s, "export") || kw(s, "override") ||
            kw(s, "include") || kw(s, "-include") || kw(s, "sinclude") || kw(s, "unexport") || kw(s, "undefine"))
            close_rule(&r);
        /* what may stand before an assignment */
        for (;;) {
            if ((a = kw(s, "override")) != 0 && *a)
                origin = O_OVERRIDE;
            else if ((a = kw(s, "export")) != 0 && *a && find_assign(a, &ne, &op, &val))
                exp = 1;
            else if ((a = kw(s, "private")) == 0 || !*a)
                break;
            s = a;
        }
        if ((a = kw(s, "define")) != 0) {
            char *name, *nm, *body = p, *end = p;
            int nest = 0;
            op = '=';
            if (find_assign(a, &ne, &op, &val))
                name = save(a, (size_t)(ne - a));
            else
                name = save_str(a);
            for (;;) {
                char *e = strchr(p, '\n'), *q = p;
                if (!*p)
                    fatal("missing 'endef', unterminated 'define'", 0, 0);
                while (*q == ' ' || *q == '\t')
                    q++;
                line++;
                end = p;
                if (strncmp(q, "define", 6) == 0 && is_space((u8)q[6]))
                    nest++;
                else if (strncmp(q, "endef", 5) == 0 && (!q[5] || is_space((u8)q[5]) || q[5] == '#') && nest-- == 0) {
                    p = e ? e + 1 : p + strlen(p);
                    break;
                }
                p = e ? e + 1 : p + strlen(p);
            }
            if (end > body)
                end--; /* the newline before endef */
            body = save(body, (size_t)(end - body));
            {
                /* lines continued with a backslash are joined, as everywhere outside recipes */
                char *src = body, *dst = body;
                while (*src) {
                    if (*src == '\\' && src[1] == '\n') {
                        int bs = 0;
                        while (dst - bs > body && dst[-1 - bs] == '\\')
                            bs++;
                        if (!(bs & 1)) {
                            while (dst > body && (dst[-1] == ' ' || dst[-1] == '\t'))
                                dst--;
                            *dst++ = ' ';
                            src += 2;
                            while (*src == ' ' || *src == '\t')
                                src++;
                            continue;
                        }
                    }
                    *dst++ = *src++;
                }
                *dst = 0;
            }
            nm = strip(name);
            {
                char *xn = xexpand(nm);
                do_assign(strip(xn), op, body, origin);
                if (exp)
                    mark_export(strip(xn), 1);
                free(xn);
            }
            free(body);
            free(name);
            continue;
        }
        if (find_assign(s, &ne, &op, &val)) {
            char *raw = save(s, (size_t)(ne - s)), *name = xexpand(raw), *nm = strip(name);
            do_assign(nm, op, val, origin);
            if (exp)
                mark_export(nm, 1);
            free(raw);
            free(name);
            continue;
        }
        if ((a = kw(s, "include")) != 0 || (a = kw(s, "-include")) != 0 || (a = kw(s, "sinclude")) != 0) {
            char *names = xexpand(a);
            Buf b;
            const char *w, *ws;
            size_t wl;
            int optional = s[0] != 'i';
            memset(&b, 0, sizeof b);
            glob_words(&b, 0, names, 1);
            ws = finish(&b);
            close_rule(&r);
            while ((w = next_word(&ws, &wl)) != 0) {
                char *n = save(w, wl);
                read_makefile(n, optional, 0);
                free(n);
            }
            buf_free(&b);
            free(names);
            continue;
        }
        if ((a = kw(s, "export")) != 0 || (a = kw(s, "unexport")) != 0) {
            int how = s[0] == 'e' ? 1 : 2;
            if (!*a) {
                export_all = how == 1;
            } else {
                char *names = xexpand(a);
                mark_export(names, how);
                free(names);
            }
            continue;
        }
        if ((a = kw(s, "undefine")) != 0) {
            char *names = xexpand(a), *nm = strip(names);
            Var *v = var_find(nm, strlen(nm));
            if (v && !var_blocked(v, origin)) {
                free(v->value);
                v->value = save_str("");
                v->origin = O_UNDEF;
                v->recursive = 0;
                var_generation++;
            }
            free(names);
            continue;
        }
        if (kw(s, "vpath"))
            continue;
        if (kw(s, "endef"))
            fatal("extraneous 'endef'", 0, 0);
        do_rule(&r, s);
    }
    close_rule(&r);
    if (depth)
        fatal("missing 'endif'", 0, 0);
    free(r.targets);
    free(r.slots);
    buf_free(&lb);
    cur_file = old_file;
    cur_line = old_line;
    scope = old_scope;
}

static void add_include_dir(const char *dir)
{
    int k;
    for (k = 0; k < ninclude_dirs; k++)
        if (strcmp(include_dirs[k], dir) == 0)
            return;
    if (ninclude_dirs < 32)
        include_dirs[ninclude_dirs++] = save_str(dir);
}

static void decode_flags(const char *s, int from_env);

static void read_makefile(const char *name, int optional, int top)
{
    size_t len;
    u8 *data = os_read_file(name, &len);
    char *found = 0, *text;
    Include *inc;
    if (!data && name[0] != '/' && !top) {
        int k;
        char *flags = xexpand("$(MAKEFLAGS)");
        decode_flags(flags, 2);
        free(flags);
        for (k = 0; k < ninclude_dirs && !data; k++) {
            found = (char *)xmalloc(strlen(include_dirs[k]) + strlen(name) + 2);
            sprintf(found, "%s/%s", include_dirs[k], name);
            data = os_read_file(found, &len);
            if (!data) {
                free(found);
                found = 0;
            }
        }
    }
    if (!top) {
        inc = (Include *)xmalloc(sizeof(Include));
        inc->name = save_str(found ? found : name);
        inc->optional = optional;
        inc->found = data != 0;
        inc->next = 0;
        *includes_tail = inc;
        includes_tail = &inc->next;
    }
    if (!data) {
        if (top)
            fatal("%s: No such file or directory", name, 0);
        return;
    }
    {
        Buf b;
        memset(&b, 0, sizeof b);
        put_str(&b, var_value("MAKEFILE_LIST"));
        buf_u8(&b, ' ');
        put_str(&b, found ? found : name);
        var_set("MAKEFILE_LIST", finish(&b), 0, O_FILE);
        buf_free(&b);
    }
    text = save((const char *)data, len);
    eval_text(text, save_str(found ? found : name), 1);
    free(text);
    free(found);
}

/* ---- implicit rules ---- */

static int ought_exist(const char *name)
{
    Target *t = target_find(name);
    if (t && (t->is_target || t->mentioned || t->phony))
        return 1;
    if (t)
        return target_mtime(t) != 0;
    return find_file(tidy(name), 0) != 0;
}

static int implicit(Target *t, int depth);

static int try_rule(Target *t, PatRule *pr, int which, const char *stem, size_t sl, size_t dirlen, int pass, int depth)
{
    Names deps;
    Buf flags;
    char *full_stem;
    const char *old_stem = t->stem;
    Target *old_scope = scope;
    int i, ok = 1;
    u32 k;
    deps.v = 0;
    deps.n = 0;
    memset(&flags, 0, sizeof flags);
    full_stem = (char *)xmalloc(dirlen + sl + 1);
    memcpy(full_stem, t->name, dirlen);
    memcpy(full_stem + dirlen, stem, sl);
    full_stem[dirlen + sl] = 0;
    for (i = 0; i < pr->ndeps; i++) {
        Buf b;
        char *s;
        const char *w = pr->deps[i];
        int order_only = i >= pr->ndeps - pr->norder;
        memset(&b, 0, sizeof b);
        if (strchr(w, '%')) {
            buf_put(&b, t->name, dirlen);
            pat_subst(&b, w, stem, sl);
        } else {
            put_str(&b, w);
        }
        s = finish(&b);
        if (pr->second && strchr(s, '$')) {
            char *x;
            const char *xs, *xw;
            size_t xl;
            t->stem = full_stem;
            scope = t;
            x = xexpand(s);
            scope = old_scope;
            t->stem = old_stem;
            xs = x;
            while ((xw = next_word(&xs, &xl)) != 0) {
                names_add(&deps, save(xw, xl));
                buf_u8(&flags, (u32)order_only);
            }
            free(x);
            free(s);
        } else {
            names_add(&deps, s);
            buf_u8(&flags, (u32)order_only);
        }
    }
    for (k = 0; k < deps.n && ok; k++) {
        if (ought_exist(deps.v[k]))
            continue;
        if (pass == 0)
            ok = 0;
        else {
            Target *dt = target_get(deps.v[k]);
            if (!dt->recipe && !implicit(dt, depth + 1))
                ok = 0;
        }
    }
    if (ok) {
        Dep *first = 0, **tail = &first;
        t->recipe = pr->recipe;
        t->stem = full_stem;
        for (k = 0; k < deps.n; k++) {
            Dep *d = new_dep(deps.v[k], flags.p[k]);
            *tail = d;
            tail = &d->next;
        }
        if (first) {
            *tail = t->deps;
            if (!t->deps)
                t->deps_tail = tail;
            t->deps = first;
        }
        for (i = 0; i < pr->ntargets; i++) {
            Buf b;
            Target *sib;
            if (i == which)
                continue;
            memset(&b, 0, sizeof b);
            buf_put(&b, t->name, dirlen);
            pat_subst(&b, pr->targets[i], stem, sl);
            sib = target_get(finish(&b));
            if (sib != t && sib->state == S_NEW && !sib->recipe && !sib->also_of)
                sib->also_of = t;
            buf_free(&b);
        }
    } else {
        free(full_stem);
    }
    for (k = 0; k < deps.n; k++)
        free(deps.v[k]);
    free(deps.v);
    buf_free(&flags);
    return ok;
}

/* Looks for a pattern rule that can make the target and applies it. */
static int implicit(Target *t, int depth)
{
    PatRule *pr;
    int pass, k;
    const char *name = t->name, *base = strrchr(name, '/');
    base = base ? base + 1 : name;
    if (t->searching || t->tried)
        return 0;
    t->searching = 1;
    for (pass = 0; pass < 2; pass++) {
        if (pass == 1 && depth >= 3)
            break;
        for (pr = pat_rules; pr; pr = pr->next) {
            if (!pr->recipe->used)
                continue;
            for (k = 0; k < pr->ntargets; k++) {
                const char *pat = pr->targets[k], *stem, *m = name;
                size_t sl, dirlen = 0;
                if (!strchr(pat, '/') && base != name) {
                    m = base;
                    dirlen = (size_t)(base - name);
                }
                if (!pat_match(pat, m, strlen(m), &stem, &sl) || !sl)
                    continue;
                if (depth && strcmp(pat, "%") == 0)
                    continue;
                if (try_rule(t, pr, k, stem, sl, dirlen, pass, depth)) {
                    t->searching = 0;
                    return 1;
                }
            }
        }
    }
    t->searching = 0;
    if (!depth)
        t->tried = 1;
    return 0;
}

/* ---- running recipes ---- */

static Target **ready, **running;
static int nready, ready_head, cap_ready, nrunning, cap_running;
static int job_fd = -1, own_fifo, implicit_used, serial = 1, stopping, any_failed, any_ran, dirty;
static char *job_path, *start_dir;
static long walk_id;
static char **passthru; /* what of the environment is handed on as it came */
static int npassthru;
static char flagset[128];

static void who(void)
{
    fflush(stdout);
    if (make_level)
        fprintf(stderr, "make[%d]: ", make_level);
    else
        fprintf(stderr, "make: ");
}

static void cleanup(void)
{
    if (own_fifo && job_path)
        os_unlink(job_path);
    own_fifo = 0;
}

static char *find_program(const char *name)
{
    const char *path = getenv("PATH"), *e;
    if (strchr(name, '/') || !path)
        return save_str(name);
    for (; *path; path = *e ? e + 1 : e) {
        char *full;
        e = strchr(path, ':');
        if (!e)
            e = path + strlen(path);
        full = (char *)xmalloc((size_t)(e - path) + strlen(name) + 2);
        sprintf(full, "%.*s/%s", (int)(e - path), path, name);
        if (e > path && os_exists(full))
            return full;
        free(full);
    }
    return save_str(name);
}

/* The shell for recipes: the one installed with this program (beside it, or in bin/ beside it) when there
   is one, so that a build needs nothing from the system; otherwise /bin/sh. */
static const char *own_shell(const char *argv0)
{
    char *prog = find_program(argv0), *slash = strrchr(prog, '/'), *sh;
    size_t n = slash ? (size_t)(slash - prog) : 0;
    Buf b;
    int i;
    for (i = 0; slash && i < 2; i++) {
        memset(&b, 0, sizeof b);
        abs_path(&b, prog, n);
        put_str(&b, i ? "/bin/sh" : "/sh");
        sh = finish(&b);
        if (os_exists(sh)) {
            free(prog);
            return save_str(sh);
        }
    }
    free(prog);
    return "/bin/sh";
}

static const char *shell_path(void)
{
    static char *cached, *cached_for;
    Var *v = var_find("SHELL", 5);
    const char *s = v ? v->value : "/bin/sh";
    if (!cached_for || strcmp(cached_for, s) != 0) {
        free(cached);
        free(cached_for);
        cached_for = save_str(s);
        cached = v && v->recursive ? xexpand(s) : save_str(s);
        if (!strchr(cached, '/')) {
            char *f = find_program(cached);
            free(cached);
            cached = f;
        }
    }
    return cached;
}

static void put_escaped(Buf *b, const char *s)
{
    for (; *s; s++) {
        if (is_space((u8)*s) || *s == '\\')
            buf_u8(b, '\\');
        buf_u8(b, (u8)*s);
    }
}

static int name_ok(const char *s)
{
    if (!*s || (*s >= '0' && *s <= '9'))
        return 0;
    for (; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '_'))
            return 0;
    return 1;
}

/* The environment for what a recipe starts; $(shell) gets the one make was started with. */
static char **make_env(int for_recipe)
{
    Names l;
    Buf b;
    u32 h;
    int k;
    Var *v;
    char num[16];
    if (!for_recipe)
        return self_envp;
    l.v = 0;
    l.n = 0;
    memset(&b, 0, sizeof b);
    for (k = 0; k < npassthru; k++)
        names_add(&l, save_str(passthru[k]));
    for (h = 0; h < VHASH; h++)
        for (v = vars[h]; v; v = v->next) {
            if (v->origin == O_UNDEF || v->exported == 2)
                continue;
            if (!(v->exported == 1 || v->origin == O_ENV || v->origin == O_CMDLINE || (export_all && v->origin != O_DEFAULT && v->origin != O_AUTO)))
                continue;
            if (!name_ok(v->name) || strcmp(v->name, "MAKEFLAGS") == 0 || strcmp(v->name, "MAKELEVEL") == 0)
                continue;
            b.len = 0;
            put_str(&b, v->name);
            buf_u8(&b, '=');
            if (v->busy)
                put_str(&b, v->value);
            else
                var_expand(&b, v->name, strlen(v->name));
            names_add(&l, save_str(finish(&b)));
        }
    b.len = 0;
    put_str(&b, "MAKEFLAGS=");
    expand(&b, "$(MAKEFLAGS)");
    if (job_path) {
        sprintf(num, " -j%d", opt_jobs > 1 ? opt_jobs : 2);
        put_str(&b, num);
        put_str(&b, " --jobserver-auth=fifo:");
        put_str(&b, job_path);
    }
    if (overrides.len) {
        put_str(&b, " -- ");
        buf_put(&b, overrides.p, overrides.len);
    }
    names_add(&l, save_str(finish(&b)));
    sprintf(num, "%d", make_level + 1);
    b.len = 0;
    put_str(&b, "MAKELEVEL=");
    put_str(&b, num);
    names_add(&l, save_str(finish(&b)));
    names_add(&l, 0);
    buf_free(&b);
    return l.v;
}

static void free_env(char **env)
{
    char **e;
    if (!env || env == self_envp)
        return;
    for (e = env; *e; e++)
        free(*e);
    free(env);
}

static void second_expand(Target *t)
{
    char *chunks = t->second, *c = chunks;
    Target *old = scope;
    t->second = 0;
    scope = t;
    while (c && *c) {
        char *nl = strchr(c, '\n'), *x, *bar;
        if (nl)
            *nl = 0;
        x = xexpand(c);
        bar = strchr(x, '|');
        if (bar)
            *bar = 0;
        add_deps_text(t, x, 0);
        if (bar)
            add_deps_text(t, bar + 1, 1);
        free(x);
        c = nl ? nl + 1 : 0;
    }
    scope = old;
    free(chunks);
}

static void add_cmd(Target *t, const char *s, size_t n, int force)
{
    Buf b;
    size_t k;
    memset(&b, 0, sizeof b);
    while (n && is_space((u8)s[n - 1]))
        n--;
    if (force)
        buf_u8(&b, '+');
    for (k = 0; k < n; k++) {
        buf_u8(&b, (u8)s[k]);
        if (s[k] == '\n' && k + 1 < n && s[k + 1] == '\t')
            k++;
    }
    if ((t->ncmds & (t->ncmds + 1)) == 0)
        t->cmds = (char **)xrealloc(t->cmds, (2 * (size_t)t->ncmds + 2) * sizeof(char *));
    t->cmds[t->ncmds++] = finish(&b);
}

/* Expands the recipe into the commands to run, one for each line. */
static void prepare_cmds(Target *t)
{
    Target *old = scope;
    char *text, *p;
    const char *of = cur_file;
    int ol = cur_line;
    buf_u8(&t->recipe->text, 0);
    t->recipe->text.len--;
    text = save_str((const char *)t->recipe->text.p);
    scope = t;
    cur_file = t->recipe->file;
    cur_line = t->recipe->line;
    for (p = text; *p;) {
        char *e = p, *x, *q, *s;
        int force;
        while (*e && !(*e == '\n' && (e == p || e[-1] != '\\')))
            e++;
        if (*e)
            *e++ = 0;
        force = strstr(p, "$(MAKE)") || strstr(p, "${MAKE}");
        x = xexpand(p);
        for (s = x; *s;) {
            q = s;
            while (*q && !(*q == '\n' && (q == s || q[-1] != '\\')))
                q++;
            add_cmd(t, s, (size_t)(q - s), force);
            s = *q ? q + 1 : q;
        }
        free(x);
        p = e;
    }
    free(text);
    scope = old;
    cur_file = of;
    cur_line = ol;
    t->cmd_i = 0;
}

static int get_slot(Target *t)
{
    char c;
    if (!implicit_used) {
        implicit_used = 1;
        t->has_token = 0;
        return 1;
    }
    if (job_fd >= 0 && !serial && os_read(job_fd, &c, 1) == 1) {
        t->has_token = 1;
        return 1;
    }
    return 0;
}

/* Runs the commands of the target from where it stands; 1 if one is now running. */
static int start_cmd(Target *t)
{
    while (t->cmd_i < t->ncmds) {
        char *c = t->cmds[t->cmd_i], *argv[4];
        int silent = opt_silent, force = 0;
        Target *old = scope;
        t->ignore = (u8)opt_ignore;
        for (;; c++) {
            if (*c == '@')
                silent = 1;
            else if (*c == '-')
                t->ignore = 1;
            else if (*c == '+')
                force = 1;
            else if (!is_space((u8)*c))
                break;
        }
        if (!*c) {
            t->cmd_i++;
            continue;
        }
        any_ran = 1;
        if (opt_dry && !force) {
            puts(c);
            t->cmd_i++;
            continue;
        }
        if (!silent || opt_dry) {
            puts(c);
            fflush(stdout);
        }
        if (strcmp(c, ":") == 0 || strcmp(c, "true") == 0) {
            t->cmd_i++;
            continue;
        }
        if (!t->env) {
            scope = t;
            t->env = make_env(1);
            scope = old;
        }
        argv[0] = (char *)shell_path();
        argv[1] = (char *)"-c";
        argv[2] = c;
        argv[3] = 0;
        t->pid = os_run(argv, t->env, -1, -1);
        return 1;
    }
    return 0;
}

static void finish_target(Target *t)
{
    int k;
    if (t->has_token) {
        char c = '+';
        os_write(job_fd, &c, 1);
        t->has_token = 0;
    } else {
        implicit_used = 0;
    }
    for (k = 0; k < nrunning; k++)
        if (running[k] == t) {
            running[k] = running[--nrunning];
            break;
        }
    for (k = 0; k < t->ncmds; k++)
        free(t->cmds[k]);
    free(t->cmds);
    t->cmds = 0;
    t->ncmds = 0;
    free_env(t->env);
    t->env = 0;
    t->pid = 0;
    t->state = S_DONE;
    t->mtime_known = 0;
    t->ran = 1;
    dirty = 1;
}

static void target_failed(Target *t)
{
    t->failed = 1;
    any_failed = 1;
    if (!opt_keep_going)
        stopping = 1;
}

static void child_done(int id, int code)
{
    Target *t = 0;
    int k;
    for (k = 0; k < nrunning; k++)
        if (running[k]->pid == id)
            t = running[k];
    if (!t)
        return;
    if (code && t->ignore) {
        who();
        fprintf(stderr, "[%s:%d: %s] Error %d (ignored)\n", t->recipe->file, t->recipe->line, t->name, code);
        code = 0;
    }
    if (code) {
        who();
        fprintf(stderr, "*** [%s:%d: %s] Error %d\n", t->recipe->file, t->recipe->line, t->name, code);
        if (delete_on_error && !t->phony) {
            i64 m = os_mtime(t->name, 0);
            if (m && m != t->old_mtime) {
                who();
                fprintf(stderr, "*** Deleting file '%s'\n", t->name);
                os_unlink(t->name);
            }
        }
        target_failed(t);
        finish_target(t);
        return;
    }
    t->cmd_i++;
    if (!start_cmd(t))
        finish_target(t);
}

/* Brings the target up to date as far as can be done now; 1 once it is finished. */
static int update(Target *t, Target *parent)
{
    Dep *d;
    int pending = 0, bad = 0, must;
    i64 tm;
    if (t->state == S_DONE)
        return 1;
    if (t->also_of) {
        int r = update(t->also_of, parent);
        if (r) {
            t->state = S_DONE;
            t->failed = t->also_of->failed;
            t->mtime_known = 0;
        }
        return r;
    }
    if (t->state == S_RUNNING || t->walk == walk_id)
        return 0;
    if (t->visiting) {
        who();
        fprintf(stderr, "Circular %s <- %s dependency dropped.\n", parent ? parent->name : "", t->name);
        return 1;
    }
    if (t->state == S_NEW) {
        t->parent = parent;
        if (t->second)
            second_expand(t);
        if (!t->recipe)
            implicit(t, 0);
        t->state = S_DEPS;
    }
    t->visiting = 1;
    for (d = t->deps; d; d = d->next) {
        if (!update(d->t, t)) {
            pending = 1;
            if (serial)
                break;
        } else if (d->t->failed) {
            bad = 1;
        }
    }
    t->visiting = 0;
    if (pending) {
        t->walk = walk_id;
        return 0;
    }
    if (bad) {
        t->failed = 1;
        t->state = S_DONE;
        return 1;
    }
    tm = target_mtime(t);
    must = t->phony || !tm || opt_always;
    for (d = t->deps; d && !must; d = d->next) {
        i64 dm;
        if (d->order_only)
            continue;
        dm = target_mtime(d->t);
        if (!dm || dm > tm)
            must = 1;
    }
    if (!must) {
        t->state = S_DONE;
        return 1;
    }
    if (!t->recipe) {
        if (!t->phony && !tm && !t->is_target) {
            who();
            if (parent)
                fprintf(stderr, "*** No rule to make target '%s', needed by '%s'.%s\n", t->name, parent->name, opt_keep_going ? "" : "  Stop.");
            else
                fprintf(stderr, "*** No rule to make target '%s'.%s\n", t->name, opt_keep_going ? "" : "  Stop.");
            target_failed(t);
        } else if (!t->phony) {
            /* made by saying so: newer than anything */
            t->mtime = (i64)1 << 62;
            t->mtime_known = 1;
        }
        t->state = S_DONE;
        return 1;
    }
    t->old_mtime = tm;
    if (t->path) {
        t->path = 0;
    }
    prepare_cmds(t);
    t->state = S_RUNNING;
    if (nready == cap_ready) {
        cap_ready = cap_ready ? 2 * cap_ready : 64;
        ready = (Target **)xrealloc(ready, (size_t)cap_ready * sizeof(Target *));
    }
    ready[nready++] = t;
    return 0;
}

static int build(Target **goals, int n)
{
    int done = 0, k, code = 0, id, waiting_said = 0;
    dirty = 1;
    stopping = any_failed = 0;
    for (;;) {
        int progressed = 0;
        if (dirty && !stopping) {
            dirty = 0;
            walk_id++;
            done = 1;
            for (k = 0; k < n; k++)
                if (!update(goals[k], 0)) {
                    done = 0;
                    if (serial)
                        break;
                }
        }
        while (!stopping && ready_head < nready) {
            Target *t = ready[ready_head];
            if (!get_slot(t))
                break;
            ready_head++;
            if (nrunning == cap_running) {
                cap_running = cap_running ? 2 * cap_running : 16;
                running = (Target **)xrealloc(running, (size_t)cap_running * sizeof(Target *));
            }
            running[nrunning++] = t;
            if (!start_cmd(t)) {
                finish_target(t);
                progressed = 1;
            }
        }
        if (ready_head == nready)
            ready_head = nready = 0;
        if (progressed || (dirty && !stopping))
            continue;
        if (!nrunning) {
            if (stopping || done)
                break;
            fatal("nothing left to run, yet not finished", 0, 0);
        }
        if (stopping && !waiting_said) {
            waiting_said = 1;
            who();
            fprintf(stderr, "*** Waiting for unfinished jobs....\n");
        }
        if (ready_head < nready && !stopping && job_fd >= 0 && !serial) {
            /* something could start as soon as another make hands back a token */
            id = os_wait_any(&code, 0);
            if (id <= 0) {
                os_sleep_ms(2);
                continue;
            }
        } else {
            id = os_wait_any(&code, 1);
            if (id <= 0)
                fatal("lost track of the running commands", 0, 0);
        }
        child_done(id, code);
    }
    return any_failed;
}

/* ---- options ---- */

static const char *makefiles[16];
static int nmakefiles, jobs_given, said_dir;

static void version(void)
{
    puts("GNU Make 4.3 (mcc make)\nBuilt for aarch64-apple-darwin");
    os_exit(0);
}

/* Takes one option word; returns how many following words it used, or -1 if it is no option. */
static int option(const char *a, const char *next, int where)
{
    /* where: 0 command line, 1 MAKEFLAGS of the environment, 2 the MAKEFLAGS variable */
    const char *v;
    int used = 0;
    if (a[0] != '-' || !a[1])
        return -1;
    if (a[1] == '-') {
        a += 2;
        if (strcmp(a, "no-print-directory") == 0)
            opt_print_dir = 0;
        else if (strcmp(a, "print-directory") == 0)
            opt_print_dir = 1;
        else if (strncmp(a, "include-dir=", 12) == 0)
            add_include_dir(a + 12);
        else if (strncmp(a, "jobserver-auth=fifo:", 20) == 0) {
            if (where == 1 && !job_path)
                job_path = save_str(a + 20);
        } else if (strncmp(a, "jobserver-", 10) == 0)
            ;
        else if (strcmp(a, "silent") == 0 || strcmp(a, "quiet") == 0)
            flagset['s'] = 1;
        else if (strcmp(a, "keep-going") == 0)
            flagset['k'] = 1;
        else if (strcmp(a, "always-make") == 0)
            flagset['B'] = 1;
        else if (strcmp(a, "ignore-errors") == 0)
            flagset['i'] = 1;
        else if (strcmp(a, "dry-run") == 0 || strcmp(a, "just-print") == 0 || strcmp(a, "recon") == 0)
            flagset['n'] = 1;
        else if (strcmp(a, "no-builtin-rules") == 0)
            flagset['r'] = 1;
        else if (strcmp(a, "no-builtin-variables") == 0)
            flagset['R'] = 1;
        else if (strcmp(a, "version") == 0)
            version();
        else if (strncmp(a, "directory=", 10) == 0) {
            if (where == 0 && os_chdir(a + 10) != 0)
                fatal("%s: No such file or directory", a + 10, 0);
            said_dir = 1;
        } else if (strncmp(a, "file=", 5) == 0 || strncmp(a, "makefile=", 9) == 0) {
            if (where == 0 && nmakefiles < 16)
                makefiles[nmakefiles++] = strchr(a, '=') + 1;
        } else if (strncmp(a, "jobs", 4) == 0) {
            if (where == 0) {
                opt_jobs = a[4] == '=' ? atoi(a + 5) : os_ncpu();
                jobs_given = 1;
            }
        } else if (strncmp(a, "output-sync", 11) != 0 && strcmp(a, "warn-undefined-variables") != 0 &&
                   strcmp(a, "no-silent") != 0 && strcmp(a, "trace") != 0 && strcmp(a, "no-keep-going") != 0)
            fatal("unrecognized option '--%s'", a, 0);
        return 0;
    }
    for (a++; *a; a++) {
        switch (*a) {
        case 'k': case 's': case 'n': case 'B': case 'i': case 'w': case 'r': case 'R': case 'e': case 'q':
            flagset[(int)*a] = 1;
            if (*a == 'w')
                opt_print_dir = 1;
            break;
        case 'v':
            version();
            break;
        case 'j':
            v = a + 1;
            if (!(*v >= '0' && *v <= '9')) {
                v = 0;
                if (next && *next >= '0' && *next <= '9' && !a[1]) {
                    v = next;
                    used = 1;
                }
            }
            if (where == 0) {
                opt_jobs = v ? atoi(v) : os_ncpu();
                jobs_given = 1;
            }
            return used;
        case 'C': case 'f': case 'I': case 'O': case 'o': case 'W': case 'l':
            v = a + 1;
            if (!*v) {
                if (*a == 'O' || *a == 'l')
                    return 0;
                if (!next)
                    fatal("option requires an argument -- %s", a, 0);
                v = next;
                used = 1;
            }
            if (*a == 'C') {
                if (where == 0 && os_chdir(v) != 0)
                    fatal("%s: No such file or directory", v, 0);
                said_dir = 1;
            } else if (*a == 'f') {
                if (where == 0 && nmakefiles < 16)
                    makefiles[nmakefiles++] = v;
            } else if (*a == 'I') {
                add_include_dir(v);
            }
            return used;
        default:
            fatal("invalid option -- '%s'", a, 0);
        }
    }
    return 0;
}

static void cmdline_var(const char *a, const char *ne, int op, const char *val)
{
    char *name = save(a, (size_t)(ne - a));
    Var *v;
    do_assign(strip(name), op, val, O_CMDLINE);
    v = var_find(strip(name), strlen(strip(name)));
    if (v)
        v->origin = O_CMDLINE;
    if (overrides.len)
        buf_u8(&overrides, ' ');
    put_escaped(&overrides, a);
    free(name);
}

/* Reads a MAKEFLAGS value: flag letters, options, and after "--" the variables of the command line. */
static void decode_flags(const char *s, int where)
{
    Buf w;
    int first = 1, vars_now = 0;
    memset(&w, 0, sizeof w);
    for (;;) {
        const char *ne, *val;
        char *a;
        int op;
        while (is_space((u8)*s))
            s++;
        if (!*s)
            break;
        w.len = 0;
        for (; *s && !is_space((u8)*s); s++) {
            if (*s == '\\' && s[1])
                s++;
            buf_u8(&w, (u8)*s);
        }
        a = finish(&w);
        if (vars_now) {
            if (where == 1 && find_assign(a, &ne, &op, &val))
                cmdline_var(a, ne, op, val);
        } else if (strcmp(a, "--") == 0) {
            vars_now = 1;
        } else if (a[0] == '-') {
            option(a, 0, where);
        } else if (first && !strchr(a, '=')) {
            for (; *a; a++)
                if (strchr("ksnBiwrReq", *a)) {
                    flagset[(int)*a] = 1;
                    if (*a == 'w')
                        opt_print_dir = 1;
                }
        } else if (where == 1 && find_assign(a, &ne, &op, &val)) {
            cmdline_var(a, ne, op, val);
        }
        first = 0;
    }
    buf_free(&w);
}

static void apply_flags(void)
{
    opt_keep_going = flagset['k'];
    opt_silent |= flagset['s'];
    opt_dry = flagset['n'];
    opt_always = flagset['B'];
    opt_ignore |= flagset['i'];
}

static void set_makeflags(void)
{
    Buf b;
    const char *p;
    int k;
    memset(&b, 0, sizeof b);
    for (p = "BeiknqrRsw"; *p; p++)
        if (flagset[(int)*p] && !(*p == 'w' && opt_print_dir != 1))
            buf_u8(&b, (u8)*p);
    if (opt_print_dir == 0)
        put_str(&b, " --no-print-directory");
    for (k = 0; k < ninclude_dirs; k++) {
        put_str(&b, " --include-dir=");
        put_str(&b, include_dirs[k]);
    }
    var_set("MAKEFLAGS", finish(&b), 0, O_FILE);
    buf_free(&b);
}

static void leave(int code)
{
    if (said_dir == 2) {
        if (make_level)
            printf("make[%d]: Leaving directory '%s'\n", make_level, cwd);
        else
            printf("make: Leaving directory '%s'\n", cwd);
    }
    fflush(stdout);
    cleanup();
    os_exit(code);
}

/* Remakes the makefiles that rules can make; starts over if one of them changed. */
static void remake_makefiles(void)
{
    Include *inc;
    Target **goals = 0;
    i64 *before = 0;
    int n = 0, cap = 0, k, changed = 0;
    for (inc = includes; inc; inc = inc->next) {
        Target *t = target_get(inc->name);
        if (t->state != S_NEW || (!t->recipe && !implicit(t, 0)))
            continue;
        for (k = 0; k < n && goals[k] != t; k++)
            ;
        if (k < n)
            continue;
        if (n == cap) {
            cap = cap ? 2 * cap : 16;
            goals = (Target **)xrealloc(goals, (size_t)cap * sizeof(Target *));
            before = (i64 *)xrealloc(before, (size_t)cap * sizeof(i64));
        }
        before[n] = os_mtime(inc->name, 0);
        goals[n++] = t;
    }
    if (n) {
        if (build(goals, n))
            leave(2);
        for (k = 0; k < n; k++)
            if (os_mtime(goals[k]->name, 0) != before[k])
                changed = 1;
    }
    for (inc = includes; inc; inc = inc->next)
        if (!inc->found && !inc->optional && !os_exists(inc->name)) {
            cur_line = 0;
            fatal("%s: No such file or directory", inc->name, 0);
        }
    free(goals);
    free(before);
    if (changed) {
        char **argv = self_argv;
        char *prog = find_program(argv[0]);
        fflush(stdout);
        cleanup();
        os_chdir(start_dir);
        argv[0] = prog;
        os_exec(argv, self_envp);
        fatal("cannot run %s again", prog, 0);
    }
    any_ran = 0;
}

int make_main(int argc, char **argv, char **envp, int skip)
{
    char **e;
    Names goal_names;
    Target **goals;
    int k, code;
    char num[16];
    Buf b;
    memset(&b, 0, sizeof b);
    goal_names.v = 0;
    goal_names.n = 0;
    self_argv = argv;
    self_envp = envp;
    start_dir = os_getcwd();
    cwd = start_dir;
    /* how this program is to be run again */
    if (strchr(argv[0], '/'))
        abs_path(&b, argv[0], strlen(argv[0]));
    else
        put_str(&b, argv[0]);
    if (skip == 2)
        put_str(&b, " make");
    self_path = save_str(finish(&b));
    /* the environment */
    passthru = (char **)xmalloc(sizeof(char *));
    for (e = envp; e && *e; e++) {
        const char *eq = strchr(*e, '=');
        char *name;
        if (!eq)
            continue;
        name = save(*e, (size_t)(eq - *e));
        if (strcmp(name, "MAKELEVEL") == 0)
            make_level = atoi(eq + 1);
        else if (strcmp(name, "MAKEFLAGS") == 0)
            decode_flags(eq + 1, 1);
        else if (!name_ok(name) || strcmp(name, "SHELL") == 0) {
            passthru = (char **)xrealloc(passthru, (size_t)(npassthru + 1) * sizeof(char *));
            passthru[npassthru++] = *e;
        } else
            var_set(name, eq + 1, 0, O_ENV);
        free(name);
    }
    for (k = skip; k < argc; k++) {
        const char *ne, *val;
        int op, used = option(argv[k], k + 1 < argc ? argv[k + 1] : 0, 0);
        if (used >= 0)
            k += used;
        else if (find_assign(argv[k], &ne, &op, &val))
            cmdline_var(argv[k], ne, op, val);
        else
            names_add(&goal_names, argv[k]);
    }
    apply_flags();
    cwd = os_getcwd();
    var_set("CURDIR", cwd, 0, O_FILE);
    var_set("MAKE", self_path, 0, O_DEFAULT);
    var_set("MAKE_COMMAND", self_path, 0, O_DEFAULT);
    var_set("MAKE_VERSION", "4.3", 0, O_DEFAULT);
    var_set("MAKE_HOST", "aarch64-apple-darwin", 0, O_DEFAULT);
    var_set("SHELL", own_shell(argv[0]), 0, O_DEFAULT);
    var_set(".SHELLFLAGS", "-c", 0, O_DEFAULT);
    var_set(".FEATURES", "target-specific order-only second-expansion else-if shortest-stem undefine oneshell jobserver", 0, O_DEFAULT);
    var_set(".RECIPEPREFIX", "", 0, O_DEFAULT);
    sprintf(num, "%d", make_level);
    var_set("MAKELEVEL", num, 0, O_ENV);
    if (!flagset['R']) {
        var_set("CC", "cc", 0, O_DEFAULT);
        var_set("AR", "ar", 0, O_DEFAULT);
        var_set("AS", "as", 0, O_DEFAULT);
        var_set("LD", "ld", 0, O_DEFAULT);
        var_set("CXX", "c++", 0, O_DEFAULT);
        var_set("CPP", "$(CC) -E", 1, O_DEFAULT);
        var_set("RM", "rm -f", 0, O_DEFAULT);
        var_set("ARFLAGS", "rv", 0, O_DEFAULT);
    }
    b.len = 0;
    for (k = 0; k < (int)goal_names.n; k++) {
        if (k)
            buf_u8(&b, ' ');
        put_str(&b, goal_names.v[k]);
    }
    var_set("MAKECMDGOALS", finish(&b), 0, O_DEFAULT);
    buf_u8(&overrides, 0);
    overrides.len--;
    var_set("MAKEOVERRIDES", overrides.p ? (const char *)overrides.p : "", 0, O_DEFAULT);
    set_makeflags();
    /* the jobs */
    if (jobs_given) {
        free(job_path);
        job_path = 0;
        if (opt_jobs > 1) {
            const char *tmp = getenv("TMPDIR");
            char c = '+';
            job_path = (char *)xmalloc((tmp ? strlen(tmp) : 4) + 40);
            sprintf(job_path, "%s/mcc-make-%d.fifo", tmp && *tmp ? tmp : "/tmp", os_getpid());
            os_unlink(job_path);
            if (os_mkfifo(job_path) != 0)
                fatal("cannot create %s", job_path, 0);
            own_fifo = 1;
            job_fd = os_open_fifo(job_path);
            for (k = 1; k < opt_jobs; k++)
                os_write(job_fd, &c, 1);
        }
    } else if (job_path) {
        job_fd = os_open_fifo(job_path);
        if (job_fd < 0) {
            free(job_path);
            job_path = 0;
        }
    }
    serial = job_fd < 0;
    if (opt_print_dir == 1 || (opt_print_dir < 0 && !opt_silent && (make_level > 0 || said_dir))) {
        said_dir = 2;
        if (make_level)
            printf("make[%d]: Entering directory '%s'\n", make_level, cwd);
        else
            printf("make: Entering directory '%s'\n", cwd);
        fflush(stdout);
    } else {
        said_dir = 0;
    }
    if (nmakefiles) {
        for (k = 0; k < nmakefiles; k++)
            read_makefile(makefiles[k], 0, 1);
    } else {
        const char *name = os_exists("GNUmakefile") ? "GNUmakefile" : os_exists("Makefile") ? "Makefile" : "makefile";
        if (!os_exists(name))
            fatal("No targets specified and no makefile found", 0, 0);
        read_makefile(name, 0, 1);
    }
    cur_line = 0;
    {
        char *flags = xexpand("$(MAKEFLAGS)");
        decode_flags(flags, 2);
        free(flags);
        apply_flags();
    }
    if (not_parallel)
        serial = 1;
    remake_makefiles();
    if (!goal_names.n) {
        char *dg = xexpand("$(.DEFAULT_GOAL)");
        if (*strip(dg))
            names_add(&goal_names, save_str(strip(dg)));
        else if (default_goal)
            names_add(&goal_names, (char *)default_goal->name);
        else
            fatal("No targets", 0, 0);
        free(dg);
    }
    goals = (Target **)xmalloc(goal_names.n * sizeof(Target *));
    for (k = 0; k < (int)goal_names.n; k++)
        goals[k] = target_get(goal_names.v[k]);
    code = build(goals, (int)goal_names.n) ? 2 : 0;
    if (!code && !any_ran && !opt_silent)
        for (k = 0; k < (int)goal_names.n; k++) {
            if (make_level)
                printf("make[%d]: ", make_level);
            else
                printf("make: ");
            if (goals[k]->recipe && goals[k]->recipe->text.len)
                printf("'%s' is up to date.\n", goals[k]->name);
            else
                printf("Nothing to be done for '%s'.\n", goals[k]->name);
        }
    leave(code);
    return code;
}
