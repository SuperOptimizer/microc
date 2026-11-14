/* Network names for Linux: address conversion, a small DNS resolver (A and PTR records over UDP,
   name servers from /etc/resolv.conf), /etc/hosts, and the lookups built on them. */
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <resolv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/ether.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

int h_errno;

/* ---- addresses as text ---- */

int inet_aton(const char *s, struct in_addr *a)
{
    unsigned long parts[4];
    int n = 0;
    char *e;
    for (;;) {
        if (!isdigit((unsigned char)*s))
            return 0;
        parts[n] = strtoul(s, &e, 0);
        if (e == s)
            return 0;
        n++;
        if (*e != '.' || n == 4)
            break;
        s = e + 1;
    }
    if (*e && !isspace((unsigned char)*e))
        return 0;
    if (n == 1)
        a->s_addr = htonl((unsigned)parts[0]);
    else if (n == 2)
        a->s_addr = htonl((unsigned)(parts[0] << 24 | (parts[1] & 0xffffff)));
    else if (n == 3)
        a->s_addr = htonl((unsigned)(parts[0] << 24 | parts[1] << 16 | (parts[2] & 0xffff)));
    else
        a->s_addr = htonl((unsigned)(parts[0] << 24 | parts[1] << 16 | parts[2] << 8 | parts[3]));
    return 1;
}

in_addr_t inet_addr(const char *s)
{
    struct in_addr a;
    return inet_aton(s, &a) ? a.s_addr : INADDR_NONE;
}

char *inet_ntoa(struct in_addr a)
{
    static char buf[16];
    unsigned v = ntohl(a.s_addr);
    sprintf(buf, "%u.%u.%u.%u", v >> 24, v >> 16 & 255, v >> 8 & 255, v & 255);
    return buf;
}

