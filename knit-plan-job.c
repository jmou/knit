#include "job.h"
#include "resource.h"

struct param_arg_list {
    struct param_arg_list* next;
    char* name;
    char* value;
    unsigned is_path : 1;
};

struct input_line {
    char* param;
    char* filename;
    unsigned file_is_optional : 1;
    unsigned is_nocache : 1;
};

static struct resource* get_empty_resource() {
    static struct resource* empty_res = NULL;
    if (!empty_res)
        empty_res = store_resource(NULL, 0);
    return empty_res;
}

// Returns <0, 0, or >0 similar to strcmp. If path_or_dir ends in '/' we only
// compare against the prefix of filepath (that is, is filepath inside
// path_or_dir); otherwise we compare the entire string. This is particularly
// useful for matching against param names.
static int path_or_dir_cmp(const char* path_or_dir, const char* filepath) {
    size_t path_or_dir_len = strlen(path_or_dir);
    assert(path_or_dir_len > 0);
    return path_or_dir[path_or_dir_len - 1] == '/'
        ? strncmp(path_or_dir, filepath, path_or_dir_len)
        : strcmp(path_or_dir, filepath);
}

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
        } else if (!strcmp(p, "nocache")) {
            line->is_nocache = 1;
        } else {
            error("cannot parse line %s", p);
            return NULL;
        }
        p = nl + 1;
    }

    return lines;
}

static int add_param_arg(const struct param_arg_list* arg,
                         struct input_line* lines, size_t num_lines,
                         struct resource_list** inputs_p) {
    size_t j;
    for (j = 0; j < num_lines; j++) {
        if (lines[j].param && !path_or_dir_cmp(lines[j].param, arg->name))
            break;
    }
    if (j == num_lines)
        return error("plan does not declare param %s", arg->name);
    // If a param with a default file value is overridden, no longer include
    // the file unless it is referenced elsewhere.
    lines[j].filename = NULL;

    size_t name_len = strlen(arg->name);
    int is_prefix = arg->name[name_len - 1] == '/';

    if (arg->next && !path_or_dir_cmp(arg->name, arg->next->name))
        return error("overlapping params %s and %s",
                     arg->name, arg->next->name);

    struct resource* res;
    if (!arg->is_path) {
        if (is_prefix)
            return error("param ending in '/' should be a directory; use -P");
        res = store_resource(arg->value, strlen(arg->value));
    } else if (!is_prefix) {
        res = store_resource_file(arg->value);
    } else if (!strcmp(arg->value, "/")) {
        return error("cowardly refusing to store resources for directory %s",
                     arg->value);
    } else {
        int num_added =
            resource_list_insert_dir_files(inputs_p, arg->value, arg->name);
        if (num_added < 0)
            return error_errno("directory traversal failed on %s", arg->value);
        if (num_added == 0)
            warning("empty directory for param %s", arg->name);
        return 0;
    }
    if (!res)
        return -1;
    resource_list_insert(inputs_p, arg->name, res);

    return 0;
}

// Returns number of files added (this will be 1 for any non-directory) or -1 on
// error. Note that name may be empty to add basedir itself.
static int add_file(const char* name, const char* basedir,
                    struct resource_list** inputs_p) {
    char path[strlen(basedir) + strlen(name) + 1];
    stpcpy(stpcpy(path, basedir), name);

    size_t input_name_len = strlen(JOB_INPUT_FILES_PREFIX) + strlen(name);
    char input_name[input_name_len + 1];
    stpcpy(stpcpy(input_name, JOB_INPUT_FILES_PREFIX), name);

    // If a directory, add every file inside.
    if (input_name[input_name_len - 1] == '/') {
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
    fprintf(stderr, "usage: %s [(-p|-P) <param>=<value>]... <plan> < <params-files>\n", arg0);
    exit(1);
}

int main(int argc, char** argv) {
    struct param_arg_list* args = NULL;

    int opt;
    while ((opt = getopt(argc, argv, "p:P:")) != -1) {
        char* value;
        struct param_arg_list* arg;
        struct param_arg_list** list_p;
        switch (opt) {
        case 'p':
        case 'P':
            if (!(value = strpbrk(optarg, "=")))
                die("missing = after param name");
            *value++ = '\0';

            arg = xmalloc(sizeof(*arg));
            arg->name = optarg;
            if (!*arg->name)
                die("missing param name");
            arg->value = value;
            arg->is_path = opt == 'P';

            // Sorted insert into args.
            list_p = &args;
            while (*list_p && strcmp((*list_p)->name, arg->name) < 0)
                list_p = &(*list_p)->next;
            arg->next = *list_p;
            *list_p = arg;
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

    for (size_t i = 0; i < num_lines; i++) {
        if (lines[i].is_nocache) {
            resource_list_insert(&inputs, JOB_INPUT_NOCACHE, get_empty_resource());
            break;
        }
    }

    for (struct param_arg_list* arg = args; arg; arg = arg->next) {
        if (add_param_arg(arg, lines, num_lines, &inputs) < 0)
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
    // TODO dedupe earlier to avoid loading files more than once
    for (struct resource_list* curr = inputs; curr && curr->next; curr = curr->next) {
        while (!strcmp(curr->name, curr->next->name)) {
            assert(curr->res == curr->next->res);
            resource_list_remove_and_free(&curr->next);
        }
    }

    struct job* job = store_job(inputs);
    if (!job)
        exit(1);

    puts(oid_to_hex(&job->object.oid));
    // leak bb, lines, and inputs
    return 0;
}
