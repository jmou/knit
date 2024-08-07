#include "hash.h"

static void die_usage(char* arg0) {
    fprintf(stderr, "usage: %s <type> <object>\n", arg0);
    exit(1);
}

static int pretty_production(char* data, size_t size) {
    struct object_id oid;
    char* p = data;
    if (size < KNIT_HASH_RAWSZ)
        return -1;
    memcpy(oid.hash, p, KNIT_HASH_RAWSZ);
    p += KNIT_HASH_RAWSZ;
    printf("job %s\n\n", oid_to_hex(&oid));
    while (p != data + size) {
        char* name = p;
        int namelen = strnlen(name, size - (p - data));
        p += namelen + 1;
        if (p + KNIT_HASH_RAWSZ > data + size)
            return -1;
        memcpy(oid.hash, p, KNIT_HASH_RAWSZ);
        p += KNIT_HASH_RAWSZ;
        printf("%s\t%.*s\n", oid_to_hex(&oid), namelen, name);
    }
    return 0;
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
        if (pretty_production(data, size) < 0)
            die("bad production");
    } else {
        die("don't know how to pretty print %s", type);
    }

    return 0;
}
