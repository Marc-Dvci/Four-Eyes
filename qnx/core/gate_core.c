/* Four Eyes: the gate's decision core. See gate_core.h. */
#include "gate_core.h"

#include <string.h>

typedef struct {
    uint16_t id;
    const char *path;
    bool writable;
    int32_t min, max;
} sig_row_t;

/* The allowlist and the bounds. Sensors are listed so the gate can name them when it refuses a write. */
static const sig_row_t SIGNALS[] = {
    {FE_SIG_SPEED, "Vehicle.Speed", false, 0, 3000},
    {FE_SIG_SELECTED_GEAR, "Vehicle.Powertrain.Transmission.SelectedGear", false, -1, 127},
    {FE_SIG_PARKING_BRAKE, "Vehicle.Chassis.ParkingBrake.IsEngaged", false, 0, 1},
    {FE_SIG_LOCK_ROW1_DRIVER, "Vehicle.Cabin.Door.Row1.DriverSide.IsLocked", true, 0, 1},
    {FE_SIG_LOCK_ROW1_PASSENGER, "Vehicle.Cabin.Door.Row1.PassengerSide.IsLocked", true, 0, 1},
    {FE_SIG_LOCK_ROW2_DRIVER, "Vehicle.Cabin.Door.Row2.DriverSide.IsLocked", true, 0, 1},
    {FE_SIG_LOCK_ROW2_PASSENGER, "Vehicle.Cabin.Door.Row2.PassengerSide.IsLocked", true, 0, 1},
    {FE_SIG_WINDOW_ROW1_DRIVER, "Vehicle.Cabin.Door.Row1.DriverSide.Window.Position", true, 0, 100},
    {FE_SIG_WINDOW_ROW1_PASSENGER, "Vehicle.Cabin.Door.Row1.PassengerSide.Window.Position", true, 0, 100},
    {FE_SIG_WINDOW_ROW2_DRIVER, "Vehicle.Cabin.Door.Row2.DriverSide.Window.Position", true, 0, 100},
    {FE_SIG_WINDOW_ROW2_PASSENGER, "Vehicle.Cabin.Door.Row2.PassengerSide.Window.Position", true, 0, 100},
    {FE_SIG_TRUNK_OPEN, "Vehicle.Body.Trunk.Rear.IsOpen", true, 0, 1},
    {FE_SIG_HVAC_TEMP_DRIVER, "Vehicle.Cabin.HVAC.Station.Row1.Driver.Temperature", true, 16, 28},
};
#define N_SIGNALS (sizeof(SIGNALS) / sizeof(SIGNALS[0]))

static const sig_row_t *row_of(uint16_t signal) {
    for (unsigned i = 0; i < N_SIGNALS; i++)
        if (SIGNALS[i].id == signal) return &SIGNALS[i];
    return NULL;
}

bool fe_signal_known(uint16_t signal) { return row_of(signal) != NULL; }

bool fe_signal_writable(uint16_t signal) {
    const sig_row_t *r = row_of(signal);
    return r != NULL && r->writable;
}

const char *fe_signal_path(uint16_t signal) {
    const sig_row_t *r = row_of(signal);
    return r ? r->path : "unknown";
}

uint16_t fe_signal_from_path(const char *path) {
    if (path == NULL) return FE_SIG_NONE;
    for (unsigned i = 0; i < N_SIGNALS; i++)
        if (strcmp(SIGNALS[i].path, path) == 0) return SIGNALS[i].id;
    return FE_SIG_NONE;
}

static const char *const REASON_TEXT[FE_R_COUNT] = {
    [FE_OK] = "allowed",
    [FE_R_RATE] = "the agent is over its rate limit",
    [FE_R_UNKNOWN_SIGNAL] = "not a signal of this vehicle",
    [FE_R_NOT_ALLOWED] = "not on the agent's allowlist",
    [FE_R_OUT_OF_RANGE] = "value out of range",
    [FE_R_NO_TURN] = "no open request from the driver",
    [FE_R_FROM_MESSAGE] = "the request carries outside text, so it may not move anything",
    [FE_R_TURN_BUDGET] = "too many actions for one request",
    [FE_R_STATE_STALE] = "vehicle state unknown",
    [FE_R_UNLOCK_MOVING] = "door unlock while the vehicle is not parked",
    [FE_R_TRUNK_MOVING] = "trunk open while the vehicle is not parked",
    [FE_R_WINDOW_SPEED] = "window opened too far at high speed",
};

