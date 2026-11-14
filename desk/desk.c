/* mdesk: a small desktop for the Linux guest. It draws on the frame buffer (/dev/fb0), reads the keyboard
   and the pointer from /dev/input/event*, and takes its letters from the kernel's own console font. There
   are windows to drag, raise and close, a bar to open them from, a terminal that runs the shell on a
   pseudo-terminal, a system monitor and a note about the machine. Strict C89 like the rest of mcc;
   built by mcc against its Linux C library (mkdesk.sh). */
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

typedef unsigned int u32;
typedef unsigned char u8;

/* ---- the screen ---- */

static u32 *fb, *back; /* the device, and the picture composed here before it is copied there */
static int W, H, pitch; /* pitch: pixels from one line of the device to the next */
static int cx0, cy0, cx1, cy1; /* drawing is clipped to this rectangle */
static int dx0, dy0, dx1, dy1; /* what has to be drawn again */
static u8 font[256][16];

static u32 le32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

static void damage(int x, int y, int w, int h)
{
    if (dx0 == dx1) {
        dx0 = x, dy0 = y, dx1 = x + w, dy1 = y + h;
        return;
    }
    if (x < dx0)
        dx0 = x;
    if (y < dy0)
        dy0 = y;
    if (x + w > dx1)
        dx1 = x + w;
    if (y + h > dy1)
        dy1 = y + h;
}

static void fill(int x, int y, int w, int h, u32 c)
{
    int x1 = x + w, y1 = y + h, i;
    if (x < cx0)
        x = cx0;
    if (y < cy0)
        y = cy0;
    if (x1 > cx1)
        x1 = cx1;
    if (y1 > cy1)
        y1 = cy1;
    for (; y < y1; y++) {
        u32 *p = back + (size_t)y * W;
        for (i = x; i < x1; i++)
            p[i] = c;
    }
}

static void frame(int x, int y, int w, int h, u32 c)
{
    fill(x, y, w, 1, c);
    fill(x, y + h - 1, w, 1, c);
    fill(x, y, 1, h, c);
    fill(x + w - 1, y, 1, h, c);
}

/* One letter; bg < 0 leaves what is behind it. */
static void glyph(int x, int y, int ch, u32 fg, long bg)
{
    int r, k;
    if (x >= cx1 || y >= cy1 || x + 8 <= cx0 || y + 16 <= cy0)
        return;
    for (r = 0; r < 16; r++) {
        int bits = font[ch & 255][r], yy = y + r;
        u32 *p;
        if (yy < cy0 || yy >= cy1)
            continue;
        p = back + (size_t)yy * W;
        for (k = 0; k < 8; k++) {
            int xx = x + k;
            if (xx < cx0 || xx >= cx1)
                continue;
            if (bits & (0x80 >> k))
                p[xx] = fg;
            else if (bg >= 0)
                p[xx] = (u32)bg;
        }
    }
}

static void text(int x, int y, const char *s, u32 fg, long bg)
{
    for (; *s; s++, x += 8)
        glyph(x, y, (u8)*s, fg, bg);
}

static void screen_open(void)
{
    u8 var[160], fix[80], *data;
    struct {
        unsigned op, flags, width, height, count;
        u8 *data;
    } fo;
    int fd = open("/dev/fb0", O_RDWR), tty, i;
    if (fd < 0 || ioctl(fd, 0x4600, var) < 0 || ioctl(fd, 0x4602, fix) < 0 || le32(var + 24) != 32) {
        fprintf(stderr, "mdesk: no 32-bit frame buffer at /dev/fb0\n");
        exit(1);
    }
    W = (int)le32(var), H = (int)le32(var + 4);
    pitch = (int)le32(fix + 48) / 4;
    fb = (u32 *)mmap(0, (size_t)pitch * H * 4, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb == (u32 *)MAP_FAILED) {
        fprintf(stderr, "mdesk: cannot map the frame buffer\n");
        exit(1);
    }
    back = (u32 *)calloc((size_t)W * H, 4);
    tty = open("/dev/tty0", O_RDWR);
    if (tty >= 0) {
        data = (u8 *)calloc(512, 128);
        memset(&fo, 0, sizeof fo);
        fo.op = 1, fo.width = 32, fo.height = 32, fo.count = 512, fo.data = data;
        if (ioctl(tty, 0x4B72, &fo) == 0 && fo.width == 8 && fo.height >= 8) /* the console's font */
            for (i = 0; i < 256; i++)
                memcpy(font[i], data + i * 32 + (fo.height > 16 ? (fo.height - 16) / 2 : 0), 16);
        free(data);
        ioctl(tty, 0x4B3A, 1); /* graphics mode: the console stops drawing */
        ioctl(tty, 0x4B45, 4); /* and stops reading the keyboard */
    }
}

