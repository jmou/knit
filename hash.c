// The "modern" EVP API is clunky. We probably won't use it but could vendor an
// implementation of SHA-256 to avoid this API deprecation.
#define OPENSSL_SUPPRESS_DEPRECATED
#include <openssl/sha.h>

#include "hash.h"

void oidcpy(struct object_id* dst, const struct object_id* src) {
    memcpy(dst, src, sizeof(*dst));
}

int hex_to_oid(const char* hex, struct object_id* oid) {
    unsigned char byte = 0; // initialize to suppress warning
    for (int i = 0; i < KNIT_HASH_HEXSZ; i++) {
        char c = hex[i];
        if (!c)
            return -1;

        unsigned char nibble;
        if (c >= '0' && c <= '9') {
            nibble = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            nibble = c - 'a' + 10;
        } else {
            return -1;
        }

        if (i % 2 == 0) {
            byte = nibble << 4;
        } else {
            byte |= nibble;
            oid->hash[i / 2] = byte;
        }
    }
    return 0;
}

int is_valid_hex_oid(const char* hex) {
    struct object_id dummy;
    return hex_to_oid(hex, &dummy) == 0;
}

const char* oid_to_hex(const struct object_id* oid) {
    const char HEX_DIGITS[] = "0123456789abcdef";
    static char hex[KNIT_HASH_HEXSZ + 1];
    for (int i = 0; i < KNIT_HASH_RAWSZ; i++) {
        unsigned char byte = oid->hash[i];
        hex[2*i] = HEX_DIGITS[byte >> 4];
        hex[2*i + 1] = HEX_DIGITS[byte & 0xf];
    }
    hex[KNIT_HASH_HEXSZ] = '\0';
    return hex;
}

uint32_t make_typesig(const char* type) {
    uint32_t u32;
    memcpy(&u32, type, 4);
    return ntohl(u32);
}

char* strtypesig(uint32_t typesig) {
    static char buf[5];
    uint32_t be = htonl(typesig);
    memcpy(buf, &be, 4);
    buf[4] = '\0';
    return buf;
}

static char* object_path(const struct object_id* oid) {
    const char* hex = oid_to_hex(oid);
    static char filename[PATH_MAX];
    if (snprintf(filename, PATH_MAX, "%s/objects/%c%c/%s",
                 get_knit_dir(), hex[0], hex[1], &hex[2]) >= PATH_MAX)
        die("path too long");
    return filename;
}

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

struct object_header {
    uint32_t typesig;
    uint32_t size;
};

int write_object(uint32_t typesig, void* data, size_t size,
                 struct object_id* out_oid) {
    struct object_header hdr = {
        .typesig = ntohl(typesig),
        .size = ntohl(size),
    };

    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, &hdr, sizeof(hdr));
    SHA256_Update(&ctx, data, size);
    SHA256_Final(out_oid->hash, &ctx);

    const char* filename = object_path(out_oid);
    struct stat st;
    if (stat(filename, &st) == 0 && st.st_size > 0)
        return 0; // object already exists

    char tmpfile[PATH_MAX];
    if (snprintf(tmpfile, PATH_MAX, "%s/tmp-XXXXXX", get_knit_dir()) >= PATH_MAX)
        return error("path too long");
    int fd = mkstemp(tmpfile);
    if (fd < 0)
        return error("cannot open object temp file: %s", strerror(errno));
    // TODO compress
    if (write(fd, &hdr, sizeof(hdr)) != sizeof(hdr) ||
            write(fd, data, size) != (ssize_t)size)
        return error("write failed");
    fchmod(fd, 0444);
    if (close(fd) < 0)
        return error("close failed: %s", strerror(errno));

    if (move_temp_to_file(tmpfile, filename) < 0)
        return error("failed to rename object file: %s", strerror(errno));

    return 0;
}

int read_object(const struct object_id* oid,
                struct bytebuf* bbuf,
                uint32_t* typesig) {
    if (mmap_file(object_path(oid), bbuf) < 0)
        return -1;

    struct object_header* hdr = bbuf->data;
    if (bbuf->size < sizeof(*hdr))
        return error("truncated header");

    *typesig = ntohl(hdr->typesig);
    size_t nrem = ntohl(hdr->size);

    if (bbuf->size != sizeof(*hdr) + nrem)
        return error("size mismatch");

    return 0;
}
