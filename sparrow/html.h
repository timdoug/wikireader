#ifndef SPARROW_HTML_H
#define SPARROW_HTML_H
#include "sparrow.h"
/* Internal links use @sparrow/<dense-id>, resolved on tap against the
 * selected local ZIM by its enwiki title, never an unchecked entry number. */
int sparrow_html(const SPARROW *, const SPARROW_RESULT *, const char *, char *, size_t, size_t *);
#endif
