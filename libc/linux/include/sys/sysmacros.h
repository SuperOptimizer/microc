#ifndef _SYS_SYSMACROS_H
#define _SYS_SYSMACROS_H
#define major(d) ((unsigned)((((d) >> 32) & 0xfffff000) | (((d) >> 8) & 0xfff)))
#define minor(d) ((unsigned)((((d) >> 12) & 0xffffff00) | ((d) & 0xff)))
#define makedev(ma, mi) ((((unsigned long)(ma) & 0xfffff000UL) << 32) | (((unsigned long)(ma) & 0xfffUL) << 8) | (((unsigned long)(mi) & 0xffffff00UL) << 12) | ((unsigned long)(mi) & 0xffUL))
#endif
