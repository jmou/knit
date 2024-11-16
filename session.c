#include "session.h"

#include "alloc.h"
#include "hash.h"
#include "job.h"

static struct bytebuf session_bb;
static struct bump_list* session_bump;

static void* bmalloc(size_t size) {
    size_t* raw = bump_alloc(&session_bump, sizeof(size) + size);
    *raw = size;
    return raw + 1;
}

static void* brealloc(void* ptr, size_t size) {
    void* copy = bmalloc(size);
    if (ptr)
        memcpy(copy, ptr, ((size_t*)ptr)[-1]); // ptr is leaked
    return copy;
}

#define MAX_SESSION_NUM (~(uint32_t)0)
static_assert(MAX_SESSION_NUM <= SIZE_MAX);

struct session_step** active_steps;
size_t num_active_steps;
static size_t alloc_active_steps;

size_t num_active_fanout;

struct session_input** active_inputs;
size_t num_active_inputs;
static size_t alloc_active_inputs;

struct session_dependency** active_deps;
size_t num_active_deps;
static size_t alloc_active_deps;
static int deps_dirty; // may not be ordered

static size_t realloc_size(size_t alloc) { return (alloc + 16) * 2; }

static void ensure_alloc_steps() {
    if (num_active_steps < alloc_active_steps)
        return;
    alloc_active_steps = realloc_size(alloc_active_steps);
    active_steps = brealloc(active_steps, alloc_active_steps * sizeof(*active_steps));
}

static void ensure_alloc_inputs() {
    if (num_active_inputs < alloc_active_inputs)
        return;
    alloc_active_inputs = realloc_size(alloc_active_inputs);
    active_inputs = brealloc(active_inputs, alloc_active_inputs * sizeof(*active_inputs));
}

static void ensure_alloc_deps() {
    if (num_active_deps < alloc_active_deps)
        return;
    alloc_active_deps = realloc_size(alloc_active_deps);
    active_deps = brealloc(active_deps, alloc_active_deps * sizeof(*active_deps));
}

size_t create_session_step(const char* name) {
    if (num_active_steps == MAX_SESSION_NUM)
        die("too many steps");
    if (num_active_fanout > 0)
        die("step created after fanout started");

    size_t namelen = strlen(name);
    struct session_step* ss = bmalloc(sizeof(struct session_step) + namelen + 1);
    memset(ss, 0, sizeof(*ss));
    if (namelen > SS_NAMEMASK)
        die("step name too long %s", name);
    ss_init_flags(ss, namelen, 0);
    strcpy(ss->name, name);

    ensure_alloc_steps();
    active_steps[num_active_steps] = ss;
    return num_active_steps++;
}

size_t create_session_input(size_t step_pos, const char* name) {
    assert(step_pos < num_active_steps + num_active_fanout);
    if (num_active_inputs == MAX_SESSION_NUM)
        die("too many inputs");

    // Maintain session inputs in strictly monotonic order.
    if (num_active_inputs > 0) {
        struct session_input* prev_si = active_inputs[num_active_inputs - 1];
        size_t prev_step_pos = ntohl(prev_si->step_pos);
        if (step_pos < prev_step_pos ||
                (step_pos == prev_step_pos && strcmp(name, prev_si->name) <= 0))
            die("step %zu input %s <= previous step %zu input %s",
                step_pos, name, prev_step_pos, prev_si->name);
    }

    size_t namelen = strlen(name);
    struct session_input* si = bmalloc(sizeof(struct session_input) + namelen + 1);
    memset(si, 0, sizeof(*si));
    si->step_pos = htonl(step_pos);
    if (namelen > SI_PATHMASK)
        die("input name too long %s", name);
    si_init_flags(si, namelen, 0);
    strcpy(si->name, name);

    ensure_alloc_inputs();
    active_inputs[num_active_inputs] = si;
    return num_active_inputs++;
}

