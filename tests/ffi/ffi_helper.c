/* ffi_helper.c - a small C library exercising every C type that Luma's
 * `extern fun` supports. Built by tests/run_e2e.sh into libffihelper.a and
 * linked with `luma ... -L <dir> -l ffihelper`. Test-only code. */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* identity functions: values must survive the round trip unchanged */
int8_t id_i8(int8_t x) { return x; }
int16_t id_i16(int16_t x) { return x; }
int32_t id_i32(int32_t x) { return x; }
int64_t id_i64(int64_t x) { return x; }
uint8_t id_u8(uint8_t x) { return x; }
uint16_t id_u16(uint16_t x) { return x; }
uint32_t id_u32(uint32_t x) { return x; }
uint64_t id_u64(uint64_t x) { return x; }

/* narrow results computed from wider inputs: the C side truncates */
int8_t wrap_i8(int32_t x) { return (int8_t)x; }
uint8_t wrap_u8(int32_t x) { return (uint8_t)x; }

/* argument order across all six integer argument registers */
int64_t weigh6(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, int64_t f) {
    return a + 10 * b + 100 * c + 1000 * d + 10000 * e + 100000 * f;
}

/* mixed argument types in one call */
int64_t mixed(int8_t a, uint16_t b, bool c, const char *d, int32_t e) {
    return (int64_t)a + (int64_t)b + (c ? 1000 : 0) + (int64_t)strlen(d) * 10000 + (int64_t)e;
}

bool negate(bool b) { return !b; }
bool is_positive(int64_t x) { return x > 0; }

const char *greeting(void) { return "hello from C"; }
const char *maybe_name(bool give) { return give ? "ada" : NULL; }
bool is_null(const char *s) { return s == NULL; }
int64_t length_or_minus_one(const char *s) { return s ? (int64_t)strlen(s) : -1; }

/* values that do not fit in a Luma int */
uint64_t too_big_u64(void) { return (uint64_t)1 << 63; }
int64_t too_small_i64(void) { return INT64_MIN; }

/* void return with observable state */
static int32_t counter;
void counter_add(int32_t n) { counter += n; }
int32_t counter_get(void) { return counter; }

/* opaque pointers */
void *buf_new(int32_t n) { return calloc((size_t)n, sizeof(int32_t)); }
void buf_set(void *p, int32_t i, int32_t v) { ((int32_t *)p)[i] = v; }
int32_t buf_get(void *p, int32_t i) { return ((int32_t *)p)[i]; }
void buf_free(void *p) { free(p); }
