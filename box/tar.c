/* tar: archives in the ustar format (what `docker import` and every other tar reads), and init: the first
   process of a container. */
#include "box.h"

/* --- tar ----------------------------------------------------------------------------------------------- */

/* A header block. Names longer than its fields are written the GNU way (an 'L' or 'K' member in front, whose
   data is the name); those and pax 'x' members (path=, linkpath=) are understood when reading. */
#define BLK 512
#define T_NAME 0
#define T_MODE 100
#define T_UID 108
#define T_GID 116
#define T_SIZE 124
#define T_MTIME 136
#define T_CHKSUM 148
#define T_TYPE 156
#define T_LINK 157
#define T_MAGIC 257
#define T_UNAME 265
#define T_GNAME 297
#define T_PREFIX 345

static FILE *tar_f;
static int tar_verbose;
static long tar_owner = -1, tar_group = -1, tar_mtime = -1;
static int tar_err;

static void tar_out(const void *p, size_t n)
{
    if (fwrite(p, 1, n, tar_f) != n)
        die("write: %s", strerror(errno));
}

static void octal(char *field, int width, unsigned long v)
{
    int i;
    for (i = width - 2; i >= 0; i--) {
        field[i] = (char)('0' + (v & 7));
        v >>= 3;
    }
    field[width - 1] = 0;
}

static void tar_header(const char *name, const char *link, int type, unsigned long mode, unsigned long size,
                       unsigned long mtime, unsigned long uid, unsigned long gid)
{
    char h[BLK];
    size_t n = strlen(name);
    unsigned sum = 0;
    int i;
    memset(h, 0, sizeof h);
    if (n > 100) {
        /* split at a slash into prefix (155) and name (100) when that fits */
        size_t cut = n > 156 ? n - 101 : 0;
        const char *s = 0;
        for (; cut < n && cut <= 155; cut++)
            if (name[cut] == '/' && n - cut - 1 <= 100 && n - cut - 1 > 0)
                s = name + cut;
        if (s && type != 'L' && type != 'K') {
            memcpy(h + T_PREFIX, name, (size_t)(s - name));
            memcpy(h + T_NAME, s + 1, strlen(s + 1));
        } else
            memcpy(h + T_NAME, name, 100);
    } else
        memcpy(h + T_NAME, name, n);
    octal(h + T_MODE, 8, mode);
    octal(h + T_UID, 8, uid);
    octal(h + T_GID, 8, gid);
    octal(h + T_SIZE, 12, size);
    octal(h + T_MTIME, 12, mtime);
    h[T_TYPE] = (char)type;
    if (link)
        memcpy(h + T_LINK, link, strlen(link) > 100 ? 100 : strlen(link));
    memcpy(h + T_MAGIC, "ustar\00000", 8);
    if (uid == 0)
        strcpy(h + T_UNAME, "root");
    if (gid == 0)
        strcpy(h + T_GNAME, "root");
    memset(h + T_CHKSUM, ' ', 8);
    for (i = 0; i < BLK; i++)
        sum += (unsigned char)h[i];
    octal(h + T_CHKSUM, 7, sum);
    tar_out(h, BLK);
}

static void tar_pad(unsigned long size)
{
    static const char zero[BLK];
    if (size % BLK)
        tar_out(zero, BLK - size % BLK);
}

/* does NAME fit a header, as it is or split? */
static int name_fits(const char *name)
{
    size_t n = strlen(name), cut;
    if (n <= 100)
        return 1;
    for (cut = n > 156 ? n - 101 : 0; cut < n && cut <= 155; cut++)
        if (name[cut] == '/' && n - cut - 1 <= 100 && n - cut - 1 > 0)
            return 1;
    return 0;
}

static void tar_longname(const char *name, int type)
{
    size_t n = strlen(name) + 1;
    tar_header("././@LongLink", 0, type, 0644, (unsigned long)n, 0, 0, 0);
    tar_out(name, n);
    tar_pad((unsigned long)n);
}

