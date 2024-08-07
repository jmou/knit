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

    const char* hex = argv[2];
    if (!is_valid_hex_oid(hex))
        die("invalid object id");

    struct bytebuf bbuf;
    char type[TYPE_MAX];
    size_t hdr_len;
    if (read_object_hex(hex, &bbuf, type, &hdr_len) < 0)
        exit(1);

    if (!pretty && strcmp(type, argv[1]))
        die("type mismatch %s != %s", type, argv[1]);

    if (!pretty) {
        char* ptr = bbuf.data + hdr_len;
        char* limit = bbuf.data + bbuf.size;
        while (ptr < limit) {
            int nwritten = write(1, ptr, limit - ptr);
            if (nwritten < 0 && errno != EAGAIN && errno != EINTR)
                die("write failed: %s", strerror(errno));
            ptr += nwritten;
        }
    } else if (!strcmp(type, "production")) {
        if (pretty_production(bbuf.data + hdr_len, bbuf.size - hdr_len) < 0)
            die("bad production");
    } else {
        die("don't know how to pretty print %s", type);
    }

    return 0;
}
