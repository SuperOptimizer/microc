/* System types, as Darwin has them. */
#ifndef _SYS_TYPES_H
#define _SYS_TYPES_H
#include <stddef.h>
#include <stdint.h>
typedef long ssize_t;
typedef long off_t;
typedef long off64_t;
typedef long loff_t;
typedef int pid_t;
typedef unsigned int uid_t;
typedef unsigned int gid_t;
typedef unsigned short mode_t;
typedef int dev_t;
typedef unsigned long ino_t;
typedef unsigned short nlink_t;
typedef int blksize_t;
typedef long blkcnt_t;
typedef long time_t;
typedef long clock_t;
typedef int clockid_t;
typedef int suseconds_t;
typedef unsigned int useconds_t;
typedef unsigned int id_t;
typedef int key_t;
typedef unsigned int socklen_t;
typedef unsigned char sa_family_t;
typedef char *caddr_t;
typedef unsigned char u_char;
typedef unsigned short u_short;
typedef unsigned int u_int;
typedef unsigned long u_long;
typedef unsigned char u_int8_t;
typedef unsigned short u_int16_t;
typedef unsigned int u_int32_t;
typedef unsigned long u_int64_t;
typedef unsigned int uint;
typedef unsigned long ulong;
typedef unsigned short ushort;
#endif
