/* virtio over MMIO (version 2 of the transport, split queues), and the simple devices: a disk backed by
   a file, an entropy source, a console. The network card is in net.c. */
#include "vm.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#define F_VERSION_1 ((u64)1 << 32)

static u8 *gp(Vm *vm, u64 gpa, u64 len)
{
    if (gpa < RAM_BASE || gpa - RAM_BASE > vm->ram_size || len > vm->ram_size - (gpa - RAM_BASE))
        return 0;
    return vm->ram + (gpa - RAM_BASE);
}

static u32 g16(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8; }
static u32 g32(const u8 *p) { return g16(p) | g16(p + 2) << 16; }
static u64 g64(const u8 *p) { return g32(p) | (u64)g32(p + 4) << 32; }

static void p16(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}

static void p32(u8 *p, u32 v)
{
    p16(p, v);
    p16(p + 2, v >> 16);
}

Virtio *virtio_add(Vm *vm, u32 dev_id, u64 features, int nq, void (*notify)(Vm *, Virtio *, int))
{
    Virtio *d;
    int i;
    for (i = 0; i < NVIRTIO && vm->virtio[i]; i++)
        ;
    if (i == NVIRTIO) {
        fprintf(stderr, "mvm: too many devices\n");
        exit(2);
    }
    d = (Virtio *)calloc(1, sizeof *d);
    d->slot = i;
    d->dev_id = dev_id;
    d->features = features | F_VERSION_1;
    d->nq = nq;
    d->notify = notify;
    vm->virtio[i] = d;
    return d;
}

static void virtio_irq(Vm *vm, Virtio *d) { gic_set_level(vm, 0, IRQ_VIRTIO + d->slot, d->isr != 0); }

int vq_pop(Vm *vm, Virtio *d, int qi, Iov *iov, int max, u16 *head)
{
    VQ *q = &d->q[qi];
    u8 *avail, *desc;
    u32 i, n = 0;
    if (!q->ready || !q->num)
        return 0;
    avail = gp(vm, q->avail, 4 + 2 * (u64)q->num);
    desc = gp(vm, q->desc, 16 * (u64)q->num);
    if (!avail || !desc || q->last == (u16)g16(avail + 2))
        return 0;
    i = g16(avail + 4 + 2 * (q->last % q->num));
    q->last++;
    *head = (u16)i;
    for (;;) {
        u8 *e;
        u32 flags;
        if (i >= q->num || n == (u32)max || n > q->num)
            return 0; /* a chain we cannot follow: drop it */
        e = desc + 16 * i;
        flags = g16(e + 12);
        iov[n].len = g32(e + 8);
        iov[n].p = gp(vm, g64(e), iov[n].len);
        iov[n].w = (flags & 2) != 0;
        if (!iov[n].p)
            return 0;
        n++;
        if (!(flags & 1))
            break;
        i = g16(e + 14);
    }
    return (int)n;
}

void vq_push(Vm *vm, Virtio *d, int qi, u16 head, u32 written)
{
    VQ *q = &d->q[qi];
    u8 *used = gp(vm, q->used, 4 + 8 * (u64)q->num);
    u32 idx;
    if (!used)
        return;
    idx = g16(used + 2);
    p32(used + 4 + 8 * (idx % q->num), head);
    p32(used + 8 + 8 * (idx % q->num), written);
    p16(used + 2, idx + 1);
    d->isr |= 1;
    virtio_irq(vm, d);
}

u64 virtio_read(Vm *vm, Virtio *d, u32 off)
{
    VQ *q = &d->q[d->qsel < 3 ? d->qsel : 0];
    (void)vm;
    switch (off) {
    case 0x000: return 0x74726976; /* "virt" */
    case 0x004: return 2;
    case 0x008: return d->dev_id;
    case 0x00c: return 0x006d766d; /* "mvm" */
    case 0x010: return d->fsel == 0 ? (u32)d->features : d->fsel == 1 ? (u32)(d->features >> 32) : 0;
    case 0x034: return d->qsel < (u32)d->nq ? 256 : 0;
    case 0x044: return q->ready;
    case 0x060: return d->isr;
    case 0x070: return d->status;
    case 0x0fc: return 0;
    case 0x0b0:
    case 0x0b4:
    case 0x0b8:
    case 0x0bc: return 0xffffffffUL; /* no shared memory regions */
    }
    if (off >= 0x100 && off < 0x100 + sizeof d->config)
        return g32(d->config + (off - 0x100));
    return 0;
}