const char *inet_ntop(int af, const void *src, char *dst, socklen_t n)
{
    if (af == AF_INET) {
        const unsigned char *p = src;
        char buf[16];
        sprintf(buf, "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
        if (strlen(buf) < n)
            return strcpy(dst, buf);
        errno = ENOSPC;
        return 0;
    }
    errno = EAFNOSUPPORT;
    return 0;
}

int inet_pton(int af, const char *src, void *dst)
{
    unsigned char b[4];
    int i;
    if (af != AF_INET) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    for (i = 0; i < 4; i++) {
        int v = 0, digits = 0;
        while (*src >= '0' && *src <= '9' && digits < 3) {
            v = v * 10 + (*src++ - '0');
            digits++;
        }
        if (!digits || v > 255)
            return 0;
        if (*src != (i < 3 ? '.' : 0))
            return 0;
        src++;
        b[i] = (unsigned char)v;
    }
    memcpy(dst, b, 4);
    return 1;
}

/* ---- ethernet addresses ---- */

struct ether_addr *ether_aton_r(const char *s, struct ether_addr *a)
{
    int i;
    for (i = 0; i < 6; i++) {
        char *e;
        unsigned long v = strtoul(s, &e, 16);
        if (e == s || v > 255 || (i < 5 && *e != ':'))
            return 0;
        a->ether_addr_octet[i] = (unsigned char)v;
        s = e + 1;
    }
    return a;
}

struct ether_addr *ether_aton(const char *s)
{
    static struct ether_addr a;
    return ether_aton_r(s, &a);
}

char *ether_ntoa_r(const struct ether_addr *a, char *buf)
{
    const unsigned char *p = a->ether_addr_octet;
    sprintf(buf, "%x:%x:%x:%x:%x:%x", p[0], p[1], p[2], p[3], p[4], p[5]);
    return buf;
}

char *ether_ntoa(const struct ether_addr *a)
{
    static char buf[18];
    return ether_ntoa_r(a, buf);
}

int ether_line(const char *line, struct ether_addr *a, char *hostname)
{
    const char *p = line;
    int n = 0;
    while (isspace((unsigned char)*p))
        p++;
    if (*p == '#' || !ether_aton_r(p, a))
        return -1;
    while (*p && !isspace((unsigned char)*p))
        p++;
    while (isspace((unsigned char)*p))
        p++;
    while (*p && *p != '#' && !isspace((unsigned char)*p) && n < 255)
        hostname[n++] = *p++;
    hostname[n] = 0;
    return n ? 0 : -1;
}

static int ethers(char *hostname, struct ether_addr *a, int by_name)
{
    FILE *f = fopen("/etc/ethers", "r");
    char line[512], name[256];
    struct ether_addr e;
    int found = -1;
    if (!f)
        return -1;
    while (found && fgets(line, sizeof line, f)) {
        if (ether_line(line, &e, name))
            continue;
        if (by_name && !strcmp(name, hostname)) {
            *a = e;
            found = 0;
        } else if (!by_name && !memcmp(&e, a, 6)) {
            strcpy(hostname, name);
            found = 0;
        }
    }
    fclose(f);
    return found;
}

int ether_hostton(const char *hostname, struct ether_addr *a) { return ethers((char *)hostname, a, 1); }
int ether_ntohost(char *hostname, const struct ether_addr *a) { return ethers(hostname, (struct ether_addr *)a, 0); }

/* ---- interfaces ---- */

unsigned if_nametoindex(const char *name)
{
    struct ifreq r;
    int fd, ok;
    if (strlen(name) >= IFNAMSIZ) {
        errno = ENODEV;
        return 0;
    }
    fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return 0;
    memset(&r, 0, sizeof r);
    strcpy(r.ifr_name, name);
    ok = ioctl(fd, SIOCGIFINDEX, &r) == 0;
    close(fd);
    return ok ? (unsigned)r.ifr_ifindex : 0;
}

char *if_indextoname(unsigned index, char *name)
{
    struct ifreq r;
    int fd, ok;
    fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return 0;
    memset(&r, 0, sizeof r);
    r.ifr_ifindex = (int)index;
    ok = ioctl(fd, SIOCGIFNAME, &r) == 0;
    close(fd);
    if (!ok) {
        if (errno == ENODEV)
            errno = ENXIO;
        return 0;
    }
    r.ifr_name[IFNAMSIZ - 1] = 0;
    return strcpy(name, r.ifr_name);
}

/* ---- the resolver ---- */

#define T_A 1
#define T_CNAME 5
#define T_PTR 12
#define MAXADDR 16
#define PKTSZ 1232

static struct __res_state res;

struct __res_state *__res_state(void) { return &res; }

static void add_server(const char *text)
{
    struct in_addr a;
    if (res.nscount < MAXNS && inet_aton(text, &a)) {
        struct sockaddr_in *s = &res.nsaddr_list[res.nscount++];
        memset(s, 0, sizeof *s);
        s->sin_family = AF_INET;
        s->sin_port = htons(53);
        s->sin_addr = a;
    }
}

int res_init(void)
{
    FILE *f;
    char line[256];
    struct timespec ts;
    res.nscount = 0;
    res.retrans = 3;
    res.retry = RES_DFLRETRY;
    res.ndots = 1;
    f = fopen(_PATH_RESCONF, "r");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            char *p = line;
            while (*p == ' ' || *p == '\t')
                p++;
            if (strncmp(p, "nameserver", 10) || !isspace((unsigned char)p[10]))
                continue;
            p += 10;
            while (*p == ' ' || *p == '\t')
                p++;
            add_server(p);
        }
        fclose(f);
    }
    if (!res.nscount)
        add_server("10.0.2.3");
    clock_gettime(CLOCK_REALTIME, &ts);
    res.id = (unsigned short)(ts.tv_nsec ^ ts.tv_nsec >> 16 ^ ts.tv_sec ^ getpid() << 5);
    res.options = RES_DEFAULT | RES_INIT;
    return 0;
}

