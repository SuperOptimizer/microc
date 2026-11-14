/* mbox: the shell and the small Unix tools a build needs, in one multi-call binary. */
#ifndef BOX_H
#define BOX_H
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <limits.h>
#include <regex.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef struct Buf {
    char *s;
    size_t n, cap;
} Buf;

void bputc(Buf *b, int c);
void bput(Buf *b, const char *s, size_t n);
void bputs(Buf *b, const char *s);
char *bstr(Buf *b); /* NUL-terminates and returns the text */

void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
void die(const char *fmt, ...);
void warn(const char *fmt, ...);
int readline(FILE *f, Buf *b);       /* one line without its newline; 0 at end of file */
char *readall(int fd, size_t *len);  /* whole file, NUL-terminated */
char *readfile(const char *path, size_t *len);
FILE *xfopen(const char *path, const char *mode); /* "-" is standard input */

extern const char *prog; /* name for messages */
extern char *box_self;   /* path of this executable */
extern char **environ;

typedef int (*applet_fn)(int argc, char **argv);
applet_fn find_applet(const char *name);
const char *applet_name(int i);
int run_applet(applet_fn fn, int argc, char **argv);
char *path_search(const char *name, const char *path);
void box_exec(char **argv, char **env); /* does not return */
int box_spawn(char **argv);
void box_huff_lengths(const unsigned *freq, int n, int limit, unsigned char *len); /* code lengths */

int sh_main(int argc, char **argv);
int echo_main(int argc, char **argv);
int printf_main(int argc, char **argv);
int test_main(int argc, char **argv);
#endif