/* ---- the terminal ---- */

#define COLS 80
#define ROWS 24

typedef struct Term {
    int fd, x, y, esc, np, color, bold;
    int par[8];
    u8 ch[ROWS][COLS], at[ROWS][COLS];
} Term;

static const u32 palette[16] = { 0x101418, 0xd04040, 0x40b050, 0xc8a030, 0x5080e0, 0xb060c0, 0x40b0b8, 0xc8ccd0,
                                 0x606870, 0xff6060, 0x60e070, 0xf0d050, 0x80a8ff, 0xe080f0, 0x60e0e8, 0xffffff };

static void term_scroll(Term *t)
{
    memmove(t->ch[0], t->ch[1], (ROWS - 1) * COLS);
    memmove(t->at[0], t->at[1], (ROWS - 1) * COLS);
    memset(t->ch[ROWS - 1], ' ', COLS);
    memset(t->at[ROWS - 1], 7, COLS);
}

static void term_clear(Term *t, int y, int x0, int x1)
{
    if (y < 0 || y >= ROWS)
        return;
    memset(t->ch[y] + x0, ' ', (size_t)(x1 - x0));
    memset(t->at[y] + x0, 7, (size_t)(x1 - x0));
}

static void term_csi(Term *t, int c)
{
    int a = t->par[0], b = t->par[1], i;
    switch (c) {
    case 'A': t->y -= a ? a : 1; break;
    case 'B': t->y += a ? a : 1; break;
    case 'C': t->x += a ? a : 1; break;
    case 'D': t->x -= a ? a : 1; break;
    case 'G': t->x = a ? a - 1 : 0; break;
    case 'H':
    case 'f':
        t->y = a ? a - 1 : 0;
        t->x = b ? b - 1 : 0;
        break;
    case 'J':
        if (a == 2)
            t->x = t->y = 0;
        term_clear(t, t->y, t->x, COLS);
        for (i = t->y + 1; i < ROWS; i++)
            term_clear(t, i, 0, COLS);
        break;
    case 'K': term_clear(t, t->y, a == 1 || a == 2 ? 0 : t->x, a == 1 ? t->x : COLS); break;
    case 'm':
        for (i = 0; i <= t->np && i < 8; i++) {
            int p = t->par[i];
            if (p == 0)
                t->color = 7, t->bold = 0;
            else if (p == 1)
                t->bold = 8;
            else if (p >= 30 && p <= 37)
                t->color = p - 30;
            else if (p == 39)
                t->color = 7;
        }
        break;
    case 'n': /* where is the cursor? */
        if (a == 6) {
            char r[32];
            sprintf(r, "\033[%d;%dR", t->y + 1, t->x + 1);
            write(t->fd, r, strlen(r));
        }
        break;
    }
    if (t->x < 0)
        t->x = 0;
    if (t->x >= COLS)
        t->x = COLS - 1;
    if (t->y < 0)
        t->y = 0;
    if (t->y >= ROWS)
        t->y = ROWS - 1;
}

static void term_put(Term *t, int c)
{
    if (t->esc == 1) {
        t->esc = c == '[' ? 2 : 0;
        t->np = 0;
        memset(t->par, 0, sizeof t->par);
        return;
    }
    if (t->esc == 2) {
        if (c >= '0' && c <= '9') {
            if (t->np < 8)
                t->par[t->np] = t->par[t->np] * 10 + c - '0';
        } else if (c == ';')
            t->np++;
        else if (c >= 0x40) {
            term_csi(t, c);
            t->esc = 0;
        }
        return;
    }
    switch (c) {
    case 27: t->esc = 1; return;
    case '\r': t->x = 0; return;
    case '\b':
        if (t->x)
            t->x--;
        return;
    case '\t': t->x = (t->x + 8) & ~7; break;
    case 7: return;
    case '\n':
        if (++t->y == ROWS) {
            t->y--;
            term_scroll(t);
        }
        return;
    default:
        if (c < 32)
            return;
        if (t->x >= COLS) {
            t->x = 0;
            if (++t->y == ROWS) {
                t->y--;
                term_scroll(t);
            }
        }
        t->ch[t->y][t->x] = (u8)c;
        t->at[t->y][t->x] = (u8)(t->color | t->bold);
        t->x++;
        return;
    }
    if (t->x >= COLS)
        t->x = COLS - 1;
}

