/* A host for tests/lib/exports.luma built with `luma --shared`: loads the
 * library, calls its exported functions, checks errors and reloading. */
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "../../runtime/luma.h"

/* the library's own functions (export fun in exports.luma) */
typedef void (*OnUpdate)(double dt);
typedef int64_t (*Add)(int64_t a, int64_t b);
typedef double (*Scale)(double x, int64_t k, bool flip);
typedef double (*Six)(int64_t a, double b, int64_t c, double d, bool e, int64_t f);
typedef int64_t (*Boom)(int64_t n);
typedef bool (*IsPos)(double x);

/* dlsym's void * as a function pointer */
#define SYM(type, name) ((type)(uintptr_t)dlsym(h, name))

static void on_error(const char *msg) { printf("host: error handler: %s\n", msg); }

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    void *h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        printf("dlopen: %s\n", dlerror());
        return 1;
    }
    __typeof__(&luma_lib_init) init = SYM(__typeof__(&luma_lib_init), "luma_lib_init");
    __typeof__(&luma_lib_error) error = SYM(__typeof__(&luma_lib_error), "luma_lib_error");
    __typeof__(&luma_lib_error_count) error_count = SYM(__typeof__(&luma_lib_error_count), "luma_lib_error_count");
    __typeof__(&luma_lib_set_error_handler) set_handler = SYM(__typeof__(&luma_lib_set_error_handler), "luma_lib_set_error_handler");
    __typeof__(&luma_lib_shutdown) shutdown = SYM(__typeof__(&luma_lib_shutdown), "luma_lib_shutdown");
    OnUpdate on_update = SYM(OnUpdate, "on_update");
    Add add = SYM(Add, "add");
    Scale scale = SYM(Scale, "scale");
    Six six = SYM(Six, "six");
    Boom boom = SYM(Boom, "boom");
    IsPos is_pos = SYM(IsPos, "is_pos");

    printf("init -> %d\n", init());
    fflush(stdout);
    for (int i = 0; i < 3; i++) on_update(0.25);
    printf("add -> %lld\n", (long long)add(40, 2));
    printf("scale -> %g %g\n", scale(1.5, 3, false), scale(1.5, 3, true));
    printf("six -> %g\n", six(1, 0.5, 2, 0.25, true, 4));
    int64_t r = boom(1);
    printf("boom(1) -> %lld, errors %llu\n", (long long)r, (unsigned long long)error_count());
    set_handler(on_error);
    fflush(stdout);
    r = boom(5);
    printf("boom(5) -> %lld, errors %llu, '%s'\n", (long long)r, (unsigned long long)error_count(), error());
    printf("boom(2) after the error -> %lld\n", (long long)boom(2));
    printf("is_pos -> %d %d\n", is_pos(2.0), is_pos(-1.0));
    fflush(stdout);
    shutdown();
    dlclose(h);

    h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL); /* again: a fresh runtime and fresh globals */
    if (!h) return 1;
    init = SYM(__typeof__(&luma_lib_init), "luma_lib_init");
    printf("reloaded: init -> %d\n", init());
    fflush(stdout);
    dlclose(h);
    return 0;
}
