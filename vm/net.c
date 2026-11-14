/* The network card (virtio-net) and the network behind it: the guest sits alone on 10.0.2.0/24 and this file
   plays every other machine. It answers ARP, hands out the address by DHCP, answers pings to the gateway,
   and turns the guest's TCP connections and UDP packets into ordinary sockets of the host, so no privilege
   is needed. 10.0.2.2 is the host itself (its loopback), 10.0.2.3 the host's name server, port 7 of the
   gateway echoes; -fwd lets programs on the host connect to a port of the guest.
   The link to the guest loses nothing, so TCP here is small: no congestion control, a fixed timer. */
#include "vm.h"
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#define IP_GW 0x0a000202UL
#define IP_DNS 0x0a000203UL
#define IP_GUEST 0x0a00020fUL
#define HDR 12 /* the virtio header before each frame */
#define MAXF 1514
#define NBACK 64
#define NTCP 64
#define NUDP 32
#define NFWD 8
#define BUF 65536U

enum { T_FREE, T_CONNECTING, T_SYNRCVD, T_SYNSENT, T_EST };
enum { FIN = 1, SYN = 2, RST = 4, PSH = 8, ACK = 16 };

typedef struct Tcp {
    int state, fd; /* fd < 0: the echo service */
    u32 rip;       /* the other end, as the guest sees it */
    u32 rport, gport;
    u32 snd_una, snd_nxt, rcv_nxt, snd_wnd, mss, adv;
    u8 *sbuf, *obuf; /* bytes for the guest from snd_una on; bytes of the guest the socket has not taken */
    u32 slen, olen;
    int host_eof, fin_sent, fin_acked, guest_fin, shut;
    u64 last;
} Tcp;

typedef struct Udp {
    int fd;
    u32 gport, real, virt; /* answers from address real are shown as coming from virt */
    u64 last;
} Udp;

static struct Net {
    Vm *vm;
    Virtio *dev;
    u8 gmac[6];
    u32 gip, ipid, iss, resolver;
    u8 pkt[MAXF + 64];
    u8 back[NBACK][MAXF];
    u32 backlen[NBACK];
    int bhead, nback;
    Tcp tcp[NTCP];
    Udp udp[NUDP];
    int nudp;
    int lfd[NFWD];
    u32 lport[NFWD];
    int nfwd, icmp;
} N;

static const u8 ourmac[6] = { 0x52, 0x55, 0x0a, 0x00, 0x02, 0x02 };

static u32 b16(const u8 *p) { return (u32)p[0] << 8 | p[1]; }
static u32 b32(const u8 *p) { return b16(p) << 16 | b16(p + 2); }

static void pb16(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 8);
    p[1] = (u8)v;
}

static void pb32(u8 *p, u32 v)
{
    pb16(p, v >> 16);
    pb16(p + 2, v);
}

static u32 sum16(const u8 *p, u32 n, u32 s)
{
    for (; n > 1; p += 2, n -= 2)
        s += b16(p);
    if (n)
        s += (u32)p[0] << 8;
    return s;
}

static u32 fold(u32 s)
{
    while (s >> 16)
        s = (s & 0xffff) + (s >> 16);
    return ~s & 0xffff;
}

/* ---- frames to the guest ---- */

static int deliver(const u8 *f, u32 len)
{
    static u8 tmp[HDR + MAXF];
    Iov iov[8];
    u16 head;
    u32 off = 0, tot = HDR + len, m;
    int k = vq_pop(N.vm, N.dev, 0, iov, 8, &head), i;
    if (k <= 0)
        return 0;
    memset(tmp, 0, HDR);
    tmp[10] = 1; /* one buffer */
    memcpy(tmp + HDR, f, len);
    for (i = 0; i < k && off < tot; i++) {
        if (!iov[i].w)
            continue;
        m = iov[i].len < tot - off ? iov[i].len : tot - off;
        memcpy(iov[i].p, tmp + off, m);
        off += m;
    }
    vq_push(N.vm, N.dev, 0, head, off);
    return 1;
}

static void to_guest(const u8 *f, u32 len)
{
    int i;
    if (len > MAXF)
        return;
    if (!N.nback && deliver(f, len))
        return;
    if (N.nback == NBACK)
        return;
    i = (N.bhead + N.nback++) % NBACK;
    memcpy(N.back[i], f, len);
    N.backlen[i] = len;
}

