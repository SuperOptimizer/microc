#ifndef _SYS_IOCTL_H
#define _SYS_IOCTL_H
#define _IOC(dir, type, nr, size) ((dir) << 30 | (type) << 8 | (nr) | (size) << 16)
#define _IOC_NONE 0U
#define _IOC_WRITE 1U
#define _IOC_READ 2U
#define _IO(type, nr) _IOC(_IOC_NONE, type, nr, 0)
#define _IOR(type, nr, t) _IOC(_IOC_READ, type, nr, sizeof(t))
#define _IOW(type, nr, t) _IOC(_IOC_WRITE, type, nr, sizeof(t))
#define _IOWR(type, nr, t) _IOC(_IOC_READ | _IOC_WRITE, type, nr, sizeof(t))
#define TCGETS 0x5401
#define TCSETS 0x5402
#define TCSETSW 0x5403
#define TCSETSF 0x5404
#define TCSBRK 0x5409
#define TCXONC 0x540A
#define TCFLSH 0x540B
#define TIOCEXCL 0x540C
#define TIOCNXCL 0x540D
#define TIOCSCTTY 0x540E
#define TIOCGPGRP 0x540F
#define TIOCSPGRP 0x5410
#define TIOCOUTQ 0x5411
#define TIOCSTI 0x5412
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define TIOCMGET 0x5415
#define FIONREAD 0x541B
#define TIOCINQ FIONREAD
#define TIOCLINUX 0x541C
#define TIOCCONS 0x541D
#define TIOCGSERIAL 0x541E
#define FIONBIO 0x5421
#define TIOCNOTTY 0x5422
#define TIOCGSID 0x5429
#define TIOCGPTN 0x80045430
#define TIOCSPTLCK 0x40045431
#define FIONCLEX 0x5450
#define FIOCLEX 0x5451
#include <linux/sockios.h>
struct winsize {
    unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel;
};
int ioctl(int fd, unsigned long req, ...);
#endif