static const char *const REASON_CODE[FE_R_COUNT] = {
    [FE_OK] = "ok",
    [FE_R_RATE] = "rate",
    [FE_R_UNKNOWN_SIGNAL] = "unknown_signal",
    [FE_R_NOT_ALLOWED] = "not_allowed",
    [FE_R_OUT_OF_RANGE] = "out_of_range",
    [FE_R_NO_TURN] = "no_turn",
    [FE_R_FROM_MESSAGE] = "from_message",
    [FE_R_TURN_BUDGET] = "turn_budget",
    [FE_R_STATE_STALE] = "state_stale",
    [FE_R_UNLOCK_MOVING] = "unlock_moving",
    [FE_R_TRUNK_MOVING] = "trunk_moving",
    [FE_R_WINDOW_SPEED] = "window_speed",
};

const char *fe_reason_text(fe_reason_t r) { return (r >= 0 && r < FE_R_COUNT) ? REASON_TEXT[r] : "invalid"; }
const char *fe_reason_code(fe_reason_t r) { return (r >= 0 && r < FE_R_COUNT) ? REASON_CODE[r] : "invalid"; }

const char *fe_origin_name(fe_origin_t o) {
    switch (o) {
        case FE_ORIGIN_DRIVER: return "driver";
        case FE_ORIGIN_MESSAGE: return "message";
        default: return "none";
    }
}

size_t fe_gate_sizeof(void) { return sizeof(fe_gate_t); }

void fe_limits_default(fe_limits_t *lim) {
    lim->rate_capacity = 6;
    lim->rate_refill_ms = 1000;
    lim->turn_budget = 4;
    lim->turn_timeout_ms = 120000; /* the model can take a minute to answer under emulation */
    lim->state_max_age_ms = 500;
    lim->stationary_dkmh = 5;      /* 0.5 km/h */
    lim->window_fast_dkmh = 1000;  /* 100 km/h */
    lim->window_fast_max = 30;
    lim->hvac_min = 16;
    lim->hvac_max = 28;
    lim->rules = FE_RULES_ALL;
}

void fe_gate_init(fe_gate_t *g, const fe_limits_t *lim, uint64_t now_ms) {
    memset(g, 0, sizeof(*g));
    g->lim = *lim;
    g->tokens_milli = (uint64_t)lim->rate_capacity * 1000u;
    g->last_refill_ms = now_ms;
    g->next_turn_id = 1;
}

uint32_t fe_gate_open_turn(fe_gate_t *g, fe_origin_t origin, uint64_t now_ms) {
    uint32_t id = g->next_turn_id++;
    if (g->next_turn_id == 0) g->next_turn_id = 1;
    g->turn_id = id;
    g->turn_origin = origin;
    g->turn_used = 0;
    g->turn_opened_ms = now_ms;
    g->turn_open = true;
    return id;
}

void fe_gate_close_turn(fe_gate_t *g, uint32_t turn_id) {
    if (g->turn_open && g->turn_id == turn_id) g->turn_open = false;
}

void fe_gate_update_vehicle(fe_gate_t *g, const fe_vehicle_t *v, uint64_t now_ms) {
    g->vehicle = *v;
    g->vehicle_ms = now_ms;
    g->vehicle_known = true;
}

static bool take_token(fe_gate_t *g, uint64_t now_ms) {
    if (now_ms > g->last_refill_ms && g->lim.rate_refill_ms > 0) {
        uint64_t gained = (now_ms - g->last_refill_ms) * 1000u / g->lim.rate_refill_ms;
        uint64_t cap = (uint64_t)g->lim.rate_capacity * 1000u;
        g->tokens_milli = (g->tokens_milli + gained > cap) ? cap : g->tokens_milli + gained;
        g->last_refill_ms = now_ms;
    }
    if (g->tokens_milli < 1000u) return false;
    g->tokens_milli -= 1000u;
    return true;
}

static bool parked(const fe_gate_t *g, bool brake_counts) {
    const fe_vehicle_t *v = &g->vehicle;
    if (v->speed_dkmh > g->lim.stationary_dkmh) return false;
    return v->gear == FE_GEAR_PARK || (brake_counts && v->parking_brake);
}

static fe_verdict_t refuse(fe_reason_t r) {
    fe_verdict_t v = {false, r};
    return v;
}

