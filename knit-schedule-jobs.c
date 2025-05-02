// Coordinate and dispatch jobs across sessions.
//
// All jobs are initially dispatched through knit-dispatch-job, which handles
// them in one of two ways:
// - Simple jobs are executed and return their productions.
// - Flow jobs set up sessions. For each session, we establish a two-way pipe
//   with knit-resume-session to receive job scheduling requests and respond
//   with their productions. When the session is complete we wrap its invocation
//   in a production.
//
// Each outstanding job, or dispatch, maintains which sessions are waiting upon
// it. Upon completion, we notify these sessions of the resulting production.

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

enum dispatch_state {
    DS_INITIAL = 0,
    DS_RUNNING,
    DS_PENDING_SESSION,
    DS_SESSION,
    DS_LAMEDUCK,
    DS_DONE,
};

struct notify_list {
    // Each session is identified by its flow job.
    struct job* flow_job;
    struct notify_list* next;
};

// Parallels a pollfd that listens for process output. Stored in the job object
// extra field.
struct dispatch {
    enum dispatch_state state;
    pid_t pid;

    // For state == DS_SESSION.
    int writefd;
    int num_outstanding;
    int* pfdp;

    struct notify_list* notify;
    char buf[KNIT_HASH_HEXSZ + 20];
};

static void free_dispatch(struct dispatch* d) {
    if (d->notify) {
        error("abandoning dispatch with notifications");
        struct notify_list* s = d->notify;
        while (s) {
            struct notify_list* tmp = s->next;
            free(s);
            s = tmp;
        }
    }
    free(d);
}