static Term *term_new(void)
{
    Term *t = (Term *)calloc(1, sizeof *t);
    struct winsize ws;
    char *name;
    int i, pid;
    for (i = 0; i < ROWS; i++)
        term_clear(t, i, 0, COLS);
    t->color = 7;
    t->fd = posix_openpt(O_RDWR | O_NOCTTY);
    if (t->fd < 0 || grantpt(t->fd) < 0 || unlockpt(t->fd) < 0 || !(name = ptsname(t->fd))) {
        const char *m = "no pseudo-terminal: is /dev/pts mounted?";
        memcpy(t->ch[0], m, strlen(m));
        return t;
    }
    memset(&ws, 0, sizeof ws);
    ws.ws_col = COLS, ws.ws_row = ROWS;
    ioctl(t->fd, TIOCSWINSZ, &ws);
    pid = fork();
    if (pid == 0) {
        int s;
        setsid();
        s = open(name, O_RDWR);
        if (s < 0)
            _exit(1);
        ioctl(s, TIOCSCTTY, 0);
        dup2(s, 0), dup2(s, 1), dup2(s, 2);
        for (i = 3; i < 64; i++)
            close(i);
        setenv("TERM", "linux", 1);
        setenv("PS1", "\\w # ", 1);
        execl("/bin/sh", "sh", (char *)0);
        _exit(1);
    }
    fcntl(t->fd, F_SETFL, O_NONBLOCK);
    return t;
}

/* ---- windows ---- */

enum { K_TERM, K_MON, K_ABOUT };
enum { TITLE = 24, BAR = 30, NWIN = 8, NHIST = 60 };

typedef struct Win {
    int used, kind, x, y, w, h; /* w, h: the inside */
    const char *title;
    Term *t;
} Win;

static Win wins[NWIN];
static int order[NWIN], nwin; /* from the bottom to the top */
static int mx, my, drag = -1, dragx, dragy, shift, ctrl;
static int hist[NHIST], nhist;
static long cpu_busy, cpu_all, mem_total, mem_free, up;
static char load[32];

static void win_damage(Win *w) { damage(w->x - 1, w->y - TITLE - 1, w->w + 2, w->h + TITLE + 2); }

static void win_open(int kind)
{
    static const char *titles[3] = { "Terminal", "System monitor", "About this machine" };
    static const int sizes[3][2] = { { COLS * 8 + 8, ROWS * 16 + 8 }, { 300, 190 }, { 400, 200 } };
    Win *w;
    int i;
    for (i = 0; i < NWIN && wins[i].used; i++)
        ;
    if (i == NWIN)
        return;
    w = &wins[i];
    memset(w, 0, sizeof *w);
    w->used = 1, w->kind = kind, w->title = titles[kind];
    w->w = sizes[kind][0], w->h = sizes[kind][1];
    w->x = 40 + 36 * nwin + (kind == K_MON ? W - 420 : 0);
    w->y = 50 + 30 * nwin;
    if (w->x + w->w > W)
        w->x = W - w->w - 4;
    if (kind == K_TERM)
        w->t = term_new();
    order[nwin++] = i;
    win_damage(w);
}

static void win_raise(int at)
{
    int i = order[at];
    memmove(order + at, order + at + 1, (size_t)(nwin - at - 1) * sizeof order[0]);
    order[nwin - 1] = i;
    win_damage(&wins[i]);
    if (nwin > 1)
        win_damage(&wins[order[nwin - 2]]);
}

static void win_close(int at)
{
    Win *w = &wins[order[at]];
    win_damage(w);
    if (w->t) {
        if (w->t->fd >= 0)
            close(w->t->fd); /* the shell gets a hangup */
        free(w->t);
    }
    w->used = 0;
    memmove(order + at, order + at + 1, (size_t)(nwin - at - 1) * sizeof order[0]);
    nwin--;
    if (nwin)
        win_damage(&wins[order[nwin - 1]]);
}

