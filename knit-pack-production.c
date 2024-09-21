#include "production.h"
#include "resource.h"

static void die_usage(char* arg0) {
    fprintf(stderr, "usage: %s <dir>\n", arg0);
    exit(1);
}

int main(int argc, char** argv) {
    if (argc != 2)
        die_usage(argv[0]);

    const char* dir = argv[1];
    char job_file[PATH_MAX];
    char outdir[PATH_MAX];
    if (snprintf(job_file, PATH_MAX, "%s/job", dir) >= PATH_MAX ||
        snprintf(outdir, PATH_MAX, "%s/out", dir) >= PATH_MAX)
        die("path too long");

    struct bytebuf bb;
    if (slurp_file(job_file, &bb) < 0)
        exit(1);

    // Chomp any trailing newline.
    if (bb.size == KNIT_HASH_HEXSZ + 1 && ((char*)bb.data)[KNIT_HASH_HEXSZ] == '\n')
        ((char*)bb.data)[KNIT_HASH_HEXSZ] = '\0';
    struct object_id job_oid;
    if (strlen(bb.data) != KNIT_HASH_HEXSZ || hex_to_oid(bb.data, &job_oid) < 0)
        die("invalid job hash");

    struct resource_list* outputs = NULL;
    if (resource_list_insert_dir_files(&outputs, outdir) < 0)
        die_errno("directory traversal failed on %s", outdir);

    struct production* prd = store_production(get_job(&job_oid), NULL, outputs);
    if (!prd)
        exit(1);

    puts(oid_to_hex(&prd->object.oid));
    return 0;
}