static int cmp_names(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void tar_add(const char *path)
{
    struct stat st;
    char link[4096];
    const char *name = path;
    char *dname = 0;
    unsigned long uid, gid, mtime;
    if (lstat(path, &st) < 0) {
        warn("%s: %s", path, strerror(errno));
        tar_err = 1;
        return;
    }
    while (name[0] == '/')
        name++;
    while (name[0] == '.' && name[1] == '/')
        name += 2;
    uid = tar_owner >= 0 ? (unsigned long)tar_owner : (unsigned long)st.st_uid;
    gid = tar_group >= 0 ? (unsigned long)tar_group : (unsigned long)st.st_gid;
    mtime = tar_mtime >= 0 ? (unsigned long)tar_mtime : (unsigned long)st.st_mtime;
    if (S_ISDIR(st.st_mode) && name[0] && strcmp(name, ".") != 0) {
        size_t n = strlen(name);
        dname = xmalloc(n + 2);
        memcpy(dname, name, n + 1);
        if (dname[n - 1] != '/')
            strcpy(dname + n, "/");
        name = dname;
    }
    if (tar_verbose && name[0] && strcmp(name, ".") != 0)
        fprintf(stderr, "%s\n", name);
    if (S_ISLNK(st.st_mode)) {
        long n = readlink(path, link, sizeof link - 1);
        if (n < 0) {
            warn("%s: %s", path, strerror(errno));
            tar_err = 1;
            return;
        }
        link[n] = 0;
        if (n > 100)
            tar_longname(link, 'K');
        if (!name_fits(name))
            tar_longname(name, 'L');
        tar_header(name, link, '2', 0777, 0, mtime, uid, gid);
    } else if (S_ISDIR(st.st_mode)) {
        DIR *d;
        struct dirent *de;
        char **names = 0;
        size_t nn = 0, cap = 0, i;
        if (name[0] && strcmp(name, ".") != 0) {
            if (!name_fits(name))
                tar_longname(name, 'L');
            tar_header(name, 0, '5', (unsigned long)(st.st_mode & 07777), 0, mtime, uid, gid);
        }
        d = opendir(path);
        if (!d) {
            warn("%s: %s", path, strerror(errno));
            tar_err = 1;
        } else {
            while ((de = readdir(d)) != 0) {
                Buf b;
                if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
                    continue;
                memset(&b, 0, sizeof b);
                bputs(&b, path);
                if (b.n && b.s[b.n - 1] != '/')
                    bputc(&b, '/');
                bputs(&b, de->d_name);
                if (nn == cap) {
                    cap = cap * 2 + 32;
                    names = xrealloc(names, cap * sizeof *names);
                }
                names[nn++] = bstr(&b);
            }
            closedir(d);
            qsort(names, nn, sizeof *names, cmp_names); /* the same archive whatever order the directory gives */
            for (i = 0; i < nn; i++) {
                tar_add(names[i]);
                free(names[i]);
            }
            free(names);
        }
    } else if (S_ISREG(st.st_mode)) {
        char buf[16384];
        unsigned long left = (unsigned long)st.st_size;
        int fd = open(path, O_RDONLY);
        if (fd < 0) {
            warn("%s: %s", path, strerror(errno));
            tar_err = 1;
        } else {
            if (!name_fits(name))
                tar_longname(name, 'L');
            tar_header(name, 0, '0', (unsigned long)(st.st_mode & 07777), left, mtime, uid, gid);
            while (left > 0) {
                long n = read(fd, buf, left < sizeof buf ? (size_t)left : sizeof buf);
                if (n < 0 && errno == EINTR)
                    continue;
                if (n <= 0) { /* the file shrank: the member keeps the size its header promised */
                    memset(buf, 0, sizeof buf);
                    n = (long)(left < sizeof buf ? left : sizeof buf);
                    tar_err = 1;
                }
                tar_out(buf, (size_t)n);
                left -= (unsigned long)n;
            }
            close(fd);
            tar_pad((unsigned long)st.st_size);
        }
    } else {
        warn("%s: not a file, directory or symbolic link (left out)", path);
    }
    free(dname);
}

static unsigned long from_octal(const char *p, int width)
{
    unsigned long v = 0;
    int i = 0;
    while (i < width && p[i] == ' ')
        i++;
    for (; i < width && p[i] >= '0' && p[i] <= '7'; i++)
        v = v << 3 | (unsigned long)(p[i] - '0');
    return v;
}

static int tar_in(char *p, size_t n)
{
    size_t got = fread(p, 1, n, tar_f);
    if (got == 0)
        return 0;
    if (got != n)
        die("unexpected end of archive");
    return 1;
}

/* a member's data as a string (long names, pax records) */
static char *tar_text(unsigned long size)
{
    char *s = xmalloc(size + 1);
    char pad[BLK];
    if (size && !tar_in(s, size))
        die("unexpected end of archive");
    s[size] = 0;
    if (size % BLK)
        tar_in(pad, BLK - size % BLK);
    return s;
}

/* a path that stays inside the directory it is unpacked in: no "..", not absolute */
static const char *safe_name(const char *name)
{
    const char *p;
    while (*name == '/')
        name++;
    for (p = name; *p; p = strchr(p, '/') ? strchr(p, '/') + 1 : p + strlen(p))
        if (p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == 0))
            return 0;
    return name;
}

