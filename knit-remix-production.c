#include "hash.h"
#include "job.h"
#include "production.h"
#include "resource.h"

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
};

static struct option longopts[] = {
    { .name = "copy-job-inputs", .val = OPT_COPY_JOB_INPUTS, .has_arg = 1 },
    { .name = "remove-prefix", .val = OPT_REMOVE_PREFIX, .has_arg = 1 },
    { .name = "set-job", .val = OPT_SET_JOB, .has_arg = 1 },
    { .name = "set-output", .val = OPT_SET_OUTPUT, .has_arg = 1 },
    { 0 }
};

static void die_usage(char* arg0) {
    int len = strlen(arg0);
    fprintf(stderr, "usage: %*s [--copy-job-inputs <job>] [--remove-prefix <prefix>]\n", len, arg0);
    fprintf(stderr, "       %*s [--set-job <job>] [--set-output <name>=<resource>]\n", len, "");
    exit(1);
}

int main(int argc, char** argv) {
    if (argc < 2)
        die_usage(argv[0]);

    struct job* final_job = NULL;
    struct resource_list* outputs = NULL;

    char* tok;
    struct job* job;
    struct object_id oid;
    struct production* prd;
    struct resource* res;
    int opt;
    while ((opt = getopt_long(argc, argv, "", longopts, NULL)) != -1) {
        switch (opt) {
        case OPT_COPY_JOB_INPUTS:
            if (hex_to_oid(optarg, &oid) < 0)
                die("invalid job hash");
            job = get_job(&oid);
            if (parse_job(job) < 0)
                exit(1);
            outputs = deep_copy(job->inputs);
            break;
        case OPT_REMOVE_PREFIX:
            for (struct resource_list** list_p = &outputs; *list_p; list_p = &(*list_p)->next) {
                if (!strncmp((*list_p)->path, optarg, strlen(optarg))) {
                    free(*list_p);
                    *list_p = (*list_p)->next;
                }
            }
            break;
        case OPT_SET_JOB:
            if (hex_to_oid(optarg, &oid) < 0)
                die("invalid job hash");
            final_job = get_job(&oid);
            break;
        case OPT_SET_OUTPUT:
            tok = strpbrk(optarg, "=");
            if (!tok)
                die("missing = after output name");
            *tok++ = '\0';
            if (hex_to_oid(tok, &oid) < 0)
                die("invalid resource hash");
            res = get_resource(&oid);
            resource_list_insert(&outputs, optarg, res);
            break;
        default:
            die_usage(argv[0]);
        }
    }

    if (!final_job)
        die("missing job");
    if (!outputs)
        die("no outputs");

    prd = store_production(final_job, outputs);
    if (!prd)
        exit(1);

    puts(oid_to_hex(&prd->object.oid));
    // leak outputs
    return 0;
}
