#include "hash.h"
#include "invocation.h"
#include "job.h"
#include "production.h"
#include "spec.h"

static void pretty_job(const struct job* job) {
    for (struct resource_list* in = job->inputs; in; in = in->next)
        printf("%s\t%s\n", oid_to_hex(&in->res->object.oid), in->name);
}

static void pretty_production(const struct production* prd) {
    printf("job %s\n", oid_to_hex(&prd->job->object.oid));
    if (prd->inv)
        printf("invocation %s\n", oid_to_hex(&prd->inv->object.oid));
    printf("\n");
    for (struct resource_list* out = prd->outputs; out; out = out->next)
        printf("%s\t%s\n", oid_to_hex(&out->res->object.oid), out->name);
}

static void die_usage(char* arg0) {
    fprintf(stderr, "usage: %s <type> <object>\n", arg0);
    fprintf(stderr, "       %s (-p | -t) <object>\n", arg0);
    exit(1);
}

int main(int argc, char** argv) {
    if (argc != 3)
        die_usage(argv[0]);

    struct object* obj = peel_spec(argv[2], strlen(argv[2]));
    if (!obj)
        die("invalid spec %s", argv[2]);

    if (!strcmp(argv[1], "-p")) {
        if (obj->typesig == OBJ_RESOURCE || obj->typesig == OBJ_INVOCATION) {
            // Note we will read the object from storage again below. This is
            // probably fine because pretty is normally for interactive use.
            // fall through
        } else if (obj->typesig == OBJ_JOB) {
            struct job* job = (struct job*)obj;
            if (parse_job(job) < 0)
                exit(1);
            pretty_job(job);
            return 0;
        } else if (obj->typesig == OBJ_PRODUCTION) {
            struct production* prd = (struct production*)obj;
            if (parse_production(prd) < 0)
                exit(1);
            pretty_production(prd);
            return 0;
        } else {
            die("don't know how to pretty print %s", strtypesig(obj->typesig));
        }
    } else if (!strcmp(argv[1], "-t")) {
        puts(strtypesig(obj->typesig));
        return 0;
    } else if (obj->typesig != make_typesig(argv[1])) {
        die("object %s is type %s, expected %s", oid_to_hex(&obj->oid),
            strtypesig(obj->typesig), argv[1]);
    }

    size_t size;
    char* buf = read_object_of_type(&obj->oid, obj->typesig, &size);
    if (!buf)
        exit(1);

    char* end = buf + size;
    while (buf < end) {
        int nwritten = xwrite(STDOUT_FILENO, buf, end - buf);
        if (nwritten < 0)
            die_errno("write failed");
        buf += nwritten;
    }

    return 0;
}
