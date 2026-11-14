/* A directory of the host for the guest: the 9P2000.L file protocol over virtio, enough of it for Linux
   to mount the directory (`mount -t 9p -o trans=virtio,version=9p2000.L host /mnt`) and work in it.
   Every fid is a path below the shared directory; names cannot leave it, and symbolic links are handed to
   the guest unread and never followed here. Files appear as root's. */
#include "vm.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#define MSIZE 131072U
#define NHASH 1024

typedef struct Fid {
    struct Fid *next;
    u32 fid;
    char *path;
    int fd;
} Fid;

typedef struct P9 {
    char *root;
    size_t rootlen;
    Fid *fids[NHASH];
    u8 *t, *r; /* the request and the answer */
    u32 tn, tp, rn;
    int bad, ro;
} P9;

/* ---- reading the request, writing the answer (little endian) ---- */

static u64 gn(P9 *p, int n)
{
    u64 v = 0;
    int i;
    if (p->tp + (u32)n > p->tn) {
        p->bad = 1;
        return 0;
    }
    for (i = 0; i < n; i++)
        v |= (u64)p->t[p->tp + (u32)i] << (8 * i);
    p->tp += (u32)n;
    return v;
}

/* A name of the request, as a C string in a buffer of the caller's. */
static char *gs(P9 *p, char *buf, size_t size)
{
    u32 n = (u32)gn(p, 2);
    if (p->bad || p->tp + n > p->tn || n >= size) {
        p->bad = 1;
        buf[0] = 0;
        return buf;
    }
    memcpy(buf, p->t + p->tp, n);
    buf[n] = 0;
    p->tp += n;
    return buf;
}

static void pn(P9 *p, u64 v, int n)
{
    int i;
    for (i = 0; i < n; i++)
        p->r[p->rn++] = (u8)(v >> (8 * i));
}

static void ps(P9 *p, const char *s)
{
    size_t n = strlen(s);
    pn(p, n, 2);
    memcpy(p->r + p->rn, s, n);
    p->rn += (u32)n;
}

static void pqid(P9 *p, const struct stat *st)
{
    pn(p, S_ISDIR(st->st_mode) ? 0x80 : S_ISLNK(st->st_mode) ? 0x02 : 0, 1);
    pn(p, (u64)st->st_mtimespec.tv_sec ^ ((u64)st->st_size << 8), 4);
    pn(p, st->st_ino, 8);
}

/* Linux's number for an error of this system. */
static u32 lerr(int e)
{
    switch (e) {
    case ENOTEMPTY: return 39;
    case ENAMETOOLONG: return 36;
    case ELOOP: return 40;
    case ENOTSUP:
    case EOPNOTSUPP: return 95;
    case EAGAIN: return 11;
    }
    return e > 0 && e <= 34 ? (u32)e : 5;
}

/* ---- fids ---- */

static Fid *fid_get(P9 *p, u32 fid)
{
    Fid *f;
    for (f = p->fids[fid % NHASH]; f; f = f->next)
        if (f->fid == fid)
            return f;
    return 0;
}

static void fid_drop(P9 *p, u32 fid)
{
    Fid **pp, *f;
    for (pp = &p->fids[fid % NHASH]; (f = *pp) != 0; pp = &f->next)
        if (f->fid == fid) {
            *pp = f->next;
            if (f->fd >= 0)
                close(f->fd);
            free(f->path);
            free(f);
            return;
        }
}

static Fid *fid_set(P9 *p, u32 fid, char *path)
{
    Fid *f;
    fid_drop(p, fid);
    f = (Fid *)malloc(sizeof *f);
    f->fid = fid;
    f->path = path;
    f->fd = -1;
    f->next = p->fids[fid % NHASH];
    p->fids[fid % NHASH] = f;
    return f;
}

static char *copy(const char *s)
{
    char *d = (char *)malloc(strlen(s) + 1);
    strcpy(d, s);
    return d;
}

