// Coordinate and dispatch jobs across sessions.
//
// All jobs are initially dispatched through knit-dispatch-job, which handles
// them in one of two ways:
// - Flow jobs set up sessions. For each session, we establish read/write pipes
//   with knit-resume-session to receive job scheduling requests and respond
//   with their productions. When the session is complete we wrap its invocation
//   in a production.
// - Simple jobs are executed and return their productions. Each outstanding
//   job, or dispatch, tracks which flow jobs have requested it. Upon
//   completion, we notify corresponding sessions of the resulting production.

#include "cache.h"
#include "hash.h"
#include "job.h"
#include "production.h"
#include "spec.h"
#include "util.h"

#include <poll.h>
#include <signal.h>

#define MAX_DISPATCHES 1024
#define MAX_DISPATCHES_PER_SESSION 8

static char sched_lockfile[PATH_MAX];

static void unlock_sched() { unlink(sched_lockfile); }

// TODO drain dispatches instead of abandoning them
static void sighandler(int signo) {
    exit(128 + signo);  // will run atexit handlers
}

// When a job completes with a production, we notify any flow job sessions that
// requested it. These sessions are stored on the notify_list.
struct notify_list {
    struct dispatch_session* session;
    struct notify_list* next;
};

// Index the notify_list by job so we can easily add sessions to be notified.
struct job_extra {
    struct notify_list* notify;
    struct production* prd;
    unsigned requested : 1;
};

struct job_extra* get_job_extra(struct job* job) {
    if (!job->object.extra) {
        job->object.extra = malloc(sizeof(struct job_extra));
        memset(job->object.extra, 0, sizeof(struct job_extra));
    }
    return job->object.extra;
}

enum dispatch_state {
    DS_INITIAL = 0,
    DS_RUNNING,
    DS_CACHE,
    DS_PENDING_SESSION,
    DS_SESSION,
    DS_LAMEDUCK,
    DS_DONE,
};

// State for dispatching flow jobs and their sessions. We could simply inline
// this into struct dispatch, but separating it seems clearer.
struct dispatch_session {
    struct pollfd* pfd;  // for throttling reads
    int writefd;
    int num_outstanding;
};

// Parallels a pollfd with the state of an outstanding job.
struct dispatch {
    enum dispatch_state state;
    pid_t pid;
    struct job* job;
    struct dispatch_session* session;  // for DS_SESSION
};

struct read_buffer {
    struct dispatch* dispatch;  // NULL iff on stdin
    size_t size;
    char buf[KNIT_HASH_HEXSZ + 1];
};

void free_dispatch(struct dispatch* d) {
    assert(!get_job_extra(d->job)->notify);
    free(d);
}

static int dispatch_is_dead(struct dispatch* dispatch) {
    return dispatch->state == DS_DONE;
}

static void pipe_cloexec(int fd[2]) {
    if (pipe(fd) < 0)
        die_errno("pipe failed");
    fcntl(fd[0], F_SETFD, FD_CLOEXEC);
    fcntl(fd[1], F_SETFD, FD_CLOEXEC);
}

// Spawn a child process with the command in argv. Respectively, pipe
// stdin/stdout, and store its write/read end in *writefd/*readfd; writefd is
// optional if NULL, but we always expect non-NULL readfd. Return the child pid.
static int spawn(char** argv, int* readfd, int* writefd) {
    assert(readfd);
    int outfd[2];
    int infd[2];
    pipe_cloexec(outfd);
    if (writefd)
        pipe_cloexec(infd);

    pid_t pid = fork();
    if (pid < 0)
        die_errno("fork failed");
    if (!pid) {
        dup2(outfd[1], STDOUT_FILENO);
        if (writefd)
            dup2(infd[0], STDIN_FILENO);
        else
            close(STDIN_FILENO);
        execvp(argv[0], argv);
        die_errno("execvp failed");
    }
    close(outfd[1]);
    if (writefd) {
        close(infd[0]);
        *writefd = infd[1];
    }
    *readfd = outfd[0];
    return pid;
}

static int write_production_to_session(const struct dispatch_session* session,
                                       const struct production* prd) {
    if (dprintf(session->writefd, "%s\n", oid_to_hex(&prd->object.oid)) < 0)
        return error_errno("cannot notify session of completion");
    return 0;
}

