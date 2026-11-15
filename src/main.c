/* mcc driver. One binary: mcc (compiler), mas (assembler), mld (linker). */
#include "cc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *base_name(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

/* Developer self-test: write an executable that exits with status 42,
   exercising the Mach-O writer without any compiler in front of it.
   With "data" it also checks a rebased pointer and the bss. */
static int test_exe(const char *out_path, int with_data)
{
    static u8 text[64];
    static u8 data[16];
    static const u64 rebases[1] = { 0 };
    ExeImage img;
    ExeLayout lay;
    Buf out;
    size_t n = 0;

    memset(&img, 0, sizeof img);
    out.p = 0;
    out.len = out.cap = 0;
    if (with_data) {
        long delta;
        macho_exe_layout(28, sizeof data, 64, &lay); /* 7 instructions */
        /* data[0] = &data[8] (needs a rebase); data[8] = 42.
           Load through the pointer, add bss[0] (zero), exit. */
        put_u64le(data, lay.data_addr + 8);
        put_u64le(data + 8, 42);
        delta = (long)((lay.data_addr >> 12) - (lay.text_addr >> 12));
        put_u32le(text + n, 0x90000001 | ((u32)(delta & 3) << 29) | ((u32)((delta >> 2) & 0x7ffff) << 5)); /* adrp x1 */
        n += 4;
        put_u32le(text + n, 0xf9400022 | ((u32)((lay.data_addr & 0xfff) >> 3) << 10)); /* ldr x2,[x1,#lo] */
        n += 4;
        put_u32le(text + n, 0xf9400040); /* ldr x0,[x2] */
        n += 4;
        put_u32le(text + n, 0xf9400000 | (1 << 5) | 3 | ((u32)(((lay.bss_addr & 0xfff)) >> 3) << 10)); /* ldr x3,[x1,#bss] */
        n += 4;
        put_u32le(text + n, 0x8b030000); /* add x0,x0,x3 */
        n += 4;
        img.data = data;
        img.data_len = sizeof data;
        img.bss_len = 64;
        img.rebases = rebases;
        img.nrebases = 1;
    } else {
        put_u32le(text + n, 0xd2800540); /* mov x0,#42 */
        n += 4;
    }
    put_u32le(text + n, 0xd2800030); /* mov x16,#1 (exit) */
    n += 4;
    put_u32le(text + n, 0xd4001001); /* svc #0x80 */
    n += 4;
    if (with_data && n != 28)
        die("internal: test text size");
    img.text = text;
    img.text_len = n;
    img.ident = base_name(out_path);
    macho_exe_write(&out, &img);
    os_write_file(out_path, out.p, out.len, 1);
    buf_free(&out);
    return 0;
}

static void usage(void)
{
    fputs("usage: mcc [options] file...\n"
          "       mcc as  [options] file.s   (or invoke as mas)\n"
          "       mcc ld  [options] file.o   (or invoke as mld)\n",
          stderr);
}

/* -march=native: does the processor this runs on have the feature (a word of the flags line of /proc/cpuinfo)? */
int x64_cpu_has(const char *flag)
{
    static char line[8192];
    FILE *fp = fopen("/proc/cpuinfo", "r");
    size_t n = strlen(flag);
    int yes = 0;
    if (!fp)
        return 0;
    while (!yes && fgets(line, sizeof line, fp)) {
        char *p = line;
        if (strncmp(line, "flags", 5) != 0)
            continue;
        while ((p = strstr(p, flag)) != 0) {
            if (p[-1] == ' ' && (p[n] == ' ' || p[n] == '\n' || p[n] == 0)) {
                yes = 1;
                break;
            }
            p += n;
        }
        break;
    }
    fclose(fp);
    return yes;
}

static int has_suffix(const char *s, const char *suffix)
{
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && strcmp(s + n - m, suffix) == 0;
}

/* Where mcc itself lives: the directory of argv[0], found on PATH when it was run by name. When that is the
   bin/ directory of an installation (symbolic links to the programs), it is the directory above. */