static void make_parents(const char *path)
{
    char *p = xstrdup(path), *s;
    for (s = p + 1; *s; s++)
        if (*s == '/') {
            *s = 0;
            mkdir(p, 0777);
            *s = '/';
        }
    free(p);
}

static void set_mtime(const char *path, unsigned long mtime)
{
    struct timeval tv[2];
    tv[0].tv_sec = (long)mtime;
    tv[0].tv_usec = 0;
    tv[1] = tv[0];
    utimes(path, tv);
}

static int wanted(const char *name, char **pats, int npats)
{
    int i;
    size_t n;
    if (npats == 0)
        return 1;
    for (i = 0; i < npats; i++) {
        n = strlen(pats[i]);
        while (n > 1 && pats[i][n - 1] == '/')
            n--;
        if (!strncmp(name, pats[i], n) && (name[n] == 0 || name[n] == '/'))
            return 1;
    }
    return 0;
}

static int tar_read(int extract, char **pats, int npats)
{
    char h[BLK], buf[16384];
    char *longname = 0, *longlink = 0;
    for (;;) {
        char namebuf[260], linkbuf[104];
        const char *name, *target;
        unsigned long size, mode, mtime, left;
        unsigned sum = 0, want;
        int type, i, out = -1, take;
        if (!tar_in(h, BLK))
            break;
        for (i = 0; i < BLK && h[i] == 0; i++)
            ;
        if (i == BLK)
            break; /* the end mark */
        want = (unsigned)from_octal(h + T_CHKSUM, 8);
        for (i = 0; i < BLK; i++)
            sum += i >= T_CHKSUM && i < T_CHKSUM + 8 ? ' ' : (unsigned char)h[i];
        if (sum != want)
            die("not a tar archive (bad header checksum)");
        size = from_octal(h + T_SIZE, 12);
        mode = from_octal(h + T_MODE, 8);
        mtime = from_octal(h + T_MTIME, 12);
        type = h[T_TYPE] ? h[T_TYPE] : '0';
        if (type == 'L' || type == 'K') {
            char *t = tar_text(size);
            if (type == 'L') {
                free(longname);
                longname = t;
            } else {
                free(longlink);
                longlink = t;
            }
            continue;
        }
        if (type == 'x' || type == 'g') {
            /* pax records "LEN key=value\n": the path and the link target of the next member */
            char *t = tar_text(size), *p = t;
            while (type == 'x' && *p) {
                unsigned long len = strtoul(p, 0, 10);
                char *kv = strchr(p, ' ');
                if (!kv || len == 0 || len > strlen(p))
                    break;
                kv++;
                p[len - 1] = 0;
                if (!strncmp(kv, "path=", 5)) {
                    free(longname);
                    longname = xstrdup(kv + 5);
                } else if (!strncmp(kv, "linkpath=", 9)) {
                    free(longlink);
                    longlink = xstrdup(kv + 9);
                }
                p += len;
            }
            free(t);
            continue;
        }
        if (longname)
            name = longname;
        else {
            size_t n = 0;
            if (!memcmp(h + T_MAGIC, "ustar", 5) && h[T_PREFIX]) {
                memcpy(namebuf, h + T_PREFIX, 155);
                namebuf[155] = 0;
                n = strlen(namebuf);
                namebuf[n++] = '/';
            }
            memcpy(namebuf + n, h + T_NAME, 100);
            namebuf[n + 100] = 0;
            name = namebuf;
        }
        if (longlink)
            target = longlink;
        else {
            memcpy(linkbuf, h + T_LINK, 100);
            linkbuf[100] = 0;
            target = linkbuf;
        }
        if (type != '0' && type != '7')
            size = type == '1' || type == '2' || type == '5' ? 0 : size;
        take = wanted(name, pats, npats);
        if (take && (tar_verbose || !extract))
            fprintf(extract ? stderr : stdout, "%s\n", name);
        if (take && extract) {
            const char *to = safe_name(name);
            if (!to) {
                warn("%s: a path with .. (not unpacked)", name);
                tar_err = 1;
            } else if (!*to || !strcmp(to, ".") || !strcmp(to, "./")) {
                ;
            } else if (type == '5') {
                make_parents(to);
                if (mkdir(to, 0777) < 0 && errno != EEXIST) {
                    warn("%s: %s", to, strerror(errno));
                    tar_err = 1;
                } else
                    chmod(to, (mode_t)(mode & 07777));
            } else if (type == '2' || type == '1') {
                make_parents(to);
                unlink(to);
                if (type == '2' ? symlink(target, to) < 0 : !safe_name(target) || link(safe_name(target), to) < 0) {
                    warn("%s: %s", to, strerror(errno));
                    tar_err = 1;
                }
            } else if (type == '0' || type == '7') {
                make_parents(to);
                unlink(to);
                out = open(to, O_WRONLY | O_CREAT | O_TRUNC, (mode_t)(mode & 0777));
                if (out < 0) {
                    warn("%s: %s", to, strerror(errno));
                    tar_err = 1;
                }
            } else
                warn("%s: member of type '%c' (left out)", name, type);
        }
        for (left = size; left > 0;) {
            size_t n = left < sizeof buf ? (size_t)left : sizeof buf;
            if (!tar_in(buf, n))
                die("unexpected end of archive");
            if (out >= 0) {
                size_t done = 0;
                while (done < n) {
                    long w = write(out, buf + done, n - done);
                    if (w < 0 && errno == EINTR)
                        continue;
                    if (w <= 0)
                        die("%s: %s", name, strerror(errno));
                    done += (size_t)w;
                }
            }
            left -= (unsigned long)n;
        }
        if (size % BLK)
            tar_in(buf, BLK - size % BLK);
        if (out >= 0) {
            close(out);
            chmod(safe_name(name), (mode_t)(mode & 07777));
            set_mtime(safe_name(name), mtime);
        }
        free(longname);
        free(longlink);
        longname = longlink = 0;
    }
    return tar_err;
}