int res_mkquery(int op, const char *name, int klass, int type, const unsigned char *data, int datalen,
                const unsigned char *newrr, unsigned char *buf, int buflen)
{
    unsigned char *p = buf + 12;
    size_t total = strlen(name);
    (void)data;
    (void)datalen;
    (void)newrr;
    if (!(res.options & RES_INIT))
        res_init();
    if (total > 253 || buflen < (int)total + 18) {
        h_errno = NO_RECOVERY;
        return -1;
    }
    res.id = (unsigned short)(res.id * 25173 + 13849);
    memset(buf, 0, 12);
    buf[0] = (unsigned char)(res.id >> 8);
    buf[1] = (unsigned char)res.id;
    buf[2] = (unsigned char)(op << 3 | 1);
    buf[5] = 1;
    while (*name) {
        const char *dot = strchr(name, '.');
        size_t n = dot ? (size_t)(dot - name) : strlen(name);
        if (n == 0 || n > 63) {
            h_errno = NO_RECOVERY;
            return -1;
        }
        *p++ = (unsigned char)n;
        memcpy(p, name, n);
        p += n;
        name += n;
        if (*name == '.')
            name++;
    }
    *p++ = 0;
    *p++ = (unsigned char)(type >> 8);
    *p++ = (unsigned char)type;
    *p++ = (unsigned char)(klass >> 8);
    *p++ = (unsigned char)klass;
    return (int)(p - buf);
}

int res_send(const unsigned char *msg, int msglen, unsigned char *answer, int anslen)
{
    int attempt, i, fd, got = -1;
    if (!(res.options & RES_INIT))
        res_init();
    fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    for (attempt = 0; attempt < res.retry && got < 0; attempt++) {
        for (i = 0; i < res.nscount && got < 0; i++) {
            struct sockaddr_in *ns = &res.nsaddr_list[i];
            int tries;
            if (sendto(fd, msg, (size_t)msglen, 0, (struct sockaddr *)ns, sizeof *ns) < 0)
                continue;
            for (tries = 0; tries < 8; tries++) {
                struct pollfd pfd;
                struct sockaddr_in from;
                socklen_t fl = sizeof from;
                ssize_t n;
                int rcode;
                pfd.fd = fd;
                pfd.events = POLLIN;
                pfd.revents = 0;
                if (poll(&pfd, 1, res.retrans * 1000) <= 0)
                    break;
                n = recvfrom(fd, answer, (size_t)anslen, 0, (struct sockaddr *)&from, &fl);
                if (n < 0)
                    break;
                if (n < 12 || answer[0] != msg[0] || answer[1] != msg[1] || !(answer[2] & 0x80) ||
                    from.sin_addr.s_addr != ns->sin_addr.s_addr)
                    continue;
                rcode = answer[3] & 15;
                if ((rcode == 2 || rcode == 4 || rcode == 5) && (i + 1 < res.nscount || attempt + 1 < res.retry))
                    break;
                got = (int)n;
                break;
            }
        }
    }
    close(fd);
    if (got < 0) {
        errno = ETIMEDOUT;
        h_errno = TRY_AGAIN;
    }
    return got;
}

int res_query(const char *name, int klass, int type, unsigned char *answer, int anslen)
{
    unsigned char q[300];
    int n = res_mkquery(0, name, klass, type, 0, 0, 0, q, sizeof q);
    int rcode;
    if (n < 0)
        return -1;
    n = res_send(q, n, answer, anslen);
    if (n < 0)
        return -1;
    rcode = answer[3] & 15;
    if (rcode == 3) {
        h_errno = HOST_NOT_FOUND;
        return -1;
    }
    if (rcode) {
        h_errno = rcode == 2 ? TRY_AGAIN : NO_RECOVERY;
        return -1;
    }
    if (!(answer[6] << 8 | answer[7])) {
        h_errno = NO_DATA;
        return -1;
    }
    return n;
}

int res_search(const char *name, int klass, int type, unsigned char *answer, int anslen)
{
    return res_query(name, klass, type, answer, anslen);
}

