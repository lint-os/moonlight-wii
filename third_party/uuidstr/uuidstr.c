#include "uuidstr.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>

static unsigned int uuid_seed;

static unsigned int uuid_next(void) {
    unsigned int x;

    if (!uuid_seed)
        uuid_seed = (unsigned int)time(NULL) ^ (unsigned int)(unsigned long)&uuid_seed;

    x = uuid_seed;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    uuid_seed = x;
    return x;
}

bool uuidstr_random(uuidstr_t *dest) {
    static const char hex[] = "0123456789abcdef";
    char *d = dest->data;
    int i, pos = 0;

    for (i = 0; i < 32; i++) {
        if (i == 8 || i == 12 || i == 16 || i == 20)
            d[pos++] = '-';
        d[pos++] = hex[(uuid_next() >> 28) & 0xF];
    }

    dest->zero = 0;
    return true;
}

void uuidstr_fromstr(uuidstr_t *dest, const char *src) {
    memcpy(dest->data, src, UUIDSTR_LENGTH);
    dest->zero = 0;
}

void uuidstr_fromchars(uuidstr_t *dest, size_t len, const char *src) {
    if (len != UUIDSTR_LENGTH) {
        dest->data[0] = 0;
        return;
    }
    memcpy(dest, src, UUIDSTR_LENGTH);
    dest->zero = 0;
}

char *uuidstr_tostr(const uuidstr_t *src) {
    char *str = calloc(UUIDSTR_CAPACITY, sizeof(char));
    memcpy(str, src->data, UUIDSTR_LENGTH);
    return str;
}

bool uuidstr_t_equals_s(const uuidstr_t *a, const char *b) {
    return strncasecmp(a->data, b, UUIDSTR_LENGTH) == 0;
}

bool uuidstr_t_equals_t(const uuidstr_t *a, const uuidstr_t *b) {
    return strncasecmp(a->data, b->data, UUIDSTR_LENGTH) == 0;
}

bool uuidstr_is_empty(const uuidstr_t *uuid) {
    return uuid->data[0] == '0';
}