#ifndef _NET_ETHERNET_H
#define _NET_ETHERNET_H
#include <sys/types.h>
#include <linux/if_ether.h>
struct ether_addr {
    unsigned char ether_addr_octet[ETH_ALEN];
} __attribute__((packed));
struct ether_header {
    unsigned char ether_dhost[ETH_ALEN];
    unsigned char ether_shost[ETH_ALEN];
    unsigned short ether_type;
} __attribute__((packed));
#define ETHERTYPE_PUP 0x0200
#define ETHERTYPE_SPRITE 0x0500
#define ETHERTYPE_IP 0x0800
#define ETHERTYPE_ARP 0x0806
#define ETHERTYPE_REVARP 0x8035
#define ETHERTYPE_AT 0x809B
#define ETHERTYPE_AARP 0x80F3
#define ETHERTYPE_VLAN 0x8100
#define ETHERTYPE_IPX 0x8137
#define ETHERTYPE_IPV6 0x86dd
#define ETHERTYPE_LOOPBACK 0x9000
#define ETHER_ADDR_LEN ETH_ALEN
#define ETHER_TYPE_LEN 2
#define ETHER_CRC_LEN 4
#define ETHER_HDR_LEN ETH_HLEN
#define ETHER_MIN_LEN (ETH_ZLEN + ETHER_CRC_LEN)
#define ETHER_MAX_LEN (ETH_FRAME_LEN + ETHER_CRC_LEN)
#define ETHERMTU ETH_DATA_LEN
#define ETHERMIN (ETHER_MIN_LEN - ETHER_HDR_LEN - ETHER_CRC_LEN)
#endif
