/* x86-64 instructions in AT&T syntax. This is a part of gas.c, which includes it: it uses the
   expression parser, the operand splitter and the fixup list of the assembler. */

enum { XO_NONE, XO_REG, XO_XMM, XO_SEG, XO_IMM, XO_MEM };
enum { XR_NONE, XR_PLT, XR_TPOFF };

typedef struct XOp {
    u8 kind, size, reg, hi, star, seg;
    u8 hasbase, base, hasindex, index, scale, rip, rel, a32;
    Val v;
} XOp;

typedef struct XFix {
    int at, kind;
    Val v;
} XFix;

static XOp xo[4];
static int xnops;
static u8 xb[32];
static int xn, x_need_rex, x_no_rex;
static XFix xfx[3];
static int nxfx;

static int x64_settle(Fix *fx, Val v, u8 *at)
{
    ObjSym *a = &go->syms[v.add - 1];
    i64 d;
    switch (fx->kind) {
    case FX_XPC8:
        if (!defined(v.add) || a->sec != fx->sec)
            err("a short jump must stay in its section", 0);
        d = (i64)a->value + v.k - (i64)fx->off;
        if (d < -128 || d > 127)
            err("branch target out of range", 0);
        at[0] = (u8)d;
        return 1;
    case FX_XPC32:
    case FX_XPLT32:
        if (defined(v.add) && a->sec == fx->sec && !a->weak) {
            put_u32le(at, (u32)((i64)a->value + v.k - (i64)fx->off));
            return 1;
        }
        obj_add_reloc(go, (int)fx->sec, fx->off, fx->kind == FX_XPC32 ? R_X64_PC32 : R_X64_PLT32, v.add - 1, v.k);
        return 1;
    case FX_X32: obj_add_reloc(go, (int)fx->sec, fx->off, R_X64_32, v.add - 1, v.k); return 1;
    case FX_XTPOFF: obj_add_reloc(go, (int)fx->sec, fx->off, R_X64_TPOFF32, v.add - 1, v.k); return 1;
    default: obj_add_reloc(go, (int)fx->sec, fx->off, R_X64_32S, v.add - 1, v.k); return 1;
    }
}

static void x_addfix(int kind, Val v, u32 off)
{
    Fix fx;
    fx.v = v;
    fx.sec = cur;
    fx.off = off;
    fx.line = as_line;
    fx.kind = (u8)kind;
    if (settle(&fx, 0))
        return;
    if (nfixes == capfixes) {
        capfixes = capfixes ? capfixes * 2 : 256;
        fixes = (Fix *)xrealloc(fixes, capfixes * sizeof(Fix));
    }
    fixes[nfixes++] = fx;
}

/* ---- operands ---- */

static size_t x_reg(const char *s, XOp *o)
{
    static const char *const names[5][8] = {
        { "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi" },
        { "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi" },
        { "ax", "cx", "dx", "bx", "sp", "bp", "si", "di" },
        { "al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil" },
        { "es", "cs", "ss", "ds", "fs", "gs", "ah", "ch" }
    };
    static const u8 sizes[4] = { 8, 4, 2, 1 };
    char b[8];
    size_t n = 0;
    int k, j;
    while (n < 7 && ((s[n] >= 'a' && s[n] <= 'z') || (s[n] >= 'A' && s[n] <= 'Z') || (s[n] >= '0' && s[n] <= '9'))) {
        b[n] = (char)(s[n] | 0x20);
        n++;
    }
    b[n] = 0;
    o->kind = XO_REG;
    o->hi = 0;
    if (b[0] == 'x' && b[1] == 'm' && b[2] == 'm' && b[3] >= '0' && b[3] <= '9') {
        k = b[3] - '0';
        if (b[4]) {
            if (b[4] < '0' || b[4] > '9' || b[5])
                return 0;
            k = k * 10 + b[4] - '0';
        }
        if (k > 15)
            return 0;
        o->kind = XO_XMM;
        o->reg = (u8)k;
        o->size = 16;
        return n;
    }
    if (b[0] == 'r' && b[1] >= '0' && b[1] <= '9') {
        k = b[1] - '0';
        j = 2;
        if (b[2] >= '0' && b[2] <= '9')
            k = k * 10 + b[j++] - '0';
        if (k < 8 || k > 15)
            return 0;
        o->reg = (u8)k;
        o->size = (u8)(!b[j] ? 8 : b[j] == 'd' ? 4 : b[j] == 'w' ? 2 : b[j] == 'b' || b[j] == 'l' ? 1 : 0);
        if (!o->size || (b[j] && b[j + 1]))
            return 0;
        return n;
    }
    if (strcmp(b, "rip") == 0) {
        o->reg = 16;
        o->size = 8;
        return n;
    }
    if (strcmp(b, "dh") == 0 || strcmp(b, "bh") == 0) {
        o->reg = (u8)(b[0] == 'd' ? 6 : 7);
        o->size = 1;
        o->hi = 1;
        return n;
    }
    for (k = 0; k < 5; k++)
        for (j = 0; j < 8; j++)
            if (strcmp(b, names[k][j]) == 0) {
                o->reg = (u8)j;
                if (k < 4) {
                    o->size = sizes[k];
                } else if (j < 6) {
                    o->kind = XO_SEG;
                    o->size = 2;
                } else {
                    o->reg = (u8)(j - 2);
                    o->size = 1;
                    o->hi = 1;
                }
                return n;
            }
    return 0;
}

static char *x_sp(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    return s;
}

static char *x_memreg(char *s, u8 *r, XOp *o)
{
    XOp t;
    size_t n = 0;
    if (*s != '%' || (n = x_reg(s + 1, &t)) == 0 || t.kind != XO_REG || (t.size != 8 && t.size != 4))
        err("expected a 64-bit register at", s);
    if (t.size == 4)
        o->a32 = 1; /* a 32-bit address */
    *r = t.reg;
    return x_sp(s + 1 + n);
}

