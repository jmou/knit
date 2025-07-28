// Stack-based interpreter to build resources, jobs, and productions. Intended
// to be more performant than writing files to disk and running processes (like
// knit-hash-object) multiple times.

#include "alloc.h"
#include "hash.h"
#include "invocation.h"
#include "job.h"
#include "production.h"
#include "resource.h"
#include "util.h"

enum operand_type {
    OPND_RESOURCE,
    OPND_RESOURCE_LIST,
    OPND_JOB,
    OPND_PRODUCTION,
    OPND_INVOCATION,
};

struct operand_list {
    enum operand_type type;
    union {
        struct resource* res;
        struct resource_list* list;
        struct job* job;
        struct production* prd;
        struct invocation* inv;
    };
    struct operand_list* next;
};

static int removeprefix(char** s, const char* prefix) {
    size_t prefixlen = strlen(prefix);
    if (strncmp(*s, prefix, prefixlen))
        return 0;
    *s += prefixlen;
    return 1;
}

static void die_usage(const char* arg0) {
    fprintf(stderr, "usage: %s < <build-instructions>\n", arg0);
    exit(1);
}

int main(int argc, char** argv) {
    if (argc != 1)
        die_usage(argv[0]);

    struct bump_list* bump = NULL;;
    struct operand_list* operands = NULL;

    char* line = NULL;
    size_t size = 0;
    ssize_t nread;
    while (errno = 0, (nread = getline(&line, &size, stdin)) >= 0) {
        if (memchr(line, '\0', nread))
            die("illegal NUL byte in instruction");
        if (line[nread - 1] != '\n')
            die("unterminated line");
        line[nread - 1] = '\0';

        char* s = line;
        if (removeprefix(&s, "push ")) {
            struct operand_list* opnd = bump_alloc(&bump, sizeof(*opnd));
            if (!strcmp(s, "list")) {
                opnd->type = OPND_RESOURCE_LIST;
                opnd->list = NULL;
                opnd->next = operands;
                operands = opnd;
                continue;
            }

            char* hex = strchr(s, ' ');
            if (hex) *hex++ = '\0';
            struct object_id oid;
            if (!hex || strlen(hex) != KNIT_HASH_HEXSZ || hex_to_oid(hex, &oid) < 0)
                die("expected: push (resource|job|production|invocation) <oid>");

            if (!strcmp(s, "resource")) {
                opnd->type = OPND_RESOURCE;
                opnd->res = get_resource(&oid);
            } else if (!strcmp(s, "job")) {
                opnd->type = OPND_JOB;
                opnd->job = get_job(&oid);
            } else if (!strcmp(s, "production")) {
                opnd->type = OPND_PRODUCTION;
                opnd->prd = get_production(&oid);
            } else if (!strcmp(s, "invocation")) {
                opnd->type = OPND_INVOCATION;
                opnd->inv = get_invocation(&oid);
            } else {
                die("push with unknown object");
            }
            opnd->next = operands;
            operands = opnd;
        } else if (removeprefix(&s, "pop ")) {
            if (!strcmp(s, "resource")) {
                if (!operands || operands->type != OPND_RESOURCE)
                    die("expected resource operand");
                puts(oid_to_hex(&operands->res->object.oid));
            } else if (!strcmp(s, "job")) {
                if (!operands || operands->type != OPND_JOB)
                    die("expected job operand");
                puts(oid_to_hex(&operands->job->object.oid));
            } else if (!strcmp(s, "production")) {
                if (!operands || operands->type != OPND_PRODUCTION)
                    die("expected production operand");
                puts(oid_to_hex(&operands->prd->object.oid));
            } else if (!strcmp(s, "invocation")) {
                if (!operands || operands->type != OPND_INVOCATION)
                    die("expected invocation operand");
                puts(oid_to_hex(&operands->inv->object.oid));
            } else {
                die("push with unknown object");
            }
            operands = operands->next;
        } else if (removeprefix(&s, "resource ")) {
            char* end;
            unsigned long size = strtoul(s, &end, 10);
            if (size == ULONG_MAX || s == end || *end != '\0')
                die("expected: resource <size>");

            char data[size];
            if (fread(data, 1, size, stdin) != size)
                die("failed to read resource data");

            struct operand_list* opnd = bump_alloc(&bump, sizeof(*opnd));
            opnd->type = OPND_RESOURCE;
            opnd->res = store_resource(data, size);
            if (!opnd->res)
                die("failed to store resource");
            opnd->next = operands;
            operands = opnd;
        } else if (removeprefix(&s, "entry ")) {
            if (!operands || operands->type != OPND_RESOURCE)
                die("expected resource operand");
            struct resource_list* list = bump_alloc(&bump, sizeof(*list));
            list->name = bump_alloc(&bump, strlen(s) + 1);
            strcpy(list->name, s);
            list->res = operands->res;
            list->next = NULL;
            operands->type = OPND_RESOURCE_LIST;
            operands->list = list;
        } else if (removeprefix(&s, "link ")) {
            char* end;
            unsigned long count = strtoul(s, &end, 10);
            if (count == ULONG_MAX || s == end || *end != '\0')
                die("expected: link <count>");

            for (; count; count--) {
                if (!operands || operands->type != OPND_RESOURCE_LIST)
                    die("expected resource list first operand");
                struct resource_list* tail = operands->list;
                operands = operands->next;
                if (!operands || operands->type != OPND_RESOURCE_LIST ||
                        operands->list->next)
                    die("expected loose resource list second operand");
                operands->list->next = tail;
            }
        } else if (!strcmp(s, "job")) {
            if (!operands || operands->type != OPND_RESOURCE_LIST)
                die("expected resource list operand");
            operands->type = OPND_JOB;
            operands->job = store_job(operands->list);
            if (!operands->job)
                die("failed to store job");
        } else if (!strcmp(s, "production")) {
            if (!operands || operands->type != OPND_RESOURCE_LIST)
                die("expected resource list first operand");
            struct resource_list* list = operands->list;
            operands = operands->next;
            if (!operands || operands->type != OPND_JOB)
                die("expected job second operand");
            struct job* job = operands->job;

            operands->type = OPND_PRODUCTION;
            operands->prd = store_production(job, NULL, list);
            if (!operands->prd)
                die("failed to store production");
        } else {
            die("invalid command: %s", s);
        }
    }
    free(line);
    if (nread < 0 && errno > 0)
        die_errno("cannot read stdin");

    if (operands)
        die("unconsumed operands");

    return 0;
}