/* The path of name in the directory dir, in fresh memory; 0 for a name that is not one. */
static char *join(P9 *p, const char *dir, const char *name)
{
    char *d;
    size_t n = strlen(dir);
    if (!name[0] || strchr(name, '/'))
        return 0;
    if (!strcmp(name, "."))
        return copy(dir);
    if (!strcmp(name, "..")) {
        d = copy(dir);
        if (n > p->rootlen)
            *strrchr(d, '/') = 0;
        return d;
    }
    d = (char *)malloc(n + strlen(name) + 2);
    sprintf(d, "%s/%s", dir, name);
    return d;
}

static int open_flags(u32 lf)
{
    int f = (int)(lf & 3) | O_NOFOLLOW;
    if (lf & 0100)
        f |= O_CREAT;
    if (lf & 0200)
        f |= O_EXCL;
    if (lf & 01000)
        f |= O_TRUNC;
    if (lf & 02000)
        f |= O_APPEND;
    return f;
}

/* ---- the requests; each returns 0 or an error number ---- */

static int p9_walk(P9 *p)
{
    char name[1024], *path, *next;
    u32 fid = (u32)gn(p, 4), newfid = (u32)gn(p, 4), n = (u32)gn(p, 2), i, at;
    Fid *f = fid_get(p, fid);
    struct stat st;
    if (!f || p->bad || n > 16)
        return EINVAL;
    path = copy(f->path);
    at = p->rn;
    pn(p, 0, 2);
    for (i = 0; i < n; i++) {
        gs(p, name, sizeof name);
        if (lstat(path, &st) < 0 || !S_ISDIR(st.st_mode))
            next = 0, errno = ENOTDIR;
        else if (p->bad || !(next = join(p, path, name)))
            errno = EINVAL;
        if (!next || lstat(next, &st) < 0) {
            int e = errno;
            free(next);
            free(path);
            if (!i)
                return e;
            p->r[at] = (u8)i;
            return 0; /* as far as it went; the new fid is not made */
        }
        free(path);
        path = next;
        pqid(p, &st);
    }
    p->r[at] = (u8)n;
    fid_set(p, newfid, path);
    return 0;
}

static int p9_getattr(P9 *p)
{
    Fid *f = fid_get(p, (u32)gn(p, 4));
    struct stat st;
    if (!f)
        return EINVAL;
    if ((f->fd >= 0 ? fstat(f->fd, &st) : lstat(f->path, &st)) < 0)
        return errno;
    pn(p, 0x7ff, 8);
    pqid(p, &st);
    pn(p, st.st_mode, 4);
    pn(p, 0, 4);
    pn(p, 0, 4);
    pn(p, st.st_nlink, 8);
    pn(p, 0, 8);
    pn(p, (u64)st.st_size, 8);
    pn(p, 4096, 8);
    pn(p, (u64)st.st_blocks, 8);
    pn(p, (u64)st.st_atimespec.tv_sec, 8), pn(p, (u64)st.st_atimespec.tv_nsec, 8);
    pn(p, (u64)st.st_mtimespec.tv_sec, 8), pn(p, (u64)st.st_mtimespec.tv_nsec, 8);
    pn(p, (u64)st.st_ctimespec.tv_sec, 8), pn(p, (u64)st.st_ctimespec.tv_nsec, 8);
    pn(p, 0, 8), pn(p, 0, 8), pn(p, 0, 8), pn(p, 0, 8);
    return 0;
}

