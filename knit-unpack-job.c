#include "job.h"
#include "resource.h"
#include "util.h"
#include "spec.h"

static char* joindir(const char* dir, const char* suffix) {
    static char buf[PATH_MAX];
    if (snprintf(buf, PATH_MAX, "%s/%s", dir, suffix) >= PATH_MAX)
        die("path too long: %s/%s", dir, suffix);
    return buf;
}

void mkdir_or_die(const char* dir) {
    if (mkdir(dir, 0777) < 0)
        die_errno("cannot mkdir %s", dir);
}

int mkparents(const char* dir, const char* filename) {
    char subdir[PATH_MAX];
    for (int i = 0; filename[i] != '\0'; i++) {
        if (i >= PATH_MAX)
            return error("path too long: %s/%s", dir, filename);
        if (filename[i] == '/') {
            subdir[i] = '\0';
            if (mkdir(joindir(dir, subdir), 0777) < 0 && errno != EEXIST)
                return error_errno("cannot mkdir %s/%s", dir, subdir);
        }
        subdir[i] = filename[i];
    }
    return 0;
}

static void die_usage(char* arg0) {
    fprintf(stderr, "usage: %s --scratch <job> <dir>\n", arg0);
    exit(1);
}

int main(int argc, char** argv) {
    if (argc != 4 || strcmp(argv[1], "--scratch"))
        die_usage(argv[0]);

    struct job* job = peel_job(argv[2]);
    if (!job || parse_job(job) < 0)
        exit(1);
    char* dir = argv[3];

    mkdir_or_die(dir);
    mkdir_or_die(joindir(dir, "work"));
    mkdir_or_die(joindir(dir, "work/out"));
    mkdir_or_die(joindir(dir, "out.knit"));

    char inputs_dir[PATH_MAX];
    if (snprintf(inputs_dir, PATH_MAX, "%s/%s", dir, "work/in") >= PATH_MAX)
        die("path too long: %s/work/in", dir);
    mkdir_or_die(inputs_dir);

    for (struct resource_list* input = job->inputs; input; input = input->next) {
        if (mkparents(inputs_dir, input->name) < 0)
            exit(1);

        size_t size;
        char* buf = read_object_of_type(&input->res->object.oid, OBJ_RESOURCE, &size);
        if (!buf)
            exit(1);

        int fd = creat(joindir(inputs_dir, input->name), 0666);
        if (fd < 0)
            die_errno("cannot open %s/%s", inputs_dir, input->name);

        size_t offset = 0;
        while (offset < size) {
            int n = xwrite(fd, buf + offset, size - offset);
            if (n < 0)
                die_errno("write failed %s/%s", inputs_dir, input->name);
            offset += n;
        }

        if (close(fd) < 0)
            die_errno("close failed %s/%s", inputs_dir, input->name);
        free(buf);
    }
}
