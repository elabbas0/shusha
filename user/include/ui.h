#ifndef SHUSHA_UI_H
#define SHUSHA_UI_H

/* Client side of the window system.
 *
 *   ui_open("Title", w, h);
 *   draw into ui_surface()          (private back buffer)
 *   ui_present_rect(x, y, w, h);    (copy that rect to the WM and repaint)
 *   while (ui_poll(&ev)) ...
 */

#include <stdint.h>
#include "draw.h"
#include "ui_proto.h" /* UI_BTN_* */

enum { UI_EV_KEY = 1, UI_EV_MOUSE, UI_EV_CLOSE, UI_EV_FOCUS };

typedef struct {
    int type;
    uint32_t key;      /* UI_EV_KEY: input byte (escape sequences for arrows) */
    int x, y;          /* UI_EV_MOUSE: window-local */
    uint32_t buttons;  /* UI_EV_MOUSE */
    int focused;       /* UI_EV_FOCUS */
} ui_event_t;

int        ui_open(const char *title, int w, int h);
surface_t *ui_surface(void);
void       ui_present(void);
void       ui_present_rect(int x, int y, int w, int h);
/* Returns 1 and fills *ev when an event is waiting, else 0. */
int        ui_poll(ui_event_t *ev);
/* Sleep until an event arrives (polling at the scheduler tick). */
void       ui_wait(ui_event_t *ev);
void       ui_close(void);

#endif
