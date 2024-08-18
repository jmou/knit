#include "hash.h"

// The "modern" EVP API is clunky. We probably won't use it but could vendor an
// implementation of SHA-256 to avoid this API deprecation.
#define OPENSSL_SUPPRESS_DEPRECATED
#include <openssl/sha.h>

struct object_id* oid_of_hash(uint8_t* hash) {
    static struct object_id oid;
    memcpy(oid.hash, hash, KNIT_HASH_RAWSZ);
    return &oid;
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

const char* oid_to_hex(const struct object_id* oid) {
    const char HEX_DIGITS[] = "0123456789abcdef";
    static char bufs[4][KNIT_HASH_HEXSZ + 1];
    static int i;
    i = (i + 1) % 4;
    char* hex = bufs[i];
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
    strncpy((char*)&u32, type, 4); // pad NUL to 4 bytes
    return ntohl(u32);
}

char* strtypesig(uint32_t typesig) {
    static char bufs[4][5];
    static int i;
    i = (i + 1) % 4;
    char* buf = bufs[i];
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

void* read_object(const struct object_id* oid, uint32_t* typesig, size_t* size) {
    void* ret = NULL;
    struct bytebuf bb;
    if (mmap_file(object_path(oid), &bb) < 0)
        return NULL;

    struct object_header* hdr = bb.data;
    if (bb.size < sizeof(*hdr)) {
        error("truncated header");
        goto cleanup;
    }

    *typesig = ntohl(hdr->typesig);
    size_t nrem = ntohl(hdr->size);
    if (bb.size != sizeof(*hdr) + nrem) {
        error("size mismatch");
        goto cleanup;
    }

    // TODO decompress
    *size = bb.size - sizeof(*hdr);
    ret = xmalloc(*size);
    memcpy(ret, bb.data + sizeof(*hdr), *size);

cleanup:
    cleanup_bytebuf(&bb);
    return ret;
}

void* read_object_of_type(const struct object_id* oid, uint32_t typesig, size_t* size) {
    uint32_t actual_typesig;
    void* buf = read_object(oid, &actual_typesig, size);
    if (buf && actual_typesig != typesig) {
        free(buf);
        error("object %s is type %s, expected %s", oid_to_hex(oid),
            strtypesig(actual_typesig), strtypesig(typesig));
        return NULL;
    }
    return buf;
}
