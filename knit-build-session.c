#include "session.h"

static void die_usage(const char* arg0) {
    fprintf(stderr, "usage: %s <session> < <build-instructions>\n", arg0);
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
    if (argc != 2)
        die_usage(argv[0]);
    if (new_session(argv[1]) < 0)
        exit(1);

    ssize_t step_pos = -1;
    ssize_t input_pos = -1;
    int should_compile_job = 0;

    char* line = NULL;
    size_t size = 0;
    ssize_t nread;
    while (errno = 0, (nread = getline(&line, &size, stdin)) >= 0) {
        if (line[nread - 1] != '\n')
            die("unterminated line");
        line[nread - 1] = '\0';

        char* s = line;
        if (removeprefix(&s, "step ")) {
            if (should_compile_job && compile_job_for_step(step_pos) < 0)
                exit(1);
            step_pos = create_session_step(s);
            should_compile_job = 1;
        } else if (removeprefix(&s, "input ")) {
            if (step_pos < 0)
                die("step must precede input");
            input_pos = create_session_input(step_pos, s);
        } else if (removeprefix(&s, "resource ")) {
            if (input_pos < 0)
                die("input must precede resource");
            struct session_input* si = active_inputs[input_pos];
            struct object_id res_oid;
            if (strlen(s) != KNIT_HASH_HEXSZ || hex_to_oid(s, &res_oid) < 0)
                die("invalid resource hash");
            memcpy(si->res_hash, res_oid.hash, KNIT_HASH_RAWSZ);
            si_setflag(si, SI_RESOURCE | SI_FINAL);
        } else if (removeprefix(&s, "dependency ")) {
            uint16_t flags = 0;
            if (removeprefix(&s, "input ")) {
                if (input_pos < 0)
                    die("input must precede input dependency");
            } else if (removeprefix(&s, "step ")) {
                flags |= SD_INPUTISSTEP;
                if (step_pos < 0)
                    die("step must precede step dependency");
            } else {
                die("dependency must be on input or step");
            }
            if (removeprefix(&s, "required ")) {
                flags |= SD_REQUIRED;
            } else if (!removeprefix(&s, "optional ")) {
                die("dependency must be required or optional");
            }
            if (removeprefix(&s, "prefix "))
                flags |= SD_PREFIX;
            size_t dep_pos;
            int off;
            if (sscanf(s, "%zu %n", &dep_pos, &off) != 1)
                die("couldn't parse dependency %s", s);
            create_session_dependency(flags & SD_INPUTISSTEP ? step_pos : input_pos,
                                      dep_pos, s + off, flags);
            should_compile_job = 0;
        } else if (!strcmp(s, "done")) {
            if (fgetc(stdin) != EOF)
                die("trailing input after done");
            if (should_compile_job && compile_job_for_step(step_pos) < 0)
                exit(1);
            if (save_session() < 0)
                exit(1);
            return 0;
        } else {
            die("invalid line: %s", s);
        }
    }
    free(line);
    if (nread < 0 && errno > 0)
        die_errno("cannot read stdin");
    die("missing done line");
}
