#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "lexer.h"

void die(const char* format, ...) {
    va_list params;
    va_start(params, format);
    fprintf(stderr, "fatal: ");
    vfprintf(stderr, format, params);
    fprintf(stderr, "\n");
    va_end(params);
    exit(1);
}

int error(const char* format, ...) {
    va_list params;
    va_start(params, format);
    fprintf(stderr, "error: ");
    vfprintf(stderr, format, params);
    fprintf(stderr, "\n");
    va_end(params);
    return -1;
}

char* lex_stuff_null(struct lex_input* in) {
    in->actual_curr = *in->curr;
    *in->curr = '\0';
    return in->prev;
}

static inline void pre_lex(struct lex_input* in) {
    in->prev = in->curr;
    if (in->actual_curr != '\0')
        *in->curr = in->actual_curr;
}

static inline void post_lex(struct lex_input* in) {
    if (in->actual_curr != '\0') {
        *in->prev = '\0';
        if (in->curr != in->prev)
            in->actual_curr = '\0';
    }
}

enum token lex(struct lex_input* in) {
    pre_lex(in);
    enum token token;
    char* marker;
    while (1) {
        // Configure re2c globally and instantiate reusable rules.
        /*!re2c
            re2c:define:YYCTYPE = char;
            re2c:define:YYCURSOR = in->curr;
            re2c:define:YYMARKER = marker;
            re2c:yyfill:enable = 0;
        */
        /*!rules:re2c
            [ ]*([#][^\n\x00]*)?[\r]?[\n] { token = TOKEN_NEWLINE; break; }
            [ ]+ { token = TOKEN_SPACE; break; }
            [!] { token = TOKEN_EXCLAMATION; break; }
            ["] { token = TOKEN_QUOTE; break; }
            [:] { token = TOKEN_COLON; break; }
            [=] { token = TOKEN_EQUALS; break; }
            "./" { token = TOKEN_DOTSLASH; break; }
            [\x00] { token = TOKEN_EOF; break; }
            * { token = TOKEN_ERROR; break; }
        */
        // TODO loosen ident to accommodate non-ASCII?
        /*!use:re2c
            ident = [a-zA-Z_][a-zA-Z0-9_.@-]*;
            ident { token = TOKEN_IDENT; break; }
        */
    }
    post_lex(in);
    return token;
}

enum token lex_keyword(struct lex_input* in) {
    pre_lex(in);
    enum token token;
    char* marker;
    while (1) {
        /*!use:re2c
            "partial" { token = TOKEN_PARTIAL; break; }
            "step" { token = TOKEN_STEP; break; }
            "param" { token = TOKEN_PARAM; break; }
            "flow" { token = TOKEN_FLOW; break; }
            "shell" { token = TOKEN_SHELL; break; }
        */
    }
    post_lex(in);
    return token;
}

int lex_path(struct lex_input* in) {
    pre_lex(in);
    while (1) {
        char* rew = in->curr;
        /*!re2c
            component = [a-zA-Z0-9_.-]+;

            ("." | "..") [/]? { return error("invalid path component"); }
            component [/] { continue; }
            component { break; }
            * { in->curr = rew; break; }
        */
    }
    if (in->curr == in->prev)
        return error("expected path");
    post_lex(in);
    return 0;
}

int lex_string(struct lex_input* in, struct bytebuf* out) {
    pre_lex(in);
    memset(out, 0, sizeof(*out));
    out->data = in->prev;

    int rc;
    int needs_copy = 0;
    size_t alloc = 0;
    char c;
    for (;; out->size++) {
        c = *in->curr;
        if (needs_copy) {
            if (alloc == 0) {
                alloc = 16;
                char* copy = xmalloc(alloc);
                memcpy(copy, out->data, out->size);
                out->data = copy;
                out->should_free = 1;
            } else if (alloc == out->size) {
                alloc *= 2;
                out->data =(char*)realloc(out->data, alloc);
            }
            ((char*)out->data)[out->size] = c;
        }
        // TODO disallow newline in string? See http://re2c.org/examples/c/real_world/example_cxx98.html
        /*!re2c
            ["] { rc = 0; break; }
            [^\\"\x00] { continue; }
            "\\n" { c = '\n'; needs_copy = 1; continue; }
            [\\][^\x00] { rc = error("invalid string escape"); break; }
            * { rc = error("unterminated string"); break; }
        */
    }

    post_lex(in);
    if (rc < 0 && out->should_free) {
        free(out->data);
        memset(out, 0, sizeof(*out));
    }
    return rc;
}
