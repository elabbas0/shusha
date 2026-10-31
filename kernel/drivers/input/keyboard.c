#include "keyboard.h"

#include "../device.h"
#include "../../proc/sched.h"
#include "../../vt/vt.h"

#include <stdint.h>

#define KEYBOARD_DATA_PORT   0x60
#define KEYBOARD_STATUS_PORT 0x64
#define KEYBOARD_QUEUE_SIZE  128

static char keymap[128] = {
    0,
    27,
    '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=',
    '\b', '\t',
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0,
    '\\',
    'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',
    0,
    '*',
    0,
    ' ',
};

static char keymap_shift[128] = {
    0,
    27,
    '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+',
    '\b', '\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0,
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0,
    '|',
    'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',
    0,
    '*',
    0,
    ' ',
};

static volatile char queue[KEYBOARD_QUEUE_SIZE];
static volatile uint32_t queue_head = 0;
static volatile uint32_t queue_tail = 0;
static int shift_down = 0;
static int ctrl_down = 0;
static int alt_down = 0;
static int caps_lock = 0;
static int extended_prefix = 0;
static int f12_down = 0;
static kernel_device_t keyboard_device;

static int keyboard_device_has_char(void *context) {
    (void)context;
    return keyboard_has_char();
}

static int keyboard_device_read_char(void *context) {
    (void)context;
    return keyboard_read_char();
}

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void queue_push(char c) {
    uint32_t next = (queue_head + 1) % KEYBOARD_QUEUE_SIZE;
    if (next == queue_tail) {
        return;
    }

    queue[queue_head] = c;
    queue_head = next;
}

static void queue_push_seq(const char *seq) {
    while (*seq) {
        queue_push(*seq++);
    }
}

static int is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static char apply_ctrl(char c) {
    if (c >= 'a' && c <= 'z') {
        return (char)(c - 'a' + 1);
    }
    if (c >= 'A' && c <= 'Z') {
        return (char)(c - 'A' + 1);
    }
    return c;
}

/* ---- escape sequences (xterm conventions) ----------------------------
 * Plain cursor keys keep the short forms (ESC [ A) every app already
 * understands; with modifiers they carry xterm's parameter
 * 1 + shift + 2*alt + 4*ctrl (ESC [ 1 ; 2 A is Shift+Up).  Keys with no
 * byte of their own - Ctrl+/, Ctrl+Space, Ctrl+Shift+letter - use the
 * "CSI u" form ESC [ <code> ; <mods> u.  Editors need all of these. */

static int key_mods(void) {
    return 1 + shift_down + 2 * alt_down + 4 * ctrl_down;
}

static void push_num(int v) {
    char d[8];
    int n = 0;
    do {
        d[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && n < 8);
    while (n) queue_push(d[--n]);
}

/* ESC [ X  or  ESC [ 1 ; m X */
static void push_cursor(char final) {
    int m = key_mods();
    queue_push_seq("\x1b[");
    if (m != 1) {
        queue_push_seq("1;");
        push_num(m);
    }
    queue_push(final);
}

/* ESC [ n ~  or  ESC [ n ; m ~ */
static void push_tilde(int n) {
    int m = key_mods();
    queue_push_seq("\x1b[");
    push_num(n);
    if (m != 1) {
        queue_push(';');
        push_num(m);
    }
    queue_push('~');
}

/* ESC [ code ; m u */
static void push_csi_u(int code) {
    queue_push_seq("\x1b[");
    push_num(code);
    queue_push(';');
    push_num(key_mods());
    queue_push('u');
}

static char translate_scancode(uint8_t scancode) {
    char c;

    if (scancode >= 128) {
        return 0;
    }

    c = shift_down ? keymap_shift[scancode] : keymap[scancode];
    if (!c) {
        return 0;
    }

    if (caps_lock && is_alpha(c)) {
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        }
        else if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
    }

    if (ctrl_down && is_alpha(c)) {
        return apply_ctrl(c);
    }

    return c;
}

