#include "hash.h"
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
            ssize_t dep_pos;
            char* dep_path;
            int dep_optional;
        };
    };
};

struct input_list {
    char* name;
    struct value val;
    struct input_list* next;
};

struct step_list {
    char* name;
    ssize_t pos;
    struct input_list* inputs;
    struct step_list* next;
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

static struct input_list* create_input(struct parse_context* ctx, char* name) {
    struct input_list* input = bump_alloc(ctx->bump_p, sizeof(*input));
    memset(input, 0, sizeof(*input));
    input->name = name;
    return input;
}

static int parse_value(struct parse_context* ctx, struct value* out) {
    struct lex_input* in = &ctx->in;
    ssize_t size;
    memset(out, 0, sizeof(*out));
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
        out->dep_pos = -1;
        if (lex(in) != TOKEN_COLON)
            return error("expected :");
        if (lex_path(in) < 0)
            return -1;
        out->dep_path = lex_stuff_null(in);
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

static int populate_value(struct parse_context* ctx, struct value* val) {
    if (val->tag == VALUE_DEPENDENCY) {
        struct step_list* dep = find_step(ctx->plan, val->dep_name);
        if (!dep)
            return error("dependency on not yet defined step %s", val->dep_name);
        val->dep_pos = dep->pos;
    }
    return 0;
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
        input_name = ".knit/cmd";
        goto parse_process_value;
    case TOKEN_FLOW:
        input_name = ".knit/flow";
parse_process_value:
        step->inputs = create_input(ctx, input_name);
        if (lex(in) != TOKEN_SPACE)
            return error("expected space");
        if (parse_value(ctx, &step->inputs->val) < 0)
            return -1;
        return populate_value(ctx, &step->inputs->val);

    case TOKEN_PARAM:
        input_name = ".knit/param";
        goto empty_process_value;
    case TOKEN_SHELL:
        input_name = ".knit/shell";
empty_process_value:
        step->inputs = create_input(ctx, input_name);
        step->inputs->val.tag = VALUE_LITERAL;
        step->inputs->val.literal = NULL;
        step->inputs->val.literal_len = 0;
        return 0;

    case TOKEN_PARTIAL:
        return parse_process_partial(ctx, step);
    default:
        return error("expected cmd, param, shell, partial, or flow");
    }
}

static struct input_list* parse_input(struct parse_context* ctx) {
    struct lex_input* in = &ctx->in;
    if (lex_path(in) < 0)
        return NULL;
    struct input_list* input = create_input(ctx, lex_stuff_null(in));

    int is_optional = 0;
    while (try_read_token(in, TOKEN_SPACE));
    if (try_read_token(in, TOKEN_QUESTION))
        is_optional = 1;
    if (lex(in) != TOKEN_EQUALS) {
        error("expected =");
        return NULL;
    }
    while (try_read_token(in, TOKEN_SPACE));

    if (parse_value(ctx, &input->val) < 0)
        return NULL;
    if (populate_value(ctx, &input->val) < 0)
        return NULL;
    if (is_optional) {
        if (input->val.tag != VALUE_DEPENDENCY) {
            error("optional input must be a dependency");
            return NULL;
        }
        input->val.dep_optional = 1;
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

static struct input_list* merge_with_partial_inputs(struct parse_context* ctx,
                                                    struct input_list* partial_inputs,
                                                    struct input_list* inputs) {
    struct input_list* ret = NULL;
    struct input_list** merged_p = &ret;
    while (inputs && partial_inputs) {
        int cmp = strcmp(inputs->name, partial_inputs->name);
        if (cmp == 0) {
            // Skip (override) any partial input with the same name.
            partial_inputs = partial_inputs->next;
        } else if (cmp < 0) {
            *merged_p = inputs;
            merged_p = &inputs->next;
            inputs = inputs->next;
        } else {
            // While we can share a common tail with any partials, any
            // preceding nodes in the merged list must be copies.
            struct input_list* copy = bump_alloc(ctx->bump_p, sizeof(*copy));
            memcpy(copy, partial_inputs, sizeof(*copy));
            *merged_p = copy;
            merged_p = &copy->next;
            partial_inputs = partial_inputs->next;
        }
    }
    *merged_p = inputs ? inputs : partial_inputs;
    return ret;
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
            if (!is_partial && new_input->val.tag == VALUE_HOLE)
                return error("step input cannot be a hole");

            struct input_list** prev_p = &inputs;
            int cmp = -1;
            while (*prev_p && (cmp = strcmp((*prev_p)->name, new_input->name)) < 0)
                prev_p = &(*prev_p)->next;
            if (cmp == 0)
                return error("duplicate input %s", new_input->name);
            new_input->next = *prev_p;
            *prev_p = new_input;
            prev_p = &new_input->next;
        }

        step->inputs = merge_with_partial_inputs(ctx, step->inputs, inputs);
        // TODO fail if no inputs?
        if (!is_partial) {
            for (inputs = step->inputs; inputs; inputs = inputs->next)
                if (inputs->val.tag == VALUE_HOLE)
                    return error("unfilled hole");
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
        error("at line %d column %zd near %s",
              ctx.in.lineno, column, ctx.block_decl ? ctx.block_decl : "top");
        // We could try to print the offending line but it would likely be
        // corrupted by NUL stuffing.
        return NULL;
    }
    return ctx.plan;
}

static int print_resource(FILE* fh, void* data, size_t size) {
    struct resource* res = store_resource(data, size);
    if (!res)
        return -1;
    fprintf(fh, "resource %s\n", oid_to_hex(&res->object.oid));
    return 0;
}

static int print_value(FILE* fh, const struct value* val) {
    struct bytebuf bb;
    switch (val->tag) {
    case VALUE_DEPENDENCY:
        assert(val->dep_pos >= 0);
        fprintf(fh, "dependency %s %zu %s\n",
                val->dep_optional ? "optional" : "required",
                val->dep_pos, val->dep_path);
        break;
    case VALUE_FILENAME:
        // TODO resource cache by (absolute?) filename
        if (mmap_file(val->filename, &bb) < 0)
            return -1;
        print_resource(fh, bb.data, bb.size);
        cleanup_bytebuf(&bb);
        break;
    case VALUE_LITERAL:
        // TODO resource cache by literal (address?)
        print_resource(fh, val->literal, val->literal_len);
        break;
    default:
        die("bad value tag");
    }
    return 0;
}

static int print_plan(FILE* fh, const struct step_list* step) {
    for (; step; step = step->next) {
        fprintf(fh, "step %s\n", step->name);
        for (const struct input_list* input = step->inputs;
             input; input = input->next) {
            fprintf(fh, "input %s\n", input->name);
            if (print_value(fh, &input->val) < 0)
                return -1;
        }
    }
    fprintf(fh, "done\n");
    return 0;
}

static void die_usage(const char* arg0) {
    fprintf(stderr, "usage: %s <plan>\n", arg0);
    exit(1);
}

int main(int argc, char** argv) {
    if (argc != 2)
        die_usage(argv[0]);

    struct bytebuf bb;
    // Our use of re2c requires a NUL sentinel byte after the file contents,
    // which slurp ensures.
    if (slurp_file(argv[1], &bb) < 0)
        exit(1);
    if (memchr(bb.data, '\0', bb.size))
        die("NUL bytes in plan %s", argv[1]);

    struct bump_list* bump = NULL;
    struct step_list* plan = parse_plan(&bump, bb.data);
    if (!plan || print_plan(stdout, plan) < 0) {
        char buf[PATH_MAX];
        error("in file %s", realpath(argv[1], buf));
        exit(1);
    }
    // leak bump and bb

    return 0;
}
