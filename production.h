#pragma once

#include "hash.h"

struct resource_list {
    char* path;
    struct object_id oid;
    struct resource_list* next;
};

struct resource_list* resource_list_insert(struct resource_list** list_p,
                                           const char* path,
                                           const struct object_id* oid);

struct production {
    struct object_id job_oid;
    struct resource_list* outputs;
};

int parse_production_bytes(void* data, size_t size, struct production* prd);

int write_production(const struct production* prd, struct object_id* oid);