static void keyboard_handle_scancode(uint8_t scancode) {
    char c;

    if (scancode == 0xE0 || scancode == 0xE1) {
        extended_prefix = 1;
        return;
    }

    if (extended_prefix) {
        extended_prefix = 0;
        /* Right Ctrl / Right Alt behave like the left ones. */
        if (scancode == 0x1D) { ctrl_down = 1; return; }
        if (scancode == 0x9D) { ctrl_down = 0; return; }
        if (scancode == 0x38) { alt_down = 1; return; }
        if (scancode == 0xB8) { alt_down = 0; return; }
        if (scancode & 0x80) {
            return;
        }
        switch (scancode) {
            case 0x48: push_cursor('A'); break;
            case 0x50: push_cursor('B'); break;
            case 0x4D: push_cursor('C'); break;
            case 0x4B: push_cursor('D'); break;
            case 0x47: push_cursor('H'); break;   /* Home */
            case 0x4F: push_cursor('F'); break;   /* End */
            case 0x52: push_tilde(2); break;      /* Insert */
            case 0x53: push_tilde(3); break;      /* Delete */
            case 0x49: push_tilde(5); break;      /* Page Up */
            case 0x51: push_tilde(6); break;      /* Page Down */
            case 0x1C: queue_push('\n'); break;   /* keypad Enter */
            case 0x35: queue_push('/'); break;    /* keypad / */
            default: return;
        }
        sched_wake_input_waiters();
        return;
    }

    if (scancode == 0x2A || scancode == 0x36) {
        shift_down = 1;
        return;
    }

    if (scancode == 0x1D) {
        ctrl_down = 1;
        return;
    }

    if (scancode == 0x38) {
        alt_down = 1;
        return;
    }

    if (scancode == 0xAA || scancode == 0xB6) {
        shift_down = 0;
        return;
    }

    if (scancode == 0x9D) {
        ctrl_down = 0;
        return;
    }

    if (scancode == 0xB8) {
        alt_down = 0;
        return;
    }

    /* Ctrl+Alt+F1..F6 switch virtual terminals (Linux-style).  F-keys are
     * not in the keymap so nothing else would consume them. */
    if (ctrl_down && alt_down && scancode >= 0x3B && scancode <= 0x40) {
        vt_request_switch((int)(scancode - 0x3B + 1));
        return;
    }

    /* F12 (make 0x58): emit 0x80 sentinel ONCE per press.  The WM
     * intercepts it; the console/tty never sees it.  Release (0xD8)
     * clears the flag so the next press fires again. */
    if (scancode == 0x58) {
        if (!f12_down) {
            f12_down = 1;
            queue_push((char)0x80);
            sched_wake_input_waiters();
        }
        return;
    }
    if (scancode == 0xD8) {
        f12_down = 0;
        return;
    }

    if (scancode == 0x3A) {
        caps_lock = !caps_lock;
        return;
    }

    if (scancode & 0x80) {
        return;
    }

    /* F1-F4: ESC O P..S; F5-F11: ESC [ 15/17/18/19/20/21/23 ~ */
    if ((scancode >= 0x3B && scancode <= 0x44) || scancode == 0x57) {
        static const int tilde[] = { 15, 17, 18, 19, 20, 21 };
        if (scancode <= 0x3E && key_mods() == 1) {
            queue_push_seq("\x1bO");
            queue_push((char)('P' + scancode - 0x3B));
        } else if (scancode <= 0x3E) {
            push_cursor((char)('P' + scancode - 0x3B));
        } else {
            push_tilde(scancode == 0x57 ? 23 : tilde[scancode - 0x3F]);
        }
        sched_wake_input_waiters();
        return;
    }
    /* Shift+Tab */
    if (scancode == 0x0F && shift_down) {
        queue_push_seq("\x1b[Z");
        sched_wake_input_waiters();
        return;
    }
    /* Ctrl with a symbol or Space, and Ctrl+Shift+letter, have no byte of
     * their own: send them in CSI u form. */
    if (ctrl_down && scancode < 128 && keymap[scancode] > ' ' - 1 && keymap[scancode] != 27 &&
        (!is_alpha(keymap[scancode]) || shift_down) && keymap[scancode] != '\b' && keymap[scancode] != '\t' &&
        keymap[scancode] != '\n') {
        push_csi_u(keymap[scancode]);
        sched_wake_input_waiters();
        return;
    }

    c = translate_scancode(scancode);
    if (c) {
        queue_push(c);
        sched_wake_input_waiters();
    }
}

static void keyboard_poll_hardware(void) {
    /* Only read when the pending byte is *keyboard* data. The 8042 status
     * port sets OBF (bit 0) for both devices; bit 5 selects the mouse.
     * Reading without checking it steals mouse packet bytes out from
     * under the mouse driver, desyncing its packet assembler. */
    uint8_t status;
    while (((status = inb(KEYBOARD_STATUS_PORT)) & 0x01) && !(status & 0x20)) {
        uint8_t scancode = inb(KEYBOARD_DATA_PORT);
        keyboard_handle_scancode(scancode);
    }
}

void keyboard_pump(void) {
    keyboard_poll_hardware();
}

/* USB HID bridge: inject already-decoded chars/sequences into the shared
 * queue. Mirrors the wakeup behavior of the PS/2 path. */
void keyboard_usb_push(char c) {
    queue_push(c);
    sched_wake_input_waiters();
}

void keyboard_usb_push_seq(const char *seq) {
    if (!seq) {
        return;
    }
    queue_push_seq(seq);
    sched_wake_input_waiters();
}

void keyboard_init(void) {
    queue_head = 0;
    queue_tail = 0;
    shift_down = 0;
    ctrl_down = 0;
    alt_down = 0;
    caps_lock = 0;
    extended_prefix = 0;
    f12_down = 0;

    while ((inb(KEYBOARD_STATUS_PORT) & 0x01) && !(inb(KEYBOARD_STATUS_PORT) & 0x20)) {
        (void)inb(KEYBOARD_DATA_PORT);
    }

    keyboard_device.name = "keyboard";
    keyboard_device.class_id = DEVICE_CLASS_INPUT;
    static const input_device_ops_t ops = {
        .has_char = keyboard_device_has_char,
        .read_char = keyboard_device_read_char
    };
    keyboard_device.ops = &ops;
    keyboard_device.context = 0;
    keyboard_device.next = 0;
    device_register(&keyboard_device);

    outb(KEYBOARD_STATUS_PORT, 0xAE);
}

void keyboard_irq(struct registers *regs) {
    (void)regs;
    keyboard_poll_hardware();
}

int keyboard_has_char(void) {
    keyboard_poll_hardware();
    return queue_head != queue_tail;
}

int keyboard_read_char(void) {
    char c;

    keyboard_poll_hardware();

    if (queue_head == queue_tail) {
        return -1;
    }

    c = queue[queue_tail];
    queue_tail = (queue_tail + 1) % KEYBOARD_QUEUE_SIZE;
    return (unsigned char)c;
}
