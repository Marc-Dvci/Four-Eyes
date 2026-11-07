/*
 * Four Eyes: the hash-chained record of every proposal and verdict.
 *
 * Each line is one JSON object. Its members are "seq", the entry's own members, "prev" (the previous line's hash, 64
 * zeros for the first) and, last, "hash". The hash is SHA-256 over the line's text with the ',"hash":"..."' member
 * removed, that is over '{"seq":...,"prev":"..."}'. Changing, removing or reordering any line breaks every hash after
 * it. tools/fe_verify.py checks a record with nothing but Python's standard library.
 */
#ifndef FE_RECORD_H
#define FE_RECORD_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t seq;   /* sequence number of the next line */
    char prev[65];  /* hash of the last line written */
} fe_chain_t;

void fe_chain_init(fe_chain_t *c);

/*
 * Formats one line from members, a comma-separated list of JSON members without braces, e.g. "\"kind\":\"verdict\"".
 * Writes a NUL-terminated line ending in '\n' and advances the chain. Returns the line's length, or -1 if it would not
 * fit in cap (the chain is then unchanged).
 */
int fe_chain_format(fe_chain_t *c, const char *members, char *line, size_t cap);

/*
 * Escapes text as the inside of a JSON string. Everything outside printable ASCII is written as \uXXXX (surrogate
 * pairs above U+FFFF), so invisible characters stay visible in the record. Invalid UTF-8 becomes \ufffd.
 * Always NUL-terminates; returns the number of bytes written, without the NUL.
 */
size_t fe_json_escape(const char *src, size_t len, char *dst, size_t cap);

/* Counts the code points a reader cannot see: format characters, tag characters, zero-width and bidi controls. */
size_t fe_count_invisible(const char *src, size_t len);

/* fe_json_escape writes at most 6 bytes per input byte (a control byte or an invalid byte becomes \u00XX or \ufffd;
 * 4-byte UTF-8 becomes a 12-byte surrogate pair), plus the NUL. A buffer of FE_ESCAPED_MAX(n) bytes never cuts n. */
#define FE_ESCAPED_MAX(n) ((size_t)(n) * 6u + 1u)
/* The longest request text the gate takes (FE_TEXT_MAX in fe_gate_msg.h must not exceed it). */
#define FE_RECORD_TEXT_MAX 1024
/* A record line holding the longest request, escaped, with room for t_ms, seq, prev and hash. */
#define FE_RECORD_LINE_MAX 8192

/*
 * Formats the members of a "turn" line: id, origin, the number of invisible code points and the whole text, escaped.
 * Returns the length written, or -1 if the members would not fit in cap. It never writes a shortened text.
 */
int fe_turn_members(uint32_t turn_id, const char *origin, const char *text, size_t len, char *out, size_t cap);

#endif
