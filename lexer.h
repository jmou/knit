#pragma once

#include <stddef.h>
#include <stdlib.h>

enum token {
    TOKEN_COLON,
    TOKEN_DOTSLASH,
    TOKEN_EQUALS,
    TOKEN_EXCLAMATION,
    TOKEN_NEWLINE,
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

void die(const char* format, ...);
int error(const char* format, ...);

static inline void* xmalloc(size_t size) {
    void* buf = malloc(size);
    if (!buf)
        die("out of memory in malloc");
    return buf;
}

static inline void* xrealloc(void* ptr, size_t size) {
    void* buf = realloc(ptr, size);
    if (!buf)
        die("out of memory in realloc");
    return buf;
}

struct lex_input {
    char* curr;
    char* prev;
    char actual_curr;
};

// Store '\0' at the current position. Subsequent lexing will return the token
// that the unmodified input would have. Returns in->prev.
char* lex_stuff_null(struct lex_input* in);

enum token lex(struct lex_input* in);
// Recognize keywords instead of ident.
enum token lex_keyword(struct lex_input* in);

struct bytebuf {
    void* data;
    size_t size;
    unsigned should_free : 1;
    unsigned should_munmap : 1;
};

int lex_path(struct lex_input* in);

// *curr should be just after the opening quote.
int lex_string(struct lex_input* in, struct bytebuf* out);
