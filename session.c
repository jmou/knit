#include "session.h"

#include "job.h"

struct session_step** active_steps;
size_t num_active_steps;
size_t alloc_active_steps;

struct session_input** active_inputs;
size_t num_active_inputs;
size_t alloc_active_inputs;

struct session_dependency** active_deps;
size_t num_active_deps;
size_t alloc_active_deps;
int deps_dirty; // may not be ordered

static size_t realloc_size(size_t alloc) { return (alloc + 16) * 2; }

static void ensure_alloc_steps() {
    if (num_active_steps < alloc_active_steps)
        return;
    alloc_active_steps = realloc_size(alloc_active_steps);
    active_steps = xrealloc(active_steps, alloc_active_steps * sizeof(*active_steps));
}

static void ensure_alloc_inputs() {
    if (num_active_inputs < alloc_active_inputs)
        return;
    alloc_active_inputs = realloc_size(alloc_active_inputs);
    active_inputs = xrealloc(active_inputs, alloc_active_inputs * sizeof(*active_inputs));
}

static void ensure_alloc_deps() {
    if (num_active_deps < alloc_active_deps)
        return;
    alloc_active_deps = realloc_size(alloc_active_deps);
    active_deps = xrealloc(active_deps, alloc_active_deps * sizeof(*active_deps));
}

size_t create_session_step(const char* name) {
    size_t namelen = strlen(name);
    struct session_step* ss = xmalloc(sizeof(struct session_step) + namelen + 1);
    memset(ss, 0, sizeof(*ss));
    if (namelen > SS_NAMEMASK)
        die("step name too long %s", name);
    ss_init_flags(ss, namelen, 0);
    strcpy(ss->name, name);

    ensure_alloc_steps();
    active_steps[num_active_steps] = ss;
    return num_active_steps++;
}

size_t create_session_input(size_t step_pos, const char* path) {
    assert(step_pos < num_active_steps);
    // Maintain session inputs in strictly monotonic order.
    if (num_active_inputs > 0) {
        struct session_input* prev_si = active_inputs[num_active_inputs - 1];
        size_t prev_step_pos = ntohl(prev_si->step_pos);
        if (step_pos < prev_step_pos ||
                (step_pos == prev_step_pos && strcmp(path, prev_si->path) <= 0))
            die("step %zu input %s <= previous step %zu input %s",
                step_pos, path, prev_step_pos, prev_si->path);
    }

    size_t pathlen = strlen(path);
    struct session_input* si = xmalloc(sizeof(struct session_input) + pathlen + 1);
    memset(si, 0, sizeof(*si));
    si->step_pos = htonl(step_pos);
    if (pathlen > SI_PATHMASK)
        die("input path too long %s", path);
    si_init_flags(si, pathlen, 0);
    strcpy(si->path, path);

    ensure_alloc_inputs();
    active_inputs[num_active_inputs] = si;
    return num_active_inputs++;
}

size_t create_session_dependency(size_t input_pos,
                                 size_t step_pos, const char* output,
                                 uint16_t flags) {
    assert(input_pos < num_active_inputs);
    struct session_input* si = active_inputs[input_pos];
    size_t dependent_pos = ntohl(si->step_pos);
    if (dependent_pos >= num_active_steps)
        die("dependent out of bounds");
    ss_inc_unresolved(active_steps[dependent_pos]);

    size_t outlen = strlen(output);
    struct session_dependency* sd = xmalloc(sizeof(struct session_dependency) + outlen + 1);
    sd->input_pos = htonl(input_pos);
    sd->step_pos = htonl(step_pos);
    if (outlen > SD_OUTPUTMASK)
        die("output path too long %s", output);
    sd_init_flags(sd, outlen, flags);
    strcpy(sd->output, output);

    deps_dirty = 1;
    ensure_alloc_deps();
    active_deps[num_active_deps] = sd;
    return num_active_deps++;
}

char session_filepath[PATH_MAX];
const char* session_name;

const char* get_session_name() {
    if (!session_name)
        die("no session persisted");
    return session_name;
}

static void set_session_name(const char* sessname) {
    if (strchr(sessname, '/') && strlen(sessname) < PATH_MAX) {
        strcpy(session_filepath, sessname);
        session_name = session_filepath;
        return;
    }

    if (snprintf(session_filepath, PATH_MAX,
                 "%s/sessions/%s", get_knit_dir(), sessname) >= PATH_MAX)
        die("session path too long");
    session_name = session_filepath + strlen(session_filepath) - strlen(sessname);
}

struct session_header {
    uint32_t num_steps;
    uint32_t num_inputs;
    uint32_t num_deps;
};

