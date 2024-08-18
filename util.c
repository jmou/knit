#include "util.h"

#include <dirent.h>
#include <sys/mman.h>

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

int warning(const char* format, ...) {
    va_list params;
    va_start(params, format);
    fprintf(stderr, "warning: ");
    vfprintf(stderr, format, params);
    fprintf(stderr, "\n");
    va_end(params);
    return -1;
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
        die("readdir: %s", strerror(errno));
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
}

int mmap_file(const char* filename, struct bytebuf* out) {
    memset(out, 0, sizeof(*out));

    int fd = open(filename, O_RDONLY);
    if (fd < 0)
        return error("cannot open %s: %s", filename, strerror(errno));
    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return error("cannot stat %s: %s", filename, strerror(errno));
    }

    if (st.st_size == 0)
        return 0;

    out->data = mmap(NULL, st.st_size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE, fd, 0);
    close(fd);
    if (out->data == MAP_FAILED)
        return error("mmap failed: %s", strerror(errno));
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
            close(fd);
            return error("read error: %s", strerror(errno));
        }
        out->size += nr;
        if (out->size == alloc) {
            alloc *= 2;
            out->data = xrealloc(out->data, alloc);
        }
    }

    close(fd);
    out->should_free = 1;
    return 0;
}