/* The payload is already at pkt + 34. */
static void ip_out(int proto, u32 src, u32 dst, u32 plen, int bcast)
{
    u8 *e = N.pkt, *ip = e + 14;
    if (bcast)
        memset(e, 0xff, 6);
    else
        memcpy(e, N.gmac, 6);
    memcpy(e + 6, ourmac, 6);
    e[12] = 8, e[13] = 0;
    ip[0] = 0x45, ip[1] = 0;
    pb16(ip + 2, 20 + plen);
    pb16(ip + 4, N.ipid++);
    pb16(ip + 6, 0x4000);
    ip[8] = 64, ip[9] = (u8)proto;
    pb16(ip + 10, 0);
    pb32(ip + 12, src);
    pb32(ip + 16, dst);
    pb16(ip + 10, fold(sum16(ip, 20, 0)));
    to_guest(e, 34 + plen);
}

static void udp_out(u32 src, u32 sport, u32 dst, u32 dport, const u8 *data, u32 len, int bcast)
{
    u8 *u = N.pkt + 34;
    if (len > 1472)
        return;
    pb16(u, sport);
    pb16(u + 2, dport);
    pb16(u + 4, 8 + len);
    pb16(u + 6, 0); /* no checksum */
    memmove(u + 8, data, len);
    ip_out(17, src, dst, 8 + len, bcast);
}

/* ---- sockets of the host ---- */

static void sa_set(struct sockaddr_in *sa, u32 ip, u32 port)
{
    memset(sa, 0, sizeof *sa);
    sa->sin_len = sizeof *sa;
    sa->sin_family = AF_INET;
    sa->sin_port = htons((uint16_t)port);
    sa->sin_addr.s_addr = htonl(ip);
}

static int sock(int type, int proto)
{
    int fd = socket(AF_INET, type, proto), one = 1;
    if (fd < 0)
        return -1;
    if (fd >= FD_SETSIZE) {
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, O_NONBLOCK);
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    return fd;
}

static u32 real_addr(u32 ip) { return ip == IP_GW ? 0x7f000001UL : ip == IP_DNS ? N.resolver : ip; }

/* ---- TCP ---- */

static u32 tcp_room(Tcp *c)
{
    u32 r = c->fd < 0 ? BUF - c->slen : BUF - c->olen;
    return r > 65535 ? 65535 : r;
}

static void tcp_seg(Tcp *c, int flags, u32 seq, const u8 *data, u32 len)
{
    u8 *t = N.pkt + 34;
    u32 hl = flags & SYN ? 24 : 20, s;
    pb16(t, c->rport);
    pb16(t + 2, c->gport);
    pb32(t + 4, seq);
    pb32(t + 8, flags & ACK ? c->rcv_nxt : 0);
    t[12] = (u8)(hl << 2);
    t[13] = (u8)flags;
    c->adv = tcp_room(c);
    pb16(t + 14, c->adv);
    pb32(t + 16, 0);
    if (flags & SYN) {
        t[20] = 2, t[21] = 4;
        pb16(t + 22, 1460);
    }
    if (len)
        memcpy(t + hl, data, len);
    s = sum16(t, hl + len, 0) + (c->rip >> 16) + (c->rip & 0xffff) + (N.gip >> 16) + (N.gip & 0xffff) + 6 + hl + len;
    pb16(t + 16, fold(s));
    ip_out(6, c->rip, N.gip, hl + len, 0);
}

static void tcp_free(Tcp *c)
{
    if (c->fd >= 0)
        close(c->fd);
    free(c->sbuf);
    free(c->obuf);
    memset(c, 0, sizeof *c);
}

static void tcp_abort(Tcp *c)
{
    tcp_seg(c, RST | ACK, c->snd_nxt, 0, 0);
    tcp_free(c);
}

static Tcp *tcp_new(void)
{
    int i;
    for (i = 0; i < NTCP; i++)
        if (N.tcp[i].state == T_FREE) {
            Tcp *c = &N.tcp[i];
            c->sbuf = (u8 *)malloc(BUF);
            c->obuf = (u8 *)malloc(BUF);
            c->mss = 536;
            c->fd = -1;
            c->last = now_usec();
            N.iss += 0x10000 + (u32)(c->last & 0xfff);
            c->snd_una = c->snd_nxt = N.iss;
            return c;
        }
    return 0;
}

