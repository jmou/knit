#include "hash.h"
#include "production.h"
#include "session.h"
#include "spec.h"

// TODO remove knit-complete-job entirely?

void die_usage(char* arg0) {
    fprintf(stderr, "usage: %s <session> <job> <production>\n", arg0);
    exit(1);
}

int main(int argc, char** argv) {
    if (argc != 4)
        die_usage(argv[0]);

    if (load_session(argv[1]) < 0)
        exit(1);
    struct job* job = peel_job(argv[2]);
    if (!job || parse_job(job) < 0)
        exit(1);
    struct production* prd = peel_production(argv[3]);
    if (!prd || parse_production(prd) < 0)
        exit(1);

    int found_job = 0;
    for (size_t i = 0; i < num_active_steps; i++) {
        struct session_step* ss = active_steps[i];
        if (!ss_hasflag(ss, SS_JOB) ||
                memcmp(ss->job_hash, job->object.oid.hash, KNIT_HASH_RAWSZ))
            continue;

        if (ss_hasflag(ss, SS_FINAL)) {
            if (!memcmp(prd->object.oid.hash, ss->prd_hash, KNIT_HASH_RAWSZ)) {
                if (!found_job)
                    found_job = -1;
                continue;
            }
            die("step already fulfilled with production %s", oid_to_hex(oid_of_hash(ss->prd_hash)));
        }

        found_job = 1;
        resolve_dependencies(i, prd->outputs);
        memcpy(ss->prd_hash, prd->object.oid.hash, KNIT_HASH_RAWSZ);
        ss_setflag(ss, SS_FINAL);
    }

    if (!found_job) {
        die("no steps matching job");
    } else if (found_job < 0) {
        warning("job redundantly completed");
    } else if (save_session() < 0) {
        exit(1);
    }
    return 0;
}
