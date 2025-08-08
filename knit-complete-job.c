#include "hash.h"
#include "production.h"
#include "resource.h"
#include "spec.h"
#include "util.h"

#include <getopt.h>
#include <sys/socket.h>
#include <sys/un.h>

static int open_run_socket() {
    int sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sockfd < 0)
        die_errno("socket failed");

    struct sockaddr_un addr = { };
    addr.sun_family = AF_UNIX;
    if (snprintf(addr.sun_path, sizeof(addr.sun_path) - 1,
                 "%s/run.sock", get_knit_dir()) >= (int)sizeof(addr.sun_path))
        die("run socket path too long");

    if (connect(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
        die_errno("connect failed");

    return sockfd;
}

enum options {
    OPT_JOB,
    OPT_FAIL,
    OPT_FILE,
    OPT_DIRECTORY,
};

static struct option longopts[] = {
    { .name = "job", .has_arg = 1, .val = OPT_JOB },
    { .name = "fail", .has_arg = 0, .val = OPT_FAIL },
    { .name = "file", .has_arg = 1, .val = OPT_FILE },
    { .name = "directory", .has_arg = 1, .val = OPT_DIRECTORY },
    { 0 }
};

static void die_usage(const char* arg0) {
    int len = strlen(arg0);
    fprintf(stderr, "usage: %*s [--job <job>] [--fail]\n", len, arg0);
    fprintf(stderr, "       %*s [--file <file>] [--directory <dir>]\n", len, arg0);
    exit(1);
}

int main(int argc, char** argv) {
    struct job* job = NULL;
    struct resource_list* outputs = NULL;

    int fail = 0;

    int opt;
    while ((opt = getopt_long(argc, argv, "", longopts, NULL)) != -1) {
        switch (opt) {
        case OPT_JOB:
            job = peel_job(optarg);
            if (!job)
                exit(1);
            break;
        case OPT_FAIL:
            fail = 1;
            break;
        case OPT_FILE: {
            struct resource* res = store_resource_file(optarg);
            if (!res)
                exit(1);

            // Remove filename prefix until /./ if present (like rsync).
            const char* base = strstr(optarg, "/./");
            const char* filename = base ? base + 3 : optarg;

            resource_list_insert(&outputs, filename, res);
            break;
        }
        case OPT_DIRECTORY:
            if (resource_list_insert_dir_files(&outputs, optarg, "") < 0)
                die_errno("directory traversal failed on %s", optarg);
            break;
        default:
            die_usage(argv[0]);
        }
    }

    if (!fail)
        resource_list_insert(&outputs, PRODUCTION_OUTPUT_OK,
                             get_empty_resource());

    struct production* prd = store_production(job, NULL, outputs);

    char msg[KNIT_HASH_HEXSZ + 13];
    if (snprintf(msg, sizeof(msg), "production %s\n",
                 oid_to_hex(&prd->object.oid)) != (int)sizeof(msg) - 1)
        die("message size mismatch");

    int sockfd = open_run_socket();
    if (write_fully(sockfd, msg, sizeof(msg)) < 0)
        die("socket write failed");

    return 0;
}
