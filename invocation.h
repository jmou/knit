#pragma once

#include "job.h"
#include "object.h"
#include "production.h"
#include "resource.h"

struct invocation_entry_list {
    struct job* job;
    struct production* prd;
    struct invocation_entry_list* next;
};

struct invocation {
    struct object object;
    struct invocation_entry_list* entries;
    struct invocation_entry_list* terminal;
};

struct invocation* get_invocation(const struct object_id* oid);
int parse_invocation(struct invocation* inv);
int parse_invocation_bytes(struct invocation* inv, void* data, size_t size);