static char *save_strn(const char *s, size_t n)
{
    char *p = (char *)xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

static char *self_dir(const char *argv0)
{
    static char *dir;
    const char *slash = strrchr(argv0, '/'), *path, *e;
    char *probe;
    size_t n;
    if (dir)
        return dir;
    if (slash)
        dir = save_strn(argv0, (size_t)(slash - argv0));
    else {
        for (path = getenv("PATH"); path && *path; path = *e ? e + 1 : e) {
            e = strchr(path, ':');
            if (!e)
                e = path + strlen(path);
            probe = (char *)xmalloc((size_t)(e - path) + strlen(argv0) + 2);
            sprintf(probe, "%.*s/%s", (int)(e - path), path, argv0);
            if (e > path && os_exists(probe)) {
                dir = save_strn(path, (size_t)(e - path));
                free(probe);
                break;
            }
            free(probe);
        }
        if (!dir)
            return 0;
    }
    n = strlen(dir);
    probe = (char *)xmalloc(n + 16);
    sprintf(probe, "%s/include", dir);
    if (!os_exists(probe) && n > 4 && strcmp(dir + n - 4, "/bin") == 0)
        dir[n - 4] = 0;
    else if (!os_exists(probe) && strcmp(dir, "bin") == 0)
        strcpy(dir, ".");
    free(probe);
    return dir;
}

/* The directory holding include/ and lib/: $MCC_HOME, else where mcc itself lives. */
static char *home_path(const char *argv0, const char *rest)
{
    const char *home = getenv("MCC_HOME");
    char *path;
    if (!home)
        home = self_dir(argv0);
    if (!home)
        return 0;
    path = (char *)xmalloc(strlen(home) + strlen(rest) + 2);
    sprintf(path, "%s/%s", home, rest);
    return path;
}

static const char **all_inputs;
static char *input_lang; /* from -x: 'c', 's' assembly, 'S' assembly to preprocess, or 0: by the name */
static int nall_inputs;

/* 'c', 's', 'S', or 'o' for what goes to the linker */
static int kind_of(const char *path)
{
    int i;
    for (i = 0; i < nall_inputs; i++)
        if (all_inputs[i] == path && input_lang[i])
            return input_lang[i];
    if (has_suffix(path, ".c") || has_suffix(path, ".i") || has_suffix(path, ".cu"))
        return 'c';
    if (has_suffix(path, ".s"))
        return 's';
    if (has_suffix(path, ".S"))
        return 'S';
    return 'o';
}

static int is_c(const char *path)
{
    return kind_of(path) == 'c';
}

/* Options that ask for code generation mcc does not do. They are refused,
   so that a build system probing for them hears "no". */
static int unsupported(const char *a)
{
    static const char *const pre[] = {"-fstack-protector", "-mbranch-protection", "-msign-return-address",
                                      "-fsanitize", "-fpatchable-function-entry", "-mstack-protector-guard",
                                      "-fplugin", "-fsplit-stack", "-fcf-protection", "-mrecord-mcount",
                                      "-fzero-call-used-regs", "-ftrivial-auto-var-init", "-fprofile-arcs",
                                      "-ftest-coverage", "-mfentry", "-fstack-clash-protection", "-fstack-check",
                                      "-mtp=", "-mcmodel=large", "-fpic", "-fPIC", "-fpie", "-fPIE", "-mbig-endian",
                                      "-mabi=ilp32", "-fharden", "-fstrict-flex-arrays", "-ffunction-sections",
                                      "-fdata-sections", "-gdwarf", "-gsplit-dwarf", "-gz", "-fmin-function-alignment",
                                      "-falign-functions=", "-mharden", "-mindirect-branch", "-mfunction-return",
                                      "-fcoverage", 0};
    int k;
    if (strcmp(a, "-pg") == 0 || strcmp(a, "-shared") == 0)
        return 1;
    for (k = 0; pre[k]; k++)
        if (strncmp(a, pre[k], strlen(pre[k])) == 0)
            return 1;
    return 0;
}

static int hosted;  /* --hosted: the POSIX flavour of the library on this machine */
static int std_c99; /* -std names C99 or later: restrict is a keyword */
static int force_cuda; /* -x cuda */
const char *opt_ptx_out;
char opt_cuda_target[24] = "sm_86"; /* CUDA: the .target of the PTX: the GPU of --cuda-arch */
static int cuda_arch = 860; /* CUDA: the value of __CUDA_ARCH__; --cuda-arch=N or nvcc's -arch=sm_NN */
int opt_fmad = 1;
int opt_fastmath;

/* --sysroot=DIR (or -mglibc: the root directory): x86-64 Linux programs for the GNU C library found there,
   dynamically linked; 0: mcc's own C library */
static char **main_envp;
static const char *sysroot;

/* -I DIR where DIR is one of the system's own directories of headers changes nothing (as in gcc): it stays
   where it is in the search order, behind the compiler's headers */
static int is_system_dir(const char *d)
{
    static const char *const dirs[] = {"/usr/local/include", "/usr/include/x86_64-linux-gnu", "/usr/include", 0};
    size_t n = strlen(d), r;
    int k;
    if (!sysroot)
        return 0;
    while (n > 1 && d[n - 1] == '/')
        n--;
    r = strlen(sysroot);
    if (n < r || strncmp(d, sysroot, r))
        return 0;
    for (k = 0; dirs[k]; k++)
        if (strlen(dirs[k]) == n - r && strncmp(d + r, dirs[k], n - r) == 0)
            return 1;
    return 0;
}
static const char **sys_objs; /* what goes to the system's linker */
static int nsys_objs, nsys_temps;

static void sys_obj(const char *path)
{
    sys_objs = (const char **)xrealloc((void *)sys_objs, (size_t)(nsys_objs + 1) * sizeof(char *));
    sys_objs[nsys_objs++] = path;
}

/* an object made here on the way to an executable: the system's linker reads it from a file */
static void sys_temp(const char *out_path, Buf *obj)
{
    char *path = (char *)xmalloc(strlen(out_path) + 32);
    sprintf(path, "%s.mcc%d.o", out_path, nsys_temps++);
    os_write_file(path, obj->p, obj->len, 0);
    sys_obj(path);
}

/* x86-64 instruction set extensions the code may use (-mavx2, -march=...): what <immintrin.h> users test */
enum { XF_SSE3 = 1, XF_SSSE3 = 2, XF_SSE41 = 4, XF_SSE42 = 8, XF_POPCNT = 16, XF_AVX = 32, XF_AVX2 = 64, XF_FMA = 128,
       XF_F16C = 256, XF_BMI = 512, XF_BMI2 = 1024 };
static u32 x86_feat;
#define XF_V2 (XF_SSE3 | XF_SSSE3 | XF_SSE41 | XF_SSE42 | XF_POPCNT)
#define XF_V3 (XF_V2 | XF_AVX | XF_AVX2 | XF_FMA | XF_F16C | XF_BMI | XF_BMI2)

/* those of the processor mcc runs on (-march=native) */
static u32 x86_native(void)
{
#if defined(__x86_64__)
    unsigned a, b, c, d, r = 0;
    int os_ymm = 0;
    __asm__ __volatile__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "0"(1), "2"(0));
    if (c & 1)
        r |= XF_SSE3;
    if (c >> 9 & 1)
        r |= XF_SSSE3;
    if (c >> 19 & 1)
        r |= XF_SSE41;
    if (c >> 20 & 1)
        r |= XF_SSE42;
    if (c >> 23 & 1)
        r |= XF_POPCNT;
    if (c >> 28 & 1)
        r |= XF_AVX;
    if (c >> 12 & 1)
        r |= XF_FMA;
    if (c >> 29 & 1)
        r |= XF_F16C;
    if (c >> 27 & 1) { /* OSXSAVE: XCR0 says which register states the system saves */
        unsigned lo, hi;
        __asm__ __volatile__(".byte 0x0f, 0x01, 0xd0" : "=a"(lo), "=d"(hi) : "c"(0)); /* xgetbv */
        os_ymm = (lo & 6) == 6; /* xmm and ymm state */
        (void)hi;
    }
    __asm__ __volatile__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "0"(7), "2"(0));
    if (b >> 3 & 1)
        r |= XF_BMI;
    if (b >> 5 & 1)
        r |= XF_AVX2;
    if (b >> 8 & 1)
        r |= XF_BMI2;
    if (!os_ymm) /* the processor has AVX but the system does not save ymm registers (some VMs): none of it */
        r &= ~(u32)(XF_AVX | XF_AVX2 | XF_FMA | XF_F16C);
    (void)a;
    (void)d;
    return r;
#else
    return 0;
#endif
}

/* -m and -march options that name extensions; 0 if a is not one */
static int x86_option(const char *a)
{
    static const struct { const char *name; u32 bits; } ext[] = {
        {"sse3", XF_SSE3}, {"ssse3", XF_SSSE3 | XF_SSE3}, {"sse4.1", XF_SSE41 | XF_SSSE3 | XF_SSE3},
        {"sse4.2", XF_SSE42 | XF_SSE41 | XF_SSSE3 | XF_SSE3}, {"sse4", XF_SSE42 | XF_SSE41 | XF_SSSE3 | XF_SSE3},
        {"popcnt", XF_POPCNT}, {"avx", XF_AVX | XF_SSE42 | XF_SSE41 | XF_SSSE3 | XF_SSE3},
        {"avx2", XF_AVX2 | XF_AVX | XF_SSE42 | XF_SSE41 | XF_SSSE3 | XF_SSE3}, {"fma", XF_FMA | XF_AVX},
        {"f16c", XF_F16C | XF_AVX}, {"bmi", XF_BMI}, {"bmi2", XF_BMI2}};
    static const char *const v3[] = {"x86-64-v3", "x86-64-v4", "haswell", "broadwell", "skylake", "skylake-avx512",
        "cascadelake", "icelake-client", "icelake-server", "tigerlake", "alderlake", "raptorlake", "meteorlake",
        "sapphirerapids", "graniterapids", "znver1", "znver2", "znver3", "znver4", "znver5", 0};
    u32 k;
    if (strncmp(a, "-march=", 7) == 0 || strncmp(a, "-mtune=", 7) == 0) {
        if (a[2] == 't')
            return 1;
        if (strcmp(a + 7, "native") == 0)
            x86_feat = x86_native();
        else if (strcmp(a + 7, "x86-64-v2") == 0 || strcmp(a + 7, "nehalem") == 0 || strcmp(a + 7, "westmere") == 0)
            x86_feat = XF_V2;
        else if (strcmp(a + 7, "x86-64") == 0)
            x86_feat = 0;
        else
            for (k = 0; v3[k]; k++)
                if (strcmp(a + 7, v3[k]) == 0)
                    x86_feat = XF_V3;
        return 1;
    }
    if (strncmp(a, "-m", 2) != 0)
        return 0;
    for (k = 0; k < sizeof ext / sizeof ext[0]; k++) {
        if (strcmp(a + 2, ext[k].name) == 0) {
            x86_feat |= ext[k].bits;
            return 1;
        }
        if (strncmp(a + 2, "no-", 3) == 0 && strcmp(a + 5, ext[k].name) == 0) {
            x86_feat &= ~ext[k].bits;
            return 1;
        }
    }
    return 0;
}