static void x_operand(char *s, XOp *o)
{
    static const u8 segp[6] = { 0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65 };
    size_t n;
    char *t;
    memset(o, 0, sizeof *o);
    if (*s == '*') {
        o->star = 1;
        s = x_sp(s + 1);
    }
    if (*s == '$') {
        p = s + 1;
        o->kind = XO_IMM;
        o->v = expr();
        simplify(&o->v);
        skip_ws();
        if (*p)
            err("syntax error at", p);
        return;
    }
    if (*s == '%') {
        n = x_reg(s + 1, o);
        if (!n)
            err("unknown register", s);
        s = x_sp(s + 1 + n);
        if (o->kind == XO_SEG && *s == ':') {
            o->seg = segp[o->reg];
            s = x_sp(s + 1);
        } else {
            if (*s)
                err("syntax error at", s);
            if (o->kind == XO_REG && o->size == 1) {
                if (o->hi)
                    x_no_rex = 1;
                else if (o->reg >= 4 && o->reg < 8)
                    x_need_rex = 1;
            }
            return;
        }
    }
    o->kind = XO_MEM;
    o->reg = 0;
    t = *s == '(' ? x_sp(s + 1) : s;
    if (*s && !(*s == '(' && (*t == '%' || *t == ','))) {
        p = s;
        o->v = expr();
        simplify(&o->v);
        if (*p == '@') {
            t = ++p;
            while (is_ident(*p))
                p++;
            n = (size_t)(p - t);
            if (n == 3 && (memcmp(t, "PLT", 3) == 0 || memcmp(t, "plt", 3) == 0))
                o->rel = XR_PLT;
            else if (n == 5 && (memcmp(t, "tpoff", 5) == 0 || memcmp(t, "TPOFF", 5) == 0))
                o->rel = XR_TPOFF;
            else
                err("this relocation is not supported: @", t);
        }
        s = x_sp(p);
    }
    if (*s == '(') {
        s = x_sp(s + 1);
        if (*s == '%') {
            o->hasbase = 1;
            s = x_memreg(s, &o->base, o);
            if (o->base == 16) {
                o->hasbase = 0;
                o->rip = 1;
            }
        }
        if (*s == ',') {
            s = x_sp(s + 1);
            if (*s == '%') {
                o->hasindex = 1;
                s = x_memreg(s, &o->index, o);
                if (o->index == 4 || o->index == 16)
                    err("this register cannot be an index", 0);
            }
            if (*s == ',') {
                i64 sc;
                p = s + 1;
                sc = const_expr();
                s = x_sp(p);
                if (sc != 1 && sc != 2 && sc != 4 && sc != 8)
                    err("the scale must be 1, 2, 4 or 8", 0);
                o->scale = (u8)(sc == 1 ? 0 : sc == 2 ? 1 : sc == 4 ? 2 : 3);
            }
        }
        if (*s != ')')
            err("expected ')' at", s);
        s = x_sp(s + 1);
    }
    if (*s)
        err("syntax error at", s);
    if (o->rip && o->hasindex)
        err("%rip cannot have an index", 0);
}

/* ---- encoding ---- */

static void xbyte(u32 b)
{
    if (xn >= (int)sizeof xb)
        err("instruction too long", 0);
    xb[xn++] = (u8)b;
}

static int x_isconst(const XOp *o)
{
    return !o->v.add && !o->v.sub;
}

/* an immediate or a displacement */
static void xval(Val v, int bytes, int kind)
{
    int k;
    if (v.add || v.sub) {
        if (nxfx == 3)
            err("too many symbols in one instruction", 0);
        xfx[nxfx].at = xn;
        xfx[nxfx].kind = bytes == 8 ? FX_D8 : bytes == 2 ? FX_D2 : bytes == 1 ? FX_D1 : kind;
        xfx[nxfx++].v = v;
        v.k = 0;
    }
    for (k = 0; k < bytes; k++)
        xbyte((u32)((u64)v.k >> (8 * k)) & 0xff);
}

static void x_modrm(int reg, const XOp *rm)
{
    int mod, b, kind = rm->rel == XR_TPOFF ? FX_XTPOFF : FX_X32S;
    reg = (reg & 7) << 3;
    if (rm->kind != XO_MEM) {
        xbyte((u32)(0xc0 | reg | (rm->reg & 7)));
        return;
    }
    if (rm->rip) {
        xbyte((u32)(reg | 5));
        if (x_isconst(rm))
            err("a %rip-relative address needs a symbol", 0);
        xval(rm->v, 4, rm->rel == XR_PLT ? FX_XPLT32 : FX_XPC32);
        return;
    }
    if (!rm->hasbase) {
        xbyte((u32)(reg | 4));
        xbyte((u32)(rm->hasindex ? (rm->scale << 6) | ((rm->index & 7) << 3) | 5 : 0x25));
        xval(rm->v, 4, kind);
        return;
    }
    b = rm->base & 7;
    mod = !x_isconst(rm) ? 2 : rm->v.k == 0 && b != 5 ? 0 : rm->v.k >= -128 && rm->v.k <= 127 ? 1 : 2;
    if (rm->hasindex || b == 4) {
        xbyte((u32)((mod << 6) | reg | 4));
        xbyte((u32)((rm->scale << 6) | ((rm->hasindex ? rm->index & 7 : 4) << 3) | b));
    } else {
        xbyte((u32)((mod << 6) | reg | b));
    }
    if (mod == 1)
        xbyte((u32)rm->v.k & 0xff);
    else if (mod == 2)
        xval(rm->v, 4, kind);
}

/* pfx: a mandatory prefix or 0; size: 2 and 8 select the operand size; opc: one to three bytes */
static void x_enc(int pfx, int size, u32 opc, int reg, const XOp *rm)
{
    int rex = 0;
    if (rm->kind == XO_MEM && rm->seg)
        xbyte(rm->seg);
    if (rm->kind == XO_MEM && rm->a32)
        xbyte(0x67);
    if (size == 2)
        xbyte(0x66);
    if (pfx)
        xbyte((u32)pfx);
    if (size == 8)
        rex |= 8;
    if (reg & 8)
        rex |= 4;
    if (rm->kind == XO_MEM) {
        if (rm->hasindex && (rm->index & 8))
            rex |= 2;
        if (rm->hasbase && (rm->base & 8))
            rex |= 1;
    } else if (rm->reg & 8) {
        rex |= 1;
    }
    if (rex || x_need_rex) {
        if (x_no_rex)
            err("%ah, %bh, %ch and %dh cannot be used in this instruction", 0);
        xbyte((u32)(0x40 | rex));
    }
    if (opc > 0xffff)
        xbyte(opc >> 16);
    if (opc > 0xff)
        xbyte((opc >> 8) & 0xff);
    xbyte(opc & 0xff);
    x_modrm(reg, rm);
}

/* opcode + register, no modrm */
static void x_plusr(int size, u32 opc, int reg)
{
    int rex = (size == 8 ? 8 : 0) | (reg & 8 ? 1 : 0);
    if (size == 2)
        xbyte(0x66);
    if (rex || x_need_rex)
        xbyte((u32)(0x40 | rex));
    if (opc > 0xff)
        xbyte(opc >> 8);
    xbyte((opc & 0xff) + (u32)(reg & 7));
}

static void x_flush(void)
{
    u32 start = here();
    int k;
    buf_put(B(), xb, (size_t)xn);
    for (k = 0; k < nxfx; k++) {
        Val v = xfx[k].v;
        if (xfx[k].kind == FX_XPC32 || xfx[k].kind == FX_XPLT32 || xfx[k].kind == FX_XPC8)
            v.k -= xn - xfx[k].at;
        x_addfix(xfx[k].kind, v, start + (u32)xfx[k].at);
    }
}

