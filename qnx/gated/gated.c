/*
 * gated: the Four Eyes gate. The only process that may both hear the agent and write to the vehicle bus.
 *
 * It serves /dev/fe/gate as a resource manager. The head unit opens a request (a "turn") and labels where its text came
 * from; the agent waits for the request, asks its model, and proposes actions one by one; the gate judges each
 * proposal with the deterministic core in core/gate_core.c, against vehicle state it reads from /dev/vcan0 itself,
 * records the proposal and the verdict in a hash-chained record, and only then sends an allowed command on the bus.
 *
 * Which client may send which command is asked of the security policy (class fe_gate) with secpol_check_permission(),
 * so the agent cannot open a request of its own, however it words its messages.
 *
 *   gated [-p path] [-b bus] [-r record_file]
 */
struct client;
#define IOFUNC_OCB_T struct client
#define RESMGR_OCB_T struct client

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <devctl.h>
#include <sys/iofunc.h>
#include <sys/dispatch.h>
#include <sys/can_dcmd.h>
#include <sys/neutrino.h>
#include <secpol/secpol.h>

#include "fe_gate_msg.h"
#include "fe_signals.h"
#include "gate_core.h"
#include "proposal_parse.h"
#include "record.h"

typedef struct client {
    iofunc_ocb_t ocb;
    int awaiting_rcvid; /* agent blocked in FE_DCMD_AWAIT */
} client_t;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static fe_gate_t gate;
static fe_chain_t chain;
static FILE *record_fp;
static int bus_fd = -1;
static client_t *awaiting; /* the one agent waiting for a request */
static char turn_text[FE_TEXT_MAX];
static uint32_t turn_text_len;
static bool turn_pending; /* opened but not yet handed to the agent */
static uint64_t frames_ok, frames_bad;
static iofunc_attr_t attr;

static secpol_permission_t *perm_propose, *perm_await, *perm_submit, *perm_read;

/* The newest record lines, for FE_DCMD_READ. */
#define RING_LINES 512
#define REC_LINE_MAX 3072
static char ring[RING_LINES][REC_LINE_MAX];
static uint64_t ring_seq[RING_LINES];

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* Appends one line to the record, flushed to disk before the verdict is acted on. Caller holds mu. */
static uint64_t record(const char *members) {
    char line[REC_LINE_MAX];
    char full[REC_LINE_MAX];
    snprintf(full, sizeof full, "\"t_ms\":%llu,%s", (unsigned long long)now_ms(), members);
    uint64_t seq = chain.seq;
    int n = fe_chain_format(&chain, full, line, sizeof line);
    if (n < 0) {
        snprintf(full, sizeof full, "\"t_ms\":%llu,\"kind\":\"truncated\"", (unsigned long long)now_ms());
        seq = chain.seq;
        n = fe_chain_format(&chain, full, line, sizeof line);
    }
    if (record_fp) {
        fputs(line, record_fp);
        fflush(record_fp);
        fsync(fileno(record_fp));
    }
    memcpy(ring[seq % RING_LINES], line, (size_t)n + 1);
    ring_seq[seq % RING_LINES] = seq;
    fprintf(stderr, "FE %s", line);
    return seq;
}

/* Continues the chain from an existing record file, so a restarted gate extends it rather than forking it. */
static void resume_chain(const char *path) {
    fe_chain_init(&chain);
    FILE *f = fopen(path, "r");
    if (f == NULL) return;
    char line[REC_LINE_MAX];
    char last[REC_LINE_MAX] = "";
    while (fgets(line, sizeof line, f)) memcpy(last, line, sizeof line);
    fclose(f);
    const char *h = strstr(last, "\"hash\":\"");
    const char *s = strstr(last, "{\"seq\":");
    if (h && s && strlen(h + 8) >= 64) {
        memcpy(chain.prev, h + 8, 64);
        chain.prev[64] = '\0';
        chain.seq = strtoull(s + 7, NULL, 10) + 1;
    }
}

static void vehicle_members(char *out, size_t cap) {
    const fe_vehicle_t *v = &gate.vehicle;
    snprintf(out, cap, "\"speed_kmh\":%u.%u,\"gear\":%d,\"parking_brake\":%s,\"state_age_ms\":%llu",
             v->speed_dkmh / 10, v->speed_dkmh % 10, v->gear, v->parking_brake ? "true" : "false",
             gate.vehicle_known ? (unsigned long long)(now_ms() - gate.vehicle_ms) : 999999ull);
}

