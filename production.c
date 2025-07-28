#include "production.h"

#include "hash.h"
#include "invocation.h"

struct production* get_production(const struct object_id* oid) {
    return intern_object(oid, OBJ_PRODUCTION, sizeof(struct production));
}

struct production_header {
    uint8_t job_hash[KNIT_HASH_RAWSZ];
    uint32_t flags;
};

#define PH_INVOCATION 0x80000000
#define PH_OUTPUTS_MASK (~PH_INVOCATION)

struct production_invocation {
    uint8_t inv_hash[KNIT_HASH_RAWSZ];
};

struct output {
    uint8_t res_hash[KNIT_HASH_RAWSZ];
    char name[];
};

int parse_production_bytes(struct production* prd, void* data, size_t size) {
    if (prd->object.is_parsed)
        return 0;
    struct production_header* hdr = (struct production_header*)data;
    size_t off = sizeof(*hdr);
    if (size < off)
        return error("truncated production header");
    prd->job = get_job(oid_of_hash(hdr->job_hash));
    size_t num_outputs = ntohl(hdr->flags) & PH_OUTPUTS_MASK;

    if (ntohl(hdr->flags) & PH_INVOCATION) {
        struct production_invocation* inv_hdr =
            (struct production_invocation*)((char*)data + off);
        prd->inv = get_invocation(oid_of_hash(inv_hdr->inv_hash));
        off += sizeof(*inv_hdr);
    }

    struct resource_list** list_p = &prd->outputs;
    const char* prev_name = "";
    for (uint32_t i = 0; i < num_outputs; i++) {
        struct output* out = (struct output*)((char*)data + off);
        ssize_t nrem = size - off - sizeof(*out);
        if (nrem <= 0)
            return error("truncated production output");
        int pathlen = strnlen(out->name, nrem);
        if (pathlen == nrem)
            return error("production output not NUL-terminated");

        struct resource_list* list = xmalloc(sizeof(*list));
        list->res = get_resource(oid_of_hash(out->res_hash));
        list->name = strdup(out->name);
        list->next = NULL;

        if (strcmp(prev_name, list->name) >= 0)
            return error("production output names not in strict lexicographical order");
        prev_name = list->name;

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
    void* buf = read_object_of_type(&prd->object.oid, OBJ_PRODUCTION, &size);
    if (!buf)
        return -1;
    int ret = parse_production_bytes(prd, buf, size);
    free(buf);
    return ret;
}

struct production* store_production(struct job* job, struct invocation* inv,
                                    struct resource_list* outputs) {
    if (parse_job(job) < 0)
        return NULL;
    size_t size = sizeof(struct production_header);
    if (inv) {
        if (parse_invocation(inv) < 0)
            return NULL;
        size += sizeof(struct production_invocation);
    }
    for (const struct resource_list* curr = outputs; curr; curr = curr->next) {
        if (parse_resource(curr->res) < 0)
            return NULL;
        size += sizeof(struct output) + strlen(curr->name) + 1;
    }

    char* buf = xmalloc(size);
    struct production_header* hdr = (struct production_header*)buf;
    memcpy(hdr->job_hash, job->object.oid.hash, KNIT_HASH_RAWSZ);
    char* p = buf + sizeof(*hdr);

    if (inv) {
        struct production_invocation* inv_hdr = (struct production_invocation*)p;
        memcpy(inv_hdr->inv_hash, inv->object.oid.hash, KNIT_HASH_RAWSZ);
        p += sizeof(*inv_hdr);
    }

    uint32_t count = 0;
    const char* prev_name = "";
    for (const struct resource_list* curr = outputs; curr; curr = curr->next) {
        if (strcmp(prev_name, curr->name) >= 0) {
            error("job input names not in strict lexicographical order");
            return NULL;
        }
        prev_name = curr->name;

        struct output* out = (struct output*)p;
        memcpy(out->res_hash, curr->res->object.oid.hash, KNIT_HASH_RAWSZ);
        size_t pathsize = strlen(curr->name) + 1;
        memcpy(out->name, curr->name, pathsize);
        p += sizeof(*out) + pathsize;
        count++;
    }
    assert(count <= PH_OUTPUTS_MASK);
    hdr->flags = htonl(count | (inv ? PH_INVOCATION : 0));

    struct object_id oid;
    int rc = write_object(OBJ_PRODUCTION, buf, size, &oid);
    free(buf);
    return rc < 0 ? NULL : get_production(&oid);
}