static int x_suffix_size(int c)
{
    return c == 'b' ? 1 : c == 'w' ? 2 : c == 'l' ? 4 : c == 'q' ? 8 : 0;
}

static int x_size(int sfx)
{
    int k;
    if (sfx)
        return sfx;
    for (k = xnops - 1; k >= 0; k--)
        if (xo[k].kind == XO_REG)
            return xo[k].size;
    err("the operand size is unknown: add a suffix", 0);
    return 0;
}

static void x_imm(const XOp *o, int size)
{
    xval(o->v, size == 8 ? 4 : size, size == 4 ? FX_X32 : FX_X32S);
}

static int x_fits8(const XOp *o)
{
    return x_isconst(o) && o->v.k >= -128 && o->v.k <= 127;
}

static int x_cc(const char *s)
{
    static const char *const names[] = { "o", "no", "b", "ae", "e", "ne", "be", "a", "s", "ns", "p", "np", "l", "ge", "le", "g",
        "", "", "c", "nc", "z", "nz", "na", "nbe", "", "", "pe", "po", "nge", "nl", "ng", "nle",
        "", "", "nae", "nb", 0 };
    int k;
    for (k = 0; names[k]; k++)
        if (names[k][0] && strcmp(names[k], s) == 0)
            return k & 15;
    return -1;
}

static int x_ismem(const XOp *o)
{
    return o->kind == XO_MEM;
}

static int x_isrm(const XOp *o)
{
    return o->kind == XO_MEM || o->kind == XO_REG;
}

static void x_want(int ok)
{
    if (!ok)
        err("these operands do not fit the instruction", 0);
}

/* ---- instructions without operands ---- */

static int x_simple(const char *m)
{
    static const struct { const char *name; u32 code; } t[] = {
        { "ret", 0xc3 }, { "retq", 0xc3 }, { "leave", 0xc9 }, { "leaveq", 0xc9 }, { "nop", 0x90 },
        { "syscall", 0x0f05 }, { "hlt", 0xf4 }, { "ud2", 0x0f0b }, { "cltq", 0x4898 }, { "cdqe", 0x4898 },
        { "cqto", 0x4899 }, { "cqo", 0x4899 }, { "cltd", 0x99 }, { "cdq", 0x99 }, { "cwtl", 0x98 },
        { "cwtd", 0x6699 }, { "cbtw", 0x6698 }, { "cld", 0xfc }, { "std", 0xfd }, { "pause", 0xf390 },
        { "cpuid", 0x0fa2 }, { "rdtsc", 0x0f31 }, { "rdtscp", 0x0f01f9 }, { "mfence", 0x0faef0 },
        { "lfence", 0x0faee8 }, { "sfence", 0x0faef8 }, { "int3", 0xcc }, { "endbr64", 0xf30f1efa },
        { "movsb", 0xa4 }, { "movsw", 0x66a5 }, { "movsl", 0xa5 }, { "movsq", 0x48a5 },
        { "stosb", 0xaa }, { "stosw", 0x66ab }, { "stosl", 0xab }, { "stosq", 0x48ab },
        { "lodsb", 0xac }, { "lodsw", 0x66ad }, { "lodsl", 0xad }, { "lodsq", 0x48ad },
        { "cmpsb", 0xa6 }, { "cmpsw", 0x66a7 }, { "cmpsl", 0xa7 }, { "cmpsq", 0x48a7 },
        { "scasb", 0xae }, { "scasw", 0x66af }, { "scasl", 0xaf }, { "scasq", 0x48af },
        { "clc", 0xf8 }, { "stc", 0xf9 }, { "cmc", 0xf5 }, { "cli", 0xfa }, { "sti", 0xfb },
        { "pushfq", 0x9c }, { "popfq", 0x9d }, { "pushf", 0x9c }, { "popf", 0x9d }, { "sahf", 0x9e }, { "lahf", 0x9f },
        { "emms", 0x0f77 }, { "xgetbv", 0x0f01d0 }, { "rdmsr", 0x0f32 }, { "wrmsr", 0x0f30 },
        { "iretq", 0x48cf }, { "swapgs", 0x0f01f8 }, { "sysretq", 0x480f07 }, { "sysretl", 0x0f07 },
        { "fninit", 0xdbe3 }, { "fwait", 0x9b }, { "wait", 0x9b }, { "clts", 0x0f06 }, { "wbinvd", 0x0f09 },
        { "vzeroupper", 0xc5f877 }, { "xlat", 0xd7 }, { "rdpmc", 0x0f33 }, { 0, 0 }
    };
    int k, sh;
    for (k = 0; t[k].name; k++)
        if (strcmp(t[k].name, m) == 0) {
            for (sh = 24; sh >= 0; sh -= 8)
                if ((t[k].code >> sh) || !sh)
                    xbyte((t[k].code >> sh) & 0xff);
            return 1;
        }
    return 0;
}

/* ---- SSE ---- */

enum { S_RM, S_RMI, S_MOV, S_GD, S_GS, S_SH, S_EXT, S_MOVD, S_MOVQ, S_RR };

typedef struct XSse {
    const char *name;
    u8 pfx, opc, kind, x;
} XSse;