void virtio_write(Vm *vm, Virtio *d, u32 off, u32 v)
{
    VQ *q = &d->q[d->qsel < 3 ? d->qsel : 0];
    switch (off) {
    case 0x014: d->fsel = v; break;
    case 0x020:
        if (d->dsel == 0)
            d->drv_features = (d->drv_features & ~(u64)0xffffffffUL) | v;
        else if (d->dsel == 1)
            d->drv_features = (d->drv_features & 0xffffffffUL) | (u64)v << 32;
        break;
    case 0x024: d->dsel = v; break;
    case 0x030: d->qsel = v; break;
    case 0x038: q->num = v <= 256 ? v : 0; break;
    case 0x044: q->ready = v & 1; break;
    case 0x050:
        if (v < (u32)d->nq) /* the entropy driver asks before it says it is ready */
            d->notify(vm, d, (int)v);
        break;
    case 0x064:
        d->isr &= ~v;
        virtio_irq(vm, d);
        break;
    case 0x070:
        d->status = v;
        if (!v) { /* reset */
            memset(d->q, 0, sizeof d->q);
            d->isr = 0;
            d->drv_features = 0;
            virtio_irq(vm, d);
        }
        break;
    case 0x080: q->desc = (q->desc & ~(u64)0xffffffffUL) | v; break;
    case 0x084: q->desc = (q->desc & 0xffffffffUL) | (u64)v << 32; break;
    case 0x090: q->avail = (q->avail & ~(u64)0xffffffffUL) | v; break;
    case 0x094: q->avail = (q->avail & 0xffffffffUL) | (u64)v << 32; break;
    case 0x0a0: q->used = (q->used & ~(u64)0xffffffffUL) | v; break;
    case 0x0a4: q->used = (q->used & 0xffffffffUL) | (u64)v << 32; break;
    }
}

/* ---- disk ---- */

typedef struct Blk {
    int fd, ro;
} Blk;

static void blk_notify(Vm *vm, Virtio *d, int qi)
{
    Blk *b = (Blk *)d->priv;
    Iov iov[132];
    u16 head;
    int n, i;
    while ((n = vq_pop(vm, d, qi, iov, 132, &head)) > 0) {
        u32 type, done = 0;
        u64 off;
        u8 status = 0;
        if (n < 2 || iov[0].len < 16 || !iov[n - 1].w || iov[n - 1].len < 1) {
            vq_push(vm, d, qi, head, 0);
            continue;
        }
        type = g32(iov[0].p);
        off = g64(iov[0].p + 8) * 512;
        for (i = 1; i < n - 1 && !status; i++) {
            long r;
            if (type == 0 && iov[i].w) {
                r = pread(b->fd, iov[i].p, iov[i].len, (off_t)off);
                if (r < 0)
                    status = 1;
                else if ((u32)r < iov[i].len)
                    memset(iov[i].p + r, 0, iov[i].len - (u32)r);
                done += iov[i].len;
            } else if (type == 1 && !iov[i].w && !b->ro) {
                r = pwrite(b->fd, iov[i].p, iov[i].len, (off_t)off);
                if (r != (long)iov[i].len)
                    status = 1;
            } else if (type == 8 && iov[i].w) {
                memset(iov[i].p, 0, iov[i].len);
                memcpy(iov[i].p, "mvm-disk", iov[i].len < 8 ? iov[i].len : 8);
                done += iov[i].len;
            } else
                status = type == 0 || type == 1 ? 1 : 2;
            off += iov[i].len;
        }
        if (type == 4)
            fsync(b->fd);
        else if (type != 0 && type != 1 && type != 8)
            status = 2;
        iov[n - 1].p[0] = status;
        vq_push(vm, d, qi, head, done + 1);
    }
}