static void target_macros(void)
{
    static const char *const linux_defs[] = {
        "__GNUC__=5", "__GNUC_MINOR__=1", "__GNUC_PATCHLEVEL__=0", "__VERSION__=\"5.1.0 (mcc)\"",
        "__aarch64__", "__AARCH64EL__", "__ARM_64BIT_STATE", "__ARM_ARCH=8", "__ARM_ARCH_ISA_A64",
        "__linux__", "__linux", "__gnu_linux__", "__unix__", "__unix", "__ELF__", "_LP64",
        "__CHAR_UNSIGNED__", "__CHAR_BIT__=8", "__SIZEOF_SHORT__=2", "__SIZEOF_INT__=4", "__SIZEOF_LONG__=8",
        "__SIZEOF_LONG_LONG__=8", "__SIZEOF_POINTER__=8", "__SIZEOF_SIZE_T__=8", "__SIZEOF_PTRDIFF_T__=8",
        "__SIZEOF_FLOAT__=4", "__SIZEOF_DOUBLE__=8", "__SIZEOF_LONG_DOUBLE__=16", "__SIZEOF_WCHAR_T__=4",
        "__SIZEOF_WINT_T__=4", "__SIZEOF_INT128__=16", "__BIGGEST_ALIGNMENT__=16",
        "__ORDER_LITTLE_ENDIAN__=1234", "__ORDER_BIG_ENDIAN__=4321", "__ORDER_PDP_ENDIAN__=3412",
        "__BYTE_ORDER__=__ORDER_LITTLE_ENDIAN__", "__SIZE_TYPE__=long unsigned int", "__PTRDIFF_TYPE__=long int",
        "__INTPTR_TYPE__=long int", "__UINTPTR_TYPE__=long unsigned int", "__WCHAR_TYPE__=unsigned int",
        "__WINT_TYPE__=unsigned int", "__INTMAX_TYPE__=long int", "__UINTMAX_TYPE__=long unsigned int",
        "__INT8_TYPE__=signed char", "__INT16_TYPE__=short int", "__INT32_TYPE__=int", "__INT64_TYPE__=long int",
        "__UINT8_TYPE__=unsigned char", "__UINT16_TYPE__=short unsigned int", "__UINT32_TYPE__=unsigned int",
        "__UINT64_TYPE__=long unsigned int", "__CHAR16_TYPE__=short unsigned int", "__CHAR32_TYPE__=unsigned int",
        "__SCHAR_MAX__=0x7f", "__SHRT_MAX__=0x7fff", "__INT_MAX__=0x7fffffff", "__LONG_MAX__=0x7fffffffffffffffL",
        "__LONG_LONG_MAX__=0x7fffffffffffffffLL", "__SIZE_MAX__=0xffffffffffffffffUL",
        "__PTRDIFF_MAX__=0x7fffffffffffffffL", "__INTMAX_MAX__=0x7fffffffffffffffL",
        "__UINTMAX_MAX__=0xffffffffffffffffUL", "__INTPTR_MAX__=0x7fffffffffffffffL",
        "__UINTPTR_MAX__=0xffffffffffffffffUL", "__WCHAR_MAX__=0xffffffffU", "__WCHAR_MIN__=0U",
        "__USER_LABEL_PREFIX__=", "__REGISTER_PREFIX__=", "__GNUC_GNU_INLINE__", "__OPTIMIZE__",
        "__NO_MATH_ERRNO__", "__STDC_HOSTED__=1", "__STDC_NO_THREADS__=1",
        "__ATOMIC_RELAXED=0", "__ATOMIC_CONSUME=1", "__ATOMIC_ACQUIRE=2", "__ATOMIC_RELEASE=3", "__ATOMIC_ACQ_REL=4",
        "__ATOMIC_SEQ_CST=5", "__GCC_ATOMIC_BOOL_LOCK_FREE=2", "__GCC_ATOMIC_CHAR_LOCK_FREE=2",
        "__GCC_ATOMIC_SHORT_LOCK_FREE=2", "__GCC_ATOMIC_INT_LOCK_FREE=2", "__GCC_ATOMIC_LONG_LOCK_FREE=2",
        "__GCC_ATOMIC_LLONG_LOCK_FREE=2", "__GCC_ATOMIC_POINTER_LOCK_FREE=2",
        "__GCC_HAVE_SYNC_COMPARE_AND_SWAP_1", "__GCC_HAVE_SYNC_COMPARE_AND_SWAP_2",
        "__GCC_HAVE_SYNC_COMPARE_AND_SWAP_4", "__GCC_HAVE_SYNC_COMPARE_AND_SWAP_8", 0};
    int k;
    if (std_c99)
        pp_define("restrict=__restrict");
    if (opt_c23)
        pp_define("__STDC_VERSION__=202311L");
    else if (std_c99)
        pp_define("__STDC_VERSION__=201710L");
    if (hosted) {
        pp_define("__unix__");
        pp_define("__STDC_HOSTED__=1");
        pp_define("environ=__environ"); /* the library's name for it */
        pp_define("_LP64");
        pp_define("__LP64__");
        pp_define("__aarch64__");
    }
    if (opt_target != TARGET_LINUX)
        return;
    for (k = 0; linux_defs[k]; k++)
        pp_define(linux_defs[k]);
    if (opt_arch == ARCH_X64) {
        static const char *const no[] = {"__aarch64__", "__AARCH64EL__", "__ARM_64BIT_STATE", "__ARM_ARCH",
            "__ARM_ARCH_ISA_A64", "__CHAR_UNSIGNED__", "__WCHAR_TYPE__", "__WCHAR_MAX__", "__WCHAR_MIN__", 0};
        static const char *const yes[] = {"__x86_64__", "__x86_64", "__amd64__", "__amd64",
            "__SSE_MATH__", "__SSE2_MATH__", "__WCHAR_TYPE__=int", "__WCHAR_MAX__=0x7fffffff",
            "__WCHAR_MIN__=(-__WCHAR_MAX__-1)", 0};
        static const struct { u32 bit; const char *name; } fm[] = {
            {XF_SSE3, "__SSE3__"}, {XF_SSSE3, "__SSSE3__"}, {XF_SSE41, "__SSE4_1__"}, {XF_SSE42, "__SSE4_2__"},
            {XF_POPCNT, "__POPCNT__"}, {XF_AVX, "__AVX__"}, {XF_AVX2, "__AVX2__"}, {XF_FMA, "__FMA__"},
            {XF_F16C, "__F16C__"}, {XF_BMI, "__BMI__"}, {XF_BMI2, "__BMI2__"}};
        for (k = 0; no[k]; k++)
            pp_undef(no[k]);
        for (k = 0; yes[k]; k++)
            pp_define(yes[k]);
        /* <immintrin.h> is mcc's own: SSE and SSE2 always, the rest as the options say */
        pp_define("__MMX__");
        pp_define("__SSE__");
        pp_define("__SSE2__");
        for (k = 0; k < (int)(sizeof fm / sizeof fm[0]); k++)
            if (x86_feat & fm[k].bit)
                pp_define(fm[k].name);
        opt_avx = (x86_feat & XF_AVX) != 0;
        opt_f16c = (x86_feat & XF_F16C) != 0;
        opt_x64_sse41 = (x86_feat & XF_SSE41) != 0; /* the vectoriser may use it */
        opt_x64_avx2 = (x86_feat & XF_AVX2) != 0;   /* 32-byte vectors in AVX functions */
    }
    if (opt_size)
        pp_define("__OPTIMIZE_SIZE__");
    if (sysroot) {
        static const char *const g[] = {
            /* the library's <math.h> then classifies numbers with its functions, not compiler builtins */
            "__SUPPORT_SNAN__",
            "__builtin_isgreater(x,y)=((x)>(y))", "__builtin_isgreaterequal(x,y)=((x)>=(y))",
            "__builtin_isless(x,y)=((x)<(y))", "__builtin_islessequal(x,y)=((x)<=(y))",
            "__builtin_islessgreater(x,y)=((x)<(y)||(x)>(y))", "__builtin_isunordered(x,y)=((x)!=(x)||(y)!=(y))",
            "__builtin_huge_vall()=((long double)__builtin_huge_val())", "__builtin_infl()=((long double)__builtin_inf())",
            "__builtin_nanl(s)=((long double)__builtin_nan(s))", "__builtin_nans(s)=__builtin_nan(s)",
            "__builtin_nansf(s)=__builtin_nanf(s)", "__builtin_nansl(s)=((long double)__builtin_nan(s))",
            "__builtin_signbit(x)=__signbit(x)", "__builtin_signbitf(x)=__signbitf(x)",
            "__builtin_signbitl(x)=__signbit(x)", 0};
        pp_undef("__STDC_NO_THREADS__");
        pp_define("__mcc_glibc__");
        for (k = 0; g[k]; k++)
            pp_define(g[k]);
    }
}