size_t create_session_dependency(size_t input_pos,
                                 size_t step_pos, const char* output,
                                 uint16_t flags) {
    if (num_active_deps == MAX_SESSION_NUM)
        die("too many dependencies");

    size_t dependent_pos;
    if (flags & SD_INPUTISSTEP) {
        dependent_pos = input_pos;
    } else {
        assert(input_pos < num_active_inputs);
        struct session_input* si = active_inputs[input_pos];
        dependent_pos = ntohl(si->step_pos);
    }
    if (dependent_pos >= num_active_steps)
        die("dependent out of bounds");
    ss_inc_unresolved(active_steps[dependent_pos]);

    size_t outlen = strlen(output);
    struct session_dependency* sd = bmalloc(sizeof(struct session_dependency) + outlen + 1);
    memset(sd, 0, sizeof(*sd));
    sd->input_pos = htonl(input_pos);
    sd->step_pos = htonl(step_pos);
    if (outlen > SD_OUTPUTMASK)
        die("output name too long %s", output);
    sd_init_flags(sd, outlen, flags);
    strcpy(sd->output, output);

    deps_dirty = 1;
    ensure_alloc_deps();
    active_deps[num_active_deps] = sd;
    return num_active_deps++;
}

// add_session_fanout_step() must be called after all normal steps have been
// created; its returned step position is always greater than num_active_steps.
size_t add_session_fanout_step() {
    if (num_active_steps + num_active_fanout == MAX_SESSION_NUM)
        die("too many fanout steps");
    return num_active_steps + num_active_fanout++;
}

static char session_filepath[PATH_MAX];
static char session_lockfile[PATH_MAX];
static const char* session_name;

static int unlock_session_atexit;

static void unlock_session() {
    if (*session_lockfile)
        unlink(session_lockfile);
}

static int set_session_name(const char* sessname) {
    assert(!session_name);

    if (strchr(sessname, '/') && strlen(sessname) < PATH_MAX) {
        strcpy(session_filepath, sessname);
        session_name = session_filepath;
    } else {
        int len = snprintf(session_filepath, PATH_MAX,
                           "%s/sessions/%s", get_knit_dir(), sessname);
        if (len >= PATH_MAX)
            return error("session path too long");
        session_name = session_filepath + len - strlen(sessname);
    }

    return 0;
}

static int set_session_name_and_lock(const char* sessname) {
    static int is_rand_seeded = 0;
    if (!is_rand_seeded) {
        srand(getpid());
        is_rand_seeded = 1;
    }

    if (set_session_name(sessname) < 0)
        return -1;

    if (snprintf(session_lockfile, PATH_MAX,
                 "%s.lock", session_filepath) >= PATH_MAX)
        return error("lock path too long");

    int fd;
    long backoff_ms = 1;
    while ((fd = open(session_lockfile, O_WRONLY | O_CREAT | O_EXCL, 0666)) < 0) {
        if (errno != EEXIST)
            return error_errno("open error %s", session_lockfile);

        backoff_ms *= 2;
        if (backoff_ms > 10000)
            return error("lockfile timed out: %s", session_lockfile);
        // Randomly perturb backoff by +/-25% and scale to microseconds.
        long sleep_us = backoff_ms * (750 + rand() % 500);
        usleep(sleep_us);
    }

    if (!unlock_session_atexit) {
        atexit(unlock_session);
        unlock_session_atexit = 1;
    }

    return 0;
}

struct session_header {
    uint32_t num_steps;
    uint32_t num_inputs;
    uint32_t num_deps;
    uint32_t num_fanout;
};

int new_session(const char* sessname) {
    if (set_session_name_and_lock(sessname) < 0)
        return -1;

    struct stat st;
    if (stat(session_filepath, &st) == 0)
        return error("existing session %s", session_name);
    return 0;
}

