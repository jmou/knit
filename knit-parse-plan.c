#include "hash.h"
#include "job.h"
#include "lexer.h"
#include "resource.h"

#include <stdalign.h>

enum value_tag {
    VALUE_DEPENDENCY,
    VALUE_FILENAME,
    VALUE_HOLE,
    VALUE_LITERAL,
};

struct value {
    enum value_tag tag;
    union {
        char* filename;
        struct {
            char* literal;
            size_t literal_len;
        };
        struct {
            char* dep_name;
            size_t dep_pos;
            char* dep_path;
            int dep_optional;
        };
    };
    struct resource* res;
};

struct input_list {
    char* name;
    struct value* val;
    struct input_list* next;
    unsigned is_param : 1;
};

struct step_list {
    char* name;
    ssize_t pos;
    struct input_list* inputs;
    struct step_list* next;
    unsigned is_params : 1;
};

static struct step_list* find_step(struct step_list* step, const char* name) {
    for (; step; step = step->next) {
        if (!strcmp(step->name, name))
            return step;
    }
    return NULL;
}

#define BUMP_PAGE_SIZE (1 << 20)

struct bump_list {
    char* base;
    size_t nused;
    struct bump_list* next;
};

void* bump_alloc(struct bump_list** bump_p, size_t size) {
    struct bump_list* bump = *bump_p;
    if (!*bump_p || (*bump_p)->nused + size < BUMP_PAGE_SIZE) {
        bump = xmalloc(sizeof(*bump));
        bump->base = xmalloc(BUMP_PAGE_SIZE > size ? BUMP_PAGE_SIZE : size);
        bump->nused = 0;
        bump->next = *bump_p;
        *bump_p = bump;
    }
    void* ret = bump->base + bump->nused;
    size_t align = alignof(max_align_t);
    bump->nused += (size + align - 1) / align * align;
    return ret;
}

static struct input_list* create_input(struct bump_list** bump_p, char* name) {
    struct input_list* input = bump_alloc(bump_p, sizeof(*input));
    memset(input, 0, sizeof(*input));
    input->name = name;
    input->val = xmalloc(sizeof(*input->val));
    return input;
}

static int input_list_insert(struct input_list** list_p, struct input_list* input) {
    int cmp = -1;
    while (*list_p && (cmp = strcmp((*list_p)->name, input->name)) < 0)
        list_p = &(*list_p)->next;
    if (cmp == 0)
        return error("duplicate input %s", input->name);
    input->next = *list_p;
    *list_p = input;
    return 0;
}

static struct input_list* input_list_override(struct bump_list** bump_p,
                                              struct input_list** inputs_p,
                                              struct input_list* override) {
    struct input_list* added = NULL;
    struct input_list* orig = *inputs_p;
    while (override && orig) {
        int cmp = strcmp(override->name, orig->name);
        if (cmp == 0) {
            *inputs_p = override;
            inputs_p = &override->next;
            orig = orig->next;
            override = override->next;
        } else if (cmp < 0) {
            added = override;
            *inputs_p = override;
            inputs_p = &override->next;
            override = override->next;
        } else {
            // Copy from the original inputs_p when we need to rewrite its next.
            struct input_list* copy = bump_alloc(bump_p, sizeof(*copy));
            memcpy(copy, orig, sizeof(*copy));
            *inputs_p = copy;
            inputs_p = &copy->next;
            orig = orig->next;
        }
    }
    if (override) {
        added = override;
        *inputs_p = override;
    } else {
        *inputs_p = orig;
    }
    return added;
}

struct parse_context {
    struct lex_input in;
    char* block_decl;
    struct step_list* plan;
    struct step_list* partials;
    struct bump_list** bump_p;
};

static struct lex_input saved_lex_input;
static void save_lex_input(const struct lex_input* in) {
    memcpy(&saved_lex_input, in, sizeof(*in));
}
static void load_lex_input(struct lex_input* in) {
    memcpy(in, &saved_lex_input, sizeof(*in));
}

static int try_read_token(struct lex_input* in, enum token expected) {
    save_lex_input(in);
    enum token actual = lex(in);
    if (actual != expected)
        load_lex_input(in);
    return actual == expected;
}

