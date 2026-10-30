#include "shm.h"
#include "../memory/pmm.h"
#include "../memory/vmm.h"
#include "../proc/sched.h"
#include "../crypto/random.h"
#include <stdint.h>

/* Handles are capabilities: (48-bit random tag << 16) | (slot + 1).
 * Knowing a slot number is not enough to map someone else's region; a
 * process needs the exact handle, which only travels over IPC to the
 * processes the creator chose (e.g. the WM hands a window's buffer
 * handle to that window's client alone). */

#define SHM_PAGE_SZ   4096ULL
#define SHM_MAX_PAGES 4096   /* 4096*4096 = 16 MiB max per region */

typedef struct {
    int      valid;
    uint64_t size;
    uint64_t num_pages;
    uint64_t phys_pages[SHM_MAX_PAGES];
    uint32_t ref_count;
    uint64_t tag;
} shm_region_t;

static shm_region_t shm_table[SHM_MAX_REGIONS];

#define SHM_SLOT_BITS 16

/* Slot for a handle, or NULL when the handle is unknown or forged. */
static shm_region_t *lookup(uint64_t handle, uint64_t *slot_out) {
    uint64_t slot = (handle & ((1ULL << SHM_SLOT_BITS) - 1)) - 1;
    shm_region_t *r;
    if ((handle & ((1ULL << SHM_SLOT_BITS) - 1)) == 0 || slot >= SHM_MAX_REGIONS) return 0;
    r = &shm_table[slot];
    if (!r->valid || r->tag != (handle >> SHM_SLOT_BITS)) return 0;
    if (slot_out) *slot_out = slot;
    return r;
}

uint64_t shm_create(uint64_t size) {
    if (size == 0 || size > (uint64_t)SHM_MAX_PAGES * SHM_PAGE_SZ) return 0;

    uint64_t idx;
    for (idx = 0; idx < SHM_MAX_REGIONS; idx++) {
        if (!shm_table[idx].valid) break;
    }
    if (idx >= SHM_MAX_REGIONS) return 0;

    uint64_t num_pages = (size + SHM_PAGE_SZ - 1) / SHM_PAGE_SZ;
    shm_region_t *r = &shm_table[idx];

    for (uint64_t i = 0; i < num_pages; i++) {
        uint64_t phys = pmm_alloc();
        if (!phys) {
            for (uint64_t j = 0; j < i; j++) pmm_free(r->phys_pages[j]);
            return 0;
        }
        /* zero page */
        uint8_t *p = (uint8_t *)PHYS_TO_VIRT(phys);
        for (uint64_t b = 0; b < SHM_PAGE_SZ; b++) p[b] = 0;
        r->phys_pages[i] = phys;
    }

    r->tag = 0;
    while (r->tag == 0) {
        if (random_get(&r->tag, sizeof(r->tag)) != 0) {
            for (uint64_t j = 0; j < num_pages; j++) pmm_free(r->phys_pages[j]);
            return 0;
        }
        r->tag &= (1ULL << (64 - SHM_SLOT_BITS)) - 1;
    }
    r->valid     = 1;
    r->size      = size;
    r->num_pages = num_pages;
    r->ref_count = 0;
    return (r->tag << SHM_SLOT_BITS) | (idx + 1);
}

uint64_t shm_map(uint64_t handle) {
    uint64_t slot;
    shm_region_t *r = lookup(handle, &slot);
    if (!r) return 0;

    process_t *proc = sched_current_process();
    if (!proc || !proc->addr_space) return 0;

    uint64_t virt_base = SHM_VIRT_BASE + slot * SHM_SLOT_SIZE;

    for (uint64_t i = 0; i < r->num_pages; i++) {
        if (vmm_map_page(proc->addr_space,
                         virt_base + i * SHM_PAGE_SZ,
                         r->phys_pages[i],
                         VMM_FLAGS_USER_RW) != 0) {
            /* unmap already-mapped pages on failure */
            for (uint64_t j = 0; j < i; j++)
                vmm_unmap_page(proc->addr_space, virt_base + j * SHM_PAGE_SZ, 0);
            return 0;
        }
    }

    r->ref_count++;
    return virt_base;
}

int shm_unmap(uint64_t handle) {
    uint64_t slot;
    shm_region_t *r = lookup(handle, &slot);
    if (!r) return -1;

    process_t *proc = sched_current_process();
    if (!proc || !proc->addr_space) return -1;

    uint64_t virt_base = SHM_VIRT_BASE + slot * SHM_SLOT_SIZE;
    for (uint64_t i = 0; i < r->num_pages; i++)
        vmm_unmap_page(proc->addr_space, virt_base + i * SHM_PAGE_SZ, 0);

    if (r->ref_count > 0) r->ref_count--;
    return 0;
}

int shm_close(uint64_t handle) {
    shm_region_t *r = lookup(handle, 0);
    if (!r) return -1;

    if (r->ref_count > 0) r->ref_count--;
    if (r->ref_count == 0) {
        for (uint64_t i = 0; i < r->num_pages; i++)
            pmm_free(r->phys_pages[i]);
        r->valid     = 0;
        r->size      = 0;
        r->num_pages = 0;
        r->tag       = 0;
    }
    return 0;
}

uint64_t shm_size(uint64_t handle) {
    shm_region_t *r = lookup(handle, 0);
    return r ? r->size : 0;
}
