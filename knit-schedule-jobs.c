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
    unsigned dispatched : 1;
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

static void start_dispatch(struct dispatch* dispatch, int* fd) {
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

static int write_hex_production_to_cache(const struct job* job,
                                         const char* prd_hex) {
    struct object_id oid;
    if (strlen(prd_hex) != KNIT_HASH_HEXSZ || hex_to_oid(prd_hex, &oid) < 0)
        return error("invalid production hash");
    struct production* prd = get_production(&oid);
    return write_cache(job, prd);
}

static void notify_when_complete(struct dispatch_session* session,
                                 struct job* job) {
    struct notify_list* list = xmalloc(sizeof(*list));
    list->session = session;
    list->next = get_job_extra(job)->notify;
    get_job_extra(job)->notify = list;
    session->num_outstanding++;
}

static int write_production_to_session(const struct dispatch_session* session,
                                       const char* prd_hex) {
    if (dprintf(session->writefd, "%s\n", prd_hex) < 0)
        return error_errno("cannot notify session of completion");
    return 0;
}

static int notify_completion(struct dispatch* dispatch, const char* prd_hex) {
    struct notify_list** list_p = &get_job_extra(dispatch->job)->notify;

    // If no one is waiting for this dispatch, assume it is the root job
    // and emit its production.
    if (!*list_p)
        puts(prd_hex);

    int ret = 0;
    while (*list_p) {
        struct dispatch_session* session = (*list_p)->session;
        if (write_production_to_session(session, prd_hex) < 0)
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

// Process a line of output from a dispatch process. Sets *req_job to request a
// job to be scheduled. Returns -1 on error, or the number of bytes processed
// from dispatch->buf. Notably, returns 0 if the buffer has no complete lines.
static ssize_t handle_output(struct dispatch* dispatch, struct job** req_job) {
    char* nl = memchr(dispatch->buf, '\n', dispatch->size);
    if (!nl) {
        if (dispatch->size >= sizeof(dispatch->buf))
            return error("subprocess output line too long");
        return 0;
    }
    *nl = '\0';
    const char* line = dispatch->buf;

    if (dispatch->state == DS_RUNNING) {
        if (!strcmp(line, "session")) {
            dispatch->state = DS_PENDING_SESSION;
        } else {
            if (write_hex_production_to_cache(dispatch->job, line) < 0)
                return -1;
            dispatch->state = DS_LAMEDUCK;
            if (notify_completion(dispatch, line) < 0)
                return -1;
        }
    } else if (dispatch->state == DS_CACHE) {
        fprintf(stderr, "!!cache-hit\t%s\t%s\n",
                oid_to_hex(&dispatch->job->object.oid), line);
        dispatch->state = DS_LAMEDUCK;
        if (notify_completion(dispatch, line) < 0)
            return -1;
    } else if (dispatch->state == DS_SESSION) {
        struct object_id oid;
        if (strlen(line) != KNIT_HASH_HEXSZ || hex_to_oid(line, &oid) < 0)
            return error("invalid job hash");
        *req_job = get_job(&oid);
    } else {
        return error("unexpected output");
    }

    size_t off = nl + 1 - dispatch->buf;
    memmove(dispatch->buf, dispatch->buf + off, dispatch->size - off);
    dispatch->size -= off;
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

    if (dispatch->size > 0)
        return error("unterminated line");

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
        if (get_job_extra(dispatch->job)->notify) {
            // We happened to receive requests for this dispatch after sending
            // notifications. Instead of deallocating our dispatch, run it again
            // with remaining notifications.
            start_dispatch(dispatch, &pfd->fd);
        } else {
            dispatch->state = DS_DONE;
        }
    } else {
        return error("unexpected eof");
    }
    return 0;
}

static int pollfd_ready(struct pollfd* pfd, struct dispatch* dispatch) {
    if (!pfd->revents)
        return 0;
    assert(!(pfd->revents & POLLNVAL));

    // On EOF we observe POLLHUP without POLLIN.
    if (pfd->revents & (POLLIN | POLLHUP)) {
        ssize_t nr = xread(pfd->fd, dispatch->buf + dispatch->size,
                           sizeof(dispatch->buf) - dispatch->size);
        if (nr < 0)
            die_errno("read failed");
        dispatch->size += nr;
        return 1;
    } else {
        die("unhandled poll revent");
    }
}

static void setup_dispatch_and_pollfd(struct job* job, struct pollfd* pfds,
                                      struct dispatch** dispatches, nfds_t* nfds) {
    if (get_job_extra(job)->dispatched)
        return;
    get_job_extra(job)->dispatched = 1;

    if (*nfds + 1 >= MAX_DISPATCHES)
        die("too many dispatches");

    struct dispatch* dispatch = malloc(sizeof(*dispatch));
    memset(dispatch, 0, sizeof(*dispatch));
    dispatch->job = job;

    dispatches[*nfds] = dispatch;
    pfds[*nfds].events = POLLIN;
    pfds[*nfds].revents = 0;
    start_dispatch(dispatch, &pfds[*nfds].fd);
    (*nfds)++;
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
    struct dispatch* dispatches[MAX_DISPATCHES];
    nfds_t nfds = 0;
    setup_dispatch_and_pollfd(root_job, pfds, dispatches, &nfds);

    nfds_t deletions[MAX_DISPATCHES];
    size_t ndel = 0;

    while (nfds > 0) {
        if (poll(pfds, nfds, -1) < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            die_errno("poll failed");
        }

        for (nfds_t i = 0; i < nfds; i++) {
            if (!pollfd_ready(&pfds[i], dispatches[i]))
                continue;

            if (dispatches[i]->size > 0) {
                // Process lines until we run out.
                int nr;
                do {
                    struct job* req_job = NULL;
                    nr = handle_output(dispatches[i], &req_job);
                    if (nr < 0)
                        exit(1);

                    if (req_job) {
                        if (parse_job(req_job) < 0)
                            exit(1);
                        // Dispatch req_job if needed and notify the requesting
                        // session when complete.
                        setup_dispatch_and_pollfd(req_job, pfds,
                                                  dispatches, &nfds);
                        assert(dispatches[i]->session);
                        notify_when_complete(dispatches[i]->session, req_job);
                    }
                } while (nr > 0);
                // Since we throttle after draining the subprocess buffer, it is
                // possible in theory to exceed MAX_DISPATCHES_PER_SESSION. In
                // practice this is mitigated by the size of our read buffer.
                maybe_throttle_pollfd(dispatches[i], &pfds[i]);
            } else {
                // If our fd is ready with no data then we've reached EOF.
                if (handle_eof(dispatches[i], &pfds[i]) < 0)
                    exit(1);
            }

            if (dispatch_is_dead(dispatches[i])) {
                assert(ndel < MAX_DISPATCHES);
                deletions[ndel++] = i;
            }
        }

        // Defer deletion until after iteration.
        for (; ndel > 0; ndel--) {
            size_t i = deletions[ndel - 1];
            free_dispatch(dispatches[i]);
            nfds--;
            if (i < nfds) {
                memcpy(&pfds[i], &pfds[nfds], sizeof(*pfds));
                dispatches[i] = dispatches[nfds];
                // Update any reference to the moved pollfd.
                if (dispatches[i]->session) {
                    assert(dispatches[i]->session->pfd == &pfds[nfds]);
                    dispatches[i]->session->pfd = &pfds[i];
                }
            }
        }
    }

    return 0;
}
