#include "resource.h"

struct resource* get_resource(const struct object_id* oid) {
    return intern_object(oid, OBJ_RESOURCE, sizeof(struct resource));
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
