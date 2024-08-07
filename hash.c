#include "hash.h"

int is_valid_hex_oid(const char* hex) {
    if (strlen(hex) != KNIT_HASH_HEXSZ)
        return 0;
    while (*hex) {
        switch (*hex++) {
        case '0': case '1': case '2': case '3':
        case '4': case '5': case '6': case '7':
        case '8': case '9': case 'a': case 'b':
        case 'c': case 'd': case 'e': case 'f':
            break;
        default:
            return 0;
        }
    }
    return 1;
}

const char* oid_to_hex(const struct object_id* oid) {
    const char HEX_DIGITS[] = "0123456789abcdef";
    static char hex[KNIT_HASH_HEXSZ + 1];
    for (int i = 0; i < KNIT_HASH_RAWSZ; i++) {
        unsigned char byte = oid->hash[i];
        hex[2*i] = HEX_DIGITS[byte >> 4];
        hex[2*i + 1] = HEX_DIGITS[byte & 0xf];
    }
    hex[KNIT_HASH_HEXSZ] = '\0';
    return hex;
}

int oid_path(const char* hex, char* filename) {
    if (snprintf(filename, PATH_MAX, "%s/objects/%c%c/%s",
                 get_knit_dir(), hex[0], hex[1], &hex[2]) >= PATH_MAX)
        return error("path overflow");
    return 0;
}