/* Send what the window and the queue to the guest allow. */
static void tcp_output(Tcp *c)
{
    if (c->state != T_EST)
        return;
    for (;;) {
        u32 off = c->snd_nxt - c->snd_una, n;
        if (c->fin_sent || off >= c->slen || off >= c->snd_wnd || N.nback >= NBACK / 2)
            break;
        n = c->slen - off;
        if (n > c->mss)
            n = c->mss;
        if (n > c->snd_wnd - off)
            n = c->snd_wnd - off;
        if (!off)
            c->last = now_usec();
        tcp_seg(c, ACK | PSH, c->snd_nxt, c->sbuf + off, n);
        c->snd_nxt += n;
    }
    if (c->host_eof && !c->fin_sent && c->snd_nxt - c->snd_una == c->slen && N.nback < NBACK / 2) {
        tcp_seg(c, FIN | ACK, c->snd_nxt, 0, 0);
        c->snd_nxt++;
        c->fin_sent = 1;
        c->last = now_usec();
    }
}

/* Give the socket what the guest sent; 0 when the connection is gone. */
static int tcp_flush(Tcp *c)
{
    while (c->olen && c->fd >= 0) {
        long r = send(c->fd, c->obuf, c->olen, 0);
        if (r < 0) {
            if (errno == EAGAIN || errno == EINTR)
                break;
            tcp_abort(c);
            return 0;
        }
        memmove(c->obuf, c->obuf + r, c->olen - (u32)r);
        c->olen -= (u32)r;
    }
    if (c->guest_fin && !c->olen && !c->shut) {
        c->shut = 1;
        if (c->fd >= 0)
            shutdown(c->fd, SHUT_WR);
        else
            c->host_eof = 1;
    }
    if (c->fin_acked && c->guest_fin && !c->olen) {
        tcp_free(c);
        return 0;
    }
    return 1;
}

