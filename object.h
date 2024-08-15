#pragma once

#include "hash.h"

struct object {
    uint32_t typesig;
    struct object_id oid;
    unsigned is_parsed : 1;
};

// For internal use.
void* intern_object(const struct object_id* oid, uint32_t typesig, size_t size);