static void write_deps(const char *dep_path, const char *target)
{
    Buf b;
    if (!dep_path)
        return;
    memset(&b, 0, sizeof b);
    buf_put(&b, target, strlen(target));
    buf_put(&b, ":", 1);
    src_deps(&b);
    buf_u8(&b, '\n');
    os_write_file(dep_path, b.p, b.len, 0);
    buf_free(&b);
}

static int cc_main(const char *argv0, int argc, char **argv)
{
    const char *out_path = 0, *entry = 0, *prof_path = 0, *dep_path = 0;
    const char **undefs = (const char **)xmalloc((size_t)(argc + 1) * sizeof(char *));
    int nostdlib = 0, nostdinc = 0, lang = 0, nundefs = 0, want_deps = 0;
    char *sys_include = home_path(argv0, "include"), *libc = home_path(argv0, "lib/libc.a");
    char *lx_include = home_path(argv0, "linux/include"), *lx_lib = home_path(argv0, "linux/lib");
    char *lx_ext_include = home_path(argv0, "linux/ext/include"), *lx_ext_lib = home_path(argv0, "linux/ext/lib");
    const char **libdirs = (const char **)xmalloc((size_t)(argc + 1) * sizeof(char *));
    const char **libs = (const char **)xmalloc((size_t)(argc + 1) * sizeof(char *));
    int nlibdirs = 0, nlibs = 0, nostartfiles = 0, relocatable = 0;
    const char **inputs = (const char **)xmalloc((size_t)(argc + 1) * sizeof(char *));
    const char **defines = (const char **)xmalloc((size_t)(argc + 1) * sizeof(char *));
    Obj **objs = 0;
    size_t nobjs = 0;
    int ninputs = 0, ndefines = 0, i, k, nsources = 0;
    int preprocess_only = 0, dump_ir = 0, compile_only = 0;
    int jobs = 0, running = 0, next = 0, failed = 0;
    int *job_id, *job_fd, out_fd = -1;

    all_inputs = inputs;
    input_lang = (char *)xmalloc((size_t)argc + 1);
#if defined(__linux__) && defined(__x86_64__)
    /* a compiler that runs on x86-64 Linux compiles for it unless told otherwise */
    opt_arch = ARCH_X64;
    opt_target = TARGET_LINUX;
    lx_include = home_path(argv0, "linux-x64/include");
    lx_lib = home_path(argv0, "linux-x64/lib");
    lx_ext_include = home_path(argv0, "linux-x64/ext/include");
    lx_ext_lib = home_path(argv0, "linux-x64/ext/lib");
#ifdef __GLIBC__
    sysroot = ""; /* and one built against the GNU C library builds against it */
#endif
#endif
    for (i = 0; i < argc; i++) {
        if (strncmp(argv[i], "--sysroot=", 10) == 0)
            sysroot = strcmp(argv[i] + 10, "/") ? argv[i] + 10 : "";
        else if (strcmp(argv[i], "-mglibc") == 0)
            sysroot = "";
        else if (strcmp(argv[i], "-mno-glibc") == 0)
            sysroot = 0;
    }
    for (i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (sysroot && (strcmp(a, "-fPIC") == 0 || strcmp(a, "-fpic") == 0 || strcmp(a, "-fPIE") == 0 ||
                        strcmp(a, "-fpie") == 0))
            continue; /* said for objects that go into an executable: what is made here will do */
        if (unsupported(a))
            die("unsupported option %s", a);
        if (strcmp(a, "-E") == 0)
            preprocess_only = 1;
        else if (strcmp(a, "--version") == 0 || strcmp(a, "-v") == 0) {
            puts("mcc 0.9 (C compiler, assembler and linker; speaks GNU C 5.1.0)");
            return 0;
        } else if (strcmp(a, "-Wa,--version") == 0) {
            puts("GNU assembler (mcc, compatible) 2.30");
            return 0;
        } else if (strcmp(a, "-dumpversion") == 0) {
            puts("5.1.0");
            return 0;
        } else if (strcmp(a, "-dumpmachine") == 0) {
            puts(opt_arch == ARCH_X64 ? "x86_64-linux-gnu" : opt_target == TARGET_LINUX ? "aarch64-linux-gnu" : "arm64-apple-darwin");
            return 0;
        } else if (strncmp(a, "-print-file-name=", 17) == 0) {
            puts(strcmp(a + 17, "include") != 0 ? a + 17 : opt_target == TARGET_LINUX && lx_include ? lx_include :
                 sys_include ? sys_include : a + 17);
            return 0;
        } else if (a[0] == '-' && a[1] == 'x' && (a[2] || i + 1 < argc)) {
            const char *l = a[2] ? a + 2 : argv[++i];
            force_cuda = strcmp(l, "cuda") == 0 || strcmp(l, "cu") == 0;
            lang = strcmp(l, "c") == 0 || force_cuda ? 'c' : strcmp(l, "assembler") == 0 ? 's' :
                   strcmp(l, "assembler-with-cpp") == 0 ? 'S' : 0;
        } else if (strcmp(a, "-P") == 0 || strcmp(a, "-C") == 0 || strcmp(a, "-undef") == 0)
            ; /* the output never has line markers or comments */
        else if (strncmp(a, "-Wp,-MMD,", 9) == 0 || strncmp(a, "-Wp,-MD,", 8) == 0)
            dep_path = strchr(a + 5, ',') + 1;
        else if (strcmp(a, "-MD") == 0 || strcmp(a, "-MMD") == 0 || strcmp(a, "-Wp,-MMD") == 0 ||
                 strcmp(a, "-Wp,-MD") == 0)
            want_deps = 1;
        else if (x86_option(a))
            ; /* instruction set extensions: the macros that say so */
        else if (strcmp(a, "-MP") == 0 || strcmp(a, "-pthread") == 0)
            ; /* phony targets for the headers: make does not need them */
        else if (strcmp(a, "-MF") == 0 && i + 1 < argc)
            dep_path = argv[++i];
        else if ((strcmp(a, "-MT") == 0 || strcmp(a, "-MQ") == 0) && i + 1 < argc)
            i++;
        else if (strncmp(a, "-U", 2) == 0)
            undefs[nundefs++] = a[2] ? a + 2 : argv[++i];
        else if (strncmp(a, "-L", 2) == 0)
            libdirs[nlibdirs++] = a[2] ? a + 2 : argv[++i];
        else if (exe_option(a, i + 1 < argc ? argv[i + 1] : 0) == 2)
            i++;
        else if (strncmp(a, "--dylib=", 8) == 0 || strncmp(a, "--entitlement=", 14) == 0)
            ;
        else if (strncmp(a, "-l", 2) == 0)
            libs[nlibs++] = a[2] ? a + 2 : argv[++i];
        else if (strcmp(a, "-nostartfiles") == 0)
            nostartfiles = 1;
        else if (strcmp(a, "-r") == 0)
            relocatable = 1;
        else if (sysroot && (strncmp(a, "-Wl,", 4) == 0 || strcmp(a, "-rdynamic") == 0))
            sys_obj(a); /* for the system's linker, like the objects */
        else if (strcmp(a, "-static") == 0 || strcmp(a, "-s") == 0 || strncmp(a, "-Wl,", 4) == 0 ||
                 strcmp(a, "-rdynamic") == 0)
            ; /* executables are static; the options for the linker that matter are its defaults */
        else if (strcmp(a, "-c") == 0)
            compile_only = 1;
        else if (strcmp(a, "-S") == 0) {
            /* No listing of the generated code is made: the output holds the
               inline assembly of the file, with its operands filled in. That
               is what build systems that compile to assembly read back. */
            static Buf asm_text;
            static const char note[] = "// mcc -S: inline assembly only\n";
            opt_asm_text = &asm_text;
            buf_put(&asm_text, note, sizeof note - 1);
            compile_only = 1;
        }
        else if (strcmp(a, "--ir") == 0)
            dump_ir = 1;
        else if (strcmp(a, "-fno-strict-aliasing") == 0)
            opt_strict_aliasing = 0;
        else if (strcmp(a, "-fno-jump-tables") == 0)
            opt_no_jump_tables = 1;
        else if (strncmp(a, "-ffp-contract=", 14) == 0)
            opt_fp_contract = strcmp(a + 14, "off") != 0;
        else if (strcmp(a, "-flto") == 0)
            opt_lto = 1;
        else if (strcmp(a, "-fprofile-generate") == 0)
            opt_prof_gen = 1;
        else if (strcmp(a, "-fprofile-use") == 0)
            prof_path = "mcc.prof";
        else if (strncmp(a, "-fprofile-use=", 14) == 0)
            prof_path = a + 14;
        else if (strncmp(a, "-finline-hot=", 13) == 0)
            opt_inline_hot = atoi(a + 13);
        else if (strncmp(a, "-fhot-shift=", 12) == 0)
            opt_hot_shift = atoi(a + 12);
        else if (strcmp(a, "-O3") == 0)
            opt_level = 3;
        else if (strcmp(a, "-fvec-report") == 0)
            opt_vec_report = 1;
        else if (strcmp(a, "-fvec-gather") == 0)
            opt_vec_gather = 1;
        else if (strcmp(a, "-fno-vec-gather") == 0)
            opt_vec_gather = -1;
        else if (strcmp(a, "-fvec-report=2") == 0)
            opt_vec_report = 2; /* with the instruction that stopped it */
        else if (strcmp(a, "-ffast-math") == 0)
            opt_fast_math = 1, opt_math_errno = 0, opt_finite_math = 1, opt_no_signed_zeros = 1, opt_reciprocal_math = 1;
        else if (strcmp(a, "-fassociative-math") == 0 || strcmp(a, "-funsafe-math-optimizations") == 0)
            opt_fast_math = 1; /* floating-point sums may be reordered (maxima / minima also need -ffinite-math-only) */
        else if (strcmp(a, "-fno-fast-math") == 0)
            opt_fast_math = 0, opt_finite_math = 0, opt_no_signed_zeros = 0, opt_reciprocal_math = 0;
        else if (strcmp(a, "-fno-signed-zeros") == 0)
            opt_no_signed_zeros = 1;
        else if (strcmp(a, "-fsigned-zeros") == 0)
            opt_no_signed_zeros = 0;
        else if (strcmp(a, "-freciprocal-math") == 0)
            opt_reciprocal_math = 1;
        else if (strcmp(a, "-fno-reciprocal-math") == 0)
            opt_reciprocal_math = 0;
        else if (strcmp(a, "-fno-associative-math") == 0)
            opt_fast_math = 0;
        else if (strcmp(a, "-ffinite-math-only") == 0)
            opt_finite_math = 1;
        else if (strcmp(a, "-fno-finite-math-only") == 0)
            opt_finite_math = 0;
        else if (strcmp(a, "-fno-math-errno") == 0)
            opt_math_errno = 0;
        else if (strcmp(a, "-fmath-errno") == 0)
            opt_math_errno = 1;
        else if (strcmp(a, "-Os") == 0)
            opt_size = 1;
        else if (strncmp(a, "--target=", 9) == 0)
        {
            opt_target = strstr(a, "linux") ? TARGET_LINUX : TARGET_MACOS;
            opt_arch = ARCH_ARM64; /* (an mcc running on x86-64 starts out at ARCH_X64) */
            if (strncmp(a + 9, "x86_64", 6) == 0 || strncmp(a + 9, "amd64", 5) == 0) {
                /* x86-64 Linux: its own C library, beside the one for aarch64 */
                opt_arch = ARCH_X64;
                opt_target = TARGET_LINUX;
                lx_include = home_path(argv0, "linux-x64/include");
                lx_lib = home_path(argv0, "linux-x64/lib");
                lx_ext_include = home_path(argv0, "linux-x64/ext/include");
                lx_ext_lib = home_path(argv0, "linux-x64/ext/lib");
            }
        }
        else if (strncmp(a, "--sysroot=", 10) == 0)
            sysroot = strcmp(a + 10, "/") ? a + 10 : "";
        else if (strcmp(a, "-mglibc") == 0)
            sysroot = "";
        else if (strcmp(a, "-mno-glibc") == 0)
            sysroot = 0;
        else if (strcmp(a, "--hosted") == 0) {
            /* programs for this machine against the POSIX flavour of the library */
            sys_include = home_path(argv0, "host/include");
            libc = home_path(argv0, "host/lib/libc.a");
            hosted = 1;
        }
        else if (strcmp(a, "-mgeneral-regs-only") == 0)
            opt_no_fp = 1;
        else if (strcmp(a, "-fmcc-skip-asm") == 0)
            opt_skip_asm = 1;
        else if (strncmp(a, "--ptx-out=", 10) == 0)
            opt_ptx_out = a + 10;
        else if (strncmp(a, "--cuda-arch=", 12) == 0) {
            /* N, ten times the GPU's number as __CUDA_ARCH__ has it; a letter after it is the GPU's own (1200a) */
            const char *q = a + 12;
            cuda_arch = atoi(q);
            while (*q >= '0' && *q <= '9')
                q++;
            if (cuda_arch >= 100 && cuda_arch < 100000)
                sprintf(opt_cuda_target, "sm_%d%c", cuda_arch / 10, *q >= 'a' && *q <= 'z' ? *q : 0);
        }
        else if (strncmp(a, "-fmad=", 6) == 0)
            opt_fmad = strcmp(a + 6, "false") != 0;
        else if (strcmp(a, "-fsyntax-only") == 0)
            opt_syntax_only = compile_only = 1;
        else if (strcmp(a, "-ansi") == 0 || strcmp(a, "-std=c89") == 0 || strcmp(a, "-std=c90") == 0)
            opt_gnu = 0;
        else if (strncmp(a, "-std=", 5) == 0) {
            opt_gnu = 1;
            std_c99 = !strstr(a, "89") && !strstr(a, "90");
            opt_c23 = strstr(a, "23") || strstr(a, "2x");
        }
        else if (strncmp(a, "-j", 2) == 0 && a[2] >= '0' && a[2] <= '9')
            jobs = atoi(a + 2);
        else if (strncmp(a, "-finline-limit=", 15) == 0)
            opt_inline_limit = atoi(a + 15);
        else if (strncmp(a, "-finline-hint=", 14) == 0)
            opt_inline_hint = atoi(a + 14);
        else if (strcmp(a, "-nostdlib") == 0)
            nostdlib = 1;
        else if (strcmp(a, "-nostdinc") == 0)
            nostdinc = 1;
        else if (strcmp(a, "-o") == 0 && i + 1 < argc)
            out_path = argv[++i];
        else if (strcmp(a, "-e") == 0 && i + 1 < argc)
            entry = argv[++i];
        else if (strncmp(a, "-D", 2) == 0)
            defines[ndefines++] = a[2] ? a + 2 : argv[++i];
        else if (strncmp(a, "-I", 2) == 0) {
            const char *d = a[2] ? a + 2 : argv[++i];
            if (!is_system_dir(d))
                pp_include_dir(d);
        }
        else if (strcmp(a, "--cuda-fast-math") == 0)
            opt_fastmath = 1; /* nvcc's -use_fast_math */
        else if (strncmp(a, "-fmacro-prefix-map=", 19) == 0)
            pp_prefix_map(a + 19);
        else if (strcmp(a, "-isystem") == 0 && i + 1 < argc) {
            /* as -I: a standard directory named again (pkg-config's -isystem /usr/include) would move it ahead of
               mcc's own glibc/include, whose headers must come first (bits/floatn.h's _Float128) */
            if (!is_system_dir(argv[++i]))
                pp_system_dir(argv[i]);
        }
        else if (strcmp(a, "-include") == 0 && i + 1 < argc)
            pp_force_include(argv[++i]);
        else if (strncmp(a, "-g", 2) == 0 && strcmp(a, "-g0") != 0 && strncmp(a, "-gsplit", 7) != 0 &&
                 strncmp(a, "-gno-", 5) != 0)
            opt_debug = 1; /* -g, -g3, -ggdb, -gdwarf-5, -gline-tables-only ...: all the same here */
        else if (strcmp(a, "-g0") == 0)
            opt_debug = 0;
        else if (strncmp(a, "-W", 2) == 0 || strncmp(a, "-f", 2) == 0 || strncmp(a, "-m", 2) == 0 ||
                 strncmp(a, "-g", 2) == 0 || strncmp(a, "-O", 2) == 0 || strncmp(a, "-Q", 2) == 0 ||
                 strcmp(a, "-pipe") == 0 || strcmp(a, "-w") == 0)
            ; /* accepted for compatibility with other compilers */
        else if (a[0] == '-' && a[1])
            die("unknown option %s", a);
        else {
            input_lang[ninputs] = (char)lang;
            inputs[ninputs++] = a;
            nall_inputs = ninputs;
        }
    }
    if (want_deps && !dep_path && out_path) {
        /* next to the output, with .d for its suffix */
        size_t n = strlen(out_path);
        char *d = (char *)xmalloc(n + 3);
        const char *dot = strrchr(out_path, '.'), *slash = strrchr(out_path, '/');
        if (dot && (!slash || dot > slash))
            n = (size_t)(dot - out_path);
        memcpy(d, out_path, n);
        strcpy(d + n, ".d");
        dep_path = d;
    }
    if (ninputs == 0) {
        usage();
        return 1;
    }
    if (opt_target == TARGET_LINUX)
        sys_include = lx_include;
    if (sysroot && opt_arch != ARCH_X64)
        die("--sysroot and -mglibc are for x86_64-linux-gnu");
    if (sysroot) {
        /* the few headers that are the compiler's, then the system's */
        static const char *const dirs[] = {"/usr/local/include", "/usr/include/x86_64-linux-gnu", "/usr/include", 0};
        sys_include = home_path(argv0, "glibc/include");
        lx_ext_include = home_path(argv0, "glibc/ext/include"); /* GLIBC=... mkulibs.sh */
        lx_ext_lib = home_path(argv0, "glibc/ext/lib");
        if (!nostdinc) {
            if (sys_include)
                pp_system_dir(sys_include);
            for (k = 0; dirs[k]; k++) {
                char *d = (char *)xmalloc(strlen(sysroot) + strlen(dirs[k]) + 1);
                sprintf(d, "%s%s", sysroot, dirs[k]);
                pp_system_dir(d);
                pp_c_dir(d);
            }
        }
    } else if (!nostdinc && sys_include) {
        pp_system_dir(sys_include);
        pp_c_dir(sys_include);
    }
    if (!nostdinc && opt_target == TARGET_LINUX && lx_ext_include && os_exists(lx_ext_include))
        pp_system_dir(lx_ext_include); /* libraries installed beside the C library (mkulibs.sh) */
    for (i = 0; i < ninputs; i++)
        nsources += is_c(inputs[i]);
    if ((preprocess_only || dump_ir || (compile_only && out_path)) && (nsources != 1 || ninputs != 1) &&
        !(ninputs == 1 && kind_of(inputs[0]) != 'o'))
        die("exactly one source file is required with these options");

    /* Several source files are compiled by one child process each, at most
       "jobs" at a time. A child sends its object back through a pipe (or
       writes the file itself with -c); results are taken in input order, so
       the output does not depend on timing. */
    if (prof_path && !opt_prof_gen)
        prof_load(prof_path);
    if (jobs == 0)
        jobs = os_ncpu();
    if (nsources < 2 || preprocess_only || dump_ir || opt_target == TARGET_LINUX)
        jobs = 1;
    if (jobs > 1)
        pp_init(); /* names the parent interns later must not get in the way of the keywords */
    job_id = (int *)xmalloc((size_t)(ninputs + 1) * sizeof(int));
    job_fd = (int *)xmalloc((size_t)(ninputs + 1) * sizeof(int));

    for (i = 0; i < ninputs; i++) {
        const char *in = inputs[i];
        Obj *o;
        int child = 0;
        if ((kind_of(in) == 's' || kind_of(in) == 'S') && opt_target == TARGET_LINUX) {
            Buf out;
            int cpp = kind_of(in) == 'S';
            memset(&out, 0, sizeof out);
            if (cpp) {
                pp_init();
                target_macros();
                for (k = 0; k < ndefines; k++)
                    pp_define(defines[k]);
                for (k = 0; k < nundefs; k++)
                    pp_undef(undefs[k]);
                if (preprocess_only) {
                    pp_asm = 1;
                    pp_define("__ASSEMBLER__");
                    pp_open(in);
                    pp_text(&out);
                    if (out_path)
                        os_write_file(out_path, out.p, out.len, 0);
                    else
                        fwrite(out.p, 1, out.len, stdout);
                    write_deps(dep_path, out_path ? out_path : "-");
                    continue;
                }
            }
            elf_write_obj(&out, gas_file(in, cpp));
            if (!compile_only) {
                if (sysroot)
                    sys_temp(out_path ? out_path : "a.out", &out);
                else
                    eld_add(in, out.p, out.len, 0);
                continue;
            }
            if (!out_path)
                die("assembling for Linux needs -o");
            os_write_file(out_path, out.p, out.len, 0);
            write_deps(dep_path, out_path);
            continue;
        }
        if (kind_of(in) == 's' || kind_of(in) == 'S') {
            if ((nobjs & (nobjs - 1)) == 0)
                objs = (Obj **)xrealloc(objs, (nobjs ? nobjs * 2 : 1) * sizeof(Obj *));
            objs[nobjs++] = as_file(in);
            continue;
        }
        if (!is_c(in)) {
            if (sysroot)
                sys_obj(in);
            else if (opt_target == TARGET_LINUX)
                eld_add_file(in, 0);
            else
                macho_load_file(in, &objs, &nobjs);
            continue;
        }
        if (jobs > 1) {
            u8 *data;
            size_t len;
            if (next < i)
                next = i;
            while (!child && !failed && running < jobs && next < ninputs) {
                int j = next++;
                if (!is_c(inputs[j]))
                    continue;
                job_id[j] = os_spawn(&job_fd[j]);
                if (job_id[j] == 0) {
                    child = 1;
                    out_fd = job_fd[j];
                    in = inputs[j];
                } else {
                    running++;
                }
            }
            if (!child) {
                if (next <= i) /* never started: an earlier file failed */
                    continue;
                data = os_receive(job_fd[i], &len);
                running--;
                if (os_wait(job_id[i]) != 0) {
                    failed = 1;
                    continue;
                }
                if (compile_only)
                    continue;
                o = macho_read_obj(in, data, len);
                if ((nobjs & (nobjs - 1)) == 0)
                    objs = (Obj **)xrealloc(objs, (nobjs ? nobjs * 2 : 1) * sizeof(Obj *));
                objs[nobjs++] = o;
                continue;
            }
        }
        opt_cuda = force_cuda || has_suffix(in, ".cu");
        opt_nounroll = getenv("MCC_NOUNROLL") != 0;
        opt_nosplit = getenv("MCC_NOSPLIT") != 0;
        opt_noauto = getenv("MCC_NOAUTO") != 0;
        opt_nolicm = getenv("MCC_NOLICM") != 0;
        opt_autolog = getenv("MCC_AUTOLOG") != 0;
        opt_dolicm = getenv("MCC_LICM") ? (getenv("MCC_LICM")[0] >= '2' && getenv("MCC_LICM")[0] <= '9' ? getenv("MCC_LICM")[0] - '0' : 1) : 0;
        opt_slotcheck = getenv("MCC_SLOTCHECK") != 0;
        opt_norounds = getenv("MCC_NOROUNDS") != 0;
        opt_nosh32 = getenv("MCC_NOSH32") != 0;
        opt_nominmax = getenv("MCC_NOMINMAX") != 0;
        opt_noselk = getenv("MCC_NOSELK") != 0;
        opt_noasmco = getenv("MCC_NOASMCO") != 0;
        opt_nopred = getenv("MCC_NOPRED") != 0;
        opt_nodist = getenv("MCC_NODIST") != 0;
        opt_noifcall = getenv("MCC_NOIFCALL") != 0;
        opt_nolocal = getenv("MCC_NOLOCAL") != 0;
        opt_noxrem = getenv("MCC_NOXREM") != 0;
        opt_nowiden = getenv("MCC_NOWIDEN") ? 1 : getenv("MCC_WIDEN") ? 2 : 0;
        opt_noglobal = getenv("MCC_NOGLOBAL") != 0;
        opt_premat = getenv("MCC_PREMAT") != 0;
        opt_nospecial1 = getenv("MCC_SPECIAL1") != 0; /* (on, despite the name) */
        if (getenv("MCC_IFC"))
            opt_ifc = atoi(getenv("MCC_IFC"));
        opt_plaindiv = getenv("MCC_PLAINDIV") != 0;
        opt_nowide = getenv("MCC_NOWIDE") != 0;
        opt_nochain = getenv("MCC_NOCHAIN") != 0;
        if (getenv("MCC_AUMAX"))
            opt_aumax = (u32)atoi(getenv("MCC_AUMAX"));
        if (getenv("MCC_AUTOMAX"))
            opt_automax = (u32)atoi(getenv("MCC_AUTOMAX"));
        opt_cxx = opt_cuda; /* CUDA is a dialect of C++ */
        pp_init();
        target_macros();
        if (opt_cuda) {
            static const char *const cuda_defs[] = {
                "__CUDACC__", "__NVCC__", "__cplusplus=201703L", "__CUDACC_VER_MAJOR__=13", "__CUDACC_VER_MINOR__=0",
                "__global__=__attribute__((global))", "__device__=__attribute__((device))",
                "__host__=__attribute__((host))", "__shared__=__attribute__((shared))",
                "__constant__=__attribute__((constant))", "__forceinline__=__inline__ __attribute__((always_inline))",
                "__noinline__=__attribute__((noinline))", "__launch_bounds__(...)=__attribute__((launch_bounds(__VA_ARGS__)))", "__align__(n)=__attribute__((aligned(n)))", 0};
            static int cuda_inc_done;
            char *cuda_include = home_path(argv0, "cuda/include");
            char archdef[48];
            for (k = 0; cuda_defs[k]; k++)
                pp_define(cuda_defs[k]);
            if (sysroot) {
                /* types that are part of C++, which the system's headers therefore do not declare */
                pp_define("_GNU_SOURCE"); /* as g++ does */
                pp_define("wchar_t=int");
                pp_define("char16_t=unsigned short");
                pp_define("char32_t=unsigned int");
            }
            /* nvcc defines it for the device half of its work only; here a unit is read once, as device code */
            sprintf(archdef, "__CUDA_ARCH__=%d", cuda_arch);
            pp_define(archdef);
            if (!cuda_inc_done && !nostdinc && cuda_include) {
                /* the CUDA headers, and what nvcc makes known without an #include */
                char *dev = (char *)xmalloc(strlen(cuda_include) + 32);
                sprintf(dev, "%s/mcuda_device.h", cuda_include);
                if (os_exists(dev)) {
                    pp_system_dir(cuda_include);
                    pp_force_include(dev);
                }
                cuda_inc_done = 1;
            }
        }
        for (k = 0; k < ndefines; k++)
            pp_define(defines[k]);
        for (k = 0; k < nundefs; k++)
            pp_undef(undefs[k]);
        if (preprocess_only) {
            Buf out;
            memset(&out, 0, sizeof out);
            pp_open(in);
            pp_text(&out);
            if (out_path)
                os_write_file(out_path, out.p, out.len, 0);
            else
                fwrite(out.p, 1, out.len, stdout);
            write_deps(dep_path, out_path ? out_path : "-");
            return 0;
        }
        o = obj_new(in);
        cc_compile(in, o, dump_ir);
        if (dump_ir)
            return 0;
        if (compile_only) {
            Buf out;
            char *path = 0;
            memset(&out, 0, sizeof out);
            if (!out_path) {
                const char *base = base_name(in);
                size_t n = strlen(base);
                path = (char *)xmalloc(n + 1);
                memcpy(path, base, n + 1);
                path[n - 1] = 'o';
            }
            if (opt_asm_text) {
                os_write_file(out_path ? out_path : "a.s", opt_asm_text->p, opt_asm_text->len, 0);
                write_deps(dep_path, out_path ? out_path : "a.s");
            } else if (!opt_syntax_only) {
                if (opt_target == TARGET_LINUX)
                    elf_write_obj(&out, o);
                else
                    macho_write_obj(&out, o);
                os_write_file(out_path ? out_path : path, out.p, out.len, 0);
                write_deps(dep_path, out_path ? out_path : path);
            }
            buf_free(&out);
            if (child) {
                os_send(out_fd, out.p, 0);
                os_exit(0);
            }
            continue;
        }
        if (child) {
            Buf out;
            memset(&out, 0, sizeof out);
            macho_write_obj(&out, o);
            os_send(out_fd, out.p, out.len);
            os_exit(0);
        }
        if (opt_target == TARGET_LINUX) {
            Buf out;
            memset(&out, 0, sizeof out);
            elf_write_obj(&out, o);
            if (sysroot)
                sys_temp(out_path ? out_path : "a.out", &out);
            else
                eld_add(in, out.p, out.len, 0);
            write_deps(dep_path, out_path ? out_path : "a.out");
            continue;
        }
        write_deps(dep_path, out_path ? out_path : "a.out");
        if ((nobjs & (nobjs - 1)) == 0)
            objs = (Obj **)xrealloc(objs, (nobjs ? nobjs * 2 : 1) * sizeof(Obj *));
        objs[nobjs++] = o;
    }
    if (failed)
        return 1;
    if (preprocess_only)
        return 0;
    if (!compile_only && sysroot) {
        /* for now the system's compiler driver does the link */
        char **av = (char **)xmalloc((size_t)(nsys_objs + 2 * nlibdirs + nlibs + 20) * sizeof(char *));
        int n = 0, code;
        {
            /* MCC_CC: another driver for that, a full path (a machine with clang-22 and no cc) */
            const char *lcc = getenv("MCC_CC");
            av[n++] = (char *)(lcc && lcc[0] ? lcc : "/usr/bin/cc");
        }
        av[n++] = (char *)"-no-pie";
        if (relocatable)
            av[n++] = (char *)"-r";
        if (nostdlib)
            av[n++] = (char *)"-nostdlib";
        if (nostartfiles)
            av[n++] = (char *)"-nostartfiles";
        av[n++] = (char *)"-o";
        av[n++] = (char *)(out_path ? out_path : "a.out");
        for (k = 0; k < nsys_objs; k++)
            av[n++] = (char *)sys_objs[k];
        for (k = 0; k < nlibdirs; k++) {
            av[n] = (char *)xmalloc(strlen(libdirs[k]) + 3);
            sprintf(av[n++], "-L%s", libdirs[k]);
        }
        if (lx_ext_lib && os_exists(lx_ext_lib)) {
            av[n] = (char *)xmalloc(strlen(lx_ext_lib) + 3);
            sprintf(av[n++], "-L%s", lx_ext_lib);
        }
        for (k = 0; k < nlibs; k++) {
            av[n] = (char *)xmalloc(strlen(libs[k]) + 3);
            sprintf(av[n++], "-l%s", libs[k]);
        }
        if (!nostdlib) {
            /* what compiled code calls without the program asking: conversions of 16-bit floating point */
            char *support = home_path(argv0, "glibc/lib/libmcc.a");
            if (support && os_exists(support) && !relocatable)
                av[n++] = support;
            av[n++] = (char *)"-lm";
            av[n++] = (char *)"-lpthread";
        }
        av[n] = 0;
        os_wait_any(&code, (os_run(av, main_envp, -1, -1), 1));
        for (k = 0; k < nsys_temps; k++) {
            char *path = (char *)xmalloc(strlen(out_path ? out_path : "a.out") + 32);
            sprintf(path, "%s.mcc%d.o", out_path ? out_path : "a.out", k);
            remove(path);
        }
        return code != 0;
    }
    if (!compile_only && opt_target == TARGET_LINUX && relocatable) {
        eld_relocatable(out_path ? out_path : "a.out");
        return 0;
    }
    if (!compile_only && opt_target == TARGET_LINUX) {
        /* libraries: -lname is libname.a in a -L directory or mcc's own; the
           C library has everything the usual companions (m, pthread, ...) would */
        for (k = 0; k < nlibs; k++) {
            int d, found = 0;
            for (d = 0; d <= nlibdirs + 1 && !found; d++) {
                const char *dir = d < nlibdirs ? libdirs[d] : d == nlibdirs ? lx_lib : lx_ext_lib;
                char *path;
                size_t len;
                u8 *data;
                if (!dir)
                    continue;
                path = (char *)xmalloc(strlen(dir) + strlen(libs[k]) + 8);
                sprintf(path, "%s/lib%s.a", dir, libs[k]);
                data = os_read_file(path, &len);
                if (data) {
                    eld_add(path, data, len, 0);
                    found = 1;
                }
            }
            if (!found && strcmp(libs[k], "m") && strcmp(libs[k], "c") && strcmp(libs[k], "gcc") &&
                strcmp(libs[k], "pthread") && strcmp(libs[k], "rt") && strcmp(libs[k], "dl") &&
                strcmp(libs[k], "util") && strcmp(libs[k], "crypt") && strcmp(libs[k], "resolv") &&
                strcmp(libs[k], "gcc_eh") && strcmp(libs[k], "stdc++"))
                die("cannot find -l%s", libs[k]);
        }
        if (!nostdlib && lx_lib) {
            char *path = (char *)xmalloc(strlen(lx_lib) + 16);
            if (!nostartfiles) {
                sprintf(path, "%s/crt1.o", lx_lib);
                eld_add_file(path, 0);
                path = (char *)xmalloc(strlen(lx_lib) + 16);
            }
            sprintf(path, "%s/libc.a", lx_lib);
            eld_add_file(path, 0);
        }
        eld_link(out_path ? out_path : "a.out", entry);
        return 0;
    }
    if (!compile_only) {
        if (!nostdlib && libc)
            macho_load_file(libc, &objs, &nobjs);
        if (!entry)
            entry = nostdlib ? "_main" : "start";
        link_exe(objs, nobjs, entry, out_path ? out_path : "a.out");
    }
    return 0;
}