/* tar -c|-x|-t [-v] [-f FILE] [-C DIR] [--owner=N] [--group=N] [--mtime=@SECONDS] [PATH...]
   (the old form, "tar cvf FILE ...", too; FILE "-" or none is standard output / input) */
int tar_main(int argc, char **argv)
{
    const char *file = 0, *dir = 0;
    int mode = 0, i, k;
    char **paths = xmalloc((size_t)(argc + 1) * sizeof *paths);
    int npaths = 0;
    char zero[BLK * 2];
    prog = "tar";
    tar_verbose = 0;
    tar_err = 0;
    tar_owner = tar_group = tar_mtime = -1;
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strncmp(a, "--owner=", 8))
            tar_owner = atol(a + 8);
        else if (!strncmp(a, "--group=", 8))
            tar_group = atol(a + 8);
        else if (!strncmp(a, "--mtime=@", 9))
            tar_mtime = atol(a + 9);
        else if (!strcmp(a, "--numeric-owner"))
            ;
        else if (a[0] == '-' && a[1] == '-')
            die("unknown option %s", a);
        else if ((a[0] == '-' && a[1]) || i == 1) {
            for (k = a[0] == '-'; a[k]; k++) {
                if (a[k] == 'c' || a[k] == 'x' || a[k] == 't')
                    mode = a[k];
                else if (a[k] == 'v')
                    tar_verbose = 1;
                else if (a[k] == 'f' || a[k] == 'C') {
                    if (i + 1 >= argc)
                        die("-%c needs an argument", a[k]);
                    if (a[k] == 'f')
                        file = argv[++i];
                    else
                        dir = argv[++i];
                } else if (a[k] != 'p' && a[k] != 'o')
                    die("unknown option -%c", a[k]);
            }
        } else
            paths[npaths++] = argv[i];
    }
    if (!mode)
        die("one of -c, -x, -t is needed");
    if (file && strcmp(file, "-") != 0) {
        tar_f = fopen(file, mode == 'c' ? "wb" : "rb");
        if (!tar_f)
            die("%s: %s", file, strerror(errno));
    } else
        tar_f = mode == 'c' ? stdout : stdin;
    if (dir && chdir(dir) < 0)
        die("%s: %s", dir, strerror(errno));
    if (mode == 'c') {
        if (npaths == 0)
            die("nothing to archive");
        for (i = 0; i < npaths; i++)
            tar_add(paths[i]);
        memset(zero, 0, sizeof zero);
        tar_out(zero, sizeof zero);
        if (fflush(tar_f) != 0)
            die("write: %s", strerror(errno));
    } else
        tar_read(mode == 'x', paths, npaths);
    if (tar_f != stdout && tar_f != stdin)
        fclose(tar_f);
    free(paths);
    return tar_err;
}

