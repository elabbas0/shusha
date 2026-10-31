#ifndef SHUSHA_AUTHPROTO_H
#define SHUSHA_AUTHPROTO_H

/* authd: Shusha's account service (see user/sbin/authd.c, svc.h).
 *
 * Requests are "<verb>" lines; secrets travel only in the data part, one
 * per line, never on the command line.  Answers are tab-separated. */

#define AUTH_QUEUE  "/auth/ctl"
#define AUTH_IO_DIR "/var/auth/io"
#define AUTH_BOX    "auth"

#define AUTH_FIRST_UID 1000
#define GREETER_UID    1      /* the login screen runs as this, with no files */

#define ACCT_ADMIN 1

#endif