static int parse_value(struct parse_context* ctx, struct value* out) {
    struct lex_input* in = &ctx->in;
    memset(out, 0, sizeof(*out));

    ssize_t size;
    struct step_list* dep;
    switch (lex(in)) {
    case TOKEN_EXCLAMATION:
        out->tag = VALUE_HOLE;
        return 0;
    case TOKEN_QUOTE:
        out->tag = VALUE_LITERAL;
        size = lex_string_alloc(in);
        if (size == 0) {
            return -1;
        } else if (size < 0) {
            out->literal = in->curr;
            lex_string(in, NULL);
            out->literal_len = -size - 1;
        } else {
            out->literal = bump_alloc(ctx->bump_p, size);
            lex_string(in, out->literal);
            out->literal_len = size - 1;
        }
        if (lex(in) != TOKEN_QUOTE)
            die("quote should follow lex_string()");
        return 0;
    case TOKEN_IDENT:
        out->tag = VALUE_DEPENDENCY;
        out->dep_name = lex_stuff_null(in);
        if (lex(in) != TOKEN_COLON)
            return error("expected :");
        if (lex_path(in) < 0)
            return -1;
        out->dep_path = lex_stuff_null(in);
        dep = find_step(ctx->plan, out->dep_name);
        if (!dep)
            return error("dependency on not yet defined step %s", out->dep_name);
        out->dep_pos = dep->pos;
        return 0;
    case TOKEN_DOTSLASH:
        out->tag = VALUE_FILENAME;
        if (lex_path(in) < 0)
            return -1;
        out->filename = lex_stuff_null(in);
        return 0;
    default:
        return error("expected value");
    }
}

static int parse_process_partial(struct parse_context* ctx, struct step_list* step) {
    struct lex_input* in = &ctx->in;
    if (lex(in) != TOKEN_SPACE)
        return error("expected space");
    if (lex(in) != TOKEN_IDENT)
        return error("expected identifier");

    char* ident = lex_stuff_null(in);
    struct step_list* partial = find_step(ctx->partials, ident);
    if (!partial)
        return error("unknown partial %s", ident);

    step->inputs = partial->inputs;
    return 0;
}

static int parse_process(struct parse_context* ctx, struct step_list* step) {
    struct lex_input* in = &ctx->in;
    char* input_name;

    switch (lex_keyword(&ctx->in)) {
    case TOKEN_CMD:
        input_name = JOB_INPUT_CMD;
        goto parse_process_value;
    case TOKEN_FLOW:
        input_name = JOB_INPUT_FLOW;
parse_process_value:
        step->inputs = create_input(ctx->bump_p, input_name);
        if (lex(in) != TOKEN_SPACE)
            return error("expected space");
        return parse_value(ctx, step->inputs->val);

    case TOKEN_PARAMS:
        step->is_params = 1;
        // fall through
    case TOKEN_IDENTITY:
        step->inputs = create_input(ctx->bump_p, JOB_INPUT_IDENTITY);
        step->inputs->val->tag = VALUE_LITERAL;
        step->inputs->val->literal = NULL;
        step->inputs->val->literal_len = 0;
        return 0;

    case TOKEN_PARTIAL:
        return parse_process_partial(ctx, step);
    default:
        return error("expected cmd, identity, params, partial, or flow");
    }
}

static struct input_list* parse_input(struct parse_context* ctx) {
    struct lex_input* in = &ctx->in;
    if (lex_path(in) < 0)
        return NULL;
    struct input_list* input = create_input(ctx->bump_p, lex_stuff_null(in));

    int is_optional = 0;
    while (try_read_token(in, TOKEN_SPACE));
    if (try_read_token(in, TOKEN_QUESTION))
        is_optional = 1;
    if (lex(in) != TOKEN_EQUALS) {
        error("expected =");
        return NULL;
    }
    while (try_read_token(in, TOKEN_SPACE));

    if (parse_value(ctx, input->val) < 0)
        return NULL;
    if (is_optional) {
        if (input->val->tag != VALUE_DEPENDENCY) {
            error("optional input must be a dependency");
            return NULL;
        }
        input->val->dep_optional = 1;
    }

    save_lex_input(in);
    switch (lex(in)) {
    case TOKEN_EOF:
        load_lex_input(in);
        // fall through
    case TOKEN_NEWLINE:
        return input;
    default:
        error("expected newline");
        return NULL;
    }
}

