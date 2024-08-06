#include <errno.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define COMMAND_MAX 32

void die_usage(char* arg0) {
    fprintf(stderr, "usage: %s <command> [<args>]\n", arg0);
    exit(1);
}

int main(int argc, char** argv) {
    if (argc < 2 || strlen(argv[1]) > COMMAND_MAX)
        die_usage(argv[0]);

    if (setenv("KNIT_DIR", ".knit", 0) < 0) {
        perror("setenv");
        exit(1);
    }

    char* bin_dir = dirname(argv[0]);
    char* env_path = getenv("PATH");
    if (env_path) {
        char* newpath = malloc(strlen(bin_dir) + strlen(env_path) + 2);
        if (!newpath) {
            perror("malloc");
            exit(1);
        }
        sprintf(newpath, "%s:%s", bin_dir, env_path);
        // We are about to exec() so it's fine to leak newpath.
        env_path = newpath;
    } else {
        env_path = bin_dir;
    }
    if (setenv("PATH", env_path, 1) < 0) {
        perror("setenv");
        exit(1);
    }

    char subcmd[COMMAND_MAX + 6];
    sprintf(subcmd, "knit-%s", argv[1]);
    argv[1] = subcmd;
    execvp(subcmd, &argv[1]);  // should not return

    if (errno == ENOENT) {
        fprintf(stderr, "knit: '%s' is not a knit command\n", argv[1] + 5);
    } else {
        perror("exec");
    }
    exit(1);
}
