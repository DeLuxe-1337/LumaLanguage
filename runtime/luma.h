/* luma.h - the C interface of a Luma program built as a shared library
 * (`luma --shared prog.luma -o libprog.so`), for the host that loads it.
 *
 *   void *h = dlopen("./libprog.so", RTLD_NOW | RTLD_LOCAL);
 *   int (*init)(void) = dlsym(h, "luma_lib_init");
 *   if (init() != 0) ... luma_lib_error() says why
 *   void (*on_update)(double) = dlsym(h, "on_update");   // export fun on_update(dt: float)
 *
 * Every `export fun` is a C function of the same name. Parameter and result
 * types map as: int <-> int64_t, float <-> double, bool <-> bool,
 * str <-> const char * (NUL-terminated; the library copies an argument, and
 * a result stays valid until the next call that returns a str; NULL after an
 * error; luma_lib_str_len() is its length); a function without a return type
 * returns void. Ints must fit in 63 bits. At most 6 parameters.
 *
 * `const char luma_exports[]` (dlsym) lists the exports for hosts that call
 * them by name: a line per function, "name:PARAMS>RESULT", where each of
 * PARAMS and RESULT is i (int), f (float), b (bool), s (str); v is a result
 * of nothing. E.g. "update:f>v\ngenerate:is>s\n".
 *
 * A runtime error inside an exported function (or the top-level code run by
 * luma_lib_init) does not end the process: the call returns (0, 0.0 or
 * false), luma_lib_error_count() goes up and luma_lib_error() holds the
 * message, which is also printed to stderr unless an error handler is set.
 * Each library has its own runtime, heap and globals; unloading it (dlclose)
 * and loading it again starts over. Calls are not thread-safe: use one thread.
 * Unbounded recursion still overflows the host's stack. */
#ifndef LUMA_H
#define LUMA_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Runs the program's top-level code. Call once, before anything else.
 * Returns 0, or 1 after a runtime error. */
int luma_lib_init(void);
/* Flushes output; under LUMA_MEMCHECK=1 reports leaks. Call before dlclose. */
void luma_lib_shutdown(void);
/* The message of the last runtime error ("" if none). */
const char *luma_lib_error(void);
/* How many runtime errors exported calls have had. */
uint64_t luma_lib_error_count(void);
/* Called with each runtime error's message instead of printing it. */
void luma_lib_set_error_handler(void (*handler)(const char *message));
/* print hands each line (without the newline) to the handler instead of
 * writing it to stdout. */
void luma_lib_set_output_handler(void (*handler)(const char *line));
/* For host functions that Luma code calls (extern fun): ends the current
 * exported call with a Luma runtime error. Does not return (longjmp): call
 * it only with no C++ objects alive in the calling frames. */
void luma_lib_raise(const char *message);
/* The length of the last str result (it may hold NUL bytes). */
uint64_t luma_lib_str_len(void);

#ifdef __cplusplus
}
#endif

#endif
