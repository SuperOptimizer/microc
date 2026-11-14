/* 128-bit division, for the compiler: q[0..1] = a / b and q[2..3] = a % b, low doubleword first. */

static void udivmod(unsigned long *q, unsigned long alo, unsigned long ahi, unsigned long blo, unsigned long bhi)
{
    unsigned long qlo = 0, qhi = 0, rlo = 0, rhi = 0;
    int i;
    if (!ahi && !bhi && blo) {
        q[0] = alo / blo;
        q[1] = 0;
        q[2] = alo % blo;
        q[3] = 0;
        return;
    }
    for (i = 127; i >= 0; i--) {
        unsigned long bit = i >= 64 ? (ahi >> (i - 64)) & 1 : (alo >> i) & 1;
        rhi = (rhi << 1) | (rlo >> 63);
        rlo = (rlo << 1) | bit;
        if (rhi > bhi || (rhi == bhi && rlo >= blo)) {
            unsigned long borrow = rlo < blo;
            rlo -= blo;
            rhi -= bhi + borrow;
            if (i >= 64)
                qhi |= 1UL << (i - 64);
            else
                qlo |= 1UL << i;
        }
    }
    q[0] = qlo;
    q[1] = qhi;
    q[2] = rlo;
    q[3] = rhi;
}

static void neg(unsigned long *lo, unsigned long *hi)
{
    *lo = ~*lo + 1;
    *hi = ~*hi + (*lo == 0);
}

void __mcc_divmod128(unsigned long *q, unsigned long alo, unsigned long ahi, unsigned long blo, unsigned long bhi,
                     int is_signed)
{
    int na = 0, nb = 0;
    if (is_signed) {
        if ((long)ahi < 0) {
            na = 1;
            neg(&alo, &ahi);
        }
        if ((long)bhi < 0) {
            nb = 1;
            neg(&blo, &bhi);
        }
    }
    udivmod(q, alo, ahi, blo, bhi);
    if (na != nb)
        neg(&q[0], &q[1]);
    if (na)
        neg(&q[2], &q[3]);
}
