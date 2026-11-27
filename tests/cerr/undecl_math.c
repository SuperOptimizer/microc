// error: call to undeclared function 'floor' (include <math.h>)
/* ufsm's phasecmp.c (11-27): floor / ceil / llround without <math.h> under -std=c23 compiled silently as int f(), so the doubles
   came back as garbage and every mapped box collapsed to 0,0,0 */
long long g(double x) { return (long long)floor(x) + (long long)ceil(x) + llround(x); }
