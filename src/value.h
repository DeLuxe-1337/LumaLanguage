/* value.h - the Luma value representation, shared by the compiler (which
 * emits tagged constants) and the runtime (which interprets them).
 * See docs/IR.md section 3.
 *
 *   fixnum n   (n << 1) | 1         63-bit signed integer
 *   pointer    8-aligned address    object with a u64 header (low byte = type id:
 *                                   1 string, 2 struct instance; bits 32-63 = the
 *                                   reference count, 0 = not counted)
 *   false      0x02
 *   nil        0x06
 *   true       0x0A
 *   0          invalid (never a value)
 *   ...100     never a value; LUMA_POISON (what fields of freed objects read as
 *              under LUMA_MEMCHECK=1) uses this tag
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
#define LUMA_TYPE_FREED 0x7F /* a freed object (kept readable: see runtime "memory") */

/* Object header: bits 0-7 type id, bits 8-31 struct id (structs), bits 32-63
 * the reference count. A count of 0 means "not counted": static string
 * literals, objects in --mm=none, and struct instances the program frees
 * with free() in --mm=manual. Counted objects are freed when the count drops
 * to 0. The count is the 32-bit word at offset 4 (little-endian). */
#define LUMA_RC_OFFSET 4
#define LUMA_STRING_HEAP (1u << 8) /* string header flag: allocated at run time (not a literal) */
#define LUMA_POISON ((LumaValue)0x00000DEAD0000004)

/* Memory-management modes (luma --mm=..., emitted as the luma_mm symbol). */
#define LUMA_STRUCT_SHARED 1 /* descriptor flags: every instance is counted */
#define LUMA_MM_NONE 0
#define LUMA_MM_MANUAL 1

/* Struct descriptor kinds (LumaStructDesc.kind; IrStructKind). */
#define LUMA_STRUCT_USER 0
#define LUMA_STRUCT_RC 1
#define LUMA_STRUCT_ARENA 2

/* A struct instance: header = LUMA_TYPE_STRUCT | (struct index + 1) << 8
 * | count << 32, then a pointer to the struct's descriptor, then one word
 * per field. */
#define LUMA_STRUCT_DESC_OFFSET 8
#define LUMA_STRUCT_FIELDS_OFFSET 16
#define LUMA_STRUCT_ID_SHIFT 8

static inline LumaValue luma_fixnum(int64_t n) { return ((uint64_t)n << 1) | 1; }

#endif