static void send_command(uint16_t signal, int32_t value, uint64_t seq) {
    CAN_MSG m;
    memset(&m, 0, sizeof m);
    m.mid = FE_CAN_ID_COMMAND;
    m.len = 8;
    m.dat[0] = (uint8_t)signal;
    m.dat[1] = (uint8_t)(value & 0xFF);
    m.dat[2] = (uint8_t)((value >> 8) & 0xFF);
    m.dat[3] = (uint8_t)(seq & 0xFF);
    m.dat[4] = (uint8_t)((seq >> 8) & 0xFF);
    m.dat[7] = fe_can_checksum(m.dat);
    if (devctl(bus_fd, CAN_DEVCTL_TX_FRAME_RAW, &m, sizeof m, NULL) != EOK)
        fprintf(stderr, "gated: bus write failed\n");
}

/* Reads vehicle state from the bus. The gate trusts only frames with the right identifier, length and checksum. */
static void *bus_reader(void *arg) {
    (void)arg;
    fe_vehicle_t v;
    memset(&v, 0, sizeof v);
    for (;;) {
        CAN_MSG m;
        int r = devctl(bus_fd, CAN_DEVCTL_RX_FRAME_RAW_BLOCK, &m, sizeof m, NULL);
        if (r != EOK) {
            if (r == EINTR) continue;
            fprintf(stderr, "gated: bus read: %s\n", strerror(r));
            sleep(1);
            continue;
        }
        pthread_mutex_lock(&mu);
        if (m.len != 8 || fe_can_checksum(m.dat) != m.dat[7]) {
            if (m.mid == FE_CAN_ID_STATE || m.mid == FE_CAN_ID_WINDOWS) frames_bad++;
        } else if (m.mid == FE_CAN_ID_STATE) {
            v = gate.vehicle;
            v.speed_dkmh = (uint16_t)(m.dat[0] | (m.dat[1] << 8));
            v.gear = (int8_t)m.dat[2];
            v.parking_brake = m.dat[3] != 0;
            v.locks = m.dat[4] & 0x0F;
            v.trunk_open = m.dat[5] != 0;
            fe_gate_update_vehicle(&gate, &v, now_ms());
            frames_ok++;
        } else if (m.mid == FE_CAN_ID_WINDOWS) {
            /* Windows and HVAC refresh the state but do not count as a fresh speed reading. */
            uint64_t t = gate.vehicle_ms;
            v = gate.vehicle;
            memcpy(v.window, m.dat, 4);
            v.hvac_temp = (int8_t)m.dat[4];
            bool known = gate.vehicle_known;
            fe_gate_update_vehicle(&gate, &v, t);
            gate.vehicle_known = known;
            frames_ok++;
        }
        pthread_mutex_unlock(&mu);
    }
    return NULL;
}

static bool permitted(resmgr_context_t *ctp, secpol_permission_t *perm) {
    return secpol_check_permission(&ctp->info, secpol_type_id(), perm) == 0;
}

static int refuse_by_policy(resmgr_context_t *ctp, const char *what) {
    char m[256];
    snprintf(m, sizeof m, "\"kind\":\"permission\",\"by\":\"policy\",\"command\":\"%s\",\"client_pid\":%d", what,
             ctp->info.pid);
    record(m);
    return EPERM;
}

/* Hands the open request to the waiting agent. Caller holds mu. */
static void hand_over(void) {
    if (!awaiting || !turn_pending) return;
    struct {
        struct _io_devctl_reply o;
        fe_msg_await_t a;
    } r;
    memset(&r, 0, sizeof r);
    r.o.nbytes = sizeof r.a;
    r.a.turn_id = gate.turn_id;
    r.a.origin = gate.turn_origin;
    r.a.text_len = turn_text_len;
    memcpy(r.a.text, turn_text, turn_text_len);
    MsgReply(awaiting->awaiting_rcvid, EOK, &r, sizeof r);
    awaiting->awaiting_rcvid = 0;
    awaiting = NULL;
    turn_pending = false;
}

