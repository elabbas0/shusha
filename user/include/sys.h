#ifndef SHUSHA_SYS_H
#define SHUSHA_SYS_H

/* Shusha syscall layer.  Numbers are the kernel's frozen native ABI
 * (docs/ABI-v1.md); only the calls Shusha userspace uses are wrapped. */

#include <stdint.h>

enum {
    SYS_CONSOLE_WRITE  = 0,
    SYS_GET_PID        = 1,
    SYS_VFS_READ       = 2,
    SYS_VFS_WRITE      = 3,
    SYS_EXIT           = 4,
    SYS_GETCWD         = 6,
    SYS_CHDIR          = 7,
    SYS_LIST_DIR       = 8,
    SYS_CONSOLE_CLEAR  = 10,
    SYS_MKDIR          = 12,
    SYS_CREATE         = 13,
    SYS_STAT           = 14,
    SYS_LIST_PROCS     = 15,
    SYS_SPAWN          = 16,
    SYS_WAITPID        = 17,
    SYS_YIELD          = 18,
    SYS_SLEEP          = 19,
    SYS_INPUT_READLINE = 24,
    SYS_SYNC           = 25,
    SYS_TICKS          = 36,
    SYS_VFS_READ_AT    = 38,
    SYS_SPAWN_ARGS     = 53,
    SYS_POWER          = 68
};

typedef struct {
    uint64_t inode;
    uint64_t size;
    uint64_t created;
    uint64_t modified;
    uint8_t  type;
    uint8_t  readonly;
} sh_stat_t;

#define SH_NODE_DIR 1

static inline uint64_t sys0(uint64_t n) {
    uint64_t r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n) : "rcx", "r11", "memory");
    return r;
}

static inline uint64_t sys1(uint64_t n, uint64_t a) {
    uint64_t r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a) : "rcx", "r11", "memory");
    return r;
}

static inline uint64_t sys2(uint64_t n, uint64_t a, uint64_t b) {
    uint64_t r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b) : "rcx", "r11", "memory");
    return r;
}

static inline uint64_t sys3(uint64_t n, uint64_t a, uint64_t b, uint64_t c) {
    uint64_t r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return r;
}

static inline uint64_t sys4(uint64_t n, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    uint64_t r;
    register uint64_t r10 __asm__("r10") = d;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10) : "rcx", "r11", "memory");
    return r;
}

#define U(p) ((uint64_t)(uintptr_t)(p))

static inline void     sh_write(const char *s)                  { (void)sys1(SYS_CONSOLE_WRITE, U(s)); }
static inline uint64_t sh_pid(void)                             { return sys0(SYS_GET_PID); }
static inline long     sh_read(const char *p, char *b, uint64_t cap) { return (long)sys3(SYS_VFS_READ, U(p), U(b), cap); }
static inline long     sh_read_at(const char *p, uint64_t off, char *b, uint64_t cap) { return (long)sys4(SYS_VFS_READ_AT, U(p), off, U(b), cap); }
static inline long     sh_write_file(const char *p, const void *b, uint64_t n) { return (long)sys3(SYS_VFS_WRITE, U(p), U(b), n); }
static inline long     sh_getcwd(char *b, uint64_t cap)         { return (long)sys2(SYS_GETCWD, U(b), cap); }
static inline long     sh_chdir(const char *p)                  { return (long)sys1(SYS_CHDIR, U(p)); }
static inline long     sh_list_dir(const char *p, char *b, uint64_t cap) { return (long)sys3(SYS_LIST_DIR, U(p), U(b), cap); }
static inline void     sh_clear(void)                           { (void)sys0(SYS_CONSOLE_CLEAR); }
static inline long     sh_mkdir(const char *p)                  { return (long)sys1(SYS_MKDIR, U(p)); }
static inline long     sh_create(const char *p)                 { return (long)sys1(SYS_CREATE, U(p)); }
static inline long     sh_stat(const char *p, sh_stat_t *st)    { return (long)sys2(SYS_STAT, U(p), U(st)); }
static inline long     sh_list_procs(char *b, uint64_t cap)     { return (long)sys2(SYS_LIST_PROCS, U(b), cap); }
static inline long     sh_spawn(const char *p)                  { return (long)sys1(SYS_SPAWN, U(p)); }
static inline long     sh_spawn_args(const char *p, const char *a) { return (long)sys2(SYS_SPAWN_ARGS, U(p), U(a)); }
static inline long     sh_waitpid(uint64_t pid)                 { return (long)sys1(SYS_WAITPID, pid); }
static inline void     sh_sleep(uint64_t ticks)                 { (void)sys1(SYS_SLEEP, ticks); }
static inline long     sh_readline(char *b, uint64_t cap)       { return (long)sys2(SYS_INPUT_READLINE, U(b), cap); }
static inline long     sh_sync(void)                            { return (long)sys0(SYS_SYNC); }
static inline uint64_t sh_ticks(void)                           { return sys0(SYS_TICKS); }
static inline long     sh_power(uint64_t op)                    { return (long)sys1(SYS_POWER, op); }

#undef U

#endif
