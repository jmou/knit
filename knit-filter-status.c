#include "util.h"

#include <signal.h>
#include <sys/ioctl.h>

static volatile sig_atomic_t columns = 1 << 20;

static void update_columns([[maybe_unused]] int signo) {
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0)
        columns = w.ws_col;
}

static void maybe_clear_line(int clear_next) {
    static int needs_clear = 0;
    if (needs_clear)
        fputs("\r\e[K", stderr);  // \e[K is ANSI Erase in Line (to right)
    needs_clear = clear_next;
}

static int enable_status;

[[gnu::format(printf, 1, 2)]]
static void statusf(const char* fmt, ...) {
    if (enable_status) {
        va_list argp;
        va_start(argp, fmt);
        maybe_clear_line(0);
        vfprintf(stderr, fmt, argp);
        fflush(stderr);
        va_end(argp);
    }
}

static void hardstatus(const char* buf) {
    if (enable_status) {
        maybe_clear_line(1);
        // Keep the cursor on this line so it will be replaced on the next call.
        fputs(buf, stderr);
        fflush(stderr);
    }
}

struct step_list {
    struct step_list* next;
    char name[];
};

struct session_list {
    struct session_list* next;
    struct step_list* steps;
    char name[];
};

static struct session_list* find_session(struct session_list* sessions,
                                         const char* name) {
    for (struct session_list* session = sessions; session; session = session->next) {
        if (!strcmp(session->name, name))
            return session;
    }
    return NULL;
}

static struct session_list* create_session(struct session_list** list_p,
                                           const char* name) {
    struct session_list* session = xmalloc(sizeof(*session) + strlen(name) + 1);
    strcpy(session->name, name);
    session->steps = NULL;
    // Most recent sessions first.
    session->next = *list_p;
    *list_p = session;
    return session;
}

static void add_step(struct session_list* session, const char* name) {
    struct step_list* step = xmalloc(sizeof(*step) + strlen(name) + 1);
    step->next = NULL;
    strcpy(step->name, name);

    struct step_list** list_p = &session->steps;
    while (*list_p)
        list_p = &(*list_p)->next;
    *list_p = step;
}

static void remove_step(struct session_list* session, const char* name) {
    struct step_list** list_p = &session->steps;
    while (*list_p) {
        if (!strcmp((*list_p)->name, name)) {
            struct step_list* tmp = *list_p;
            *list_p = (*list_p)->next;
            free(tmp);
            break;
        }
        list_p = &(*list_p)->next;
    }
}

struct line_step {
    uint64_t ns;
    char* session;
    char* job;
    char* state;
    char* step;
};

static void split_tab(char* s, char** tok) {
    if (s) {
        *tok = strchr(s, '\t');
        if (*tok)
            *(*tok)++ = '\0';
    } else {
        *tok = NULL;
    }
}

// Line format: !!step\tns\tsession\tjob\tstate\tstep
static int parse_line_step(char* s, struct line_step* parsed) {
    parsed->ns = strtoull(s + 7, &s, 10);
    if (*s == '\t')
        *s++ = '\0';
    else
        s = NULL;
    parsed->session = s;
    split_tab(parsed->session, &parsed->job);
    split_tab(parsed->job, &parsed->state);
    split_tab(parsed->state, &parsed->step);

    return parsed->step ? 0 : -1;
}

struct progress {
    // Job counts
    unsigned num_dispatched;
    unsigned num_cached;
    unsigned num_final;

    // Nanosecond-resolution monotonic timestamps
    uint64_t first_ns;
    uint64_t last_ns;

    struct session_list* sessions;
};

static void progress_handle_step(struct progress* progress,
                                 const struct line_step* parsed) {
    if (progress->first_ns == 0)
        progress->first_ns = parsed->ns;
    progress->last_ns = parsed->ns;

    struct session_list* session = find_session(progress->sessions,
                                                parsed->session);
    if (!session) {
        session = create_session(&progress->sessions, parsed->session);
        statusf("Session %s\n", session->name);
    }

    if (!strcmp(parsed->state, "-")) {
        add_step(session, parsed->step);
        progress->num_dispatched++;
    } else {
        remove_step(session, parsed->step);
        progress->num_final++;
    }
}

static int easy_snprintf(size_t* off, char* s, size_t size,
                         const char* fmt, ...) {
    assert(*off <= size);
    va_list argp;
    va_start(argp, fmt);
    int nr = vsnprintf(s + *off, size - *off, fmt, argp);
    va_end(argp);
    *off += nr < 0 ? 0 : nr;
    if (*off >= size) {
        *off = size;
        return 0;
    }
    return 1;
}