int dn_skipname(const unsigned char *src, const unsigned char *eom)
{
    const unsigned char *p = src;
    while (p < eom) {
        unsigned n = *p++;
        if (!n)
            return (int)(p - src);
        if ((n & 0xc0) == 0xc0)
            return p < eom ? (int)(p + 1 - src) : -1;
        if (n & 0xc0)
            return -1;
        p += n;
    }
    return -1;
}

int dn_expand(const unsigned char *msg, const unsigned char *eom, const unsigned char *src, char *dst, int dstsiz)
{
    const unsigned char *p = src;
    int len = -1, out = 0, hops = 0;
    for (;;) {
        unsigned n;
        if (p >= eom)
            return -1;
        n = *p++;
        if (!n)
            break;
        if ((n & 0xc0) == 0xc0) {
            if (p >= eom || ++hops > 64)
                return -1;
            if (len < 0)
                len = (int)(p + 1 - src);
            p = msg + ((n & 0x3f) << 8 | *p);
            continue;
        }
        if ((n & 0xc0) || p + n > eom || out + (int)n + 2 > dstsiz)
            return -1;
        if (out)
            dst[out++] = '.';
        memcpy(dst + out, p, n);
        out += (int)n;
        p += n;
    }
    if (dstsiz < 1)
        return -1;
    dst[out] = 0;
    return len < 0 ? (int)(p - src) : len;
}

/* Ask for records of TYPE for NAME.  A records go to addrs (at most max), the target of a CNAME or
   PTR record to text (256 bytes).  Returns the number of A records, or of PTR records; -1 on failure
   with h_errno set. */
static int dns_lookup(const char *name, int type, struct in_addr *addrs, int max, char *text)
{
    unsigned char *ans;
    const unsigned char *p, *eom;
    int n, qd, an, count = 0;
    ans = malloc(PKTSZ);
    if (!ans) {
        h_errno = NO_RECOVERY;
        return -1;
    }
    n = res_query(name, 1, type, ans, PKTSZ);
    if (n < 0) {
        free(ans);
        return -1;
    }
    eom = ans + n;
    qd = ans[4] << 8 | ans[5];
    an = ans[6] << 8 | ans[7];
    p = ans + 12;
    while (qd-- > 0) {
        int k = dn_skipname(p, eom);
        if (k < 0 || p + k + 4 > eom)
            goto bad;
        p += k + 4;
    }
    while (an-- > 0) {
        int k = dn_skipname(p, eom), rtype, rdlen;
        if (k < 0 || p + k + 10 > eom)
            break;
        p += k;
        rtype = p[0] << 8 | p[1];
        rdlen = p[8] << 8 | p[9];
        p += 10;
        if (p + rdlen > eom)
            break;
        if (rtype == T_A && type == T_A && rdlen == 4) {
            if (count < max)
                memcpy(&addrs[count++], p, 4);
        } else if (rtype == T_CNAME && type == T_A && text) {
            if (dn_expand(ans, eom, p, text, 256) < 0)
                text[0] = 0;
        } else if (rtype == T_PTR && type == T_PTR && text && !count) {
            if (dn_expand(ans, eom, p, text, 256) >= 0 && text[0])
                count++;
        }
        p += rdlen;
    }
    free(ans);
    if (!count) {
        h_errno = NO_DATA;
        return -1;
    }
    return count;
bad:
    free(ans);
    h_errno = NO_RECOVERY;
    return -1;
}

/* /etc/hosts: by name (addr filled in) or by address (name filled in, 256 bytes) */
static int hosts_lookup(char *name, struct in_addr *addr, int by_name)
{
    FILE *f = fopen("/etc/hosts", "r");
    char line[512];
    int found = 0;
    if (!f)
        return 0;
    while (!found && fgets(line, sizeof line, f)) {
        char *p = line, *tok;
        struct in_addr a;
        char *hash = strchr(line, '#');
        if (hash)
            *hash = 0;
        tok = strtok(p, " \t\r\n");
        if (!tok || !inet_aton(tok, &a))
            continue;
        if (!by_name) {
            tok = strtok(0, " \t\r\n");
            if (tok && a.s_addr == addr->s_addr && strlen(tok) < 256) {
                strcpy(name, tok);
                found = 1;
            }
            continue;
        }
        while ((tok = strtok(0, " \t\r\n")) != 0)
            if (!strcasecmp(tok, name)) {
                *addr = a;
                found = 1;
                break;
            }
    }
    fclose(f);
    return found;
}

