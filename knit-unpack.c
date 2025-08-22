#include "hash.h"
#include "job.h"
#include "production.h"
#include "resource.h"
#include "spec.h"
#include "util.h"

#include <getopt.h>

static int mkparents(const char* path) {
    char subdir[strlen(path) + 1];
    strcpy(subdir, path);
    for (char* p = subdir; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(subdir, 0777) < 0 && errno != EEXIST)
                return error_errno("cannot mkdir %s", subdir);
            *p = '/';
        }
    }
    return 0;
}

static int write_environ(int fd, const char* name, const char* buf, size_t size) {
    if (memchr(buf, '\0', size))
        die("NUL in environment variable %s", name);
    if (write_fully(fd, name, strlen(name)) < 0 ||
        write_fully(fd, "=", 1) < 0 ||
        write_fully(fd, buf, size) < 0 ||
        write_fully(fd, "", 1) < 0)
        die_errno("write failed to environ");
    return 0;
}

static int write_file(const char* path, const char* buf, size_t size) {
    if (mkparents(path) < 0)
        return -1;
    int fd = creat(path, 0666);
    if (fd < 0)
        return error_errno("cannot open %s", path);
    if (write_fully(fd, buf, size) < 0)
        return error_errno("write failed %s", path);
    if (close(fd) < 0)
        return error_errno("close failed %s", path);
    return 0;
}

// path is the full output path including basedir. Remove the prefix from path
// (after basedir) and return 1 iff this path should be output.
static int transform_path(char* path, const char* basedir,
                          const char* remove_prefix) {
    if (!remove_prefix)
        return 1;

    size_t basedir_len = strlen(basedir);
    if (strncmp(path, basedir, basedir_len) || path[basedir_len] != '/')
        return 0;

    path += basedir_len + 1;
    if (!strncmp(path, remove_prefix, strlen(remove_prefix))) {
        char* suffix = path + strlen(remove_prefix);
        memmove(path, suffix, strlen(suffix) + 1);  // include terminating NUL
        return 1;
    }
    return 0;
}

enum options {
    OPT_REMOVE_PREFIX,
};

static struct option longopts[] = {
    { .name = "remove-prefix", .val = OPT_REMOVE_PREFIX, .has_arg = 1 },
    { }
};

static void die_usage(char* arg0) {
    fprintf(stderr, "usage: %s [--remove-prefix <prefix>] <type> <object> <dir>\n", arg0);
    exit(1);
}

int main(int argc, char** argv) {
    const char* remove_prefix = NULL;

    int opt;
    while ((opt = getopt_long(argc, argv, "", longopts, NULL)) != -1) {
        switch (opt) {
        case OPT_REMOVE_PREFIX:
            remove_prefix = optarg;
            break;
        default:
            die_usage(argv[0]);
        }
    }
    if (optind + 3 != argc)
        die_usage(argv[0]);

    uint32_t typesig = make_typesig(argv[optind]);
    char* object = argv[optind + 1];
    char* dir = argv[optind + 2];

    char* res_dir;
    struct resource_list* list;

    if (typesig == OBJ_JOB) {
        res_dir = "in";
        struct job* job = peel_job(object);
        if (!job || parse_job(job) < 0)
            exit(1);
        list = job->inputs;
    } else if (typesig == OBJ_PRODUCTION) {
        res_dir = "out";
        struct production* prd = peel_production(object);
        if (!prd || parse_production(prd) < 0)
            exit(1);
        list = prd->outputs;
    } else {
        die("cannot unpack %s", strtypesig(typesig));
    }


    if (mkdir(dir, 0777) < 0)
        die_errno("cannot mkdir %s", dir);

    int env_fd = -1;
    for (; list; list = list->next) {
        size_t size;
        char* buf = read_object_of_type(&list->res->object.oid, OBJ_RESOURCE, &size);
        if (!buf)
            exit(1);

        if (typesig == OBJ_JOB && *list->name == '$') {
            char path[strlen(dir) + strlen("/environ") + 1];
            stpcpy(stpcpy(path, dir), "/environ");

            if (!transform_path(path, dir, remove_prefix))
                continue;

            if (env_fd == -1) {
                env_fd = creat(path, 0666);
                if (env_fd < 0)
                    die_errno("cannot open %s", path);
            }

            if (write_environ(env_fd, list->name + 1, buf, size))
                exit(1);
        } else {
            char path[PATH_MAX];
            if (snprintf(path, PATH_MAX, "%s/%s/%s",
                         dir, res_dir, list->name) >= PATH_MAX)
                die("path too long: %s/%s/%s", dir, res_dir, list->name);

            if (!transform_path(path, dir, remove_prefix))
                continue;

            if (write_file(path, buf, size) < 0)
                exit(1);
        }

        free(buf);
    }
    if (env_fd >= 0 && close(env_fd) < 0)
        die_errno("close failed %s/environ", dir);
}