static int reply_data(resmgr_context_t *ctp, io_devctl_t *msg, unsigned nbytes) {
    memset(&msg->o, 0, sizeof msg->o);
    msg->o.nbytes = nbytes;
    return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o) + nbytes);
}

static int do_submit(resmgr_context_t *ctp, io_devctl_t *msg, void *data, size_t have) {
    if (!permitted(ctp, perm_submit)) return refuse_by_policy(ctp, "submit_turn");
    if (have < offsetof(fe_msg_turn_t, text)) return EINVAL;
    fe_msg_turn_t *t = data;
    uint32_t len = t->text_len;
    if (len > FE_TEXT_MAX || offsetof(fe_msg_turn_t, text) + len > have) return EINVAL;
    fe_origin_t origin = t->origin == FE_ORIGIN_DRIVER ? FE_ORIGIN_DRIVER : FE_ORIGIN_MESSAGE;
    static char esc[2001];
    fe_json_escape(t->text, len, esc, sizeof esc);
    size_t invisible = fe_count_invisible(t->text, len);
    uint32_t id = fe_gate_open_turn(&gate, origin, now_ms());
    memcpy(turn_text, t->text, len);
    turn_text_len = len;
    turn_pending = true;
    char m[REC_LINE_MAX];
    snprintf(m, sizeof m, "\"kind\":\"turn\",\"turn\":%u,\"origin\":\"%s\",\"invisible\":%zu,\"text\":\"%s\"", id,
             fe_origin_name(origin), invisible, esc);
    record(m);
    fe_msg_turn_reply_t *out = data;
    out->turn_id = id;
    out->invisible = (uint32_t)invisible;
    hand_over();
    return reply_data(ctp, msg, sizeof *out);
}

static int do_propose(resmgr_context_t *ctp, io_devctl_t *msg, void *data, size_t have) {
    if (!permitted(ctp, perm_propose)) return refuse_by_policy(ctp, "propose");
    if (have < sizeof(fe_msg_propose_t)) return EINVAL;
    fe_msg_propose_io_t *io = data;
    fe_msg_propose_t p = io->in;
    fe_proposal_t prop = {p.turn_id, p.signal, p.value};
    fe_verdict_t v = fe_gate_decide(&gate, &prop, now_ms());
    char veh[160], m[REC_LINE_MAX];
    vehicle_members(veh, sizeof veh);
    snprintf(m, sizeof m,
             "\"kind\":\"proposal\",\"turn\":%u,\"signal\":\"%s\",\"signal_id\":%u,\"value\":%d,"
             "\"verdict\":\"%s\",\"by\":\"gate\",\"reason\":\"%s\",\"why\":\"%s\",%s",
             p.turn_id, fe_signal_path(p.signal), p.signal, p.value, v.allowed ? "allowed" : "refused",
             fe_reason_code(v.reason), fe_reason_text(v.reason), veh);
    uint64_t seq = record(m);
    if (v.allowed) send_command(p.signal, p.value, seq);
    fe_msg_verdict_t *out = &io->out;
    out->allowed = v.allowed;
    out->reason = v.reason;
    out->record_seq = seq;
    return reply_data(ctp, msg, sizeof *out);
}

static int do_done(resmgr_context_t *ctp, void *data, size_t have) {
    if (!permitted(ctp, perm_propose)) return refuse_by_policy(ctp, "done");
    if (have < sizeof(fe_msg_done_t)) return EINVAL;
    fe_msg_done_t *d = data;
    d->reply[FE_REPLY_MAX - 1] = '\0';
    char esc[1801], m[REC_LINE_MAX];
    fe_json_escape(d->reply, strlen(d->reply), esc, sizeof esc);
    snprintf(m, sizeof m,
             "\"kind\":\"done\",\"turn\":%u,\"parse\":\"%s\",\"actions\":%u,\"model_ms\":%u,\"reply\":\"%s\"",
             d->turn_id, fe_parse_status_name((fe_parse_status_t)d->parse_status), d->n_actions, d->model_ms, esc);
    record(m);
    fe_gate_close_turn(&gate, d->turn_id);
    return EOK;
}

