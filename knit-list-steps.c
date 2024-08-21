#include "hash.h"
#include "session.h"

static int debug;
static int porcelain;
static int wants_available;
static int wants_blocked;
static int wants_fulfilled;
static int wants_unmet;

static const char* to_hex(const uint8_t* rawhash) {
    struct object_id oid;
    memcpy(oid.hash, rawhash, KNIT_HASH_RAWSZ);
    return oid_to_hex(&oid);
}

static const char* pflags(uint16_t flags) {
    const char binbyte[16][5] = {
        "0000", "0001", "0010", "0011", "0100", "0101", "0110", "0111",
        "1000", "1001", "1010", "1011", "1100", "1101", "1110", "1111",
    };
    flags = ntohs(flags);
    static_assert(0xfff == SS_NAMEMASK);
    static_assert(0xfff == SI_PATHMASK);
    static_assert(0xfff == SD_OUTPUTMASK);
    static char buf[10];
    sprintf(buf, "%.4s %4u", binbyte[flags >> 12], flags & 0xfff);
    return buf;
}

static void emit(size_t step_pos, struct session_step* ss) {
    if (debug) {
        printf("step @%zu \t%-2u %s %s\n",
               step_pos, ntohs(ss->num_unresolved), pflags(ss->ss_flags), ss->name);
        printf("     job\t%s\n", to_hex(ss->job_hash));
        printf("     production\t%s\n", to_hex(ss->prd_hash));

        for (size_t i = 0; i < num_active_inputs; i++) {
            struct session_input* si = active_inputs[i];
            if (ntohl(si->step_pos) != step_pos)
                continue;
            printf("     input\t%-2zu %s %s\n", i, pflags(si->si_flags), si->path);
            printf("     resource\t%s\n", to_hex(si->res_hash));
        }

        for (size_t i = 0; i < num_active_deps; i++) {
            struct session_dependency* sd = active_deps[i];
            if (ntohl(sd->step_pos) != step_pos)
                continue;
            printf("     dependency\t%-2u %s %s\n",
                   ntohl(sd->input_pos), pflags(sd->sd_flags), sd->output);
        }
    } else if (porcelain) {
        printf("@%zu\t", step_pos);
        if (ss_hasflag(ss, SS_FINAL)) {
            if (ss_hasflag(ss, SS_JOB))
                printf("f");
            else
                printf("u");
        } else {
            if (ss_hasflag(ss, SS_JOB))
                printf("a");
            else
                printf("%d", ntohs(ss->num_unresolved));
        }
        printf("\t%s\t%s\t%s\n", to_hex(ss->job_hash), to_hex(ss->prd_hash), ss->name);
    } else {
        puts(ss->name);
    }
}

static void die_usage(char* arg0) {
    fprintf(stderr, "usage: %s [--available] [--blocked] [--fulfilled] [--unmet] [--porcelain|--debug] <session>\n", arg0);
    exit(1);
}

int main(int argc, char** argv) {
    int rc = 0;
    int wants_all = 1;
    int i;
    for (i = 1; i < argc; i++) {
        char* flag = argv[i];
        if (!strcmp(flag, "--debug")) {
            debug = 1;
        } else if (!strcmp(flag, "--porcelain")) {
            porcelain = 1;
        } else if (!strcmp(flag, "--available")) {
            wants_available = 1;
            wants_all = 0;
        } else if (!strcmp(flag, "--blocked")) {
            wants_blocked = 1;
            wants_all = 0;
        } else if (!strcmp(flag, "--fulfilled")) {
            wants_fulfilled = 1;
            wants_all = 0;
        } else if (!strcmp(flag, "--unmet")) {
            wants_unmet = 1;
            wants_all = 0;
        } else {
            break;
        }
    }
    if (i + 1 != argc)
        die_usage(argv[0]);
    if (load_session_nolock(argv[i]) < 0)
        exit(1);

    if (wants_all) {
        wants_available = 1;
        wants_blocked = 1;
        wants_fulfilled = 1;
        wants_unmet = 1;
    }

    if (debug)
        printf("counts \t\t%-3zu %-3zu %-3zu\n", num_active_steps, num_active_inputs, num_active_deps);

    for (size_t i = 0; i < num_active_steps; i++) {
        struct session_step* ss = active_steps[i];
        if (ss_hasflag(ss, SS_JOB)) {
            if (ss_hasflag(ss, SS_FINAL)) {
                if (wants_fulfilled)
                    emit(i, ss);
            } else {
                if (wants_available)
                    emit(i, ss);
            }
        } else {
            if (ss_hasflag(ss, SS_FINAL)) {
                if (wants_unmet)
                    emit(i, ss);
            } else {
                if (!ss->num_unresolved)
                    rc = error("step not blocked but has no job: %s", ss->name);
                if (wants_blocked)
                    emit(i, ss);
            }
        }
    }

    return rc < 0 ? 1 : 0;
}
