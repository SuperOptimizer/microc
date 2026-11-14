/* The window that shows the guest's screen and gives it the keyboard and the mouse: Cocoa, reached from C
   through the Objective-C runtime (objc_msgSend), with the frameworks bound by mcc's linker. Everything
   here runs on the main thread, which AppKit insists on. Closing the window turns the machine off. */
#include "vm.h"

typedef void *id;

id objc_getClass(const char *name);
id sel_registerName(const char *name);
void objc_msgSend(void);
void gui_msg_point(id obj, id sel, double *xy); /* gui_arm64.s: a message whose answer is a point */
id CGColorSpaceCreateDeviceRGB(void);
id CGDataProviderCreateWithData(void *info, const void *data, unsigned long size, void *release);
id CGImageCreate(unsigned long w, unsigned long h, unsigned long bpc, unsigned long bpp, unsigned long stride, id space,
                 unsigned info, id provider, const double *decode, int interpolate, int intent);
void CGImageRelease(id image);
void CGDataProviderRelease(id provider);

#define SEL(s) sel_registerName(s)
#define M0(o, s) ((id (*)(id, id))objc_msgSend)((id)(o), SEL(s))
#define M1(o, s, a) ((id (*)(id, id, id))objc_msgSend)((id)(o), SEL(s), (id)(a))
#define ML(o, s, a) ((id (*)(id, id, long))objc_msgSend)((id)(o), SEL(s), (long)(a))
#define L0(o, s) ((long (*)(id, id))objc_msgSend)((id)(o), SEL(s))

static id app, window, view, layer, space, mode;
static unsigned long flags;
static u32 *shown; /* what the window's picture is made from: the screen may change under a picture */
static int inside;

/* Linux's key code for each of the Mac's */
static const unsigned char keymap[128] = {
    30, 31, 32, 33, 35, 34, 44, 45, 46, 47, 86, 48, 16, 17, 18, 19, /* a s d f h g z x c v § b q w e r */
    21, 20, 2, 3, 4, 5, 7, 6, 13, 10, 8, 12, 9, 11, 27, 24,         /* y t 1 2 3 4 6 5 = 9 7 - 8 0 ] o */
    22, 26, 23, 25, 28, 38, 36, 40, 37, 39, 43, 51, 53, 49, 50, 52, /* u [ i p ret l j ' k ; \ , / n m . */
    15, 57, 41, 14, 0, 1, 0, 125, 42, 58, 56, 29, 54, 100, 97, 0,   /* tab space ` del . esc . cmd shift caps alt ctrl */
    0, 83, 0, 55, 0, 78, 0, 69, 0, 0, 0, 98, 96, 0, 74, 0,          /* keypad */
    0, 117, 82, 79, 80, 81, 75, 76, 77, 71, 0, 72, 73, 0, 0, 0,
    63, 64, 65, 61, 66, 67, 0, 87, 0, 0, 0, 0, 0, 68, 0, 88,        /* f5 f6 f7 f3 f8 f9 . f11 ... f10 . f12 */
    0, 0, 110, 102, 104, 111, 62, 107, 60, 109, 59, 105, 106, 108, 103, 0 /* ins home pgup del f4 end f2 pgdn f1 arrows */
};

int gui_open(Vm *vm)
{
    id pool, title;
    char name[64];
    if (!vm->scr)
        return -1;
    pool = M0(M0(objc_getClass("NSAutoreleasePool"), "alloc"), "init");
    app = M0(objc_getClass("NSApplication"), "sharedApplication");
    if (!app)
        return -1;
    ML(app, "setActivationPolicy:", 0);
    window = ((id (*)(id, id, double, double, double, double, unsigned long, unsigned long, int))objc_msgSend)(
        M0(objc_getClass("NSWindow"), "alloc"), SEL("initWithContentRect:styleMask:backing:defer:"), 100.0, 100.0,
        (double)vm->scr_w, (double)vm->scr_h, 1 | 2 | 4, 2, 0); /* titled, closable, miniaturizable; buffered */
    if (!window)
        return -1;
    ML(window, "setReleasedWhenClosed:", 0);
    ML(window, "setAcceptsMouseMovedEvents:", 1);
    sprintf(name, "mvm %ux%u", vm->scr_w, vm->scr_h);
    title = M1(objc_getClass("NSString"), "stringWithUTF8String:", name);
    M1(window, "setTitle:", title);
    mode = M0(M1(objc_getClass("NSString"), "stringWithUTF8String:", "kCFRunLoopDefaultMode"), "retain");
    view = M0(window, "contentView");
    ML(view, "setWantsLayer:", 1);
    layer = M0(view, "layer");
    M1(layer, "setMagnificationFilter:", M1(objc_getClass("NSString"), "stringWithUTF8String:", "nearest"));
    space = CGColorSpaceCreateDeviceRGB();
    shown = (u32 *)calloc((size_t)vm->scr_w * vm->scr_h, 4);
    M0(app, "finishLaunching");
    M1(window, "makeKeyAndOrderFront:", 0);
    ML(app, "activateIgnoringOtherApps:", 1);
    M0(pool, "drain");
    vm->gui = 1;
    vm->scr_dirty = 1;
    return 0;
}

