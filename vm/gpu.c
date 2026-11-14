/* The screen (virtio-gpu, two-dimensional), the keyboard and the pointer (virtio-input). The guest keeps
   its pictures ("resources") in its own memory, says which one the screen shows, and tells when a part has
   changed; this file copies that part into vm->scr, which a window (gui.c) or a screenshot shows.
   Input comes from the window or, for tests, from a script. */
#include "vm.h"

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

/* ---- the graphics card ---- */

typedef struct Res {
    u32 id, w, h, fmt;
    u32 *pix; /* the host's copy, 0x00RRGGBB */
    u8 **mem; /* the guest's pages of it */
    u32 *len;
    u32 nmem;
} Res;

#define NRES 64
static Res res[NRES];
static u32 scanout; /* the resource on the screen */

static Res *res_find(u32 id)
{
    int i;
    for (i = 0; i < NRES; i++)
        if (res[i].id == id && id)
            return &res[i];
    return 0;
}

static void res_detach(Res *r)
{
    free(r->mem);
    free(r->len);
    r->mem = 0, r->len = 0, r->nmem = 0;
}

/* n bytes at offset off of the guest's copy; 0 when it has no such bytes */
static int res_read(Res *r, u64 off, u8 *dst, u32 n)
{
    u32 i;
    for (i = 0; i < r->nmem && n; i++) {
        if (off < r->len[i]) {
            u32 k = r->len[i] - (u32)off < n ? r->len[i] - (u32)off : n;
            memcpy(dst, r->mem[i] + off, k);
            dst += k, n -= k, off = 0;
        } else
            off -= r->len[i];
    }
    return n == 0;
}

