#ifndef SHUSHA_UI_PROTO_H
#define SHUSHA_UI_PROTO_H

/* Shusha window-system protocol.
 *
 * Every message is 64 bytes (the kernel message-queue slot size).
 * Clients send to the WM's queue (UI_WM_QUEUE); the WM replies on the
 * client's queue "/ui/p<pid>".  Pixels travel through shared memory:
 * the WM creates one 32bpp 0x00RRGGBB buffer per window and the client
 * maps it.  Clients draw privately and copy only the damaged rectangle
 * into shared memory before sending UI_DAMAGE, so the WM never composites
 * a half-drawn frame and never repaints more than changed. */

#include <stdint.h>

#define UI_WM_QUEUE "/ui/wm"

enum {
    UI_OPEN = 1,     /* client -> wm: w, h, pid, title          */
    UI_OPEN_OK,      /* wm -> client: window id, shm handle, w, h */
    UI_OPEN_FAIL,    /* wm -> client                             */
    UI_DAMAGE,       /* client -> wm: rect changed in the buffer */
    UI_CLOSE,        /* both directions                          */
    UI_KEY,          /* wm -> client: one input byte             */
    UI_MOUSE,        /* wm -> client: window-local x, y, buttons */
    UI_FOCUS         /* wm -> client: focused 0/1                */
};

#define UI_BTN_LEFT   1
#define UI_BTN_RIGHT  2
#define UI_BTN_MIDDLE 4

#define UI_TITLE_MAX 32

typedef struct {
    uint32_t type;
    uint32_t window;
    union {
        struct { int32_t w, h; uint32_t pid; char title[UI_TITLE_MAX]; } open;
        struct { uint64_t shm; int32_t w, h; } open_ok;
        struct { int32_t x, y, w, h; } damage;
        struct { uint32_t code; } key;
        struct { int32_t x, y; uint32_t buttons; } mouse;
        struct { uint32_t focused; } focus;
        uint8_t raw[56];
    };
} ui_msg_t;

typedef char ui_msg_size_check[sizeof(ui_msg_t) == 64 ? 1 : -1];

#endif