static void tcp_in(u32 dst, const u8 *t, u32 len)
{
    u32 sport, dport, seq, ack, hl, flags, win, dlen, i;
    const u8 *data;
    Tcp *c = 0;
    int need_ack = 0;
    if (len < 20)
        return;
    sport = b16(t), dport = b16(t + 2), seq = b32(t + 4), ack = b32(t + 8);
    hl = (u32)(t[12] >> 4) * 4, flags = t[13], win = b16(t + 14);
    if (hl < 20 || hl > len)
        return;
    data = t + hl, dlen = len - hl;
    for (i = 0; i < NTCP; i++)
        if (N.tcp[i].state != T_FREE && N.tcp[i].gport == sport && N.tcp[i].rport == dport && N.tcp[i].rip == dst)
            c = &N.tcp[i];
    if (!c) {
        struct sockaddr_in sa;
        Tcp tmp;
        if (flags & RST)
            return;
        if ((flags & (SYN | ACK)) == SYN && (c = tcp_new()) != 0) {
            c->rip = dst, c->rport = dport, c->gport = sport;
            c->rcv_nxt = seq + 1;
            c->snd_wnd = win;
            for (i = 20; i + 3 < hl && t[i]; i += t[i] == 1 ? 1 : t[i + 1] < 2 ? 2 : t[i + 1])
                if (t[i] == 2 && t[i + 1] == 4)
                    c->mss = b16(t + i + 2);
            if (c->mss > 1460)
                c->mss = 1460;
            if (dst == IP_GW && dport == 7) { /* echo */
                c->state = T_SYNRCVD;
                tcp_seg(c, SYN | ACK, c->snd_nxt++, 0, 0);
                return;
            }
            c->fd = real_addr(dst) ? sock(SOCK_STREAM, 0) : -1;
            if (c->fd >= 0) {
                int one = 1;
                setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                sa_set(&sa, real_addr(dst), dport);
                if (connect(c->fd, (struct sockaddr *)&sa, sizeof sa) == 0 || errno == EINPROGRESS) {
                    c->state = T_CONNECTING;
                    return;
                }
            }
            c->state = T_EST;
            c->snd_nxt = 0;
            tcp_abort(c);
            return;
        }
        memset(&tmp, 0, sizeof tmp); /* nothing listens here */
        tmp.rip = dst, tmp.rport = dport, tmp.gport = sport;
        tmp.rcv_nxt = seq + dlen + ((flags & SYN) != 0) + ((flags & FIN) != 0);
        tcp_seg(&tmp, RST | ACK, flags & ACK ? ack : 0, 0, 0);
        return;
    }
    if (flags & RST) {
        tcp_free(c);
        return;
    }
    if (c->state == T_CONNECTING)
        return;
    if (c->state == T_SYNSENT) {
        if ((flags & (SYN | ACK)) != (SYN | ACK) || ack != c->snd_nxt)
            return;
        for (i = 20; i + 3 < hl && t[i]; i += t[i] == 1 ? 1 : t[i + 1] < 2 ? 2 : t[i + 1])
            if (t[i] == 2 && t[i + 1] == 4)
                c->mss = b16(t + i + 2);
        if (c->mss > 1460)
            c->mss = 1460;
        c->rcv_nxt = seq + 1;
        c->snd_una = ack;
        c->snd_wnd = win;
        c->state = T_EST;
        tcp_seg(c, ACK, c->snd_nxt, 0, 0);
        tcp_output(c);
        return;
    }
    if (flags & SYN) { /* our answer was lost, or is late */
        if (c->state == T_SYNRCVD)
            tcp_seg(c, SYN | ACK, c->snd_nxt - 1, 0, 0);
        return;
    }
    if (!(flags & ACK))
        return;
    if (c->state == T_SYNRCVD) {
        if (ack != c->snd_nxt)
            return;
        c->snd_una = ack;
        c->state = T_EST;
    }
    c->snd_wnd = win;
    if (ack - c->snd_una <= c->snd_nxt - c->snd_una && ack != c->snd_una) {
        u32 n = ack - c->snd_una;
        if (c->fin_sent && ack == c->snd_nxt) {
            c->fin_acked = 1;
            n--;
        }
        if (n > c->slen)
            n = c->slen;
        memmove(c->sbuf, c->sbuf + n, c->slen - n);
        c->slen -= n;
        c->snd_una = ack;
        c->last = now_usec();
    }
    if (dlen || (flags & FIN)) {
        u32 skip = c->rcv_nxt - seq; /* bytes we already have */
        need_ack = 1;
        if (skip <= dlen && !c->guest_fin) {
            u32 n = dlen - skip, room = c->fd < 0 ? BUF - c->slen : BUF - c->olen;
            if (n > room)
                n = room;
            if (c->fd < 0) {
                memcpy(c->sbuf + c->slen, data + skip, n);
                c->slen += n;
            } else {
                memcpy(c->obuf + c->olen, data + skip, n);
                c->olen += n;
            }
            c->rcv_nxt += n;
            if ((flags & FIN) && n == dlen - skip) {
                c->guest_fin = 1;
                c->rcv_nxt++;
            }
        }
    }
    if (!tcp_flush(c))
        return;
    if (need_ack || tcp_room(c) >= c->adv + BUF / 4)
        tcp_seg(c, ACK, c->snd_nxt, 0, 0);
    tcp_output(c);
}

/* ---- UDP, DHCP, ICMP, ARP ---- */

static void dhcp_in(const u8 *b, u32 len)
{
    static u8 r[300];
    u32 i, type = 0;
    u8 *o;
    if (len < 240 || b[0] != 1 || b32(b + 236) != 0x63825363UL)
        return;
    for (i = 240; i + 1 < len && b[i] != 255; i += b[i] ? 2 + b[i + 1] : 1)
        if (b[i] == 53 && i + 2 < len)
            type = b[i + 2];
    if (type != 1 && type != 3)
        return;
    memset(r, 0, sizeof r);
    r[0] = 2, r[1] = 1, r[2] = 6;
    memcpy(r + 4, b + 4, 4);   /* xid */
    memcpy(r + 10, b + 10, 2); /* flags */
    pb32(r + 16, IP_GUEST);
    pb32(r + 20, IP_GW);
    memcpy(r + 28, b + 28, 16);
    pb32(r + 236, 0x63825363UL);
    o = r + 240;
    *o++ = 53, *o++ = 1, *o++ = type == 1 ? 2 : 5;
    *o++ = 54, *o++ = 4, pb32(o, IP_GW), o += 4;
    *o++ = 51, *o++ = 4, pb32(o, 86400), o += 4;
    *o++ = 1, *o++ = 4, pb32(o, 0xffffff00UL), o += 4;
    *o++ = 3, *o++ = 4, pb32(o, IP_GW), o += 4;
    *o++ = 6, *o++ = 4, pb32(o, IP_DNS), o += 4;
    *o++ = 255;
    udp_out(IP_GW, 67, 0xffffffffUL, 68, r, sizeof r, 1);
}

