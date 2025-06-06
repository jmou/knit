#include "hash.h"
#include "util.h"

#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>

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

static int hide_filtered;
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

struct linebuf {
    size_t off;
    size_t size;
    char buf[1024];
};

static void linebuf_init(struct linebuf* lb) {
    lb->off = lb->size = 0;
}

// Read from *fd into linebuf. Returns whether EOF; if so, cleans up *fd.
static int linebuf_read_is_eof(struct linebuf* lb, int* fd) {
    assert(lb->size < sizeof(lb->buf));
    ssize_t nr = xread(*fd, lb->buf + lb->size, sizeof(lb->buf) - lb->size);
    if (nr <= 0) {
        if (nr < 0)
            warning_errno("read failed");
        if (lb->off < lb->size)
            warning("unexpected EOF");
        return 1;
    }
    if (nr > 0) {
        if (memchr(lb->buf + lb->size, '\0', nr))
            warning("illegal NUL byte; line will be truncated");
        lb->size += nr;
    }
    return 0;
}

static char* linebuf_getline(struct linebuf* lb) {
    char* start = lb->buf + lb->off;
    char* nl = memchr(start, '\n', lb->size - lb->off);
    if (!nl) {
        if (lb->off == 0 && lb->size == sizeof(lb->buf))
            die("line too long");

        // Shift remaining to the start of the buffer.
        memmove(lb->buf, lb->buf + lb->off, lb->size - lb->off);
        lb->size -= lb->off;
        lb->off = 0;
        return NULL;
    }

    *nl++ = '\0';
    lb->off = nl - lb->buf;
    assert(lb->off <= lb->size);
    return start;
}

// Returns -1 if the fd should be closed, or 0 to remain open.
static int child_ready(struct pollfd* pfd, struct linebuf* lbuf,
                       struct progress* progress) {
    if (linebuf_read_is_eof(lbuf, &pfd->fd))
        return -1;

    char* line;
    while ((line = linebuf_getline(lbuf))) {
        // Pass through lines not starting with !!
        if (line[0] != '!' || line[1] != '!') {
            maybe_clear_line(0);
            fprintf(stderr, "%s\n", line);
            continue;
        }

        if (!hide_filtered)
            fprintf(stderr, "%s\n", line);

        if (!strncmp(line, "!!cache-hit\t", 12)) {
            progress->num_cached++;
        } else if (!strncmp(line, "!!step\t", 7)) {
            struct line_step parsed;
            if (parse_line_step(line, &parsed) < 0) {
                warning("could not parse !!step");
                return 0;
            }
            progress_handle_step(progress, &parsed);

            char buf[columns + 1];
            format_status(progress, buf, sizeof(buf));
            hardstatus(buf);
        }
    }
    return 0;
}

static int write_line(int fd, const char* s) {
    size_t size = strlen(s);
    char buf[size + 1];
    memcpy(buf, s, size);
    buf[size++] = '\n';

    size_t off = 0;
    while (off < size) {
        int n = xwrite(fd, buf + off, size - off);
        if (n < 0)
            return -1;
        off += n;
    }
    return 0;
}

// Returns -1 if the fd should be closed, or 0 to remain open.
static int client_ready(struct pollfd* pfd, struct linebuf* lbuf,
                        int child_infd) {
    if (linebuf_read_is_eof(lbuf, &pfd->fd))
        return -1;

    char* line;
    while ((line = linebuf_getline(lbuf))) {
        if (!strncmp(line, "production ", 11)) {
            line += 11;
            struct object_id oid;
            if (strlen(line) != KNIT_HASH_HEXSZ || hex_to_oid(line, &oid) < 0) {
                warning("invalid production hash");
                continue;
            }
            if (write_line(child_infd, oid_to_hex(&oid)) < 0)
                die_errno("write failed");
        } else {
            warning("unknown command");
        }
    }
    return 0;
}

static int poll_enabled(struct pollfd* pfd) {
    return pfd->fd >= 0;
}

static int poll_any(struct pollfd* pfds, nfds_t nfds) {
    for (nfds_t i = 0; i < nfds; i++) {
        if (poll_enabled(&pfds[i]))
            return 1;
    }
    return 0;
}

// Returns nfds if all pfds are in use.
static nfds_t poll_next_available(struct pollfd* pfds, nfds_t nfds) {
    for (nfds_t i = 0; i < nfds; i++) {
        if (!poll_enabled(&pfds[i]))
            return i;
    }
    return nfds;
}

static int poll_full(struct pollfd* pfds, nfds_t nfds) {
    return poll_next_available(pfds, nfds) == nfds;
}

