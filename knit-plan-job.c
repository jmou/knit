#include "job.h"
#include "resource.h"

#include <limits.h>

#define MAX_ARGS 512

struct param_arg {
    char* name;
    struct resource* res;
};

struct input_line {
    char* param;
    char* filename;
};

static struct input_line* parse_lines(char* buf, size_t size, size_t* num_lines) {
    char* p = buf;
    char* end = buf + size;
    *num_lines = 0;
    while ((p = memchr(p, '\n', end - p))) {
        (*num_lines)++;
        p++;
    }

    struct input_line* lines = xmalloc(*num_lines * sizeof(*lines));
    memset(lines, 0, *num_lines * sizeof(*lines));
    p = buf;
    for (size_t i = 0; i < *num_lines; i++) {
        struct input_line* line = &lines[i];
        char* nl = memchr(p, '\n', end - p);
        if (!memcmp(p, "param ", 6)) {
            p += 6;
            line->param = p;
            line->filename = memchr(p, '=', nl - p);
            if (line->filename)
                line->filename++;
        } else if (!memcmp(p, "file ", 5)) {
            line->filename = p + 5;
        }
        *nl = '\0';
        p = nl + 1;
    }

    return lines;
}

static char* joindir(const char* dir, const char* filename) {
    static char buf[PATH_MAX];
    if (snprintf(buf, PATH_MAX, "%s/%s", dir, filename) >= PATH_MAX)
        die("path too long");
    return buf;
}

static int add_param_arg(const struct param_arg* arg,
                         struct input_line* lines, size_t num_lines,
                         struct resource_list** inputs_p) {
    size_t j;
    for (j = 0; j < num_lines; j++) {
        if (!strcmp(lines[j].param, arg->name))
            break;
    }
    if (j == num_lines)
        return error("plan does not declare param %s", arg->name);
    // If a param with a default file value is overridden, no longer include
    // the file unless it is referenced elsewhere.
    lines[j].filename = NULL;

    struct resource_list* inserted =
        resource_list_insert(inputs_p, joindir("params", arg->name), arg->res);
    if (inserted->next && !strcmp(inserted->name, inserted->next->name))
        return error("duplicate param %s", arg->name);
    return 0;
}

static int add_file(const char* files_dir, const char* filename,
                    struct resource_list** inputs_p) {
    struct resource* res =
        store_resource_file(files_dir ? joindir(files_dir, filename) : filename);
    if (!res)
        return -1;
    struct resource_list* inserted =
        resource_list_insert(inputs_p, joindir("files", filename), res);
    // The flow plan may include distinct references to the same file;
    // dedupe them here.
    if (inserted->next && !strcmp(inserted->name, inserted->next->name)) {
        assert(inserted->res == inserted->next->res);
        struct resource_list* next = inserted->next->next;
        free(inserted->next);
        inserted->next = next;
    }
    return 0;
}

static void die_usage(const char* arg0) {
    fprintf(stderr, "usage: %s [-F <files-dir>] [(-p|-P) <param>=<value>]... <plan>\n", arg0);
    exit(1);
}

int main(int argc, char** argv) {
    const char* files_dir = NULL;
    struct param_arg args[MAX_ARGS];
    size_t num_args = 0;

    int opt;
    while ((opt = getopt(argc, argv, "F:p:P:")) != -1) {
        char* value;
        struct param_arg* arg;
        switch (opt) {
        case 'F':
            files_dir = optarg;
            break;

        case 'p':
        case 'P':
            if (num_args == MAX_ARGS)
                die("too many param args");
            arg = &args[num_args++];

            if (!(value = strpbrk(optarg, "=")))
                die("missing = after param name");
            *value++ = '\0';

            arg->name = optarg;
            if (opt == 'p')
                arg->res = store_resource(value, strlen(value));
            else
                arg->res = store_resource_file(value);
            if (!arg->res)
                exit(1);
            break;

        default:
            die_usage(argv[0]);
        }
    }
    if (argc != optind + 1)
        die_usage(argv[0]);
    const char* plan_filename = argv[optind];

    struct resource_list* inputs = NULL;
    struct resource* plan_res = store_resource_file(plan_filename);
    if (!plan_res)
        exit(1);
    resource_list_insert(&inputs, JOB_INPUT_FLOW, plan_res);

    struct bytebuf bb;
    if (slurp_fd(STDIN_FILENO, &bb) < 0)
        die("cannot read stdin: %s", strerror(errno));
    size_t num_lines;
    struct input_line* lines = parse_lines(bb.data, bb.size, &num_lines);

    for (size_t i = 0; i < num_args; i++) {
        if (add_param_arg(&args[i], lines, num_lines, &inputs) < 0)
            exit(1);
    }

    for (size_t i = 0; i < num_lines; i++) {
        char* name = lines[i].filename;
        if (!name)
            continue;
        if (add_file(files_dir, name, &inputs) < 0)
            exit(1);
    }

    struct job* job = store_job(inputs);
    if (!job)
        exit(1);

    puts(oid_to_hex(&job->object.oid));
    // leak bb and lines
    return 0;
}
