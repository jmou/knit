#include "alloc.h"

#include <stdalign.h>

#define BUMP_PAGE_SIZE (1 << 20)

void* bump_alloc(struct bump_list** bump_p, size_t size) {
    struct bump_list* bump = *bump_p;
    if (!*bump_p || (*bump_p)->nused + size < BUMP_PAGE_SIZE) {
        bump = xmalloc(sizeof(*bump));
        bump->base = xmalloc(BUMP_PAGE_SIZE > size ? BUMP_PAGE_SIZE : size);
        bump->nused = 0;
        bump->next = *bump_p;
        *bump_p = bump;
    }
    void* ret = bump->base + bump->nused;
    size_t align = alignof(max_align_t);
    bump->nused += (size + align - 1) / align * align;
    return ret;
}
