#include "session.h"

static void die_usage(const char* arg0) {
    fprintf(stderr, "usage: %s < <build-instructions>\n", arg0);
    exit(1);
}

static int removeprefix(char** s, const char* prefix) {
    size_t prefixlen = strlen(prefix);
    if (strncmp(*s, prefix, prefixlen))
        return 0;
    *s += prefixlen;
    return 1;
}

int main(int argc, char** argv) {
    if (argc != 1)
        die_usage(argv[0]);

    ssize_t step_pos = -1;
    ssize_t input_pos = -1;

    char* line = NULL;
    size_t size = 0;
    ssize_t nread;
    while (errno = 0, (nread = getline(&line, &size, stdin)) >= 0) {
        if (line[nread - 1] != '\n')
            die("unterminated line");
        line[nread - 1] = '\0';

        char* s = line;
        if (removeprefix(&s, "step ")) {
            step_pos = create_session_step(s);
        } else if (!strcmp(s, "param")) {
            // TODO handle param
        } else if (removeprefix(&s, "flow ")) {
            // TODO handle flow
        } else if (!strcmp(s, "shell")) {
            // TODO every step is currently treated like shell
        } else if (removeprefix(&s, "input ")) {
            if (step_pos < 0)
                die("step must precede input");
            input_pos = create_session_input(step_pos, s);
        } else if (removeprefix(&s, "resource ")) {
            if (input_pos < 0)
                die("input must precede resource");
            struct session_input* si = active_inputs[input_pos];
            struct object_id res_oid;
            if (hex_to_oid(s, &res_oid) < 0)
                die("invalid resource hash");
            memcpy(si->res_hash, res_oid.hash, KNIT_HASH_RAWSZ);
            si_setflag(si, SI_RESOURCE | SI_FINAL);
        } else if (removeprefix(&s, "dependency ")) {
            if (input_pos < 0)
                die("input must precede dependency");
            size_t dep_pos;
            int off;
            if (sscanf(s, "%zu %n", &dep_pos, &off) != 1)
                die("couldn't parse dependency %s", s);
            create_session_dependency(input_pos, dep_pos, s + off);
        } else if (!strcmp(s, "save")) {
            if (save_session() < 0)
                exit(1);
        } else {
            die("invalid input: %s", s);
        }
    }
    free(line);
    if (nread < 0 && errno > 0)
        die("cannot read stdin: %s", strerror(errno));

    puts(get_session_name());
    return 0;
}
