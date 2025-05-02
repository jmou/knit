#include "cache.h"
#include "spec.h"

static void die_usage(const char* arg0) {
    fprintf(stderr, "usage: %s <job> [<production>]\n", arg0);
    exit(1);
}

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3)
        die_usage(argv[0]);

    struct job* job = peel_job(argv[1]);
    if (!job)
        exit(1);
    if (argc == 2) {
        struct production* prd = read_cache(job);
        if (!prd)
            exit(1);
        puts(oid_to_hex(&prd->object.oid));
    } else {
        struct production* prd = peel_production(argv[2]);
        if (!prd || write_cache(job, prd) < 0)
            exit(1);
    }

    return 0;
}