static const XSse x_sse_tab[] = {
    { "movaps", 0, 0x28, S_MOV, 0x29 }, { "movups", 0, 0x10, S_MOV, 0x11 }, { "movapd", 0x66, 0x28, S_MOV, 0x29 },
    { "movupd", 0x66, 0x10, S_MOV, 0x11 }, { "movdqa", 0x66, 0x6f, S_MOV, 0x7f }, { "movdqu", 0xf3, 0x6f, S_MOV, 0x7f },
    { "movss", 0xf3, 0x10, S_MOV, 0x11 }, { "movsd", 0xf2, 0x10, S_MOV, 0x11 }, { "movlps", 0, 0x12, S_MOV, 0x13 },
    { "movhps", 0, 0x16, S_MOV, 0x17 }, { "movlpd", 0x66, 0x12, S_MOV, 0x13 }, { "movhpd", 0x66, 0x16, S_MOV, 0x17 },
    { "movntps", 0, 0x2b, S_MOV, 0x2b }, { "movntdq", 0x66, 0xe7, S_MOV, 0xe7 }, { "movntpd", 0x66, 0x2b, S_MOV, 0x2b },
    { "movd", 0x66, 0x6e, S_MOVD, 0x7e }, { "movq", 0x66, 0x6e, S_MOVQ, 0x7e },
    { "movlhps", 0, 0x16, S_RR, 0 }, { "movhlps", 0, 0x12, S_RR, 0 },
    { "addps", 0, 0x58, S_RM, 0 }, { "addpd", 0x66, 0x58, S_RM, 0 }, { "addss", 0xf3, 0x58, S_RM, 0 }, { "addsd", 0xf2, 0x58, S_RM, 0 },
    { "mulps", 0, 0x59, S_RM, 0 }, { "mulpd", 0x66, 0x59, S_RM, 0 }, { "mulss", 0xf3, 0x59, S_RM, 0 }, { "mulsd", 0xf2, 0x59, S_RM, 0 },
    { "subps", 0, 0x5c, S_RM, 0 }, { "subpd", 0x66, 0x5c, S_RM, 0 }, { "subss", 0xf3, 0x5c, S_RM, 0 }, { "subsd", 0xf2, 0x5c, S_RM, 0 },
    { "minps", 0, 0x5d, S_RM, 0 }, { "minpd", 0x66, 0x5d, S_RM, 0 }, { "minss", 0xf3, 0x5d, S_RM, 0 }, { "minsd", 0xf2, 0x5d, S_RM, 0 },
    { "divps", 0, 0x5e, S_RM, 0 }, { "divpd", 0x66, 0x5e, S_RM, 0 }, { "divss", 0xf3, 0x5e, S_RM, 0 }, { "divsd", 0xf2, 0x5e, S_RM, 0 },
    { "maxps", 0, 0x5f, S_RM, 0 }, { "maxpd", 0x66, 0x5f, S_RM, 0 }, { "maxss", 0xf3, 0x5f, S_RM, 0 }, { "maxsd", 0xf2, 0x5f, S_RM, 0 },
    { "sqrtps", 0, 0x51, S_RM, 0 }, { "sqrtpd", 0x66, 0x51, S_RM, 0 }, { "sqrtss", 0xf3, 0x51, S_RM, 0 }, { "sqrtsd", 0xf2, 0x51, S_RM, 0 },
    { "rsqrtps", 0, 0x52, S_RM, 0 }, { "rsqrtss", 0xf3, 0x52, S_RM, 0 }, { "rcpps", 0, 0x53, S_RM, 0 }, { "rcpss", 0xf3, 0x53, S_RM, 0 },
    { "andps", 0, 0x54, S_RM, 0 }, { "andpd", 0x66, 0x54, S_RM, 0 }, { "andnps", 0, 0x55, S_RM, 0 }, { "andnpd", 0x66, 0x55, S_RM, 0 },
    { "orps", 0, 0x56, S_RM, 0 }, { "orpd", 0x66, 0x56, S_RM, 0 }, { "xorps", 0, 0x57, S_RM, 0 }, { "xorpd", 0x66, 0x57, S_RM, 0 },
    { "ucomiss", 0, 0x2e, S_RM, 0 }, { "ucomisd", 0x66, 0x2e, S_RM, 0 }, { "comiss", 0, 0x2f, S_RM, 0 }, { "comisd", 0x66, 0x2f, S_RM, 0 },
    { "cvtss2sd", 0xf3, 0x5a, S_RM, 0 }, { "cvtsd2ss", 0xf2, 0x5a, S_RM, 0 }, { "cvtps2pd", 0, 0x5a, S_RM, 0 },
    { "cvtpd2ps", 0x66, 0x5a, S_RM, 0 }, { "cvtdq2ps", 0, 0x5b, S_RM, 0 }, { "cvtps2dq", 0x66, 0x5b, S_RM, 0 },
    { "cvttps2dq", 0xf3, 0x5b, S_RM, 0 }, { "cvtdq2pd", 0xf3, 0xe6, S_RM, 0 }, { "cvttpd2dq", 0x66, 0xe6, S_RM, 0 },
    { "cvtpd2dq", 0xf2, 0xe6, S_RM, 0 },
    { "cvttsd2si", 0xf2, 0x2c, S_GD, 0 }, { "cvtsd2si", 0xf2, 0x2d, S_GD, 0 }, { "cvttss2si", 0xf3, 0x2c, S_GD, 0 },
    { "cvtss2si", 0xf3, 0x2d, S_GD, 0 }, { "movmskps", 0, 0x50, S_GD, 0 }, { "movmskpd", 0x66, 0x50, S_GD, 0 },
    { "pmovmskb", 0x66, 0xd7, S_GD, 0 },
    { "cvtsi2sd", 0xf2, 0x2a, S_GS, 0 }, { "cvtsi2ss", 0xf3, 0x2a, S_GS, 0 },
    { "unpcklps", 0, 0x14, S_RM, 0 }, { "unpckhps", 0, 0x15, S_RM, 0 }, { "unpcklpd", 0x66, 0x14, S_RM, 0 }, { "unpckhpd", 0x66, 0x15, S_RM, 0 },
    { "shufps", 0, 0xc6, S_RMI, 0 }, { "shufpd", 0x66, 0xc6, S_RMI, 0 }, { "cmpps", 0, 0xc2, S_RMI, 0 }, { "cmppd", 0x66, 0xc2, S_RMI, 0 },
    { "cmpss", 0xf3, 0xc2, S_RMI, 0 }, { "cmpsd", 0xf2, 0xc2, S_RMI, 0 }, { "pshufd", 0x66, 0x70, S_RMI, 0 },
    { "pshuflw", 0xf2, 0x70, S_RMI, 0 }, { "pshufhw", 0xf3, 0x70, S_RMI, 0 },
    { "punpcklbw", 0x66, 0x60, S_RM, 0 }, { "punpcklwd", 0x66, 0x61, S_RM, 0 }, { "punpckldq", 0x66, 0x62, S_RM, 0 },
    { "packsswb", 0x66, 0x63, S_RM, 0 }, { "pcmpgtb", 0x66, 0x64, S_RM, 0 }, { "pcmpgtw", 0x66, 0x65, S_RM, 0 },
    { "pcmpgtd", 0x66, 0x66, S_RM, 0 }, { "packuswb", 0x66, 0x67, S_RM, 0 }, { "punpckhbw", 0x66, 0x68, S_RM, 0 },
    { "punpckhwd", 0x66, 0x69, S_RM, 0 }, { "punpckhdq", 0x66, 0x6a, S_RM, 0 }, { "packssdw", 0x66, 0x6b, S_RM, 0 },
    { "punpcklqdq", 0x66, 0x6c, S_RM, 0 }, { "punpckhqdq", 0x66, 0x6d, S_RM, 0 }, { "pcmpeqb", 0x66, 0x74, S_RM, 0 },
    { "pcmpeqw", 0x66, 0x75, S_RM, 0 }, { "pcmpeqd", 0x66, 0x76, S_RM, 0 }, { "paddq", 0x66, 0xd4, S_RM, 0 },
    { "pmullw", 0x66, 0xd5, S_RM, 0 }, { "psubusb", 0x66, 0xd8, S_RM, 0 }, { "psubusw", 0x66, 0xd9, S_RM, 0 },
    { "pminub", 0x66, 0xda, S_RM, 0 }, { "pand", 0x66, 0xdb, S_RM, 0 }, { "paddusb", 0x66, 0xdc, S_RM, 0 },
    { "paddusw", 0x66, 0xdd, S_RM, 0 }, { "pmaxub", 0x66, 0xde, S_RM, 0 }, { "pandn", 0x66, 0xdf, S_RM, 0 },
    { "pavgb", 0x66, 0xe0, S_RM, 0 }, { "pavgw", 0x66, 0xe3, S_RM, 0 }, { "pmulhuw", 0x66, 0xe4, S_RM, 0 },
    { "pmulhw", 0x66, 0xe5, S_RM, 0 }, { "psubsb", 0x66, 0xe8, S_RM, 0 }, { "psubsw", 0x66, 0xe9, S_RM, 0 },
    { "pminsw", 0x66, 0xea, S_RM, 0 }, { "por", 0x66, 0xeb, S_RM, 0 }, { "paddsb", 0x66, 0xec, S_RM, 0 },
    { "paddsw", 0x66, 0xed, S_RM, 0 }, { "pmaxsw", 0x66, 0xee, S_RM, 0 }, { "pxor", 0x66, 0xef, S_RM, 0 },
    { "pmuludq", 0x66, 0xf4, S_RM, 0 }, { "pmaddwd", 0x66, 0xf5, S_RM, 0 }, { "psadbw", 0x66, 0xf6, S_RM, 0 },
    { "psubb", 0x66, 0xf8, S_RM, 0 }, { "psubw", 0x66, 0xf9, S_RM, 0 }, { "psubd", 0x66, 0xfa, S_RM, 0 },
    { "psubq", 0x66, 0xfb, S_RM, 0 }, { "paddb", 0x66, 0xfc, S_RM, 0 }, { "paddw", 0x66, 0xfd, S_RM, 0 },
    { "paddd", 0x66, 0xfe, S_RM, 0 },
    { "psrlw", 0x66, 0xd1, S_SH, 0x12 }, { "psrld", 0x66, 0xd2, S_SH, 0x22 }, { "psrlq", 0x66, 0xd3, S_SH, 0x32 },
    { "psraw", 0x66, 0xe1, S_SH, 0x14 }, { "psrad", 0x66, 0xe2, S_SH, 0x24 }, { "psllw", 0x66, 0xf1, S_SH, 0x16 },
    { "pslld", 0x66, 0xf2, S_SH, 0x26 }, { "psllq", 0x66, 0xf3, S_SH, 0x36 }, { "psrldq", 0x66, 0, S_SH, 0x33 },
    { "pslldq", 0x66, 0, S_SH, 0x37 },
    { "ldmxcsr", 0, 0xae, S_EXT, 2 }, { "stmxcsr", 0, 0xae, S_EXT, 3 }, { "fxsave", 0, 0xae, S_EXT, 0 },
    { "fxrstor", 0, 0xae, S_EXT, 1 }, { "clflush", 0, 0xae, S_EXT, 7 }, { "prefetchnta", 0, 0x18, S_EXT, 0 },
    { "prefetcht0", 0, 0x18, S_EXT, 1 }, { "prefetcht1", 0, 0x18, S_EXT, 2 }, { "prefetcht2", 0, 0x18, S_EXT, 3 },
    { 0, 0, 0, 0, 0 }
};

