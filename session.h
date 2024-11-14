#pragma once

#include "hash.h"

struct session_step {
    uint8_t job_hash[KNIT_HASH_RAWSZ];
    uint8_t prd_hash[KNIT_HASH_RAWSZ];
    uint16_t num_unresolved;
    uint16_t ss_flags;
    char name[];
};

size_t create_session_step(const char* name);

#define SS_FINAL    0x8000
#define SS_JOB      0x4000
#define SS_NAMEMASK 0x0fff

#define ss_init_flags(ss, name_len, flags) ((void)((ss)->ss_flags = htons(((name_len) & SS_NAMEMASK) | (flags))))
#define ss_setflag(ss, flag) ((void)((ss)->ss_flags |= htons(flag)))
#define ss_hasflag(ss, flag) (ntohs((ss)->ss_flags) & (flag))
#define ss_name_len(ss) (ntohs((ss)->ss_flags) & SS_NAMEMASK)
#define ss_size(ss) (sizeof(struct session_step) + ss_name_len(ss) + 1)
static inline void ss_inc_unresolved(struct session_step* ss) {
    ss->num_unresolved = htons(ntohs(ss->num_unresolved) + 1);
}
static inline void ss_dec_unresolved(struct session_step* ss) {
    ss->num_unresolved = htons(ntohs(ss->num_unresolved) - 1);
}

struct session_input {
    union {
        uint8_t res_hash[KNIT_HASH_RAWSZ];
        uint32_t fanout_step_pos;
    };
    uint32_t step_pos;
    // TODO remove padding?
    uint16_t padding;
    uint16_t si_flags;
    char name[];
};

// SI_FINAL is technically redundant since it is implied by !ss->num_unresolved.
#define SI_FINAL    0x8000
#define SI_RESOURCE 0x4000
#define SI_FANOUT   0x2000
#define SI_PATHMASK 0x0fff

#define si_init_flags(si, name_len, flags) ((void)((si)->si_flags = htons(((name_len) & SI_PATHMASK) | (flags))))
#define si_setflag(si, flag) ((void)((si)->si_flags |= htons(flag)))
#define si_hasflag(si, flag) (ntohs((si)->si_flags) & (flag))
#define si_name_len(si) (ntohs((si)->si_flags) & SI_PATHMASK)
#define si_size(si) (sizeof(struct session_input) + si_name_len(si) + 1)

size_t create_session_input(size_t step_pos, const char* name);

struct session_dependency {
    uint32_t input_pos;
    uint32_t step_pos;
    // TODO remove padding?
    uint16_t padding;
    uint16_t sd_flags;
    char output[];
};

#define SD_REQUIRED    0x8000
#define SD_PREFIX      0x4000
#define SD_INPUTISSTEP 0x2000
#define SD_OUTPUTMASK  0x0fff

#define sd_init_flags(sd, output_len, flags) ((void)((sd)->sd_flags = htons(((output_len) & SD_OUTPUTMASK) | (flags))))
#define sd_setflag(sd, flag) ((void)((sd)->sd_flags |= htons(flag)))
#define sd_hasflag(sd, flag) (ntohs((sd)->sd_flags) & (flag))
#define sd_output_len(sd) (ntohs((sd)->sd_flags) & SD_OUTPUTMASK)
#define sd_size(sd) (sizeof(struct session_dependency) + sd_output_len(sd) + 1)

size_t create_session_dependency(size_t input_pos,
                                 size_t step_pos, const char* output,
                                 uint16_t flags);

extern struct session_step** active_steps;
extern size_t num_active_steps;
extern struct session_input** active_inputs;
extern size_t num_active_inputs;
extern struct session_dependency** active_deps;
extern size_t num_active_deps;

// add_session_fanout_step() must be called after all normal steps have been
// created; its returned step position is always greater than num_active_steps.
size_t add_session_fanout_step();

extern size_t num_active_fanout;

int new_session(const char* sessname);
int load_session(const char* sessname);
int save_session();
// Cannot be used to save_session() later.
int load_session_nolock(const char* sessname);

int compile_job_for_step(size_t step_pos);