fe_verdict_t fe_gate_decide(fe_gate_t *g, const fe_proposal_t *p, uint64_t now_ms) {
    const uint32_t rules = g->lim.rules;
    /* 1. Rate first: a flood is refused before anything else is looked at. */
    if ((rules & FE_RULE_RATE) && !take_token(g, now_ms)) return refuse(FE_R_RATE);
    /* 2. Allowlist and bounds. */
    const sig_row_t *row = row_of(p->signal);
    if (row == NULL) return refuse(FE_R_UNKNOWN_SIGNAL);
    if (!row->writable) return refuse(FE_R_NOT_ALLOWED);
    if ((rules & FE_RULE_BOUNDS) && (p->value < row->min || p->value > row->max)) return refuse(FE_R_OUT_OF_RANGE);
    /* 3. The request: open, current, from the driver alone, within its budget. */
    if (g->turn_open && now_ms - g->turn_opened_ms > g->lim.turn_timeout_ms) g->turn_open = false;
    if ((rules & FE_RULE_TURN) && (!g->turn_open || p->turn_id != g->turn_id)) return refuse(FE_R_NO_TURN);
    if ((rules & FE_RULE_ORIGIN) && g->turn_origin != FE_ORIGIN_DRIVER) return refuse(FE_R_FROM_MESSAGE);
    if ((rules & FE_RULE_TURN) && g->turn_used >= g->lim.turn_budget) return refuse(FE_R_TURN_BUDGET);
    if (!(rules & FE_RULE_STATE)) {
        g->turn_used++;
        fe_verdict_t ok = {true, FE_OK};
        return ok;
    }
    /* 4. The vehicle state, fresh from the bus. */
    if (!g->vehicle_known || now_ms - g->vehicle_ms > g->lim.state_max_age_ms) return refuse(FE_R_STATE_STALE);
    switch (p->signal) {
        case FE_SIG_LOCK_ROW1_DRIVER:
        case FE_SIG_LOCK_ROW1_PASSENGER:
        case FE_SIG_LOCK_ROW2_DRIVER:
        case FE_SIG_LOCK_ROW2_PASSENGER:
            if (p->value == 0 && !parked(g, true)) return refuse(FE_R_UNLOCK_MOVING);
            break;
        case FE_SIG_TRUNK_OPEN:
            if (p->value == 1 && !parked(g, false)) return refuse(FE_R_TRUNK_MOVING);
            break;
        case FE_SIG_WINDOW_ROW1_DRIVER:
        case FE_SIG_WINDOW_ROW1_PASSENGER:
        case FE_SIG_WINDOW_ROW2_DRIVER:
        case FE_SIG_WINDOW_ROW2_PASSENGER:
            if (g->vehicle.speed_dkmh > g->lim.window_fast_dkmh && p->value > g->lim.window_fast_max)
                return refuse(FE_R_WINDOW_SPEED);
            break;
        default:
            break;
    }
    g->turn_used++;
    fe_verdict_t ok = {true, FE_OK};
    return ok;
}

void fe_vehicle_apply(fe_vehicle_t *v, uint16_t signal, int32_t value) {
    switch (signal) {
        case FE_SIG_LOCK_ROW1_DRIVER:
        case FE_SIG_LOCK_ROW1_PASSENGER:
        case FE_SIG_LOCK_ROW2_DRIVER:
        case FE_SIG_LOCK_ROW2_PASSENGER: {
            uint8_t bit = (uint8_t)(1u << (signal - FE_SIG_LOCK_ROW1_DRIVER));
            v->locks = value ? (uint8_t)(v->locks | bit) : (uint8_t)(v->locks & ~bit);
            break;
        }
        case FE_SIG_WINDOW_ROW1_DRIVER:
        case FE_SIG_WINDOW_ROW1_PASSENGER:
        case FE_SIG_WINDOW_ROW2_DRIVER:
        case FE_SIG_WINDOW_ROW2_PASSENGER:
            if (value >= 0 && value <= 100) v->window[signal - FE_SIG_WINDOW_ROW1_DRIVER] = (uint8_t)value;
            break;
        case FE_SIG_TRUNK_OPEN:
            v->trunk_open = value != 0;
            break;
        case FE_SIG_HVAC_TEMP_DRIVER:
            if (value >= -128 && value <= 127) v->hvac_temp = (int8_t)value;
            break;
        default:
            break;
    }
}