static int x_sse(const char *m)
{
    const XSse *e;
    char base[32];
    size_t n = strlen(m);
    int k, anyx = 0, w = 0;
    XOp *a = &xo[0], *b = &xo[1], *c = &xo[2];
    for (k = 0; k < xnops; k++)
        if (xo[k].kind == XO_XMM)
            anyx = 1;
    strcpy(base, m);
    if (n == 9 && memcmp(m, "cvtsi2s", 7) == 0 && (m[8] == 'l' || m[8] == 'q')) {
        base[8] = 0;
        w = m[8] == 'q';
    }
    for (e = x_sse_tab; e->name; e++)
        if (strcmp(e->name, base) == 0)
            break;
    if (!e->name)
        return 0;
    if (!anyx && (e->kind == S_MOV || e->kind == S_MOVD || e->kind == S_MOVQ || strcmp(m, "cmpsd") == 0))
        return 0;
    switch (e->kind) {
    case S_RM:
        x_want(xnops == 2 && b->kind == XO_XMM && (a->kind == XO_XMM || x_ismem(a)));
        x_enc(e->pfx, 0, 0x0f00 | e->opc, b->reg, a);
        break;
    case S_RR:
        x_want(xnops == 2 && b->kind == XO_XMM && a->kind == XO_XMM);
        x_enc(e->pfx, 0, 0x0f00 | e->opc, b->reg, a);
        break;
    case S_RMI:
        x_want(xnops == 3 && a->kind == XO_IMM && c->kind == XO_XMM && (b->kind == XO_XMM || x_ismem(b)));
        x_enc(e->pfx, 0, 0x0f00 | e->opc, c->reg, b);
        xval(a->v, 1, 0);
        break;
    case S_MOV:
        x_want(xnops == 2);
        if (b->kind == XO_XMM && e->opc != e->x) {
            x_want(a->kind == XO_XMM || x_ismem(a));
            x_enc(e->pfx, 0, 0x0f00 | e->opc, b->reg, a);
        } else {
            x_want(a->kind == XO_XMM && x_ismem(b));
            x_enc(e->pfx, 0, 0x0f00 | e->x, a->reg, b);
        }
        break;
    case S_MOVD:
    case S_MOVQ:
        x_want(xnops == 2);
        w = e->kind == S_MOVQ;
        if (w && b->kind == XO_XMM && (a->kind == XO_XMM || x_ismem(a)))
            x_enc(0xf3, 0, 0x0f7e, b->reg, a);
        else if (w && a->kind == XO_XMM && x_ismem(b))
            x_enc(0x66, 0, 0x0fd6, a->reg, b);
        else if (b->kind == XO_XMM)
            x_enc(0x66, w ? 8 : 0, 0x0f6e, b->reg, a);
        else
            x_enc(0x66, w ? 8 : 0, 0x0f7e, a->reg, b);
        break;
    case S_GD:
        x_want(xnops == 2 && b->kind == XO_REG && (a->kind == XO_XMM || x_ismem(a)));
        x_enc(e->pfx, b->size == 8 ? 8 : 0, 0x0f00 | e->opc, b->reg, a);
        break;
    case S_GS:
        x_want(xnops == 2 && b->kind == XO_XMM && x_isrm(a));
        if (a->kind == XO_REG)
            w = a->size == 8;
        x_enc(e->pfx, w ? 8 : 0, 0x0f00 | e->opc, b->reg, a);
        break;
    case S_SH:
        x_want(xnops == 2 && b->kind == XO_XMM);
        if (a->kind == XO_IMM) {
            x_enc(e->pfx, 0, 0x0f70 | (e->x >> 4), e->x & 15, b);
            xval(a->v, 1, 0);
        } else {
            x_want(e->opc && (a->kind == XO_XMM || x_ismem(a)));
            x_enc(e->pfx, 0, 0x0f00 | e->opc, b->reg, a);
        }
        break;
    default:
        x_want(xnops == 1 && x_ismem(a));
        x_enc(e->pfx, 0, 0x0f00 | e->opc, e->x, a);
        break;
    }
    return 1;
}