static void draw_term(Win *w)
{
    Term *t = w->t;
    int r, c, x0 = w->x + 4, y0 = w->y + 4;
    fill(w->x, w->y, w->w, w->h, palette[0]);
    for (r = 0; r < ROWS; r++) {
        if (y0 + r * 16 >= cy1 || y0 + r * 16 + 16 <= cy0)
            continue;
        for (c = 0; c < COLS; c++)
            if (t->ch[r][c] != ' ')
                glyph(x0 + c * 8, y0 + r * 16, t->ch[r][c], palette[t->at[r][c] & 15], -1);
    }
    if (w == &wins[order[nwin - 1]])
        fill(x0 + t->x * 8, y0 + t->y * 16 + 13, 8, 3, 0xc8ccd0);
    else
        frame(x0 + t->x * 8, y0 + t->y * 16, 8, 16, 0x606870);
}

static void draw_mon(Win *w)
{
    char s[64];
    int i, gx = w->x + 12, gy = w->y + 96, gh = 80;
    fill(w->x, w->y, w->w, w->h, 0xf2f4f6);
    sprintf(s, "up %ld:%02ld:%02ld", up / 3600, up / 60 % 60, up % 60);
    text(w->x + 12, w->y + 10, s, 0x202830, -1);
    sprintf(s, "load %s", load);
    text(w->x + 12, w->y + 30, s, 0x202830, -1);
    sprintf(s, "memory %ld of %ld MB free", mem_free / 1024, mem_total / 1024);
    text(w->x + 12, w->y + 50, s, 0x202830, -1);
    sprintf(s, "processor %d%%", nhist ? hist[nhist - 1] : 0);
    text(w->x + 12, w->y + 74, s, 0x202830, -1);
    fill(gx, gy, NHIST * 4 + 2, gh + 2, 0x182028);
    for (i = 0; i < nhist; i++) {
        int v = hist[i] * gh / 100;
        fill(gx + 1 + i * 4, gy + 1 + gh - v, 3, v, hist[i] > 80 ? 0xff6060 : 0x60e070);
    }
}

static void draw_about(Win *w)
{
    static const char *lines[] = { "Linux, built by mcc alone:", "compiler, assembler, linker, make, shell.",
                                   "It runs in mvm, mcc's own virtual machine,", "and draws through its virtio-gpu.",
                                   "", "This desktop is mdesk; drag the windows,", "open more from the bar below." };
    int i;
    fill(w->x, w->y, w->w, w->h, 0xf2f4f6);
    for (i = 0; i < 7; i++)
        text(w->x + 16, w->y + 14 + i * 18, lines[i], 0x202830, -1);
    for (i = 0; i < 8; i++)
        fill(w->x + 16 + i * 46, w->y + 150, 46, 30, palette[i + 8]);
}

static void draw_win(Win *w, int top)
{
    int x = w->x - 1, y = w->y - TITLE - 1, ww = w->w + 2;
    if (x >= cx1 || y >= cy1 || x + ww <= cx0 || w->y + w->h + 1 <= cy0)
        return;
    fill(x + 4, y + 4, ww, w->h + TITLE + 2, 0x283038); /* a shadow, of a sort */
    frame(x, y, ww, w->h + TITLE + 2, 0x101418);
    fill(x + 1, y + 1, ww - 2, TITLE, top ? 0x3868b8 : 0x8890a0);
    text(x + 10, y + 5, w->title, 0xffffff, -1);
    fill(x + ww - 21, y + 5, 16, 16, 0xd04040);
    text(x + ww - 17, y + 5, "x", 0xffffff, -1);
    if (w->kind == K_TERM)
        draw_term(w);
    else if (w->kind == K_MON)
        draw_mon(w);
    else
        draw_about(w);
}

static const char *buttons[3] = { "Terminal", "Monitor", "About" };

static void draw_bar(void)
{
    char s[32];
    int i;
    if (cy1 <= H - BAR)
        return;
    fill(0, H - BAR, W, BAR, 0x202830);
    fill(0, H - BAR, W, 1, 0x485868);
    for (i = 0; i < 3; i++) {
        fill(8 + i * 100, H - BAR + 5, 92, 21, 0x3868b8);
        text(8 + i * 100 + (92 - (int)strlen(buttons[i]) * 8) / 2, H - BAR + 7, buttons[i], 0xffffff, -1);
    }
    sprintf(s, "mdesk  %ld:%02ld:%02ld", up / 3600, up / 60 % 60, up % 60);
    text(W - 8 - (int)strlen(s) * 8, H - BAR + 7, s, 0xc8ccd0, -1);
}

