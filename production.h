#pragma once

#include "job.h"
#include "object.h"
#include "resource.h"

struct production {
    struct object object;
    struct job* job;
    struct resource_list* outputs;
};

struct production* get_production(const struct object_id* oid);
int parse_production(struct production* prd);
int parse_production_bytes(struct production* prd, void* data, size_t size);
struct production* store_production(struct job* job, struct resource_list* outputs);