static int p9_setattr(P9 *p)
{
    Fid *f = fid_get(p, (u32)gn(p, 4));
    u32 valid = (u32)gn(p, 4), mode = (u32)gn(p, 4);
    u64 size, as, ans, ms, mns;
    struct stat st;
    gn(p, 8); /* owner and group stay */
    size = gn(p, 8);
    as = gn(p, 8), ans = gn(p, 8), ms = gn(p, 8), mns = gn(p, 8);
    if (!f || p->bad)
        return EINVAL;
    if (p->ro)
        return EROFS;
    if (lstat(f->path, &st) < 0)
        return errno;
    if (S_ISLNK(st.st_mode))
        return 0;
    if ((valid & 1) && chmod(f->path, (mode_t)(mode & 07777)) < 0)
        return errno;
    if ((valid & 8) && (f->fd >= 0 ? ftruncate(f->fd, (off_t)size) : truncate(f->path, (off_t)size)) < 0)
        return errno;
    if (valid & 0x30) {
        struct timeval tv[2];
        gettimeofday(&tv[0], 0);
        tv[1] = tv[0];
        if (!(valid & 0x10))
            tv[0].tv_sec = st.st_atimespec.tv_sec, tv[0].tv_usec = (int)(st.st_atimespec.tv_nsec / 1000);
        else if (valid & 0x80)
            tv[0].tv_sec = (long)as, tv[0].tv_usec = (int)(ans / 1000);
        if (!(valid & 0x20))
            tv[1].tv_sec = st.st_mtimespec.tv_sec, tv[1].tv_usec = (int)(st.st_mtimespec.tv_nsec / 1000);
        else if (valid & 0x100)
            tv[1].tv_sec = (long)ms, tv[1].tv_usec = (int)(mns / 1000);
        if (utimes(f->path, tv) < 0)
            return errno;
    }
    return 0;
}

static int p9_open(P9 *p)
{
    Fid *f = fid_get(p, (u32)gn(p, 4));
    u32 flags = (u32)gn(p, 4);
    struct stat st;
    if (!f || p->bad)
        return EINVAL;
    if (p->ro && ((flags & 3) || (flags & 01100)))
        return EROFS;
    if (lstat(f->path, &st) < 0)
        return errno;
    if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode))
        return S_ISLNK(st.st_mode) ? ELOOP : EPERM; /* devices and pipes of the host are not the guest's */
    if (S_ISREG(st.st_mode)) {
        if (f->fd >= 0)
            close(f->fd);
        f->fd = open(f->path, open_flags(flags) & ~(O_CREAT | O_EXCL));
        if (f->fd < 0)
            return errno;
    }
    pqid(p, &st);
    pn(p, 0, 4);
    return 0;
}

static int p9_create(P9 *p)
{
    char name[1024], *path;
    Fid *f = fid_get(p, (u32)gn(p, 4));
    u32 flags, mode;
    struct stat st;
    int fd;
    gs(p, name, sizeof name);
    flags = (u32)gn(p, 4), mode = (u32)gn(p, 4);
    if (!f || p->bad || !(path = join(p, f->path, name)))
        return EINVAL;
    if (p->ro) {
        free(path);
        return EROFS;
    }
    fd = open(path, open_flags(flags) | O_CREAT, mode & 0777);
    if (fd < 0 || fstat(fd, &st) < 0) {
        int e = errno;
        free(path);
        return e;
    }
    free(f->path);
    f->path = path;
    f->fd = fd;
    pqid(p, &st);
    pn(p, 0, 4);
    return 0;
}

static int p9_read(P9 *p)
{
    Fid *f = fid_get(p, (u32)gn(p, 4));
    u64 off = gn(p, 8);
    u32 count = (u32)gn(p, 4);
    long n;
    if (!f || p->bad || f->fd < 0)
        return EINVAL;
    if (count > MSIZE - 24)
        count = MSIZE - 24;
    n = pread(f->fd, p->r + p->rn + 4, count, (off_t)off);
    if (n < 0)
        return errno;
    pn(p, (u64)n, 4);
    p->rn += (u32)n;
    return 0;
}

static int p9_write(P9 *p)
{
    Fid *f = fid_get(p, (u32)gn(p, 4));
    u64 off = gn(p, 8);
    u32 count = (u32)gn(p, 4);
    long n;
    if (!f || p->bad || f->fd < 0 || count > p->tn - p->tp)
        return EINVAL;
    n = pwrite(f->fd, p->t + p->tp, count, (off_t)off);
    if (n < 0)
        return errno;
    pn(p, (u64)n, 4);
    return 0;
}

