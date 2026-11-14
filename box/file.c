/* Tools that work on files as a whole: cat rm mkdir rmdir mv cp ln touch chmod ls
   readlink realpath mktemp cmp tee dd stat. */
#include "box.h"

static int copy_fd(int in, int out)
{
    char buf[65536];
    long n;
    while ((n = read(in, buf, sizeof buf)) != 0) {
        char *p = buf;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        while (n > 0) {
            long w = write(out, p, (size_t)n);
            if (w < 0) {
                if (errno == EINTR)
                    continue;
                return -1;
            }
            p += w;
            n -= w;
        }
    }
    return 0;
}

int cat_main(int argc, char **argv)
{
    int i = 1, st = 0, fd;
    while (i < argc && argv[i][0] == '-' && argv[i][1] && strcmp(argv[i], "--"))
        i++;
    if (i < argc && !strcmp(argv[i], "--"))
        i++;
    fflush(stdout);
    if (i >= argc)
        return copy_fd(0, 1) < 0;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "-"))
            fd = 0;
        else if ((fd = open(argv[i], O_RDONLY)) < 0) {
            warn("%s: %s", argv[i], strerror(errno));
            st = 1;
            continue;
        }
        if (copy_fd(fd, 1) < 0) {
            warn("%s: %s", argv[i], strerror(errno));
            st = 1;
        }
        if (fd)
            close(fd);
    }
    return st;
}

static char *joinpath(const char *dir, const char *name)
{
    size_t n = strlen(dir);
    char *s = xmalloc(n + strlen(name) + 2);
    strcpy(s, dir);
    if (n && dir[n - 1] != '/')
        s[n++] = '/';
    strcpy(s + n, name);
    return s;
}

static const char *base_of(const char *path)
{
    static char buf[4096];
    size_t n = strlen(path);
    const char *s;
    while (n > 1 && path[n - 1] == '/')
        n--;
    if (n >= sizeof buf)
        n = sizeof buf - 1;
    memcpy(buf, path, n);
    buf[n] = 0;
    s = strrchr(buf, '/');
    return s && s[1] ? s + 1 : buf;
}

static int isdir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int rm_path(const char *path, int rec, int force)
{
    struct stat st;
    int r = 0;
    if (lstat(path, &st) < 0) {
        if (force && (errno == ENOENT || errno == ENOTDIR))
            return 0;
        warn("%s: %s", path, strerror(errno));
        return 1;
    }
    if (S_ISDIR(st.st_mode)) {
        DIR *d;
        struct dirent *de;
        char **names = 0;
        int n = 0, i;
        if (!rec) {
            warn("%s: is a directory", path);
            return 1;
        }
        d = opendir(path);
        if (d) {
            while ((de = readdir(d)) != 0) {
                if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
                    continue;
                names = xrealloc(names, (size_t)(n + 1) * sizeof *names);
                names[n++] = joinpath(path, de->d_name);
            }
            closedir(d);
        }
        for (i = 0; i < n; i++) {
            r |= rm_path(names[i], rec, force);
            free(names[i]);
        }
        free(names);
        if (rmdir(path) < 0) {
            warn("%s: %s", path, strerror(errno));
            return 1;
        }
        return r;
    }
    if (unlink(path) < 0) {
        if (force && errno == ENOENT)
            return 0;
        warn("%s: %s", path, strerror(errno));
        return 1;
    }
    return 0;
}

int rm_main(int argc, char **argv)
{
    int i, rec = 0, force = 0, st = 0;
    for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *p;
        if (!strcmp(argv[i], "--")) {
            i++;
            break;
        }
        for (p = argv[i] + 1; *p; p++) {
            if (*p == 'r' || *p == 'R')
                rec = 1;
            else if (*p == 'f')
                force = 1;
        }
    }
    if (i >= argc && !force)
        die("missing operand");
    for (; i < argc; i++)
        st |= rm_path(argv[i], rec, force);
    return st;
}

