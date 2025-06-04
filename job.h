#pragma once

#include "object.h"
#include "resource.h"

enum job_process {
    JOB_PROCESS_INVALID = 0,  // never used by parsed jobs
    JOB_PROCESS_CMD,
    JOB_PROCESS_FLOW,
    JOB_PROCESS_IDENTITY,
    JOB_PROCESS_EXTERNAL,
};

const char* job_process_name(enum job_process process);

struct job {
    struct object object;
    struct resource_list* inputs;
    enum job_process process;
    unsigned is_nocache : 1;
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

#define JOB_INPUT_RESERVED_PREFIX ".knit/"
#define JOB_INPUT_FILES_PREFIX ".knit/files/"
#define JOB_INPUT_CMD ".knit/cmd"
#define JOB_INPUT_FLOW ".knit/flow"
#define JOB_INPUT_IDENTITY ".knit/identity"
#define JOB_INPUT_EXTERNAL ".knit/external"
#define JOB_INPUT_NOCACHE ".knit/nocache"