/* --- init ---------------------------------------------------------------------------------------------- */

/* init [COMMAND [ARG...]]: run COMMAND (default: /bin/sh) as the one job of a container. As process 1 it
   inherits every orphan, so it waits for all of them; a signal that asks it to stop (TERM, INT, HUP, QUIT) is
   passed on to the command; it leaves with the command's status. */
static volatile int init_child;

static void init_pass(int sig)
{
    if (init_child > 0)
        kill(init_child, sig);
}

int init_main(int argc, char **argv)
{
    static char *shell[] = {(char *)"/bin/sh", 0};
    static const int sigs[] = {SIGTERM, SIGINT, SIGHUP, SIGQUIT, 0};
    char **cmd = argc > 1 ? argv + 1 : shell;
    int st, i;
    pid_t pid;
    prog = "init";
    pid = fork();
    if (pid < 0)
        die("fork: %s", strerror(errno));
    if (pid == 0) {
        if (!getenv("PATH"))
            putenv((char *)"PATH=/usr/bin:/bin");
        if (!getenv("HOME"))
            putenv((char *)"HOME=/root");
        box_exec(cmd, environ);
        _exit(127);
    }
    init_child = (int)pid;
    for (i = 0; sigs[i]; i++)
        signal(sigs[i], init_pass);
    for (;;) {
        pid_t w = wait(&st);
        if (w < 0 && errno == EINTR)
            continue;
        if (w < 0)
            return 127;
        if (w == pid)
            return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    }
}
