#include "spec.h"

#include "hash.h"
#include "job.h"
#include "object.h"
#include "invocation.h"
#include "production.h"
#include "resource.h"
#include "util.h"

static struct object* get_object(const struct object_id* oid, uint32_t typesig) {
    size_t size;
    void* buf = NULL;

    if (typesig == OBJ_UNKNOWN) {
        buf = read_object(oid, &typesig, &size);
        if (!buf)
            return NULL;
    }

    struct object* ret = NULL;
    if (typesig == OBJ_RESOURCE) {
        struct resource* res = get_resource(oid);
        ret = &res->object;
    } else if (typesig == OBJ_JOB) {
        struct job* job = get_job(oid);
        ret = &job->object;
        // We will probably use the typed object, so incur the overhead of
        // parsing it if buf has already been read.
        if (buf && parse_job_bytes(job, buf, size) < 0)
            warning("failed to parse job %s", oid_to_hex(oid));
    } else if (typesig == OBJ_PRODUCTION) {
        struct production* prd = get_production(oid);
        ret = &prd->object;
        if (buf && parse_production_bytes(prd, buf, size) < 0)
            warning("failed to parse production %s", oid_to_hex(oid));
    } else if (typesig == OBJ_INVOCATION) {
        struct invocation* inv = get_invocation(oid);
        ret = &inv->object;
        if (buf && parse_invocation_bytes(inv, buf, size) < 0)
            warning("failed to parse invocation %s", oid_to_hex(oid));
    } else {
        warning("unknown object type %s", strtypesig(typesig));
    }
    free(buf);
    return ret;
}

static struct object* deref_type(struct object* obj, uint32_t typesig) {
    if (obj->typesig == typesig)
        return obj;

    if (obj->typesig == OBJ_INVOCATION && typesig == OBJ_PRODUCTION) {
        struct invocation* inv = (struct invocation*)obj;
        if (parse_invocation(inv) < 0)
            return NULL;
        // TODO how to handle missing production?
        if (!inv->entries->prd || parse_production(inv->entries->prd) < 0)
            return NULL;
        return &inv->entries->prd->object;
    } else if (obj->typesig == OBJ_PRODUCTION) {
        struct production* prd = (struct production*)obj;
        if (parse_production(prd) < 0)
            return NULL;
        if (typesig == OBJ_JOB) {
            if (parse_job(prd->job) < 0)
                return NULL;
            return &prd->job->object;
        } else if (typesig == OBJ_INVOCATION) {
            // TODO how to handle missing invocation?
            if (!prd->inv || parse_invocation(prd->inv) < 0)
                return NULL;
            return &prd->inv->object;
        }
    }
    return NULL;
}

// Find position of sigil that is followed by a value within braces, or else -1.
// Given return value pos, the braced value starts at pos + 2 until len - 1.
static int find_sigil_braced(char* spec, size_t len, char sigil) {
    if (len < 4 || spec[len - 1] != '}')
        return -1;

    char* p = &spec[len - 3];
    for (; p >= spec; p--)
        if (p[1] == '{')
            return p[0] == sigil ? p - spec : -1;
    return -1;
}

static struct object* peel_deref(char* spec, size_t len) {
    // Try to peel a dereference of the form ^{type}.
    int inner_len = find_sigil_braced(spec, len, '^');
    if (inner_len < 0 || inner_len == (int)len)
        return NULL;

    // Icky NUL termination.
    spec[len - 1] = '\0';
    uint32_t typesig = make_typesig(&spec[inner_len + 2]);

    struct object* inner = peel_spec(spec, inner_len);
    if (!inner)
        return NULL;
    return deref_type(inner, typesig);
}

static struct object* peel_step(char* spec, size_t len) {
    // Try to peel an invocation step of the form @{step}.
    int inner_len = find_sigil_braced(spec, len, '@');
    if (inner_len < 0 || inner_len == (int)len)
        return NULL;
    char* step = &spec[inner_len + 2];
    size_t step_len = len - inner_len - 3;

    struct object* inner = peel_spec(spec, inner_len);
    if (!inner)
        return NULL;
    inner = deref_type(inner, OBJ_INVOCATION);
    if (!inner)
        return NULL;
    struct invocation* inv = (struct invocation*)inner;
    if (parse_invocation(inv) < 0)
        return NULL;

    char* end;
    errno = 0;
    long pos = strtol(step, &end, 10);
    if (end != step + step_len || errno) { // non-numeric step name
        for (struct invocation_entry_list* entry = inv->entries; entry; entry = entry->next) {
            if (strlen(entry->name) == step_len && !memcmp(entry->name, step, step_len))
                return &entry->prd->object;
        }
        return NULL;
    }

