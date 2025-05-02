#include "cache.h"

#include "hash.h"
#include "util.h"

// TODO overlaps with object file functions in hash.c

static char* cache_path(const struct job* job) {
    const char* hex = oid_to_hex(&job->object.oid);
    static char filename[PATH_MAX];
    if (snprintf(filename, PATH_MAX, "%s/cache/%c%c/%s",
                 get_knit_dir(), hex[0], hex[1], &hex[2]) >= PATH_MAX)
        die("path too long");
    return filename;
}

int open_cache_file(const struct job* job) {
    if (job->is_nocache)
        return -1;
    int fd = open(cache_path(job), O_RDONLY);
    if (fd < 0 && errno != ENOENT)
        warning_errno("could not open cache file");
    return fd;
}

struct production* read_cache(const struct job* job) {
    int fd = open_cache_file(job);
    if (fd < 0)
        return NULL;

    char buf[KNIT_HASH_HEXSZ + 2];
    int rc;
    size_t len = 0;
    do {
        rc = read(fd, buf + len, sizeof(buf) - len);
        if (rc < 0 && errno == EINTR)
            continue;
        len += rc;
    } while (rc > 0 && len < sizeof(buf));
    if (rc < 0)
        warning_errno("read cache error");
    close(fd);

    struct object_id oid;
    if (len != KNIT_HASH_HEXSZ + 1 || buf[KNIT_HASH_HEXSZ] != '\n' ||
            hex_to_oid(buf, &oid) < 0) {
        warning("invalid cached production in %s", cache_path(job));
        return NULL;
    }

    return get_production(&oid);
}

static int mkstemp_mode(char* template, mode_t mode) {
    int fd = mkstemp(template);
    if (fd >= 0) {
        mode_t orig_mask = umask(0);
        umask(orig_mask);
        fchmod(fd, mode & ~orig_mask);
    }
    return fd;
}

static int move_temp_to_file(const char* tmpfile, const char* filename) {
    int ret = rename(tmpfile, filename);
    if (ret < 0 && errno == ENOENT) {
        // Retry after trying to create the cache subdirectory.
        char* dir = strrchr(filename, '/');
        *dir = '\0';
        mkdir(filename, 0777);
        *dir = '/';
        return rename(tmpfile, filename);
    }
    return ret;
}

int write_cache(const struct job* job, const struct production* prd) {
    if (job->is_nocache)
        return 0;

    char tmpfile[PATH_MAX];
    if (snprintf(tmpfile, PATH_MAX, "%s/tmp-XXXXXX", get_knit_dir()) >= PATH_MAX)
        return error("path too long");
    int fd = mkstemp_mode(tmpfile, 0666);
    if (fd < 0)
        return error_errno("cannot open cache temp file");
    if (dprintf(fd, "%s\n", oid_to_hex(&prd->object.oid)) < 0)
        return error_errno("write failed");
    if (close(fd) < 0)
        return error_errno("close failed");

    if (move_temp_to_file(tmpfile, cache_path(job)) < 0)
        return error_errno("failed to rename cache file");

    return 0;
}
