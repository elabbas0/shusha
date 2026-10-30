#ifndef SHUSHA_VERSION_H
#define SHUSHA_VERSION_H

#include <stdint.h>

#define SHUSHA_VERSION_MAJOR  0
#define SHUSHA_VERSION_MINOR  1
#define SHUSHA_VERSION_PATCH  0

#define SHUSHA_VERSION_STRING "0.1.0"
#define SHUSHA_VERSION_BANNER "Shusha " SHUSHA_VERSION_STRING

/* Kernel code inherited from ICDA reads the ICDA_ names. */
#define ICDA_VERSION_STRING SHUSHA_VERSION_STRING

#endif /* SHUSHA_VERSION_H */
