#include <string.h>

#include "lexer.h"
#include "util.h"

void lex_input_init(struct lex_input* in, char* buf) {
    memset(in, 0, sizeof(*in));
    in->line_p = in->curr = buf;
    in->lineno = 1;
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
            ident = [a-zA-Z_][a-zA-Z0-9_.@-]*;
            comment = [ ]*[#][^\n\x00]*;

            comment? [\r]?[\n] { in->lineno++; in->line_p = in->curr; token = TOKEN_NEWLINE; break; }
            [ ]+ { token = TOKEN_SPACE; break; }
            [!] { token = TOKEN_EXCLAMATION; break; }
            ["] { token = TOKEN_QUOTE; break; }
            [:] { token = TOKEN_COLON; break; }
            [=] { token = TOKEN_EQUALS; break; }
            [?] { token = TOKEN_QUESTION; break; }
            "./" { token = TOKEN_DOTSLASH; break; }
            [\x00] { token = TOKEN_EOF; break; }
            * { token = TOKEN_ERROR; break; }
        */
        // TODO loosen ident to accommodate non-ASCII?
        /*!use:re2c
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
            "cmd" { token = TOKEN_CMD; break; }
            "identity" { token = TOKEN_IDENTITY; break; }
            "nocache" { token = TOKEN_NOCACHE; break; }
            "params" { token = TOKEN_PARAMS; break; }
            "flow" { token = TOKEN_FLOW; break; }
        */
    }
    post_lex(in);
    return token;
}

int lex_path_or_empty(struct lex_input* in) {
    pre_lex(in);
    while (1) {
        char* rew = in->curr;
        /*!re2c
            component = [a-zA-Z0-9_.-]+;

            (".knit") [/]? { return error("reserved path component"); }
            ("." | "..") [/]? { return error("invalid path component"); }
            component [/] { continue; }
            component { break; }
            * { in->curr = rew; break; }
        */
    }
    post_lex(in);
    return 0;
}

int lex_path(struct lex_input* in) {
    if (lex_path_or_empty(in) < 0)
        return -1;
    if (in->curr == in->prev)
        return error("expected path");
    return 0;
}

static int lex_string_internal(struct lex_input* in, char* buf, ssize_t* size) {
    pre_lex(in);

    int rc;
    int needs_copy = 0;
    size_t i = 0;
    for (char c;; i++) {
        c = *in->curr;
        /*!re2c
            ["] { rc = 0; in->curr--; break; }
            [^"\\\n\x00] { goto append; }
            "\\\"" { c = '"';  needs_copy = 1; goto append; }
            "\\\\" { c = '\\'; needs_copy = 1; goto append; }
            "\\n"  { c = '\n'; needs_copy = 1; goto append; }
            "\\0" [0]?[0]? { c = '\0';  needs_copy = 1; goto append; }
            [\\][0]?[0]?[1-7] { rc = error("unsupported octal escape"); break; }
            [\\][^\x00] { rc = error("invalid string escape"); break; }
            * { rc = error("unterminated string"); break; }
        */
append:
        if (buf)
            buf[i] = c;
    }
    if (buf)
        buf[i] = '\0';
    if (size)
        *size = needs_copy ? i + 1 : -i - 1;

    post_lex(in);
    return rc;
}

ssize_t lex_string_alloc(struct lex_input* in) {
    ssize_t size;
    struct lex_input tmp_in;
    memcpy(&tmp_in, in, sizeof(tmp_in));
    if (lex_string_internal(&tmp_in, NULL, &size) < 0)
        return 0;
    return size;
}

void lex_string(struct lex_input* in, char* buf) {
    int rc = lex_string_internal(in, buf, NULL);
    assert(rc == 0); // should succeed if lex_string_alloc() succeeds
}
