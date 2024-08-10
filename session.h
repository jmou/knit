#pragma once

#include "hash.h"

struct session_step {
    uint8_t job_hash[KNIT_HASH_RAWSZ];
    uint8_t prd_hash[KNIT_HASH_RAWSZ];
    uint16_t num_pending;
    uint16_t ss_flags;
    char name[];
};

size_t create_session_step(const char* name);

#define SS_FINAL    0x8000
#define SS_JOB      0x4000
#define SS_NAMEMASK 0x0fff

#define ss_init_flags(ss, name_len) ((void)(ss->ss_flags = htons(name_len) & SS_NAMEMASK))
#define ss_setflag(ss, flag) ((void)(ss->ss_flags |= htons(flag)))
#define ss_hasflag(ss, flag) (ntohs(ss->ss_flags) & (flag))
#define ss_name_len(ss) (ntohs(ss->ss_flags) & SS_NAMEMASK)
#define ss_size(ss) (sizeof(struct session_step) + ss_name_len(ss) + 1)
static inline void ss_inc_pending(struct session_step* ss) {
    ss->num_pending = htons(ntohs(ss->num_pending) + 1);
}
static inline void ss_dec_pending(struct session_step* ss) {
    ss->num_pending = htons(ntohs(ss->num_pending) - 1);
}

struct session_input {
    uint32_t step_pos;
    uint8_t res_hash[KNIT_HASH_RAWSZ];
    uint16_t si_flags;
    char path[];
};

// SI_FINAL and SI_RESOURCE only exist for diagnostics.
#define SI_FINAL    0x8000
#define SI_RESOURCE 0x4000
#define SI_PATHMASK 0x0fff

#define si_init_flags(si, path_len) ((void)(si->si_flags = htons(path_len) & SI_PATHMASK))
#define si_setflag(si, flag) ((void)(si->si_flags |= htons(flag)))
#define si_hasflag(si, flag) (ntohs(si->si_flags) & (flag))
#define si_path_len(si) (ntohs(si->si_flags) & SI_PATHMASK)
#define si_size(si) (sizeof(struct session_input) + si_path_len(si) + 1)

size_t create_session_input(size_t step_pos, const char* path);

struct session_dependency {
    uint32_t input_pos;
    uint32_t step_pos;
    uint16_t sd_flags;
    char output[];
};

// There are no plans to add flags to dependencies, but we maintain a similar
// code structure.
#define SD_OUTPUTMASK 0x0fff

#define sd_init_flags(sd, output_len) ((void)(sd->sd_flags = htons(output_len) & SD_OUTPUTMASK))
#define sd_setflag(sd, flag) ((void)(sd->sd_flags |= htons(flag)))
#define sd_hasflag(sd, flag) (ntohs(sd->sd_flags) & (flag))
#define sd_output_len(sd) (ntohs(sd->sd_flags) & SD_OUTPUTMASK)
#define sd_size(sd) (sizeof(struct session_dependency) + sd_output_len(sd) + 1)

size_t create_session_dependency(size_t input_pos,
                                 size_t step_pos, const char* output);

extern struct session_step** active_steps;
extern size_t num_steps;
extern struct session_input** active_inputs;
extern size_t num_inputs;
extern struct session_dependency** active_deps;
extern size_t num_deps;

const char* get_session_name();
int load_session(const char* sessname);
int save_session();

// A "stepish" is either a step name, or '@' followed by a decimal step position
// to avoid any ambiguity.
ssize_t find_stepish(const char* stepish);
