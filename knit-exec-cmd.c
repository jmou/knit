#include "util.h"

static void putenv_buf(char* buf, size_t size) {
    char* end = buf + size;
    while (buf < end) {
        putenv(buf);
        while (*buf++)
            continue;
    }
}

static char** parse_args(char* buf, size_t size) {
    char** args = NULL;
    size_t alloc_args = 0;
    size_t num_args = 0;

    char* p = buf;
    char* end = p + size;
    do {
        if (num_args + 1 >= alloc_args) {
            alloc_args = (alloc_args + 16) * 2;
            args = xrealloc(args, alloc_args * sizeof(char*));
        }
        args[num_args++] = p;
        p = memchr(p, '\0', end + 1 - p);
        assert(p);
        p++;
    } while (p < end);
    args[num_args] = NULL;
    return args;
}

static void die_usage(char* arg0) {
    fprintf(stderr, "usage: %s <scratch-dir> <cmdfile>\n", arg0);
    exit(1);
}

int main(int argc, char** argv) {
    if (argc != 3)
        die_usage(argv[0]);

    struct bytebuf bb;
    if (slurp_file(argv[2], &bb) < 0)
        exit(1);
    ensure_bytebuf_null_terminated(&bb);

    char path[PATH_MAX];
    if (snprintf(path, PATH_MAX, "%s/environ", argv[1]) >= PATH_MAX)
        die("path too long");
    int env_fd = open(path, O_RDONLY);
    if (env_fd >= 0) {
        struct bytebuf bb; // never deallocated
        if (slurp_fd(env_fd, &bb) < 0)
            die_errno("cannot read %s", path);
        if (*((char*)bb.data + bb.size - 1) != '\0')
            die("unterminated environ %s", path);
        putenv_buf(bb.data, bb.size);
    } else if (errno != ENOENT) {
        die_errno("cannot open %s", path);
    }

    if (snprintf(path, PATH_MAX, "%s/work", argv[1]) >= PATH_MAX)
        die("path too long");
    if (chdir(path) < 0) {
        perror("chdir");
        exit(1);
    }

    close(STDIN_FILENO);
    int stderr_fd = fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, 0);
    if (stderr_fd < 0 || dup2(STDOUT_FILENO, STDERR_FILENO) < 0) {
        perror("fcntl/dup2");
        exit(1);
    }

    char** args = parse_args(bb.data, bb.size);
    execvp(args[0], args); // should not return
    dup2(stderr_fd, STDERR_FILENO);
    perror("exec");
    exit(1);
}
