#ifndef BLOCK_H
#define BLOCK_H

#include <stdint.h>

typedef struct block_device block_device_t;

typedef int (*block_read_fn)(void *context, uint64_t lba, uint32_t count, void *buffer);
typedef int (*block_write_fn)(void *context, uint64_t lba, uint32_t count, const void *buffer);

struct block_device {
    const char *name;
    void *context;
    uint32_t sector_size;
    uint64_t sector_count;
    block_read_fn read;
    block_write_fn write;
    /* Shusha: the drive's serial number from IDENTIFY (ASCII, trimmed,
     * "" when unknown).  One of the hardware IDs the state store binds
     * its key to. */
    char serial[24];
};

/* IDENTIFY words 10-19 hold the serial as byte-swapped ASCII pairs. */
void block_serial_from_identify(char out[24], const uint16_t *identify);

int block_register(block_device_t *device);
uint32_t block_count(void);
block_device_t *block_get(uint32_t index);
block_device_t *block_find(const char *name);

#endif
