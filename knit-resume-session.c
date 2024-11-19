#include "hash.h"
#include "session.h"
#include "production.h"

#include <getopt.h>

struct child {
    pid_t pid;
    int fd;
};

static char* session_name;

static void step_status(const struct session_step* ss,
                        const struct production* prd) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    unsigned long long ns = ts.tv_sec * 1000000000 + ts.tv_nsec;

    struct job* job = get_job(oid_of_hash(ss->job_hash));
    fprintf(stderr, "!!step\t%llu\t%s\t%s\t%s\t%s\n",
            ns, session_name, oid_to_hex(&job->object.oid),
            prd ? oid_to_hex(&prd->object.oid) : "-", ss->name);
}

static void dispatch(const struct session_step* ss, pid_t* pid, int* readfd) {
    step_status(ss, NULL);

    int fd[2];
    if (pipe(fd) < 0)
        die_errno("pipe failed");

    *pid = fork();
    if (*pid < 0)
        die_errno("fork failed");
    if (!*pid) {
        dup2(fd[1], STDOUT_FILENO);
        close(STDIN_FILENO);
        close(fd[0]);
        close(fd[1]);
        char* argv[] = {
            "knit-dispatch-job", oid_to_hex(oid_of_hash(ss->job_hash)), NULL
        };
        execvp(argv[0], argv);
        die("execvp failed");
    }
    *readfd = fd[0];
    close(fd[1]);
}

int schedule_children(struct child* children) {
    int settling = 1;
    int unfinished = 0;
    while (settling) {
        settling = 0;
        for (size_t i = 0; i < num_active_steps; i++) {
            struct session_step* ss = active_steps[i];
            if (!ss_hasflag(ss, SS_FINAL)) {
                unfinished = 1;
                if (ss_hasflag(ss, SS_JOB) && !children[i].pid) {
                    settling = 1;
                    dispatch(ss, &children[i].pid, &children[i].fd);
                }
            }
        }
    }
    return unfinished;
}

size_t wait_for_child(const struct child* children) {
    size_t orig_num_active_steps = num_active_steps;
    close_session();

    pid_t pid;
    while (1) {
        // We wait for child processes before reading their output; the pipe
        // buffer should be large enough to hold the production hash.
        int status;
        pid = waitpid(-1, &status, 0);
        if (pid < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            die_errno("waitpid failed");
        }

        if (WIFEXITED(status)) {
            int code = WEXITSTATUS(status);
            if (code)
                die("knit-dispatch-job died with error code %d", code);
            break;
        } else if (WIFSIGNALED(status)) {
            die("knit-dispatch-job died of signal %d", WTERMSIG(status));
        }
    }

    if (load_session(session_name) < 0)
        exit(1);
    if (orig_num_active_steps != num_active_steps)
        die("session %s changed while lock released", session_name);

    for (size_t i = 0; i < num_active_steps; i++) {
        if (children[i].pid == pid)
            return i;
    }
    die("unknown child process");
}

static struct production* read_production(int fd) {
    char buf[KNIT_HASH_HEXSZ + 2];
    ssize_t nread;
    while ((nread = read(fd, buf, sizeof(buf))) < 0) {
        if (errno == EINTR || errno == EAGAIN)
            continue;
        die_errno("read failed");
    }
    if (nread != KNIT_HASH_HEXSZ + 1)
        die("expected %d bytes, got %zd", KNIT_HASH_HEXSZ + 1, nread);
    close(fd);

    if (buf[KNIT_HASH_HEXSZ] != '\n')
        die("expected trailing newline");
    struct object_id oid;
    if (hex_to_oid(buf, &oid) < 0)
        die("illegal production %.*s", KNIT_HASH_HEXSZ, buf);

    struct production* prd = get_production(&oid);
    if (parse_production(prd) < 0)
        die("could not parse production %s", oid_to_hex(&oid));
    return prd;
}

static void die_usage(const char* arg0) {
    fprintf(stderr, "usage: %s <session>\n", arg0);
    exit(1);
}

int main(int argc, char** argv) {
    if (argc != optind + 1)
        die_usage(argv[0]);
    session_name = argv[optind];

    if (load_session(session_name) < 0)
        exit(1);

    struct child children[num_active_steps];
    memset(children, 0, sizeof(children));

    while (schedule_children(children)) {
        size_t step_pos = wait_for_child(children);

        struct production* prd = read_production(children[step_pos].fd);
        children[step_pos].pid = 0;
        children[step_pos].fd = 0;

        struct session_step* ss = active_steps[step_pos];
        if (ss_hasflag(ss, SS_FINAL))
            die("step %s already finished", ss->name);
        memcpy(ss->prd_hash, prd->object.oid.hash, KNIT_HASH_RAWSZ);
        ss_setflag(ss, SS_FINAL);

        step_status(ss, prd);

        resolve_dependencies(step_pos, prd->outputs);

        if (save_session() < 0)
            exit(1);
    }

    close_session();

    char* args[] = { "knit-close-session", session_name, NULL };
    execvp(args[0], args);
    die("execvp failed");
}
