#include "invocation.h"

struct invocation* get_invocation(const struct object_id* oid) {
    return intern_object(oid, OBJ_INVOCATION, sizeof(struct invocation));
}

int parse_invocation_bytes(struct invocation* inv, void* data, size_t size) {
    if (inv->object.is_parsed)
        return 0;
    char* end = (char*)data + size;
    char* p = memmem(data, size, "\n\n", 2);
    if (!p)
        return error("truncated invocation header");
    p += 2;

    struct invocation_entry_list** entry_p = &inv->entries;
    while (p + 132 < end) {
        if (p[1] != ' ' || p[66] != ' ' || p[131] != ' ')
            return error("bad invocation entry");
        struct invocation_entry_list* entry = xmalloc(sizeof(*entry));
        memset(entry, 0, sizeof(*entry));

        if (p[0] == 'f') {
            struct object_id oid;
            if (hex_to_oid(&p[2], &oid) < 0)
                return error("invalid job hash");
            entry->job = get_job(&oid);
            if (hex_to_oid(&p[67], &oid) < 0)
                return error("invalid production hash");
            entry->prd = get_production(&oid);
        } else if (p[0] != 'u') {
            die("unknown invocation entry stage: %c", p[0]);
        }

        p = memchr(p, '\n', end - p);
        if (!p)
            return error("unterminated invocation entry");
        p++;
        *entry_p = inv->terminal = entry;
        entry_p = &entry->next;
    }
    if (p != end)
        return error("truncated invocation entry");
    if (!inv->entries)
        return error("empty invocation");

    inv->object.is_parsed = 1;
    return 0;
}

int parse_invocation(struct invocation* inv) {
    if (inv->object.is_parsed)
        return 0;
    size_t size;
    void* buf = read_object_of_type(&inv->object.oid, OBJ_INVOCATION, &size);
    if (!buf)
        return -1;
    int ret = parse_invocation_bytes(inv, buf, size);
    free(buf);
    return ret;
}
