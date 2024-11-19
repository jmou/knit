#include "hash.h"
#include "spec.h"

static void die_usage(char* arg0) {
    fprintf(stderr, "usage: %s <spec>\n", arg0);
    exit(1);
}

int main(int argc, char** argv) {
    if (argc != 2)
        die_usage(argv[0]);
    struct object* obj = peel_spec(argv[1], strlen(argv[1]));
    // TODO error is ambiguous: bad spec, I/O error, non-existent deref
    if (!obj)
        die("could not parse object specifier");
    puts(oid_to_hex(&obj->oid));
    return 0;
}
