/* value.h - the Luma value representation, shared by the compiler (which
 * emits tagged constants) and the runtime (which interprets them).
 * See docs/IR.md section 3.
 *
 *   fixnum n   (n << 1) | 1         63-bit signed integer
 *   pointer    8-aligned address    object with a u64 header (low byte = type id:
 *                                   1 string, 2 struct instance)
 *   false      0x02
 *   nil        0x06
 *   true       0x0A
 *   0          invalid (never a value)
 *
 * Falsy values are exactly nil and false: (v & ~4) == 2. */
#ifndef LUMA_VALUE_H
#define LUMA_VALUE_H

#include <stdint.h>

typedef uint64_t LumaValue;

#define LUMA_FALSE ((LumaValue)0x02)
#define LUMA_NIL ((LumaValue)0x06)
#define LUMA_TRUE ((LumaValue)0x0A)
#define LUMA_FALSY_MASK (~(LumaValue)4) /* (v & mask) == 2  <=>  v is falsy */

#define LUMA_FIXNUM_MAX ((int64_t)0x3FFFFFFFFFFFFFFF)    /*  2^62 - 1 */
#define LUMA_FIXNUM_MIN (-LUMA_FIXNUM_MAX - 1)           /* -2^62     */

#define LUMA_TYPE_STRING 1
#define LUMA_TYPE_STRUCT 2

/* A struct instance: header = LUMA_TYPE_STRUCT | (struct index + 1) << 8
 * (bits 32-63 stay free for a collector), then a pointer to the struct's
 * descriptor, then one word per field. */
#define LUMA_STRUCT_DESC_OFFSET 8
#define LUMA_STRUCT_FIELDS_OFFSET 16
#define LUMA_STRUCT_ID_SHIFT 8

static inline LumaValue luma_fixnum(int64_t n) { return ((uint64_t)n << 1) | 1; }

#endif