static int mkdir_p(char *path, int mode)
{
    char *p;
    struct stat st;
    for (p = path + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            if (mkdir(path, 0777) < 0 && errno != EEXIST && stat(path, &st) < 0) {
                *p = '/';
                return -1;
            }
            *p = '/';
        }
    if (mkdir(path, (mode_t)mode) < 0) {
        int e = errno;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
            return 0;
        errno = e;
        return -1;
    }
    return 0;
}

int mkdir_main(int argc, char **argv)
{
    int i, parents = 0, mode = 0777, st = 0;
    for (i = 1; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "--")) {
            i++;
            break;
        }
        if (strchr(argv[i], 'p'))
            parents = 1;
        if (strchr(argv[i], 'm')) {
            const char *m = strchr(argv[i], 'm') + 1;
            if (!*m && i + 1 < argc)
                m = argv[++i];
            mode = (int)strtol(m, 0, 8);
        }
    }
    if (i >= argc)
        die("missing operand");
    for (; i < argc; i++) {
        int r = parents ? mkdir_p(argv[i], mode) : mkdir(argv[i], (mode_t)mode);
        if (r < 0) {
            warn("%s: %s", argv[i], strerror(errno));
            st = 1;
        }
    }
    return st;
}

int rmdir_main(int argc, char **argv)
{
    int i, st = 0;
    for (i = 1; i < argc; i++) {
        if (argv[i][0] == '-')
            continue;
        if (rmdir(argv[i]) < 0) {
            warn("%s: %s", argv[i], strerror(errno));
            st = 1;
        }
    }
    return st;
}

#define CP_REC 1
#define CP_LINKS 2 /* copy symbolic links as links */
#define CP_KEEP 4  /* keep times */

static int cp_path(const char *from, const char *to, int flags)
{
    struct stat st;
    int in, out, r;
    if ((flags & CP_LINKS ? lstat(from, &st) : stat(from, &st)) < 0) {
        warn("%s: %s", from, strerror(errno));
        return 1;
    }
    if (S_ISLNK(st.st_mode)) {
        char target[4096];
        long n = readlink(from, target, sizeof target - 1);
        if (n < 0)
            return 1;
        target[n] = 0;
        unlink(to);
        if (symlink(target, to) < 0) {
            warn("%s: %s", to, strerror(errno));
            return 1;
        }
        return 0;
    }
    if (S_ISDIR(st.st_mode)) {
        DIR *d;
        struct dirent *de;
        if (!(flags & CP_REC)) {
            warn("%s is a directory (not copied)", from);
            return 1;
        }
        if (mkdir(to, 0777) < 0 && !isdir(to)) {
            warn("%s: %s", to, strerror(errno));
            return 1;
        }
        d = opendir(from);
        r = 0;
        if (d) {
            while ((de = readdir(d)) != 0) {
                char *a, *b;
                if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
                    continue;
                a = joinpath(from, de->d_name);
                b = joinpath(to, de->d_name);
                r |= cp_path(a, b, flags);
                free(a);
                free(b);
            }
            closedir(d);
        }
        chmod(to, st.st_mode & 07777);
        return r;
    }
    in = open(from, O_RDONLY);
    if (in < 0) {
        warn("%s: %s", from, strerror(errno));
        return 1;
    }
    out = open(to, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 0777);
    if (out < 0) {
        unlink(to);
        out = open(to, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 0777);
    }
    if (out < 0) {
        warn("%s: %s", to, strerror(errno));
        close(in);
        return 1;
    }
    r = copy_fd(in, out) < 0;
    if (r)
        warn("%s: %s", to, strerror(errno));
    close(in);
    close(out);
    if (flags & CP_KEEP) {
        struct timeval tv[2];
        tv[0].tv_sec = st.st_atime;
        tv[0].tv_usec = 0;
        tv[1].tv_sec = st.st_mtime;
        tv[1].tv_usec = 0;
        utimes(to, tv);
        chmod(to, st.st_mode & 07777);
    }
    return r;
}

