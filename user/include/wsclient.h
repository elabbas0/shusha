#ifndef SHUSHA_WSCLIENT_H
#define SHUSHA_WSCLIENT_H

#include <stdint.h>

/* Run one workspace command line (e.g. "members") through workspaced and
 * copy its output, NUL-terminated, into out.  Returns the output length,
 * or -1 with a one-line reason in out if the service could not be used. */
long ws_call(const char *line, char *out, uint64_t cap);

#endif
