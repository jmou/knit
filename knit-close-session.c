#include "session.h"

static void die_usage(char* arg0) {
    fprintf(stderr, "usage: %s <session>\n", arg0);
    exit(1);
}

int main(int argc, char** argv) {
    if (argc != 2)
        die_usage(argv[0]);

    if (load_session(argv[1]) < 0)
        exit(1);

    int num_missing_deps = 0;
    for (size_t i = 0; i < num_active_steps; i++) {
        struct session_step* ss = active_steps[i];
        if (!ss_hasflag(ss, SS_FINAL))
            exit(1);
        if (!ss_hasflag(ss, SS_JOB))
            num_missing_deps++;
    }

    // TODO surface invocation summary at a higher level
    if (num_missing_deps > 0) {
        fprintf(stderr, "%d step%s have missing dependencies:\n",
                num_missing_deps, num_missing_deps == 1 ? "" : "s");
        for (size_t i = 0; i < num_active_steps; i++) {
            struct session_step* ss = active_steps[i];
            if (!ss_hasflag(ss, SS_JOB))
                fprintf(stderr, "%s\n", ss->name);
        }
    }

    // TODO invocation id
    puts(get_session_name());
    // TODO remove or rename session file
    return 0;
}
