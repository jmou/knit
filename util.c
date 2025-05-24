#include "util.h"

#include <dirent.h>
#include <sys/mman.h>

static void emit_stderr(const char* prefix, int with_errno,
                        const char* fmt, va_list argp) {
    fputs(prefix, stderr);
    vfprintf(stderr, fmt, argp);
    if (with_errno)
        fprintf(stderr, ": %s", strerror(errno));
    fputc('\n', stderr);
}

void die(const char* fmt, ...) {
    va_list argp;
    va_start(argp, fmt);
    emit_stderr("fatal: ", 0, fmt, argp);
    va_end(argp);
    exit(1);
}

void die_errno(const char* fmt, ...) {
    va_list argp;
    va_start(argp, fmt);
    emit_stderr("fatal: ", 1, fmt, argp);
    va_end(argp);
    exit(1);
}

int error(const char* fmt, ...) {
    va_list argp;
    va_start(argp, fmt);
    emit_stderr("error: ", 0, fmt, argp);
    va_end(argp);
    return -1;
}

int error_errno(const char* fmt, ...) {
    va_list argp;
    va_start(argp, fmt);
    emit_stderr("error: ", 1, fmt, argp);
    va_end(argp);
    return -1;
}

void warning(const char* fmt, ...) {
    va_list argp;
    va_start(argp, fmt);
    emit_stderr("warning: ", 0, fmt, argp);
    va_end(argp);
}

void warning_errno(const char* fmt, ...) {
    va_list argp;
    va_start(argp, fmt);
    emit_stderr("warning: ", 1, fmt, argp);
    va_end(argp);
}

ssize_t xread(int fd, void* buf, size_t len) {
    ssize_t ret;
    do {
        ret = read(fd, buf, len);
    } while (ret < 0 && (errno == EAGAIN || errno == EINTR));
    return ret;
}

ssize_t xwrite(int fd, const void* buf, size_t len) {
    ssize_t ret;
    do {
        ret = write(fd, buf, len);
    } while (ret < 0 && (errno == EAGAIN || errno == EINTR));
    return ret;
}

static int valid_knit_dir(const char* dirname) {
    DIR* dir = opendir(dirname);
    if (!dir) {
        if (errno != ENOENT)
            die("cannot open knit directory %s", dirname);
        return 0;
    }
    int needs_entries = 2;
    struct dirent* dent;
    while (errno = 0, needs_entries > 0 && (dent = readdir(dir))) {
        if (dent->d_type == DT_DIR || dent->d_type == DT_UNKNOWN) {
            if (!strcmp(dent->d_name, "objects") || !strcmp(dent->d_name, "sessions"))
                needs_entries--;
        }
    }
    if (errno != 0)
        die_errno("readdir");
    closedir(dir);
    return needs_entries == 0;
}

const char* get_knit_dir() {
    static const char* knit_dir;
    if (!knit_dir) {
        knit_dir = getenv("KNIT_DIR");
        if (!knit_dir)
            knit_dir = ".knit";
        if (!valid_knit_dir(knit_dir))
            die("not a valid knit repository: %s", knit_dir);
    }
    return knit_dir;
}

void cleanup_bytebuf(struct bytebuf* bbuf) {
    if (bbuf->should_free)
        free(bbuf->data);
    if (bbuf->should_munmap)
        munmap(bbuf->data, bbuf->size);
    memset(bbuf, 0, sizeof(*bbuf));
}

void ensure_bytebuf_null_terminated(struct bytebuf* bb) {
    if (bb->null_terminated)
        return;
    if (bb->should_free) {
        bb->data = xrealloc(bb->data, bb->size + 1);
        ((char*)bb->data)[bb->size] = '\0';
    } else {
        assert(bb->should_munmap);
        char* buf = xmalloc(bb->size + 1);
        memcpy(buf, bb->data, bb->size);
        buf[bb->size] = '\0';
        if (munmap(bb->data, bb->size) < 0)
            warning_errno("munmap");
        bb->data = buf;
        bb->should_munmap = 0;
        bb->should_free = 1;
    }
    bb->null_terminated = 1;
}

int mmap_fd(int fd, struct bytebuf* out) {
    memset(out, 0, sizeof(*out));

    struct stat st;
    if (fstat(fd, &st) < 0)
        return -1;

    if (!S_ISREG(st.st_mode)) {
        errno = ENOTSUP;
        return -1;
    }

    if (st.st_size == 0)
        return 0;

    out->data = mmap(NULL, st.st_size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE, fd, 0);
    if (out->data == MAP_FAILED)
        return -1;

    out->should_munmap = 1;
    out->size = st.st_size;
    return 0;
}

int slurp_fd(int fd, struct bytebuf* out) {
    memset(out, 0, sizeof(*out));

    size_t alloc = 4096;
    struct stat st;
    if (fstat(fd, &st) == 0 && st.st_size > 0)
        alloc = st.st_size;

    out->data = xmalloc(alloc);
    out->size = 0;

    ssize_t nr;
    while ((nr = read(fd, out->data + out->size, alloc - out->size)) != 0) {
        if (nr < 0 && errno != EAGAIN && errno != EINTR) {
            free(out->data);
            return -1;
        }
        out->size += nr;
        if (out->size == alloc) {
            alloc *= 2;
            out->data = xrealloc(out->data, alloc);
        }
    }
    // NUL termination is free because read() requires free space in the buffer.
    ((char*)out->data)[out->size] = '\0';

    out->should_free = 1;
    out->null_terminated = 1;
    return 0;
}

int mmap_file(const char* filename, struct bytebuf* out) {
    int fd = open(filename, O_RDONLY);
    if (fd < 0)
        return error_errno("cannot open %s", filename);

    if (mmap_fd(fd, out) < 0) {
        close(fd);
        return error_errno("cannot mmap %s", filename);
    }
    close(fd);
    return 0;
}

int slurp_file(const char* filename, struct bytebuf* out) {
    int fd = open(filename, O_RDONLY);
    if (fd < 0)
        return error_errno("cannot open %s", filename);

    if (slurp_fd(fd, out) < 0) {
        close(fd);
        return error_errno("cannot read %s", filename);
    }
    close(fd);
    return 0;
}

int mmap_or_slurp_file(const char* filename, struct bytebuf* out) {
    int fd = open(filename, O_RDONLY);
    if (fd < 0)
        return error_errno("cannot open %s", filename);

    if (mmap_fd(fd, out) < 0 && slurp_fd(fd, out) < 0) {
        close(fd);
        return error_errno("cannot read %s", filename);
    }
    close(fd);
    return 0;
}

int acquire_lockfile(const char* lockfile) {
    static int is_rand_seeded = 0;
    if (!is_rand_seeded) {
        srand(getpid());
        is_rand_seeded = 1;
    }

    int fd;
    long backoff_ms = 1;
    while ((fd = open(lockfile,
                      O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666)) < 0) {
        if (errno != EEXIST)
            return error_errno("open error %s", lockfile);

        backoff_ms *= 2;
        if (backoff_ms > 5000)
            return error("lockfile timed out: %s", lockfile);
        // Randomly perturb backoff by +/-25% and scale to microseconds.
        long sleep_us = backoff_ms * (750 + rand() % 500);
        usleep(sleep_us);
    }

    return fd;
}
