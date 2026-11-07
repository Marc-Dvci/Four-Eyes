/*
 * Four Eyes: the gate's decision core.
 *
 * Plain C99 with no operating-system calls, so the same file is compiled into the QNX gate (gated) and into the host
 * evaluator (fe-eval) that the tests drive. The model proposes a signal and a value; this code decides. Nothing here
 * reads text: a verdict depends only on the proposal's numbers, the request's origin as the gate itself labelled it,
 * the vehicle state from the bus, the clock and the rate counters.
 */
#ifndef FE_GATE_CORE_H
#define FE_GATE_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fe_signals.h"

typedef enum {
    FE_ORIGIN_NONE = 0,
    FE_ORIGIN_DRIVER = 1,   /* typed by the driver at the head unit, with no outside text in it */
    FE_ORIGIN_MESSAGE = 2,  /* carries text from outside the car: an incoming message, or a request quoting one */
} fe_origin_t;

/* Why a proposal was refused. Stable numbers: the record and the attack set's labels use them. */
typedef enum {
    FE_OK = 0,
    FE_R_RATE = 1,             /* the agent is over its rate limit */
    FE_R_UNKNOWN_SIGNAL = 2,   /* not a vehicle signal this car has */
    FE_R_NOT_ALLOWED = 3,      /* a real signal, but not on the agent's allowlist */
    FE_R_OUT_OF_RANGE = 4,     /* value outside the signal's bounds */
    FE_R_NO_TURN = 5,          /* no open request from the head unit, or a stale request id */
    FE_R_FROM_MESSAGE = 6,     /* the open request carries outside text, so it may not move anything */
    FE_R_TURN_BUDGET = 7,      /* too many actions for one request */
    FE_R_STATE_STALE = 8,      /* the gate has not heard the vehicle state recently: fail closed */
    FE_R_UNLOCK_MOVING = 9,    /* door unlock unless stationary and in Park or with the parking brake on */
    FE_R_TRUNK_MOVING = 10,    /* trunk open unless stationary and in Park */
    FE_R_WINDOW_SPEED = 11,    /* window opened beyond the limit at high speed */
    FE_R_COUNT
} fe_reason_t;

typedef struct {
    uint16_t speed_dkmh;   /* Vehicle.Speed in 0.1 km/h */
    int8_t gear;           /* SelectedGear */
    bool parking_brake;    /* ParkingBrake.IsEngaged */
    uint8_t locks;         /* bit set = locked, as in the STATE frame */
    bool trunk_open;
    uint8_t window[4];     /* percent */
    int8_t hvac_temp;
} fe_vehicle_t;

typedef struct {
    uint32_t turn_id;  /* the request this proposal answers */
    uint16_t signal;   /* fe_signal_t; any number is accepted here and judged */
    int32_t value;
} fe_proposal_t;

typedef struct {
    bool allowed;
    fe_reason_t reason;
} fe_verdict_t;

/* Every limit the gate applies, in one place, so the policy table is reviewable and testable. */
typedef struct {
    uint32_t rate_capacity;        /* proposals the agent may burst */
    uint32_t rate_refill_ms;       /* one more proposal allowed every this many ms */
    uint32_t turn_budget;          /* actions allowed per request */
    uint32_t turn_timeout_ms;      /* a request closes by itself after this long */
    uint32_t state_max_age_ms;     /* vehicle state older than this is stale */
    uint16_t stationary_dkmh;      /* at or below this speed the vehicle counts as stationary */
    uint16_t window_fast_dkmh;     /* above this speed a window may open at most window_fast_max */
    uint8_t window_fast_max;
    int8_t hvac_min, hvac_max;
    uint32_t rules;                /* FE_RULE_* bits in force; FE_RULES_ALL in the product, fewer only for ablations */
} fe_limits_t;

/* Rule groups, so the evaluation can measure what each one adds. The allowlist is always in force. */
#define FE_RULE_BOUNDS 0x01u  /* value ranges */
#define FE_RULE_TURN 0x02u    /* an open request, within its budget */
#define FE_RULE_ORIGIN 0x04u  /* a request carrying outside text moves nothing */
#define FE_RULE_STATE 0x08u   /* fresh vehicle state, and the speed, gear and brake conditions */
#define FE_RULE_RATE 0x10u    /* the token bucket */
#define FE_RULES_ALL 0x1Fu

typedef struct {
    fe_limits_t lim;
    /* rate limiter: a token bucket in thousandths of a token, so refill needs no division at decision time */
    uint64_t tokens_milli;
    uint64_t last_refill_ms;
    /* the one open request */
    uint32_t turn_id;
    fe_origin_t turn_origin;
    uint32_t turn_used;
    uint64_t turn_opened_ms;
    bool turn_open;
    uint32_t next_turn_id;
    /* the latest vehicle state from the bus */
    fe_vehicle_t vehicle;
    uint64_t vehicle_ms;
    bool vehicle_known;
} fe_gate_t;

void fe_limits_default(fe_limits_t *lim);
size_t fe_gate_sizeof(void); /* for callers that hold the gate as an opaque buffer (the host bindings) */
void fe_gate_init(fe_gate_t *g, const fe_limits_t *lim, uint64_t now_ms);

/* Opens a request from the head unit; any request still open is closed. Returns its id (never 0). */
uint32_t fe_gate_open_turn(fe_gate_t *g, fe_origin_t origin, uint64_t now_ms);
void fe_gate_close_turn(fe_gate_t *g, uint32_t turn_id);
void fe_gate_update_vehicle(fe_gate_t *g, const fe_vehicle_t *v, uint64_t now_ms);

/* Judges one proposal. Consumes a rate token whatever the outcome, and a turn action if it is allowed. */
fe_verdict_t fe_gate_decide(fe_gate_t *g, const fe_proposal_t *p, uint64_t now_ms);

/* Signal table lookups. */
bool fe_signal_known(uint16_t signal);
bool fe_signal_writable(uint16_t signal);
const char *fe_signal_path(uint16_t signal);   /* VSS path, or "unknown" */
uint16_t fe_signal_from_path(const char *path); /* FE_SIG_NONE if not a known path */
const char *fe_reason_text(fe_reason_t r);
const char *fe_reason_code(fe_reason_t r);      /* short stable name, e.g. "unlock_moving" */
const char *fe_origin_name(fe_origin_t o);

/* Applies an allowed command to a vehicle state, as the body controller does. */
void fe_vehicle_apply(fe_vehicle_t *v, uint16_t signal, int32_t value);

#endif
