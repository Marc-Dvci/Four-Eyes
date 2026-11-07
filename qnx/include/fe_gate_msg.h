/*
 * Four Eyes: the gate's interface at /dev/fe/gate, as devctl() commands carried by QNX message passing.
 *
 * Who may send which command is not decided here: each command maps to a permission of the custom security-policy
 * class fe_gate, and gated asks the policy with secpol_check_permission() before acting. The policy gives the agent
 * only "propose" and "await"; the head unit (the bridge) gets "submit_turn" and "read"; the body controller "state".
 */
#ifndef FE_GATE_MSG_H
#define FE_GATE_MSG_H

#include <stdint.h>
#include <devctl.h>

#define FE_GATE_PATH "/dev/fe/gate"
#define FE_TEXT_MAX 1024
#define FE_REPLY_MAX 256

/* Head unit -> gate: a new request, from the driver or carrying outside text. */
typedef struct {
    uint32_t origin;              /* fe_origin_t, as the head unit labels it */
    uint32_t text_len;
    char text[FE_TEXT_MAX];       /* recorded, shown, passed to the agent; never read by the decision core */
} fe_msg_turn_t;

typedef struct {
    uint32_t turn_id;
    uint32_t invisible;           /* invisible code points the gate counted in the text */
} fe_msg_turn_reply_t;

/* Agent <- gate: the next request to answer. The devctl blocks until one is open. */
typedef struct {
    uint32_t turn_id;
    uint32_t origin;
    uint32_t text_len;
    char text[FE_TEXT_MAX];
} fe_msg_await_t;

/* Agent -> gate: one proposed action. */
typedef struct {
    uint32_t turn_id;
    uint16_t signal;
    uint16_t reserved;
    int32_t value;
    uint32_t parse_status;        /* fe_parse_status_t of the model's output, recorded */
} fe_msg_propose_t;

typedef struct {
    uint32_t allowed;
    uint32_t reason;              /* fe_reason_t */
    uint64_t record_seq;
} fe_msg_verdict_t;

/* FE_DCMD_PROPOSE sends the proposal and receives the verdict in the same buffer. */
typedef union {
    fe_msg_propose_t in;
    fe_msg_verdict_t out;
} fe_msg_propose_io_t;

/* Agent -> gate: the request is answered; the words for the driver and how the model's output parsed. */
typedef struct {
    uint32_t turn_id;
    uint32_t parse_status;
    uint32_t n_actions;
    uint32_t model_ms;
    char reply[FE_REPLY_MAX];
} fe_msg_done_t;

/* Head unit <- gate: state and the newest record lines, for the car page. */
typedef struct {
    uint64_t since_seq;           /* in: first record line wanted */
} fe_msg_read_req_t;

typedef struct {
    uint64_t next_seq;            /* out: pass back as since_seq */
    uint32_t n_bytes;             /* record lines in data, each ending in '\n' */
    char data[8192];
} fe_msg_read_t;

/* A kernel refusal the agent saw, reported for the record. The agent is untrusted, so the record marks it as
 * "reported by the agent"; the kernel's refusal itself does not depend on this report. */
typedef struct {
    int32_t err;                  /* errno the kernel returned */
    char path[96];
    char op[32];
} fe_msg_kernel_t;

/* Body controller -> gate: the vehicle state, every body cycle. This is the gate's only source of vehicle state.
 * The gate takes it only from a client whose security type holds fe_gate:state, which the policy gives to body_t
 * alone; the kernel, not the message, says who the client is. State frames on the CAN bus are not trusted. */
typedef struct {
    uint16_t speed_dkmh;          /* 0.1 km/h */
    int8_t gear;
    uint8_t parking_brake;
    uint8_t locks;                /* bit per door, 1 = locked */
    uint8_t trunk_open;
    uint8_t window[4];            /* % open */
    int8_t hvac_temp;
    uint8_t reserved[3];
    uint32_t seq;                 /* body cycle number */
} fe_msg_state_t;

#define FE_DCMD_SUBMIT_TURN __DIOTF(_DCMD_MISC, 0x4601, fe_msg_turn_t)
#define FE_DCMD_AWAIT __DIOF(_DCMD_MISC, 0x4602, fe_msg_await_t)
#define FE_DCMD_PROPOSE __DIOTF(_DCMD_MISC, 0x4603, fe_msg_propose_t)
#define FE_DCMD_DONE __DIOT(_DCMD_MISC, 0x4604, fe_msg_done_t)
#define FE_DCMD_READ __DIOTF(_DCMD_MISC, 0x4605, fe_msg_read_t)
#define FE_DCMD_KERNEL __DIOT(_DCMD_MISC, 0x4606, fe_msg_kernel_t)
#define FE_DCMD_STATE __DIOT(_DCMD_MISC, 0x4607, fe_msg_state_t)

#endif
