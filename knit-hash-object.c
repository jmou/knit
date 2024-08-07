#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// The "modern" EVP API is clunky. We probably won't use it but could vendor an
// implementation of SHA-256 to avoid this API deprecation.
#define OPENSSL_SUPPRESS_DEPRECATED
#include <openssl/sha.h>

#include "hash.h"
#include "util.h"

static int move_temp_to_file(const char* tmpfile, const char* filename) {
    int ret = rename(tmpfile, filename);
    if (ret < 0 && errno == ENOENT) {
        // Retry after trying to create the object subdirectory.
        char* dir = strrchr(filename, '/');
        *dir = '\0';
        mkdir(filename, 0777);
        *dir = '/';
        return rename(tmpfile, filename);
    }
    return ret;
}

static int write_object(const char* type, void* data, size_t size,
                        struct object_id* out_oid) {
    char hdr[32];
    int hdr_size = snprintf(hdr, sizeof(hdr), "%s %zu", type, size) + 1;
    if (hdr_size >= (int)sizeof(hdr))
        die("header overflow");

    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, hdr, hdr_size);
    SHA256_Update(&ctx, data, size);
    SHA256_Final(out_oid->hash, &ctx);

    const char* hex = oid_to_hex(out_oid);
    char filename[PATH_MAX];
    if (snprintf(filename, PATH_MAX, "%s/objects/%c%c/%s",
                 get_knit_dir(), hex[0], hex[1], &hex[2]) >= PATH_MAX)
        return error("path overflow");

    struct stat st;
    if (stat(filename, &st) == 0 && st.st_size > 0)
        return 0; // object already exists

    char tmpfile[PATH_MAX];
    if (snprintf(tmpfile, PATH_MAX, "%s/tmp-XXXXXX", get_knit_dir()) >= PATH_MAX)
        return error("path overflow");
    int fd = mkstemp(tmpfile);
    if (fd < 0)
        return error("cannot open object temp file: %s", strerror(errno));
    // TODO compress
    if (write(fd, hdr, hdr_size) != hdr_size ||
        write(fd, data, size) != (ssize_t)size)
        return error("write failed");
    fchmod(fd, 0444);
    if (close(fd) < 0)
        return error("close failed: %s", strerror(errno));

    if (move_temp_to_file(tmpfile, filename) < 0)
        return error("failed to rename object file: %s", strerror(errno));

    return 0;
}

static void die_usage(const char* arg0) {
    fprintf(stderr, "usage: %s -t resource -w [--stdin] <file> ...\n", arg0);
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
    if ((optind == argc && !read_stdin) || type == NULL || !should_write)
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
        if (write_object(type, bbuf.data, bbuf.size, &oid) < 0) {
            rc = 1;
        } else {
            puts(oid_to_hex(&oid));
        }
        cleanup_bytebuf(&bbuf);
    }
    return rc;
}