int main(int argc, char **argv, char **envp)
{
    const char *tool = base_name(argv[0]);
    main_envp = envp;
    if (strcmp(tool, "make") == 0 || strcmp(tool, "gmake") == 0)
        return make_main(argc, argv, envp, 1);
    if (argc > 1 && strcmp(argv[1], "make") == 0)
        return make_main(argc, argv, envp, 2);
    if (strcmp(tool, "lex") == 0 || strcmp(tool, "flex") == 0)
        return lex_main(argc - 1, argv + 1);
    if (argc > 1 && strcmp(argv[1], "lex") == 0)
        return lex_main(argc - 2, argv + 2);
    if (strcmp(tool, "yacc") == 0 || strcmp(tool, "bison") == 0)
        return yacc_main(argc - 1, argv + 1);
    if (argc > 1 && strcmp(argv[1], "yacc") == 0)
        return yacc_main(argc - 2, argv + 2);
    if (argc >= 3 && strcmp(argv[1], "--test-exe") == 0)
        return test_exe(argv[2], argc > 3 && strcmp(argv[3], "data") == 0);
    /* the cross tools: "aarch64-linux-gnu-gcc" and friends as names of this binary, or "mcc elf TOOL ..." */
    {
        const char *t = 0;
        int skip = 1, x64 = opt_arch == ARCH_X64;
        if (strncmp(tool, "aarch64-linux-gnu-", 18) == 0) {
            t = tool + 18;
            x64 = 0;
        } else if (strncmp(tool, "x86_64-linux-gnu-", 17) == 0) {
            t = tool + 17;
            x64 = 1;
            opt_arch = ARCH_X64;
            opt_target = TARGET_LINUX;
        } else if (argc > 2 && strcmp(argv[1], "elf") == 0) {
            t = argv[2];
            skip = 3;
        }
        if (t) {
            if (strcmp(t, "ar") == 0)
                return elf_ar_main(argc - skip, argv + skip);
            if (strcmp(t, "nm") == 0)
                return elf_nm_main(argc - skip, argv + skip);
            if (strcmp(t, "objcopy") == 0 || strcmp(t, "strip") == 0)
                return elf_objcopy_main(argc - skip, argv + skip, t[0] == 's');
            if (strcmp(t, "ld") == 0)
                return elf_ld_main(argc - skip, argv + skip);
            if (strcmp(t, "objdump") == 0)
                return elf_objdump_main(argc - skip, argv + skip);
            if (strcmp(t, "gcc") == 0 || strcmp(t, "cc") == 0 || strcmp(t, "nvcc") == 0) {
                /* the compiler, for this target */
                char **v = (char **)xmalloc((size_t)(argc + 3) * sizeof(char *));
                char *archv = 0;
                int k, n = 0, nv = t[0] == 'n';
                v[n++] = (char *)(x64 ? "--target=x86_64-linux-gnu" : "--target=aarch64-linux-gnu");
                for (k = skip; k < argc; k++) {
                    const char *a = argv[k];
                    if (nv) {
                        /* nvcc's own options: what code to make for which GPU is not a question here */
                        const char *g = strncmp(a, "-arch=", 6) == 0 ? a + 6 : strcmp(a, "-gencode") == 0 && k + 1 < argc ? argv[k + 1] : 0;
                        if (g && !archv) {
                            /* the GPU named first is the one device code is told it is made for (__CUDA_ARCH__) */
                            while (*g && (*g < '0' || *g > '9'))
                                g++;
                            if (atoi(g) > 0) {
                                archv = (char *)xmalloc(40);
                                sprintf(archv, "--cuda-arch=%d%c", atoi(g) * 10, g[strspn(g, "0123456789")] == 'a' ? 'a' : 0);
                                v[n++] = archv;
                            }
                        }
                        if (strcmp(a, "-lineinfo") == 0 || strcmp(a, "--generate-line-info") == 0 ||
                            strcmp(a, "-G") == 0 || strcmp(a, "--device-debug") == 0) {
                            v[n++] = (char *)"-g"; /* lines for device code (and host code) */
                            continue;
                        }
                        if (strcmp(a, "-use_fast_math") == 0) {
                            v[n++] = (char *)"--cuda-fast-math";
                            continue;
                        }
                        if (strncmp(a, "-arch=", 6) == 0 ||
                            strncmp(a, "-Xfatbin=", 9) == 0)
                            continue;
                        if (strcmp(a, "-gencode") == 0 || strcmp(a, "-Xcompiler") == 0 || strcmp(a, "-ccbin") == 0 ||
                            strcmp(a, "--default-stream") == 0 || strcmp(a, "-Xlinker") == 0 || strcmp(a, "-arch") == 0) {
                            k++;
                            continue;
                        }
                    }
                    v[n++] = argv[k];
                }
                v[n] = 0;
                return cc_main(argv[0], n, v);
            }
            die("no such tool: %s", t);
        }
    }
    if (strcmp(tool, "mas") == 0)
        return as_main(argc - 1, argv + 1);
    if (argc > 1 && strcmp(argv[1], "as") == 0)
        return as_main(argc - 2, argv + 2);
    if (strcmp(tool, "mld") == 0)
        return ld_main(argc - 1, argv + 1);
    if (argc > 1 && strcmp(argv[1], "ld") == 0)
        return ld_main(argc - 2, argv + 2);
    if (argc > 1 && strcmp(argv[1], "ar") == 0)
        return ar_main(argc - 2, argv + 2);
    return cc_main(argv[0], argc - 1, argv + 1);
}
