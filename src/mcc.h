/* mcc - minimal C compiler. Shared declarations. Strict C89, LP64 hosts. */
#ifndef MCC_H
#define MCC_H

#include <stddef.h>

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long u64;
typedef signed char i8;
typedef short i16;
typedef int i32;
typedef long i64;

/* ---- util.c ---- */

void die(const char *fmt, ...);
/* getenv with the answers remembered by the address of the name: the switches read in inner loops (MCC_NO...) cost a
   walk of the environment each time. The environment does not change while mcc runs. */
char *mcc_getenv(const char *name);
#define getenv mcc_getenv
void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);

typedef struct ArenaChunk ArenaChunk;
typedef struct Arena {
    ArenaChunk *chunks;
    char *p;
    char *end;
} Arena;

void *arena_alloc(Arena *a, size_t n);     /* zeroed, 8-byte aligned */
void *arena_alloc_raw(Arena *a, size_t n); /* not zeroed */
void arena_reset(Arena *a);                /* keep one chunk, drop contents */
void arena_free(Arena *a);

typedef struct Buf {
    u8 *p;
    size_t len;
    size_t cap;
} Buf;

void buf_reserve(Buf *b, size_t extra);
void buf_put(Buf *b, const void *src, size_t n);
void buf_zero(Buf *b, size_t n);
void buf_align(Buf *b, size_t align);
void buf_u8(Buf *b, u32 v);
void buf_u32(Buf *b, u32 v); /* little endian */
void buf_u64(Buf *b, u64 v); /* little endian */
void buf_uleb(Buf *b, u64 v);
void buf_free(Buf *b);

void put_u32le(u8 *p, u32 v);
void put_u64le(u8 *p, u64 v);
void put_u32be(u8 *p, u32 v);
void put_u64be(u8 *p, u64 v);

u32 get_u32le(const u8 *p);
u64 get_u64le(const u8 *p);

u64 align_up(u64 v, u64 align);

/* String interning: every identifier and symbol name becomes a small integer. */
u32 intern(const char *s, size_t len);
u32 intern_hashed(const char *s, size_t len, u32 hash); /* hash = HASH_STEP folded over s from HASH_INIT */
#define HASH_INIT 2166136261U
#define HASH_STEP(h, c) (((h) ^ (u8)(c)) * 16777619U)
u32 intern_cstr(const char *s);
const char *intern_str(u32 id); /* NUL terminated; id 0 is "" */
size_t intern_len(u32 id);
u32 intern_count(void);

/* ---- os.c: the only file that uses non-C89 library calls ---- */

u8 *os_read_file(const char *path, size_t *len); /* NULL if unreadable */
void os_write_file(const char *path, const u8 *data, size_t len, int executable);
int os_ncpu(void);
int os_spawn(int *fd);                             /* fork: 0 in the child (*fd writes to the parent), else the child's id (*fd reads from it) */
int os_wait(int id);                               /* exit status of a child; nonzero if it was killed */
void os_send(int fd, const u8 *data, size_t len);  /* child: write everything, close */
u8 *os_receive(int fd, size_t *len);               /* parent: read to end of file, close */
void os_exit(int status);                          /* leave without running exit handlers */
i64 os_mtime(const char *path, int *is_dir);       /* nanoseconds; 0 if the file is missing */
int os_chdir(const char *dir);
char *os_getcwd(void);
int os_run(char **argv, char **envp, int out_fd, int close_fd);
int os_wait_any(int *code, int block);
int os_pipe(int fd[2]);
long os_read(int fd, void *buf, size_t n);
long os_write(int fd, const void *buf, size_t n);
void os_close(int fd);
int os_getpid(void);
int os_exists(const char *path);
int os_mkfifo(const char *path);
int os_open_fifo(const char *path);
void os_unlink(const char *path);
void os_exec(char **argv, char **envp);
void os_sleep_ms(int ms);
int os_list_dir(const char *dir, void (*fn)(const char *name, void *ctx), void *ctx);

/* ---- sha256.c ---- */

typedef struct Sha256 {
    u32 h[8];
    u8 block[64];
    u32 fill;
    u64 total;
} Sha256;

void sha256_init(Sha256 *s);
void sha256_update(Sha256 *s, const u8 *data, size_t len);
void sha256_final(Sha256 *s, u8 out[32]);
void sha256(const u8 *data, size_t len, u8 out[32]);

/* ---- macho_exe.c: executable image writer ---- */

