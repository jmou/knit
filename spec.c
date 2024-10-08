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
            if (!prd->inv || parse_invocation(prd->inv) < 0)
                return NULL;
            return &prd->inv->object;
        }
    }
    return NULL;
}

static struct object* peel_spec_and_deref(char* spec, size_t len, uint32_t typesig) {
    struct object* obj = peel_spec(spec, len);
    return obj ? deref_type(obj, typesig) : NULL;
}

static struct invocation* peel_spec_and_deref_invocation(char* spec, size_t len) {
    struct object* obj = peel_spec_and_deref(spec, len, OBJ_INVOCATION);
    if (!obj)
        return NULL;
    struct invocation* inv = (struct invocation*)obj;
    return inv && !parse_invocation(inv) ? inv : NULL;
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

// Find position of sigil that is optionally followed by a non-negative number,
// or else -1. If a number is present it will be written to *num; otherwise it
// will be ignored.
static int find_sigil_numeric(char* spec, size_t len, char sigil, unsigned* num) {
    if (len < 2)
        return -1;
    char* delim = spec + len - 1;
    while (delim > spec && isdigit(*delim))
        delim--;
    if (delim == spec || *delim != sigil)
        return -1;

    if (delim + 1 < spec + len) {
        *num = 0;
        for (char* c = delim + 1; c < spec + len; c++)
            *num = 10 * (*num) + *c - '0';
    }

    return delim - spec;
}

static struct object* peel_type(char* spec, size_t len) {
    int inner_len = find_sigil_braced(spec, len, '^');
    if (inner_len < 0 || inner_len == (int)len)
        return NULL;

    // Icky NUL termination.
    spec[len - 1] = '\0';
    uint32_t typesig = make_typesig(&spec[inner_len + 2]);

    return peel_spec_and_deref(spec, inner_len, typesig);
}

static struct object* peel_type_job(char* spec, size_t len) {
    if (len < 1 || spec[len - 1] != '^')
        return NULL;
    return peel_spec_and_deref(spec, len - 1, OBJ_JOB);
}

static struct object* peel_step_name(char* spec, size_t len) {
    int inner_len = find_sigil_braced(spec, len, '=');
    if (inner_len < 0 || inner_len == (int)len)
        return NULL;
    char* step = &spec[inner_len + 2];
    size_t step_len = len - inner_len - 3;

    struct invocation* inv = peel_spec_and_deref_invocation(spec, inner_len);
    if (!inv)
        return NULL;

    for (struct invocation_entry_list* entry = inv->entries; entry; entry = entry->next) {
        if (strlen(entry->name) == step_len && !memcmp(entry->name, step, step_len))
            return &entry->prd->object;
    }
    return NULL;
}

static struct object* peel_step_pos(char* spec, size_t len) {
    unsigned pos = 1;
    int inner_len = find_sigil_numeric(spec, len, '=', &pos);
    if (inner_len < 0)
        return NULL;
    if (pos == 0)
        die("=0 is invalid");

    struct invocation* inv = peel_spec_and_deref_invocation(spec, inner_len);
    if (!inv)
        return NULL;

    struct invocation_entry_list* entry = inv->entries;
    for (unsigned i = 1; i < pos && entry; i++) // pos is 1-indexed
        entry = entry->next;
    if (!entry) {
        die("step position %d too large for invocation %s",
            pos, oid_to_hex(&inv->object.oid));
    }
    return &entry->prd->object;
}

static struct object* peel_chain(char* spec, size_t len) {
    unsigned depth = 0;
    int inner_len = find_sigil_numeric(spec, len, '~', &depth);
    if (inner_len < 0)
        return NULL;

    struct invocation* inv = peel_spec_and_deref_invocation(spec, inner_len);
    if (!inv)
        return NULL;

    for (unsigned i = 0; i < depth; i++) {
        if (parse_invocation(inv) < 0)
            return NULL;
        struct production* prd =
            (struct production*)deref_type(&inv->object, OBJ_PRODUCTION);
        if (!prd || parse_production(prd) < 0)
            return NULL;
        inv = (struct invocation*)deref_type(&prd->object, OBJ_INVOCATION);
        if (!inv) {
            die("cannot unwrap invocation of depth %u from %.*s",
                depth, inner_len, spec);
        }
    }
    return &inv->object;
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

    if (inner->typesig != OBJ_PRODUCTION && inner->typesig != OBJ_JOB) {
        die("%*s invalid on %s %s", (int)(spec + len - delim), delim,
            strtypesig(inner->typesig), oid_to_hex(&inner->oid));
    }

    // Without a path, just return the resourceful object.
    if (delim + 1 == spec + len)
        return inner;

    struct resource_list* resources;
    if (inner->typesig == OBJ_JOB) {
        struct job* job = (struct job*)inner;
        if (parse_job(job) < 0)
            return NULL;
        resources = job->inputs;
    } else {
        assert(inner->typesig == OBJ_PRODUCTION);
        struct production* prd = (struct production*)inner;
        if (parse_production(prd) < 0)
            return NULL;
        resources = prd->outputs;
    }

    for (; resources; resources = resources->next) {
        if (!strcmp(resources->name, delim + 1))
            return &resources->res->object;
    }
    die("'%s' not found in %s %s",
        delim + 1, strtypesig(inner->typesig), oid_to_hex(&inner->oid));
}

static struct object* production_history_last() {
    char filename[PATH_MAX];
    if (snprintf(filename, PATH_MAX, "%s/history", get_knit_dir()) >= PATH_MAX)
        die("path too long");

    int fd = open(filename, O_RDONLY);
    if (fd < 0)
        die_errno("failed to open %s", filename);

    char buf[KNIT_HASH_HEXSZ + 1];
    if (lseek(fd, -(off_t)sizeof(buf), SEEK_END) < 0)
        die_errno("lseek (short history file?)");

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

    if (buf[sizeof(buf) - 1] != '\n')
        die("corrupted history line");

    struct object_id oid;
    if (hex_to_oid(buf, &oid) < 0)
        die("bad production history hash");
    return &get_production(&oid)->object;
}

static struct object* peel_spec_fast(char* spec, size_t len, uint32_t typesig) {
    struct object* ret;
    struct object_id oid;

    // Job input or production output of the form :path.
    if ((ret = peel_path(spec, len)))
        return ret;

    // Dereference of the form ^{type}.
    if ((ret = peel_type(spec, len)))
        return ret;

    // Shorthand of ^ for ^{job}.
    if ((ret = peel_type_job(spec, len)))
        return ret;

    // Production from invocation step of the form ={step}.
    if ((ret = peel_step_name(spec, len)))
        return ret;

    // Production from invocation step by = optionally followed by a number.
    if ((ret = peel_step_pos(spec, len)))
        return ret;

    // Chain of invocations by ~ optionally followed by a number.
    if ((ret = peel_chain(spec, len)))
        return ret;

    if (len == KNIT_HASH_HEXSZ && !hex_to_oid(spec, &oid))
        return get_object(&oid, typesig);

    if (len == 1 && spec[0] == '@')
        return production_history_last();

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
    if (ret->typesig != typesig)
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