static void udp_in(u32 dst, const u8 *u, u32 len)
{
    u32 sport, dport, real;
    struct sockaddr_in sa;
    Udp *f = 0;
    int i;
    if (len < 8 || b16(u + 4) < 8 || b16(u + 4) > len)
        return;
    sport = b16(u), dport = b16(u + 2);
    len = b16(u + 4) - 8;
    u += 8;
    if (dport == 67) {
        dhcp_in(u, len);
        return;
    }
    if (dst == IP_GW && dport == 7) {
        udp_out(dst, dport, N.gip, sport, u, len, 0);
        return;
    }
    real = real_addr(dst);
    if (!real || (dst & 0xff) == 0xff || dst >> 28 == 14)
        return;
    for (i = 0; i < N.nudp; i++)
        if (N.udp[i].gport == sport)
            f = &N.udp[i];
    if (!f) {
        int fd = sock(SOCK_DGRAM, 0);
        if (fd < 0)
            return;
        if (N.nudp == NUDP) { /* the one unused for longest goes */
            int old = 0;
            for (i = 1; i < NUDP; i++)
                if (N.udp[i].last < N.udp[old].last)
                    old = i;
            close(N.udp[old].fd);
            N.udp[old] = N.udp[--N.nudp];
        }
        f = &N.udp[N.nudp++];
        f->fd = fd;
        f->gport = sport;
    }
    f->last = now_usec();
    f->real = real, f->virt = dst;
    sa_set(&sa, real, dport);
    sendto(f->fd, u, len, 0, (struct sockaddr *)&sa, sizeof sa);
}

static void icmp_in(u32 dst, const u8 *m, u32 len)
{
    struct sockaddr_in sa;
    if (len < 8 || len > 1472 || m[0] != 8)
        return;
    if (dst == IP_GW || dst == IP_DNS) {
        u8 *r = N.pkt + 34;
        memmove(r, m, len);
        r[0] = 0;
        pb16(r + 2, 0);
        pb16(r + 2, fold(sum16(r, len, 0)));
        ip_out(1, dst, N.gip, len, 0);
        return;
    }
    if (N.icmp < 0)
        N.icmp = sock(SOCK_DGRAM, IPPROTO_ICMP);
    if (N.icmp < 0)
        return;
    sa_set(&sa, dst, 0);
    sendto(N.icmp, m, len, 0, (struct sockaddr *)&sa, sizeof sa);
}

static void arp_in(const u8 *a, u32 len)
{
    u8 *e = N.pkt, *r = e + 14;
    u32 target;
    if (len < 28 || b16(a + 6) != 1)
        return;
    target = b32(a + 24);
    if (b32(a + 14) && b32(a + 14) >> 8 == IP_GW >> 8)
        N.gip = b32(a + 14);
    if (target >> 8 != IP_GW >> 8 || target == N.gip)
        return;
    memcpy(e, N.gmac, 6);
    memcpy(e + 6, ourmac, 6);
    e[12] = 8, e[13] = 6;
    memcpy(r, a, 8);
    pb16(r + 6, 2);
    memcpy(r + 8, ourmac, 6);
    pb32(r + 14, target);
    memcpy(r + 18, a + 8, 10);
    to_guest(e, 42);
}

static void net_in(const u8 *f, u32 len)
{
    const u8 *ip = f + 14;
    u32 hl, tot, dst;
    if (len < 14)
        return;
    memcpy(N.gmac, f + 6, 6);
    if (b16(f + 12) == 0x0806) {
        arp_in(ip, len - 14);
        return;
    }
    if (b16(f + 12) != 0x0800 || len < 34 || ip[0] >> 4 != 4)
        return;
    hl = (u32)(ip[0] & 15) * 4, tot = b16(ip + 2);
    if (hl < 20 || tot < hl || tot > len - 14 || (b16(ip + 6) & 0x3fff)) /* no fragments */
        return;
    dst = b32(ip + 16);
    if (ip[9] == 6)
        tcp_in(dst, ip + hl, tot - hl);
    else if (ip[9] == 17)
        udp_in(dst, ip + hl, tot - hl);
    else if (ip[9] == 1)
        icmp_in(dst, ip + hl, tot - hl);
}

