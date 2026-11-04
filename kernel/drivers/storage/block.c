#include "block.h"

#define BLOCK_MAX_DEVICES 8

static block_device_t *block_devices[BLOCK_MAX_DEVICES];
static uint32_t block_device_count = 0;

static int str_eq(const char *a, const char *b) {
    uint64_t i = 0;
    if (!a || !b) return 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return 0;
        i++;
    }
    return a[i] == 0 && b[i] == 0;
}

int block_register(block_device_t *device) {
    for (uint32_t i = 0; i < block_device_count; i++) {
        if (block_devices[i] == device) {
            return 0;
        }
    }
    if (!device || !device->read || block_device_count >= BLOCK_MAX_DEVICES) {
        return -1;
    }
    block_devices[block_device_count++] = device;
    return 0;
}

uint32_t block_count(void) {
    return block_device_count;
}

block_device_t *block_get(uint32_t index) {
    if (index >= block_device_count) {
        return 0;
    }
    return block_devices[index];
}

block_device_t *block_find(const char *name) {
    for (uint32_t i = 0; i < block_device_count; i++) {
        if (str_eq(block_devices[i]->name, name)) {
            return block_devices[i];
        }
    }
    return 0;
}

void block_serial_from_identify(char out[24], const uint16_t *identify) {
    char raw[21];
    int n = 0, start = 0, end;
    for (int w = 10; w < 20; w++) {
        raw[n++] = (char)(identify[w] >> 8);
        raw[n++] = (char)(identify[w] & 0xff);
    }
    raw[20] = 0;
    while (start < 20 && raw[start] == ' ') start++;
    end = 20;
    while (end > start && (raw[end - 1] == ' ' || raw[end - 1] == 0)) end--;
    n = 0;
    for (int i = start; i < end && n < 23; i++) {
        char c = raw[i];
        out[n++] = (c >= 0x21 && c <= 0x7e) ? c : '?';
    }
    out[n] = 0;
}