/* all the addresses of a host name; canon (256 bytes) gets its official name */
static int name_to_addrs(const char *name, struct in_addr *addrs, int max, char *canon, int numeric_only)
{
    size_t len = strlen(name);
    int n;
    if (len == 0 || len > 255) {
        h_errno = HOST_NOT_FOUND;
        return -1;
    }
    strcpy(canon, name);
    if (inet_aton(name, &addrs[0]))
        return 1;
    if (numeric_only) {
        h_errno = HOST_NOT_FOUND;
        return -1;
    }
    if (canon[len - 1] == '.')
        canon[len - 1] = 0;
    if (hosts_lookup(canon, &addrs[0], 1))
        return 1;
    if (!strcasecmp(canon, "localhost")) {
        addrs[0].s_addr = htonl(INADDR_LOOPBACK);
        return 1;
    }
    {
        char target[256];
        target[0] = 0;
        n = dns_lookup(canon, T_A, addrs, max, target);
        if (n > 0 && target[0])
            strcpy(canon, target);
    }
    return n;
}

static int addr_to_name(struct in_addr a, char *name)
{
    char q[40];
    unsigned v = ntohl(a.s_addr);
    if (hosts_lookup(name, &a, 0))
        return 1;
    sprintf(q, "%u.%u.%u.%u.in-addr.arpa", v & 255, v >> 8 & 255, v >> 16 & 255, v >> 24);
    return dns_lookup(q, T_PTR, 0, 0, name) > 0;
}

static struct hostent host;
static struct in_addr host_addrs[MAXADDR];
static char *host_list[MAXADDR + 1], *host_none[1];
static char host_name[256];

static struct hostent *host_result(int n)
{
    int i;
    for (i = 0; i < n; i++)
        host_list[i] = (char *)&host_addrs[i];
    host_list[n] = 0;
    host.h_name = host_name;
    host.h_aliases = host_none;
    host.h_addrtype = AF_INET;
    host.h_length = 4;
    host.h_addr_list = host_list;
    return &host;
}

struct hostent *gethostbyname(const char *name)
{
    int n = name_to_addrs(name, host_addrs, MAXADDR, host_name, 0);
    return n > 0 ? host_result(n) : 0;
}

struct hostent *gethostbyname2(const char *name, int af)
{
    if (af != AF_INET) {
        h_errno = NO_DATA;
        return 0;
    }
    return gethostbyname(name);
}

struct hostent *gethostbyaddr(const void *addr, socklen_t len, int type)
{
    if (type != AF_INET || len < 4) {
        h_errno = NO_RECOVERY;
        return 0;
    }
    memcpy(&host_addrs[0], addr, 4);
    if (!addr_to_name(host_addrs[0], host_name)) {
        h_errno = HOST_NOT_FOUND;
        return 0;
    }
    return host_result(1);
}

void sethostent(int stayopen) { (void)stayopen; }
void endhostent(void) {}

const char *hstrerror(int err)
{
    switch (err) {
    case 0: return "Resolver Error 0 (no error)";
    case HOST_NOT_FOUND: return "Unknown host";
    case TRY_AGAIN: return "Host name lookup failure";
    case NO_RECOVERY: return "Unknown server error";
    case NO_DATA: return "No address associated with name";
    }
    return "Unknown resolver error";
}

void herror(const char *s) { fprintf(stderr, "%s: %s\n", s, hstrerror(h_errno)); }

/* ---- services and protocols: a built-in table ---- */

