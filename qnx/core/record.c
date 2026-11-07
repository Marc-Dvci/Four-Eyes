/* Four Eyes: the hash-chained record. See record.h. */
#include "record.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "sha256.h"

void fe_chain_init(fe_chain_t *c) {
    c->seq = 0;
    memset(c->prev, '0', 64);
    c->prev[64] = '\0';
}

int fe_chain_format(fe_chain_t *c, const char *members, char *line, size_t cap) {
    /* hashed text: {"seq":N,<members>,"prev":"<hex>"} */
    int n = snprintf(line, cap, "{\"seq\":%llu,%s,\"prev\":\"%s\"}", (unsigned long long)c->seq, members, c->prev);
    if (n < 0 || (size_t)n + 77 >= cap) return -1; /* room for ,"hash":"<64>"}\n */
    char hex[65];
    fe_sha256_hex(line, (size_t)n, hex);
    n -= 1; /* drop the closing brace */
    n += snprintf(line + n, cap - (size_t)n, ",\"hash\":\"%s\"}\n", hex);
    memcpy(c->prev, hex, 65);
    c->seq++;
    return n;
}

/* Decodes one UTF-8 sequence. Returns its length (at least 1) and the code point, 0xFFFD if invalid. */
static size_t utf8_next(const unsigned char *s, size_t len, uint32_t *cp) {
    unsigned char b = s[0];
    size_t need;
    uint32_t v, min;
    if (b < 0x80) {
        *cp = b;
        return 1;
    } else if ((b & 0xE0) == 0xC0) {
        need = 1, v = b & 0x1F, min = 0x80;
    } else if ((b & 0xF0) == 0xE0) {
        need = 2, v = b & 0x0F, min = 0x800;
    } else if ((b & 0xF8) == 0xF0) {
        need = 3, v = b & 0x07, min = 0x10000;
    } else {
        *cp = 0xFFFD;
        return 1;
    }
    if (need >= len) {
        *cp = 0xFFFD;
        return 1;
    }
    for (size_t k = 1; k <= need; k++) {
        if ((s[k] & 0xC0) != 0x80) {
            *cp = 0xFFFD;
            return 1;
        }
        v = (v << 6) | (s[k] & 0x3F);
    }
    if (v < min || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) v = 0xFFFD;
    *cp = v;
    return need + 1;
}

static bool emit(char *dst, size_t cap, size_t *o, const char *s, size_t n) {
    if (*o + n + 1 > cap) return false;
    memcpy(dst + *o, s, n);
    *o += n;
    return true;
}

size_t fe_json_escape(const char *src, size_t len, char *dst, size_t cap) {
    size_t o = 0, i = 0;
    if (cap == 0) return 0;
    while (i < len) {
        uint32_t cp;
        size_t k = utf8_next((const unsigned char *)src + i, len - i, &cp);
        char tmp[16];
        int n;
        if (cp == '"') {
            n = snprintf(tmp, sizeof tmp, "\\\"");
        } else if (cp == '\\') {
            n = snprintf(tmp, sizeof tmp, "\\\\");
        } else if (cp >= 0x20 && cp < 0x7F) {
            tmp[0] = (char)cp;
            n = 1;
        } else if (cp < 0x10000) {
            n = snprintf(tmp, sizeof tmp, "\\u%04x", (unsigned)cp);
        } else {
            uint32_t v = cp - 0x10000;
            n = snprintf(tmp, sizeof tmp, "\\u%04x\\u%04x", (unsigned)(0xD800 + (v >> 10)), (unsigned)(0xDC00 + (v & 0x3FF)));
        }
        if (!emit(dst, cap, &o, tmp, (size_t)n)) break;
        i += k;
    }
    dst[o] = '\0';
    return o;
}

static bool invisible(uint32_t cp) {
    return (cp >= 0xE0000 && cp <= 0xE007F)     /* tag characters */
           || (cp >= 0x200B && cp <= 0x200F)    /* zero-width space, joiners, LRM, RLM */
           || (cp >= 0x202A && cp <= 0x202E)    /* bidi embeddings and overrides */
           || (cp >= 0x2060 && cp <= 0x2064)    /* word joiner, invisible operators */
           || (cp >= 0x2066 && cp <= 0x2069)    /* bidi isolates */
           || cp == 0xFEFF || cp == 0x00AD      /* byte order mark, soft hyphen */
           || cp == 0x180E || cp == 0x034F      /* Mongolian vowel separator, combining grapheme joiner */
           || (cp >= 0xFE00 && cp <= 0xFE0F)    /* variation selectors */
           || (cp >= 0xE0100 && cp <= 0xE01EF); /* variation selectors supplement */
}

size_t fe_count_invisible(const char *src, size_t len) {
    size_t count = 0, i = 0;
    while (i < len) {
        uint32_t cp;
        i += utf8_next((const unsigned char *)src + i, len - i, &cp);
        if (invisible(cp)) count++;
    }
    return count;
}

int fe_turn_members(uint32_t turn_id, const char *origin, const char *text, size_t len, char *out, size_t cap) {
    int n = snprintf(out, cap, "\"kind\":\"turn\",\"turn\":%u,\"origin\":\"%s\",\"invisible\":%zu,\"text\":\"",
                     (unsigned)turn_id, origin, fe_count_invisible(text, len));
    if (n < 0 || (size_t)n >= cap) return -1;
    size_t o = (size_t)n;
    /* the exact escaped length, one code point at a time; if it does not fit, refuse rather than cut the text */
    size_t i = 0, need = 0;
    char probe[FE_ESCAPED_MAX(4)];
    while (i < len) {
        uint32_t cp;
        size_t k = utf8_next((const unsigned char *)text + i, len - i, &cp);
        need += fe_json_escape(text + i, k, probe, sizeof probe);
        i += k;
    }
    if (o + need + 2 > cap) return -1;
    o += fe_json_escape(text, len, out + o, cap - o);
    out[o++] = '"';
    out[o] = '\0';
    return (int)o;
}