/* ---- integer instructions ---- */

enum { C_ALU, C_MOV, C_MOVABS, C_TEST, C_LEA, C_XCHG, C_UN, C_INCDEC, C_SHIFT, C_IMUL, C_PUSH, C_POP, C_CALL, C_JMP,
       C_MOVZ, C_MOVS, C_BT, C_RM2, C_BSWAP, C_XADD, C_SHXD, C_INT, C_RET, C_NOP, C_CX16, C_SYSM };

typedef struct XInsn {
    const char *name;
    u8 cls, x;
} XInsn;

static const XInsn x_int_tab[] = {
    { "add", C_ALU, 0 }, { "or", C_ALU, 1 }, { "adc", C_ALU, 2 }, { "sbb", C_ALU, 3 }, { "and", C_ALU, 4 },
    { "sub", C_ALU, 5 }, { "xor", C_ALU, 6 }, { "cmp", C_ALU, 7 },
    { "mov", C_MOV, 0 }, { "movabs", C_MOVABS, 0 }, { "test", C_TEST, 0 }, { "lea", C_LEA, 0 }, { "xchg", C_XCHG, 0 },
    { "not", C_UN, 2 }, { "neg", C_UN, 3 }, { "mul", C_UN, 4 }, { "div", C_UN, 6 }, { "idiv", C_UN, 7 },
    { "inc", C_INCDEC, 0 }, { "dec", C_INCDEC, 1 },
    { "rol", C_SHIFT, 0 }, { "ror", C_SHIFT, 1 }, { "rcl", C_SHIFT, 2 }, { "rcr", C_SHIFT, 3 }, { "shl", C_SHIFT, 4 },
    { "sal", C_SHIFT, 4 }, { "shr", C_SHIFT, 5 }, { "sar", C_SHIFT, 7 },
    { "imul", C_IMUL, 0 }, { "push", C_PUSH, 0 }, { "pop", C_POP, 0 }, { "call", C_CALL, 0 }, { "jmp", C_JMP, 0 },
    { "movzbw", C_MOVZ, 0x12 }, { "movzbl", C_MOVZ, 0x14 }, { "movzbq", C_MOVZ, 0x18 }, { "movzwl", C_MOVZ, 0x24 },
    { "movzwq", C_MOVZ, 0x28 }, { "movzb", C_MOVZ, 0x10 }, { "movzw", C_MOVZ, 0x20 },
    { "movsbw", C_MOVS, 0x12 }, { "movsbl", C_MOVS, 0x14 }, { "movsbq", C_MOVS, 0x18 }, { "movswl", C_MOVS, 0x24 },
    { "movswq", C_MOVS, 0x28 }, { "movslq", C_MOVS, 0x48 }, { "movsb", C_MOVS, 0x10 }, { "movsw", C_MOVS, 0x20 },
    { "movsl", C_MOVS, 0x40 },
    { "bt", C_BT, 4 }, { "bts", C_BT, 5 }, { "btr", C_BT, 6 }, { "btc", C_BT, 7 },
    { "bsf", C_RM2, 0xbc }, { "bsr", C_RM2, 0xbd }, { "popcnt", C_RM2, 0xb8 }, { "lzcnt", C_RM2, 0xbd }, { "tzcnt", C_RM2, 0xbc },
    { "bswap", C_BSWAP, 0 }, { "xadd", C_XADD, 0xc0 }, { "cmpxchg", C_XADD, 0xb0 },
    { "shld", C_SHXD, 0xa4 }, { "shrd", C_SHXD, 0xac }, { "int", C_INT, 0 }, { "ret", C_RET, 0 }, { "nop", C_NOP, 0 },
    { "cmpxchg16b", C_CX16, 8 }, { "cmpxchg8b", C_CX16, 0 },
    { "lgdt", C_SYSM, 2 }, { "lidt", C_SYSM, 3 }, { "sgdt", C_SYSM, 0 }, { "sidt", C_SYSM, 1 }, { "invlpg", C_SYSM, 7 },
    { 0, 0, 0 }
};

static int x_direct(const XOp *o)
{
    return o->kind == XO_MEM && !o->star && !o->hasbase && !o->hasindex && !o->rip;
}

/* a jump: short when the target is behind and near, else with a 32-bit displacement */
static void x_jump(const XOp *o, int cc)
{
    Val v = o->v;
    if (v.add && !v.sub && defined(v.add) && go->syms[v.add - 1].sec == cur && !go->syms[v.add - 1].weak) {
        i64 d = (i64)go->syms[v.add - 1].value + v.k - ((i64)here() + 2);
        if (d >= -128 && d <= 127) {
            xbyte(cc < 0 ? 0xeb : (u32)(0x70 | cc));
            xbyte((u32)d & 0xff);
            return;
        }
    }
    if (x_isconst(o))
        err("the target is a number, not a label", 0);
    if (cc < 0) {
        xbyte(0xe9);
    } else {
        xbyte(0x0f);
        xbyte((u32)(0x80 | cc));
    }
    xval(v, 4, FX_XPLT32);
}

