// Unapologetically leaks memory since we assume the process is short lived.

#include "hash.h"
#include "lexer.h"

enum value_tag {
    VALUE_CONSTANT,
    VALUE_DEPENDENCY,
    VALUE_HOLE,
};

struct value {
    enum value_tag tag;
    union {
        struct {
            char* con_filename;
            struct bytebuf con_bb;
        };
        struct {
            char* dep_name;
            ssize_t dep_pos;
            char* dep_path;
        };
    };
};

struct input {
    char* key;
    struct value val;
};

enum process_tag {
    PROCESS_PARAM,
    PROCESS_FLOW,
    PROCESS_SHELL,
};

struct step {
    char* name;
    enum process_tag process;
    // TODO should subflow plan be a normal (or special?) input?
    struct value flow; // iff process == PROCESS_FLOW
    struct input** inputs;
    int num_inputs;
    int alloc_inputs;
};

struct section {
    struct step** steps;
    int num_steps;
    int alloc_steps;
};

static struct section* active_plan = NULL;
static struct section* active_partials = NULL;

static ssize_t find_step(struct section* sect, const char* name) {
    for (int i = 0; i < sect->num_steps; i++) {
        if (!strcmp(sect->steps[i]->name, name))
            return i;
    }
    return -1;
}

static enum token read_trim(struct lex_input* in) {
    enum token token;
    do {
        token = lex(in);
    } while (token == TOKEN_SPACE);
    if (lex(in) != TOKEN_SPACE)
        in->curr = in->prev;
    return token;
}

static int is_read_keyword(struct lex_input* in, enum token expected) {
    enum token actual = lex_keyword(in);
    if (actual == expected)
        return 1;
    in->curr = in->prev;
    return 0;
}

static char* read_path(struct lex_input* in) {
    if (lex_path(in) < 0)
        return NULL;
    lex_stuff_null(in);
    return in->prev;
}

static int parse_value(struct lex_input* in, struct value* out) {
    memset(out, 0, sizeof(*out));
    switch (lex(in)) {
    case TOKEN_EXCLAMATION:
        out->tag = VALUE_HOLE;
        return 0;
    case TOKEN_QUOTE:
        out->tag = VALUE_CONSTANT;
        return lex_string(in, &out->con_bb);
    case TOKEN_IDENT:
        out->tag = VALUE_DEPENDENCY;
        out->dep_name = lex_stuff_null(in);
        out->dep_pos = -1;
        if (lex(in) != TOKEN_COLON)
            return error("expected :");
        out->dep_path = read_path(in);
        return out->dep_path ? 0 : -1;
    case TOKEN_DOTSLASH:
        out->tag = VALUE_CONSTANT;
        out->con_filename = read_path(in);
        return out->con_filename ? 0 : -1;
    default:
        return error("expected value");
    }
}

static int populate_value(struct value* val) {
    if (val->tag == VALUE_DEPENDENCY) {
        val->dep_pos = find_step(active_plan, val->dep_name);
        if (val->dep_pos < 0)
            return error("unknown dependency %s", val->dep_name);
    } else if (val->tag == VALUE_CONSTANT && val->con_filename) {
        mmap_file(val->con_filename, &val->con_bb);
    }
    return 0;
}

static int parse_process_partial(struct lex_input* in, struct step* step) {
    if (lex(in) != TOKEN_SPACE)
        return error("expected space");
    if (lex(in) != TOKEN_IDENT)
        return error("expected identifier");
    char* ident = lex_stuff_null(in);
    ssize_t pos = find_step(active_partials, ident);
    if (pos < 0)
        return error("unknown partial %s", ident);
    step->process = active_partials->steps[pos]->process;
    step->flow = active_partials->steps[pos]->flow;
    // TODO copy inputs from partial (merge sort?)
    fprintf(stderr, "partial %s\n", ident);
    return 0;
}

static int parse_process(struct lex_input* in, struct step* step) {
    switch (lex_keyword(in)) {
    case TOKEN_PARAM:
        step->process = PROCESS_PARAM;
        return 0;
    case TOKEN_SHELL:
        step->process = PROCESS_SHELL;
        return 0;
    case TOKEN_PARTIAL:
        return parse_process_partial(in, step);
    case TOKEN_FLOW:
        step->process = PROCESS_FLOW;
        if (parse_value(in, &step->flow) < 0)
            return -1;
        return populate_value(&step->flow);
    default:
        return error("expected param, shell, partial, or flow");
    }
}

static int parse_input(struct lex_input* in, struct input* input) {
    memset(input, 0, sizeof(*input));
    input->key = read_path(in);
    if (!input->key)
        return -1;
    if (read_trim(in) != TOKEN_EQUALS)
        return error("expected =");
    if (parse_value(in, &input->val) < 0)
        return -1;
    if (populate_value(&input->val) < 0)
        return -1;

    switch (lex(in)) {
    case TOKEN_EOF:
        in->curr = in->prev;
        // fall through
    case TOKEN_NEWLINE:
        return 0;
    default:
        return error("expected newline");
    }
}

