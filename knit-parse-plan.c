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
            // TODO memory ownership is unclear with partials
            struct bytebuf con_bb;
        };
        struct {
            char* dep_name;
            ssize_t dep_pos;
            char* dep_path;
        };
    };
};

struct input_list {
    char* name;
    struct value val;
    struct input_list* next;
};

enum process_type {
    PROCESS_PARAM,
    PROCESS_FLOW,
    PROCESS_SHELL,
};

struct step_list {
    char* name;
    ssize_t pos;
    enum process_type process;
    // TODO should subflow plan be a normal (or special?) input?
    struct value flow; // iff process == PROCESS_FLOW
    struct input_list* inputs;
    struct step_list* next;
};

static struct step_list* active_plan = NULL;
static struct step_list* active_partials = NULL;

static void destroy_step_list(struct step_list** list_p) {
    if (*list_p) {
        struct step_list* curr = *list_p;
        while (curr) {
            struct input_list* inputs = curr->inputs;
            while (inputs) {
                struct input_list* next = inputs->next;
                free(inputs);
                inputs = next;
            }
            struct step_list* next = curr->next;
            free(curr);
            curr = next;
        }
        *list_p = NULL;
    }
}

static struct step_list* find_step(struct step_list* step, const char* name) {
    for (; step; step = step->next) {
        if (!strcmp(step->name, name))
            return step;
    }
    return NULL;
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
        struct step_list* dep = find_step(active_plan, val->dep_name);
        if (!dep)
            return error("unknown dependency %s", val->dep_name);
        val->dep_pos = dep->pos;
    } else if (val->tag == VALUE_CONSTANT && val->con_filename) {
        mmap_file(val->con_filename, &val->con_bb);
    }
    return 0;
}

static int parse_process_partial(struct lex_input* in, struct step_list* step) {
    if (lex(in) != TOKEN_SPACE)
        return error("expected space");
    if (lex(in) != TOKEN_IDENT)
        return error("expected identifier");

    char* ident = lex_stuff_null(in);
    struct step_list* partial = find_step(active_partials, ident);
    if (!partial)
        return error("unknown partial %s", ident);

    step->process = partial->process;
    step->flow = partial->flow;

    assert(!step->inputs);
    struct input_list** inputs_p = &step->inputs;
    for (const struct input_list* orig = partial->inputs; orig; orig = orig->next) {
        struct input_list* copy = xmalloc(sizeof(struct input_list));
        memcpy(copy, orig, sizeof(*copy));
        copy->next = NULL;
        *inputs_p = copy;
        inputs_p = &copy->next;
    }

    return 0;
}

static int parse_process(struct lex_input* in, struct step_list* step) {
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

static int parse_input(struct lex_input* in, struct input_list* input) {
    input->name = read_path(in);
    if (!input->name)
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

static int parse_plan_internal(struct lex_input* in) {
    active_plan = active_partials = NULL;
    struct step_list** plan_p = &active_plan;
    struct step_list** partials_p = &active_partials;
    ssize_t step_pos = 0;
    while (1) {
        int is_partial;
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

        struct step_list* step = xmalloc(sizeof(struct step_list));
        memset(step, 0, sizeof(*step));
        step->name = lex_stuff_null(in);
        step->pos = is_partial ? -1 : step_pos++;

        // Append to the appropriate partials or plan list. This precedes
        // possible errors to avoid leaking memory.
        struct step_list*** steps_pp = is_partial ? &partials_p : &plan_p;
        **steps_pp = step;
        *steps_pp = &step->next;

        if (read_trim(in) != TOKEN_COLON)
            return error("expected :");
        if (parse_process(in, step) < 0)
            return -1;

        if (lex(in) != TOKEN_NEWLINE)
            return error("expected newline");
        while (is_read_keyword(in, TOKEN_NEWLINE));

        // Build inputs in sorted order.
        struct input_list* inputs = NULL;
        while (is_read_keyword(in, TOKEN_SPACE)) {
            struct input_list* new_input = xmalloc(sizeof(struct input_list));
            memset(new_input, 0, sizeof(*new_input));
            if (parse_input(in, new_input) < 0)
                return -1;
            if (!is_partial && new_input->val.tag == VALUE_HOLE)
                return error("unfilled hole");

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

        // Merge inputs into any (partial) step inputs.
        struct input_list** inputs_p = &step->inputs;
        while (inputs) {
            int cmp = -1;
            while (*inputs_p && (cmp = strcmp((*inputs_p)->name, inputs->name)) < 0)
                inputs_p = &(*inputs_p)->next;
            struct input_list* rest = inputs->next;
            // Replace any input with the same name.
            inputs->next = !cmp ? (*inputs_p)->next : *inputs_p;
            *inputs_p = inputs;
            inputs_p = &inputs->next;
            inputs = rest;
        }
        // TODO fail if no inputs?
    }
}

static struct step_list* parse_plan(struct lex_input* in) {
    if (parse_plan_internal(in) < 0)
        destroy_step_list(&active_plan);
    destroy_step_list(&active_partials);
    struct step_list* ret = active_plan;
    active_plan = NULL;
    return ret;
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

static int print_plan(FILE* fh, const struct step_list* step) {
    for (; step; step = step->next) {
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

        for (const struct input_list* input = step->inputs;
             input; input = input->next) {
            fprintf(fh, "input %s\n", input->name);
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
    struct step_list* plan = parse_plan(&in);
    if (!plan || print_plan(stdout, plan) < 0)
        exit(1);

    return 0;
}