int blk_add(Vm *vm, const char *path, int readonly)
{
    Blk *b = (Blk *)calloc(1, sizeof *b);
    Virtio *d;
    struct stat st;
    u64 sectors;
    b->ro = readonly;
    b->fd = open(path, readonly ? O_RDONLY : O_RDWR);
    if (b->fd < 0 || fstat(b->fd, &st) < 0)
        return -1;
    sectors = (u64)st.st_size / 512;
    d = virtio_add(vm, 2, (u64)1 << 2 | (u64)1 << 9 | (readonly ? (u64)1 << 5 : 0), 1, blk_notify);
    d->priv = b;
    p32(d->config, (u32)sectors);
    p32(d->config + 4, (u32)(sectors >> 32));
    p32(d->config + 12, 128); /* segments in one request */
    return 0;
}

/* ---- entropy ---- */

static void rng_notify(Vm *vm, Virtio *d, int qi)
{
    int fd = *(int *)d->priv, n, i;
    Iov iov[8];
    u16 head;
    while ((n = vq_pop(vm, d, qi, iov, 8, &head)) > 0) {
        u32 done = 0;
        for (i = 0; i < n; i++)
            if (iov[i].w) {
                long r = read(fd, iov[i].p, iov[i].len);
                if (r > 0)
                    done += (u32)r;
            }
        vq_push(vm, d, qi, head, done);
    }
}

int rng_add(Vm *vm)
{
    int *fd = (int *)malloc(sizeof *fd);
    *fd = open("/dev/urandom", O_RDONLY);
    if (*fd < 0)
        return -1;
    virtio_add(vm, 4, 0, 1, rng_notify)->priv = fd;
    return 0;
}

/* ---- console: queue 0 carries the terminal's input to the guest, queue 1 its output back ---- */

typedef struct Vcon {
    u8 in[256];
    int nin;
} Vcon;

static void vcon_feed(Vm *vm, Virtio *d)
{
    Vcon *c = (Vcon *)d->priv;
    Iov iov[4];
    u16 head;
    while (c->nin && vq_pop(vm, d, 0, iov, 4, &head) > 0) {
        u32 k = iov[0].len < (u32)c->nin ? iov[0].len : (u32)c->nin;
        if (!iov[0].w)
            k = 0;
        memcpy(iov[0].p, c->in, k);
        memmove(c->in, c->in + k, (size_t)c->nin - k);
        c->nin -= (int)k;
        vq_push(vm, d, 0, head, k);
        if (!k)
            break;
    }
}

static void vcon_notify(Vm *vm, Virtio *d, int qi)
{
    Iov iov[16];
    u16 head;
    int n, i;
    u32 k;
    if (qi == 0) {
        vcon_feed(vm, d);
        return;
    }
    while ((n = vq_pop(vm, d, 1, iov, 16, &head)) > 0) {
        for (i = 0; i < n; i++)
            if (!iov[i].w)
                for (k = 0; k < iov[i].len; k++)
                    con_out(vm, iov[i].p[k]);
        vq_push(vm, d, 1, head, 0);
    }
}

void vcon_add(Vm *vm)
{
    vm->vcon = virtio_add(vm, 3, 0, 2, vcon_notify);
    vm->vcon->priv = calloc(1, sizeof(Vcon));
}

int vcon_room(Vm *vm) { return (int)sizeof ((Vcon *)0)->in - ((Vcon *)vm->vcon->priv)->nin; }

void vcon_input(Vm *vm, const u8 *buf, int n)
{
    Vcon *c = (Vcon *)vm->vcon->priv;
    memcpy(c->in + c->nin, buf, (size_t)n);
    c->nin += n;
    vcon_feed(vm, vm->vcon);
}