static int do_kernel(resmgr_context_t *ctp, void *data, size_t have) {
    if (!permitted(ctp, perm_propose)) return refuse_by_policy(ctp, "kernel_report");
    if (have < sizeof(fe_msg_kernel_t)) return EINVAL;
    fe_msg_kernel_t *k = data;
    k->path[sizeof k->path - 1] = '\0';
    k->op[sizeof k->op - 1] = '\0';
    char p[200], o[100], m[600];
    fe_json_escape(k->path, strlen(k->path), p, sizeof p);
    fe_json_escape(k->op, strlen(k->op), o, sizeof o);
    snprintf(m, sizeof m,
             "\"kind\":\"kernel\",\"by\":\"kernel\",\"reported_by\":\"agent\",\"op\":\"%s\",\"path\":\"%s\","
             "\"errno\":%d,\"error\":\"%s\",\"verdict\":\"%s\"",
             o, p, k->err, k->err ? strerror(k->err) : "none", k->err ? "refused" : "NOT REFUSED");
    record(m);
    return EOK;
}

static int do_read(resmgr_context_t *ctp, io_devctl_t *msg, void *data, size_t have) {
    if (!permitted(ctp, perm_read)) return refuse_by_policy(ctp, "read");
    if (have < sizeof(fe_msg_read_req_t)) return EINVAL;
    uint64_t since = ((fe_msg_read_req_t *)data)->since_seq;
    fe_msg_read_t *out = data;
    uint64_t first = chain.seq > RING_LINES ? chain.seq - RING_LINES : 0;
    if (since < first) since = first;
    if (since > chain.seq) since = chain.seq;
    uint32_t used = 0;
    uint64_t s = since;
    for (; s < chain.seq; s++) {
        const char *line = ring[s % RING_LINES];
        size_t n = strlen(line);
        if (used + n > sizeof out->data) break;
        memcpy(out->data + used, line, n);
        used += (uint32_t)n;
    }
    out->next_seq = s;
    out->n_bytes = used;
    return reply_data(ctp, msg, offsetof(fe_msg_read_t, data) + used);
}

static int io_devctl(resmgr_context_t *ctp, io_devctl_t *msg, client_t *c) {
    int st = iofunc_devctl_default(ctp, msg, &c->ocb);
    if (st != _RESMGR_DEFAULT) return st;
    void *data = _DEVCTL_DATA(msg->i);
    size_t have = (size_t)ctp->info.msglen - sizeof(msg->i);
    if (have > msg->i.nbytes) have = msg->i.nbytes;
    int r;
    pthread_mutex_lock(&mu);
    switch (msg->i.dcmd) {
        case FE_DCMD_SUBMIT_TURN: r = do_submit(ctp, msg, data, have); break;
        case FE_DCMD_PROPOSE: r = do_propose(ctp, msg, data, have); break;
        case FE_DCMD_DONE: r = do_done(ctp, data, have); break;
        case FE_DCMD_KERNEL: r = do_kernel(ctp, data, have); break;
        case FE_DCMD_READ: r = do_read(ctp, msg, data, have); break;
        case FE_DCMD_AWAIT:
            if (!permitted(ctp, perm_await)) {
                r = refuse_by_policy(ctp, "await");
            } else if (awaiting && awaiting != c) {
                r = EBUSY;
            } else {
                c->awaiting_rcvid = ctp->rcvid;
                awaiting = c;
                hand_over();
                r = _RESMGR_NOREPLY;
            }
            break;
        default: r = ENOTTY; break;
    }
    pthread_mutex_unlock(&mu);
    return r;
}

static client_t *client_calloc(resmgr_context_t *ctp, iofunc_attr_t *a) {
    (void)ctp;
    (void)a;
    return calloc(1, sizeof(client_t));
}

static void client_free(client_t *c) { free(c); }

static int io_close_ocb(resmgr_context_t *ctp, void *reserved, client_t *c) {
    (void)reserved;
    pthread_mutex_lock(&mu);
    if (awaiting == c) awaiting = NULL;
    pthread_mutex_unlock(&mu);
    iofunc_ocb_detach(ctp, &c->ocb);
    free(c);
    return EOK;
}

static int io_unblock(resmgr_context_t *ctp, io_pulse_t *msg, client_t *c) {
    /* The unblock pulse carries the rcvid of the client that wants out (a signal, or its process being killed).
     * Without this reply a killed agent would stay REPLY-blocked on the gate and could not die. */
    int rcvid = msg->value.sival_int;
    (void)ctp;
    pthread_mutex_lock(&mu);
    if (awaiting == c && c->awaiting_rcvid == rcvid) {
        awaiting = NULL;
        c->awaiting_rcvid = 0;
        MsgError(rcvid, EINTR);
    }
    pthread_mutex_unlock(&mu);
    return _RESMGR_NOREPLY;
}