int load_session(const char* sessname) {
    set_session_name(sessname);

    int fd = open(session_filepath, O_RDONLY);
    if (fd < 0)
        return error("cannot open %s: %s", session_filepath, strerror(errno));
    struct bytebuf bb; // leaked
    if (mmap_fd(fd, &bb) < 0 && slurp_fd(fd, &bb) < 0)
        return -1;

    if (bb.size < sizeof(struct session_header))
        return error("truncated session header");
    struct session_header* hdr = bb.data;

    alloc_active_steps = num_active_steps = ntohl(hdr->num_steps);
    active_steps = xmalloc(alloc_active_steps * sizeof(struct session_step*));
    alloc_active_inputs = num_active_inputs = ntohl(hdr->num_inputs);
    active_inputs = xmalloc(alloc_active_inputs * sizeof(struct session_input*));
    alloc_active_deps = num_active_deps = ntohl(hdr->num_deps);
    active_deps = xmalloc(alloc_active_deps * sizeof(struct session_dependency*));

    char* end = (char*)bb.data + bb.size;
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
        if (si->path[si_path_len(si)] != '\0')
            return error("input path not NUL-terminated");
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
    // Ordering by input is not essential but determinism is nice.
    size_t a_input = ntohl(pa->input_pos);
    size_t b_input = ntohl(pb->input_pos);
    if (a_input != b_input)
        return a_input < b_input ? -1 : 1;
    return 0;
}

int save_session() {
    // TODO should allocate a new session name, avoiding collisions
    if (!session_name)
        set_session_name("default");

    if (deps_dirty)
        qsort(active_deps, num_active_deps, sizeof(*active_deps), cmp_dep);

    char lockfile[PATH_MAX];
    if (snprintf(lockfile, PATH_MAX, "%s.lock", session_filepath) >= PATH_MAX)
        return error("lock path too long");

    int fd = open(lockfile, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd < 0) {
        if (errno == EEXIST)
            return error("lockfile already exists: %s", lockfile);
        return error("open error %s: %s", lockfile, strerror(errno));
    }

    struct session_header hdr = {
        .num_steps = htonl(num_active_steps),
        .num_inputs = htonl(num_active_inputs),
        .num_deps = htonl(num_active_deps),
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
        error("close error %s: %s", lockfile, strerror(errno));
        goto fail_and_unlock;
    }

    if (rename(lockfile, session_filepath) < 0) {
        error("rename error to %s: %s", session_filepath, strerror(errno));
        goto fail_and_unlock;
    }

    return 0;

write_fail:
    error("write error %s: %s", lockfile, strerror(errno));
    close(fd);

fail_and_unlock:
    unlink(lockfile);
    return -1;
}

ssize_t find_stepish(const char* stepish) {
    if (stepish[0] == '@' && isdigit(stepish[1])) {
        char* end;
        size_t pos = strtoul(&stepish[1], &end, 10);
        if (end != &stepish[1] && *end == '\0' && pos < num_active_steps)
            return pos;
    }
    for (size_t i = 0; i < num_active_steps; i++) {
        if (!strcmp(active_steps[i]->name, stepish))
            return i;
    }
    return -1;
}

// This translates between session_input and job, so it could reasonably reside
// in either job.c or session.c. In any case, store_job() is primarily concerned
// with serialization, while compile_job_for_step() does the rest.
static struct job* store_job(struct session_input** inputs, size_t inputs_size) {
    size_t size = sizeof(struct job_header);
    size_t num_inputs = 0;
    for (size_t i = 0; i < inputs_size; i++) {
        if (!si_hasflag(inputs[i], SI_RESOURCE))
            continue;
        size += sizeof(struct job_input) + strlen(inputs[i]->path) + 1;
        num_inputs++;
    }

    char* buf = xmalloc(size);
    struct job_header* hdr = (struct job_header*)buf;
    hdr->num_inputs = htonl(num_inputs);
    char* p = buf + sizeof(*hdr);

    for (size_t i = 0; i < inputs_size; i++) {
        if (!si_hasflag(inputs[i], SI_RESOURCE))
            continue;
        struct job_input* in = (struct job_input*)p;
        memcpy(in->res_hash, inputs[i]->res_hash, KNIT_HASH_RAWSZ);
        size_t pathsize = strlen(inputs[i]->path) + 1;
        memcpy(in->path, inputs[i]->path, pathsize);
        p += sizeof(*in) + pathsize;
    }

    struct object_id oid;
    int rc = write_object(TYPE_JOB, buf, size, &oid);
    free(buf);
    return rc < 0 ? NULL : get_job(&oid);
}

int compile_job_for_step(size_t step_pos) {
    struct session_step* ss = active_steps[step_pos];
    if (ss_hasflag(ss, SS_JOB))
        return error("step already has job");
    if (ss_hasflag(ss, SS_FINAL))
        return error("step has unmet requirements");
    if (ss->num_unresolved)
        return error("step blocked on %u dependencies", ntohs(ss->num_unresolved));

    size_t i = 0;
    while (i < num_active_inputs && ntohl(active_inputs[i]->step_pos) < step_pos)
        i++;
    size_t start = i;
    while (i < num_active_inputs && ntohl(active_inputs[i]->step_pos) == step_pos) {
        if (!si_hasflag(active_inputs[i], SI_FINAL))
            return error("expected final input");
        i++;
    }
    size_t limit = i;

    struct job* job = store_job(&active_inputs[start], limit - start);
    if (!job)
        return -1;

    memcpy(ss->job_hash, job->object.oid.hash, KNIT_HASH_RAWSZ);
    ss_setflag(ss, SS_JOB);
    return 0;
}
