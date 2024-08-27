#pragma once

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define NORETURN __attribute__((__noreturn__))

void die(const char* format, ...) NORETURN;
void die_errno(const char* format, ...) NORETURN;
int error(const char* format, ...);
int error_errno(const char* format, ...);
void warning(const char* format, ...);
void warning_errno(const char* format, ...);

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

// xwrite retries on EAGAIN and EINTR but does not ensure len bytes are written.
ssize_t xwrite(int fd, const void* buf, size_t len);

const char* get_knit_dir();

struct bytebuf {
    void* data;
    size_t size;
    unsigned should_free : 1;
    unsigned should_munmap : 1;
    unsigned null_terminated : 1;
};

void cleanup_bytebuf(struct bytebuf* bbuf);
void ensure_bytebuf_null_terminated(struct bytebuf* bb);

// mmap will be private and writable (COW).
int mmap_file(const char* filename, struct bytebuf* out);
int mmap_fd(int fd, struct bytebuf* out);

int slurp_file(const char* filename, struct bytebuf* out);
int slurp_fd(int fd, struct bytebuf* out);

int mmap_or_slurp_file(const char* filename, struct bytebuf* out);
