#pragma once

#include "util.h"

#define KNIT_HASH_RAWSZ 32 // SHA-256
#define KNIT_HASH_HEXSZ (2 * KNIT_HASH_RAWSZ)

struct object_id {
    unsigned char hash[KNIT_HASH_RAWSZ];
};

int is_valid_hex_oid(const char* hex);

// Returns statically allocated buffer (not reentrant).
const char* oid_to_hex(const struct object_id* oid);

int oid_path(const char* hex, char* filename);
