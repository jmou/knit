// This command exists only in vestigial form (and still using the term
// "resolve" improperly since steps are not dependencies); its functionality has
// migrated to knit-build-session and knit-fulfill-step.
// TODO replace entirely when knit-list-steps can be used to read out jobs

#include "session.h"

static void die_usage(char* arg0) {
    fprintf(stderr, "usage: %s <session> <step>\n", arg0);
    exit(1);
}

int main(int argc, char** argv) {
    if (argc != 3)
        die_usage(argv[0]);

    if (load_session(argv[1]) < 0)
        exit(1);
    ssize_t step_pos = find_stepish(argv[2]);
    if (step_pos < 0)
        die("step not found");

    struct session_step* ss = active_steps[step_pos];
    if (ss->num_unresolved)
        die("step blocked on %u dependencies", ntohs(ss->num_unresolved));
    if (ss_hasflag(ss, SS_FINAL) || !ss_hasflag(ss, SS_JOB))
        die("step is not available");

    struct object_id oid;
    memcpy(oid.hash, ss->job_hash, KNIT_HASH_RAWSZ);
    puts(oid_to_hex(&oid));
    return 0;
}