static void show(Vm *vm)
{
    size_t n = (size_t)vm->scr_w * vm->scr_h * 4;
    id prov, img;
    memcpy(shown, vm->scr, n);
    prov = CGDataProviderCreateWithData(0, shown, n, 0);
    /* 8 bits a colour, 32 a pixel, no alpha, little-endian words: 0x00RRGGBB */
    img = CGImageCreate(vm->scr_w, vm->scr_h, 8, 32, (unsigned long)vm->scr_w * 4, space, 6 | 2 << 12, prov, 0, 0, 0);
    M1(layer, "setContents:", img);
    CGImageRelease(img);
    CGDataProviderRelease(prov);
}

static void modifier(Vm *vm, unsigned long now, unsigned long bit, int code)
{
    if ((now ^ flags) & bit)
        input_key(vm, code, (now & bit) != 0);
}

void gui_poll(Vm *vm)
{
    id pool, ev;
    if (!window)
        return;
    pool = M0(M0(objc_getClass("NSAutoreleasePool"), "alloc"), "init");
    for (;;) {
        long type, code;
        double xy[2];
        ev = ((id (*)(id, id, unsigned long, id, id, int))objc_msgSend)(
            app, SEL("nextEventMatchingMask:untilDate:inMode:dequeue:"), ~0UL, 0, mode, 1);
        if (!ev)
            break;
        type = L0(ev, "type");
        switch (type) {
        case 10: /* key down, key up */
        case 11:
            code = L0(ev, "keyCode") & 0xffff;
            if ((L0(ev, "modifierFlags") & (1L << 20)) && type == 10) { /* command: the window's own keys */
                if (code == 12) /* q */
                    vm->poweroff = 1, vm->status = 0;
                break;
            }
            if (code < 128 && keymap[code])
                input_key(vm, keymap[code], type == 10 ? (L0(ev, "isARepeat") ? 2 : 1) : 0);
            break;
        case 12: { /* a modifier went down or up */
            unsigned long now = (unsigned long)L0(ev, "modifierFlags");
            modifier(vm, now, 1UL << 17, 42);
            modifier(vm, now, 1UL << 18, 29);
            modifier(vm, now, 1UL << 19, 56);
            modifier(vm, now, 1UL << 16, 58);
            flags = now;
            break;
        }
        case 1: /* the mouse: buttons, movement, wheel */
        case 2:
        case 3:
        case 4:
        case 25:
        case 26:
        case 5:
        case 6:
        case 7:
        case 27:
            gui_msg_point(ev, SEL("locationInWindow"), xy);
            code = M0(ev, "window") == window && xy[0] >= 0 && xy[1] >= 0 && xy[0] < vm->scr_w && xy[1] < vm->scr_h;
            if (code != inside) /* over the screen the guest's pointer is the one to see */
                M0(objc_getClass("NSCursor"), code ? "hide" : "unhide");
            inside = (int)code;
            if (inside) {
                input_pointer(vm, (int)(xy[0] * 32767 / (vm->scr_w - 1)),
                              (int)(((double)vm->scr_h - 1 - xy[1]) * 32767 / (vm->scr_h - 1)));
                if (type == 1 || type == 2)
                    input_button(vm, 0x110, type == 1);
                else if (type == 3 || type == 4)
                    input_button(vm, 0x111, type == 3);
                else if (type == 25 || type == 26)
                    input_button(vm, 0x112, type == 25);
            }
            M1(app, "sendEvent:", ev);
            break;
        case 22: { /* the wheel */
            double dy = ((double (*)(id, id))objc_msgSend)(ev, SEL("scrollingDeltaY"));
            if (inside && dy != 0)
                input_wheel(vm, dy > 0 ? 1 : -1);
            break;
        }
        default: M1(app, "sendEvent:", ev);
        }
    }
    if (!L0(window, "isVisible"))
        vm->poweroff = 1, vm->status = 0;
    else if (vm->scr_dirty) {
        vm->scr_dirty = 0;
        show(vm);
    }
    M0(pool, "drain");
}
