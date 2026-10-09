#include "util.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void oom(void) {
    fprintf(stderr, "luma: fatal: out of memory\n");
    exit(2);
}

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) oom();
    return p;
}

void *xcalloc(size_t count, size_t size) {
    void *p = calloc(count ? count : 1, size ? size : 1);
    if (!p) oom();
    return p;
}

void *xrealloc(void *p, size_t n) {
    p = realloc(p, n ? n : 1);
    if (!p) oom();
    return p;
}

char *xstrdup(const char *s) { return xstrndup(s, strlen(s)); }

char *xstrndup(const char *s, size_t n) {
    char *d = xmalloc(n + 1);
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

void buf_reserve(Buf *b, size_t extra) {
    if (extra > SIZE_MAX - b->len) oom();
    size_t need = b->len + extra;
    if (need <= b->cap) return;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) { cap = need; break; }
        cap *= 2;
    }
    b->data = xrealloc(b->data, cap);
    b->cap = cap;
}

void buf_append(Buf *b, const void *p, size_t n) {
    if (n == 0) return;
    buf_reserve(b, n);
    memcpy(b->data + b->len, p, n);
    b->len += n;
}

void buf_byte(Buf *b, uint8_t v) { buf_append(b, &v, 1); }

void buf_u16(Buf *b, uint16_t v) {
    uint8_t t[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    buf_append(b, t, 2);
}

void buf_u32(Buf *b, uint32_t v) {
    uint8_t t[4];
    for (int i = 0; i < 4; i++) t[i] = (uint8_t)(v >> (8 * i));
    buf_append(b, t, 4);
}

void buf_u64(Buf *b, uint64_t v) {
    uint8_t t[8];
    for (int i = 0; i < 8; i++) t[i] = (uint8_t)(v >> (8 * i));
    buf_append(b, t, 8);
}

void buf_zeros(Buf *b, size_t n) {
    buf_reserve(b, n);
    memset(b->data + b->len, 0, n);
    b->len += n;
}

void buf_align(Buf *b, size_t align) {
    if (align <= 1) return;
    size_t rem = b->len % align;
    if (rem) buf_zeros(b, align - rem);
}

void buf_printf(Buf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { va_end(ap2); return; }
    buf_reserve(b, (size_t)n + 1);
    vsnprintf((char *)b->data + b->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)n;
}

void buf_free(Buf *b) {
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

bool buf_patch_u32(Buf *b, size_t off, uint32_t v) {
    if (off > b->len || b->len - off < 4) return false;
    for (int i = 0; i < 4; i++) b->data[off + i] = (uint8_t)(v >> (8 * i));
    return true;
}

char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "luma: error: cannot open '%s': %s\n", path, strerror(errno));
        return NULL;
    }
    Buf b = {0};
    char tmp[4096];
    size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) buf_append(&b, tmp, n);
    if (ferror(f)) {
        fprintf(stderr, "luma: error: cannot read '%s': %s\n", path, strerror(errno));
        fclose(f);
        buf_free(&b);
        return NULL;
    }
    fclose(f);
    buf_byte(&b, 0);
    *len = b.len - 1;
    return (char *)b.data;
}

bool write_file(const char *path, const void *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "luma: error: cannot create '%s': %s\n", path, strerror(errno));
        return false;
    }
    bool ok = fwrite(data, 1, len, f) == len;
    if (fclose(f) != 0) ok = false;
    if (!ok) fprintf(stderr, "luma: error: cannot write '%s': %s\n", path, strerror(errno));
    return ok;
}
