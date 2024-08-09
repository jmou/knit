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
    fprintf(stderr, "usage: %s <job> <dir>\n", arg0);
    exit(1);
}

int main(int argc, char** argv) {
    if (argc != 3)
        die_usage(argv[0]);

    struct production prd;
    if (hex_to_oid(argv[1], &prd.job_oid) < 0)
        die("invalid job");

    const char* dir = argv[2];
    filename_offset = strlen(dir) + 1;
    if (nftw(dir, each_file, 16, 0) != 0)
        die("directory traversal failed on %s: %s", dir, strerror(errno));
    prd.outputs = resources;

    struct object_id oid;
    if (write_production(&prd, &oid) < 0)
        exit(1);

    puts(oid_to_hex(&oid));
    return 0;
}
