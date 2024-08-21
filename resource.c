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

struct resource_list* resource_list_insert(struct resource_list** list_p,
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
