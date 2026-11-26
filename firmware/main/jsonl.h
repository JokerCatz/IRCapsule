/* Tiny helpers for the flat, single-line JSON this project speaks.
 * Not a general JSON parser: keys are found by their quoted name, values are flat. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool js_int(const char *s, const char *key, long *out);
bool js_str(const char *s, const char *key, char *out, size_t cap);   /* decodes \" \\ \n \t \uXXXX */
/* Parse an array of non-negative integers. Returns the count, or -1 if the key is missing. */
int js_arr_u16(const char *s, const char *key, uint16_t *out, size_t max);

/* Output builder: buffers small writes, flushes to a sink. */
typedef struct out {
    void (*sink)(void *ctx, const char *data, size_t n);
    void *ctx;
    char buf[400];
    size_t len;
} out_t;

void out_init(out_t *o, void (*sink)(void *, const char *, size_t), void *ctx);
void out_puts(out_t *o, const char *s);
void out_printf(out_t *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void out_jstr(out_t *o, const char *s);          /* quoted and escaped */
void out_flush(out_t *o);
