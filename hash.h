#pragma once

#include "util.h"

#define KNIT_HASH_RAWSZ 32 // SHA-256
#define KNIT_HASH_HEXSZ (2 * KNIT_HASH_RAWSZ)

struct object_id {
    unsigned char hash[KNIT_HASH_RAWSZ];
};

int hex_to_oid(const char* hex, struct object_id* oid);
// Returns statically allocated buffer; rotates among 4 buffers so recent
// results are valid when invoked multiple times (like in printf()).
const char* oid_to_hex(const struct object_id* oid);
// Returns statically allocated object_id.
struct object_id* oid_of_hash(uint8_t* hash);

#define TYPE_RESOURCE   0x72657300 // "res\0"
#define TYPE_JOB        0x6a6f6200 // "job\0"
#define TYPE_PRODUCTION 0x70726400 // "prd\0"

uint32_t make_typesig(const char* type); // type should be 4 bytes
// Returns statically allocated, null-terminated buffer; rotates among 4 buffers
// so recent results are valid when invoked multiple times (like in printf()).
char* strtypesig(uint32_t typesig);

int write_object(uint32_t typesig, void* data, size_t size,
                 struct object_id* out_oid);
void* read_object(const struct object_id* oid, uint32_t* typesig, size_t* size);
void* read_object_of_type(const struct object_id* oid, uint32_t typesig, size_t* size);