static int parse_plan_internal(struct parse_context* ctx) {
    struct step_list** plan_p = &ctx->plan;
    struct step_list** partials_p = &ctx->partials;
    struct lex_input* in = &ctx->in;
    ssize_t step_pos = 0;
    while (1) {
        int is_partial;
        char* block_decl = in->curr;
        switch (lex_keyword(in)) {
        case TOKEN_STEP:    is_partial = 0; break;
        case TOKEN_PARTIAL: is_partial = 1; break;
        case TOKEN_NEWLINE: continue;
        case TOKEN_EOF:     return 0;
        default:            return error("expected step or partial");
        }

        if (lex(in) != TOKEN_SPACE)
            return error("expected space");
        if (lex(in) != TOKEN_IDENT)
            return error("expected identifier");

        struct step_list* step = bump_alloc(ctx->bump_p, sizeof(*step));
        memset(step, 0, sizeof(*step));
        step->name = lex_stuff_null(in);
        step->pos = is_partial ? -1 : step_pos++;
        ctx->block_decl = block_decl;

        // Append to the appropriate partials or plan list.
        struct step_list*** steps_pp = is_partial ? &partials_p : &plan_p;
        **steps_pp = step;
        *steps_pp = &step->next;

        while (try_read_token(in, TOKEN_SPACE));
        if (lex(in) != TOKEN_COLON)
            return error("expected :");
        while (try_read_token(in, TOKEN_SPACE));
        if (parse_process(ctx, step) < 0)
            return -1;

        if (step->is_params && step->pos != 0)
            return error("params step must be first");

        switch (lex(in)) {
        case TOKEN_NEWLINE:
        case TOKEN_EOF:
            break;
        default:
            return error("expected newline");
        }

        // Build inputs in sorted order.
        struct input_list* inputs = NULL;
        while (1) {
            while (try_read_token(in, TOKEN_NEWLINE));
            if (!try_read_token(in, TOKEN_SPACE))
                break;

            struct input_list* new_input = parse_input(ctx);
            if (!new_input)
                return -1;
            if (!is_partial && new_input->val->tag == VALUE_HOLE)
                return error("step input cannot be a hole");

            if (input_list_insert(&inputs, new_input))
                return -1;
        }

        input_list_override(ctx->bump_p, &step->inputs, inputs);
    }
}

// The resulting parse tree will be allocated in *bump_p. It may contain
// pointers into buf as well.
static struct step_list* parse_plan(struct bump_list** bump_p, char* buf) {
    struct parse_context ctx = { .bump_p = bump_p };
    lex_input_init(&ctx.in, buf);
    if (parse_plan_internal(&ctx) < 0) {
        int column = ctx.in.prev < ctx.in.line_p ? 0 : ctx.in.prev - ctx.in.line_p;
        error("at line %d column %zd near %s",
              ctx.in.lineno, column, ctx.block_decl ? ctx.block_decl : "top");
        // We could try to print the offending line but it would likely be
        // corrupted by NUL stuffing.
        return NULL;
    }
    return ctx.plan;
}

static int print_value(FILE* fh, const struct value* val) {
    switch (val->tag) {
    case VALUE_DEPENDENCY:
        fprintf(fh, "dependency %s %zu %s\n",
                val->dep_optional ? "optional" : "required",
                val->dep_pos, val->dep_path);
        return 0;
    case VALUE_FILENAME:
    case VALUE_LITERAL:
        fprintf(fh, "resource %s\n", oid_to_hex(&val->res->object.oid));
        return 0;
    default:
        die("bad value tag");
    }
}

static int print_build_instructions(FILE* fh, const struct job* job,
                                    const struct step_list* step) {
    fprintf(fh, "session %s\n", oid_to_hex(&job->object.oid));
    for (; step; step = step->next) {
        fprintf(fh, "step %s\n", step->name);
        for (const struct input_list* input = step->inputs;
             input; input = input->next) {
            fprintf(fh, "input %s\n", input->name);
            if (print_value(fh, input->val) < 0)
                return -1;
        }
    }
    fprintf(fh, "done\n");
    return 0;
}

static char* joindir(const char* dir, char* filename) {
    static char buf[PATH_MAX];
    if (snprintf(buf, PATH_MAX, "%s/%s", dir, filename) >= PATH_MAX)
        die("path too long");
    return buf;
}