static int x_int(const char *m, int sfx)
{
    const XInsn *e;
    XOp *a = &xo[0], *b = &xo[1], *c = &xo[2];
    int size, cc, x;

    if (m[0] == 'j' && (cc = x_cc(m + 1)) >= 0 && !sfx) {
        x_want(xnops == 1 && x_direct(a));
        x_jump(a, cc);
        return 1;
    }
    if (memcmp(m, "set", 3) == 0 && (cc = x_cc(m + 3)) >= 0) {
        x_want(xnops == 1 && x_isrm(a) && (a->kind == XO_MEM || a->size == 1) && (!sfx || sfx == 1));
        x_enc(0, 0, (u32)(0x0f90 | cc), 0, a);
        return 1;
    }
    if (memcmp(m, "cmov", 4) == 0 && (cc = x_cc(m + 4)) >= 0) {
        x_want(xnops == 2 && b->kind == XO_REG && x_isrm(a));
        x_enc(0, b->size, (u32)(0x0f40 | cc), b->reg, a);
        return 1;
    }
    for (e = x_int_tab; e->name; e++)
        if (strcmp(e->name, m) == 0)
            break;
    if (!e->name)
        return 0;
    x = e->x;
    switch (e->cls) {
    case C_ALU:
        x_want(xnops == 2);
        size = x_size(sfx);
        if (a->kind == XO_IMM) {
            x_want(x_isrm(b));
            if (b->kind == XO_REG && b->reg == 0 && (size == 1 || !x_fits8(a))) {
                /* the short forms for al, ax, eax, rax */
                if (size == 2)
                    xbyte(0x66);
                else if (size == 8)
                    xbyte(0x48);
                xbyte((u32)(x * 8 + 4 + (size != 1)));
                x_imm(a, size);
            } else if (size == 1) {
                x_enc(0, 1, 0x80, x, b);
                xval(a->v, 1, 0);
            } else if (x_fits8(a)) {
                x_enc(0, size, 0x83, x, b);
                xval(a->v, 1, 0);
            } else {
                x_enc(0, size, 0x81, x, b);
                x_imm(a, size);
            }
        } else if (a->kind == XO_REG) {
            x_want(x_isrm(b));
            x_enc(0, size, (u32)(x * 8 + (size != 1)), a->reg, b);
        } else {
            x_want(x_ismem(a) && b->kind == XO_REG);
            x_enc(0, size, (u32)(x * 8 + 2 + (size != 1)), b->reg, a);
        }
        break;
    case C_MOV:
        x_want(xnops == 2);
        if (a->kind == XO_SEG || b->kind == XO_SEG) {
            if (b->kind == XO_SEG) {
                x_want(x_isrm(a));
                x_enc(0, 0, 0x8e, b->reg, a);
            } else {
                x_want(x_isrm(b));
                x_enc(0, b->kind == XO_REG && b->size != 4 ? b->size : 0, 0x8c, a->reg, b);
            }
            break;
        }
        size = x_size(sfx);
        if (a->kind == XO_IMM) {
            if (b->kind == XO_REG && size == 8 && x_isconst(a) && (a->v.k < -0x7fffffff - 1 || a->v.k > 0x7fffffff)) {
                x_plusr(8, 0xb8, b->reg);
                xval(a->v, 8, 0);
            } else if (b->kind == XO_REG && size != 8) {
                x_plusr(size, size == 1 ? 0xb0 : 0xb8, b->reg);
                x_imm(a, size);
            } else {
                x_want(x_isrm(b));
                x_enc(0, size, size == 1 ? 0xc6 : 0xc7, 0, b);
                x_imm(a, size);
            }
        } else if (a->kind == XO_REG) {
            x_want(x_isrm(b));
            x_enc(0, size, size == 1 ? 0x88 : 0x89, a->reg, b);
        } else {
            x_want(x_ismem(a) && b->kind == XO_REG);
            x_enc(0, size, size == 1 ? 0x8a : 0x8b, b->reg, a);
        }
        break;
    case C_MOVABS:
        x_want(xnops == 2 && a->kind == XO_IMM && b->kind == XO_REG && b->size == 8);
        x_plusr(8, 0xb8, b->reg);
        xval(a->v, 8, 0);
        break;
    case C_TEST:
        x_want(xnops == 2);
        size = x_size(sfx);
        if (a->kind == XO_IMM && b->kind == XO_REG && b->reg == 0) {
            if (size == 2)
                xbyte(0x66);
            else if (size == 8)
                xbyte(0x48);
            xbyte(size == 1 ? 0xa8 : 0xa9);
            x_imm(a, size);
        } else if (a->kind == XO_IMM) {
            x_want(x_isrm(b));
            x_enc(0, size, size == 1 ? 0xf6 : 0xf7, 0, b);
            x_imm(a, size);
        } else if (a->kind == XO_REG) {
            x_want(x_isrm(b));
            x_enc(0, size, size == 1 ? 0x84 : 0x85, a->reg, b);
        } else {
            x_want(x_ismem(a) && b->kind == XO_REG);
            x_enc(0, size, size == 1 ? 0x84 : 0x85, b->reg, a);
        }
        break;
    case C_LEA:
        x_want(xnops == 2 && x_ismem(a) && b->kind == XO_REG);
        x_enc(0, b->size, 0x8d, b->reg, a);
        break;
    case C_XCHG:
        x_want(xnops == 2);
        size = x_size(sfx);
        if (a->kind == XO_REG) {
            x_want(x_isrm(b));
            x_enc(0, size, size == 1 ? 0x86 : 0x87, a->reg, b);
        } else {
            x_want(x_ismem(a) && b->kind == XO_REG);
            x_enc(0, size, size == 1 ? 0x86 : 0x87, b->reg, a);
        }
        break;
    case C_UN:
        x_want(xnops == 1 && x_isrm(a));
        size = x_size(sfx);
        x_enc(0, size, size == 1 ? 0xf6 : 0xf7, x, a);
        break;
    case C_INCDEC:
        x_want(xnops == 1 && x_isrm(a));
        size = x_size(sfx);
        x_enc(0, size, size == 1 ? 0xfe : 0xff, x, a);
        break;
    case C_SHIFT:
        size = sfx;
        if (!size) {
            x_want(xnops >= 1 && xo[xnops - 1].kind == XO_REG);
            size = xo[xnops - 1].size;
        }
        if (xnops == 1) {
            x_want(x_isrm(a));
            x_enc(0, size, size == 1 ? 0xd0 : 0xd1, x, a);
        } else if (a->kind == XO_IMM) {
            x_want(xnops == 2 && x_isrm(b));
            if (x_isconst(a) && a->v.k == 1) {
                x_enc(0, size, size == 1 ? 0xd0 : 0xd1, x, b);
            } else {
                x_enc(0, size, size == 1 ? 0xc0 : 0xc1, x, b);
                xval(a->v, 1, 0);
            }
        } else {
            x_want(xnops == 2 && a->kind == XO_REG && a->size == 1 && a->reg == 1 && x_isrm(b));
            x_enc(0, size, size == 1 ? 0xd2 : 0xd3, x, b);
        }
        break;
    case C_IMUL:
        if (xnops == 1) {
            x_want(x_isrm(a));
            size = x_size(sfx);
            x_enc(0, size, size == 1 ? 0xf6 : 0xf7, 5, a);
            break;
        }
        if (xnops == 2 && a->kind == XO_IMM) {
            xo[2] = xo[1];
            xnops = 3;
        }
        if (xnops == 2) {
            x_want(b->kind == XO_REG && x_isrm(a));
            x_enc(0, b->size, 0x0faf, b->reg, a);
        } else {
            x_want(xnops == 3 && a->kind == XO_IMM && c->kind == XO_REG && x_isrm(b));
            if (x_fits8(a)) {
                x_enc(0, c->size, 0x6b, c->reg, b);
                xval(a->v, 1, 0);
            } else {
                x_enc(0, c->size, 0x69, c->reg, b);
                x_imm(a, c->size);
            }
        }
        break;
    case C_PUSH:
        x_want(xnops == 1);
        if (a->kind == XO_REG) {
            x_want(a->size == 8 || a->size == 2);
            x_plusr(a->size == 2 ? 2 : 0, 0x50, a->reg);
        } else if (a->kind == XO_IMM) {
            if (x_fits8(a)) {
                xbyte(0x6a);
                xval(a->v, 1, 0);
            } else {
                xbyte(0x68);
                xval(a->v, 4, FX_X32S);
            }
        } else {
            x_want(x_ismem(a));
            x_enc(0, 0, 0xff, 6, a);
        }
        break;
    case C_POP:
        x_want(xnops == 1);
        if (a->kind == XO_REG) {
            x_want(a->size == 8 || a->size == 2);
            x_plusr(a->size == 2 ? 2 : 0, 0x58, a->reg);
        } else {
            x_want(x_ismem(a));
            x_enc(0, 0, 0x8f, 0, a);
        }
        break;
    case C_CALL:
    case C_JMP:
        x_want(xnops == 1);
        if (x_direct(a)) {
            if (e->cls == C_JMP) {
                x_jump(a, -1);
            } else {
                xbyte(0xe8);
                xval(a->v, 4, FX_XPLT32);
            }
        } else {
            x_want(a->star && x_isrm(a) && (a->kind == XO_MEM || a->size == 8));
            x_enc(0, 0, 0xff, e->cls == C_CALL ? 2 : 4, a);
        }
        break;
    case C_MOVZ:
    case C_MOVS:
        x_want(xnops == 2 && b->kind == XO_REG && x_isrm(a));
        size = x & 15 ? x & 15 : b->size;
        x >>= 4;
        if (a->kind == XO_REG)
            x_want(a->size == x);
        x_want(size > x);
        if (x == 4)
            x_enc(0, 8, 0x63, b->reg, a);
        else
            x_enc(0, size, (u32)((e->cls == C_MOVZ ? 0x0fb6 : 0x0fbe) + (x == 2)), b->reg, a);
        break;
    case C_BT:
        x_want(xnops == 2 && x_isrm(b));
        size = x_size(sfx);
        if (a->kind == XO_IMM) {
            x_enc(0, size, 0x0fba, x, b);
            xval(a->v, 1, 0);
        } else {
            x_want(a->kind == XO_REG);
            x_enc(0, size, (u32)(0x0fa3 + 8 * (x - 4)), a->reg, b);
        }
        break;
    case C_RM2:
        x_want(xnops == 2 && b->kind == XO_REG && x_isrm(a));
        x_enc(m[0] == 'b' ? 0 : 0xf3, b->size, (u32)(0x0f00 | x), b->reg, a);
        break;
    case C_BSWAP:
        x_want(xnops == 1 && a->kind == XO_REG && a->size >= 4);
        x_plusr(a->size, 0x0fc8, a->reg);
        break;
    case C_XADD:
        x_want(xnops == 2 && a->kind == XO_REG && x_isrm(b));
        size = x_size(sfx);
        x_enc(0, size, (u32)(0x0f00 | x) + (size != 1), a->reg, b);
        break;
    case C_SHXD:
        x_want(xnops == 3 && b->kind == XO_REG && x_isrm(c));
        if (a->kind == XO_IMM) {
            x_enc(0, b->size, (u32)(0x0f00 | x), b->reg, c);
            xval(a->v, 1, 0);
        } else {
            x_want(a->kind == XO_REG && a->size == 1 && a->reg == 1);
            x_enc(0, b->size, (u32)(0x0f01 | x), b->reg, c);
        }
        break;
    case C_INT:
        x_want(xnops == 1 && a->kind == XO_IMM);
        if (x_isconst(a) && a->v.k == 3) {
            xbyte(0xcc);
            break;
        }
        xbyte(0xcd);
        xval(a->v, 1, 0);
        break;
    case C_RET:
        x_want(xnops == 1 && a->kind == XO_IMM);
        xbyte(0xc2);
        xval(a->v, 2, 0);
        break;
    case C_NOP:
        x_want(xnops == 1 && x_isrm(a));
        x_enc(0, sfx == 8 ? 8 : sfx == 2 ? 2 : 0, 0x0f1f, 0, a);
        break;
    case C_CX16:
        x_want(xnops == 1 && x_ismem(a));
        x_enc(0, x, 0x0fc7, 1, a);
        break;
    default:
        x_want(xnops == 1 && x_ismem(a));
        x_enc(0, 0, 0x0f01, x, a);
        break;
    }
    return 1;
}