static u32 pixel(u32 fmt, const u8 *p)
{
    switch (fmt) {
    case 1:
    case 2: return (u32)p[2] << 16 | (u32)p[1] << 8 | p[0]; /* B G R x */
    case 3:
    case 4: return (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; /* x R G B */
    case 121:
    case 68: return (u32)p[3] << 16 | (u32)p[2] << 8 | p[1]; /* x B G R */
    }
    return (u32)p[0] << 16 | (u32)p[1] << 8 | p[2]; /* R G B x */
}

static void to_screen(Vm *vm, Res *r, u32 x, u32 y, u32 w, u32 h)
{
    u32 row;
    if (x >= r->w || y >= r->h || x >= vm->scr_w || y >= vm->scr_h)
        return;
    if (w > r->w - x)
        w = r->w - x;
    if (w > vm->scr_w - x)
        w = vm->scr_w - x;
    if (h > r->h - y)
        h = r->h - y;
    if (h > vm->scr_h - y)
        h = vm->scr_h - y;
    for (row = y; row < y + h; row++)
        memcpy(vm->scr + (size_t)row * vm->scr_w + x, r->pix + (size_t)row * r->w + x, (size_t)w * 4);
    vm->scr_dirty = 1;
}

/* One command; the answer goes to out (room for 408 bytes), its length is returned. */
static u32 gpu_cmd(Vm *vm, const u8 *c, u32 n, u8 *out)
{
    u32 type = g32(c), err = 0x1200, i;
    Res *r;
    memset(out, 0, 24);
    if (g32(c + 4) & 1) { /* fenced: the answer names the fence */
        p32(out + 4, 1);
        memcpy(out + 8, c + 8, 12);
    }
    p32(out, 0x1100);
    switch (type) {
    case 0x100: /* display info */
        memset(out + 24, 0, 384);
        p32(out, 0x1101);
        p32(out + 24 + 8, vm->scr_w);
        p32(out + 24 + 12, vm->scr_h);
        p32(out + 24 + 16, 1);
        return 408;
    case 0x101: /* create: id, format, width, height */
        if (n < 40 || !g32(c + 24) || res_find(g32(c + 24)) || g32(c + 32) > 8192 || g32(c + 36) > 8192)
            break;
        for (i = 0; i < NRES && res[i].id; i++)
            ;
        if (i == NRES)
            break;
        r = &res[i];
        memset(r, 0, sizeof *r);
        r->fmt = g32(c + 28), r->w = g32(c + 32), r->h = g32(c + 36);
        r->pix = (u32 *)calloc((size_t)r->w * r->h + 1, 4);
        r->id = g32(c + 24);
        return 24;
    case 0x102: /* unref */
        if (n < 28 || !(r = res_find(g32(c + 24))))
            break;
        if (scanout == r->id)
            scanout = 0;
        res_detach(r);
        free(r->pix);
        r->id = 0;
        return 24;
    case 0x103: /* set scanout: rect, scanout, resource */
        if (n < 48 || g32(c + 40))
            break;
        scanout = g32(c + 44);
        if ((r = res_find(scanout)) != 0)
            to_screen(vm, r, 0, 0, r->w, r->h);
        return 24;
    case 0x104: /* flush: rect, resource */
        if (n < 44 || !(r = res_find(g32(c + 40))))
            break;
        if (r->id == scanout)
            to_screen(vm, r, g32(c + 24), g32(c + 28), g32(c + 32), g32(c + 36));
        return 24;
    case 0x105: { /* transfer to host: rect, offset, resource */
        u32 x, y, w, h, row, col;
        u64 off;
        static u8 line[8192 * 4];
        if (n < 52 || !(r = res_find(g32(c + 48))))
            break;
        x = g32(c + 24), y = g32(c + 28), w = g32(c + 32), h = g32(c + 36), off = g64(c + 40);
        if (x >= r->w || y >= r->h || w > r->w - x || h > r->h - y)
            break;
        for (row = 0; row < h; row++) {
            u32 *d = r->pix + (size_t)(y + row) * r->w + x;
            if (!res_read(r, off + (u64)row * r->w * 4, line, w * 4))
                break;
            if (r->fmt == 1 || r->fmt == 2)
                for (col = 0; col < w; col++)
                    d[col] = g32(line + 4 * col) & 0xffffff;
            else
                for (col = 0; col < w; col++)
                    d[col] = pixel(r->fmt, line + 4 * col);
        }
        return 24;
    }
    case 0x106: { /* attach backing: resource, count, then address and length of each piece */
        u32 cnt;
        if (n < 32 || !(r = res_find(g32(c + 24))))
            break;
        cnt = g32(c + 28);
        if (cnt > (n - 32) / 16)
            break;
        res_detach(r);
        r->mem = (u8 **)calloc(cnt + 1, sizeof *r->mem);
        r->len = (u32 *)calloc(cnt + 1, sizeof *r->len);
        for (i = 0; i < cnt; i++) {
            u64 a = g64(c + 32 + 16 * i);
            u32 l = g32(c + 40 + 16 * i);
            if (a < RAM_BASE || a - RAM_BASE > vm->ram_size || l > vm->ram_size - (a - RAM_BASE))
                break;
            r->mem[i] = vm->ram + (a - RAM_BASE);
            r->len[i] = l;
        }
        r->nmem = i;
        return 24;
    }
    case 0x107: /* detach backing */
        if (n < 28 || !(r = res_find(g32(c + 24))))
            break;
        res_detach(r);
        return 24;
    case 0x300: /* the cursor: the guest's programs draw their own */
    case 0x301: return 24;
    default: err = 0x1200;
    }
    p32(out, err);
    return 24;
}

static void gpu_notify(Vm *vm, Virtio *d, int qi)
{
    static u8 *req;
    static u32 cap;
    u8 out[408];
    Iov iov[260];
    u16 head;
    int k, i;
    while ((k = vq_pop(vm, d, qi, iov, 260, &head)) > 0) {
        u32 n = 0, m, off = 0, on;
        for (i = 0; i < k; i++)
            if (!iov[i].w) {
                if (n + iov[i].len > cap) {
                    cap = (n + iov[i].len) * 2;
                    req = (u8 *)realloc(req, cap);
                }
                memcpy(req + n, iov[i].p, iov[i].len);
                n += iov[i].len;
            }
        on = n >= 24 ? gpu_cmd(vm, req, n, out) : 0;
        for (i = 0; i < k && off < on; i++) {
            if (!iov[i].w)
                continue;
            m = iov[i].len < on - off ? iov[i].len : on - off;
            memcpy(iov[i].p, out + off, m);
            off += m;
        }
        vq_push(vm, d, qi, head, off);
    }
}

/* ---- keyboard and pointer ---- */

typedef struct Input {
    Virtio *dev;
    int tablet;
    u8 ev[256][8]; /* events the guest had no room for */
    int head, n;
} Input;

static Input kbd, ptr;

static void input_flush(Vm *vm, Input *in)
{
    Iov iov[2];
    u16 head;
    while (in->n && vq_pop(vm, in->dev, 0, iov, 2, &head) > 0) {
        if (iov[0].w && iov[0].len >= 8)
            memcpy(iov[0].p, in->ev[in->head], 8);
        in->head = (in->head + 1) % 256;
        in->n--;
        vq_push(vm, in->dev, 0, head, 8);
    }
}

static void input_event(Vm *vm, Input *in, u32 type, u32 code, u32 value)
{
    u8 *e;
    if (!in->dev || in->n == 256)
        return;
    e = in->ev[(in->head + in->n++) % 256];
    p16(e, type), p16(e + 2, code), p32(e + 4, value);
    if (type == 0)
        input_flush(vm, in);
}

void input_key(Vm *vm, int code, int down)
{
    input_event(vm, &kbd, 1, (u32)code, (u32)down);
    input_event(vm, &kbd, 0, 0, 0);
}

void input_pointer(Vm *vm, int x, int y)
{
    input_event(vm, &ptr, 3, 0, (u32)x);
    input_event(vm, &ptr, 3, 1, (u32)y);
    input_event(vm, &ptr, 0, 0, 0);
}

void input_button(Vm *vm, int code, int down)
{
    input_event(vm, &ptr, 1, (u32)code, (u32)down);
    input_event(vm, &ptr, 0, 0, 0);
}

void input_wheel(Vm *vm, int steps)
{
    input_event(vm, &ptr, 2, 8, (u32)steps);
    input_event(vm, &ptr, 0, 0, 0);
}

static void input_notify(Vm *vm, Virtio *d, int qi)
{
    Iov iov[2];
    u16 head;
    if (qi == 0)
        input_flush(vm, (Input *)d->priv);
    else /* lights of the keyboard: taken and forgotten */
        while (vq_pop(vm, d, 1, iov, 2, &head) > 0)
            vq_push(vm, d, 1, head, 0);
}

/* The guest asks what the device is by writing a question (select, subsel) into config. */
static void input_config(Vm *vm, Virtio *d)
{
    Input *in = (Input *)d->priv;
    u8 *c = d->config, *u = c + 8;
    u32 sel = c[0], sub = c[1], n = 0, i;
    (void)vm;
    memset(c + 2, 0, sizeof d->config - 2);
    if (sel == 1) { /* name */
        const char *s = in->tablet ? "mvm pointer" : "mvm keyboard";
        n = (u32)strlen(s);
        memcpy(u, s, n);
    } else if (sel == 3) { /* bus, vendor, product, version */
        p16(u, 6), p16(u + 2, 0x6d76), p16(u + 4, in->tablet ? 2 : 1), p16(u + 6, 1);
        n = 8;
    } else if (sel == 0x11 && !in->tablet) { /* which events of a type there are, as a bit map */
        if (sub == 1) {
            for (i = 1; i < 128; i++)
                u[i / 8] |= (u8)(1 << (i % 8));
            n = 16;
        } else if (sub == 4) {
            u[0] = 0x10;
            n = 1;
        } else if (sub == 0x14) {
            u[0] = 3;
            n = 1;
        }
    } else if (sel == 0x11) {
        if (sub == 1) {
            u[0x110 / 8] = 7; /* left, right, middle */
            n = 0x110 / 8 + 1;
        } else if (sub == 2) {
            u[1] = 1; /* wheel */
            n = 2;
        } else if (sub == 3) {
            u[0] = 3; /* x, y */
            n = 1;
        }
    } else if (sel == 0x12 && in->tablet && sub < 2) { /* range of an axis */
        p32(u, 0), p32(u + 4, 32767);
        n = 20;
    }
    c[2] = (u8)n;
}

static void input_add(Vm *vm, Input *in, int tablet)
{
    in->tablet = tablet;
    in->dev = virtio_add(vm, 18, 0, 2, input_notify);
    in->dev->priv = in;
    in->dev->cfg_write = input_config;
}

/* ---- pictures of the screen, scripts of input ---- */

/* A BMP file: every viewer reads it, and it needs no compression. */
int screenshot(Vm *vm, const char *file)
{
    FILE *f;
    u8 h[54];
    u32 y, x, row = (vm->scr_w * 3 + 3) & ~(u32)3;
    u8 *line;
    if (!vm->scr || !(f = fopen(file, "wb")))
        return -1;
    memset(h, 0, sizeof h);
    h[0] = 'B', h[1] = 'M';
    p32(h + 2, 54 + row * vm->scr_h);
    p32(h + 10, 54);
    p32(h + 14, 40);
    p32(h + 18, vm->scr_w);
    p32(h + 22, vm->scr_h);
    p16(h + 26, 1);
    p16(h + 28, 24);
    p32(h + 34, row * vm->scr_h);
    fwrite(h, 1, 54, f);
    line = (u8 *)calloc(row, 1);
    for (y = vm->scr_h; y-- > 0;) {
        for (x = 0; x < vm->scr_w; x++) {
            u32 p = vm->scr[(size_t)y * vm->scr_w + x];
            line[3 * x] = (u8)p, line[3 * x + 1] = (u8)(p >> 8), line[3 * x + 2] = (u8)(p >> 16);
        }
        fwrite(line, 1, row, f);
    }
    free(line);
    return fclose(f);
}

/* A script: lines of "MILLISECONDS WHAT ...", in the order of their times, counted from the start:
     key CODE 0|1    move X Y (pixels)    button CODE 0|1    shot FILE    quit */
static char *play, *play_at;
static u64 play_t0;

int gpu_play(Vm *vm, const char *file)
{
    FILE *f = fopen(file, "r");
    long n;
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    play = (char *)calloc((size_t)n + 2, 1);
    n = (long)fread(play, 1, (size_t)n, f);
    fclose(f);
    play_at = play;
    play_t0 = now_usec();
    vm->gui = 1;
    return 0;
}

void gpu_poll(Vm *vm)
{
    gui_poll(vm);
    while (play_at && *play_at) {
        unsigned long ms;
        int a, b, k = 0;
        char *nl = strchr(play_at, '\n'), name[256];
        if (sscanf(play_at, "%lu", &ms) != 1 || (now_usec() - play_t0) / 1000 < ms)
            break;
        if (sscanf(play_at, "%*u key %d %d", &a, &b) == 2)
            input_key(vm, a, b);
        else if (sscanf(play_at, "%*u move %d %d", &a, &b) == 2)
            input_pointer(vm, (int)((long)a * 32767 / (long)(vm->scr_w - 1)), (int)((long)b * 32767 / (long)(vm->scr_h - 1)));
        else if (sscanf(play_at, "%*u button %d %d", &a, &b) == 2)
            input_button(vm, a, b);
        else if (sscanf(play_at, "%*u shot %255s%n", name, &k) == 1)
            screenshot(vm, name);
        else if (sscanf(play_at, "%*u qui%c", name) == 1)
            vm->poweroff = 1, vm->status = 0;
        play_at = nl ? nl + 1 : play_at + strlen(play_at);
    }
}

void gpu_add(Vm *vm, u32 w, u32 h)
{
    Virtio *d;
    vm->scr_w = w, vm->scr_h = h;
    vm->scr = (u32 *)calloc((size_t)w * h, 4);
    d = virtio_add(vm, 16, 0, 2, gpu_notify);
    p32(d->config + 8, 1); /* one screen */
    input_add(vm, &kbd, 0);
    input_add(vm, &ptr, 1);
}