static int complete_job(struct job* job, struct production* prd) {
    struct notify_list** list_p = &get_job_extra(job)->notify;

    if (parse_production(prd) < 0)
        return -1;
    assert(prd->job == job);

    assert(!get_job_extra(job)->prd);
    get_job_extra(job)->prd = prd;

    int ret = 0;
    while (*list_p) {
        struct dispatch_session* session = (*list_p)->session;
        if (write_production_to_session(session, prd) < 0)
            ret = -1;

        if (session->num_outstanding-- == MAX_DISPATCHES_PER_SESSION) {
            assert(session->pfd->fd < 0);
            session->pfd->fd = ~session->pfd->fd;  // enable pollfd
        }

        struct notify_list* tmp = (*list_p)->next;
        free(*list_p);
        *list_p = tmp;
    }
    return ret;
}

static void notify_when_complete(struct dispatch_session* session,
                                 struct job* job) {
    struct notify_list* list = xmalloc(sizeof(*list));
    list->session = session;
    list->next = get_job_extra(job)->notify;
    get_job_extra(job)->notify = list;
    session->num_outstanding++;
}

static void start_dispatch(struct dispatch* dispatch, int* fd) {
    assert(dispatch->state == DS_INITIAL);
    dispatch->state = DS_CACHE;

    *fd = open_cache_file(dispatch->job);
    if (*fd < 0) {
        dispatch->state = DS_RUNNING;

        // Copy from job_process_name() for const correctness.
        char process[20];
        if (!memccpy(process, job_process_name(dispatch->job->process),
                     '\0', sizeof(process)))
            die("process name overflow");

        char* argv[] = {
            "knit-dispatch-job", process,
            oid_to_hex(&dispatch->job->object.oid), NULL
        };
        dispatch->pid = spawn(argv, fd, NULL);
    }
}

static struct production* get_production_hex(const char* hex) {
    struct object_id oid;
    if (strlen(hex) != KNIT_HASH_HEXSZ || hex_to_oid(hex, &oid) < 0) {
        error("invalid production hash");
        return NULL;
    }
    struct production* prd = get_production(&oid);
    return prd;
}

// Process a line of output from a dispatch process. Sets *req_job to request a
// job to be scheduled.
static int dispatch_line(struct dispatch* dispatch, const char* line,
                         struct job** req_job) {
    if (dispatch->state == DS_RUNNING) {
        if (!strcmp(line, "session")) {
            dispatch->state = DS_PENDING_SESSION;
        } else {
            struct production* prd = get_production_hex(line);
            if (!prd ||
                    write_cache(dispatch->job, prd) < 0 ||
                    complete_job(dispatch->job, prd) < 0)
                return -1;
            dispatch->state = DS_LAMEDUCK;
        }
    } else if (dispatch->state == DS_CACHE) {
        fprintf(stderr, "!!cache-hit\t%s\t%s\n",
                oid_to_hex(&dispatch->job->object.oid), line);
        struct production* prd = get_production_hex(line);
        if (!prd || complete_job(dispatch->job, prd) < 0)
            return -1;
        dispatch->state = DS_LAMEDUCK;
    } else if (dispatch->state == DS_SESSION) {
        struct object_id oid;
        if (strlen(line) != KNIT_HASH_HEXSZ || hex_to_oid(line, &oid) < 0)
            return error("invalid job hash");
        *req_job = get_job(&oid);
    } else {
        return error("unexpected output");
    }
    return 0;
}

static int stdin_line(const char* line) {
    struct production* prd = get_production_hex(line);
    if (!prd || parse_production(prd) < 0 || parse_job(prd->job) < 0)
        return -1;

    if (!get_job_extra(prd->job)->requested)
        return error("job %s not requested", oid_to_hex(&prd->job->object.oid));
    if (prd->job->process != JOB_PROCESS_EXTERNAL)
        return error("job %s not external", oid_to_hex(&prd->job->object.oid));
    if (get_job_extra(prd->job)->prd)
        return error("job %s already has production", oid_to_hex(&prd->job->object.oid));

    return complete_job(prd->job, prd);
}

// Returns -1 on error, or the number of bytes processed. Notably, returns 0 if
// the buffer has no complete lines.
static ssize_t handle_read(struct read_buffer* readbuf, struct job** req_job) {
    char* nl = memchr(readbuf->buf, '\n', readbuf->size);
    if (!nl) {
        if (readbuf->size >= sizeof(readbuf->buf))
            return error("subprocess output line too long");
        return 0;
    }
    *nl = '\0';

    if (readbuf->dispatch) {
        if (dispatch_line(readbuf->dispatch, readbuf->buf, req_job) < 0)
            return -1;
    } else {
        if (stdin_line(readbuf->buf) < 0)
            return -1;
    }

    size_t off = nl + 1 - readbuf->buf;
    memmove(readbuf->buf, readbuf->buf + off, readbuf->size - off);
    readbuf->size -= off;
    return off;
}

static void cleanup_dispatch_session(struct dispatch_session** session) {
    close((*session)->writefd);
    free(*session);
    *session = NULL;
}

