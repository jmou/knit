#include <stdarg.h>
#include <stdio.h>

#include "util.h"

void die(const char* format, ...) {
    va_list params;
    va_start(params, format);
    fprintf(stderr, "fatal: ");
    vfprintf(stderr, format, params);
    fprintf(stderr, "\n");
    va_end(params);
    exit(1);
}

int error(const char* format, ...) {
    va_list params;
    va_start(params, format);
    fprintf(stderr, "error: ");
    vfprintf(stderr, format, params);
    fprintf(stderr, "\n");
    va_end(params);
    return -1;
}
