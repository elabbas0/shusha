#ifndef SHUSHA_WSCLIENT_H
#define SHUSHA_WSCLIENT_H

#include <stdint.h>

#define WS_MAX_FILE 8192

/* Send one command line, plus optional data (the contents for `put`), to
 * workspaced and copy its answer, NUL-terminated, into out.  Returns the
 * answer's length, or -1 with a one-line reason in out if the service
 * could not be used.  *failed (if given) is set when the service reports
 * the command failed (used by `raw`, whose answer is file content). */
long ws_request(const char *line, const void *data, uint64_t dlen, char *out, uint64_t cap, int *failed);

/* A user-facing command line.  `put <path>` reads the file here, with the
 * caller's own permissions, and sends only its contents; `get <file>
 * <dest>` saves a shared file into the caller's own files.  Everything
 * else goes straight to the service. */
long ws_run(const char *cmdline, char *out, uint64_t cap);

#endif