static void cleanup_pollfd(struct pollfd* pfd) {
    close(pfd->fd);
    pfd->fd = -1;
}

static int cleanup_subprocess(struct dispatch* dispatch) {
    if (!dispatch->pid)  // no process (read from cache file)
        return 0;

    int status;
    int rc;
    do {
        rc = waitpid(dispatch->pid, &status, 0);
    } while (rc < 0 && errno == EINTR);
    if (rc < 0)
        die_errno("waitpid failed");
    dispatch->pid = 0;

    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (code)
            return error("process died with error code %d", code);
    } else if (WIFSIGNALED(status)) {
        return error("process died of signal %d", WTERMSIG(status));
    }
    return 0;
}

static int handle_eof(struct dispatch* dispatch, struct pollfd* pfd) {
    if (dispatch->session)
        cleanup_dispatch_session(&dispatch->session);
    cleanup_pollfd(pfd);
    if (cleanup_subprocess(dispatch) < 0)
        return -1;

    if (dispatch->state == DS_PENDING_SESSION) {
        char* argv[] = {
            "knit-resume-session", oid_to_hex(&dispatch->job->object.oid), NULL
        };

        dispatch->session = malloc(sizeof(*dispatch->session));
        memset(dispatch->session, 0, sizeof(*dispatch->session));
        dispatch->session->pfd = pfd;
        dispatch->pid = spawn(argv, &pfd->fd, &dispatch->session->writefd);
        dispatch->state = DS_SESSION;
    } else if (dispatch->state == DS_SESSION) {
        // Somewhat awkward script to wrap invocation in a production.
        // TODO this lacks error handling on knit-close-session
        char script[1024];
        snprintf(script, sizeof(script),
                 "knit-remix-production --set-job %1$s"
                 " --wrap-invocation `knit-close-session %1$s`",
                 oid_to_hex(&dispatch->job->object.oid));
        char* argv[] = { "sh", "-c", script, NULL };
        dispatch->pid = spawn(argv, &pfd->fd, NULL);
        dispatch->state = DS_RUNNING;
    } else if (dispatch->state == DS_LAMEDUCK) {
        assert(!get_job_extra(dispatch->job)->notify);
        dispatch->state = DS_DONE;
    } else {
        return error("unexpected eof");
    }
    return 0;
}

static int pollfd_ready(struct pollfd* pfd, struct read_buffer* readbuf) {
    if (!pfd->revents)
        return 0;
    assert(!(pfd->revents & POLLNVAL));

    // On EOF we observe POLLHUP without POLLIN.
    if (pfd->revents & (POLLIN | POLLHUP)) {
        ssize_t nr = xread(pfd->fd, readbuf->buf + readbuf->size,
                           sizeof(readbuf->buf) - readbuf->size);
        if (nr < 0)
            die_errno("read failed");
        readbuf->size += nr;
        return 1;
    } else {
        die("unhandled poll revent");
    }
}

static void setup_stdin_pollfd(struct pollfd* pfds,
                               struct read_buffer** readbufs,
                               nfds_t* nfds) {
    struct read_buffer* readbuf = malloc(sizeof(*readbuf));
    readbuf->dispatch = NULL;
    readbuf->size = 0;

    readbufs[*nfds] = readbuf;
    pfds[*nfds].fd = STDIN_FILENO;
    pfds[*nfds].events = POLLIN;
    (*nfds)++;
}

static void setup_dispatch(struct job* job,
                           struct pollfd* pfds,
                           struct read_buffer** readbufs,
                           nfds_t* nfds) {
    assert(!get_job_extra(job)->prd);
    if (get_job_extra(job)->requested)
        return;
    get_job_extra(job)->requested = 1;

    if (job->process == JOB_PROCESS_EXTERNAL) {
        printf("external %s\n", oid_to_hex(&job->object.oid));
        return;
    }

    if (*nfds + 1 >= MAX_DISPATCHES)
        die("too many dispatches");

    struct dispatch* dispatch = malloc(sizeof(*dispatch));
    memset(dispatch, 0, sizeof(*dispatch));
    dispatch->job = job;

    struct read_buffer* readbuf = malloc(sizeof(*readbuf));
    readbuf->dispatch = dispatch;
    readbuf->size = 0;

    readbufs[*nfds] = readbuf;
    pfds[*nfds].events = POLLIN;
    pfds[*nfds].revents = 0;
    start_dispatch(dispatch, &pfds[*nfds].fd);
    (*nfds)++;
}

