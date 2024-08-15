#pragma once

#include "object.h"

struct resource {
    struct object object;
};

struct resource* get_resource(const struct object_id* oid);
struct resource* store_resource(void* data, size_t size);

struct resource_list {
    struct resource* res;
    char* path;
    struct resource_list* next;
};