#define _XOPEN_SOURCE 500 // for nftw
#include <ftw.h>

#include "production.h"

// Global state used by nftw callback each_file().
static struct resource_list* resources;
static size_t filename_offset;

static int each_file(const char* filename, const struct stat* /*st*/,
                     int type, struct FTW* /*ftwbuf*/) {
    switch (type) {
    case FTW_F:
        break;
    case FTW_D:
        return 0; // skip directories
    case FTW_DNR:
    case FTW_NS:
        errno = EACCES;
        return 1;
    case FTW_SLN:
        errno = ENOENT;
        return 1;
    default:
        errno = EINVAL;
        return 1;
    }

    struct bytebuf bbuf;
    struct object_id oid;
    if (mmap_file(filename, &bbuf) < 0 ||
            write_object(TYPE_RESOURCE, bbuf.data, bbuf.size, &oid) < 0) {
        errno = EIO;
        return 1;
    }
    // We expect resource lists to be short, but building them may be quadratic.
    resource_list_insert(&resources, filename + filename_offset, &oid);
    return 0;
}

static void die_usage(char* arg0) {
    fprintf(stderr, "usage: %s <dir>\n", arg0);
    exit(1);
}

int main(int argc, char** argv) {
    if (argc != 2)
        die_usage(argv[0]);

    const char* dir = argv[1];
    char job_file[PATH_MAX];
    char outputs[PATH_MAX];
    if (snprintf(job_file, PATH_MAX, "%s/job", dir) >= PATH_MAX ||
        snprintf(outputs, PATH_MAX, "%s/out", dir) >= PATH_MAX)
        die("path too long");

    int fd = open(job_file, O_RDONLY);
    if (fd < 0)
        die("open %s failed: %s", job_file, strerror(errno));
    struct bytebuf bb;
    if (slurp_fd(fd, &bb) < 0)
        exit(1);
    close(fd);

    // Chomp any trailing newline.
    if (bb.size == KNIT_HASH_HEXSZ + 1 && ((char*)bb.data)[KNIT_HASH_HEXSZ] == '\n')
        ((char*)bb.data)[KNIT_HASH_HEXSZ] = '\0';
    struct production prd;
    if (hex_to_oid(bb.data, &prd.job_oid) < 0)
        die("invalid job hash");

    filename_offset = strlen(outputs) + 1;
    if (nftw(outputs, each_file, 16, 0) != 0)
        die("directory traversal failed on %s: %s", outputs, strerror(errno));
    prd.outputs = resources;

    struct object_id oid;
    if (write_production(&prd, &oid) < 0)
        exit(1);

    puts(oid_to_hex(&oid));
    return 0;
}
