#include "production.h"

struct production* get_production(const struct object_id* oid) {
    return intern_object(oid, TYPE_PRODUCTION, sizeof(struct production));
}

struct production_header {
    uint8_t job_hash[KNIT_HASH_RAWSZ];
    uint32_t num_outputs;
};

struct output {
    uint8_t res_hash[KNIT_HASH_RAWSZ];
    char path[];
};

int parse_production_bytes(struct production* prd, void* data, size_t size) {
    if (prd->object.is_parsed)
        return 0;
    struct production_header* hdr = (struct production_header*)data;
    size_t off = sizeof(*hdr);
    if (size < off)
        return error("truncated production header");
    prd->job = get_job(oid_of_hash(hdr->job_hash));
    size_t num_outputs = ntohl(hdr->num_outputs);

    struct resource_list** list_p = &prd->outputs;
    for (uint32_t i = 0; i < num_outputs; i++) {
        struct output* out = (struct output*)((char*)data + off);
        ssize_t nrem = size - off - sizeof(*out);
        if (nrem <= 0)
            return error("truncated production output");
        int pathlen = strnlen(out->path, nrem);
        if (pathlen == nrem)
            return error("production output not NUL-terminated");

        struct resource_list* list = xmalloc(sizeof(*list));
        list->res = get_resource(oid_of_hash(out->res_hash));
        list->path = strdup(out->path);
        list->next = NULL;

        off += sizeof(*out) + pathlen + 1;
        *list_p = list;
        list_p = &list->next;
    }
    if (off != size)
        return error("trailing production data");

    prd->object.is_parsed = 1;
    return 0;
}

int parse_production(struct production* prd) {
    if (prd->object.is_parsed)
        return 0;
    size_t size;
    void* buf = read_object_of_type(&prd->object.oid, TYPE_PRODUCTION, &size);
    if (!buf)
        return -1;
    int ret = parse_production_bytes(prd, buf, size);
    free(buf);
    return ret;
}

struct production* store_production(struct job* job, struct resource_list* outputs) {
    size_t size = sizeof(struct production_header);
    for (const struct resource_list* curr = outputs; curr; curr = curr->next)
        size += sizeof(struct output) + strlen(curr->path) + 1;

    char* buf = xmalloc(size);
    struct production_header* hdr = (struct production_header*)buf;
    memcpy(hdr->job_hash, job->object.oid.hash, KNIT_HASH_RAWSZ);
    char* p = buf + sizeof(*hdr);

    uint32_t count = 0;
    for (const struct resource_list* curr = outputs; curr; curr = curr->next) {
        struct output* out = (struct output*)p;
        memcpy(out->res_hash, curr->res->object.oid.hash, KNIT_HASH_RAWSZ);
        size_t pathsize = strlen(curr->path) + 1;
        memcpy(out->path, curr->path, pathsize);
        p += sizeof(*out) + pathsize;
        count++;
    }
    hdr->num_outputs = htonl(count);

    struct object_id oid;
    int rc = write_object(TYPE_PRODUCTION, buf, size, &oid);
    free(buf);
    return rc < 0 ? NULL : get_production(&oid);
}