typedef struct ExeLayout {
    u64 text_addr; /* vm address of first byte of text */
    u64 data_addr; /* vm address of first byte of data */
    u64 bss_addr;  /* vm address of first byte of bss */
    u64 text_off;  /* file offset of text */
    int has_data;
} ExeLayout;

typedef struct ExeImage {
    const u8 *text; /* code and read-only data */
    size_t text_len;
    const u8 *data; /* initialized writable data */
    size_t data_len;
    size_t bss_len;
    u64 entry_off;      /* entry point, offset into text */
    const u64 *rebases; /* sorted offsets into data of absolute 64-bit pointers */
    size_t nrebases;
    const char *ident; /* code signature identifier */
    const u8 *symtab; /* nlist_64 entries, or 0 */
    size_t nsyms;
    const u8 *strtab;
    size_t strtab_len;
    const u64 *binds; /* offsets into data of pointers that dyld fills in with imported symbols */
    const char *const *bind_names;
    size_t nbinds;
} ExeImage;

/* set by the driver: libraries to load besides libSystem, and entitlements for the signature */
#define EXE_MAXLIBS 8
extern const char *exe_dylibs[EXE_MAXLIBS], *exe_ents[EXE_MAXLIBS];
extern int exe_ndylibs, exe_nents;
int exe_option(const char *arg, const char *next); /* 0, or the number of arguments used */

void macho_exe_layout(size_t text_len, size_t data_len, size_t bss_len, ExeLayout *lay);
void macho_exe_write(Buf *out, const ExeImage *img);

/* ---- lex.c: source files, locations, raw tokens ---- */

enum {
    T_EOF,
    T_IDENT, /* v = interned name */
    T_NUM,   /* preprocessing number: p, v = length */
    T_CHAR,  /* character constant, spelling in p, v */
    T_STR,   /* string literal, spelling in p, v */
    T_OTHER, /* stray character */
    T_PARAM, /* preprocessor internal: macro parameter, v = index */
    T_PLACEMARKER,
    T_LBRACK,
    T_RBRACK,
    T_LPAREN,
    T_RPAREN,
    T_LBRACE,
    T_RBRACE,
    T_DOT,
    T_ARROW,
    T_INC,
    T_DEC,
    T_AMP,
    T_STAR,
    T_PLUS,
    T_MINUS,
    T_TILDE,
    T_NOT,
    T_SLASH,
    T_PERCENT,
    T_SHL,
    T_SHR,
    T_LT,
    T_GT,
    T_LE,
    T_GE,
    T_EQ,
    T_NE,
    T_CARET,
    T_PIPE,
    T_ANDAND,
    T_OROR,
    T_QUESTION,
    T_COLON,
    T_ASSIGN,
    T_MULEQ,
    T_DIVEQ,
    T_MODEQ,
    T_ADDEQ,
    T_SUBEQ,
    T_SHLEQ,
    T_SHREQ,
    T_ANDEQ,
    T_XOREQ,
    T_OREQ,
    T_COMMA,
    T_HASH,
    T_HASHHASH,
    T_SEMI,
    T_ELLIPSIS,
    T_SCOPE, /* C++ :: */
    T_KEYWORD /* first keyword; keyword k has kind T_KEYWORD + k and name id k + 1 */
};

enum {
    KW_AUTO, KW_BREAK, KW_CASE, KW_CHAR, KW_CONST, KW_CONTINUE, KW_DEFAULT, KW_DO,
    KW_DOUBLE, KW_ELSE, KW_ENUM, KW_EXTERN, KW_FLOAT, KW_FOR, KW_GOTO, KW_IF,
    KW_INT, KW_LONG, KW_REGISTER, KW_RETURN, KW_SHORT, KW_SIGNED, KW_SIZEOF, KW_STATIC,
    KW_STRUCT, KW_SWITCH, KW_TYPEDEF, KW_UNION, KW_UNSIGNED, KW_VOID, KW_VOLATILE, KW_WHILE,
    /* GNU C and later standards; each is spelled as in tok_names, most have aliases (lex.c) */
    KW_INLINE, KW_RESTRICT, KW_TYPEOF, KW_ASM, KW_ATTRIBUTE, KW_EXTENSION, KW_ALIGNOF, KW_BOOL,
    KW_LABEL, KW_STATIC_ASSERT, KW_INT128, KW_NORETURN, KW_GENERIC, KW_AUTO_TYPE, KW_ATOMIC, KW_THREAD_LOCAL,
    KW_COUNT
};

