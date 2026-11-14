#ifndef _RESOLV_H
#define _RESOLV_H
#include <sys/types.h>
#include <netinet/in.h>
#define MAXNS 3
#define MAXDNSRCH 6
#define RES_TIMEOUT 5
#define RES_DFLRETRY 2
struct __res_state {
    int retrans;
    int retry;
    unsigned long options;
    int nscount;
    struct sockaddr_in nsaddr_list[MAXNS];
    unsigned short id;
    char *dnsrch[MAXDNSRCH + 1];
    char defdname[256];
    unsigned int ndots;
};
typedef struct __res_state *res_state;
struct __res_state *__res_state(void);
#define _res (*__res_state())
#define RES_INIT 0x0001
#define RES_DEBUG 0x0002
#define RES_USEVC 0x0008
#define RES_RECURSE 0x0040
#define RES_DEFNAMES 0x0080
#define RES_DNSRCH 0x0200
#define RES_DEFAULT (RES_RECURSE | RES_DEFNAMES | RES_DNSRCH)
#define _PATH_RESCONF "/etc/resolv.conf"
int res_init(void);
int res_query(const char *name, int klass, int type, unsigned char *answer, int anslen);
int res_search(const char *name, int klass, int type, unsigned char *answer, int anslen);
int res_mkquery(int op, const char *name, int klass, int type, const unsigned char *data, int datalen,
                const unsigned char *newrr, unsigned char *buf, int buflen);
int res_send(const unsigned char *msg, int msglen, unsigned char *answer, int anslen);
int dn_expand(const unsigned char *msg, const unsigned char *eom, const unsigned char *src, char *dst, int dstsiz);
int dn_skipname(const unsigned char *src, const unsigned char *eom);
#endif