static void net_notify(Vm *vm, Virtio *d, int qi)
{
    static u8 frame[HDR + MAXF + 2];
    Iov iov[8];
    u16 head;
    int k, i;
    if (qi == 0) { /* room for frames again */
        while (N.nback && deliver(N.back[N.bhead], N.backlen[N.bhead])) {
            N.bhead = (N.bhead + 1) % NBACK;
            N.nback--;
        }
        for (i = 0; i < NTCP; i++)
            tcp_output(&N.tcp[i]);
        return;
    }
    while ((k = vq_pop(vm, d, 1, iov, 8, &head)) > 0) {
        u32 n = 0;
        for (i = 0; i < k; i++)
            if (!iov[i].w && n + iov[i].len <= sizeof frame) {
                memcpy(frame + n, iov[i].p, iov[i].len);
                n += iov[i].len;
            }
        vq_push(vm, d, 1, head, 0);
        if (n > HDR)
            net_in(frame + HDR, n - HDR);
    }
}

/* ---- the host's side ---- */

static int fdsets(fd_set *r, fd_set *w)
{
    int i, max = -1;
#define WANT(set, fd) (FD_SET(fd, set), max = (fd) > max ? (fd) : max)
    for (i = 0; i < NTCP; i++) {
        Tcp *c = &N.tcp[i];
        if (c->state == T_FREE || c->fd < 0)
            continue;
        if (c->state == T_CONNECTING || c->olen)
            WANT(w, c->fd);
        if (c->state == T_EST && !c->host_eof && c->slen < BUF)
            WANT(r, c->fd);
    }
    for (i = 0; i < N.nudp; i++)
        WANT(r, N.udp[i].fd);
    for (i = 0; i < N.nfwd; i++)
        WANT(r, N.lfd[i]);
    if (N.icmp >= 0)
        WANT(r, N.icmp);
#undef WANT
    return max + 1;
}

/* For the wait of the whole machine: the descriptors this file waits on, added to r and w. */
int net_fdset(Vm *vm, fd_set *r, fd_set *w) { return vm->net ? fdsets(r, w) : 0; }

