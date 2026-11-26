#include "jsonl.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *find_val(const char *s, const char *key)
{
    char pat[40];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = s;
    while ((p = strstr(p, pat))) {
        const char *q = p + strlen(pat);
        while (*q == ' ') q++;
        if (*q == ':') { q++; while (*q == ' ') q++; return q; }
        p += 1;
    }
    return NULL;
}

bool js_int(const char *s, const char *key, long *out)
{
    const char *v = find_val(s, key);
    if (!v) return false;
    if (!strncmp(v, "true", 4)) { *out = 1; return true; }
    if (!strncmp(v, "false", 5)) { *out = 0; return true; }
    char *end;
    long x = strtol(v, &end, 10);
    if (end == v) return false;
    *out = x;
    return true;
}

bool js_str(const char *s, const char *key, char *out, size_t cap)
{
    const char *v = find_val(s, key);
    if (!v || *v != '"') return false;
    v++;
    size_t n = 0;
    while (*v && *v != '"') {
        unsigned c = (unsigned char)*v++;
        if (c == '\\') {
            char e = *v++;
            if (e == 'n') c = '\n';
            else if (e == 't') c = '\t';
            else if (e == 'u') {
                unsigned u = 0;
                for (int i = 0; i < 4 && *v; i++) { char h = *v++; u = u * 16 + (h <= '9' ? h - '0' : (h | 32) - 'a' + 10); }
                if (u < 0x80) c = u;
                else {   /* encode BMP code point as UTF-8 */
                    char tmp[3]; int k;
                    if (u < 0x800) { tmp[0] = 0xC0 | (u >> 6); tmp[1] = 0x80 | (u & 63); k = 2; }
                    else { tmp[0] = 0xE0 | (u >> 12); tmp[1] = 0x80 | ((u >> 6) & 63); tmp[2] = 0x80 | (u & 63); k = 3; }
                    for (int i = 0; i < k; i++) if (n + 1 < cap) out[n++] = tmp[i];
                    continue;
                }
            } else c = (unsigned char)e;
        }
        if (n + 1 < cap) out[n++] = (char)c;
    }
    out[n] = 0;
    return true;
}

int js_arr_u16(const char *s, const char *key, uint16_t *out, size_t max)
{
    const char *v = find_val(s, key);
    if (!v || *v != '[') return -1;
    v++;
    size_t n = 0;
    for (;;) {
        while (*v == ' ' || *v == ',') v++;
        if (*v == ']' || !*v) break;
        char *end;
        long x = strtol(v, &end, 10);
        if (end == v) break;
        if (n < max) out[n++] = x < 0 ? 0 : (x > 65535 ? 65535 : (uint16_t)x);
        v = end;
    }
    return (int)n;
}

void out_init(out_t *o, void (*sink)(void *, const char *, size_t), void *ctx)
{
    o->sink = sink; o->ctx = ctx; o->len = 0;
}

void out_flush(out_t *o)
{
    if (o->len) { o->sink(o->ctx, o->buf, o->len); o->len = 0; }
}

static void out_bytes(out_t *o, const char *d, size_t n)
{
    while (n) {
        size_t room = sizeof o->buf - o->len;
        if (!room) { out_flush(o); room = sizeof o->buf; }
        size_t k = n < room ? n : room;
        memcpy(o->buf + o->len, d, k);
        o->len += k; d += k; n -= k;
    }
}

void out_puts(out_t *o, const char *s) { out_bytes(o, s, strlen(s)); }

void out_printf(out_t *o, const char *fmt, ...)
{
    char tmp[600];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0) out_bytes(o, tmp, n < (int)sizeof tmp ? (size_t)n : sizeof tmp - 1);
}

void out_jstr(out_t *o, const char *s)
{
    out_bytes(o, "\"", 1);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { char e[2] = { '\\', (char)c }; out_bytes(o, e, 2); }
        else if (c < 0x20) { char e[8]; int k = snprintf(e, sizeof e, "\\u%04x", c); out_bytes(o, e, k); }
        else out_bytes(o, (const char *)&c, 1);
    }
    out_bytes(o, "\"", 1);
}
