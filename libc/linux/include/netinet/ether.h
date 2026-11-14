#ifndef _NETINET_ETHER_H
#define _NETINET_ETHER_H
#include <netinet/if_ether.h>
char *ether_ntoa(const struct ether_addr *a);
char *ether_ntoa_r(const struct ether_addr *a, char *buf);
struct ether_addr *ether_aton(const char *s);
struct ether_addr *ether_aton_r(const char *s, struct ether_addr *a);
int ether_ntohost(char *hostname, const struct ether_addr *a);
int ether_hostton(const char *hostname, struct ether_addr *a);
int ether_line(const char *line, struct ether_addr *a, char *hostname);
#endif
