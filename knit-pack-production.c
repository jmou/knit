#define _XOPEN_SOURCE 500 // for nftw
#include <ftw.h>

#include "production.h"
#include "resource.h"

// Global state used by nftw callback each_file().
static struct resource_list* resources;
static size_t filename_offset;

static struct resource_list* resource_list_insert(struct resource_list** list_p,
                                                  const char* path,
                                                  struct resource* res) {
    struct resource_list* list = *list_p;
    while (list && strcmp(list->path, path) < 0) {
        list_p = &list->next;
        list = list->next;
    }
    struct resource_list* node = xmalloc(sizeof(*node));
    node->path = strdup(path);
    node->res = res;
    node->next = list;
    *list_p = node;
    return node;
}

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
    struct resource* res;
    if (mmap_file(filename, &bbuf) < 0 ||
            !(res = store_resource(bbuf.data, bbuf.size))) {
        cleanup_bytebuf(&bbuf);
        errno = EIO;
        return 1;
    }
    cleanup_bytebuf(&bbuf);
    // We expect resource lists to be short, but building them may be quadratic.
    resource_list_insert(&resources, filename + filename_offset, res);
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

    struct bytebuf bb;
    if (slurp_file(job_file, &bb) < 0)
        exit(1);

    // Chomp any trailing newline.
    if (bb.size == KNIT_HASH_HEXSZ + 1 && ((char*)bb.data)[KNIT_HASH_HEXSZ] == '\n')
        ((char*)bb.data)[KNIT_HASH_HEXSZ] = '\0';
    struct object_id job_oid;
    if (hex_to_oid(bb.data, &job_oid) < 0)
        die("invalid job hash");

    filename_offset = strlen(outputs) + 1;
    if (nftw(outputs, each_file, 16, 0) != 0)
        die("directory traversal failed on %s: %s", outputs, strerror(errno));

    struct production* prd = store_production(get_job(&job_oid), resources);
    if (!prd)
        exit(1);

    puts(oid_to_hex(&prd->object.oid));
    return 0;
}
