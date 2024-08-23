#pragma once

#include "object.h"
#include "resource.h"

struct job {
    struct object object;
    struct resource_list* inputs;
};

struct job* get_job(const struct object_id* oid);
int parse_job(struct job* job);
int parse_job_bytes(struct job* job, void* data, size_t size);
struct job* store_job(struct resource_list* inputs);

struct job_header {
    uint32_t num_inputs;
};

struct job_input {
    uint8_t res_hash[KNIT_HASH_RAWSZ];
    char name[];
};

#define JOB_INPUT_CMD ".knit/cmd"
#define JOB_INPUT_FLOW ".knit/flow"
#define JOB_INPUT_IDENTITY ".knit/identity"