static void draw_pointer(void)
{
    static const char *shape[] = { "#", "##", "#.#", "#..#", "#...#", "#....#", "#.....#", "#......#", "#.......#",
                                   "#........#", "#.....#####", "#..#..#", "#.# #..#", "##  #..#", "#    #..#",
                                   "     #..#", "      ##" };
    int r, c;
    for (r = 0; r < 17; r++)
        for (c = 0; shape[r][c]; c++)
            if (shape[r][c] != ' ')
                fill(mx + c, my + r, 1, 1, shape[r][c] == '#' ? 0x000000 : 0xffffff);
}

/* Draw what was damaged, and put it on the device. */
static void redraw(void)
{
    int y, i;
    if (dx0 == dx1)
        return;
    cx0 = dx0 < 0 ? 0 : dx0, cy0 = dy0 < 0 ? 0 : dy0;
    cx1 = dx1 > W ? W : dx1, cy1 = dy1 > H ? H : dy1;
    dx0 = dx1 = 0;
    if (cx0 >= cx1 || cy0 >= cy1)
        return;
    for (y = cy0; y < cy1; y++) { /* the desk: a colour that changes down the screen */
        u32 c = (u32)(0x18 + y * 0x20 / H) << 16 | (u32)(0x58 + y * 0x40 / H) << 8 | (u32)(0x78 + y * 0x30 / H);
        u32 *p = back + (size_t)y * W;
        for (i = cx0; i < cx1; i++)
            p[i] = c;
    }
    text(16, 14, "mdesk on Linux on mvm", 0xe0f0f8, -1);
    for (i = 0; i < nwin; i++)
        draw_win(&wins[order[i]], i == nwin - 1);
    draw_bar();
    draw_pointer();
    for (y = cy0; y < cy1; y++)
        memcpy(fb + (size_t)y * pitch + cx0, back + (size_t)y * W + cx0, (size_t)(cx1 - cx0) * 4);
}

/* ---- input ---- */

static void press(int x, int y)
{
    int i;
    if (y >= H - BAR) {
        for (i = 0; i < 3; i++)
            if (x >= 8 + i * 100 && x < 100 + i * 100)
                win_open(i);
        return;
    }
    for (i = nwin - 1; i >= 0; i--) {
        Win *w = &wins[order[i]];
        if (x < w->x - 1 || x > w->x + w->w || y < w->y - TITLE - 1 || y > w->y + w->h)
            continue;
        if (y < w->y && x >= w->x + w->w - 20) {
            win_close(i);
            return;
        }
        win_raise(i);
        if (y < w->y) {
            drag = order[nwin - 1];
            dragx = x - w->x, dragy = y - w->y;
        }
        return;
    }
}

static void key(int code, int value)
{
    static const char plain[] = "\0\0331234567890-=\177\tqwertyuiop[]\r\0asdfghjkl;'`\0\\zxcvbnm,./\0*\0 ";
    static const char shifted[] = "\0\033!@#$%^&*()_+\177\tQWERTYUIOP{}\r\0ASDFGHJKL:\"~\0|ZXCVBNM<>?\0*\0 ";
    Win *w = nwin ? &wins[order[nwin - 1]] : 0;
    char out[4];
    int n = 0;
    if (code == 42 || code == 54)
        shift = value != 0;
    else if (code == 29 || code == 97)
        ctrl = value != 0;
    if (!value || !w || w->kind != K_TERM || w->t->fd < 0)
        return;
    if (code == 103 || code == 108 || code == 106 || code == 105) {
        out[0] = 27, out[1] = '[';
        out[2] = code == 103 ? 'A' : code == 108 ? 'B' : code == 106 ? 'C' : 'D';
        n = 3;
    } else if (code > 0 && code < (int)sizeof plain - 1 && plain[code]) {
        out[0] = shift ? shifted[code] : plain[code];
        if (ctrl)
            out[0] &= 0x1f;
        n = 1;
    }
    if (n)
        write(w->t->fd, out, (size_t)n);
}

static void input(int fd)
{
    u8 ev[24 * 16];
    long n = read(fd, ev, sizeof ev), i;
    for (i = 0; i + 24 <= n; i += 24) {
        int type = ev[i + 16] | ev[i + 17] << 8, code = ev[i + 18] | ev[i + 19] << 8;
        int value = (int)le32(ev + i + 20);
        if (type == 3 && code < 2) {
            damage(mx, my, 12, 18);
            if (code == 0)
                mx = (int)((long)value * (W - 1) / 32767);
            else
                my = (int)((long)value * (H - 1) / 32767);
            damage(mx, my, 12, 18);
            if (drag >= 0) {
                Win *w = &wins[drag];
                win_damage(w);
                damage(w->x + 3, w->y - TITLE + 3, w->w + 2, w->h + TITLE + 2);
                w->x = mx - dragx, w->y = my - dragy;
                if (w->y < TITLE + 1)
                    w->y = TITLE + 1;
                win_damage(w);
                damage(w->x + 3, w->y - TITLE + 3, w->w + 2, w->h + TITLE + 2);
            }
        } else if (type == 1 && code == 0x110) {
            if (value)
                press(mx, my);
            else
                drag = -1;
        } else if (type == 1 && code < 0x100)
            key(code, value);
    }
}