int cp_main(int argc, char **argv)
{
    int i, flags = 0, st = 0;
    const char *dest;
    for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *p;
        if (!strcmp(argv[i], "--")) {
            i++;
            break;
        }
        for (p = argv[i] + 1; *p; p++) {
            if (*p == 'r' || *p == 'R')
                flags |= CP_REC;
            else if (*p == 'a')
                flags |= CP_REC | CP_LINKS | CP_KEEP;
            else if (*p == 'd' || *p == 'P')
                flags |= CP_LINKS;
            else if (*p == 'p')
                flags |= CP_KEEP;
        }
    }
    if (argc - i < 2)
        die("missing operand");
    dest = argv[argc - 1];
    if (argc - i > 2 && !isdir(dest))
        die("%s: not a directory", dest);
    for (; i < argc - 1; i++) {
        if (isdir(dest)) {
            char *to = joinpath(dest, base_of(argv[i]));
            st |= cp_path(argv[i], to, flags);
            free(to);
        } else
            st |= cp_path(argv[i], dest, flags);
    }
    return st;
}

int mv_main(int argc, char **argv)
{
    int i, st = 0;
    const char *dest;
    for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
        if (!strcmp(argv[i], "--")) {
            i++;
            break;
        }
    if (argc - i < 2)
        die("missing operand");
    dest = argv[argc - 1];
    if (argc - i > 2 && !isdir(dest))
        die("%s: not a directory", dest);
    for (; i < argc - 1; i++) {
        char *to = isdir(dest) ? joinpath(dest, base_of(argv[i])) : xstrdup(dest);
        if (rename(argv[i], to) < 0) {
            if (errno == EXDEV) {
                if (cp_path(argv[i], to, CP_REC | CP_LINKS | CP_KEEP) || rm_path(argv[i], 1, 1))
                    st = 1;
            } else {
                warn("%s: %s", argv[i], strerror(errno));
                st = 1;
            }
        }
        free(to);
    }
    return st;
}

int ln_main(int argc, char **argv)
{
    int i, sym = 0, force = 0, nofollow = 0, st = 0;
    const char *dest;
    for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *p;
        for (p = argv[i] + 1; *p; p++) {
            if (*p == 's')
                sym = 1;
            else if (*p == 'f')
                force = 1;
            else if (*p == 'n' || *p == 'T' || *p == 'h')
                nofollow = 1;
        }
    }
    if (argc - i < 1)
        die("missing operand");
    dest = argc - i == 1 ? "." : argv[argc - 1];
    if (argc - i == 1)
        argc++;
    for (; i < argc - 1; i++) {
        struct stat ls;
        int todir = isdir(dest) && !(nofollow && lstat(dest, &ls) == 0 && S_ISLNK(ls.st_mode));
        char *to = todir ? joinpath(dest, base_of(argv[i])) : xstrdup(dest);
        if (force)
            unlink(to);
        if ((sym ? symlink(argv[i], to) : link(argv[i], to)) < 0) {
            warn("%s: %s", to, strerror(errno));
            st = 1;
        }
        free(to);
    }
    return st;
}

