#pragma once

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void die(const char* format, ...);
int error(const char* format, ...);
int warning(const char* format, ...);

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

const char* get_knit_dir();

struct bytebuf {
    void* data;
    size_t size;
    unsigned should_free : 1;
    unsigned should_munmap : 1;
};

void cleanup_bytebuf(struct bytebuf* bbuf);

// mmap will be private and writable (COW).
int mmap_file(const char* filename, struct bytebuf* out);
int mmap_fd(int fd, struct bytebuf* out);

// slurp will always ensure a NUL byte exists after the end of the file.
int slurp_file(const char* filename, struct bytebuf* out);
int slurp_fd(int fd, struct bytebuf* out);
