#define _XOPEN_SOURCE 500 // for nftw
#include <ftw.h>

#include "resource.h"

struct resource* get_resource(const struct object_id* oid) {
    return intern_object(oid, OBJ_RESOURCE, sizeof(struct resource));
}

int parse_resource(struct resource* res) {
    if (res->object.is_parsed)
        return 0;
    size_t size;
    void* buf = read_object_of_type(&res->object.oid, OBJ_RESOURCE, &size);
    if (!buf)
        return -1;
    free(buf);
    return 0;
}

struct resource* store_resource(void* data, size_t size) {
    struct object_id oid;
    if (write_object(OBJ_RESOURCE, data, size, &oid))
        return NULL;
    return get_resource(&oid);
}

struct resource* store_resource_file(const char* filename) {
    struct bytebuf bb;
    if (mmap_or_slurp_file(filename, &bb) < 0)
        return NULL;
    struct resource* res = store_resource(bb.data, bb.size);
    cleanup_bytebuf(&bb);
    return res;
}

struct resource_list* resource_list_insert(struct resource_list** list_p,
                                           const char* name,
                                           struct resource* res) {
    struct resource_list* list = *list_p;
    while (list && strcmp(list->name, name) < 0) {
        list_p = &list->next;
        list = list->next;
    }
    struct resource_list* node = xmalloc(sizeof(*node));
    node->name = strdup(name);
    node->res = res;
    node->next = list;
    *list_p = node;
    return node;
}

void resource_list_remove_and_free(struct resource_list** list_p) {
    struct resource_list* next = (*list_p)->next;
    free((*list_p)->name);
    free(*list_p);
    *list_p = next;
}

// Global state used by nftw callback each_file().
static struct resource_list** resources_p;
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

    const char* name = filename + filename_offset;
    if (!strncmp(name, ".knit/", 6))
        return 0;

    struct resource* res = store_resource_file(filename);
    if (!res) {
        errno = EIO;
        return 1;
    }

    // We expect resource lists to be short, but building them may be quadratic.
    resource_list_insert(resources_p, name, res);
    return 0;
}

int resource_list_insert_dir_files(struct resource_list** list_p,
                                   const char* dir) {
    // Assume a reasonable implementation of nftw that preserves dir as the
    // callback filename prefix.
    filename_offset = strlen(dir);
    if (dir[filename_offset - 1] != '/')
        filename_offset++;

    resources_p = list_p;
    if (nftw(dir, each_file, 16, 0) != 0) {
        while (*resources_p)
            resource_list_remove_and_free(resources_p);
        return -1;
    }
    return 0;
}
