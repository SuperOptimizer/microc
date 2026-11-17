/* The users and groups of /etc/passwd and /etc/group (pwd.h, grp.h), read line by line; no other source of
   names. The results are in static storage, as the functions' descriptions allow: one for users, one for
   groups. */
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LINE 1024
#define MEMBERS 64

static FILE *pw_file, *gr_file;
static char pw_line[LINE], gr_line[LINE];
static struct passwd pw;
static struct group gr;
static char *gr_members[MEMBERS + 1];

/* the next field of a line: *p moves past its colon */
static char *field(char **p)
{
    char *s = *p, *e;
    if (!s)
        return 0;
    e = strchr(s, ':');
    if (e) {
        *e = 0;
        *p = e + 1;
    } else
        *p = 0;
    return s;
}

static int get_line(FILE *f, char *buf)
{
    size_t n;
    for (;;) {
        if (!fgets(buf, LINE, f))
            return 0;
        n = strlen(buf);
        while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
            buf[--n] = 0;
        if (n && buf[0] != '#')
            return 1;
    }
}

static struct passwd *pw_next(FILE *f)
{
    while (get_line(f, pw_line)) {
        char *p = pw_line, *uid, *gid;
        pw.pw_name = field(&p);
        pw.pw_passwd = field(&p);
        uid = field(&p);
        gid = field(&p);
        pw.pw_gecos = field(&p);
        pw.pw_dir = field(&p);
        pw.pw_shell = field(&p);
        if (!gid || !pw.pw_dir)
            continue; /* (not a line of the file's form) */
        pw.pw_uid = (uid_t)strtoul(uid, 0, 10);
        pw.pw_gid = (gid_t)strtoul(gid, 0, 10);
        if (!pw.pw_shell)
            pw.pw_shell = (char *)"";
        return &pw;
    }
    return 0;
}

static struct group *gr_next(FILE *f)
{
    while (get_line(f, gr_line)) {
        char *p = gr_line, *gid, *m;
        int n = 0;
        gr.gr_name = field(&p);
        gr.gr_passwd = field(&p);
        gid = field(&p);
        if (!gid)
            continue;
        gr.gr_gid = (gid_t)strtoul(gid, 0, 10);
        for (m = p; m && *m && n < MEMBERS;) {
            char *e = strchr(m, ',');
            if (e)
                *e = 0;
            gr_members[n++] = m;
            m = e ? e + 1 : 0;
        }
        gr_members[n] = 0;
        gr.gr_mem = gr_members;
        return &gr;
    }
    return 0;
}

void setpwent(void)
{
    if (pw_file)
        fclose(pw_file);
    pw_file = 0;
}
void endpwent(void) { setpwent(); }
struct passwd *getpwent(void)
{
    if (!pw_file)
        pw_file = fopen("/etc/passwd", "r");
    return pw_file ? pw_next(pw_file) : 0;
}

struct passwd *getpwnam(const char *name)
{
    FILE *f = fopen("/etc/passwd", "r");
    struct passwd *p = 0;
    if (!f)
        return 0;
    while ((p = pw_next(f)) != 0 && strcmp(p->pw_name, name) != 0)
        ;
    fclose(f);
    return p;
}

struct passwd *getpwuid(uid_t uid)
{
    FILE *f = fopen("/etc/passwd", "r");
    struct passwd *p = 0;
    if (!f)
        return 0;
    while ((p = pw_next(f)) != 0 && p->pw_uid != uid)
        ;
    fclose(f);
    return p;
}

void setgrent(void)
{
    if (gr_file)
        fclose(gr_file);
    gr_file = 0;
}
void endgrent(void) { setgrent(); }
struct group *getgrent(void)
{
    if (!gr_file)
        gr_file = fopen("/etc/group", "r");
    return gr_file ? gr_next(gr_file) : 0;
}

struct group *getgrnam(const char *name)
{
    FILE *f = fopen("/etc/group", "r");
    struct group *g = 0;
    if (!f)
        return 0;
    while ((g = gr_next(f)) != 0 && strcmp(g->gr_name, name) != 0)
        ;
    fclose(f);
    return g;
}

struct group *getgrgid(gid_t gid)
{
    FILE *f = fopen("/etc/group", "r");
    struct group *g = 0;
    if (!f)
        return 0;
    while ((g = gr_next(f)) != 0 && g->gr_gid != gid)
        ;
    fclose(f);
    return g;
}

/* the groups of a user: the one given and every group of /etc/group that lists the name. *n: the room in
   groups on entry, the number there are on return; -1 when they did not all fit. */
int getgrouplist(const char *user, gid_t group, gid_t *groups, int *n)
{
    FILE *f = fopen("/etc/group", "r");
    struct group *g;
    int room = *n, k = 0, i;
    if (k < room)
        groups[k] = group;
    k++;
    while (f && (g = gr_next(f)) != 0) {
        if (g->gr_gid == group)
            continue;
        for (i = 0; g->gr_mem[i]; i++)
            if (strcmp(g->gr_mem[i], user) == 0) {
                if (k < room)
                    groups[k] = g->gr_gid;
                k++;
                break;
            }
    }
    if (f)
        fclose(f);
    *n = k;
    return k > room ? -1 : k;
}

int initgroups(const char *user, gid_t group)
{
    gid_t list[64];
    int n = 64;
    if (getgrouplist(user, group, list, &n) < 0)
        n = 64;
    return setgroups((size_t)n, list);
}