/* The directory is read anew for each piece; the offset is the number of entries already given. */
static int p9_readdir(P9 *p)
{
    Fid *f = fid_get(p, (u32)gn(p, 4));
    u64 off = gn(p, 8), i = 0;
    u32 count = (u32)gn(p, 4), at;
    DIR *d;
    struct dirent *e;
    if (!f || p->bad)
        return EINVAL;
    if (count > MSIZE - 24)
        count = MSIZE - 24;
    if (!(d = opendir(f->path)))
        return errno;
    at = p->rn;
    pn(p, 0, 4);
    while ((e = readdir(d)) != 0) {
        size_t n = strlen(e->d_name);
        if (i++ < off)
            continue;
        if (p->rn - at - 4 + 24 + n > count)
            break;
        pn(p, e->d_type == DT_DIR ? 0x80 : e->d_type == DT_LNK ? 0x02 : 0, 1);
        pn(p, 0, 4);
        pn(p, e->d_ino, 8);
        pn(p, i, 8);
        pn(p, e->d_type, 1);
        ps(p, e->d_name);
    }
    closedir(d);
    count = p->rn - at - 4;
    p->r[at] = (u8)count, p->r[at + 1] = (u8)(count >> 8), p->r[at + 2] = (u8)(count >> 16), p->r[at + 3] = 0;
    return 0;
}

/* mkdir, symlink, link, unlinkat, renameat, rename: a directory fid and names */
static int p9_names(P9 *p, int type)
{
    char name[1024], name2[1024], *path = 0, *path2 = 0;
    Fid *f = fid_get(p, (u32)gn(p, 4)), *f2 = 0;
    struct stat st;
    int r = -1;
    if (type == 70 || type == 20) /* link: dfid fid name; rename: fid dfid name */
        f2 = fid_get(p, (u32)gn(p, 4));
    gs(p, name, sizeof name);
    if (type == 74)
        f2 = fid_get(p, (u32)gn(p, 4));
    if (type == 16 || type == 74)
        gs(p, name2, sizeof name2);
    if (!f || p->bad || ((type == 70 || type == 20 || type == 74) && !f2))
        return EINVAL;
    if (p->ro)
        return EROFS;
    path = join(p, type == 20 ? f2->path : f->path, name);
    if (type == 74)
        path2 = join(p, f2->path, name2);
    if (!path || (type == 74 && !path2) || !strcmp(name, ".") || !strcmp(name, "..")) {
        free(path);
        free(path2);
        return EINVAL;
    }
    errno = 0;
    switch (type) {
    case 72: r = mkdir(path, (mode_t)(gn(p, 4) & 0777)); break;
    case 16: r = symlink(name2, path); break;
    case 70: r = link(f2->path, path); break;
    case 76: r = gn(p, 4) & 0x200 ? rmdir(path) : unlink(path); break;
    case 74: r = rename(path, path2); break;
    case 20:
        r = rename(f->path, path);
        if (!r) {
            free(f->path);
            f->path = path;
            path = 0;
        }
        break;
    }
    if (r < 0)
        r = errno ? errno : EIO;
    else if (type == 72 || type == 16) {
        if (lstat(path, &st) < 0)
            r = errno;
        else
            pqid(p, &st);
    }
    free(path);
    free(path2);
    return r;
}