static void dispatch_add_session(struct dispatch* dispatch,
                                 struct job* flow_job) {
    struct notify_list* list = xmalloc(sizeof(*list));
    list->flow_job = flow_job;
    list->next = dispatch->notify;
    dispatch->notify = list;
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

static int spawn(struct dispatch* dispatch, char** argv, int pipe_stdin) {
    assert(dispatch->pid == 0);
    assert(dispatch->writefd == -1);

    int infd[2];
    int outfd[2];
    pipe_cloexec(outfd);
    if (pipe_stdin)
        pipe_cloexec(infd);

    pid_t pid = fork();
    if (pid < 0)
        die_errno("fork failed");
    if (!pid) {
        dup2(outfd[1], STDOUT_FILENO);
        if (pipe_stdin)
            dup2(infd[0], STDIN_FILENO);
        else
            close(STDIN_FILENO);
        execvp(argv[0], argv);
        die_errno("execvp failed");
    }
    close(outfd[1]);
    if (pipe_stdin) {
        close(infd[0]);
        dispatch->writefd = infd[1];
    }
    dispatch->pid = pid;
    return outfd[0];
}

static void dispatch_job(struct job* job, int* fd) {
    struct dispatch* dispatch = job->object.extra;
    assert(dispatch);
    dispatch->state = DS_RUNNING;

    *fd = open_cache_file(job);
    if (*fd < 0) {
        // Copy from job_process_name() for const correctness.
        char process[20];
        if (!memccpy(process, job_process_name(job->process),
                     '\0', sizeof(process)))
            die("process name overflow");

        char* argv[] = {
            "knit-dispatch-job", process, oid_to_hex(&job->object.oid), NULL
        };
        *fd = spawn(dispatch, argv, 0);
    }
    // This violates encapsulation, but it lets us easily reactivate a pollfd
    // from its dispatch.
    dispatch->pfdp = fd;
}

static int handle_eof(struct job* job, int* fd) {
    struct dispatch* dispatch = job->object.extra;
    assert(dispatch);
    if (dispatch->state == DS_PENDING_SESSION) {
        char* argv[] = {
            "knit-resume-session", oid_to_hex(&job->object.oid), NULL
        };
        *fd = spawn(dispatch, argv, 1);
        dispatch->state = DS_SESSION;
    } else if (dispatch->state == DS_SESSION) {
        // Somewhat awkward script to wrap invocation in a production.
        char script[1024];
        snprintf(script, sizeof(script),
                 "knit-remix-production --set-job %1$s"
                 " --wrap-invocation `knit-close-session %1$s`",
                 oid_to_hex(&job->object.oid));
        char* argv[] = { "sh", "-c", script, NULL };
        *fd = spawn(dispatch, argv, 0);
        dispatch->state = DS_RUNNING;
    } else if (dispatch->state == DS_LAMEDUCK) {
        if (dispatch->notify) {
            // We happened to receive requests for this job after sending out
            // notifications. Instead of deallocating our dispatch, run it again
            // with remaining notifications.
            dispatch_job(job, fd);
        } else {
            dispatch->state = DS_DONE;
        }
    } else {
        return error("unexpected eof");
    }
    return 0;
}

static int write_production_to_session(const struct dispatch* dispatch,
                                       const char* prd_hex) {
    assert(dispatch->state == DS_SESSION);
    assert(dispatch->writefd >= 0);
    if (dprintf(dispatch->writefd, "%s\n", prd_hex) < 0)
        return error_errno("cannot notify session of completion");
    return 0;
}

static int notify_completion(const char* prd_hex, struct notify_list** list_p) {
    int ret = 0;
    while (*list_p) {
        struct dispatch* dispatch = (*list_p)->flow_job->object.extra;
        assert(dispatch);
        if (write_production_to_session(dispatch, prd_hex) < 0)
            ret = -1;

        if (dispatch->num_outstanding-- == MAX_DISPATCHES_PER_SESSION) {
            assert(*dispatch->pfdp < 0);
            *dispatch->pfdp = ~*dispatch->pfdp;  // enable pollfd
        }

        struct notify_list* tmp = (*list_p)->next;
        free(*list_p);
        *list_p = tmp;
    }
    return ret;
}

// Process a line of output from a dispatch process. Sets *req_job if it should
// be scheduled for dispatch.
static int handle_output(struct job* job, const char* line,
                         struct job** req_job) {
    struct dispatch* dispatch = job->object.extra;
    assert(dispatch);
    if (dispatch->state == DS_RUNNING) {
        if (!strcmp(line, "session")) {
            dispatch->state = DS_PENDING_SESSION;
        } else {
            // If we were running a process (knit-dispatch-job) then we should
            // cache the result. Otherwise the result itself was from the cache.
            if (dispatch->pid) {
                struct object_id oid;
                if (strlen(line) != KNIT_HASH_HEXSZ || hex_to_oid(line, &oid) < 0)
                    return error("invalid production hash");
                struct production* prd = get_production(&oid);
                if (write_cache(job, prd) < 0)
                    return -1;
            } else {
                fprintf(stderr, "!!cache-hit\t%s\t%s\n",
                        oid_to_hex(&job->object.oid), line);
            }

            dispatch->state = DS_LAMEDUCK;
            // If no one is waiting for this dispatch, assume it is the root job
            // and emit its production.
            if (!dispatch->notify)
                puts(line);
            return notify_completion(line, &dispatch->notify);
        }
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

static void create_dispatch_and_pollfd(struct job* job, struct pollfd* pfds,
                                       struct job** jobs, nfds_t* nfds) {
    assert(!job->object.extra);
    if (*nfds + 1 >= MAX_DISPATCHES)
        die("too many dispatches");

    struct dispatch* dispatch = xmalloc(sizeof(*dispatch));
    memset(dispatch, 0, sizeof(*dispatch));
    dispatch->writefd = -1;
    job->object.extra = dispatch;

    dispatch_job(job, &pfds[*nfds].fd);
    pfds[*nfds].events = POLLIN;
    jobs[*nfds] = job;
    (*nfds)++;
}

static int cleanup_dispatch_process(struct dispatch* dispatch,
                                    struct pollfd* pfd) {
    close(pfd->fd);
    pfd->fd = -1;
    if (dispatch->writefd >= 0) {
        close(dispatch->writefd);
        dispatch->writefd = -1;
    }

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

// Main poll loop iteration. Returns -1 on error, 1 to process the next index,
// or 0 to reprocess the current index.
static int poll_tick(struct pollfd* pfds, struct job** jobs,
                     nfds_t* nfds, size_t i) {
    struct dispatch* dispatch = jobs[i]->object.extra;
    assert(dispatch);
    size_t size = strlen(dispatch->buf);
    ssize_t nr = read(pfds[i].fd, dispatch->buf + size,
                      sizeof(dispatch->buf) - size);
    if (nr < 0) {
        if (errno == EINTR || errno == EAGAIN)
            return 0;
        return error_errno("read failed");
    } else if (nr == 0) {
        if (cleanup_dispatch_process(dispatch, &pfds[i]) < 0)
            return -1;

        if (*dispatch->buf)
            return error("unterminated line");

        if (handle_eof(jobs[i], &pfds[i].fd) < 0)
            return -1;
        if (dispatch_is_dead(dispatch)) {
            free_dispatch(dispatch);
            jobs[i]->object.extra = NULL;
            (*nfds)--;
            if (i < *nfds) {
                memcpy(&pfds[i], &pfds[*nfds], sizeof(*pfds));
                jobs[i] = jobs[*nfds];
            }
            return 0;
        }
        return 1;
    }

    if (memchr(dispatch->buf + size, '\0', nr))
        return error("illegal NUL byte");
    size += nr;

    char* nl;
    while ((nl = memchr(dispatch->buf, '\n', size))) {
        *nl = '\0';
        struct job* req_job = NULL;
        if (handle_output(jobs[i], dispatch->buf, &req_job) < 0)
            return -1;
        if (req_job) {
            if (parse_job(req_job) < 0)
                return -1;
            struct dispatch* req_dispatch = req_job->object.extra;
            if (!req_dispatch) {
                create_dispatch_and_pollfd(req_job, pfds, jobs, nfds);
                req_dispatch = req_job->object.extra;
            }
            dispatch_add_session(req_dispatch, jobs[i]);
            dispatch->num_outstanding++;
        }
        size_t off = nl + 1 - dispatch->buf;
        memmove(dispatch->buf, dispatch->buf + off, size - off);
        size -= off;

        if (dispatch->num_outstanding >= MAX_DISPATCHES_PER_SESSION) {
            assert(pfds[i].fd >= 0);
            pfds[i].fd = ~pfds[i].fd;  // disable pollfd
            break;  // stop processing this fd
        }
    }

    if (size >= sizeof(dispatch->buf))
        return error("output too long");
    dispatch->buf[size] = '\0';

    return 1;
}

static void die_usage(char* arg0) {
    fprintf(stderr, "usage: %s <job>\n", arg0);
    exit(1);
}

int main(int argc, char** argv) {
    if (argc != 2)
        die_usage(argv[0]);

    struct job* job = peel_job(argv[1]);
    if (!job || parse_job(job) < 0)
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
    struct job* jobs[MAX_DISPATCHES];
    nfds_t nfds = 0;
    create_dispatch_and_pollfd(job, pfds, jobs, &nfds);

    while (nfds > 0) {
        if (poll(pfds, nfds, -1) < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            die_errno("poll failed");
        }

        nfds_t i = 0;
        while (i < nfds) {
            if (!pfds[i].revents) {
                i++;
                continue;
            }
            assert(!(pfds[i].revents & POLLNVAL));
            // On EOF we observe POLLHUP without POLLIN.
            if (pfds[i].revents & (POLLIN | POLLHUP)) {
                int rc = poll_tick(pfds, jobs, &nfds, i);
                if (rc < 0)
                    exit(1);
                i += rc;
            } else {
                die("unhandled poll revent");
            }
        }
    }

    return 0;
}
