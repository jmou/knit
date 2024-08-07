#include "hash.h"

#define TYPE_MAX 10

static int parse_header(const char* hdr, char* out_type, size_t* out_size) {
    int i = TYPE_MAX;
    while (1) {
        char c = *hdr++;
        if (c == ' ') {
            break;
        } else if (--i == 0 || c == '\0') {
            return -1;
        }
        *out_type++ = c;
    }
    *out_type = '\0';

    char* end;
    *out_size = strtoul(hdr, &end, 10);
    if (*end != '\0')
        return -1;
    return 0;
}

static void die_usage(char* arg0) {
    fprintf(stderr, "usage: %s <type> <object>\n", arg0);
    exit(1);
}

int main(int argc, char** argv) {
    if (argc != 3)
        die_usage(argv[0]);

    char filename[PATH_MAX];
    const char* hex = argv[2];
    if (!is_valid_hex_oid(hex))
        die("invalid object id");
    if (oid_path(hex, filename) < 0)
        return 1;

    struct bytebuf bbuf;
    if (mmap_file(filename, &bbuf) < 0)
        return 1;

    char type[TYPE_MAX];
    size_t hdr_len = strnlen(bbuf.data, bbuf.size) + 1;
    size_t nrem;
    if (hdr_len > bbuf.size || parse_header(bbuf.data, type, &nrem) < 0)
        die("invalid header");

    if (strcmp(type, argv[1]))
        die("type mismatch %s != %s", type, argv[1]);

    if (bbuf.size != hdr_len + nrem)
        die("size mismatch");

    char* ptr = bbuf.data + hdr_len;
    while (nrem > 0) {
        int nwritten = write(1, ptr, nrem);
        if (nwritten < 0 && errno != EAGAIN && errno != EINTR)
            die("write failed: %s", strerror(errno));
        nrem -= nwritten;
        ptr += nwritten;
    }

    return 0;
}
