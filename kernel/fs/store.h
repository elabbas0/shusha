#ifndef SHUSHA_STORE_H
#define SHUSHA_STORE_H

#include <stdint.h>

/* Shusha's state store (store.c): what a machine keeps between boots -
 * accounts, homes, the workspace - encrypted on its own GPT partition and
 * bound to the machine's hardware.  Read and controlled through
 * /dev/store (see docs/STORAGE.md). */

enum {
    STORE_NONE = 0,     /* no Shusha disk found (live session) */
    STORE_UNLOCKED,     /* loaded; changes are saved */
    STORE_LOCKED,       /* found, but the hardware no longer matches */
    STORE_DAMAGED       /* found, but unreadable */
};

/* Boot: `cmdline` is the kernel command line (shusha.uuid=... from GRUB).
 * Finds the state partition, unlocks it and loads the saved tree. */
void store_init(const char *cmdline, int live);
int  store_state(void);
/* vfs sync hook: saves the tree if it changed since the last save. */
int  store_sync(void);
/* /dev/store: status text (root sees more) and root-only commands. */
long store_status_text(char *buf, uint64_t cap, int root);
long store_command(const char *cmd, int root);
/* The device the store lives on (formatting it is refused), or -1. */
int  store_device(void);

#endif
