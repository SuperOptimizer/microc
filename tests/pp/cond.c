#define A 1
#define B 0
#if A && !B
ok1
#else
bad1
#endif
#if defined(A) && defined B && !defined(C)
ok2
#endif
#if (2 + 3 * 4 == 14) && (10 / 3 == 3) && (10 % 3 == 1) && (1 << 4 == 16) && (-1 < 0) && (-1 > 0u)
ok3
#endif
#if 0
don't lex "this
# if 1
bad
# else
bad
# endif
#elif A == 1
ok4
#elif 1
bad4
#else
bad4
#endif
#ifdef UNDEFINED
bad5
#elif UNDEFINED_TOO + 1
ok5
#endif
#ifndef A
bad6
#else
ok6
#endif
#if 'a' == 97 && '\n' == 10 && 0x10 == 16 && 010 == 8 && (1 ? 2 : (1/0)) == 2
ok7
#endif
#if 0 /* comment
#endif inside comment */
bad8
#endif
#include "inc.h"
#include "inc.h"
#define HDR "inc.h"
#include HDR
INC_VALUE twice_ok
a ??= b ??( c ??) lo\
ng_ident
after_splice __LINE__
#line 100
line __LINE__
#
#pragma whatever
end
