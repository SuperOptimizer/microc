#ifndef _SYS_IOCTL_H
#define _SYS_IOCTL_H
struct winsize {
    unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel;
};
#define TIOCGETA 0x40487413UL
#define TIOCSETA 0x80487414UL
#define TIOCSETAW 0x80487415UL
#define TIOCSETAF 0x80487416UL
#define TIOCGWINSZ 0x40087468UL
int ioctl(int fd, unsigned long req, ...);
#endif
