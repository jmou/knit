#pragma once

#include "util.h"

#define KNIT_HASH_RAWSZ 32 // SHA-256
#define KNIT_HASH_HEXSZ (2 * KNIT_HASH_RAWSZ)

struct object_id {
    unsigned char hash[KNIT_HASH_RAWSZ];
};

void oidcpy(struct object_id* dst, const struct object_id* src);

int is_valid_hex_oid(const char* hex);

int hex_to_oid(const char* hex, struct object_id* oid);
// Returns statically allocated buffer (not reentrant).
const char* oid_to_hex(const struct object_id* oid);

#define TYPE_RESOURCE   0x72657300 // "res\0"
#define TYPE_JOB        0x6a6f6200 // "job\0"
#define TYPE_PRODUCTION 0x70726400 // "prd\0"

uint32_t make_typesig(const char* type); // type must be 4 bytes
// Returns statically allocated, null-terminated buffer.
char* strtypesig(uint32_t typesig);

#define OBJECT_HEADER_SIZE 8

int write_object(uint32_t typesig, void* data, size_t size,
                 struct object_id* out_oid);
int read_object(const struct object_id* oid,
                struct bytebuf* bbuf, uint32_t* typesig);
