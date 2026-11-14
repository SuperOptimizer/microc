#ifndef _STDIO_H
#define _STDIO_H
#include <stddef.h>
#include <stdarg.h>
#include <sys/types.h>
#define EOF (-1)
#define BUFSIZ 4096
#define FOPEN_MAX 64
#define FILENAME_MAX 4096
#define L_tmpnam 32
#define TMP_MAX 10000
#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2
#define P_tmpdir "/tmp"
typedef struct __file FILE;
typedef long fpos_t;
extern FILE *__stdin, *__stdout, *__stderr;
#define stdin __stdin
#define stdout __stdout
#define stderr __stderr
FILE *fopen(const char *path, const char *mode);
FILE *freopen(const char *path, const char *mode, FILE *f);
FILE *fdopen(int fd, const char *mode);
#define fopen64 fopen
int fclose(FILE *f);
int fflush(FILE *f);
size_t fread(void *buf, size_t size, size_t count, FILE *f);
size_t fwrite(const void *buf, size_t size, size_t count, FILE *f);
int fgetc(FILE *f);
int getc(FILE *f);
int getchar(void);
int ungetc(int c, FILE *f);
char *fgets(char *s, int n, FILE *f);
char *gets(char *s);
int fputc(int c, FILE *f);
int putc(int c, FILE *f);
int putchar(int c);
int fputs(const char *s, FILE *f);
int puts(const char *s);
int printf(const char *fmt, ...);
int fprintf(FILE *f, const char *fmt, ...);
int sprintf(char *s, const char *fmt, ...);
int snprintf(char *s, size_t n, const char *fmt, ...);
int dprintf(int fd, const char *fmt, ...);
int asprintf(char **out, const char *fmt, ...);
int vprintf(const char *fmt, va_list ap);
int vfprintf(FILE *f, const char *fmt, va_list ap);
int vsprintf(char *s, const char *fmt, va_list ap);
int vsnprintf(char *s, size_t n, const char *fmt, va_list ap);
int vdprintf(int fd, const char *fmt, va_list ap);
int vasprintf(char **out, const char *fmt, va_list ap);
int scanf(const char *fmt, ...);
int fscanf(FILE *f, const char *fmt, ...);
int sscanf(const char *s, const char *fmt, ...);
int vsscanf(const char *s, const char *fmt, va_list ap);
int vfscanf(FILE *f, const char *fmt, va_list ap);
int fseek(FILE *f, long off, int whence);
long ftell(FILE *f);
int fseeko(FILE *f, off_t off, int whence);
off_t ftello(FILE *f);
#define fseeko64 fseeko
#define ftello64 ftello
void rewind(FILE *f);
int fgetpos(FILE *f, fpos_t *pos);
int fsetpos(FILE *f, const fpos_t *pos);
int feof(FILE *f);
int ferror(FILE *f);
void clearerr(FILE *f);
int fileno(FILE *f);
int remove(const char *path);
int rename(const char *from, const char *to);
int renameat(int fromdir, const char *from, int todir, const char *to);
FILE *tmpfile(void);
char *tmpnam(char *s);
void setbuf(FILE *f, char *buf);
int setvbuf(FILE *f, char *buf, int mode, size_t size);
void setlinebuf(FILE *f);
void setbuffer(FILE *f, char *buf, size_t size);
void perror(const char *s);
ssize_t getline(char **line, size_t *cap, FILE *f);
ssize_t getdelim(char **line, size_t *cap, int delim, FILE *f);
FILE *popen(const char *cmd, const char *mode);
int pclose(FILE *f);
int getc_unlocked(FILE *f);
int putc_unlocked(int c, FILE *f);
int getchar_unlocked(void);
int putchar_unlocked(int c);
int fputs_unlocked(const char *s, FILE *f);
char *fgets_unlocked(char *s, int n, FILE *f);
size_t fwrite_unlocked(const void *buf, size_t size, size_t count, FILE *f);
size_t fread_unlocked(void *buf, size_t size, size_t count, FILE *f);
int fflush_unlocked(FILE *f);
int feof_unlocked(FILE *f);
int ferror_unlocked(FILE *f);
int fileno_unlocked(FILE *f);
void clearerr_unlocked(FILE *f);
int fputc_unlocked(int c, FILE *f);
int fgetc_unlocked(FILE *f);
void flockfile(FILE *f);
void funlockfile(FILE *f);
char *ctermid(char *s);
FILE *fmemopen(void *buf, size_t size, const char *mode);
FILE *open_memstream(char **ptr, size_t *sizeloc);
#endif
