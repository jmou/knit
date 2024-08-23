#include "job.h"
#include "production.h"

static void pretty_job(const struct job* job) {
    for (struct resource_list* in = job->inputs; in; in = in->next)
        printf("%s\t%s\n", oid_to_hex(&in->res->object.oid), in->name);
}

static void pretty_production(const struct production* prd) {
    printf("job %s\n\n", oid_to_hex(&prd->job->object.oid));
    for (struct resource_list* out = prd->outputs; out; out = out->next)
        printf("%s\t%s\n", oid_to_hex(&out->res->object.oid), out->name);
}

static void die_usage(char* arg0) {
    fprintf(stderr, "usage: %s <type> <object>\n", arg0);
    exit(1);
}

int main(int argc, char** argv) {
    if (argc != 3)
        die_usage(argv[0]);

    int pretty = !strcmp(argv[1], "-p");
    struct object_id oid;
    if (hex_to_oid(argv[2], &oid) < 0)
        die("invalid object id");

    size_t size;
    char* buf = NULL; // leaked
    if (pretty) {
        uint32_t typesig;
        buf = read_object(&oid, &typesig, &size);
        if (!buf) {
            exit(1);
        } else if (typesig == OBJ_RESOURCE || typesig == OBJ_INVOCATION) {
            // fall through
        } else if (typesig == OBJ_JOB) {
            struct job* job = get_job(&oid);
            if (parse_job_bytes(job, buf, size) < 0)
                exit(1);
            pretty_job(job);
            return 0;
        } else if (typesig == OBJ_PRODUCTION) {
            struct production* prd = get_production(&oid);
            if (parse_production_bytes(prd, buf, size) < 0)
                exit(1);
            pretty_production(prd);
            return 0;
        } else {
            die("don't know how to pretty print %s", strtypesig(typesig));
        }
    }

    if (!buf) {
        assert(!pretty);
        buf = read_object_of_type(&oid, make_typesig(argv[1]), &size);
        if (!buf)
            exit(1);
    }

    char* end = buf + size;
    while (buf < end) {
        int nwritten = write(STDOUT_FILENO, buf, end - buf);
        if (nwritten < 0 && errno != EAGAIN && errno != EINTR)
            die("write failed: %s", strerror(errno));
        buf += nwritten;
    }

    return 0;
}
