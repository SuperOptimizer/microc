#ifndef _SETJMP_H
#define _SETJMP_H
typedef long jmp_buf[24];
typedef long sigjmp_buf[26];
int setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val);
int _setjmp(jmp_buf env);
void _longjmp(jmp_buf env, int val);
int sigsetjmp(sigjmp_buf env, int save);
void siglongjmp(sigjmp_buf env, int val);
#endif
