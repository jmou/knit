#define _XOPEN_SOURCE 500 // for nftw
#include <ftw.h>

#include "hash.h"

struct resource {
    char* name;
    struct object_id id;
    struct resource* next;
};

// We expect to have short resource lists, but building a list may be quadratic
// if items are already ordered.
static void insert_resource(struct resource** head,
                            const char* name, const struct object_id* id) {
    struct resource** prevp = head;
    struct resource* curr = *head;
    while (curr && strcmp(curr->name, name) < 0) {
        prevp = &curr->next;
        curr = curr->next;
    }
    struct resource* node = xmalloc(sizeof(struct resource));
    node->name = strdup(name);
    oidcpy(&node->id, id);
    node->next = curr;
    *prevp = node;
}

// Global state used by visit_output.
static struct resource* outputs;
static size_t filename_offset;

static int visit_output(const char* filename, const struct stat* /*st*/,
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
            write_object("resource", bbuf.data, bbuf.size, &oid) < 0) {
        errno = EIO;
        return 1;
    }
    insert_resource(&outputs, filename + filename_offset, &oid);
    return 0;
}

int write_production(const struct object_id* job,
                     const struct resource* head,
                     struct object_id* production) {
    size_t size = KNIT_HASH_RAWSZ;
    for (const struct resource* curr = head; curr; curr = curr->next) {
        size += strlen(curr->name) + 1;
        size += KNIT_HASH_RAWSZ;
    }
    char* buf = xmalloc(size);

    char* p = buf;
    memcpy(p, job->hash, KNIT_HASH_RAWSZ);
    p += KNIT_HASH_RAWSZ;
    for (; head; head = head->next) {
        size_t name_size = strlen(head->name) + 1;
        memcpy(p, head->name, name_size);
        p += name_size;
        memcpy(p, head->id.hash, KNIT_HASH_RAWSZ);
        p += KNIT_HASH_RAWSZ;
    }

    int rc = write_object("production", buf, size, production);
    free(buf);
    return rc;
}

static void die_usage(char* arg0) {
    fprintf(stderr, "usage: %s <job> <dir>\n", arg0);
    exit(1);
}

int main(int argc, char** argv) {
    if (argc != 3)
        die_usage(argv[0]);

    struct object_id job;
    if (hex_to_oid(argv[1], &job) < 0)
        die("invalid job");

    const char* dir = argv[2];
    filename_offset = strlen(dir) + 1;
    if (nftw(dir, visit_output, 16, 0) != 0)
        die("directory traversal failed on %s: %s", dir, strerror(errno));

    struct object_id production;
    if (write_production(&job, outputs, &production) < 0)
        return 1;

    puts(oid_to_hex(&production));
    return 0;
}
