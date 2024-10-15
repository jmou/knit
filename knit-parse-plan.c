#include "alloc.h"
#include "hash.h"
#include "job.h"
#include "lexer.h"
#include "resource.h"
#include "spec.h"
#include "util.h"

enum value_tag {
    VALUE_DEPENDENCY,
    VALUE_FILENAME,
    VALUE_HOLE,
    VALUE_LITERAL,
};

struct value {
    enum value_tag tag;
    union {
        struct {
            // For VALUE_LITERAL.
            char* literal;
            size_t literal_len;
        };
        struct {
            // For both VALUE_DEPENDENCY and VALUE_FILENAME.
            // path may be a dependency output or filename respectively.
            char* path;
            unsigned path_dir : 1;
            unsigned path_optional : 1;
            // For only VALUE_DEPENDENCY.
            char* dep_step;
            size_t dep_pos;
            unsigned dep_implicit_ok : 1;
        };
    };
    struct resource* res;
};

struct input_list {
    char* name;
    struct value* val;
    struct input_list* next;
};

struct step_list {
    char* name;
    ssize_t pos;
    struct input_list* inputs;
    struct step_list* next;
    // When modifying flags, be sure to consider propagation through partials.
    unsigned is_params : 1;
    unsigned is_nocache : 1;
};

static struct step_list* find_step(struct step_list* step, const char* name) {
    for (; step; step = step->next) {
        if (!strcmp(step->name, name))
            return step;
    }
    return NULL;
}

