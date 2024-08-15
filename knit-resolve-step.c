#include "session.h"

static void die_usage(char* arg0) {
    fprintf(stderr, "usage: %s <session> <step>\n", arg0);
    exit(1);
}

static int resolve_step_to_job(size_t step_pos, struct object_id* job_oid) {
    size_t i = 0;
    while (i < num_active_inputs && ntohl(active_inputs[i]->step_pos) < step_pos)
        i++;
    size_t start = i;
    while (i < num_active_inputs && ntohl(active_inputs[i]->step_pos) == step_pos)
        i++;
    size_t limit = i;

    size_t size = (limit - start) * KNIT_HASH_RAWSZ;
    for (i = start; i < limit; i++)
        size += si_path_len(active_inputs[i]) + 1;

    char* buf = xmalloc(size);
    char* p = buf;
    for (i = start; i < limit; i++) {
        struct session_input* si = active_inputs[i];
        size_t len = si_path_len(si);
        memcpy(p, si->path, len + 1);
        p += len + 1;
        memcpy(p, si->res_hash, KNIT_HASH_RAWSZ);
        p += KNIT_HASH_RAWSZ;
    }
    assert(p == buf + size);

    if (write_object(TYPE_JOB, buf, p - buf, job_oid) < 0) {
        free(buf);
        return -1;
    }

    return 0;
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
    if (ss_hasflag(ss, SS_JOB))
        die("step already resolved");
    if (ss_hasflag(ss, SS_FINAL))
        die("step is unresolvable");
    if (ss->num_pending)
        die("step blocked on %u dependencies", ntohs(ss->num_pending));

    struct object_id job_oid;
    if (resolve_step_to_job(step_pos, &job_oid) < 0)
        return -1;
    memcpy(ss->job_hash, job_oid.hash, KNIT_HASH_RAWSZ);
    ss_setflag(ss, SS_JOB);

    if (save_session() < 0)
        return -1;

    puts(oid_to_hex(&job_oid));
    return 0;
}
