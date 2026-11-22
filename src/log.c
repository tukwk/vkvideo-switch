#include "log.h"

#include <stdarg.h>
#include <stdio.h>

void applog(const char *fmt, ...)
{
#ifdef __SWITCH__
    FILE *f = fopen("sdmc:/switch/vkvideo/log.txt", "a");
    if (!f)
        return;
#else
    FILE *f = stderr;
#endif

    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);

#ifdef __SWITCH__
    fclose(f);
#endif
}
