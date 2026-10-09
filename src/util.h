/* util.h - small shared helpers: growable byte buffers, checked allocation,
 * file I/O. Shared by every stage of the Luma bootstrap toolchain. */
#ifndef LUMA_UTIL_H
#define LUMA_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void *xmalloc(size_t n);
void *xcalloc(size_t count, size_t size);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

/* Growable byte buffer. Used for machine code, section data, text output. */
typedef struct {
    unsigned char *data;
    size_t len;
    size_t cap;
} Buf;

void buf_reserve(Buf *b, size_t extra);
void buf_append(Buf *b, const void *p, size_t n);
void buf_byte(Buf *b, uint8_t v);
void buf_u16(Buf *b, uint16_t v); /* little endian */
void buf_u32(Buf *b, uint32_t v); /* little endian */
void buf_u64(Buf *b, uint64_t v); /* little endian */
void buf_zeros(Buf *b, size_t n);
void buf_align(Buf *b, size_t align); /* pad with zeros to a multiple of align */
void buf_printf(Buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void buf_free(Buf *b);
/* Overwrite 4 bytes at off (little endian). Returns false if out of range. */
bool buf_patch_u32(Buf *b, size_t off, uint32_t v);

/* Reads a whole file. Returns NULL and prints a diagnostic on failure.
 * The result is NUL-terminated (the terminator is not counted in *len). */
char *read_file(const char *path, size_t *len);
/* Writes a whole file. Prints a diagnostic and returns false on failure. */
bool write_file(const char *path, const void *data, size_t len);

#endif
