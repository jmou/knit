#pragma once

#include <stdlib.h>

void die(const char* format, ...);
int error(const char* format, ...);

static inline void* xmalloc(size_t size) {
    void* buf = malloc(size);
    if (!buf)
        die("out of memory in malloc");
    return buf;
}

static inline void* xrealloc(void* ptr, size_t size) {
    void* buf = realloc(ptr, size);
    if (!buf)
        die("out of memory in realloc");
    return buf;
}
