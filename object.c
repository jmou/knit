#include "object.h"

#define TRIE_BITS_PER_LEVEL 4
#define TRIE_LEVELS_PER_BYTE (8 / TRIE_BITS_PER_LEVEL)
#define TRIE_BRANCHING_FACTOR (1 << TRIE_BITS_PER_LEVEL)
#define TRIE_MAX_LEVELS (KNIT_HASH_RAWSZ * TRIE_LEVELS_PER_BYTE)

struct object_trie {
    int num_leaves;
    union {
        struct object* leaves[TRIE_BRANCHING_FACTOR];
        struct object_trie* children[TRIE_BRANCHING_FACTOR];
    };
};

static int trie_is_leaf(struct object_trie* trie) { return trie->num_leaves >= 0; }

static size_t trie_child(size_t level, const struct object_id* oid) {
    unsigned char byte = oid->hash[level / TRIE_LEVELS_PER_BYTE];
    int shift = (level % TRIE_LEVELS_PER_BYTE) * TRIE_BITS_PER_LEVEL;
    return byte >> shift;
}

// Do not call with duplicate obj->oid.
static void trie_insert(struct object_trie** trie_p, size_t level, struct object* obj) {
    assert(level < TRIE_MAX_LEVELS);
    struct object_trie* trie;
    if (!*trie_p) {
        *trie_p = trie = xmalloc(sizeof(*trie));
        trie->num_leaves = 0;
        memset(trie->children, 0, sizeof(trie->children));
    }
    trie = *trie_p;
    if (trie->num_leaves == TRIE_BRANCHING_FACTOR) {
        // Make this node internal and redistribute its children.
        struct object* leaves[TRIE_BRANCHING_FACTOR];
        memcpy(leaves, trie->leaves, sizeof(leaves));
        trie->num_leaves = -1;
        for (int i = 0; i < TRIE_BRANCHING_FACTOR; i++)
            trie_insert(trie_p, level, leaves[i]);
    } else if (trie_is_leaf(trie)) {
        trie->leaves[trie->num_leaves++] = obj;
        return;
    }
    trie_insert(&trie->children[trie_child(level, &obj->oid)],
                level + 1, obj);
}

static struct object* trie_find(struct object_trie* trie, const struct object_id* oid) {
    for (size_t level = 0;; level++) {
        assert(level < TRIE_MAX_LEVELS);
        if (!trie)
            break;
        if (trie_is_leaf(trie)) {
            for (int i = 0; i < trie->num_leaves; i++) {
                struct object* leaf = trie->leaves[i];
                if (!memcmp(leaf->oid.hash, oid->hash, KNIT_HASH_RAWSZ))
                    return leaf;
            }
            break;
        }
        trie = trie->children[trie_child(level, oid)];
    }
    return NULL;
}

static struct object_trie* interned_objects;

static void* intern_new_object(const struct object_id* oid, uint32_t typesig, size_t size) {
    struct object* obj = xmalloc(size);
    memset(obj, 0, size);
    memcpy(obj->oid.hash, oid->hash, KNIT_HASH_RAWSZ);
    obj->typesig = typesig;
    obj->is_parsed = 0;
    trie_insert(&interned_objects, 0, obj);
    return obj;
}

void* intern_object(const struct object_id* oid, uint32_t typesig, size_t size) {
    struct object* obj = trie_find(interned_objects, oid);
    if (!obj)
        return intern_new_object(oid, typesig, size);

    if (obj->typesig != typesig)
        die("object %s is type %s, expected %s", oid_to_hex(&obj->oid),
            strtypesig(obj->typesig), strtypesig(typesig));
    return obj;
}
