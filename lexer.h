#pragma once

#include <stddef.h>

#include "util.h"

enum token {
    TOKEN_COLON,
    TOKEN_DOTSLASH,
    TOKEN_EQUALS,
    TOKEN_EXCLAMATION,
    TOKEN_NEWLINE,
    TOKEN_QUESTION,
    TOKEN_QUOTE,
    TOKEN_SPACE,

    TOKEN_FLOW,
    TOKEN_IDENT,
    TOKEN_PARAM,
    TOKEN_PARTIAL,
    TOKEN_SHELL,
    TOKEN_STEP,

    TOKEN_EOF,
    TOKEN_ERROR,
};

struct lex_input {
    char* curr;
    char* prev;
    char actual_curr;
    int lineno;
    char* line_p;
};

void lex_input_init(struct lex_input* in, char* buf);

// Store '\0' at the current position. Subsequent lexing will return what the
// unmodified input would have. Returns the (now NUL-terminated) previous token.
char* lex_stuff_null(struct lex_input* in);

enum token lex(struct lex_input* in);
// Recognize keywords instead of ident.
enum token lex_keyword(struct lex_input* in);

int lex_path(struct lex_input* in);

// in->curr should be just after the opening quote. lex_string*() lexes up to
// but not past the closing quote.
//
// Does not advance in. Returns the size of the allocation needed to store the
// unescaped string (including NUL terminator) or 0 on error; if the value is
// negative, then the string need not be escaped (can use lex_stuff_null()).
ssize_t lex_string_alloc(struct lex_input* in);
// If buf is not NULL, the unescaped string will be stored in it; buf must have
// at least lex_string_alloc(in) bytes allocated.
void lex_string(struct lex_input* in, char* buf);
