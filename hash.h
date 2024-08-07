#pragma once

#include "util.h"

#define KNIT_HASH_RAWSZ 32 // SHA-256
#define KNIT_HASH_HEXSZ (2 * KNIT_HASH_RAWSZ)

#define TYPE_MAX 16

struct object_id {
    unsigned char hash[KNIT_HASH_RAWSZ];
};

void oidcpy(struct object_id* dst, const struct object_id* src);

int is_valid_hex_oid(const char* hex);

int hex_to_oid(const char* hex, struct object_id* oid);
// Returns statically allocated buffer (not reentrant).
const char* oid_to_hex(const struct object_id* oid);

int write_object(const char* type, void* data, size_t size,
                 struct object_id* out_oid);
int read_object_hex(const char* hex, struct bytebuf* bbuf,
                    char* type, size_t* hdr_len);
