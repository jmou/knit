#include <errno.h>
#include <fcntl.h>
#include <linux/limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void die_usage(const char* arg0) {
    fprintf(stderr, "usage: %s <plan>\n", arg0);
    exit(1);
}

static void die(const char* format, ...) {
    va_list params;
    va_start(params, format);
    fprintf(stderr, "fatal: ");
    vfprintf(stderr, format, params);
    fprintf(stderr, "\n");
    va_end(params);
    exit(1);
}

static void mksubdir(const char* format, ...) {
    va_list params;
    va_start(params, format);
    char pathname[PATH_MAX];
    if (vsnprintf(pathname, PATH_MAX, format, params) >= PATH_MAX) {
        die("path overflow");
    }
    va_end(params);
    if (mkdir(pathname, 0777) < 0) {
        die("cannot initialize step: %s", strerror(errno));
    }
}

static void writefile(const char* pathname, const char* buf, size_t size) {
    int fd = creat(pathname, 0666);
    if (fd < 0) {
        die("open error %s: %s", pathname, strerror(errno));
    }
    if (write(fd, buf, size) != size) {
        die("write error %s: %s", pathname, strerror(errno));
    }
    if (close(fd) < 0) {
        die("close error %s: %s", pathname, strerror(errno));
    }
}

static void step_init(const char* dir, const char* step) {
    mksubdir("%s/inputs/%s", dir, step);
    mksubdir("%s/awaiting/%s", dir, step);
}

// TODO this should be a resource, but we don't yet have a way to define the sentinel resource
static void step_constant(const char* dir, const char* step,
                          const char* key, const char* value) {
    char pathname[PATH_MAX];
    if (snprintf(pathname, PATH_MAX,
                 "%s/inputs/%s/%s", dir, step, key) >= PATH_MAX) {
        die("path overflow");
    }
    writefile(pathname, value, strlen(value));
}

static void step_dependency(const char* dir, const char* step,
                            const char* key, const char* dep, const char* path) {
    char target[PATH_MAX];
    char linkpath[PATH_MAX];

    if (snprintf(target, PATH_MAX, "../../productions/%s", dep) >= PATH_MAX ||
        snprintf(linkpath, PATH_MAX, "%s/awaiting/%s/%s", dir, step, dep) >= PATH_MAX) {
        die("path overflow");
    }
    if (symlink(target, linkpath) < 0 && errno != EEXIST) {
        die("cannot declare dependency: %s", strerror(errno));
    }

    if (snprintf(target, PATH_MAX, "../../productions/%s/%s", dep, path) >= PATH_MAX ||
        snprintf(linkpath, PATH_MAX, "%s/inputs/%s/%s", dir, step, key) >= PATH_MAX) {
        die("path overflow");
    }
    if (symlink(target, linkpath) < 0) {
        die("cannot declare dependency: %s", strerror(errno));
    }
}

static void compile_plan(const char* plan, FILE* fd) {
    // TODO session directory
    char dir[] = "/tmp/knit-session.XXXXXX";
    if (!mkdtemp(dir)) {
        die("cannot make session directory: %s", strerror(errno));
    }
    mksubdir("%s/inputs", dir);
    mksubdir("%s/awaiting", dir);
    mksubdir("%s/resolved", dir);
    mksubdir("%s/productions", dir);

    // TODO hardcoded plan
    step_init(dir, "a");
    step_constant(dir, "a", "shell", "mkdir out/result.d && seq 1 3 > out/result.d/data\n");

    step_init(dir, "b");
    step_constant(dir, "b", "shell", "cp -RL in/input.d out/result.d\n");
    step_dependency(dir, "b", "a.ok.d", "a", ".knit/ok.d");
    step_dependency(dir, "b", "input.d", "a", "result.d");

    step_init(dir, "c");
    step_constant(dir, "c", "shell", "mkdir out/result.d && tac in/lines.d/data > out/result.d/data\n");
    step_dependency(dir, "c", "b.ok.d", "b", ".knit/ok.d");
    step_dependency(dir, "c", "lines.d", "b", "result.d");

    fprintf(fd, "%s\n", dir);
}

int main(int argc, char** argv) {
    if (argc != 2) {
        die_usage(argv[0]);
    }

    compile_plan(argv[1], stdout);

    return 0;
}
