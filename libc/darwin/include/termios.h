/* Terminal settings; layout and numbers are Darwin's. */
#ifndef _TERMIOS_H
#define _TERMIOS_H
typedef unsigned long tcflag_t;
typedef unsigned char cc_t;
typedef unsigned long speed_t;
#define NCCS 20
struct termios {
    tcflag_t c_iflag, c_oflag, c_cflag, c_lflag;
    cc_t c_cc[NCCS];
    speed_t c_ispeed, c_ospeed;
};
#define VEOF 0
#define VEOL 1
#define VERASE 3
#define VKILL 5
#define VINTR 8
#define VQUIT 9
#define VSUSP 10
#define VSTART 12
#define VSTOP 13
#define VMIN 16
#define VTIME 17
#define IGNBRK 1
#define BRKINT 2
#define IGNPAR 4
#define PARMRK 8
#define INPCK 0x10
#define ISTRIP 0x20
#define INLCR 0x40
#define IGNCR 0x80
#define ICRNL 0x100
#define IXON 0x200
#define IXOFF 0x400
#define IXANY 0x800
#define OPOST 1
#define ONLCR 2
#define CSIZE 0x300
#define CS8 0x300
#define CSTOPB 0x400
#define CREAD 0x800
#define PARENB 0x1000
#define HUPCL 0x4000
#define CLOCAL 0x8000
#define ECHOE 2
#define ECHOK 4
#define ECHO 8
#define ECHONL 0x10
#define ISIG 0x80
#define ICANON 0x100
#define IEXTEN 0x400
#define TOSTOP 0x400000
#define NOFLSH 0x80000000UL
#define TCSANOW 0
#define TCSADRAIN 1
#define TCSAFLUSH 2
int tcgetattr(int fd, struct termios *t);
int tcsetattr(int fd, int how, const struct termios *t);
#endif
