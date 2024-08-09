#include "production.h"

static void pretty_production(const struct production* prd) {
    printf("job %s\n\n", oid_to_hex(&prd->job_oid));
    for (struct resource_list* out = prd->outputs; out; out = out->next)
        printf("%s\t%s\n", oid_to_hex(&out->oid), out->path);
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

    struct bytebuf bbuf;
    uint32_t typesig;
    if (read_object(&oid, &bbuf, &typesig) < 0)
        exit(1);
    char* data = bbuf.data + OBJECT_HEADER_SIZE;
    size_t size = bbuf.size - OBJECT_HEADER_SIZE;

    const char* type = strtypesig(typesig);
    if (!pretty && strcmp(type, argv[1]))
        die("type mismatch %s != %s", type, argv[1]);

    if (!pretty) {
        char* limit = data + size;
        while (data < limit) {
            int nwritten = write(1, data, limit - data);
            if (nwritten < 0 && errno != EAGAIN && errno != EINTR)
                die("write failed: %s", strerror(errno));
            data += nwritten;
        }
    } else if (typesig == TYPE_PRODUCTION) {
        struct production prd;
        if (parse_production_bytes(data, size, &prd) < 0)
            exit(1);
        pretty_production(&prd);
    } else {
        die("don't know how to pretty print %s", type);
    }

    return 0;
}
