#include "production.h"
#include "session.h"

static void resolve_dependencies(size_t step_pos,
                                 const struct resource_list* outputs) {
    size_t dep_pos = 0;
    while (dep_pos < num_active_deps && ntohl(active_deps[dep_pos]->step_pos) < step_pos)
        dep_pos++;

    while (dep_pos < num_active_deps) {
        struct session_dependency* dep = active_deps[dep_pos];
        if (dep->step_pos != htonl(step_pos))
            break;

        // Iterate production outputs in parallel with the dependencies waiting
        // on this step. Compare their paths to find matching dependencies.
        int cmp = outputs ? strcmp(outputs->path, dep->output) : 1;

        // If a production output has no dependencies on it, we can skip it.
        if (cmp < 0) {
            outputs = outputs->next;
            continue;
        }

        size_t input_pos = ntohl(dep->input_pos);
        if (input_pos >= num_active_inputs)
            die("input out of bounds");
        struct session_input* input = active_inputs[input_pos];

        size_t dependent_pos = ntohl(input->step_pos);
        if (dependent_pos >= num_active_steps)
            die("dependent out of bounds");
        if (dependent_pos <= step_pos)
            die("step later than its dependent");
        struct session_step* dependent = active_steps[dependent_pos];

        // Otherwise, we have a dependency to resolve. If we have no matching
        // production output, then the dependency is missing.
        if (cmp > 0) {
            si_setflag(input, SI_FINAL);
            if (!sd_hasflag(dep, SD_REQUIRED)) {
                goto mark_resolved;
            } else if (!ss_hasflag(dependent, SS_FINAL)) {
                // If the missing dependency is required, its dependent step
                // must finish (with unmet requirements). Since there are no
                // production outputs, we trivially resolve dependencies that
                // are provided by the dependent step. This in turn may finish
                // several additional steps with unmet requirements.
                resolve_dependencies(dependent_pos, NULL);
                ss_setflag(dependent, SS_FINAL);
            }
            dep_pos++;
            continue;
        }

        // Note: cmp == 0
        // The production and dependency outputs match so write through to the
        // dependent step.
        memcpy(input->res_hash, outputs->res->object.oid.hash, KNIT_HASH_RAWSZ);
        si_setflag(input, SI_RESOURCE | SI_FINAL);

mark_resolved:
        if (!dependent->num_unresolved)
            die("num_unresolved underflow on step %s", dependent->name);
        ss_dec_unresolved(dependent);
        if (!dependent->num_unresolved)
            if (compile_job_for_step(dependent_pos) < 0)
                exit(1);
        dep_pos++;
        // The same production output may resolve additional dependencies, so
        // leave it for the next iteration.
    }
}

void die_usage(char* arg0) {
    fprintf(stderr, "usage: %s <session> <job> <production>\n", arg0);
    exit(1);
}

int main(int argc, char** argv) {
    if (argc != 4)
        die_usage(argv[0]);

    if (load_session(argv[1]) < 0)
        exit(1);

    struct object_id oid;
    if (hex_to_oid(argv[2], &oid) < 0)
        die("invalid job hash");
    struct job* job = get_job(&oid);
    if (parse_job(job) < 0)
        exit(1);

    if (hex_to_oid(argv[3], &oid) < 0)
        die("invalid production hash");
    struct production* prd = get_production(&oid);
    if (parse_production(prd) < 0)
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
            memcpy(oid.hash, ss->prd_hash, KNIT_HASH_RAWSZ);
            die("step already fulfilled with production %s", oid_to_hex(&oid));
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