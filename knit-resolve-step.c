#include "job.h"
#include "session.h"

static struct job* store_job(struct session_input** inputs, size_t num_inputs) {
    size_t size = sizeof(struct job_header);
    for (size_t i = 0; i < num_inputs; i++)
        size += sizeof(struct job_input) + strlen(inputs[i]->path) + 1;

    char* buf = xmalloc(size);
    struct job_header* hdr = (struct job_header*)buf;
    hdr->num_inputs = htonl(num_inputs);
    char* p = buf + sizeof(*hdr);

    for (size_t i = 0; i < num_inputs; i++) {
        struct job_input* in = (struct job_input*)p;
        memcpy(in->res_hash, inputs[i]->res_hash, KNIT_HASH_RAWSZ);
        size_t pathsize = strlen(inputs[i]->path) + 1;
        memcpy(in->path, inputs[i]->path, pathsize);
        p += sizeof(*in) + pathsize;
    }

    struct object_id oid;
    int rc = write_object(TYPE_JOB, buf, size, &oid);
    free(buf);
    return rc < 0 ? NULL : get_job(&oid);
}

static struct job* resolve_step_to_job(size_t step_pos) {
    size_t i = 0;
    while (i < num_active_inputs && ntohl(active_inputs[i]->step_pos) < step_pos)
        i++;
    size_t start = i;
    while (i < num_active_inputs && ntohl(active_inputs[i]->step_pos) == step_pos)
        i++;
    size_t limit = i;

    return store_job(&active_inputs[start], limit - start);
}

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
    if (ss_hasflag(ss, SS_JOB))
        die("step already resolved");
    if (ss_hasflag(ss, SS_FINAL))
        die("step is unresolvable");
    if (ss->num_pending)
        die("step blocked on %u dependencies", ntohs(ss->num_pending));

    struct job* job = resolve_step_to_job(step_pos);
    if (!job)
        return -1;

    memcpy(ss->job_hash, job->object.oid.hash, KNIT_HASH_RAWSZ);
    ss_setflag(ss, SS_JOB);
    if (save_session() < 0)
        return -1;

    puts(oid_to_hex(&job->object.oid));
    return 0;
}
