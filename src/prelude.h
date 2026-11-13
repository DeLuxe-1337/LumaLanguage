/* prelude.h - the prelude and standard library (std/NAME.luma), built into luma.
 *
 * std/prelude.luma (Rc<T>, Arena) and the standard library modules are
 * ordinary Luma that every program sees; lowering merges in only what a
 * program uses (lower.c, with_prelude). Each file's statements carry line
 * numbers offset by LUMA_PRELUDE_LINE * (index + 1), so diagnostics can name
 * the file. */
#ifndef LUMA_PRELUDE_H
#define LUMA_PRELUDE_H

#include <stdbool.h>

#include "ast.h"

#define LUMA_PRELUDE_LINE 1000000
/* A program of several files (luma a.luma b.luma ...): the first file keeps
 * its line numbers, file k (k >= 1) is offset by LUMA_FILE_LINE_BASE +
 * (k - 1) * LUMA_FILE_LINE_STRIDE, above every std file. */
#define LUMA_FILE_LINE_BASE 100000000
#define LUMA_FILE_LINE_STRIDE 100000
#define LUMA_MAX_FILES 2000

/* Parses every std file (built in; $LUMA_STD=DIR reads DIR/NAME.luma
 * instead, and $LUMA_PRELUDE=FILE replaces std/prelude.luma) into out. False
 * (with an error printed) if one does not parse. */
bool prelude_load(Program *out);

/* For diagnostics: "<prelude>" or "<std/NAME.luma>" and the line within that
 * file, for a line from the std files; else path and line unchanged. */
const char *prelude_path(const char *path, int line);
int prelude_line(int line);

/* Parses the program's files (paths[0] first) into one program: their
 * statements in order, lines offset as above. False (with an error printed)
 * when one cannot be read or parsed. */
bool program_load_files(const char *const *paths, int n, Program *out);

/* Is a line from the std files (not the program's own files)? */
bool prelude_line_is_std(int line);

/* Is a statement at this line part of std/prelude.luma (whose names are
 * reserved), rather than an overridable library module? */
bool prelude_is_core(int line);

#endif
