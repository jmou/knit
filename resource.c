#include "resource.h"

struct resource* get_resource(const struct object_id* oid) {
    return intern_object(oid, TYPE_RESOURCE, sizeof(struct resource));
}

struct resource* store_resource(void* data, size_t size) {
    struct object_id oid;
    if (write_object(TYPE_RESOURCE, data, size, &oid))
        return NULL;
    return get_resource(&oid);
}
