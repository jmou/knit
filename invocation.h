#pragma once

#include "job.h"
#include "object.h"
#include "production.h"
#include "resource.h"

struct invocation_entry_list {
    struct invocation_entry_list* next;
    char* name;
    // prd will be NULL if the step has unmet dependencies.
    struct production* prd;
};

struct invocation {
    struct object object;
    struct invocation_entry_list* entries;
};

struct invocation* get_invocation(const struct object_id* oid);
int parse_invocation(struct invocation* inv);
int parse_invocation_bytes(struct invocation* inv, void* data, size_t size);