int main(int argc, char **argv) {
    const char *path = FE_GATE_PATH, *bus = "/dev/vcan0", *record_path = "/data/fe/record.jsonl";
    int opt;
    while ((opt = getopt(argc, argv, "p:b:r:")) != -1) {
        switch (opt) {
            case 'p': path = optarg; break;
            case 'b': bus = optarg; break;
            case 'r': record_path = optarg; break;
            default: fprintf(stderr, "usage: gated [-p path] [-b bus] [-r record]\n"); return 2;
        }
    }
    fe_limits_t lim;
    fe_limits_default(&lim);
    fe_gate_init(&gate, &lim, now_ms());

    perm_propose = secpol_get_permission(NULL, "fe_gate", "propose", 0);
    perm_await = secpol_get_permission(NULL, "fe_gate", "await", 0);
    perm_submit = secpol_get_permission(NULL, "fe_gate", "submit_turn", 0);
    perm_read = secpol_get_permission(NULL, "fe_gate", "read", 0);
    if (!perm_propose || !perm_await || !perm_submit || !perm_read) {
        fprintf(stderr, "gated: security policy class fe_gate not available: %s\n", strerror(errno));
        return 1;
    }

    resume_chain(record_path);
    record_fp = fopen(record_path, "a");
    if (record_fp == NULL) fprintf(stderr, "gated: record %s: %s (in memory only)\n", record_path, strerror(errno));

    bus_fd = open(bus, O_RDWR);
    int bus_err = bus_fd < 0 ? errno : 0;
    {
        char m[400];
        snprintf(m, sizeof m,
                 "\"kind\":\"boot\",\"process\":\"gated\",\"bus\":\"%s\",\"bus_open\":\"%s\",\"pid\":%d,"
                 "\"secpol_type\":%u",
                 bus, bus_err ? strerror(bus_err) : "ok", getpid(), secpol_type_id());
        pthread_mutex_lock(&mu);
        record(m);
        pthread_mutex_unlock(&mu);
    }
    if (bus_fd < 0) {
        fprintf(stderr, "gated: open %s: %s\n", bus, strerror(bus_err));
        return 1;
    }
    pthread_t th;
    pthread_create(&th, NULL, bus_reader, NULL);

    dispatch_t *dpp = dispatch_create_channel(-1, DISPATCH_FLAG_NOLOCK);
    static resmgr_connect_funcs_t cfuncs;
    static resmgr_io_funcs_t ifuncs;
    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &cfuncs, _RESMGR_IO_NFUNCS, &ifuncs);
    ifuncs.devctl = io_devctl;
    ifuncs.close_ocb = io_close_ocb;
    ifuncs.unblock = io_unblock;
    static iofunc_funcs_t ocb_funcs = {.nfuncs = _IOFUNC_NFUNCS, .ocb_calloc = client_calloc, .ocb_free = client_free};
    static iofunc_mount_t mount = {.funcs = &ocb_funcs, .conf = IOFUNC_PC_ACL};
    iofunc_attr_init(&attr, S_IFCHR | 0666, NULL, NULL);
    attr.mount = &mount;
    resmgr_attr_t rattr;
    memset(&rattr, 0, sizeof rattr);
    rattr.nparts_max = 2;
    rattr.msg_max_size = sizeof(io_devctl_t) + sizeof(fe_msg_read_t) + 64;
    if (secpol_resmgr_attach(NULL, dpp, &rattr, path, _FTYPE_ANY, 0, &cfuncs, &ifuncs, &attr, NULL) == -1) {
        fprintf(stderr, "gated: attach %s: %s\n", path, strerror(errno));
        return 1;
    }
    fprintf(stderr, "gated: serving %s, bus %s\n", path, bus);
    dispatch_context_t *ctp = dispatch_context_alloc(dpp);
    for (;;) {
        if (dispatch_block(ctp) == NULL) {
            if (errno == EINTR) continue;
            perror("gated: dispatch_block");
            return 1;
        }
        dispatch_handler(ctp);
    }
}
