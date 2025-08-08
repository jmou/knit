#include "hash.h"
#include "resource.h"
#include "util.h"

#include <getopt.h>
#include <sys/socket.h>
#include <sys/un.h>

// XXX DRY w/ knit-plan-job.c?
static struct resource* get_empty_resource() {
    static struct resource* empty_res = NULL;
    if (!empty_res)
        empty_res = store_resource(NULL, 0);
    return empty_res;
}

static char* format_output_arg(const char* filename, struct resource* res) {
    char* result = xmalloc(strlen(filename) + KNIT_HASH_HEXSZ + 2);  // leaked
    sprintf(result, "%s=%s", filename, oid_to_hex(&res->object.oid));
    return result;
}

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
    char* cmd[argc * 2 + 10];  // intentionally overprovisioned
    int cmd_idx = 0;
    cmd[cmd_idx++] = "knit-remix-production";

    int fail = 0;

    int opt;
    while ((opt = getopt_long(argc, argv, "", longopts, NULL)) != -1) {
        switch (opt) {
        case OPT_JOB:
            cmd[cmd_idx++] = "--set-job";
            cmd[cmd_idx++] = optarg;
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

            cmd[cmd_idx++] = "--set-output";
            cmd[cmd_idx++] = format_output_arg(filename, res);
            break;
        }
        case OPT_DIRECTORY:
            cmd[cmd_idx++] = "--read-outputs-from-dir";
            cmd[cmd_idx++] = optarg;
            break;
        default:
            die_usage(argv[0]);
        }
    }

    if (!fail) {
        cmd[cmd_idx++] = "--set-output";
        cmd[cmd_idx++] = format_output_arg(".knit/ok", get_empty_resource());
    }

    cmd[cmd_idx] = NULL;

    int fds[2];
    if (pipe(fds) < 0)
        die_errno("pipe failed");

    pid_t pid = fork();
    if (pid < 0)
        die_errno("fork failed");
    if (!pid) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        execvp(cmd[0], cmd);
        die_errno("execvp failed");
    }

    close(fds[1]);

    struct bytebuf bbuf;  // leaked
    if (slurp_fd(fds[0], &bbuf) < 0)
        die_errno("cannot read knit-remix-production output");
    close(fds[0]);

    int status;
    if (waitpid(pid, &status, 0) < 0)
        die_errno("waitpid failed");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        exit(1);

    char* prd_hex = bbuf.data;
    if (bbuf.size != KNIT_HASH_HEXSZ + 1 || prd_hex[KNIT_HASH_HEXSZ] != '\n')
        die("unexpected production hex size");
    prd_hex[KNIT_HASH_HEXSZ] = '\0';

    int sockfd = open_run_socket();
    if (write_fully(sockfd, "production ", 11) < 0 ||
            write_fully(sockfd, prd_hex, KNIT_HASH_HEXSZ) < 0 ||
            write_fully(sockfd, "\n", 1) < 0)
        die("socket write failed");

    return 0;
}
