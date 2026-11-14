#ifndef _LINUX_IF_ETHER_H
#define _LINUX_IF_ETHER_H
#include <linux/types.h>
#define ETH_ALEN 6
#define ETH_TLEN 2
#define ETH_HLEN 14
#define ETH_ZLEN 60
#define ETH_DATA_LEN 1500
#define ETH_FRAME_LEN 1514
#define ETH_FCS_LEN 4
#define ETH_P_LOOP 0x0060
#define ETH_P_PUP 0x0200
#define ETH_P_PUPAT 0x0201
#define ETH_P_IP 0x0800
#define ETH_P_X25 0x0805
#define ETH_P_ARP 0x0806
#define ETH_P_BPQ 0x08FF
#define ETH_P_RARP 0x8035
#define ETH_P_ATALK 0x809B
#define ETH_P_AARP 0x80F3
#define ETH_P_8021Q 0x8100
#define ETH_P_IPX 0x8137
#define ETH_P_IPV6 0x86DD
#define ETH_P_PAUSE 0x8808
#define ETH_P_SLOW 0x8809
#define ETH_P_PPP_DISC 0x8863
#define ETH_P_PPP_SES 0x8864
#define ETH_P_PAE 0x888E
#define ETH_P_8021AD 0x88A8
#define ETH_P_802_3 0x0001
#define ETH_P_AX25 0x0002
#define ETH_P_ALL 0x0003
#define ETH_P_802_2 0x0004
#define ETH_P_SNAP 0x0005
struct ethhdr {
    unsigned char h_dest[ETH_ALEN];
    unsigned char h_source[ETH_ALEN];
    __be16 h_proto;
} __attribute__((packed));
#endif