    if (pos == 0) {
        warning("@{0} is invalid; first entry is @{1}");
        return NULL;
    } else if (pos < 0) {
        long num_entries = 0;
        for (struct invocation_entry_list* entry = inv->entries; entry; entry = entry->next)
            num_entries++;
        if (pos + num_entries < 0) {
            warning("negative position %ld too large for %ld steps of invocation %s",
                    pos, num_entries, oid_to_hex(&inv->object.oid));
            return NULL;
        }
        pos += num_entries;
    } else {
        pos--;
    }
    struct invocation_entry_list* entry = inv->entries;
    for (long i = 0; i < pos && entry; i++)
        entry = entry->next;
    if (!entry) {
        warning("step position %ld too large for invocation %s",
                pos, oid_to_hex(&inv->object.oid));
        return NULL;
    }
    return &entry->prd->object;
}

static struct object* peel_path(char* spec, size_t len) {
    char* delim = spec + len - 1;
    while (delim >= spec && *delim != ':')
        delim--;
    if (delim < spec)
        return NULL;

    struct object* inner = peel_spec(spec, delim - spec);
    if (!inner)
        return NULL;

    if (inner->typesig == OBJ_INVOCATION) {
        inner = deref_type(inner, OBJ_PRODUCTION);
        if (!inner)
            return NULL;
    }

    // Without a path, just return the resourceful object.
    if (delim + 1 == spec + len) {
        if (inner->typesig != OBJ_PRODUCTION && inner->typesig != OBJ_JOB) {
            warning(":path invalid on %s", strtypesig(inner->typesig));
            return NULL;
        }
        return inner;
    }

    struct resource_list* resources;
    if (inner->typesig == OBJ_JOB) {
        struct job* job = (struct job*)inner;
        if (parse_job(job) < 0)
            return NULL;
        resources = job->inputs;
    } else if (inner->typesig == OBJ_PRODUCTION) {
        struct production* prd = (struct production*)inner;
        if (parse_production(prd) < 0)
            return NULL;
        resources = prd->outputs;
    } else {
        warning("%s specified on type %s", delim, strtypesig(inner->typesig));
        return NULL;
    }

    for (; resources; resources = resources->next) {
        if (!strcmp(resources->name, delim + 1))
            return &resources->res->object;
    }
    warning("'%s' not found in %s %s",
            delim + 1, strtypesig(inner->typesig), oid_to_hex(&inner->oid));
    return NULL;
}

static struct object* invocation_history_last() {
    char filename[PATH_MAX];
    if (snprintf(filename, PATH_MAX, "%s/history", get_knit_dir()) >= PATH_MAX)
        die("path too long");

    int fd = open(filename, O_RDONLY);
    if (fd < 0)
        warning_errno("failed to open %s", filename);

    char buf[KNIT_HASH_HEXSZ + 1];
    if (lseek(fd, -(off_t)sizeof(buf), SEEK_END) < 0) {
        close(fd);
        warning_errno("lseek (short history file?)");
        return NULL;
    }

    int nr;
    size_t offset = 0;
    while (offset < sizeof(buf)) {
        nr = read(fd, buf + offset, sizeof(buf) - offset);
        if (nr < 0 && errno != EAGAIN && errno != EINTR)
            die_errno("read");
        if (nr == 0)
            die("unexpected eof");
        offset += nr;
    }
    close(fd);

    if (buf[sizeof(buf) - 1] != '\n') {
        warning("corrupted history line");
        return NULL;
    }

    struct object_id oid;
    if (hex_to_oid(buf, &oid) < 0) {
        warning("bad invocation history hash");
        return NULL;
    }
    return &get_invocation(&oid)->object;
}

static struct object* peel_spec_fast(char* spec, size_t len, uint32_t typesig) {
    struct object* ret;
    struct object_id oid;

    if ((ret = peel_deref(spec, len)))
        return ret;

    if ((ret = peel_step(spec, len)))
        return ret;

    if ((ret = peel_path(spec, len)))
        return ret;

    if (len == KNIT_HASH_HEXSZ && !hex_to_oid(spec, &oid))
        return get_object(&oid, typesig);

    if (len == 1 && spec[0] == '@')
        return invocation_history_last();

    return NULL;
}

struct object* peel_spec(char* spec, size_t len) {
    return peel_spec_fast(spec, len, OBJ_UNKNOWN);
}

static struct object* peel_spec_of_type(char* spec, size_t len, uint32_t typesig) {
    struct object* ret;
    if (!(ret = peel_spec_fast(spec, len, typesig))) {
        error("could not parse %s %.*s", strtypesig(typesig), (int)len, spec);
        return NULL;
    }
    if (!(ret = deref_type(ret, typesig)))
        error("object %s not of type %s", oid_to_hex(&ret->oid), strtypesig(typesig));
    return ret;
}

struct resource* peel_resource(char* spec) {
    return (struct resource*)peel_spec_of_type(spec, strlen(spec), OBJ_RESOURCE);
}

struct job* peel_job(char* spec) {
    return (struct job*)peel_spec_of_type(spec, strlen(spec), OBJ_JOB);
}

struct production* peel_production(char* spec) {
    return (struct production*)peel_spec_of_type(spec, strlen(spec), OBJ_PRODUCTION);
}

struct invocation* peel_invocation(char* spec) {
    return (struct invocation*)peel_spec_of_type(spec, strlen(spec), OBJ_INVOCATION);
}