int touch_main(int argc, char **argv)
{
    int i, st = 0, create = 1, fd;
    const char *ref = 0;
    struct timeval tv[2], *times = 0;
    struct stat rs;
    for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (strchr(argv[i], 'c'))
            create = 0;
        if (!strcmp(argv[i], "-r") && i + 1 < argc)
            ref = argv[++i];
        else if ((!strcmp(argv[i], "-d") || !strcmp(argv[i], "-t")) && i + 1 < argc)
            i++;
    }
    if (ref) {
        if (stat(ref, &rs) < 0)
            die("%s: %s", ref, strerror(errno));
        tv[0].tv_sec = rs.st_atime;
        tv[1].tv_sec = rs.st_mtime;
        tv[0].tv_usec = tv[1].tv_usec = 0;
        times = tv;
    }
    for (; i < argc; i++) {
        if (access(argv[i], F_OK) < 0) {
            if (!create)
                continue;
            fd = open(argv[i], O_WRONLY | O_CREAT, 0666);
            if (fd < 0) {
                warn("%s: %s", argv[i], strerror(errno));
                st = 1;
                continue;
            }
            close(fd);
            if (!times)
                continue;
        }
        if (utimes(argv[i], times) < 0) {
            warn("%s: %s", argv[i], strerror(errno));
            st = 1;
        }
    }
    return st;
}

static int parse_mode(const char *s, int old, int dir)
{
    int mode = old;
    if (isdigit((unsigned char)*s))
        return (int)strtol(s, 0, 8);
    while (*s) {
        int who = 0, op, bits = 0;
        for (; *s && strchr("ugoa", *s); s++)
            who |= *s == 'u' ? 04700 : *s == 'g' ? 02070 : *s == 'o' ? 01007 : 07777;
        if (!who)
            who = 07777 & ~022;
        op = *s;
        if (!op || !strchr("+-=", op))
            return -1;
        for (s++; *s && *s != ','; s++) {
            switch (*s) {
            case 'r': bits |= 0444; break;
            case 'w': bits |= 0222; break;
            case 'x': bits |= 0111; break;
            case 'X':
                if (dir || (old & 0111))
                    bits |= 0111;
                break;
            case 's': bits |= 06000; break;
            case 't': bits |= 01000; break;
            default: return -1;
            }
        }
        bits &= who;
        if (op == '+')
            mode |= bits;
        else if (op == '-')
            mode &= ~bits;
        else
            mode = (mode & ~(who & 0777)) | bits;
        if (*s == ',')
            s++;
    }
    return mode;
}

static int chmod_path(const char *path, const char *spec, int rec)
{
    struct stat st;
    int mode, r = 0;
    if (stat(path, &st) < 0) {
        warn("%s: %s", path, strerror(errno));
        return 1;
    }
    mode = parse_mode(spec, (int)(st.st_mode & 07777), S_ISDIR(st.st_mode));
    if (mode < 0)
        die("bad mode %s", spec);
    if (chmod(path, (mode_t)mode) < 0) {
        warn("%s: %s", path, strerror(errno));
        r = 1;
    }
    if (rec && S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        struct dirent *de;
        while (d && (de = readdir(d)) != 0) {
            char *p;
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
                continue;
            p = joinpath(path, de->d_name);
            r |= chmod_path(p, spec, rec);
            free(p);
        }
        if (d)
            closedir(d);
    }
    return r;
}

int chmod_main(int argc, char **argv)
{
    int i = 1, rec = 0, st = 0;
    const char *spec;
    while (i < argc && (!strcmp(argv[i], "-R") || !strcmp(argv[i], "-f")))
        if (!strcmp(argv[i++], "-R"))
            rec = 1;
    if (argc - i < 2)
        die("missing operand");
    spec = argv[i++];
    for (; i < argc; i++)
        st |= chmod_path(argv[i], spec, rec);
    return st;
}