/* ---- the numbers of the monitor ---- */

static void sample(void)
{
    char buf[1024], *p;
    long v[8], busy, all;
    FILE *f;
    int i;
    if ((f = fopen("/proc/stat", "r")) != 0) {
        if (fgets(buf, sizeof buf, f) && sscanf(buf, "cpu %ld %ld %ld %ld %ld %ld %ld", v, v + 1, v + 2, v + 3, v + 4, v + 5, v + 6) == 7) {
            busy = v[0] + v[1] + v[2] + v[5] + v[6], all = busy + v[3] + v[4];
            if (nhist == NHIST)
                memmove(hist, hist + 1, (--nhist) * sizeof hist[0]);
            hist[nhist++] = all > cpu_all && cpu_all ? (int)((busy - cpu_busy) * 100 / (all - cpu_all)) : 0;
            cpu_busy = busy, cpu_all = all;
        }
        fclose(f);
    }
    if ((f = fopen("/proc/meminfo", "r")) != 0) {
        while (fgets(buf, sizeof buf, f)) {
            sscanf(buf, "MemTotal: %ld", &mem_total);
            sscanf(buf, "MemFree: %ld", &mem_free);
        }
        fclose(f);
    }
    if ((f = fopen("/proc/loadavg", "r")) != 0) {
        if (fgets(buf, sizeof buf, f)) {
            for (p = buf, i = 0; *p && i < 3; p++)
                if (*p == ' ')
                    i++;
            if (i == 3)
                p[-1] = 0;
            strncpy(load, buf, sizeof load - 1);
        }
        fclose(f);
    }
    if ((f = fopen("/proc/uptime", "r")) != 0) {
        if (fgets(buf, sizeof buf, f))
            up = atol(buf);
        fclose(f);
    }
    for (i = 0; i < nwin; i++)
        if (wins[order[i]].kind == K_MON)
            win_damage(&wins[order[i]]);
    damage(W - 200, H - BAR, 200, BAR);
}

int main(void)
{
    struct pollfd p[4 + NWIN];
    int in[4], nin = 0, i, n, map[NWIN];
    time_t last = 0;
    char name[32];
    signal(SIGCHLD, SIG_IGN);
    screen_open();
    for (i = 0; i < 4; i++) {
        sprintf(name, "/dev/input/event%d", i);
        if ((in[nin] = open(name, O_RDONLY)) >= 0)
            nin++;
    }
    mx = W / 2, my = H / 2;
    sample();
    win_open(K_ABOUT);
    win_open(K_MON);
    win_open(K_TERM);
    damage(0, 0, W, H);
    for (;;) {
        redraw();
        for (i = 0; i < nin; i++)
            p[i].fd = in[i], p[i].events = POLLIN;
        n = nin;
        for (i = 0; i < nwin; i++)
            if (wins[order[i]].t && wins[order[i]].t->fd >= 0) {
                map[n - nin] = order[i];
                p[n].fd = wins[order[i]].t->fd, p[n].events = POLLIN;
                n++;
            }
        poll(p, (nfds_t)n, 250);
        for (i = 0; i < nin; i++)
            if (p[i].revents & POLLIN)
                input(in[i]);
        for (i = nin; i < n; i++)
            if (p[i].revents) {
                Win *w = &wins[map[i - nin]];
                char buf[4096];
                long k = w->used && w->t ? read(w->t->fd, buf, sizeof buf) : 0, j;
                if (!w->used || !w->t)
                    continue;
                if (k <= 0) { /* the shell is gone: so is its window */
                    for (j = 0; j < nwin; j++)
                        if (&wins[order[j]] == w) {
                            win_close((int)j);
                            break;
                        }
                    continue;
                }
                for (j = 0; j < k; j++)
                    term_put(w->t, (u8)buf[j]);
                damage(w->x, w->y, w->w, w->h);
            }
        if (time(0) != last) {
            last = time(0);
            sample();
        }
    }
}
