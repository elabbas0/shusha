/* syncd: saves the file tree to the state store every few seconds.
 *
 * The kernel no longer writes the disk on every file change (each save
 * re-encrypts the whole tree, kernel/fs/store.c); it only counts changes.
 * syncd asks it to save about every 3 s.  The store skips the save when
 * nothing changed, so an idle machine never touches the disk.  Power off
 * and restart save first as well. */
#include "lib.h"

#define SYNCD_QUEUE "syncd"

int main(void) {
    uint64_t q = sh_msg_open(SYNCD_QUEUE);
    if (!q || sh_msg_owner(q) != (long)sh_pid()) return 0;  /* already running */
    for (;;) {
        sh_sleep(300);
        (void)sh_sync();
    }
}