static int cmpname(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void ls_entry(const char *path, const char *name, int lflag)
{
    struct stat st;
    char mode[11], date[32];
    static const char kinds[] = "?pc?d?b?-?l?s???";
    int i;
    struct tm *tm;
    if (!lflag) {
        puts(name);
        return;
    }
    if (lstat(path, &st) < 0) {
        warn("%s: %s", path, strerror(errno));
        return;
    }
    mode[0] = kinds[(st.st_mode >> 12) & 15];
    for (i = 0; i < 9; i++)
        mode[1 + i] = (st.st_mode >> (8 - i)) & 1 ? "rwx"[i % 3] : '-';
    mode[10] = 0;
    tm = localtime(&st.st_mtime);
    strftime(date, sizeof date, "%b %e %H:%M", tm);
    printf("%s %2d %d %d %8ld %s %s", mode, (int)st.st_nlink, (int)st.st_uid, (int)st.st_gid, (long)st.st_size,
           date, name);
    if (S_ISLNK(st.st_mode)) {
        char target[4096];
        long n = readlink(path, target, sizeof target - 1);
        if (n > 0) {
            target[n] = 0;
            printf(" -> %s", target);
        }
    }
    putchar('\n');
}

int ls_main(int argc, char **argv)
{
    int i, lflag = 0, dflag = 0, aflag = 0, st = 0, nfiles;
    for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *p;
        for (p = argv[i] + 1; *p; p++) {
            if (*p == 'l' || *p == 'n')
                lflag = 1;
            else if (*p == 'd')
                dflag = 1;
            else if (*p == 'a' || *p == 'A')
                aflag = *p;
        }
    }
    nfiles = argc - i;
    if (!nfiles) {
        static char *dot[] = { (char *)".", 0 };
        argv = dot;
        i = 0;
        argc = 1;
    }
    for (; i < argc; i++) {
        struct stat s;
        if (lstat(argv[i], &s) < 0) {
            warn("%s: %s", argv[i], strerror(errno));
            st = 1;
            continue;
        }
        if (!dflag && (isdir(argv[i]) && !(lflag && S_ISLNK(s.st_mode)))) {
            DIR *d = opendir(argv[i]);
            struct dirent *de;
            char **names = 0;
            int n = 0, j;
            if (!d) {
                warn("%s: %s", argv[i], strerror(errno));
                st = 1;
                continue;
            }
            while ((de = readdir(d)) != 0) {
                if (de->d_name[0] == '.' && (!aflag || (aflag == 'A' && (!de->d_name[1] || !strcmp(de->d_name, "..")))))
                    continue;
                names = xrealloc(names, (size_t)(n + 1) * sizeof *names);
                names[n++] = xstrdup(de->d_name);
            }
            closedir(d);
            qsort(names, (size_t)n, sizeof *names, cmpname);
            if (nfiles > 1)
                printf("%s:\n", argv[i]);
            for (j = 0; j < n; j++) {
                char *p = joinpath(argv[i], names[j]);
                ls_entry(p, names[j], lflag);
                free(p);
                free(names[j]);
            }
            free(names);
        } else
            ls_entry(argv[i], argv[i], lflag);
    }
    return st;
}

static char *real_path(const char *path)
{
    char buf[4096], *r = realpath(path, buf);
    return r ? xstrdup(r) : 0;
}

int readlink_main(int argc, char **argv)
{
    int i = 1, canon = 0, nl = 1;
    char buf[4096];
    long n;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (strchr(argv[i], 'f') || strchr(argv[i], 'e') || strchr(argv[i], 'm'))
            canon = 1;
        if (strchr(argv[i], 'n'))
            nl = 0;
    }
    if (i >= argc)
        die("missing operand");
    if (canon || !strcmp(argv[0], "realpath")) {
        char *r = real_path(argv[i]);
        if (!r)
            return 1;
        fputs(r, stdout);
    } else {
        n = readlink(argv[i], buf, sizeof buf - 1);
        if (n < 0)
            return 1;
        buf[n] = 0;
        fputs(buf, stdout);
    }
    if (nl)
        putchar('\n');
    return 0;
}

