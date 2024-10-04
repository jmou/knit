#pragma once

#include "object.h"

struct resource {
    struct object object;
};

struct resource* get_resource(const struct object_id* oid);
int parse_resource(struct resource* res);
struct resource* store_resource(const void* data, size_t size);
struct resource* store_resource_file(const char* filename);

struct resource_list {
    char* name;
    struct resource* res;
    struct resource_list* next;
};

struct resource_list* resource_list_insert(struct resource_list** list_p,
                                           const char* name,
                                           struct resource* res);
void resource_list_remove_and_free(struct resource_list** list_p);

// Recursively walk dir and add all files to *list_p. The name will be relative
// to dir and prepended with prefix.
//
// Returns the number of files added; on error, returns -1 and sets errno.
// Not thread-safe.
int resource_list_insert_dir_files(struct resource_list** list_p,
                                   const char* dir, const char* prefix);
