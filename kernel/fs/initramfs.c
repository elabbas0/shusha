#include "initramfs.h"
#include "vfs.h"

#include <stdint.h>

/* Shusha initramfs: a few static text files plus every userspace
 * program the build embeds.  The program list is not hard-coded here:
 * the Makefile generates build/gen/userprogs.{asm,c} from the
 * USERPROGS manifest and links initramfs_blobs[] in. */

extern const initramfs_file_t initramfs_blobs[];
extern const uint64_t initramfs_blob_count;

static const char motd_txt[] =
    "welcome to shusha\n"
    "\n"
    "a security-first, multi-user operating system built on the ICDA kernel.\n"
    "type 'help' for commands.\n";

static const char files_txt[] =
    "mounted roots:\n"
    "  /etc      basic system text\n"
    "  /sbin     system programs (init)\n"
    "  /apps     programs\n"
    "  /home     default writable user area\n"
    "  /volumes  detected filesystem mounts\n";

static const initramfs_file_t initramfs_text[] = {
    { "/etc/motd.txt", motd_txt, sizeof(motd_txt) - 1 },
    { "/etc/files.txt", files_txt, sizeof(files_txt) - 1 },
};

#define TEXT_COUNT (sizeof(initramfs_text) / sizeof(initramfs_text[0]))

static int path_eq(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

int initramfs_init(void) {
    return 0;
}

int initramfs_populate(void) {
    for (uint64_t i = 0; i < initramfs_file_count(); i++) {
        const initramfs_file_t *f = initramfs_file_at(i);
        if (vfs_seed_readonly(f->path, f->data, f->size) != 0) {
            return -1;
        }
    }
    return 0;
}

const initramfs_file_t *initramfs_find(const char *path) {
    if (!path) {
        return 0;
    }
    for (uint64_t i = 0; i < initramfs_file_count(); i++) {
        const initramfs_file_t *f = initramfs_file_at(i);
        if (path_eq(f->path, path)) {
            return f;
        }
    }
    return 0;
}

const initramfs_file_t *initramfs_file_at(uint64_t index) {
    if (index < TEXT_COUNT) {
        return &initramfs_text[index];
    }
    index -= TEXT_COUNT;
    if (index < initramfs_blob_count) {
        return &initramfs_blobs[index];
    }
    return 0;
}

uint64_t initramfs_file_count(void) {
    return TEXT_COUNT + initramfs_blob_count;
}

uint64_t initramfs_total_bytes(void) {
    uint64_t total = 0;

    for (uint64_t i = 0; i < initramfs_file_count(); i++) {
        total += initramfs_file_at(i)->size;
    }
    return total;
}
