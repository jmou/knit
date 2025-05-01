#include "hash.h"
#include "job.h"
#include "production.h"
#include "session.h"
#include "util.h"

#include <getopt.h>

// Steps corresponding to jobs are stored in the job object extra field.
struct step_list {
    size_t step_pos;
    struct step_list* next;
};

static char* session_name;

static void step_status(const struct session_step* ss,
                        const struct production* prd) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    unsigned long long ns = ts.tv_sec * 1000000000 + ts.tv_nsec;

    struct job* job = get_job(oid_of_hash(ss->job_hash));
    fprintf(stderr, "!!step\t%llu\t%s\t%s\t%s\t%s\n",
            ns, session_name, oid_to_hex(&job->object.oid),
            prd ? oid_to_hex(&prd->object.oid) : "-", ss->name);
}

static void dispatch_step(size_t step_pos) {
    const struct session_step* ss = active_steps[step_pos];
    step_status(ss, NULL);
    struct job* job = get_job(oid_of_hash(ss->job_hash));
    struct step_list* tail = job->object.extra;
    if (!tail)
        puts(oid_to_hex(&job->object.oid));
    struct step_list* head = xmalloc(sizeof(*head));
    head->step_pos = step_pos;
    head->next = tail;
    job->object.extra = head;
}

int schedule_steps(int* steps_dispatched) {
    int unfinished = 0;
    for (size_t i = 0; i < num_active_steps; i++) {
        struct session_step* ss = active_steps[i];
        if (!ss_hasflag(ss, SS_FINAL)) {
            unfinished = 1;
            if (ss_hasflag(ss, SS_JOB) && !steps_dispatched[i]) {
                dispatch_step(i);
                steps_dispatched[i] = 1;
            }
        }
    }
    return unfinished;
}

static struct production* read_production() {
    size_t orig_num_active_steps = num_active_steps;
    close_session();

    char buf[KNIT_HASH_HEXSZ + 2];
    errno = 0;
    if (!fgets(buf, sizeof(buf), stdin)) {
        if (errno)
            die_errno("cannot read stdin");
        die("stdin prematurely closed");
    }
    if (strlen(buf) != KNIT_HASH_HEXSZ + 1 || buf[KNIT_HASH_HEXSZ] != '\n')
        die("malformed line");

    struct object_id oid;
    if (hex_to_oid(buf, &oid) < 0)
        die("invalid production hash");
    struct production* prd = get_production(&oid);
    if (parse_production(prd) < 0)
        die("could not parse production %s", oid_to_hex(&oid));

    if (load_session(session_name) < 0)
        exit(1);
    if (orig_num_active_steps != num_active_steps)
        die("session %s changed while lock released", session_name);

    return prd;
}

static void complete_steps(struct production* prd) {
    struct step_list* list = prd->job->object.extra;
    if (!list) {
        die("cannot complete unscheduled job %s",
            oid_to_hex(&prd->job->object.oid));
    }

    while (list) {
        struct session_step* ss = active_steps[list->step_pos];
        if (ss_hasflag(ss, SS_FINAL))
            die("step %s already finished", ss->name);
        memcpy(ss->prd_hash, prd->object.oid.hash, KNIT_HASH_RAWSZ);
        ss_setflag(ss, SS_FINAL);

        step_status(ss, prd);

        resolve_dependencies(list->step_pos, prd->outputs);

        struct step_list* tmp = list;
        list = tmp->next;
        free(tmp);
    }
    prd->job->object.extra = NULL;
}

static void die_usage(const char* arg0) {
    fprintf(stderr, "usage: %s <session>\n", arg0);
    exit(1);
}

int main(int argc, char** argv) {
    if (argc != optind + 1)
        die_usage(argv[0]);
    session_name = argv[optind];

    if (load_session(session_name) < 0)
        exit(1);

    setlinebuf(stdout);

    int steps_dispatched[num_active_steps];
    memset(steps_dispatched, 0, sizeof(steps_dispatched));

    while (schedule_steps(steps_dispatched)) {
        struct production* prd = read_production();
        complete_steps(prd);
        if (save_session() < 0)
            exit(1);
    }

    close_session();
}