int load_current_session() {
    if (mmap_or_slurp_file(session_filepath, &session_bb) < 0)
        return -1;

    if (session_bb.size < sizeof(struct session_header))
        return error("truncated session header");
    struct session_header* hdr = session_bb.data;

    alloc_active_steps = num_active_steps = ntohl(hdr->num_steps);
    active_steps = bmalloc(alloc_active_steps * sizeof(struct session_step*));
    alloc_active_inputs = num_active_inputs = ntohl(hdr->num_inputs);
    active_inputs = bmalloc(alloc_active_inputs * sizeof(struct session_input*));
    alloc_active_deps = num_active_deps = ntohl(hdr->num_deps);
    active_deps = bmalloc(alloc_active_deps * sizeof(struct session_dependency*));
    num_active_fanout = ntohl(hdr->num_fanout);

    char* end = (char*)session_bb.data + session_bb.size;
    char* p = (char*)hdr + sizeof(*hdr);
    for (size_t i = 0; i < num_active_steps; i++) {
        struct session_step* ss = (struct session_step*)p;
        active_steps[i] = ss;
        p += ss_size(ss);
        if (p > end)
            return error("session EOF in steps");
        if (ss->name[ss_name_len(ss)] != '\0')
            return error("step name not NUL-terminated");
    }
    for (size_t i = 0; i < num_active_inputs; i++) {
        struct session_input* si = (struct session_input*)p;
        active_inputs[i] = si;
        p += si_size(si);
        if (p > end)
            return error("session EOF in inputs");
        if (si->name[si_name_len(si)] != '\0')
            return error("input name not NUL-terminated");
    }
    for (size_t i = 0; i < num_active_deps; i++) {
        struct session_dependency* sd = (struct session_dependency*)p;
        active_deps[i] = sd;
        p += sd_size(sd);
        if (p > end)
            return error("session EOF in dependencies");
        if (sd->output[sd_output_len(sd)] != '\0')
            return error("dependency output not NUL-terminated");
    }
    if (p != end)
        return error("trailing session data");

    deps_dirty = 0;
    return 0;
}

int load_session(const char* sessname) {
    if (set_session_name_and_lock(sessname) < 0)
        return -1;
    return load_current_session();
}

int load_session_nolock(const char* sessname) {
    if (set_session_name(sessname) < 0)
        return -1;
    return load_current_session();
}

void close_session() {
    active_steps = NULL;
    num_active_steps = num_active_fanout = alloc_active_steps = 0;

    active_inputs = NULL;
    num_active_inputs = alloc_active_inputs = 0;

    active_deps = NULL;
    num_active_deps = alloc_active_deps = 0;

    deps_dirty = 0;

    free_bump_list(&session_bump);
    cleanup_bytebuf(&session_bb);

    unlock_session();

    session_filepath[0] = session_lockfile[0] = '\0';
    session_name = NULL;
}

static int cmp_dep(const void* a, const void* b) {
    const struct session_dependency* pa = *(const struct session_dependency**)a;
    const struct session_dependency* pb = *(const struct session_dependency**)b;
    size_t a_step = ntohl(pa->step_pos);
    size_t b_step = ntohl(pb->step_pos);
    if (a_step != b_step)
        return a_step < b_step ? -1 : 1;
    int rc = strcmp(pa->output, pb->output);
    if (rc != 0)
        return rc;
    // Ordering by flags and input is not essential but determinism is nice.
    size_t a_flags = ntohs(pa->sd_flags);
    size_t b_flags = ntohs(pb->sd_flags);
    if (a_flags != b_flags)
        return a_flags < b_flags ? -1 : 1;
    size_t a_input = ntohl(pa->input_pos);
    size_t b_input = ntohl(pb->input_pos);
    if (a_input != b_input)
        return a_input < b_input ? -1 : 1;
    return 0;
}

