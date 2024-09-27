#include "job.h"
#include "resource.h"
#include "util.h"
#include "spec.h"

static char* joindir(const char* dir, const char* suffix) {
    static char buf[PATH_MAX];
    if (snprintf(buf, PATH_MAX, "%s/%s", dir, suffix) >= PATH_MAX)
        die("path too long: %s/%s", dir, suffix);
    return buf;
}

static void mkdir_or_die(const char* dir) {
    if (mkdir(dir, 0777) < 0)
        die_errno("cannot mkdir %s", dir);
}

static int mkparents(const char* dir, const char* filename) {
    char subdir[PATH_MAX];
    for (int i = 0; filename[i] != '\0'; i++) {
        if (i >= PATH_MAX)
            return error("path too long: %s/%s", dir, filename);
        if (filename[i] == '/') {
            subdir[i] = '\0';
            if (mkdir(joindir(dir, subdir), 0777) < 0 && errno != EEXIST)
                return error_errno("cannot mkdir %s/%s", dir, subdir);
        }
        subdir[i] = filename[i];
    }
    return 0;
}

static int write_in_full(int fd, const char* buf, size_t size) {
    size_t offset = 0;
    while (offset < size) {
        int n = xwrite(fd, buf + offset, size - offset);
        if (n < 0)
            return -1;
        offset += n;
    }
    return 0;
}

static int write_environ(int fd, const char* name, const char* buf, size_t size) {
    if (memchr(buf, '\0', size))
        die("NUL in environment variable %s", name);
    if (write_in_full(fd, name, strlen(name)) < 0 ||
        write_in_full(fd, "=", 1) < 0 ||
        write_in_full(fd, buf, size) < 0 ||
        write_in_full(fd, "", 1) < 0)
        die_errno("write failed to environ");
    return 0;
}

static int write_input(const char* inputs_dir, const char* name,
                       const char* buf, size_t size) {
    if (mkparents(inputs_dir, name) < 0)
        return -1;
    int fd = creat(joindir(inputs_dir, name), 0666);
    if (fd < 0)
        return error_errno("cannot open %s/%s", inputs_dir, name);
    if (write_in_full(fd, buf, size) < 0)
        return error_errno("write failed %s/%s", inputs_dir, name);
    if (close(fd) < 0)
        return error_errno("close failed %s/%s", inputs_dir, name);
    return 0;
}

static void die_usage(char* arg0) {
    fprintf(stderr, "usage: %s --scratch <job> <dir>\n", arg0);
    exit(1);
}

int main(int argc, char** argv) {
    if (argc != 4 || strcmp(argv[1], "--scratch"))
        die_usage(argv[0]);

    struct job* job = peel_job(argv[2]);
    if (!job || parse_job(job) < 0)
        exit(1);
    char* dir = argv[3];

    mkdir_or_die(dir);
    mkdir_or_die(joindir(dir, "work"));
    mkdir_or_die(joindir(dir, "work/out"));
    mkdir_or_die(joindir(dir, "out.knit"));

    char inputs_dir[PATH_MAX];
    if (snprintf(inputs_dir, PATH_MAX, "%s/%s", dir, "work/in") >= PATH_MAX)
        die("path too long: %s/work/in", dir);
    mkdir_or_die(inputs_dir);

    int env_fd = -1;
    for (struct resource_list* input = job->inputs; input; input = input->next) {
        size_t size;
        char* buf = read_object_of_type(&input->res->object.oid, OBJ_RESOURCE, &size);
        if (!buf)
            exit(1);

        if (*input->name == '$') {
            if (env_fd == -1) {
                env_fd = creat(joindir(dir, "environ"), 0666);
                if (env_fd < 0)
                    die_errno("cannot open %s/environ", dir);
            }

            if (write_environ(env_fd, input->name + 1, buf, size))
                exit(1);
        } else {
            if (write_input(inputs_dir, input->name, buf, size) < 0)
                exit(1);
        }

        free(buf);
    }
    if (env_fd >= 0 && close(env_fd) < 0)
        die_errno("close failed %s/environ", dir);
}
