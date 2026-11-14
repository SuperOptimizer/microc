#ifndef _NETINET_IF_ETHER_H
#define _NETINET_IF_ETHER_H
#include <sys/types.h>
#include <linux/if_ether.h>
#include <net/ethernet.h>
#include <net/if_arp.h>
struct ether_arp {
    struct arphdr ea_hdr;
    unsigned char arp_sha[ETH_ALEN];
    unsigned char arp_spa[4];
    unsigned char arp_tha[ETH_ALEN];
    unsigned char arp_tpa[4];
};
#define arp_hrd ea_hdr.ar_hrd
#define arp_pro ea_hdr.ar_pro
#define arp_hln ea_hdr.ar_hln
#define arp_pln ea_hdr.ar_pln
#define arp_op ea_hdr.ar_op
#endif
