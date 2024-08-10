#include "production.h"
#include "session.h"

static void satisfy_dependencies(size_t step_pos,
                                 const struct resource_list* outputs) {
    size_t dep_pos = 0;
    while (dep_pos < num_deps && ntohl(active_deps[dep_pos]->step_pos) < step_pos)
        dep_pos++;

    while (dep_pos < num_deps) {
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
        if (input_pos >= num_inputs)
            die("input out of bounds");
        struct session_input* input = active_inputs[input_pos];

        size_t dependent_pos = ntohl(input->step_pos);
        if (dependent_pos >= num_steps)
            die("dependent out of bounds");
        if (dependent_pos <= step_pos)
            die("step later than its dependent");
        struct session_step* dependent = active_steps[dependent_pos];

        // Otherwise, we have a dependency to satisfy. If we have no matching
        // production output, then any step depending on it is unresolvable.
        if (cmp > 0) {
            si_setflag(input, SI_FINAL);
            if (!ss_hasflag(dependent, SS_FINAL)) {
                satisfy_dependencies(dependent_pos, NULL);
                ss_setflag(dependent, SS_FINAL);
            }
            dep_pos++;
            continue;
        }

        // Note: cmp == 0
        // The production and dependency outputs match so write through to the
        // dependent step.
        memcpy(input->res_hash, outputs->oid.hash, KNIT_HASH_RAWSZ);
        si_setflag(input, SI_RESOURCE | SI_FINAL);
        if (!dependent->num_pending)
            die("num_pending underflow on step %s", dependent->name);
        ss_dec_pending(dependent);
        dep_pos++;
        // The same production output may satisfy multiple dependencies, so
        // leave it for the next iteration.
    }
}

void die_usage(char* arg0) {
    fprintf(stderr, "usage: %s <session> <step> <production>\n", arg0);
    exit(1);
}

int main(int argc, char** argv) {
    if (argc != 4)
        die_usage(argv[0]);

    if (load_session(argv[1]) < 0)
        exit(1);
    ssize_t step_pos = find_stepish(argv[2]);
    if (step_pos < 0)
        die("invalid step");
    struct object_id prd_oid;
    if (hex_to_oid(argv[3], &prd_oid) < 0)
        die("invalid production hash");

    // TODO make reading a production less cumbersome and error prone
    uint32_t typesig;
    struct bytebuf bb;
    if (read_object(&prd_oid, &bb, &typesig) < 0)
        exit(1);
    if (typesig != TYPE_PRODUCTION)
        die("object is not a production");
    struct production prd;
    if (parse_production_bytes(bb.data + OBJECT_HEADER_SIZE,
                               bb.size - OBJECT_HEADER_SIZE, &prd) < 0)
        exit(1);

    struct session_step* ss = active_steps[step_pos];
    memcpy(ss->prd_hash, prd_oid.hash, KNIT_HASH_RAWSZ);
    // When SS_JOB is set, num_pending is also 0; check both anyway.
    if (!ss_hasflag(ss, SS_JOB) || ss->num_pending)
        die("step not resolved");
    if (ss_hasflag(ss, SS_FINAL))
        die("step already recorded");
    satisfy_dependencies(step_pos, prd.outputs);
    ss_setflag(ss, SS_FINAL);

    if (save_session() < 0)
        return -1;

    return 0;
}