static int setup_dispatch_and_notify(struct job* job,
                                     struct dispatch_session* session,
                                     struct pollfd* pfds,
                                     struct read_buffer** readbufs,
                                     nfds_t* nfds) {
    if (parse_job(job) < 0)
        return -1;

    const struct production* prd = get_job_extra(job)->prd;
    if (prd)
        return write_production_to_session(session, prd);

    setup_dispatch(job, pfds, readbufs, nfds);
    notify_when_complete(session, job);
    return 0;
}

static void maybe_throttle_pollfd(struct dispatch* dispatch,
                                  struct pollfd* pfd) {
    if (dispatch->session &&
            dispatch->session->num_outstanding >= MAX_DISPATCHES_PER_SESSION) {
        assert(pfd->fd >= 0);
        pfd->fd = ~pfd->fd;  // disable pollfd
    }
}

static void die_usage(char* arg0) {
    fprintf(stderr, "usage: %s <job>\n", arg0);
    exit(1);
}

int main(int argc, char** argv) {
    if (argc != 2)
        die_usage(argv[0]);

    struct job* root_job = peel_job(argv[1]);
    if (!root_job || parse_job(root_job) < 0)
        exit(1);

    // To avoid races only one instance may be running for any knit directory.
    if (snprintf(sched_lockfile, PATH_MAX,
                 "%s/scheduler.lock", get_knit_dir()) >= PATH_MAX)
        die("lock path too long");
    if (acquire_lockfile(sched_lockfile) < 0)  // leaks returned fd
        exit(1);
    atexit(unlock_sched);

    struct sigaction act = { .sa_handler = sighandler };
    sigaction(SIGINT, &act, NULL);
    sigaction(SIGTERM, &act, NULL);

    struct pollfd pfds[MAX_DISPATCHES];
    struct read_buffer* readbufs[MAX_DISPATCHES];
    nfds_t nfds = 0;

    setlinebuf(stdout);
    setup_stdin_pollfd(pfds, readbufs, &nfds);
    setup_dispatch(root_job, pfds, readbufs, &nfds);

    nfds_t deletions[MAX_DISPATCHES];
    size_t ndel = 0;

    // STDIN is always polled; run until all other child processes complete.
    while (nfds > 1) {
        if (poll(pfds, nfds, -1) < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            die_errno("poll failed");
        }

        for (nfds_t i = 0; i < nfds; i++) {
            if (!pollfd_ready(&pfds[i], readbufs[i]))
                continue;

            if (readbufs[i]->size > 0) {
                // Process lines until we run out.
                int nr;
                do {
                    struct job* req_job = NULL;
                    nr = handle_read(readbufs[i], &req_job);
                    if (nr < 0)
                        exit(1);

                    if (req_job) {
                        assert(readbufs[i]->dispatch);
                        assert(readbufs[i]->dispatch->session);
                        if (setup_dispatch_and_notify(req_job,
                                                      readbufs[i]->dispatch->session,
                                                      pfds, readbufs,
                                                      &nfds) < 0)
                            exit(1);
                    }
                } while (nr > 0);
                // Since we throttle after draining the subprocess buffer, it is
                // possible in theory to exceed MAX_DISPATCHES_PER_SESSION. In
                // practice this is mitigated by the size of our read buffer.
                if (readbufs[i]->dispatch)
                    maybe_throttle_pollfd(readbufs[i]->dispatch, &pfds[i]);
            } else {
                if (readbufs[i]->size > 0)
                    die("unterminated line");

                // If our fd is ready with no data then we've reached EOF.
                if (readbufs[i]->dispatch &&
                        handle_eof(readbufs[i]->dispatch, &pfds[i]) < 0)
                    exit(1);
            }

            if (readbufs[i]->dispatch &&
                    dispatch_is_dead(readbufs[i]->dispatch)) {
                assert(ndel < MAX_DISPATCHES);
                deletions[ndel++] = i;
            }
        }

        // Defer deletion until after iteration.
        for (; ndel > 0; ndel--) {
            size_t i = deletions[ndel - 1];
            assert(readbufs[i]->dispatch);
            free_dispatch(readbufs[i]->dispatch);
            free(readbufs[i]);
            nfds--;
            if (i < nfds) {
                memcpy(&pfds[i], &pfds[nfds], sizeof(*pfds));
                readbufs[i] = readbufs[nfds];
                // Update any reference to the moved pollfd.
                if (readbufs[i]->dispatch->session) {
                    assert(readbufs[i]->dispatch->session->pfd == &pfds[nfds]);
                    readbufs[i]->dispatch->session->pfd = &pfds[i];
                }
            }
        }
    }

    const struct production* prd = get_job_extra(root_job)->prd;
    assert(prd);
    printf("ok %s\n", oid_to_hex(&prd->object.oid));
    return 0;
}
