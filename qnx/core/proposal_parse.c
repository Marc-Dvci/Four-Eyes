/* Four Eyes: turns the model's text into proposals. See proposal_parse.h. */
#include "proposal_parse.h"

#include <string.h>

#include "gate_core.h"

#define MAX_DEPTH 16

typedef struct {
    const char *s;
    size_t n, i;
    bool err;
} cur_t;

static void ws(cur_t *c) {
    while (c->i < c->n && (c->s[c->i] == ' ' || c->s[c->i] == '\t' || c->s[c->i] == '\n' || c->s[c->i] == '\r'))
        c->i++;
}

static bool eat(cur_t *c, char ch) {
    ws(c);
    if (c->i < c->n && c->s[c->i] == ch) {
        c->i++;
        return true;
    }
    return false;
}

static int hexval(char h) {
    if (h >= '0' && h <= '9') return h - '0';
    if (h >= 'a' && h <= 'f') return h - 'a' + 10;
    if (h >= 'A' && h <= 'F') return h - 'A' + 10;
    return -1;
}

/* Appends one byte to a bounded buffer; drops what does not fit. */
static void put(char *buf, size_t cap, size_t *len, char ch) {
    if (buf && *len + 1 < cap) buf[(*len)++] = ch;
}

static void put_utf8(char *buf, size_t cap, size_t *len, uint32_t cp) {
    if (cp < 0x80) {
        put(buf, cap, len, (char)cp);
    } else if (cp < 0x800) {
        put(buf, cap, len, (char)(0xC0 | (cp >> 6)));
        put(buf, cap, len, (char)(0x80 | (cp & 0x3F)));
    } else {
        put(buf, cap, len, (char)(0xE0 | (cp >> 12)));
        put(buf, cap, len, (char)(0x80 | ((cp >> 6) & 0x3F)));
        put(buf, cap, len, (char)(0x80 | (cp & 0x3F)));
    }
}

/* Reads a JSON string into buf (may be NULL to skip). */
static bool str(cur_t *c, char *buf, size_t cap) {
    size_t len = 0;
    if (!eat(c, '"')) return false;
    while (c->i < c->n) {
        char ch = c->s[c->i++];
        if (ch == '"') {
            if (buf && cap) buf[len] = '\0';
            return true;
        }
        if ((unsigned char)ch < 0x20) return false;
        if (ch != '\\') {
            put(buf, cap, &len, ch);
            continue;
        }
        if (c->i >= c->n) return false;
        char e = c->s[c->i++];
        switch (e) {
            case '"': put(buf, cap, &len, '"'); break;
            case '\\': put(buf, cap, &len, '\\'); break;
            case '/': put(buf, cap, &len, '/'); break;
            case 'b': put(buf, cap, &len, '\b'); break;
            case 'f': put(buf, cap, &len, '\f'); break;
            case 'n': put(buf, cap, &len, '\n'); break;
            case 'r': put(buf, cap, &len, '\r'); break;
            case 't': put(buf, cap, &len, '\t'); break;
            case 'u': {
                if (c->i + 4 > c->n) return false;
                uint32_t cp = 0;
                for (int k = 0; k < 4; k++) {
                    int h = hexval(c->s[c->i++]);
                    if (h < 0) return false;
                    cp = (cp << 4) | (uint32_t)h;
                }
                put_utf8(buf, cap, &len, cp);
                break;
            }
            default: return false;
        }
    }
    return false;
}

/* Reads a JSON number as an int32, rounded toward zero and clamped. */
static bool num(cur_t *c, int32_t *out) {
    ws(c);
    size_t start = c->i;
    bool neg = false;
    if (c->i < c->n && c->s[c->i] == '-') {
        neg = true;
        c->i++;
    }
    if (c->i >= c->n || c->s[c->i] < '0' || c->s[c->i] > '9') return false;
    double v = 0;
    while (c->i < c->n && c->s[c->i] >= '0' && c->s[c->i] <= '9') v = v * 10 + (c->s[c->i++] - '0');
    if (c->i < c->n && c->s[c->i] == '.') {
        c->i++;
        if (c->i >= c->n || c->s[c->i] < '0' || c->s[c->i] > '9') return false;
        double scale = 0.1;
        while (c->i < c->n && c->s[c->i] >= '0' && c->s[c->i] <= '9') {
            v += (c->s[c->i++] - '0') * scale;
            scale /= 10;
        }
    }
    if (c->i < c->n && (c->s[c->i] == 'e' || c->s[c->i] == 'E')) {
        c->i++;
        bool eneg = false;
        if (c->i < c->n && (c->s[c->i] == '+' || c->s[c->i] == '-')) eneg = c->s[c->i++] == '-';
        if (c->i >= c->n || c->s[c->i] < '0' || c->s[c->i] > '9') return false;
        int ex = 0;
        while (c->i < c->n && c->s[c->i] >= '0' && c->s[c->i] <= '9') {
            if (ex < 400) ex = ex * 10 + (c->s[c->i] - '0');
            c->i++;
        }
        for (int k = 0; k < ex && v != 0; k++) {
            v = eneg ? v / 10 : v * 10;
            if (v > 1e12) break;
        }
    }
    if (neg) v = -v;
    if (v > 2147483647.0) v = 2147483647.0;
    if (v < -2147483648.0) v = -2147483648.0;
    if (out) *out = (int32_t)v;
    return c->i > start;
}