static int create_run_socket() {
    int sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sockfd < 0)
        die_errno("socket failed");

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (snprintf(addr.sun_path, sizeof(addr.sun_path) - 1,
                 "%s/run.sock", get_knit_dir()) >= (int)sizeof(addr.sun_path))
        die("run socket path too long");

    unlink(addr.sun_path);
    if (bind(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
        die_errno("bind failed");
    if (listen(sockfd, 4) < 0)
        die_errno("listen failed");

    return sockfd;
}

static int spawn(char** argv, int* infdp, int* errfdp) {
    int fds[2][2];
    for (size_t i = 0; i < 2; i++) {
        if (pipe(fds[i]) < 0)
            die_errno("pipe failed");
        fcntl(fds[i][0], F_SETFD, FD_CLOEXEC);
        fcntl(fds[i][1], F_SETFD, FD_CLOEXEC);
    }

    pid_t pid = fork();
    if (pid < 0)
        die_errno("fork failed");
    if (!pid) {
        dup2(fds[0][0], STDIN_FILENO);
        dup2(fds[1][1], STDERR_FILENO);
        execvp(argv[0], argv);
        die_errno("execvp failed");
    }

    close(fds[0][0]);
    close(fds[1][1]);
    *infdp = fds[0][1];
    *errfdp = fds[1][0];
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
    fprintf(stderr, "usage: %*s [--off|--dumb|--on]\n", len, arg0);
    fprintf(stderr, "       %*s <child-process> [<args>]...\n", len, "");
    exit(1);
}

int main(int argc, char** argv) {
    if (argc < 4)
        die_usage(argv[0]);

    if (!strcmp(argv[1], "--off")) {
        hide_filtered = 0;
        enable_status = 0;
    } else if (!strcmp(argv[1], "--dumb")) {
        hide_filtered = 1;
        enable_status = 0;
    } else if (!strcmp(argv[1], "--on")) {
        hide_filtered = 1;
        enable_status = 1;
    } else {
        die_usage(argv[0]);
    }

    struct sigaction act = {
        .sa_handler = update_columns,
        // Otherwise getline() is interrupted and not cleanly recoverable.
        .sa_flags = SA_RESTART,
    };
    sigaction(SIGWINCH, &act, NULL);
    update_columns(SIGWINCH);

    enum {
        POLL_CLIENT_MAX = 7,
        POLL_CHILD,
        POLL_SOCKET,  // needs no linebuf; must immediately precede POLL_NUM_FDS
        POLL_NUM_FDS,
    };

    struct linebuf lbufs[POLL_SOCKET];
    struct pollfd pfds[POLL_NUM_FDS];
    for (size_t i = 0; i <= POLL_CLIENT_MAX; i++) {
        pfds[i].fd = -1;
        pfds[i].events = POLLIN;
    }

    pfds[POLL_SOCKET].fd = create_run_socket();
    pfds[POLL_SOCKET].events = POLLIN;

    int child_infd;
    int child_pid = spawn(&argv[2], &child_infd, &pfds[POLL_CHILD].fd);
    pfds[POLL_CHILD].events = POLLIN;
    linebuf_init(&lbufs[POLL_CHILD]);

    struct progress progress = { 0 };  // .sessions leaked on exit

    // Loop until all fds are idle, except for the listening socket.
    while (poll_any(pfds, POLL_SOCKET)) {
        if (poll(pfds, POLL_NUM_FDS, -1) < 0) {
            if (errno == EINTR)
                continue;
            die_errno("poll failed");
        }

        for (size_t i = 0; i < POLL_NUM_FDS; i++) {
            if (pfds[i].revents & (POLLIN | POLLHUP)) {
                if (i == POLL_CHILD) {
                    if (child_ready(&pfds[i], &lbufs[i], &progress) < 0) {
                        close(pfds[i].fd);
                        pfds[i].fd = -1;
                    }
                } else if (i == POLL_SOCKET) {
                    assert(!poll_full(pfds, POLL_CLIENT_MAX + 1));
                    int fd = accept(pfds[i].fd, NULL, NULL);
                    if (fd < 0) {
                        warning_errno("accept failed");
                        continue;
                    }
                    nfds_t c = poll_next_available(pfds, POLL_CLIENT_MAX + 1);
                    pfds[c].fd = fd;
                    linebuf_init(&lbufs[c]);
                    // Disable accepting new connections if full.
                    if (poll_full(pfds, POLL_CLIENT_MAX + 1)) {
                        assert(poll_enabled(&pfds[POLL_SOCKET]));
                        pfds[POLL_SOCKET].fd = ~pfds[POLL_SOCKET].fd;
                    }
                } else {
                    assert(i <= POLL_CLIENT_MAX);
                    if (client_ready(&pfds[i], &lbufs[i], child_infd) < 0) {
                        close(pfds[i].fd);
                        pfds[i].fd = -1;
                        // Enable accepting new connections if disabled.
                        if (!poll_enabled(&pfds[POLL_SOCKET]))
                            pfds[POLL_SOCKET].fd = ~pfds[POLL_SOCKET].fd;
                    }
                }
            }
        }
    }
    close(pfds[POLL_SOCKET].fd);

    if (progress.num_final > 0)
        print_summary(&progress);

    exit_like_child(child_pid);
}
