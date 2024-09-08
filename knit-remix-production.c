#include "invocation.h"
#include "job.h"
#include "production.h"
#include "resource.h"
#include "spec.h"

#include <getopt.h>

struct resource_list* deep_copy(struct resource_list* orig) {
    struct resource_list* ret = NULL;
    struct resource_list** copy_p = &ret;
    for (; orig; orig = orig->next) {
        *copy_p = xmalloc(sizeof(**copy_p));
        memcpy(*copy_p, orig, sizeof(**copy_p));
        copy_p = &(*copy_p)->next;
    }
    return ret;
}

enum options {
    OPT_COPY_JOB_INPUTS,
    OPT_REMOVE_PREFIX,
    OPT_SET_JOB,
    OPT_SET_OUTPUT,
    OPT_WRAP_INVOCATION,
};

static struct option longopts[] = {
    { .name = "copy-job-inputs", .val = OPT_COPY_JOB_INPUTS, .has_arg = 1 },
    { .name = "remove-prefix", .val = OPT_REMOVE_PREFIX, .has_arg = 1 },
    { .name = "set-job", .val = OPT_SET_JOB, .has_arg = 1 },
    { .name = "set-output", .val = OPT_SET_OUTPUT, .has_arg = 1 },
    { .name = "wrap-invocation", .val = OPT_WRAP_INVOCATION, .has_arg = 1 },
    { 0 }
};

static void die_usage(char* arg0) {
    int len = strlen(arg0);
    fprintf(stderr, "usage: %*s [--copy-job-inputs <job>] [--remove-prefix <prefix>]\n", len, arg0);
    fprintf(stderr, "       %*s [--set-job <job>] [--set-output <name>=<resource>]\n", len, "");
    fprintf(stderr, "       %*s [--wrap-invocation <invocation>]\n", len, "");
    exit(1);
}

int main(int argc, char** argv) {
    if (argc < 2)
        die_usage(argv[0]);

    struct invocation* final_inv;
    struct job* final_job = NULL;
    struct resource_list* outputs = NULL;

    char* tok;
    struct job* job;
    struct production* prd;
    struct resource* res;
    int opt;
    while ((opt = getopt_long(argc, argv, "", longopts, NULL)) != -1) {
        switch (opt) {
        case OPT_COPY_JOB_INPUTS:
            job = peel_job(optarg);
            if (!job || parse_job(job) < 0)
                exit(1);
            outputs = deep_copy(job->inputs);
            break;
        case OPT_REMOVE_PREFIX:
            for (struct resource_list** list_p = &outputs; *list_p; list_p = &(*list_p)->next) {
                if (!strncmp((*list_p)->name, optarg, strlen(optarg))) {
                    free(*list_p);
                    *list_p = (*list_p)->next;
                }
            }
            break;
        case OPT_SET_JOB:
            final_job = peel_job(optarg);
            if (!final_job)
                exit(1);
            break;
        case OPT_SET_OUTPUT:
            tok = strpbrk(optarg, "=");
            if (!tok)
                die("missing = after output name");
            *tok++ = '\0';
            res = peel_resource(tok);
            if (!res)
                exit(1);
            resource_list_insert(&outputs, optarg, res);
            break;
        case OPT_WRAP_INVOCATION:
            final_inv = peel_invocation(optarg);
            if (!final_inv || parse_invocation(final_inv) < 0)
                exit(1);
            if (final_inv->terminal->prd) {
                if (parse_production(final_inv->terminal->prd) < 0)
                    exit(1);
                outputs = deep_copy(final_inv->terminal->prd->outputs);
            }
            break;
        default:
            die_usage(argv[0]);
        }
    }

    if (!final_job)
        die("missing job");
    if (!outputs && !final_inv)
        die("no outputs nor invocation");

    prd = store_production(final_job, final_inv, outputs);
    if (!prd)
        exit(1);

    puts(oid_to_hex(&prd->object.oid));
    // leak outputs
    return 0;
}
