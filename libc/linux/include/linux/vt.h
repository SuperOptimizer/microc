#ifndef _LINUX_VT_H
#define _LINUX_VT_H
#define VT_OPENQRY 0x5600
#define VT_GETSTATE 0x5603
#define VT_ACTIVATE 0x5606
#define VT_WAITACTIVE 0x5607
#define VT_DISALLOCATE 0x5608
struct vt_stat {
    unsigned short v_active, v_signal, v_state;
};
#endif
