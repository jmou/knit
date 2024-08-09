#include "production.h"

struct production_header {
    uint8_t job_hash[KNIT_HASH_RAWSZ];
    uint32_t num_outputs;
};

struct output {
    uint8_t res_hash[KNIT_HASH_RAWSZ];
    char path[];
};

int parse_production_bytes(void* data, size_t size, struct production* prd) {
    struct production_header* hdr = data;
    size_t off = sizeof(*hdr);
    if (size < off)
        return error("truncated production header");
    memcpy(prd->job_oid.hash, hdr->job_hash, KNIT_HASH_RAWSZ);
    size_t num_outputs = ntohl(hdr->num_outputs);

    struct resource_list** list_p = &prd->outputs;
    for (uint32_t i = 0; i < num_outputs; i++) {
        struct output* out = (struct output*)((char*)data + off);
        ssize_t nrem = size - off - sizeof(*out);
        if (nrem <= 0)
            return error("truncated production output hash");
        int pathlen = strnlen(out->path, nrem);
        if (pathlen == nrem)
            return error("production output not NUL-terminated");

        struct resource_list* list = xmalloc(sizeof(struct resource_list));
        memcpy(list->oid.hash, out->res_hash, KNIT_HASH_RAWSZ);
        list->path = strdup(out->path);
        list->next = NULL;

        off += sizeof(*out) + pathlen + 1;
        *list_p = list;
        list_p = &list->next;
    }
    if (off != size)
        return error("trailing production data");
    return 0;
}

int write_production(const struct production* prd, struct object_id* oid) {
    size_t size = sizeof(struct production_header);
    for (const struct resource_list* curr = prd->outputs; curr; curr = curr->next)
        size += sizeof(struct output) + strlen(curr->path) + 1;
    char* buf = xmalloc(size);
    char* p = buf;

    struct production_header* hdr = (struct production_header*)p;
    memcpy(hdr->job_hash, prd->job_oid.hash, KNIT_HASH_RAWSZ);
    p += sizeof(*hdr);

    uint32_t count = 0;
    for (const struct resource_list* curr = prd->outputs; curr; curr = curr->next) {
        struct output* out = (struct output*)p;
        memcpy(out->res_hash, curr->oid.hash, KNIT_HASH_RAWSZ);
        size_t pathsize = strlen(curr->path) + 1;
        memcpy(out->path, curr->path, pathsize);
        p += sizeof(*out) + pathsize;
        count++;
    }
    hdr->num_outputs = htonl(count);

    int rc = write_object(TYPE_PRODUCTION, buf, size, oid);
    free(buf);
    return rc;
}

struct resource_list* resource_list_insert(struct resource_list** list_p,
                                           const char* path,
                                           const struct object_id* oid) {
    struct resource_list* list = *list_p;
    while (list && strcmp(list->path, path) < 0) {
        list_p = &list->next;
        list = list->next;
    }
    struct resource_list* node = xmalloc(sizeof(struct resource_list));
    node->path = strdup(path);
    oidcpy(&node->oid, oid);
    node->next = list;
    *list_p = node;
    return node;
}