static void x64_instruction(char *m)
{
    static const struct { const char *name; u8 byte; } pfx[] = {
        { "lock", 0xf0 }, { "rep", 0xf3 }, { "repe", 0xf3 }, { "repz", 0xf3 }, { "repne", 0xf2 }, { "repnz", 0xf2 },
        { "data16", 0x66 }, { "notrack", 0x3e }
    };
    char base[32];
    size_t n;
    int k;

    for (k = 0; k < (int)(sizeof pfx / sizeof pfx[0]); k++)
        if (strcmp(pfx[k].name, m) == 0) {
            char *w;
            buf_u8(B(), pfx[k].byte);
            skip_ws();
            w = p;
            while (is_ident(*p))
                p++;
            n = (size_t)(p - w);
            if (!n)
                return;
            if (n >= sizeof base)
                err("unknown instruction", w);
            memcpy(base, w, n);
            base[n] = 0;
            lower(base, base, sizeof base);
            x64_instruction(base);
            return;
        }
    split_ops();
    if (nops > 3)
        err("too many operands", 0);
    xn = nxfx = 0;
    x_need_rex = x_no_rex = 0;
    xnops = nops;
    for (k = 0; k < nops; k++)
        x_operand(ops[k], &xo[k]);
    n = strlen(m);
    if (!(xnops == 0 && x_simple(m)) && !x_sse(m) && !x_int(m, 0)) {
        int sfx = n > 1 ? x_suffix_size(m[n - 1]) : 0;
        strcpy(base, m);
        base[n - 1] = 0;
        if (!sfx || !x_int(base, sfx))
            err("unknown instruction", m);
    }
    x_flush();
}