extern int opt_cuda;           /* CUDA: device code goes to PTX */
extern int opt_c23, opt_cxx;   /* C23 keywords and syntax; C++ subset */
extern int opt_gnu;         /* 0: strict C89, so inline, asm and typeof are plain identifiers */
extern int opt_c99;         /* -std=c99 or later (C or C++): no implicit function declarations, no implicit int */
extern int opt_permissive;  /* PERM_*: what C99 removed, accepted again on request (with a warning at each use) */
enum { PERM_IMPLICIT_FN = 1, PERM_IMPLICIT_INT = 2 };
extern int opt_syntax_only; /* parse and check, generate nothing */
extern int opt_skip_asm;    /* debugging aid: inline assembly generates nothing */
int lex_keyword(u32 name);  /* token kind of a keyword or one of its aliases, else 0 */

#define TF_BOL 1       /* first token on its line */
#define TF_SPACE 2     /* preceded by white space */
#define TF_NOEXPAND 4  /* identifier that must not be macro-expanded again */
#define TF_WIDE 8      /* L'x' or L"x" */
#define TF_STRINGIFY 16 /* macro parameter preceded by # */

typedef struct Token {
    u8 kind;
    u8 flags;
    u32 loc;
    u32 v; /* spelling length, or interned name for identifiers */
    const char *p;
} Token;

typedef struct SrcFile {
    const char *name;
    char *data; /* NUL terminated */
    size_t len;
    u32 base;    /* location of data[0] */
    u32 guard;   /* include guard macro, if the whole file is wrapped in one */
    u32 once;
    u32 *lines;  /* -g: offset of the start of each line, made when first needed (loc_line) */
    u32 nlines;
    struct SrcFile *next;
} SrcFile;

typedef struct Lexer {
    const char *p;
    const char *start;
    const char *end;
    u32 base;
    int bol;
    int lenient; /* inside a skipped conditional group: no errors */
} Lexer;

extern const char *const tok_names[];

void lex_init(void);                                             /* interns the keywords */
SrcFile *src_load(const char *path);                             /* cached; NULL if unreadable */
SrcFile *src_buffer(const char *name, const char *text, size_t len);
void src_set_line(u32 loc, u32 line, const char *name); /* #line */
void loc_decode(u32 loc, const char **file, u32 *line, u32 *col);
void loc_line(u32 loc, const char **file, u32 *line, u32 *col); /* the same, quickly: for line tables (-g) */
void error_at(u32 loc, const char *fmt, ...);
void warn_at(u32 loc, const char *fmt, ...);
void lex_start(Lexer *lx, SrcFile *f);
void lex_raw(Lexer *lx, Token *t);
void tok_spell(const Token *t, Buf *b);

#define NUM_UNSIGNED 1
#define NUM_LONG 2
#define NUM_FLOAT 4   /* f suffix */
#define NUM_DECIMAL 8 /* decimal integer constant */
#define NUM_LLONG 16  /* ll suffix (with NUM_LONG) */
/* Returns 1 for a floating constant (value in *fval), 0 for an integer. */
int lex_number(const Token *t, u64 *ival, double *fval, int *flags);
int lex_char(const Token *t);
void lex_string(const Token *t, Buf *out); /* appends decoded bytes (ints if wide), no terminator */

/* ---- pp.c: preprocessor ---- */

void pp_init(void);
void pp_include_dir(const char *dir);
void pp_system_dir(const char *dir);    /* -isystem */
void pp_force_include(const char *path); /* -include */
void pp_text(Buf *b);
void pp_undef(const char *name);
void src_deps(Buf *b); /* " name" for every file read, the first one first */
void pp_prefix_map(const char *map);
extern int pp_asm;
/* eld.c: the ELF linker */
void eld_add(const char *name, const u8 *d, size_t len, int whole);
void eld_add_file(const char *path, int whole);
void eld_link(const char *out_path, const char *entry);
void eld_relocatable(const char *out_path);
void eld_link_script(const char *out_path, const char *script_path, const char *entry);
int elf_ld_main(int argc, char **argv);
int elf_ar_main(int argc, char **argv);
int elf_nm_main(int argc, char **argv);
int elf_objdump_main(int argc, char **argv);
int elf_objcopy_main(int argc, char **argv, int strip);
extern Buf *opt_asm_text; /* -S: inline assembly is collected here, not assembled */
void pp_define(const char *def); /* "NAME" or "NAME=value" */
void pp_open(const char *path);
void pp_next(Token *t);
int pp_in_system(u32 loc);
void pp_c_dir(const char *dir);
void pp_dump(void); /* -E */