static const struct {
    const char *name;
    unsigned short port;
    char tcp, udp;
} services[] = {
    {"echo", 7, 1, 1}, {"discard", 9, 1, 1}, {"daytime", 13, 1, 1}, {"ftp-data", 20, 1, 0}, {"ftp", 21, 1, 0},
    {"ssh", 22, 1, 0}, {"telnet", 23, 1, 0}, {"smtp", 25, 1, 0}, {"time", 37, 1, 1}, {"domain", 53, 1, 1},
    {"bootps", 67, 0, 1}, {"bootpc", 68, 0, 1}, {"tftp", 69, 0, 1}, {"http", 80, 1, 0}, {"www", 80, 1, 0},
    {"pop3", 110, 1, 0}, {"sunrpc", 111, 1, 1}, {"ntp", 123, 0, 1}, {"imap", 143, 1, 0}, {"snmp", 161, 0, 1},
    {"https", 443, 1, 0}, {"syslog", 514, 0, 1}, {"printer", 515, 1, 0}, {"nfs", 2049, 1, 1}
};

static struct servent serv;
static char *serv_none[1];
static char serv_proto[4];

static struct servent *serv_result(int i, const char *proto)
{
    if (proto && strcmp(proto, "tcp") && strcmp(proto, "udp"))
        return 0;
    if (proto && !(proto[0] == 't' ? services[i].tcp : services[i].udp))
        return 0;
    strcpy(serv_proto, proto ? proto : services[i].tcp ? "tcp" : "udp");
    serv.s_name = (char *)services[i].name;
    serv.s_aliases = serv_none;
    serv.s_port = htons(services[i].port);
    serv.s_proto = serv_proto;
    return &serv;
}

struct servent *getservbyname(const char *name, const char *proto)
{
    int i;
    struct servent *s;
    for (i = 0; i < (int)(sizeof services / sizeof services[0]); i++)
        if (!strcmp(services[i].name, name) && (s = serv_result(i, proto)) != 0)
            return s;
    return 0;
}

struct servent *getservbyport(int port, const char *proto)
{
    int i;
    struct servent *s;
    for (i = 0; i < (int)(sizeof services / sizeof services[0]); i++)
        if (htons(services[i].port) == port && (s = serv_result(i, proto)) != 0)
            return s;
    return 0;
}

static const struct {
    const char *name;
    int number;
} protocols[] = {
    {"ip", 0}, {"icmp", 1}, {"igmp", 2}, {"tcp", 6}, {"udp", 17}, {"ipv6", 41}, {"gre", 47}, {"ipv6-icmp", 58},
    {"raw", 255}
};

static struct protoent *proto_result(int i)
{
    static struct protoent p;
    static char *none[1];
    p.p_name = (char *)protocols[i].name;
    p.p_aliases = none;
    p.p_proto = protocols[i].number;
    return &p;
}

struct protoent *getprotobyname(const char *name)
{
    int i;
    for (i = 0; i < (int)(sizeof protocols / sizeof protocols[0]); i++)
        if (!strcmp(protocols[i].name, name))
            return proto_result(i);
    return 0;
}

struct protoent *getprotobynumber(int proto)
{
    int i;
    for (i = 0; i < (int)(sizeof protocols / sizeof protocols[0]); i++)
        if (protocols[i].number == proto)
            return proto_result(i);
    return 0;
}

/* ---- getaddrinfo ---- */