int save_session() {
    assert(session_name);
    assert(*session_lockfile);

    if (deps_dirty)
        qsort(active_deps, num_active_deps, sizeof(*active_deps), cmp_dep);

    char tempfile[PATH_MAX];
    if (snprintf(tempfile, PATH_MAX, "%s.tmp", session_filepath) >= PATH_MAX)
        return error("tempfile path too long");

    int fd = creat(tempfile, 0666);
    if (fd < 0)
        return error_errno("open error %s", tempfile);

    struct session_header hdr = {
        .num_steps = htonl(num_active_steps),
        .num_inputs = htonl(num_active_inputs),
        .num_deps = htonl(num_active_deps),
        .num_fanout = htonl(num_active_fanout),
    };

    if (write(fd, &hdr, sizeof(hdr)) != sizeof(hdr))
        goto write_fail;

    for (size_t i = 0; i < num_active_steps; i++) {
        struct session_step* ss = active_steps[i];
        if (write(fd, ss, ss_size(ss)) != (ssize_t)ss_size(ss))
            goto write_fail;
    }
    for (size_t i = 0; i < num_active_inputs; i++) {
        struct session_input* si = active_inputs[i];
        if (write(fd, si, si_size(si)) != (ssize_t)si_size(si))
            goto write_fail;
    }
    for (size_t i = 0; i < num_active_deps; i++) {
        struct session_dependency* sd = active_deps[i];
        if (write(fd, sd, sd_size(sd)) != (ssize_t)sd_size(sd))
            goto write_fail;
    }

    if (close(fd) < 0) {
        error_errno("close error %s", tempfile);
        goto fail_and_unlink;
    }

    if (rename(tempfile, session_filepath) < 0) {
        error_errno("rename error to %s", session_filepath);
        goto fail_and_unlink;
    }

    return 0;

write_fail:
    error_errno("write error %s", tempfile);
    close(fd);

fail_and_unlink:
    unlink(tempfile);
    return -1;
}

size_t first_step_input(size_t step_pos, size_t start, size_t end) {
    while (start < end) {
        size_t mid = (start + end) / 2;
        if (step_pos <= ntohl(active_inputs[mid]->step_pos)) {
            end = mid;
        } else {
            start = mid + 1;
        }
    }
    return start;
}

static struct resource_list* step_inputs_to_resource_list(struct bump_list** bump_p,
                                                          size_t step_pos,
                                                          const char* prefix) {
    struct resource_list* head = NULL;
    struct resource_list** list_p = &head;

    for (size_t i = first_step_input(step_pos, 0, num_active_inputs);
         i < num_active_inputs; i++) {
        struct session_input* si = active_inputs[i];
        if (ntohl(si->step_pos) != step_pos)
            break;

        assert(si_hasflag(si, SI_FINAL));
        if (si_hasflag(si, SI_FANOUT)) {
            assert(!prefix); // recursive SI_FANOUT is not supported
            *list_p = step_inputs_to_resource_list(bump_p, ntohl(si->fanout_step_pos),
                                                   si->name);
            assert(*list_p);
            while (*list_p)
                list_p = &(*list_p)->next;
            continue;
        }
        if (!si_hasflag(si, SI_RESOURCE))
            continue;

        struct resource_list* list = bump_alloc(bump_p, sizeof(*list));
        if (prefix) {
            char name[PATH_MAX];
            int len = snprintf(name, PATH_MAX, "%s%s", prefix, si->name);
            if (len >= PATH_MAX)
                die("input name too long");
            list->name = bump_alloc(bump_p, len + 1);
            memcpy(list->name, name, len + 1);
        } else {
            list->name = si->name;
        }

        list->res = get_resource(oid_of_hash(si->res_hash));
        list->next = NULL;
        *list_p = list;
        list_p = &list->next;
    }

    return head;
}

int compile_job_for_step(size_t step_pos) {
    struct session_step* ss = active_steps[step_pos];
    if (ss_hasflag(ss, SS_JOB))
        return error("step already has job");
    if (ss_hasflag(ss, SS_FINAL))
        return error("step has unmet requirements");
    if (ss->num_unresolved)
        return error("step blocked on %u dependencies", ntohs(ss->num_unresolved));

    struct bump_list* bump = NULL;
    struct resource_list* inputs = step_inputs_to_resource_list(&bump, step_pos, NULL);
    if (!inputs)
        warning("empty job at step_pos %zu", step_pos);

    struct job* job = store_job(inputs);
    free_bump_list(&bump);
    if (!job)
        return -1;

    memcpy(ss->job_hash, job->object.oid.hash, KNIT_HASH_RAWSZ);
    ss_setflag(ss, SS_JOB);
    return 0;
}

static void set_input_resource(struct session_input* si, struct resource* res) {
    memcpy(si->res_hash, res->object.oid.hash, KNIT_HASH_RAWSZ);
    si_setflag(si, SI_RESOURCE | SI_FINAL);
}

