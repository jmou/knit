#include "job.h"

struct job* get_job(const struct object_id* oid) {
    return intern_object(oid, OBJ_JOB, sizeof(struct job));
}

int parse_job_bytes(struct job* job, void* data, size_t size) {
    if (job->object.is_parsed)
        return 0;
    struct job_header* hdr = (struct job_header*)data;
    size_t off = sizeof(*hdr);
    if (size < off)
        return error("truncated job header");
    size_t num_inputs = ntohl(hdr->num_inputs);

    struct resource_list** list_p = &job->inputs;
    for (uint32_t i = 0; i < num_inputs; i++) {
        struct job_input* in = (struct job_input*)((char*)data + off);
        ssize_t nrem = size - off - sizeof(*in);
        if (nrem <= 0)
            return error("truncated job input");
        int pathlen = strnlen(in->path, nrem);
        if (pathlen == nrem)
            return error("job input not NUL-terminated");

        struct resource_list* list = xmalloc(sizeof(*list));
        list->res = get_resource(oid_of_hash(in->res_hash));
        list->path = strdup(in->path);
        list->next = NULL;

        off += sizeof(*in) + pathlen + 1;
        *list_p = list;
        list_p = &list->next;
    }
    if (off != size)
        return error("trailing job data");

    job->object.is_parsed = 1;
    return 0;
}

int parse_job(struct job* job) {
    if (job->object.is_parsed)
        return 0;
    size_t size;
    void* buf = read_object_of_type(&job->object.oid, OBJ_JOB, &size);
    if (!buf)
        return -1;
    int ret = parse_job_bytes(job, buf, size);
    free(buf);
    return ret;
}
