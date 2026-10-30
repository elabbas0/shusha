/* ipccheck: tries the IPC attacks the kernel must refuse, from an
 * ordinary process.  Run it from the Terminal while the desktop is up. */
#include "lib.h"
#include "ui_proto.h"

static int fails;

static void expect(int ok, const char *what) {
    print(ok ? "  ok    " : "  FAIL  ");
    print(what);
    print("\n");
    if (!ok) fails++;
}

int main(void) {
    char name[48];
    uint8_t msg[64];
    uint64_t wm, mine, h;
    int mapped = 0;

    print("ipccheck: shared memory\n");
    /* Old-style guessable handles (bare slot numbers) must not map. */
    for (uint64_t slot = 1; slot <= 32; slot++) {
        if (sh_shm_map(slot)) mapped++;
    }
    expect(mapped == 0, "guessing a slot number maps nothing");
    h = sh_shm_create(4096);
    expect(h != 0 && sh_shm_map(h) != 0, "own region maps with its handle");
    expect(sh_shm_map(h ^ (1ULL << 20)) == 0, "a handle with a wrong tag is refused");
    sh_shm_unmap(h);
    sh_shm_close(h);

    print("ipccheck: message queues\n");
    wm = sh_msg_open(UI_WM_QUEUE);
    expect(wm != 0, "anyone can open the WM queue to send");
    expect(sh_msg_recv(wm, msg, 0) == -1, "but cannot read the WM's queue");
    expect(sh_msg_owner(wm) > 0 && sh_msg_owner(wm) != (long)sh_pid(), "the WM owns its queue");
    ui_mailbox_name(name, sizeof(name), 1);
    expect(sh_msg_open(name) == 0 || sh_msg_recv(sh_msg_open(name), msg, 0) == -1,
           "cannot claim or read another process's /proc/<pid>/ mailbox");
    ui_mailbox_name(name, sizeof(name), sh_pid());
    mine = sh_msg_open(name);
    expect(mine != 0, "own /proc/<pid>/ mailbox opens");
    mem_set(msg, 0x5a, sizeof(msg));
    expect(sh_msg_send(mine, msg) == 0 && sh_msg_recv_from(mine, msg) == (long)sh_pid(),
           "receiver learns the kernel-stamped sender pid");

    print(fails ? "ipccheck: FAILURES\n" : "ipccheck: all passed\n");
    return fails ? 1 : 0;
}
