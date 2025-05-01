#pragma once

#include "hash.h"

struct object {
    uint32_t typesig;
    struct object_id oid;
    unsigned is_parsed : 1;

    // Subcommands may store arbitrary data here. Initially zeroed out.
    void* extra;
};

// For internal use.
void* intern_object(const struct object_id* oid, uint32_t typesig, size_t size);
