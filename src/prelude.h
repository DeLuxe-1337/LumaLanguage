/* prelude.h - the Luma prelude (std/prelude.luma), built into luma.
 *
 * The prelude is ordinary Luma that every program sees: Rc<T> and Arena are
 * written in it. Its statements carry line numbers offset by
 * LUMA_PRELUDE_LINE, so diagnostics can tell them from the program's. */
#ifndef LUMA_PRELUDE_H
#define LUMA_PRELUDE_H

#include <stdbool.h>

#include "ast.h"

#define LUMA_PRELUDE_LINE 1000000

/* Parses the prelude (the built-in text, or the file named by $LUMA_PRELUDE)
 * into out; its lines are offset by LUMA_PRELUDE_LINE. False (with an error
 * printed) if it does not parse. */
bool prelude_load(Program *out);

/* For diagnostics: "<prelude>" and the prelude's own line number for a line
 * in the prelude, else path and line unchanged. */
const char *prelude_path(const char *path, int line);
int prelude_line(int line);

#endif
