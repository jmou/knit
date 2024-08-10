#include "hash.h"
#include "session.h"

static int debug;
static int positional;
static int wants_available;
static int wants_blocked;
static int wants_prepared;
static int wants_recorded;
static int wants_unresolvable;

static const char* to_hex(const uint8_t* rawhash) {
    struct object_id oid;
    memcpy(oid.hash, rawhash, KNIT_HASH_RAWSZ);
    return oid_to_hex(&oid);
}

static void emit(size_t step_pos, struct session_step* ss) {
    if (debug) {
        printf("step @%zu \t%-2u %04x\t%s\n",
               step_pos, ntohs(ss->num_pending), ntohs(ss->ss_flags), ss->name);
        printf("     job\t%s\n", to_hex(ss->job_hash));
        printf("     production\t%s\n", to_hex(ss->prd_hash));

        for (size_t i = 0; i < num_inputs; i++) {
            struct session_input* si = active_inputs[i];
            if (ntohl(si->step_pos) != step_pos)
                continue;
            printf("     input\t%-2zu %04x\t%s\n", i, ntohs(si->si_flags), si->path);
            printf("     resource\t%s\n", to_hex(si->res_hash));
        }

        for (size_t i = 0; i < num_deps; i++) {
            struct session_dependency* sd = active_deps[i];
            if (ntohl(sd->step_pos) != step_pos)
                continue;
            printf("     dependency\t%-2u %04x\t%s\n",
                   ntohl(sd->input_pos), ntohs(sd->sd_flags), sd->output);
        }
    } else if (positional) {
        printf("@%zu\n", step_pos);
    } else {
        puts(ss->name);
    }
}

static void die_usage(char* arg0) {
    fprintf(stderr, "usage: %s [--available] [--positional|--debug]\n", arg0);
    exit(1);
}

int main(int argc, char** argv) {
    int wants_all = 1;
    int i;
    for (i = 1; i < argc; i++) {
        char* flag = argv[i];
        if (!strcmp(flag, "--debug")) {
            debug = 1;
        } else if (!strcmp(flag, "--positional")) {
            positional = 1;
        } else if (!strcmp(flag, "--available")) {
            wants_available = 1;
            wants_all = 0;
        } else if (!strcmp(flag, "--recorded")) {
            wants_recorded = 1;
            wants_all = 0;
        } else if (!strcmp(flag, "--unresolvable")) {
            wants_unresolvable = 1;
            wants_all = 0;
        } else {
            break;
        }
    }
    if (i + 1 != argc)
        die_usage(argv[0]);
    if (load_session(argv[i]) < 0)
        exit(1);

    if (wants_all) {
        wants_available = 1;
        wants_blocked = 1;
        wants_prepared = 1;
        wants_recorded = 1;
        wants_unresolvable = 1;
    }

    if (debug)
        printf("counts \t\t%-3zu %-3zu %-3zu\n", num_steps, num_inputs, num_deps);

    for (size_t i = 0; i < num_steps; i++) {
        struct session_step* ss = active_steps[i];
        if (ss_hasflag(ss, SS_JOB)) {
            if (ss_hasflag(ss, SS_FINAL)) {
                if (wants_recorded)
                    emit(i, ss);
            } else {
                if (wants_prepared)
                    emit(i, ss);
            }
        } else {
            if (ss_hasflag(ss, SS_FINAL)) {
                if (wants_unresolvable)
                    emit(i, ss);
            } else if (ss->num_pending) {
                if (wants_blocked)
                    emit(i, ss);
            } else {
                if (wants_available)
                    emit(i, ss);
            }
        }
    }

    return 0;
}
