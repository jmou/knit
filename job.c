#include "job.h"

const char* job_process_name(enum job_process process) {
    switch (process) {
    case JOB_PROCESS_CMD:
        return "cmd";
    case JOB_PROCESS_FLOW:
        return "flow";
    case JOB_PROCESS_IDENTITY:
        return "identity";
    case JOB_PROCESS_EXTERNAL:
        return "external";
    case JOB_PROCESS_INVALID:
        break;
    }
    die("invalid job process");
}

static enum job_process parse_job_process(const char* s) {
    if (!strcmp(s, JOB_INPUT_CMD)) {
        return JOB_PROCESS_CMD;
    } else if (!strcmp(s, JOB_INPUT_FLOW)) {
        return JOB_PROCESS_FLOW;
    } else if (!strcmp(s, JOB_INPUT_IDENTITY)) {
        return JOB_PROCESS_IDENTITY;
    } else if (!strcmp(s, JOB_INPUT_EXTERNAL)) {
        return JOB_PROCESS_EXTERNAL;
    } else {
        return JOB_PROCESS_INVALID;
    }
}

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
    const char* prev_name = "";
    for (uint32_t i = 0; i < num_inputs; i++) {
        struct job_input* in = (struct job_input*)((char*)data + off);
        ssize_t nrem = size - off - sizeof(*in);
        if (nrem <= 0)
            return error("truncated job input");
        int pathlen = strnlen(in->name, nrem);
        if (pathlen == nrem)
            return error("job input not NUL-terminated");

        struct resource_list* list = xmalloc(sizeof(*list));
        list->res = get_resource(oid_of_hash(in->res_hash));
        list->name = strdup(in->name);
        list->next = NULL;

        if (strcmp(prev_name, list->name) >= 0)
            return error("job input names not in strict lexicographical order");
        prev_name = list->name;

        enum job_process process = JOB_PROCESS_INVALID;
        if (!strcmp(list->name, JOB_INPUT_NOCACHE)) {
            job->is_nocache = 1;
        } else {
            process = parse_job_process(list->name);
        }
        if (process != JOB_PROCESS_INVALID) {
            if (job->process != JOB_PROCESS_INVALID)
                return error("job has multiple processes");
            job->process = process;
        }

        off += sizeof(*in) + pathlen + 1;
        *list_p = list;
        list_p = &list->next;
    }
    if (off != size)
        return error("trailing job data");

    if (job->process == JOB_PROCESS_INVALID)
        return error("job missing process");

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

struct job* store_job(struct resource_list* inputs) {
    size_t size = sizeof(struct job_header);
    size_t num_inputs = 0;
    for (struct resource_list* curr = inputs; curr; curr = curr->next) {
        if (parse_resource(curr->res) < 0)
            return NULL;
        size += sizeof(struct job_input) + strlen(curr->name) + 1;
        num_inputs++;
    }

    char* buf = xmalloc(size);
    struct job_header* hdr = (struct job_header*)buf;
    hdr->num_inputs = htonl(num_inputs);
    char* p = buf + sizeof(*hdr);

    int has_process = 0;
    const char* prev_name = "";
    for (struct resource_list* curr = inputs; curr; curr = curr->next) {
        if (strcmp(prev_name, curr->name) >= 0) {
            error("job input names not in strict lexicographical order");
            return NULL;
        }
        prev_name = curr->name;

        if (parse_job_process(curr->name) != JOB_PROCESS_INVALID) {
            if (has_process) {
                error("job has multiple processes");
                return NULL;
            }
            has_process = 1;
        }

        struct job_input* in = (struct job_input*)p;
        memcpy(in->res_hash, curr->res->object.oid.hash, KNIT_HASH_RAWSZ);
        size_t pathsize = strlen(curr->name) + 1;
        memcpy(in->name, curr->name, pathsize);
        p += sizeof(*in) + pathsize;
    }
    if (!has_process) {
        error("job missing process");
        return NULL;
    }

    struct object_id oid;
    int rc = write_object(OBJ_JOB, buf, size, &oid);
    free(buf);
    return rc < 0 ? NULL : get_job(&oid);
}