int mktemp_main(int argc, char **argv)
{
    int i, dir = 0, dry = 0, tflag = 0, fd;
    const char *tmpl = 0, *base = 0;
    char *path;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-d"))
            dir = 1;
        else if (!strcmp(argv[i], "-u"))
            dry = 1;
        else if (!strcmp(argv[i], "-t"))
            tflag = 1;
        else if (!strcmp(argv[i], "-q"))
            ;
        else if (!strcmp(argv[i], "-p") && i + 1 < argc)
            base = argv[++i];
        else if (!strncmp(argv[i], "--tmpdir", 8)) {
            tflag = 1;
            if (argv[i][8] == '=')
                base = argv[i] + 9;
        } else
            tmpl = argv[i];
    }
    if (!tmpl) {
        tmpl = "tmp.XXXXXXXXXX";
        tflag = 1;
    }
    if ((tflag || base) && !strchr(tmpl, '/')) {
        if (!base)
            base = getenv("TMPDIR");
        if (!base || !*base)
            base = "/tmp";
        path = joinpath(base, tmpl);
    } else
        path = xstrdup(tmpl);
    if (dir) {
        if (!mkdtemp(path))
            die("%s: %s", path, strerror(errno));
        if (dry)
            rmdir(path);
    } else {
        fd = mkstemp(path);
        if (fd < 0)
            die("%s: %s", path, strerror(errno));
        close(fd);
        if (dry)
            unlink(path);
    }
    puts(path);
    return 0;
}

int cmp_main(int argc, char **argv)
{
    int i = 1, silent = 0, c1, c2;
    long byte = 1, line = 1;
    FILE *a, *b;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
        if (strchr(argv[i], 's'))
            silent = 1;
    if (argc - i < 1)
        die("missing operand");
    a = !strcmp(argv[i], "-") ? stdin : fopen(argv[i], "r");
    b = i + 1 >= argc || !strcmp(argv[i + 1], "-") ? stdin : fopen(argv[i + 1], "r");
    if (!a || !b) {
        if (!silent)
            warn("%s: %s", a ? argv[i + 1] : argv[i], strerror(errno));
        return 2;
    }
    for (;; byte++) {
        c1 = getc(a);
        c2 = getc(b);
        if (c1 != c2) {
            if (silent)
                return 1;
            if (c1 == EOF || c2 == EOF)
                warn("EOF on %s", c1 == EOF ? argv[i] : argv[i + 1]);
            else
                printf("%s %s differ: char %ld, line %ld\n", argv[i], i + 1 < argc ? argv[i + 1] : "-", byte, line);
            return 1;
        }
        if (c1 == EOF)
            return 0;
        if (c1 == '\n')
            line++;
    }
}

int tee_main(int argc, char **argv)
{
    int i = 1, append = 0, n = 0, j, st = 0;
    int fds[64];
    char buf[65536];
    long got;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
        if (strchr(argv[i], 'a'))
            append = 1;
    fds[n++] = 1;
    for (; i < argc && n < 64; i++) {
        int fd = open(argv[i], O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC), 0666);
        if (fd < 0) {
            warn("%s: %s", argv[i], strerror(errno));
            st = 1;
        } else
            fds[n++] = fd;
    }
    fflush(stdout);
    while ((got = read(0, buf, sizeof buf)) > 0)
        for (j = 0; j < n; j++) {
            long off = 0, w;
            while (off < got && (w = write(fds[j], buf + off, (size_t)(got - off))) > 0)
                off += w;
        }
    return st;
}

static long dd_num(const char *s)
{
    char *end;
    long v = strtol(s, &end, 10);
    switch (*end) {
    case 'k': case 'K': v *= 1024; break;
    case 'M': v *= 1024 * 1024; break;
    case 'G': v *= 1024L * 1024 * 1024; break;
    case 'b': v *= 512; break;
    case 'w': v *= 2; break;
    }
    return v;
}

static long read_full(int fd, char *buf, long n)
{
    long got = 0, r;
    while (got < n) {
        r = read(fd, buf + got, (size_t)(n - got));
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        got += r;
    }
    return got;
}