void net_poll(Vm *vm)
{
    static u8 buf[2048];
    fd_set r, w;
    struct timeval tv;
    struct sockaddr_in sa;
    socklen_t sl;
    u64 now;
    int i, n;
    if (!vm->net)
        return;
    FD_ZERO(&r);
    FD_ZERO(&w);
    n = fdsets(&r, &w);
    tv.tv_sec = 0, tv.tv_usec = 0;
    if (n && select(n, &r, &w, 0, &tv) < 0) {
        FD_ZERO(&r);
        FD_ZERO(&w);
    }
    now = now_usec();
    for (i = 0; i < NTCP; i++) {
        Tcp *c = &N.tcp[i];
        if (c->state == T_FREE)
            continue;
        if (c->fd >= 0 && n && c->state == T_CONNECTING && FD_ISSET(c->fd, &w)) {
            int err = 0;
            sl = sizeof err;
            getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &sl);
            if (err) {
                c->snd_nxt = 0;
                tcp_abort(c);
                continue;
            }
            c->state = T_SYNRCVD;
            c->last = now;
            tcp_seg(c, SYN | ACK, c->snd_nxt++, 0, 0);
            continue;
        }
        if (c->fd >= 0 && n && c->olen && FD_ISSET(c->fd, &w)) {
            if (!tcp_flush(c))
                continue;
            if (tcp_room(c) >= c->adv + BUF / 4)
                tcp_seg(c, ACK, c->snd_nxt, 0, 0);
        }
        if (c->fd >= 0 && n && c->state == T_EST && !c->host_eof && c->slen < BUF && FD_ISSET(c->fd, &r)) {
            long k = recv(c->fd, c->sbuf + c->slen, BUF - c->slen, 0);
            if (k > 0)
                c->slen += (u32)k;
            else if (k == 0)
                c->host_eof = 1;
            else if (errno != EAGAIN && errno != EINTR) {
                tcp_abort(c);
                continue;
            }
        }
        if (c->snd_nxt != c->snd_una && now - c->last > 1000000) { /* unanswered for a second: again */
            c->last = now;
            if (c->state == T_SYNSENT)
                tcp_seg(c, SYN, c->snd_nxt - 1, 0, 0);
            else if (c->state == T_SYNRCVD)
                tcp_seg(c, SYN | ACK, c->snd_nxt - 1, 0, 0);
            else if (c->state == T_EST) {
                if (c->fin_sent && !c->fin_acked) {
                    c->fin_sent = 0;
                    c->snd_nxt--;
                }
                c->snd_nxt = c->snd_una;
            }
        }
        tcp_output(c);
    }
    for (i = N.nudp - 1; i >= 0; i--)
        if (now - N.udp[i].last > 60000000UL) { /* a minute of silence ends a UDP exchange */
            if (n)
                FD_CLR(N.udp[i].fd, &r);
            close(N.udp[i].fd);
            N.udp[i] = N.udp[--N.nudp];
        }
    if (!n)
        return;
    for (i = 0; i < N.nudp; i++)
        if (FD_ISSET(N.udp[i].fd, &r)) {
            long k;
            sl = sizeof sa;
            k = recvfrom(N.udp[i].fd, buf, sizeof buf, 0, (struct sockaddr *)&sa, &sl);
            if (k >= 0) {
                u32 from = ntohl(sa.sin_addr.s_addr);
                N.udp[i].last = now;
                udp_out(from == N.udp[i].real ? N.udp[i].virt : from, ntohs(sa.sin_port), N.gip, N.udp[i].gport, buf,
                        (u32)k, 0);
            }
        }
    for (i = 0; i < N.nfwd; i++)
        if (FD_ISSET(N.lfd[i], &r)) {
            int fd, one = 1;
            Tcp *c;
            sl = sizeof sa;
            fd = accept(N.lfd[i], (struct sockaddr *)&sa, &sl);
            if (fd < 0)
                continue;
            if (fd >= FD_SETSIZE || !(c = tcp_new())) {
                close(fd);
                continue;
            }
            fcntl(fd, F_SETFL, O_NONBLOCK);
            setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            c->fd = fd;
            c->rip = IP_GW, c->rport = ntohs(sa.sin_port), c->gport = N.lport[i];
            c->state = T_SYNSENT;
            tcp_seg(c, SYN, c->snd_nxt++, 0, 0);
        }
    if (N.icmp >= 0 && FD_ISSET(N.icmp, &r)) { /* the answer comes with its IP header */
        long k = recv(N.icmp, buf, sizeof buf, 0);
        u32 hl = k > 0 ? (u32)(buf[0] & 15) * 4 : 0;
        if (k >= 28 && hl >= 20 && (u32)k >= hl + 8 && (u32)k - hl <= 1472 && buf[hl] == 0) {
            u32 from = b32(buf + 12);
            memmove(N.pkt + 34, buf + hl, (u32)k - hl);
            ip_out(1, from, N.gip, (u32)k - hl, 0);
        }
    }
}

/* Anything that could wake a sleeping guest? */
int net_active(Vm *vm)
{
    int i;
    if (!vm->net)
        return 0;
    if (N.nudp || N.nfwd || N.icmp >= 0)
        return 1;
    for (i = 0; i < NTCP; i++)
        if (N.tcp[i].state != T_FREE)
            return 1;
    return 0;
}

int net_forward(Vm *vm, u32 hport, u32 gport)
{
    struct sockaddr_in sa;
    int fd, one = 1;
    (void)vm;
    if (N.nfwd == NFWD || (fd = sock(SOCK_STREAM, 0)) < 0)
        return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sa_set(&sa, 0x7f000001UL, hport); /* for this machine only */
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(fd, 16) < 0) {
        close(fd);
        return -1;
    }
    N.lfd[N.nfwd] = fd;
    N.lport[N.nfwd++] = gport;
    return 0;
}

void net_add(Vm *vm)
{
    static const u8 mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
    FILE *f = fopen("/etc/resolv.conf", "r");
    char line[256];
    N.vm = vm;
    N.gip = IP_GUEST;
    N.icmp = -1;
    N.iss = (u32)now_usec();
    memcpy(N.gmac, mac, 6);
    while (f && fgets(line, sizeof line, f)) {
        unsigned a, b, c, d;
        if (!N.resolver && sscanf(line, "nameserver %u.%u.%u.%u", &a, &b, &c, &d) == 4)
            N.resolver = a << 24 | b << 16 | c << 8 | d;
    }
    if (f)
        fclose(f);
    N.dev = virtio_add(vm, 1, (u64)1 << 5, 2, net_notify);
    memcpy(N.dev->config, mac, 6);
    vm->net = 1;
}