static int populate_input(struct input_list* input, const char* files_dir,
                          struct resource_list** job_inputs_p) {
    struct value* val = input->val;
    if (val->res)
        return 0;

    switch (val->tag) {
    case VALUE_HOLE:
        return error("unfilled hole");
    case VALUE_DEPENDENCY:
        return 0;
    case VALUE_LITERAL:
        val->res = store_resource(val->literal, val->literal_len);
        break;
    case VALUE_FILENAME:
        val->res = store_resource_file(!input->is_param && files_dir ?
                                       joindir(files_dir, val->filename) :
                                       val->filename);
        break;
    }
    if (!val->res)
        return -1;

    // Add params and files referenced by the plan to the flow job.
    if (input->is_param) {
        char* path = joindir("params", input->name);
        resource_list_insert(job_inputs_p, path, val->res);
    } else if (val->tag == VALUE_FILENAME) {
        char* path = joindir("files", val->filename);
        struct resource_list* inserted =
            resource_list_insert(job_inputs_p, path, val->res);
        // The flow plan may include distinct references to the same file;
        // dedupe them here.
        if (inserted->next && !strcmp(inserted->path, inserted->next->path)) {
            assert(inserted->res == inserted->next->res);
            inserted->next = inserted->next->next;
        }
    }
    return 0;
}

static char* tokenize_param_optarg(char* s) {
    struct lex_input in;
    lex_input_init(&in, s);
    if (lex_path(&in) < 0)
        die("param name not a valid path");
    lex_stuff_null(&in);
    if (lex(&in) != TOKEN_EQUALS)
        die("missing = after param name");
    return in.curr;
}

static void die_usage(const char* arg0) {
    fprintf(stderr, "usage: %s [-f <plan>] [-F <files-dir>] [(-p|-P) <param>=<value>]...\n", arg0);
    exit(1);
}

int main(int argc, char** argv) {
    struct bump_list* bump = NULL;

    const char* plan_filename = "plan.knit";
    struct input_list* params = NULL;
    const char* files_dir = NULL;

    int opt;
    while ((opt = getopt(argc, argv, "f:F:p:P:")) != -1) {
        char* value;
        struct input_list* input;
        switch (opt) {
        case 'f':
            plan_filename = optarg;
            break;
        case 'F':
            files_dir = optarg;
            break;
        case 'p':
            value = tokenize_param_optarg(optarg);
            input = create_input(&bump, optarg);
            input->is_param = 1;
            input->val->tag = VALUE_LITERAL;
            input->val->literal = value;
            input->val->literal_len = strlen(value);
            if (input_list_insert(&params, input) < 0)
                exit(1);
            break;
        case 'P':
            value = tokenize_param_optarg(optarg);
            input = create_input(&bump, optarg);
            input->is_param = 1;
            input->val->tag = VALUE_FILENAME;
            input->val->filename = value;
            if (input_list_insert(&params, input) < 0)
                exit(1);
            break;
        default:
            die_usage(argv[0]);
        }
    }
    if (optind < argc)
        die_usage(argv[0]);

    struct bytebuf bb;
    // Our use of re2c requires a NUL sentinel byte after the file contents,
    // which slurp ensures.
    if (slurp_file(plan_filename, &bb) < 0)
        exit(1);
    if (memchr(bb.data, '\0', bb.size))
        die("NUL bytes in plan %s", plan_filename);

    // We will create a flow job including the plan, files it references, and
    // any params. Here we add the plan (before any NUL stuffing in
    // parse_plan()); the remaining resources will be added in populate_input().
    struct resource_list* job_inputs = NULL;
    struct resource* plan_res = store_resource(bb.data, bb.size);
    if (!plan_res)
        exit(1);
    resource_list_insert(&job_inputs, JOB_INPUT_FLOW, plan_res);

    struct step_list* plan = parse_plan(&bump, bb.data);
    if (!plan) {
        char buf[PATH_MAX];
        error("in file %s", realpath(plan_filename, buf));
        exit(1);
    }

    if (params) {
        struct input_list* new_param =
            input_list_override(&bump, &plan->inputs, params);
        if (new_param)
            die("params step missing or does not declare %s", new_param->name);
    }

    for (struct step_list* step = plan; step; step = step->next)
        for (struct input_list* input = step->inputs; input; input = input->next)
            if (populate_input(input, files_dir, &job_inputs) < 0)
                exit(1);

    struct job* job = store_job(job_inputs);
    if (!job || print_build_instructions(stdout, job, plan) < 0)
        exit(1);
    // leak bump and bb

    return 0;
}
