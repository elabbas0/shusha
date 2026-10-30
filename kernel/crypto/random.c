#include "random.h"

#include "drbg.h"
#include "sha2.h"
#include "sc.h"

static sc_drbg_t drbg;
static uint32_t sources;
static uint64_t event_pool;

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static void cpuid(uint32_t leaf, uint32_t sub, uint32_t r[4]) {
    __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(sub));
}

static int has_rdrand(void) {
    uint32_t r[4];
    cpuid(1, 0, r);
    return (r[2] >> 30) & 1;
}

static int has_rdseed(void) {
    uint32_t r[4];
    cpuid(0, 0, r);
    if (r[0] < 7) return 0;
    cpuid(7, 0, r);
    return (r[1] >> 18) & 1;
}

/* Both return 1 on success.  The instruction can transiently fail
 * (CF=0), so callers retry a bounded number of times. */
static int rdrand64(uint64_t *v) {
    uint8_t ok;
    __asm__ volatile("rdrand %0; setc %1" : "=r"(*v), "=qm"(ok));
    return ok;
}

static int rdseed64(uint64_t *v) {
    uint8_t ok;
    __asm__ volatile("rdseed %0; setc %1" : "=r"(*v), "=qm"(ok));
    return ok;
}

static uint64_t irq_save(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static void irq_restore(uint64_t flags) {
    if (flags & (1u << 9)) __asm__ volatile("sti" : : : "memory");
}

/* Timing jitter: time a workload whose duration depends on cache and
 * pipeline state, and hash every delta.  Weak on its own in a VM, which
 * is why hardware sources are preferred and the result is reported. */
static void collect_jitter(sc_sha256_t *h) {
    volatile uint64_t sink = 0;
    for (int i = 0; i < 4096; i++) {
        uint64_t t0 = rdtsc();
        for (int j = 0; j < (int)(t0 & 31) + 8; j++) sink += (uint64_t)j * t0;
        uint64_t d = rdtsc() - t0;
        sc_sha256_update(h, (const uint8_t *)&d, sizeof(d));
    }
    sc_sha256_update(h, (const uint8_t *)&sink, sizeof(sink));
}

static void mix_hardware(sc_sha256_t *h, int words) {
    for (int i = 0; i < words; i++) {
        uint64_t v;
        int got = 0;
        for (int tries = 0; tries < 10 && !got; tries++) {
            if ((sources & RANDOM_SRC_RDSEED) && rdseed64(&v)) got = 1;
            else if ((sources & RANDOM_SRC_RDRAND) && rdrand64(&v)) got = 1;
        }
        if (got) sc_sha256_update(h, (const uint8_t *)&v, sizeof(v));
        sc_wipe(&v, sizeof(v));
    }
}

uint32_t random_init(void) {
    sc_sha256_t h;
    uint8_t seed[32];
    uint64_t t = rdtsc();

    if (has_rdseed()) sources |= RANDOM_SRC_RDSEED;
    if (has_rdrand()) sources |= RANDOM_SRC_RDRAND;
    sources |= RANDOM_SRC_JITTER;

    sc_sha256_init(&h);
    sc_sha256_update(&h, (const uint8_t *)&t, sizeof(t));
    mix_hardware(&h, 16);
    collect_jitter(&h);
    sc_sha256_final(&h, seed);
    sc_drbg_reseed(&drbg, seed, sizeof(seed));
    sc_wipe(seed, sizeof(seed));
    return sources;
}

int random_ready(void) {
    return drbg.seeded;
}

void random_add_event(uint64_t value) {
    /* Cheap enough for IRQ context: fold into a word that the next
     * random_get() hashes into the DRBG. */
    event_pool = (event_pool << 7 | event_pool >> 57) ^ value ^ rdtsc();
}

int random_get(void *buf, uint64_t n) {
    struct {
        uint64_t tsc;
        uint64_t events;
        uint64_t hw;
    } fresh;
    uint64_t flags;
    int r;

    if (!drbg.seeded) return -1;
    flags = irq_save();
    fresh.tsc = rdtsc();
    fresh.events = event_pool;
    fresh.hw = 0;
    if (sources & RANDOM_SRC_RDRAND) (void)rdrand64(&fresh.hw);
    sc_drbg_reseed(&drbg, (const uint8_t *)&fresh, sizeof(fresh));
    r = sc_drbg_generate(&drbg, (uint8_t *)buf, (size_t)n);
    irq_restore(flags);
    sc_wipe(&fresh, sizeof(fresh));
    return r;
}
