/*
 * Four Eyes: turns the model's text into proposals.
 *
 * The model is asked for one JSON object:
 *   {"reply": "<words for the driver>", "actions": [{"signal": "<VSS path>", "value": <number|true|false>}, ...]}
 * This parser is strict: anything else is "unparseable" and produces no proposal. It decides nothing. A parsed action
 * whose path this car does not have is still passed on, as signal 0, so the gate records the attempt and refuses it.
 * Plain C99, no allocation, bounded input; fuzzed on the host.
 */
#ifndef FE_PROPOSAL_PARSE_H
#define FE_PROPOSAL_PARSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FE_PARSE_MAX_ACTIONS 8
#define FE_PARSE_MAX_PATH 96
#define FE_PARSE_MAX_REPLY 256

typedef struct {
    uint16_t signal;               /* fe_signal_t, or 0 for a path this car does not have */
    char path[FE_PARSE_MAX_PATH];  /* as the model wrote it, truncated */
    int32_t value;                 /* booleans become 1 and 0; numbers are rounded toward zero and clamped */
} fe_action_t;

typedef enum {
    FE_PARSE_OK = 0,
    FE_PARSE_EMPTY,        /* no JSON object in the text */
    FE_PARSE_SYNTAX,       /* malformed JSON */
    FE_PARSE_SCHEMA,       /* JSON, but not the shape asked for */
    FE_PARSE_TOO_MANY,     /* more actions than FE_PARSE_MAX_ACTIONS */
} fe_parse_status_t;

typedef struct {
    fe_parse_status_t status;
    size_t n_actions;
    fe_action_t actions[FE_PARSE_MAX_ACTIONS];
    char reply[FE_PARSE_MAX_REPLY];
} fe_parsed_t;

/* Parses at most len bytes of text (need not be NUL-terminated). Never reads past len. */
void fe_parse_model_output(const char *text, size_t len, fe_parsed_t *out);
const char *fe_parse_status_name(fe_parse_status_t s);

#endif