// Create a session_input for each output with a matching prefix; outputs should
// be positioned at the first match. The prefix will be removed from the input.
static void create_fanout_inputs(size_t fanout_step_pos,
                                 const struct resource_list* outputs,
                                 const char* prefix) {
    do {
        // When depending on an entire production, skip special .knit/ outputs.
        if (!*prefix && !strncmp(outputs->name, ".knit/", 6)) {
            outputs = outputs->next;
            continue;
        }

        char* name = outputs->name + strlen(prefix);
        size_t input_pos = create_session_input(fanout_step_pos, name);
        struct session_input* si = active_inputs[input_pos];
        set_input_resource(si, outputs->res);
        outputs = outputs->next;
    } while (outputs && !strncmp(outputs->name, prefix, strlen(prefix)));
}

void resolve_dependencies(size_t step_pos, const struct resource_list* outputs) {
    size_t dep_pos = 0;
    while (dep_pos < num_active_deps && ntohl(active_deps[dep_pos]->step_pos) < step_pos)
        dep_pos++;

    while (dep_pos < num_active_deps) {
        struct session_dependency* dep = active_deps[dep_pos];
        if (dep->step_pos != htonl(step_pos))
            break;

        // Iterate production outputs in parallel with the dependencies waiting
        // on this step. Compare their paths to find matching dependencies.
        int cmp = 1;
        if (outputs) {
            cmp = sd_hasflag(dep, SD_PREFIX)
                ? strncmp(outputs->name, dep->output, strlen(dep->output))
                : strcmp(outputs->name, dep->output);
        }

        // If a production output has no dependencies on it, we can skip it.
        if (cmp < 0) {
            outputs = outputs->next;
            continue;
        }

        struct session_input* input;
        size_t dependent_pos;
        if (sd_hasflag(dep, SD_INPUTISSTEP)) {
            input = NULL;
            dependent_pos = ntohl(dep->input_pos);
            if (sd_hasflag(dep, SD_PREFIX))
                die("SD_INPUTISSTEP and SD_PREFIX are incompatible");
        } else {
            size_t input_pos = ntohl(dep->input_pos);
            if (input_pos >= num_active_inputs)
                die("input out of bounds");
            input = active_inputs[input_pos];
            dependent_pos = ntohl(input->step_pos);
        }
        if (dependent_pos >= num_active_steps)
            die("dependent out of bounds");
        if (dependent_pos <= step_pos)
            die("step later than its dependent");
        struct session_step* dependent = active_steps[dependent_pos];

        // Otherwise, we have a dependency to resolve. If we have no matching
        // production output, then the dependency is missing.
        if (cmp > 0) {
            if (input)
                si_setflag(input, SI_FINAL);
            if (!sd_hasflag(dep, SD_REQUIRED)) {
                goto mark_resolved;
            } else if (!ss_hasflag(dependent, SS_FINAL)) {
                // If the missing dependency is required, its dependent step
                // must finish (with unmet requirements). Since there are no
                // production outputs, we trivially resolve dependencies that
                // are provided by the dependent step. This in turn may finish
                // several additional steps with unmet requirements.
                resolve_dependencies(dependent_pos, NULL);
                ss_setflag(dependent, SS_FINAL);
            }
            dep_pos++;
            continue;
        }

        // Note: cmp == 0
        // The production and dependency outputs match.

        // If the dependency is a prefix, create a fanout step whose inputs are
        // all matching production outputs.
        if (sd_hasflag(dep, SD_PREFIX)) {
            assert(input);
            size_t fanout_step_pos = add_session_fanout_step();
            create_fanout_inputs(fanout_step_pos, outputs, dep->output);
            si_setflag(input, SI_FANOUT | SI_FINAL);
            input->fanout_step_pos = htonl(fanout_step_pos);
        } else if (input) {
            // Set a single matching resource for the input.
            set_input_resource(input, outputs->res);
        }

mark_resolved:
        if (!dependent->num_unresolved)
            die("num_unresolved underflow on step %s", dependent->name);
        ss_dec_unresolved(dependent);
        if (!dependent->num_unresolved)
            if (compile_job_for_step(dependent_pos) < 0)
                exit(1);
        dep_pos++;
        // The same production output may resolve additional dependencies, so
        // leave it for the next iteration.
    }
}
