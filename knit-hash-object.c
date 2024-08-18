#include <getopt.h>

#include "hash.h"

static void die_usage(const char* arg0) {
    fprintf(stderr, "usage: %s -t <type> -w [--stdin] <file>...\n", arg0);
    exit(1);
}

int main(int argc, char** argv) {
    const char* type = NULL;
    int should_write = 0;
    int read_stdin = 0;

    struct option longopts[2] = {
        {
            .name = "stdin",
            .has_arg = 0,
            .flag = &read_stdin,
            .val = 1,
        },
        { 0 }
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "t:w", longopts, NULL)) != -1) {
        switch (opt) {
        case 0:
            break;
        case 't':
            type = optarg;
            break;
        case 'w':
            should_write = 1;
            break;
        default:
            die_usage(argv[0]);
        }
    }
    if ((optind == argc && !read_stdin) ||
            type == NULL ||
            !should_write)
        die_usage(argv[0]);

    int rc = 0;
    for (int i = optind - (read_stdin ? 1 : 0); i < argc; i++) {
        struct bytebuf bbuf;
        if (read_stdin) {
            read_stdin = 0;
            if (slurp_fd(0, &bbuf) < 0) {
                rc = 1;
                continue;
            }
        } else if (mmap_file(argv[i], &bbuf) < 0) {
            rc = 1;
            continue;
        }

        struct object_id oid;
        if (write_object(make_typesig(type), bbuf.data, bbuf.size, &oid) < 0) {
            rc = 1;
        } else {
            puts(oid_to_hex(&oid));
        }
        cleanup_bytebuf(&bbuf);
    }
    return rc;
}
