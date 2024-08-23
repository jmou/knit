#include "hash.h"
#include "object.h"
#include "invocation.h"
#include "production.h"

static struct object* peel_spec(char* spec, size_t len);

// TODO refactor w/ cat-file
static struct object* get_object(const struct object_id* oid) {
    struct object* ret = NULL;
    uint32_t typesig;
    size_t size;
    void* buf = read_object(oid, &typesig, &size);
    if (!buf)
        return NULL;
    if (typesig == OBJ_RESOURCE) {
        struct resource* res = get_resource(oid);
        if (!parse_resource(res))
            ret = &res->object;
    } else if (typesig == OBJ_JOB) {
        struct job* job = get_job(oid);
        if (!parse_job_bytes(job, buf, size))
            ret = &job->object;
    } else if (typesig == OBJ_PRODUCTION) {
        struct production* prd = get_production(oid);
        if (!parse_production_bytes(prd, buf, size))
            ret = &prd->object;
    } else if (typesig == OBJ_INVOCATION) {
        struct invocation* inv = get_invocation(oid);
        if (!parse_invocation_bytes(inv, buf, size))
            ret = &inv->object;
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
        // TODO how to handle missing production?
        if (!inv->terminal->prd || parse_production(inv->terminal->prd) < 0)
            return NULL;
        return &inv->terminal->prd->object;
    } else if (obj->typesig == OBJ_PRODUCTION) {
        struct production* prd = (struct production*)obj;
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

static struct object* peel_deref(char* spec, size_t len) {
    // Try to peel a dereference of the form ^{type}.
    if (len < 4 || spec[len - 1] != '}')
        return NULL;

    char* p;
    for (p = &spec[len - 3]; p > spec; p--)
        if (p[0] == '^' && p[1] == '{')
            break;

    if (p == spec)
        return NULL;

    size_t inner_len = p - spec;
    p += 2;
    // Icky NUL termination.
    spec[len - 1] = '\0';
    uint32_t typesig = make_typesig(p);

    struct object* inner = peel_spec(spec, inner_len);
    if (!inner)
        return NULL;
    return deref_type(inner, typesig);
}

static struct object* peel_spec(char* spec, size_t len) {
    struct object* ret;
    struct object_id oid;

    // TODO peel :path from job or output

    if ((ret = peel_deref(spec, len)))
        return ret;

    if (!hex_to_oid(spec, &oid))
        return get_object(&oid);

    return NULL;
}

static void die_usage(char* arg0) {
    fprintf(stderr, "usage: %s <spec>", arg0);
    exit(1);
}

int main(int argc, char** argv) {
    if (argc != 2)
        die_usage(argv[0]);
    struct object* obj = peel_spec(argv[1], strlen(argv[1]));
    // TODO error is ambiguous: bad spec, I/O error, non-existent deref
    if (!obj)
        die("could not parse object specifier");
    puts(oid_to_hex(&obj->oid));
    return 0;
}