static void p9_request(P9 *p)
{
    char name[64];
    u32 type, tag, fid, n;
    Fid *f;
    struct stat st;
    int e = 0;
    p->tp = 4, p->bad = 0;
    type = (u32)gn(p, 1), tag = (u32)gn(p, 2);
    p->rn = 7;
    switch (type) {
    case 100: /* version */
        n = (u32)gn(p, 4);
        gs(p, name, sizeof name);
        pn(p, n < MSIZE ? n : MSIZE, 4);
        ps(p, !strcmp(name, "9P2000.L") ? "9P2000.L" : "unknown");
        break;
    case 104: /* attach */
        fid = (u32)gn(p, 4);
        if (lstat(p->root, &st) < 0)
            e = errno;
        else {
            fid_set(p, fid, copy(p->root));
            pqid(p, &st);
        }
        break;
    case 110: e = p9_walk(p); break;
    case 24: e = p9_getattr(p); break;
    case 26: e = p9_setattr(p); break;
    case 12: e = p9_open(p); break;
    case 14: e = p9_create(p); break;
    case 116: e = p9_read(p); break;
    case 118: e = p9_write(p); break;
    case 40: e = p9_readdir(p); break;
    case 72:
    case 16:
    case 70:
    case 76:
    case 74:
    case 20: e = p9_names(p, (int)type); break;
    case 120: /* clunk */
        fid_drop(p, (u32)gn(p, 4));
        break;
    case 122: /* remove */
        fid = (u32)gn(p, 4);
        f = fid_get(p, fid);
        if (!f)
            e = EINVAL;
        else if (p->ro)
            e = EROFS;
        else if (lstat(f->path, &st) < 0 || (S_ISDIR(st.st_mode) ? rmdir(f->path) : unlink(f->path)) < 0)
            e = errno;
        fid_drop(p, fid);
        break;
    case 22: /* readlink */
        f = fid_get(p, (u32)gn(p, 4));
        {
            char target[1024];
            long k = f ? (long)readlink(f->path, target, sizeof target - 1) : -1;
            if (k < 0)
                e = f ? errno : EINVAL;
            else {
                target[k] = 0;
                ps(p, target);
            }
        }
        break;
    case 8: /* statfs: plenty of room */
        pn(p, 0x01021997, 4), pn(p, 4096, 4);
        pn(p, 1 << 24, 8), pn(p, 1 << 23, 8), pn(p, 1 << 23, 8);
        pn(p, 1 << 20, 8), pn(p, 1 << 19, 8), pn(p, 0, 8), pn(p, 255, 4);
        break;
    case 50: /* fsync */
        f = fid_get(p, (u32)gn(p, 4));
        if (f && f->fd >= 0)
            fsync(f->fd);
        break;
    case 52: /* lock: granted */
        pn(p, 0, 1);
        break;
    case 54: /* getlock: nobody holds one */
        gn(p, 4), gn(p, 1);
        pn(p, 2, 1);
        pn(p, gn(p, 8), 8), pn(p, gn(p, 8), 8), pn(p, gn(p, 4), 4);
        ps(p, "");
        break;
    case 108: /* flush: every request is answered at once */
        break;
    default: /* extended attributes, device nodes, ... */
        e = EOPNOTSUPP;
    }
    if (p->bad && !e)
        e = EINVAL;
    if (e) {
        p->rn = 7;
        pn(p, lerr(e), 4);
        type = 6; /* the answer will be Rlerror, 7 */
    }
    n = p->rn;
    p->rn = 0;
    pn(p, n, 4), pn(p, type + 1, 1), pn(p, tag, 2);
    p->rn = n;
}

static void p9_notify(Vm *vm, Virtio *d, int qi)
{
    P9 *p = (P9 *)d->priv;
    Iov iov[260];
    u16 head;
    int k, i;
    while ((k = vq_pop(vm, d, qi, iov, 260, &head)) > 0) {
        u32 off = 0, m;
        p->tn = 0;
        for (i = 0; i < k; i++)
            if (!iov[i].w && iov[i].len <= MSIZE - p->tn) {
                memcpy(p->t + p->tn, iov[i].p, iov[i].len);
                p->tn += iov[i].len;
            }
        p->rn = 0;
        if (p->tn >= 7)
            p9_request(p);
        for (i = 0; i < k && off < p->rn; i++) {
            if (!iov[i].w)
                continue;
            m = iov[i].len < p->rn - off ? iov[i].len : p->rn - off;
            memcpy(iov[i].p, p->r + off, m);
            off += m;
        }
        vq_push(vm, d, qi, head, off);
    }
}

int p9_add(Vm *vm, const char *dir, int readonly)
{
    P9 *p = (P9 *)calloc(1, sizeof *p);
    Virtio *d;
    struct stat st;
    if (stat(dir, &st) < 0 || !S_ISDIR(st.st_mode))
        return -1;
    p->root = copy(dir);
    p->rootlen = strlen(dir);
    while (p->rootlen > 1 && p->root[p->rootlen - 1] == '/')
        p->root[--p->rootlen] = 0;
    p->ro = readonly;
    p->t = (u8 *)malloc(MSIZE);
    p->r = (u8 *)malloc(MSIZE + 64);
    d = virtio_add(vm, 9, 1, 1, p9_notify); /* feature 0: the mount tag */
    d->priv = p;
    d->config[0] = 4;
    memcpy(d->config + 2, "host", 4);
    return 0;
}