struct ainode {
    struct addrinfo ai;
    struct sockaddr_in sa;
    char canon[256];
};

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res_out)
{
    struct in_addr addrs[MAXADDR];
    char canon[256];
    struct addrinfo *first = 0, **link = &first;
    int flags = hints ? hints->ai_flags : 0;
    int socktype = hints ? hints->ai_socktype : 0;
    int port = 0, n = 1, i;
    if (!node && !service)
        return EAI_NONAME;
    if (hints && hints->ai_family != AF_UNSPEC && hints->ai_family != AF_INET)
        return EAI_FAMILY;
    if (service) {
        char *e;
        port = (int)strtol(service, &e, 10);
        if (*e || e == service) {
            struct servent *s;
            if (flags & AI_NUMERICSERV)
                return EAI_NONAME;
            s = getservbyname(service, socktype == SOCK_DGRAM ? "udp" : socktype == SOCK_STREAM ? "tcp" : 0);
            if (!s)
                return EAI_SERVICE;
            port = ntohs((unsigned short)s->s_port);
        }
    }
    canon[0] = 0;
    if (node) {
        n = name_to_addrs(node, addrs, MAXADDR, canon, flags & AI_NUMERICHOST);
        if (n <= 0)
            return h_errno == TRY_AGAIN ? EAI_AGAIN : h_errno == NO_RECOVERY ? EAI_FAIL : EAI_NONAME;
    } else
        addrs[0].s_addr = htonl(flags & AI_PASSIVE ? INADDR_ANY : INADDR_LOOPBACK);
    for (i = 0; i < n; i++) {
        struct ainode *b = calloc(1, sizeof *b);
        if (!b) {
            freeaddrinfo(first);
            return EAI_MEMORY;
        }
        b->sa.sin_family = AF_INET;
        b->sa.sin_port = htons((unsigned short)port);
        b->sa.sin_addr = addrs[i];
        b->ai.ai_family = AF_INET;
        b->ai.ai_socktype = socktype ? socktype : SOCK_STREAM;
        b->ai.ai_protocol = hints && hints->ai_protocol ? hints->ai_protocol :
                            b->ai.ai_socktype == SOCK_STREAM ? IPPROTO_TCP :
                            b->ai.ai_socktype == SOCK_DGRAM ? IPPROTO_UDP : 0;
        b->ai.ai_addrlen = sizeof b->sa;
        b->ai.ai_addr = (struct sockaddr *)&b->sa;
        if (i == 0 && (flags & AI_CANONNAME)) {
            strcpy(b->canon, canon);
            b->ai.ai_canonname = b->canon;
        }
        *link = &b->ai;
        link = &b->ai.ai_next;
    }
    *res_out = first;
    return 0;
}

void freeaddrinfo(struct addrinfo *ai)
{
    while (ai) {
        struct addrinfo *next = ai->ai_next;
        free(ai);
        ai = next;
    }
}

int getnameinfo(const struct sockaddr *sa, socklen_t salen, char *host, socklen_t hostlen, char *serv_out,
                socklen_t servlen, int flags)
{
    const struct sockaddr_in *in = (const struct sockaddr_in *)sa;
    if (sa->sa_family != AF_INET || salen < sizeof *in)
        return EAI_FAMILY;
    if (host && hostlen) {
        char name[256];
        if (!(flags & NI_NUMERICHOST) && addr_to_name(in->sin_addr, name)) {
            if (strlen(name) >= hostlen)
                return EAI_OVERFLOW;
            strcpy(host, name);
        } else {
            if (flags & NI_NAMEREQD)
                return EAI_NONAME;
            if (!inet_ntop(AF_INET, &in->sin_addr, host, hostlen))
                return EAI_OVERFLOW;
        }
    }
    if (serv_out && servlen) {
        struct servent *s = 0;
        if (!(flags & NI_NUMERICSERV))
            s = getservbyport(in->sin_port, flags & NI_DGRAM ? "udp" : "tcp");
        if (s)
            snprintf(serv_out, servlen, "%s", s->s_name);
        else
            snprintf(serv_out, servlen, "%d", ntohs(in->sin_port));
    }
    return 0;
}

const char *gai_strerror(int err)
{
    switch (err) {
    case EAI_BADFLAGS: return "Bad value for ai_flags";
    case EAI_NONAME: return "Name or service not known";
    case EAI_AGAIN: return "Temporary failure in name resolution";
    case EAI_FAIL: return "Non-recoverable failure in name resolution";
    case EAI_FAMILY: return "ai_family not supported";
    case EAI_SOCKTYPE: return "ai_socktype not supported";
    case EAI_SERVICE: return "Servname not supported for ai_socktype";
    case EAI_MEMORY: return "Memory allocation failure";
    case EAI_SYSTEM: return "System error";
    case EAI_OVERFLOW: return "Argument buffer overflow";
    }
    return "Unknown error";
}