int dd_main(int argc, char **argv)
{
    long ibs = 512, obs = 512, count = -1, skip = 0, seek = 0, full = 0, part = 0, total = 0, n;
    int in = 0, out = 1, i, notrunc = 0, quiet = 0;
    const char *ifn = 0, *ofn = 0;
    char *buf;
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strncmp(a, "if=", 3))
            ifn = a + 3;
        else if (!strncmp(a, "of=", 3))
            ofn = a + 3;
        else if (!strncmp(a, "bs=", 3))
            ibs = obs = dd_num(a + 3);
        else if (!strncmp(a, "ibs=", 4))
            ibs = dd_num(a + 4);
        else if (!strncmp(a, "obs=", 4))
            obs = dd_num(a + 4);
        else if (!strncmp(a, "count=", 6))
            count = dd_num(a + 6);
        else if (!strncmp(a, "skip=", 5))
            skip = dd_num(a + 5);
        else if (!strncmp(a, "seek=", 5))
            seek = dd_num(a + 5);
        else if (!strncmp(a, "conv=", 5))
            notrunc = strstr(a, "notrunc") != 0;
        else if (!strncmp(a, "status=", 7))
            quiet = 1;
        else
            die("bad operand %s", a);
    }
    if (ibs <= 0 || obs <= 0)
        die("bad block size");
    fflush(stdout);
    if (ifn && (in = open(ifn, O_RDONLY)) < 0)
        die("%s: %s", ifn, strerror(errno));
    if (ofn && (out = open(ofn, O_WRONLY | O_CREAT | (notrunc || seek ? 0 : O_TRUNC), 0666)) < 0)
        die("%s: %s", ofn, strerror(errno));
    buf = xmalloc((size_t)ibs);
    if (skip && lseek(in, skip * ibs, SEEK_SET) < 0)
        for (; skip > 0; skip--)
            if (read_full(in, buf, ibs) <= 0)
                break;
    if (seek && lseek(out, seek * obs, SEEK_SET) < 0)
        die("cannot seek");
    while (count < 0 || full + part < count) {
        long off = 0, w;
        n = read_full(in, buf, ibs);
        if (n <= 0)
            break;
        if (n == ibs)
            full++;
        else
            part++;
        while (off < n && (w = write(out, buf + off, (size_t)(n - off))) > 0)
            off += w;
        if (off < n)
            die("write error: %s", strerror(errno));
        total += n;
    }
    if (!quiet)
        fprintf(stderr, "%ld+%ld records in\n%ld+%ld records out\n%ld bytes copied\n", full, part, full, part, total);
    return 0;
}

int stat_main(int argc, char **argv)
{
    int i = 1, st = 0;
    const char *fmt = "%n %s";
    if (argc > 2 && (!strcmp(argv[1], "-c") || !strcmp(argv[1], "-f"))) {
        fmt = argv[2];
        i = 3;
    } else if (argc > 1 && !strncmp(argv[1], "--format=", 9)) {
        fmt = argv[1] + 9;
        i = 2;
    }
    for (; i < argc; i++) {
        struct stat s;
        const char *p;
        if (stat(argv[i], &s) < 0) {
            warn("%s: %s", argv[i], strerror(errno));
            st = 1;
            continue;
        }
        for (p = fmt; *p; p++) {
            if (*p != '%' || !p[1]) {
                putchar(*p);
                continue;
            }
            switch (*++p) {
            case 'n': fputs(argv[i], stdout); break;
            case 's': case 'z': printf("%ld", (long)s.st_size); break;
            case 'a': printf("%o", (unsigned)(s.st_mode & 07777)); break;
            case 'u': printf("%d", (int)s.st_uid); break;
            case 'g': printf("%d", (int)s.st_gid); break;
            case 'Y': case 'm': printf("%ld", (long)s.st_mtime); break;
            case 'X': printf("%ld", (long)s.st_atime); break;
            case 'h': printf("%d", (int)s.st_nlink); break;
            case 'i': printf("%lu", (unsigned long)s.st_ino); break;
            default: putchar(*p);
            }
        }
        putchar('\n');
    }
    return st;
}
