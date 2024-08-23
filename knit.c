#include "util.h"

void die_usage(char* arg0) {
    fprintf(stderr, "usage: %s [-C <dir>] <command> [<args>]\n", arg0);
    exit(1);
}

int main(int argc, char** argv) {
    // We are about to exec() so it's fine (even preferable) to leak memory.

    char* bin_dir = realpath(dirname(argv[0]), NULL);
    if (!bin_dir) {
        perror("realpath");
        exit(1);
    }

    int argi = 1;
    while (argv[argi] && *argv[argi] == '-') {
        const char* option = argv[argi++];
        if (!strcmp(option, "-C")) {
            const char* dir = argv[argi++];
            if (!dir)
                die("missing directory argument to -C");
            if (chdir(dir) < 0)
                die_errno("cannot change to %s", dir);
        } else {
            die("unrecognized option %s", option);
        }
    }
    if (argc < argi + 1)
        die_usage(argv[0]);

    char* env_path = getenv("PATH");
    if (env_path) {
        char* newpath = xmalloc(strlen(bin_dir) + strlen(env_path) + 2);
        sprintf(newpath, "%s:%s", bin_dir, env_path);
        env_path = newpath;
    } else {
        env_path = bin_dir;
    }
    if (setenv("PATH", env_path, 1) < 0) {
        perror("setenv");
        exit(1);
    }

    char* subcmd = xmalloc(strlen(argv[argi]) + 6);
    sprintf(subcmd, "knit-%s", argv[argi]);
    argv[argi] = subcmd;
    execvp(subcmd, &argv[argi]); // should not return
    if (errno == ENOENT) {
        fprintf(stderr, "knit: '%s' is not a knit command\n", subcmd + 5);
    } else {
        perror("exec");
    }
    exit(1);
}
