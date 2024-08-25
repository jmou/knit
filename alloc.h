#pragma once

#include "util.h"

struct bump_list {
    char* base;
    size_t nused;
    struct bump_list* next;
};

void free_bump_list(struct bump_list** bump_p);

void* bump_alloc(struct bump_list** bump_p, size_t size);
