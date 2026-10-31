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
    SYS_INPUT_READ     = 5,
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
    SYS_PROC_INFO      = 20,
    SYS_INPUT_READLINE = 24,
    SYS_SYNC           = 25,
    SYS_TICKS          = 36,
    SYS_VFS_READ_AT    = 38,
    SYS_SPAWN_ARGS     = 53,
    SYS_SHM_CREATE     = 54,
    SYS_SHM_MAP        = 55,
    SYS_SHM_UNMAP      = 56,
    SYS_SHM_CLOSE      = 57,
    SYS_MSG_OPEN       = 58,
    SYS_MSG_SEND       = 59,
    SYS_MSG_RECV       = 60,
    SYS_MSG_POLL       = 61,
    SYS_MAP_FRAMEBUFFER = 62,
    SYS_INPUT_READ_MOUSE = 63,
    SYS_GPU_QUERY      = 65,
    SYS_GPU_PRESENT    = 66,
    SYS_POWER          = 68
};

typedef struct {
    uint64_t virt_addr;
    int32_t  width;
    int32_t  height;
    uint32_t pitch;
    uint32_t bpp;
} sh_fb_info_t;

typedef struct {
    int32_t  abs_x;
    int32_t  abs_y;
    int32_t  dx;
    int32_t  dy;
    uint8_t  buttons;
} sh_mouse_t;

typedef struct {
    char     name[32];
    int32_t  width;
    int32_t  height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t mode_count;
    uint32_t hw_cursor;
    uint32_t present_supported;
    uint32_t flip_active;
    uint32_t needs_present;
} sh_gpu_info_t;

typedef struct {
    uint64_t pid;
    uint64_t ppid;
    uint64_t sid;
    uint64_t pgid;
    uint64_t kind;
    uint64_t state;
    uint64_t exit_code;
} sh_proc_info_t;

#define SH_PROC_EXITED 5

typedef struct {
    uint64_t inode;
    uint64_t size;
    uint64_t created;
    uint64_t modified;
    uint8_t  type;
    uint8_t  readonly;
    uint32_t uid;   /* owner */
    uint16_t mode;  /* 0700 owner bits, 0007 others */
} sh_stat_t;

#define SH_NODE_DIR 2

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

static inline long     sh_read_char(void)                       { return (long)sys0(SYS_INPUT_READ); }
static inline long     sh_proc_info(uint64_t pid, sh_proc_info_t *o) { return (long)sys2(SYS_PROC_INFO, pid, U(o)); }
static inline uint64_t sh_shm_create(uint64_t size)             { return sys1(SYS_SHM_CREATE, size); }
static inline void    *sh_shm_map(uint64_t h)                   { return (void *)(uintptr_t)sys1(SYS_SHM_MAP, h); }
static inline void     sh_shm_unmap(uint64_t h)                 { (void)sys1(SYS_SHM_UNMAP, h); }
static inline void     sh_shm_close(uint64_t h)                 { (void)sys1(SYS_SHM_CLOSE, h); }
static inline uint64_t sh_msg_open(const char *name)            { return sys1(SYS_MSG_OPEN, U(name)); }
static inline int      sh_msg_send(uint64_t q, const void *m)   { return (int)sys2(SYS_MSG_SEND, q, U(m)); }
static inline int      sh_msg_recv(uint64_t q, void *m, int block) { return (int)sys3(SYS_MSG_RECV, q, U(m), (uint64_t)(block & 1)); }
/* Receive and report the sender's pid (kernel-stamped): returns pid > 0, or -1. */
static inline long     sh_msg_recv_from(uint64_t q, void *m)   { return (long)sys3(SYS_MSG_RECV, q, U(m), 2); }
/* pid | uid << 32 of the sender (kernel-stamped), or <= 0 if empty. */
static inline long     sh_msg_recv_cred(uint64_t q, void *m)   { return (long)sys3(SYS_MSG_RECV, q, U(m), 4); }
/* Pid of the process that owns (created) a queue. */
static inline long     sh_msg_owner(uint64_t q)                { return (long)sys2(SYS_MSG_POLL, q, 1); }
static inline uint64_t sh_map_framebuffer(sh_fb_info_t *i)      { return sys1(SYS_MAP_FRAMEBUFFER, U(i)); }
static inline int      sh_read_mouse(sh_mouse_t *m)             { return (int)sys1(SYS_INPUT_READ_MOUSE, U(m)); }
static inline long     sh_gpu_query(sh_gpu_info_t *g)           { return (long)sys1(SYS_GPU_QUERY, U(g)); }
static inline void     sh_gpu_present(void)                     { (void)sys1(SYS_GPU_PRESENT, 0); }
static inline void     sh_exit(int code)                        { (void)sys1(SYS_EXIT, (uint64_t)code); for (;;) {} }

#undef U

#endif
