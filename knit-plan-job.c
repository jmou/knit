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
    unsigned file_is_optional : 1;
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
        *nl = '\0';
        if (!strncmp(p, "param ", 6)) {
            line->param = p + 6;
            line->filename = strstr(p, "=./");
            if (line->filename) {
                *line->filename = '\0';
                line->filename += 3;
            }
            if (!*line->param) {
                error("missing param name");
                return NULL;
            }
        } else if (!strncmp(p, "file optional ./", 16)) {
            line->filename = p + 16;
            line->file_is_optional = 1;
        } else if (!strncmp(p, "file ./", 7)) {
            line->filename = p + 7;
        } else {
            error("cannot parse line %s", p);
            return NULL;
        }
        p = nl + 1;
    }

    return lines;
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

    char input_name[strlen(arg->name) + 8];
    stpcpy(stpcpy(input_name, "params/"), arg->name);
    struct resource_list* inserted =
        resource_list_insert(inputs_p, input_name, arg->res);
    if (inserted->next && !strcmp(inserted->name, inserted->next->name))
        return error("duplicate param %s", arg->name);
    return 0;
}

// Returns number of files added (this will be 1 for any non-directory) or -1 on
// error. Note that name may be empty to add basedir itself.
static int add_file(const char* name, const char* basedir,
                    struct resource_list** inputs_p) {
    char path[strlen(basedir) + strlen(name) + 1];
    stpcpy(stpcpy(path, basedir), name);

    size_t name_len = strlen(name);
    char input_name[name_len + 7];
    stpcpy(stpcpy(input_name, "files/"), name);

    // If a directory, add every file inside.
    if (name_len == 0 || name[name_len - 1] == '/') {
        int rc = resource_list_insert_dir_files(inputs_p, path, input_name);
        if (rc < 0)
            return error_errno("directory traversal failed on %s", path);
        return rc;
    }

    struct resource* res = store_resource_file(path);
    if (!res)
        return -1;
    resource_list_insert(inputs_p, input_name, res);
    return 1;
}

static void die_usage(const char* arg0) {
    fprintf(stderr, "usage: %s [(-p|-P) <param>=<value>]... <plan>\n", arg0);
    exit(1);
}

int main(int argc, char** argv) {
    struct param_arg args[MAX_ARGS];
    size_t num_args = 0;

    int opt;
    while ((opt = getopt(argc, argv, "p:P:")) != -1) {
        char* value;
        struct param_arg* arg;
        switch (opt) {
        case 'p':
        case 'P':
            if (num_args == MAX_ARGS)
                die("too many param args");
            arg = &args[num_args++];

            if (!(value = strpbrk(optarg, "=")))
                die("missing = after param name");
            *value++ = '\0';

            arg->name = optarg;
            if (!*arg->name)
                die("missing param name");

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
    char* plan_filename = argv[optind];

    struct resource_list* inputs = NULL;
    struct resource* plan_res = store_resource_file(plan_filename);
    if (!plan_res)
        exit(1);
    resource_list_insert(&inputs, JOB_INPUT_FLOW, plan_res);

    struct bytebuf bb;
    if (slurp_fd(STDIN_FILENO, &bb) < 0)
        die_errno("cannot read stdin");
    size_t num_lines;
    struct input_line* lines = parse_lines(bb.data, bb.size, &num_lines);
    if (!lines)
        exit(1);

    for (size_t i = 0; i < num_args; i++) {
        if (add_param_arg(&args[i], lines, num_lines, &inputs) < 0)
            exit(1);
    }

    // Look for files referenced by the flow plan relative to its own directory.
    char* basedir_end = strrchr(plan_filename, '/');
    if (basedir_end)
        *++basedir_end = '\0';
    const char* basedir = basedir_end ? plan_filename : "./";

    for (size_t i = 0; i < num_lines; i++) {
        char* name = lines[i].filename;
        if (!name)
            continue;
        int num_added = add_file(name, basedir, &inputs);
        if (num_added < 0)
            exit(1);
        if (num_added == 0 && !lines[i].file_is_optional)
            die("no files in %s", lines[i].filename);
    }

    // The flow plan may include distinct references to the same file; dedupe
    // them here.
    for (struct resource_list* curr = inputs; curr && curr->next; curr = curr->next) {
        if (!strcmp(curr->name, curr->next->name)) {
            assert(curr->res == curr->next->res);
            resource_list_remove_and_free(&curr->next);
        }
    }

    struct job* job = store_job(inputs);
    if (!job)
        exit(1);

    puts(oid_to_hex(&job->object.oid));
    // leak bb and lines
    return 0;
}
