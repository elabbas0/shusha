#include "lib.h"

uint64_t str_len(const char *s) {
    uint64_t n = 0;
    while (s[n]) n++;
    return n;
}

int str_eq(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

int str_starts(const char *s, const char *prefix) {
    while (*prefix) {
        if (*s++ != *prefix++) return 0;
    }
    return 1;
}

char *str_copy(char *dst, const char *src, uint64_t cap) {
    uint64_t i = 0;
    if (cap == 0) return dst;
    while (src[i] && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
    return dst;
}

char *str_cat(char *dst, const char *src, uint64_t cap) {
    uint64_t n = str_len(dst);
    if (n < cap) str_copy(dst + n, src, cap - n);
    return dst;
}

void mem_set(void *dst, int v, uint64_t n) {
    uint8_t *d = dst;
    while (n--) *d++ = (uint8_t)v;
}

void mem_copy(void *dst, const void *src, uint64_t n) {
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n--) *d++ = *s++;
}

/* Constant-time comparison: never branch on secret bytes. */
int mem_eq_ct(const void *a, const void *b, uint64_t n) {
    const uint8_t *x = a, *y = b;
    uint8_t diff = 0;
    for (uint64_t i = 0; i < n; i++) diff |= x[i] ^ y[i];
    return diff == 0;
}

/* Wipe that the compiler cannot drop as a dead store. */
void mem_wipe(void *dst, uint64_t n) {
    volatile uint8_t *d = dst;
    while (n--) *d++ = 0;
}

char *fmt_u64(char *buf, uint64_t v, int base) {
    static const char digits[] = "0123456789abcdef";
    char tmp[24];
    int i = 0, j = 0;
    do {
        tmp[i++] = digits[v % (uint64_t)base];
        v /= (uint64_t)base;
    } while (v);
    while (i) buf[j++] = tmp[--i];
    buf[j] = '\0';
    return buf;
}

void print(const char *s) {
    sh_write(s);
}

void print_u64(uint64_t v) {
    char buf[24];
    print(fmt_u64(buf, v, 10));
}

void print_hex(const uint8_t *data, uint64_t n) {
    static const char digits[] = "0123456789abcdef";
    char buf[65];
    uint64_t o = 0;
    for (uint64_t i = 0; i < n; i++) {
        buf[o++] = digits[data[i] >> 4];
        buf[o++] = digits[data[i] & 15];
        if (o == 64) {
            buf[o] = '\0';
            print(buf);
            o = 0;
        }
    }
    buf[o] = '\0';
    print(buf);
}

int split_args(char *line, char **argv, int max) {
    int argc = 0;
    while (*line && argc < max) {
        while (*line == ' ') *line++ = '\0';
        if (!*line) break;
        argv[argc++] = line;
        while (*line && *line != ' ') line++;
    }
    return argc;
}

long sh_getuid(void) {
    char buf[24];
    long n = sh_read("/dev/setuid", buf, sizeof(buf));
    long v = 0;
    if (n <= 0) return -1;
    for (int i = 0; buf[i] >= '0' && buf[i] <= '9'; i++) v = v * 10 + (buf[i] - '0');
    return v;
}

static int control(const char *dev, const char *text) {
    return sh_write_file(dev, text, str_len(text)) < 0 ? -1 : 0;
}

int sh_setuid(uint64_t uid) {
    char buf[24];
    return control("/dev/setuid", fmt_u64(buf, uid, 10));
}

int sh_chmod(const char *path, uint64_t mode) {
    char buf[160];
    fmt_u64(buf, mode, 8);
    str_cat(buf, " ", sizeof(buf));
    str_cat(buf, path, sizeof(buf));
    return control("/dev/chmod", buf);
}

int sh_chown(const char *path, uint64_t uid) {
    char buf[160];
    fmt_u64(buf, uid, 10);
    str_cat(buf, " ", sizeof(buf));
    str_cat(buf, path, sizeof(buf));
    return control("/dev/chown", buf);
}

char *fmt_mode(char *buf, uint32_t mode) {
    static const char rwx[] = "rwxrwxrwx";
    for (int i = 0; i < 9; i++) buf[i] = (mode & (0400u >> i)) ? rwx[i] : '-';
    buf[9] = '\0';
    return buf;
}
