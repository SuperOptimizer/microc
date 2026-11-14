#ifndef _NETINET_UDP_H
#define _NETINET_UDP_H
#include <sys/types.h>
struct udphdr {
    union {
        struct {
            unsigned short uh_sport, uh_dport, uh_ulen, uh_sum;
        };
        struct {
            unsigned short source, dest, len, check;
        };
    };
};
#define UDP_CORK 1
#define UDP_ENCAP 100
#define SOL_UDP 17
#endif
