#pragma once

#include "object.h"

struct resource {
    struct object object;
};

struct resource* get_resource(const struct object_id* oid);
int parse_resource(struct resource* res);
struct resource* store_resource(void* data, size_t size);
struct resource* store_resource_file(const char* filename);

struct resource_list {
    char* name;
    struct resource* res;
    struct resource_list* next;
};

struct resource_list* resource_list_insert(struct resource_list** list_p,
                                           const char* name,
                                           struct resource* res);
void free_resource_list(struct resource_list* list);
