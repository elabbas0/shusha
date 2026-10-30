#ifndef SHUSHA_LIB_H
#define SHUSHA_LIB_H

#include <stdint.h>
#include "sys.h"

uint64_t str_len(const char *s);
int      str_eq(const char *a, const char *b);
int      str_starts(const char *s, const char *prefix);
char    *str_copy(char *dst, const char *src, uint64_t cap);
char    *str_cat(char *dst, const char *src, uint64_t cap);
void     mem_set(void *dst, int v, uint64_t n);
void     mem_copy(void *dst, const void *src, uint64_t n);
int      mem_eq_ct(const void *a, const void *b, uint64_t n);
void     mem_wipe(void *dst, uint64_t n);

/* Format an unsigned integer in base 10 or 16 into buf; returns buf. */
char    *fmt_u64(char *buf, uint64_t v, int base);

void     print(const char *s);
void     print_u64(uint64_t v);
void     print_hex(const uint8_t *data, uint64_t n);

/* Split `line` in place on spaces; returns argc (at most max). */
int      split_args(char *line, char **argv, int max);

#endif
