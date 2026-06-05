#ifndef HOJA_LIB_DONGLE_LOG_H
#define HOJA_LIB_DONGLE_LOG_H

#include <dongle.h>

/* -------------------------------------------------------------------------- */
/* Logging                                                                    */
/*                                                                            */
/* The library avoids assuming any particular platform output. By default it   */
/* routes debug logging through printf gated by DONGLE_LIB_DEBUG_LOG. A        */
/* platform may override DONGLE_LOGF before including this header to redirect   */
/* logs (e.g. to a UART driver or a ring buffer).                             */
/* -------------------------------------------------------------------------- */

#ifndef DONGLE_LOGF
#if defined(DONGLE_LIB_DEBUG_LOG) && (DONGLE_LIB_DEBUG_LOG == 1)
#include <stdio.h>
#define DONGLE_LOGF(...) printf(__VA_ARGS__)
#else
#define DONGLE_LOGF(...) ((void)0)
#endif
#endif

#endif /* HOJA_LIB_DONGLE_LOG_H */