static struct input_list* create_input(struct bump_list** bump_p, char* name) {
    struct input_list* input = bump_alloc(bump_p, sizeof(*input));
    memset(input, 0, sizeof(*input));
    input->name = name;
    input->val = bump_alloc(bump_p, sizeof(*input->val));
    memset(input->val, 0, sizeof(*input->val));
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
    int discard_orig = 0;
    while (override && orig) {
        size_t orig_name_len = strlen(orig->name);
        assert(orig_name_len > 0);
        int is_prefix = orig->name[orig_name_len - 1] == '/';
        int cmp = is_prefix
            ? strncmp(override->name, orig->name, orig_name_len)
            : strcmp(override->name, orig->name);
        if (cmp == 0) {
            *inputs_p = override;
            inputs_p = &override->next;
            override = override->next;
            // When comparing to a prefix that may match multiple times, defer
            // iterating to orig->next.
            if (is_prefix) {
                discard_orig = 1;
            } else {
                orig = orig->next;
            }
        } else if (cmp < 0) {
            added = override;
            *inputs_p = override;
            inputs_p = &override->next;
            override = override->next;
        } else if (discard_orig) {
            orig = orig->next;
            discard_orig = 0;
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
    } else if (discard_orig) {
        assert(orig);
        *inputs_p = orig->next;
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

static int is_dir(const char* s) {
    return s[0] == '\0' || s[strlen(s) - 1] == '/';
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
        out->dep_step = lex_stuff_null(in);
        if (lex(in) != TOKEN_COLON)
            return error("expected :");
        if (lex_path_or_empty(in) < 0)
            return -1;
        out->path = lex_stuff_null(in);
        out->path_dir = is_dir(out->path);
        dep = find_step(ctx->plan, out->dep_step);
        if (!dep)
            return error("dependency on not yet defined step %s", out->dep_step);
        out->dep_pos = dep->pos;
        return 0;
    case TOKEN_DOTSLASH:
        out->tag = VALUE_FILENAME;
        if (lex_path_or_empty(in) < 0)
            return -1;
        out->path = lex_stuff_null(in);
        out->path_dir = is_dir(out->path);
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
    assert(!partial->is_params);
    if (step->is_nocache && partial->is_nocache)
        warning("both step and partial are marked nocache");
    step->is_nocache = partial->is_nocache;
    return 0;
}

static int val_is_dir(const struct value* val) {
    if (val->tag != VALUE_DEPENDENCY && val->tag != VALUE_FILENAME)
        return 0;
    return val->path_dir;
}

static void append_value_suffix(struct parse_context* ctx, const struct value* in,
                                const char* suffix, struct value* out) {
    assert(in->tag == VALUE_DEPENDENCY || in->tag == VALUE_FILENAME);
    memcpy(out, in, sizeof(*out));
    out->path = bump_alloc(ctx->bump_p, strlen(in->path) + strlen(suffix) + 1);
    stpcpy(stpcpy(out->path, in->path), suffix);
    out->path_dir = is_dir(out->path);
}

static int parse_process(struct parse_context* ctx, struct step_list* step) {
    struct lex_input* in = &ctx->in;

    enum token tok = lex_keyword(in);
    if (tok == TOKEN_NOCACHE) {
        if (lex(in) != TOKEN_SPACE)
            return error("expected space");
        step->is_nocache = 1;
        tok = lex_keyword(in);
    }

    struct input_list* context_input;
    switch (tok) {
    case TOKEN_CMD:
        step->inputs = create_input(ctx->bump_p, JOB_INPUT_CMD);
        if (lex(in) != TOKEN_SPACE)
            return error("expected space");
        if (parse_value(ctx, step->inputs->val) < 0)
            return -1;
        if (val_is_dir(step->inputs->val))
            return error("cmd path cannot end in '/'");
        return 0;

    case TOKEN_FLOW:
        step->inputs = create_input(ctx->bump_p, JOB_INPUT_FLOW);
        context_input = create_input(ctx->bump_p, JOB_INPUT_FILES_PREFIX);
        if (lex(in) != TOKEN_SPACE)
            return error("expected space");
        if (parse_value(ctx, context_input->val) < 0)
            return -1;
        if (!val_is_dir(context_input->val)) {
            step->inputs->val = context_input->val;
            return 0;
        }
        if (try_read_token(in, TOKEN_SPACE)) {
            if (try_read_token(in, TOKEN_COLON)) {
                if (lex_path_or_empty(in) < 0)
                    return -1;
                char* suffix = lex_stuff_null(in);
                append_value_suffix(ctx, context_input->val, suffix,
                                    step->inputs->val);
            } else if (parse_value(ctx, step->inputs->val) < 0) {
                return -1;
            }
        } else {
            append_value_suffix(ctx, context_input->val, "plan.knit",
                                step->inputs->val);
        }
        if (val_is_dir(step->inputs->val))
            return error("flow plan path cannot end in '/'");
        if (input_list_insert(&step->inputs, context_input) < 0)
            return -1;
        return 0;

    case TOKEN_PARAMS:
        step->is_params = 1;
        // fall through
    case TOKEN_IDENTITY:
        if (step->is_nocache)
            return error("identity and params cannot be nocache");
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
    if (!try_read_token(in, TOKEN_ENVVAR) &&
            lex_path(in) < 0)
        return NULL;
    struct input_list* input = create_input(ctx->bump_p, lex_stuff_null(in));

    int is_optional = 0;
    int suppress_implicit_ok = 0;
    while (try_read_token(in, TOKEN_SPACE));
    if (try_read_token(in, TOKEN_QUESTION))
        is_optional = 1;
    if (try_read_token(in, TOKEN_COLON))
        suppress_implicit_ok = 1;
    if (lex(in) != TOKEN_EQUALS) {
        error("expected =");
        return NULL;
    }
    while (try_read_token(in, TOKEN_SPACE));

    if (parse_value(ctx, input->val) < 0)
        return NULL;

    switch (input->val->tag) {
    case VALUE_DEPENDENCY:
        input->val->dep_implicit_ok = !suppress_implicit_ok;
        // fall through
    case VALUE_FILENAME:
        if (is_dir(input->name)) {
            if (!input->val->path_dir) {
                error("input name ends in '/' but input value path does not");
                return NULL;
            }
        } else if (input->val->path_dir) {
            error("input value path ends in '/' but input name does not");
            return NULL;
        } else if (input->val->tag == VALUE_FILENAME && is_optional) {
            error("optional file input must end in '/'");
            return NULL;
        }
        input->val->path_optional = is_optional;
        break;

    case VALUE_LITERAL:
        if (is_dir(input->name)) {
            error("input name ending in '/' cannot be a string literal");
            return NULL;
        }
        // fall through
    case VALUE_HOLE:
        if (is_optional) {
            error("optional input must be a dependency or file");
            return NULL;
        }
    }
    if (input->val->tag != VALUE_DEPENDENCY && suppress_implicit_ok) {
        error("input suppressing implicit ok must be a dependency");
        return NULL;
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
            return error("params must be first step");

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
            if (new_input->val->tag == VALUE_HOLE && !is_partial && !step->is_params)
                return error("non-params step input cannot be a hole");

            if (input_list_insert(&inputs, new_input))
                return -1;
        }

        input_list_override(ctx->bump_p, &step->inputs, inputs);

        for (struct input_list* input = step->inputs;
             input && input->next; input = input->next) {
            if (is_dir(input->name) &&
                    !strncmp(input->next->name, input->name, strlen(input->name)))
                return error("overlapping inputs %s and %s",
                             input->name, input->next->name);
        }
    }
}

// The resulting parse tree will be allocated in *bump_p. It may contain
// pointers into buf as well.
static struct step_list* parse_plan(struct bump_list** bump_p, char* buf) {
    struct parse_context ctx = { .bump_p = bump_p };
    lex_input_init(&ctx.in, buf);
    if (parse_plan_internal(&ctx) < 0) {
        int column = ctx.in.prev < ctx.in.line_p ? 0 : ctx.in.prev - ctx.in.line_p;
        error("at line %d column %d near %s",
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
        fprintf(fh, "dependency input %s%s %zu %s\n",
                val->path_optional ? "optional" : "required",
                val->path_dir ? " prefix" : "",
                val->dep_pos, val->path);
        // We could deduplicate implicit dependencies to reduce session size and
        // reduce work in knit-complete-job, but it probably has a minor impact.
        if (val->dep_implicit_ok)
            fprintf(fh, "dependency step required %zu .knit/ok\n", val->dep_pos);
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

static struct input_list* job_params_to_inputs(struct bump_list** bump_p,
                                               struct resource_list* job_inputs) {
    struct input_list* ret = NULL;
    struct input_list** inputs_p = &ret;
    for (; job_inputs; job_inputs = job_inputs->next) {
        if (!strncmp(job_inputs->name, JOB_INPUT_RESERVED_PREFIX,
                     strlen(JOB_INPUT_RESERVED_PREFIX)))
            continue;

        struct input_list* input = bump_alloc(bump_p, sizeof(*input));
        input->name = job_inputs->name;
        input->val = bump_alloc(bump_p, sizeof(*input->val));
        input->val->tag = VALUE_FILENAME;
        input->val->path = job_inputs->name;
        input->val->res = job_inputs->res;
        input->next = NULL;
        *inputs_p = input;
        inputs_p = &input->next;
    }
    return ret;
}

static struct resource* find_resource(struct resource_list* list, const char* name) {
    for (; list; list = list->next) {
        int cmp = strcmp(list->name, name);
        if (!cmp)
            break;
        else if (cmp > 0)
            return NULL;
    }
    return list ? list->res : NULL;
}

static struct resource* find_file_resource(struct resource_list* list,
                                           const char* filename) {
    char buf[strlen(JOB_INPUT_FILES_PREFIX) + strlen(filename) + 1];
    stpcpy(stpcpy(buf, JOB_INPUT_FILES_PREFIX), filename);
    return find_resource(list, buf);
}

static int populate_input(struct input_list* input, struct resource_list* job_inputs) {
    struct value* val = input->val;
    if (val->res)
        return 0;

    switch (val->tag) {
    case VALUE_HOLE:
        return error("unfilled hole %s", input->name);
    case VALUE_DEPENDENCY:
        return 0;
    case VALUE_LITERAL:
        val->res = store_resource(val->literal, val->literal_len);
        break;
    case VALUE_FILENAME:
        val->res = find_file_resource(job_inputs, val->path);
        if (!val->res)
            return error("missing job input file %s", val->path);
        break;
    }
    return val->res ? 0 : -1;
}

char* make_joined_str(struct bump_list** bump_p,
                      const char* prefix, const char* suffix) {
    char* buf = bump_alloc(bump_p, strlen(prefix) + strlen(suffix) + 1);
    stpcpy(stpcpy(buf, prefix), suffix);
    return buf;
}

static void expand_input_file_dir(struct bump_list** bump_p,
                                  struct input_list** input_p,
                                  struct resource_list* job_inputs) {
    struct input_list* dir_input = *input_p;
    // Remove dir_input (that is, *input_p) from the step.
    *input_p = dir_input->next;

    size_t prefix_len = strlen(JOB_INPUT_FILES_PREFIX) + strlen(dir_input->val->path);
    char prefix[prefix_len + 1];
    stpcpy(stpcpy(prefix, JOB_INPUT_FILES_PREFIX), dir_input->val->path);

    // Add an expanded step input for each job input in the directory of the
    // dir_input value.
    for (; job_inputs; job_inputs = job_inputs->next) {
        int cmp = strncmp(job_inputs->name, prefix, prefix_len);
        if (cmp < 0) {
            continue;
        } else if (cmp > 0) {
            break;
        }

        char* suffix = job_inputs->name + prefix_len;
        char* step_input_name = make_joined_str(bump_p, dir_input->name, suffix);
        struct input_list* inserted = create_input(bump_p, step_input_name);
        inserted->val->tag = VALUE_FILENAME;
        inserted->val->path =
            make_joined_str(bump_p, dir_input->val->path, suffix);
        inserted->next = *input_p;
        *input_p = inserted;
        input_p = &inserted->next;
    }
}

static int finalize_flow_job_step(struct bump_list** bump_p,
                                  struct step_list* step,
                                  struct resource_list* job_inputs) {
    // Represent nocache as a special input so it is part of the job id.
    if (step->is_nocache) {
        struct input_list* input = create_input(bump_p, JOB_INPUT_NOCACHE);
        input->val->tag = VALUE_LITERAL;
        input->val->literal = NULL;
        input->val->literal_len = 0;
        if (input_list_insert(&step->inputs, input))
            return -1;
    }

    for (struct input_list** input_p = &step->inputs;
         *input_p; input_p = &(*input_p)->next) {
        struct value* val = (*input_p)->val;
        if (val->tag == VALUE_FILENAME && val->path_dir) {
            expand_input_file_dir(bump_p, input_p, job_inputs);
            if (!*input_p) // empty expansion
                break;
        }

        if (populate_input(*input_p, job_inputs) < 0)
            return -1;
    }
    return 0;
}

static int finalize_flow_job_plan(struct bump_list** bump_p,
                                  struct step_list* plan,
                                  struct resource_list* job_inputs) {
    // Only the first step of the plan may be a params process. If the job
    // has any params they should override the params step inputs.
    struct input_list* params = job_params_to_inputs(bump_p, job_inputs);
    if (params && !plan->is_params)
        return error("params step missing");
    struct input_list* added = input_list_override(bump_p, &plan->inputs, params);
    if (added)
        return error("params step does not declare %s", added->name);

    for (struct step_list* step = plan; step; step = step->next) {
        if (finalize_flow_job_step(bump_p, step, job_inputs) < 0)
            return error("in step %s", step->name);
    }

    return 0;
}

static void emit_params(struct step_list* step) {
    for (struct input_list* input = step->inputs; input; input = input->next) {
        if (!strncmp(input->name, JOB_INPUT_RESERVED_PREFIX,
                     strlen(JOB_INPUT_RESERVED_PREFIX)))
            continue;
        printf("param %s", input->name);
        if (input->val->tag == VALUE_FILENAME)
            printf("=./%s\n", input->val->path);
        else
            printf("\n");
    }
}

static void emit_files(struct step_list* step) {
    for (struct input_list* input = step->inputs; input; input = input->next)
        if (input->val->tag == VALUE_FILENAME)
            printf("file%s ./%s\n",
                   input->val->path_optional ? " optional" : "",
                   input->val->path);
}

static void die_usage(const char* arg0) {
    fprintf(stderr, "usage: %s --job-to-session <job>\n", arg0);
    fprintf(stderr, "       %s --emit-params-files <plan>\n", arg0);
    exit(1);
}

int main(int argc, char** argv) {
    struct job* job = NULL;
    struct bytebuf bb;

    if (argc != 3)
        die_usage(argv[0]);
    if (!strcmp(argv[1], "--job-to-session")) {
        job = peel_job(argv[2]);
        if (!job || parse_job(job) < 0)
            exit(1);

        struct resource* plan_res = find_resource(job->inputs, JOB_INPUT_FLOW);
        bb.data = read_object_of_type(&plan_res->object.oid, OBJ_RESOURCE, &bb.size);
        if (!bb.data)
            exit(1);
        bb.should_free = 1;
    } else if (!strcmp(argv[1], "--emit-params-files")) {
        if (slurp_file(argv[2], &bb) < 0)
            exit(1);
    } else {
        die_usage(argv[0]);
    }
    if (memchr(bb.data, '\0', bb.size))
        die("NUL bytes in plan");

    struct bump_list* bump = NULL;
    // Our use of re2c requires a NUL sentinel byte after the plan contents.
    ensure_bytebuf_null_terminated(&bb);
    struct step_list* plan = parse_plan(&bump, bb.data);
    if (!plan) {
        char buf[PATH_MAX];
        if (job)
            error("in job %s input %s", argv[2], JOB_INPUT_FLOW);
        else
            error("in file %s", realpath(argv[2], buf));
        exit(1);
    }

    if (job) {
        if (finalize_flow_job_plan(&bump, plan, job->inputs) < 0) {
            error("in job %s", oid_to_hex(&job->object.oid));
            exit(1);
        }
        if (print_build_instructions(stdout, job, plan) < 0)
            exit(1);
    } else {
        for (struct step_list* step = plan; step; step = step->next) {
            if (step->is_nocache)
                printf("nocache\n");
            if (step->is_params)
                emit_params(step);
            else
                emit_files(step);
        }
    }

    // leak bump and bb
    return 0;
}