static void format_status(const struct progress* progress,
                          char* buf, size_t size) {
    size_t off = 0;
    easy_snprintf(&off, buf, size, "    job %u/%u",
                  progress->num_final, progress->num_dispatched);

    for (struct session_list* session = progress->sessions; session; session = session->next) {
        if (!session->steps)
            continue;
        if (!easy_snprintf(&off, buf, size, " [%.2s]", session->name))
            break;
        for (struct step_list* step = session->steps; step; step = step->next) {
            if (!easy_snprintf(&off, buf, size, " %s", step->name))
                break;
        }
    }

    if (off >= size) {
        off = size - 1;
        if (off > 3)
            strcpy(&buf[off - 3], "...");
    }
    buf[off] = '\0';
}

static void print_summary(struct progress* progress) {
    float duration = (float)(progress->last_ns - progress->first_ns) / 1000000000;
    const char* plural = progress->num_final > 1 ? "s" : "";
    statusf("Ran %u job%s (%u cached) in %.1f seconds\n",
            progress->num_final, plural, progress->num_cached, duration);
}

static int spawn(char** argv, int infd, int* errfdp) {
    int fds[2];
    if (pipe(fds) < 0)
        die_errno("pipe failed");
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);

    pid_t pid = fork();
    if (pid < 0)
        die_errno("fork failed");
    if (!pid) {
        dup2(infd, STDIN_FILENO);
        dup2(fds[1], STDERR_FILENO);
        execvp(argv[0], argv);
        die_errno("execvp failed");
    }

    close(fds[1]);
    *errfdp = fds[0];
    return pid;
}

[[noreturn]]
static void exit_like_child(int pid) {
    int status;
    int rc;
    do {
        rc = waitpid(pid, &status, 0);
    } while (rc < 0 && errno == EINTR);
    if (rc < 0)
        die_errno("waitpid failed");

    if (WIFEXITED(status))
        exit(WEXITSTATUS(status));
    if (WIFSIGNALED(status))
        exit(128 + WTERMSIG(status));
    die("unexpected subprocess status");
}

static void die_usage(const char* arg0) {
    int len = strlen(arg0);
    fprintf(stderr, "usage: %*s [--off|--dumb|--on] <input-pipe>\n", len, arg0);
    fprintf(stderr, "       %*s <child-process> [<args>]...\n", len, "");
    exit(1);
}

int main(int argc, char** argv) {
    if (argc < 4)
        die_usage(argv[0]);

    // Open input pipe for read/write to prevent waiting for writer.
    // See https://unix.stackexchange.com/a/496812
    int infd = open(argv[2], O_RDWR);
    if (infd < 0)
        die_errno("cannot open input pipe %s", argv[2]);
    fcntl(infd, F_SETFD, FD_CLOEXEC);

    if (!strcmp(argv[1], "--off")) {
        // Do not filter, just exec the child (with the input pipe set).
        dup2(infd, STDIN_FILENO);
        execvp(argv[3], &argv[3]);
        die_errno("execvp failed");
    } else if (!strcmp(argv[1], "--dumb")) {
        enable_status = 0;
    } else if (!strcmp(argv[1], "--on")) {
        enable_status = 1;
    } else {
        die_usage(argv[0]);
    }

    int child_errfd;
    int child_pid = spawn(&argv[3], infd, &child_errfd);
    close(infd);

    FILE* child_stderr = fdopen(child_errfd, "r");

    struct sigaction act = {
        .sa_handler = update_columns,
        // Otherwise getline() is interrupted and not cleanly recoverable.
        .sa_flags = SA_RESTART,
    };
    sigaction(SIGWINCH, &act, NULL);
    update_columns(SIGWINCH);

    struct progress progress = { 0 };

    char* line = NULL;
    size_t size = 0;
    ssize_t nread;
    while (errno = 0, (nread = getline(&line, &size, child_stderr)) > 0) {
        // Pass through incomplete lines or not starting with !!
        if (line[nread - 1] != '\n' || line[0] != '!' || line[1] != '!') {
            maybe_clear_line(0);
            fputs(line, stderr);
            continue;
        }
        line[nread - 1] = '\0';

        if (!strncmp(line, "!!cache-hit\t", 12)) {
            progress.num_cached++;
        } else if (!strncmp(line, "!!step\t", 7)) {
            struct line_step parsed;
            if (parse_line_step(line, &parsed) < 0)
                continue;
            progress_handle_step(&progress, &parsed);

            char buf[columns + 1];
            format_status(&progress, buf, sizeof(buf));
            hardstatus(buf);
        }
    }
    if (nread < 0 && errno > 0)
        die_errno("cannot read stdin");

    if (progress.num_final > 0)
        print_summary(&progress);

    fclose(child_stderr);
    // leak line and sessions
    exit_like_child(child_pid);
}
