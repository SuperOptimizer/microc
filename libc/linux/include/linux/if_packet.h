#ifndef _LINUX_IF_PACKET_H
#define _LINUX_IF_PACKET_H
#include <linux/types.h>
struct sockaddr_pkt {
    unsigned short spkt_family;
    unsigned char spkt_device[14];
    __be16 spkt_protocol;
};
struct sockaddr_ll {
    unsigned short sll_family;
    __be16 sll_protocol;
    int sll_ifindex;
    unsigned short sll_hatype;
    unsigned char sll_pkttype;
    unsigned char sll_halen;
    unsigned char sll_addr[8];
};
#define PACKET_HOST 0
#define PACKET_BROADCAST 1
#define PACKET_MULTICAST 2
#define PACKET_OTHERHOST 3
#define PACKET_OUTGOING 4
#define PACKET_LOOPBACK 5
#define PACKET_FASTROUTE 6
#define PACKET_ADD_MEMBERSHIP 1
#define PACKET_DROP_MEMBERSHIP 2
#define PACKET_RECV_OUTPUT 3
#define PACKET_RX_RING 5
#define PACKET_STATISTICS 6
#define PACKET_COPY_THRESH 7
#define PACKET_AUXDATA 8
#define PACKET_ORIGDEV 9
#define PACKET_VERSION 10
#define PACKET_HDRLEN 11
#define PACKET_RESERVE 12
#define PACKET_TX_RING 13
#define PACKET_LOSS 14
struct tpacket_stats {
    unsigned int tp_packets;
    unsigned int tp_drops;
};
struct tpacket_auxdata {
    __u32 tp_status;
    __u32 tp_len;
    __u32 tp_snaplen;
    __u16 tp_mac;
    __u16 tp_net;
    __u16 tp_vlan_tci;
    __u16 tp_vlan_tpid;
};
#define TP_STATUS_KERNEL 0
#define TP_STATUS_USER (1 << 0)
#define TP_STATUS_COPY (1 << 1)
#define TP_STATUS_LOSING (1 << 2)
#define TP_STATUS_CSUMNOTREADY (1 << 3)
#define TP_STATUS_VLAN_VALID (1 << 4)
struct packet_mreq {
    int mr_ifindex;
    unsigned short mr_type;
    unsigned short mr_alen;
    unsigned char mr_address[8];
};
#define PACKET_MR_MULTICAST 0
#define PACKET_MR_PROMISC 1
#define PACKET_MR_ALLMULTI 2
#endif