static int parse_plan(struct lex_input* in, struct section* plan) {
    active_plan = plan;
    static struct section partials = { 0 };
    active_partials = &partials;

    while (1) {
        int is_partial;
        switch (lex_keyword(in)) {
        case TOKEN_STEP:    is_partial = 0; break;
        case TOKEN_PARTIAL: is_partial = 1; break;
        case TOKEN_NEWLINE: continue;
        case TOKEN_EOF:     active_partials = NULL; return 0;
        default:            return error("expected step or partial");
        }

        if (lex(in) != TOKEN_SPACE)
            return error("expected space");
        if (lex(in) != TOKEN_IDENT)
            return error("expected identifier");
        struct step* step = xmalloc(sizeof(struct step));
        step->name = lex_stuff_null(in);
        if (read_trim(in) != TOKEN_COLON)
            return error("expected :");
        if (parse_process(in, step) < 0)
            return -1;

        if (lex(in) != TOKEN_NEWLINE)
            return error("expected newline");
        while (is_read_keyword(in, TOKEN_NEWLINE));

        while (is_read_keyword(in, TOKEN_SPACE)) {
            struct input* input = xmalloc(sizeof(struct input));
            if (parse_input(in, input) < 0)
                return -1;
            if (!is_partial && input->val.tag == VALUE_HOLE)
                return error("unfilled hole");

            if (step->num_inputs == step->alloc_inputs) {
                step->alloc_inputs =
                    step->alloc_inputs == 0 ? 4 : step->alloc_inputs * 2;
                step->inputs = xrealloc(step->inputs,
                                        step->alloc_inputs * sizeof(*step->inputs));
            }
            step->inputs[step->num_inputs++] = input;
        }

        // TODO fail if no inputs?

        struct section* sect = is_partial ? active_partials : active_plan;
        if (sect->num_steps == sect->alloc_steps) {
            sect->alloc_steps = sect->alloc_steps == 0 ? 4 : sect->alloc_steps * 2;
            sect->steps = xrealloc(sect->steps,
                                   sect->alloc_steps * sizeof(*sect->steps));
        }
        sect->steps[sect->num_steps++] = step;
    }
}


// TODO probably need to synthesize a flow input
void print_value(FILE* fh, const struct value* val) {
    switch (val->tag) {
    case VALUE_CONSTANT:
        if (val->con_filename)
            fprintf(fh, "%s->", val->con_filename);
        putc('"', fh);
        fwrite(val->con_bb.data, 1, val->con_bb.size, fh);
        fprintf(fh, "\"\n");
        break;
    case VALUE_DEPENDENCY:
        fprintf(fh, "%s[%zd]:%s\n", val->dep_name, val->dep_pos, val->dep_path);
        break;
    case VALUE_HOLE:
        fprintf(fh, "!\n");
        break;
    }
}

static int print_plan(FILE* fh, const struct section* plan) {
    for (int i = 0; i < plan->num_steps; i++) {
        const struct step* step = plan->steps[i];
        fprintf(fh, "step %s\n", step->name);

        switch (step->process) {
        case PROCESS_PARAM:
            fprintf(fh, "param\n");
            break;
        case PROCESS_FLOW:
            fprintf(fh, "flow ");
            print_value(fh, &step->flow);
            break;
        case PROCESS_SHELL:
            fprintf(fh, "shell\n");
            break;
        }

        for (int j = 0; j < step->num_inputs; j++) {
            const struct input* input = step->inputs[j];
            fprintf(fh, "input %s\n", input->key);
            if (input->val.tag == VALUE_CONSTANT) {
                struct object_id res_oid;
                if (write_object(TYPE_RESOURCE, input->val.con_bb.data,
                                 input->val.con_bb.size, &res_oid) < 0)
                    return -1;
                fprintf(fh, "resource %s\n", oid_to_hex(&res_oid));
            } else if (input->val.tag == VALUE_DEPENDENCY) {
                assert(input->val.dep_pos >= 0);
                fprintf(fh, "dependency %zu %s\n", input->val.dep_pos, input->val.dep_path);
            }
        }
    }

    fprintf(fh, "save\n");
    return 0;
}

static void die_usage(const char* arg0) {
    fprintf(stderr, "usage: %s <plan>\n", arg0);
    exit(1);
}

int main(int argc, char** argv) {
    if (argc != 2)
        die_usage(argv[0]);

    int fd = open(argv[1], O_RDONLY);
    if (fd < 0)
        die("cannot open %s: %s", argv[1], strerror(errno));
    struct bytebuf bb;
    if (slurp_fd(fd, &bb) < 0)
        exit(1);
    // Copy the buffer to NUL-terminate it; our use of re2c treats NUL as EOF.
    char* buf = xmalloc(bb.size + 1);
    if (memccpy(buf, bb.data, '\0', bb.size))
        die("NUL bytes in plan %s", argv[1]);
    buf[bb.size] = '\0';
    cleanup_bytebuf(&bb);

    struct lex_input in = { .curr = buf };
    struct section plan = { 0 };
    if (parse_plan(&in, &plan) < 0 ||
            print_plan(stdout, &plan) < 0)
        exit(1);

    return 0;
}