static bool lit(cur_t *c, const char *word) {
    ws(c);
    size_t k = strlen(word);
    if (c->i + k <= c->n && memcmp(c->s + c->i, word, k) == 0) {
        c->i += k;
        return true;
    }
    return false;
}

static bool skip_value(cur_t *c, int depth);

static bool skip_container(cur_t *c, int depth, char open, char close) {
    if (depth > MAX_DEPTH || !eat(c, open)) return false;
    if (eat(c, close)) return true;
    do {
        if (open == '{') {
            if (!str(c, NULL, 0) || !eat(c, ':')) return false;
        }
        if (!skip_value(c, depth + 1)) return false;
    } while (eat(c, ','));
    return eat(c, close);
}

static bool skip_value(cur_t *c, int depth) {
    ws(c);
    if (c->i >= c->n) return false;
    char ch = c->s[c->i];
    if (ch == '"') return str(c, NULL, 0);
    if (ch == '{') return skip_container(c, depth, '{', '}');
    if (ch == '[') return skip_container(c, depth, '[', ']');
    if (ch == 't') return lit(c, "true");
    if (ch == 'f') return lit(c, "false");
    if (ch == 'n') return lit(c, "null");
    return num(c, NULL);
}

/* One action object. Returns false on a syntax error; sets *schema_ok false if the shape is wrong. */
static bool action(cur_t *c, fe_action_t *a, bool *schema_ok) {
    bool have_sig = false, have_val = false;
    memset(a, 0, sizeof(*a));
    if (!eat(c, '{')) {
        *schema_ok = false;
        return skip_value(c, 2);
    }
    if (!eat(c, '}')) {
        do {
            char key[16];
            if (!str(c, key, sizeof key) || !eat(c, ':')) return false;
            if (strcmp(key, "signal") == 0) {
                ws(c);
                if (c->i < c->n && c->s[c->i] == '"') {
                    if (!str(c, a->path, sizeof a->path)) return false;
                    have_sig = true;
                } else if (!skip_value(c, 3)) {
                    return false;
                }
            } else if (strcmp(key, "value") == 0) {
                ws(c);
                if (lit(c, "true")) {
                    a->value = 1;
                    have_val = true;
                } else if (lit(c, "false")) {
                    a->value = 0;
                    have_val = true;
                } else if (c->i < c->n && (c->s[c->i] == '-' || (c->s[c->i] >= '0' && c->s[c->i] <= '9'))) {
                    if (!num(c, &a->value)) return false;
                    have_val = true;
                } else if (!skip_value(c, 3)) {
                    return false;
                }
            } else if (!skip_value(c, 3)) {
                return false;
            }
        } while (eat(c, ','));
        if (!eat(c, '}')) return false;
    }
    if (!have_sig || !have_val) *schema_ok = false;
    a->signal = fe_signal_from_path(a->path);
    return true;
}

void fe_parse_model_output(const char *text, size_t len, fe_parsed_t *out) {
    memset(out, 0, sizeof(*out));
    if (text == NULL) {
        out->status = FE_PARSE_EMPTY;
        return;
    }
    size_t start = 0;
    while (start < len && text[start] != '{') {
        if (text[start] == '\0') break;
        start++;
    }
    if (start >= len || text[start] != '{') {
        out->status = FE_PARSE_EMPTY;
        return;
    }
    cur_t c = {text, len, start, false};
    bool schema_ok = true, have_actions = false;
    eat(&c, '{');
    if (!eat(&c, '}')) {
        do {
            char key[16];
            if (!str(&c, key, sizeof key) || !eat(&c, ':')) goto syntax;
            if (strcmp(key, "reply") == 0) {
                ws(&c);
                if (c.i < c.n && c.s[c.i] == '"') {
                    if (!str(&c, out->reply, sizeof out->reply)) goto syntax;
                } else if (!skip_value(&c, 1)) {
                    goto syntax;
                }
            } else if (strcmp(key, "actions") == 0) {
                have_actions = true;
                if (!eat(&c, '[')) {
                    schema_ok = false;
                    if (!skip_value(&c, 1)) goto syntax;
                    continue;
                }
                if (!eat(&c, ']')) {
                    do {
                        fe_action_t a;
                        if (!action(&c, &a, &schema_ok)) goto syntax;
                        if (out->n_actions >= FE_PARSE_MAX_ACTIONS) {
                            out->status = FE_PARSE_TOO_MANY;
                            out->n_actions = 0;
                            return;
                        }
                        out->actions[out->n_actions++] = a;
                    } while (eat(&c, ','));
                    if (!eat(&c, ']')) goto syntax;
                }
            } else if (!skip_value(&c, 1)) {
                goto syntax;
            }
        } while (eat(&c, ','));
        if (!eat(&c, '}')) goto syntax;
    }
    if (!have_actions || !schema_ok) {
        out->status = FE_PARSE_SCHEMA;
        out->n_actions = 0;
        return;
    }
    out->status = FE_PARSE_OK;
    return;
syntax:
    out->status = FE_PARSE_SYNTAX;
    out->n_actions = 0;
    out->reply[0] = '\0';
}

const char *fe_parse_status_name(fe_parse_status_t s) {
    switch (s) {
        case FE_PARSE_OK: return "ok";
        case FE_PARSE_EMPTY: return "empty";
        case FE_PARSE_SYNTAX: return "syntax";
        case FE_PARSE_SCHEMA: return "schema";
        case FE_PARSE_TOO_MANY: return "too_many";
    }
    return "invalid";
}