/* ---- obj.c: in-memory relocatable object ---- */

/* SEC_IR holds functions as IR for link-time optimisation (lto.c); it is
   kept beside the four real sections and never reaches an executable. */
enum {
    SEC_TEXT, SEC_CONST, SEC_DATA, SEC_BSS, SEC_COUNT, SEC_IR = SEC_COUNT, SEC_NSTORE,
    SEC_COMMON = 0xfffd, SEC_UNDEF = 0xfffe, SEC_NONE = 0xffff
};

/* Named sections (ELF targets) come after the standard ones. */
#define SF_ALLOC 1
#define SF_WRITE 2
#define SF_EXEC 4
#define SF_NOBITS 8
#define SF_MERGE 16
#define SF_STRINGS 32
#define SF_TLS 64 /* thread-local: every thread has a copy */

enum { TARGET_MACOS, TARGET_LINUX };
extern int opt_target;
enum { ARCH_ARM64, ARCH_X64 };
extern int opt_arch; /* ARCH_X64 only with TARGET_LINUX */
extern int opt_avx;  /* x86-64: AVX may be used for vector types (-mavx, -march=...) (parse.c) */
extern int opt_f16c;
extern int opt_avx512; /* x86-64: 64-byte vectors in zmm registers (EVEX), -mavx512f */ /* x86-64: F16C converts _Float16 (-mf16c, -march=...) (parse.c) */

enum {
    R_ABS64, R_CALL26, R_PAGE21, R_PAGEOFF12, R_GOTPAGE21, R_GOTPAGEOFF12,
    /* ELF only */
    R_ABS32, R_PREL32, R_PREL64, R_JUMP26, R_CONDBR19, R_TSTBR14, R_ADR21, R_LIT19, R_ABS16, R_PREL16,
    R_TPREL_HI12, R_TPREL_LO12, /* the offset of a thread-local object from the thread pointer, in two adds */
    /* x86-64 */
    R_X64_PC32, R_X64_PLT32, R_X64_32, R_X64_32S, R_X64_TPOFF32, R_X64_PC8
};

typedef struct ObjSym {
    u64 value; /* offset in section; size for SEC_COMMON */
    u32 name;  /* interned linker name, 0 if anonymous */
    u16 sec;
    u8 global;
    u8 align; /* SEC_COMMON only, log2 */
    u8 weak;
    u8 kind;  /* 0 unknown, 1 function, 2 object, 3 thread-local object */
    u8 hidden;
    u8 trap; /* a function that must never be called (the error attribute) */
    u8 isdev; /* CUDA: __builtin_mcuda_device: a call gives 1 in device code and 0 in host code */
    u64 size;
    u64 dsize; /* an object's size by its declaration, when that is known (defined here or not); else 0 */
} ObjSym;

typedef struct ObjReloc {
    i64 addend;
    u32 off;
    u32 sym;
    u32 type;
} ObjReloc;

typedef struct Obj {
    const char *name;
    Buf *sec; /* SEC_BSS (and SF_NOBITS sections) have a length but no bytes */
    u32 *align;
    ObjSym *syms;
    u32 nsyms, capsyms;
    ObjReloc **rel;
    u32 *nrel, *caprel;
    u32 *secname;  /* named sections: interned name (0 for the standard ones) */
    u32 *secflags; /* SF_* */
    u32 *secent;   /* entry size of a mergeable section */
    u32 nsec, capsec;
    u32 *byname; /* linker name -> symbol + 1, for the symbols made by obj_named_sym */
    u32 capnames;
    u64 base[SEC_COUNT]; /* set by the linker: offset in merged section */
    int lazy;            /* archive member: link only if needed */
    int loaded;
} Obj;

Obj *obj_new(const char *name);
u32 obj_add_sym(Obj *o, u32 name, int sec, u64 value, int global);
void obj_add_reloc(Obj *o, int sec, u32 off, int type, u32 sym, i64 addend);
u64 obj_reserve(Obj *o, int sec, u64 size, u32 align); /* returns offset; no bytes written */
int obj_section_new(Obj *o, u32 name, u32 flags);
u32 obj_find_sym(Obj *o, u32 name);
u32 obj_named_sym(Obj *o, u32 name);
void obj_bind_name(Obj *o, u32 sym, u32 name);
int obj_section(Obj *o, u32 name, u32 flags);          /* the section of that name, made if new */

/* ---- dwarf.c: debug information (-g) ---- */

struct Type; /* cc.h */

