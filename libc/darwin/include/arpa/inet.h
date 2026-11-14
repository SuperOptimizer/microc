#ifndef _ARPA_INET_H
#define _ARPA_INET_H
#include <stdint.h>
uint32_t htonl(uint32_t v);
uint32_t ntohl(uint32_t v);
uint16_t htons(uint16_t v);
uint16_t ntohs(uint16_t v);
#endif