extern int opt_debug; /* -g: line tables, the functions of each unit, frame descriptions */
/* a change of the frame, at an offset in a function's code (x86-64): */
enum { CFI_PUSHFP,  /* rbp pushed: the frame is 16 bytes above rsp, rbp saved at its bottom */
       CFI_SETFP,   /* rbp holds rsp: the frame is found from rbp from here on */
       CFI_SAVE,    /* register reg (the processor's number) saved n * 8 bytes below the frame */
       CFI_EPI,     /* the frame is gone (after leave): rsp + 8, until the next CFI_RESTORE */
       CFI_RESTORE,
       CFI_SUBSP }; /* rsp lowered (no frame pointer): the frame is v bytes above rsp */
typedef struct DwCfi {
    u32 off, v;
    u8 op, reg, n;
} DwCfi;
void dwarf_reset(const char *path);
void dwarf_func_loc(u32 sym, u32 loc, struct Type *ret); /* where the definition of a function is, its result type */
/* a parameter or local of function fsym, in stack slot slot (byref: the slot holds its address) */
void dwarf_var(u32 fsym, u32 name, struct Type *ty, u32 slot, int param, int byref, u32 loc);
void dwarf_scope_open(u32 fsym, u32 loc); /* a block of fsym's body begins at loc (its variables are in it) */
void dwarf_scope_close(u32 loc);           /* ... and ends before loc */
u32 dwarf_vars_peek(u32 fsym);         /* ... without starting afresh (the optimiser: which slots are variables) */
u32 dwarf_vars(u32 fsym);              /* the back end: 1 + its first variable, or 0 */
u32 dwarf_var_next(u32 k);
u32 dwarf_var_slot(u32 k);
int dwarf_var_byref(u32 k);
void dwarf_var_frame(u32 k, i32 cfa_off); /* it stays in memory, at the frame's address + cfa_off */
void dwarf_var_place(u32 k, u32 start, u32 end, const u8 *expr, u32 len, u32 rank); /* there from start to end (DWARF expression) */
u32 dwarf_get_func_loc(u32 sym);
u32 dwarf_file(const char *name);      /* its index in the unit's table of files */
const char *dwarf_file_name(u32 k);
u32 dwarf_nfiles(void);
void dwarf_where(u32 loc, u32 *file, u32 *line, u32 *col);
u32 dwarf_line_of(u32 loc); /* the source line of loc, 0 if none (reports) */
u32 dwarf_inline(u32 callee, u32 call_loc);  /* an inlined call of callee (1 + its number) */
u32 dwarf_inline_loc(u32 inst, u32 loc);     /* the location of code at loc copied into inlined call inst */
/* a function's code is final: size bytes; the source location from off[k] on is loc[k]; its frame changes */
void dwarf_func(u32 sym, u32 size, const u32 *off, const u32 *loc, u32 n, const DwCfi *cf, u32 ncf, u32 prologue_end);
void dwarf_finish(Obj *o); /* the unit's sections */

/* ---- elf.c: ELF relocatable objects (aarch64) ---- */

void elf_write_obj(Buf *out, Obj *o);
Obj *elf_read_obj(const char *name, const u8 *d, size_t len, int full); /* x86-64 ELF object back (lto) */

/* ---- macho.c: MH_OBJECT files and archives ---- */

Obj *macho_read_obj(const char *name, const u8 *data, size_t len);
void macho_write_obj(Buf *out, Obj *o);
/* Reads path (.o or .a) and appends its objects. */
void macho_load_file(const char *path, Obj ***objs, size_t *nobjs);
int ar_main(int argc, char **argv);

/* ---- arm64_asm.c: assembler ---- */

Obj *as_file(const char *path);
/* gas.c: GNU syntax, ELF targets */
void gas_begin(Obj *o);
void gas_text(const char *text, size_t len, int sec, const char *path, u32 line, int file);
void gas_end(void);
u32 gas_pending(u32 k);
Obj *gas_file(const char *path, int cpp);
int as_main(int argc, char **argv);
int arm64_logical_imm(u64 v, int w64, u32 *enc);

/* ---- link.c ---- */

void link_exe(Obj **objs, size_t nobjs, const char *entry, const char *out_path);
int ld_main(int argc, char **argv);

/* ---- lto.c: link-time optimisation ---- */

/* Replaces the objects that carry IR by one object compiled from all of them. */
void lto_link(Obj **objs, size_t *nobjs, const char *entry);

int lex_main(int argc, char **argv);
int yacc_main(int argc, char **argv);
int make_main(int argc, char **argv, char **envp, int skip);
#endif
