/*
 * vcand: a virtual CAN bus for QNX 8.0, as a resource manager at /dev/vcan0.
 *
 * QNX SDP 8.0 ships the CAN devctl interface (<sys/can_dcmd.h>) but, in this installation, no virtual CAN device, so
 * this resource manager provides one with the same commands a dev-can-* driver in raw-frame mode answers:
 * CAN_DEVCTL_TX_FRAME_RAW, CAN_DEVCTL_RX_FRAME_RAW_BLOCK, CAN_DEVCTL_RX_FRAME_RAW_NOBLOCK, CAN_DEVCTL_GET_STATS and
 * CAN_DEVCTL_GET_INFO. Every open is one node on the bus: a frame one node transmits is queued for every other node.
 * Frames that a classic CAN controller could not put on the wire (more than 8 data bytes, an identifier beyond 11 or
 * 29 bits) are refused with EINVAL and counted as form errors.
 *
 * Who may open the bus is not decided here: the security policy lets only the gate's and the body controller's types
 * connect to this process's channel, so the kernel refuses everyone else before a message arrives.
 *
 *   vcand [-p path] [-q queue_depth] [-v]
 */
struct node;
#define IOFUNC_OCB_T struct node
#define RESMGR_OCB_T struct node

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/iofunc.h>
#include <sys/dispatch.h>
#include <sys/can_dcmd.h>
#include <sys/neutrino.h>
#include <secpol/secpol.h>

#define MAX_DEPTH 512

typedef struct node node_t;

struct node {
    iofunc_ocb_t ocb; /* must come first */
    node_t *next;
    CAN_MSG *ring;
    unsigned head, count;
    int waiting_rcvid; /* a client blocked in RX_FRAME_RAW_BLOCK, or 0 */
    uint32_t dropped;
};

static node_t *nodes;
static unsigned depth = 128;
static int verbose;
static CAN_DEVCTL_STATS stats;
static iofunc_attr_t attr;

static uint32_t now_ms32(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

static node_t *node_calloc(resmgr_context_t *ctp, iofunc_attr_t *a) {
    (void)ctp;
    (void)a;
    node_t *n = calloc(1, sizeof(*n));
    if (n == NULL) return NULL;
    n->ring = calloc(depth, sizeof(CAN_MSG));
    if (n->ring == NULL) {
        free(n);
        return NULL;
    }
    n->next = nodes;
    nodes = n;
    return n;
}

static void node_free(node_t *n) {
    for (node_t **pp = &nodes; *pp; pp = &(*pp)->next) {
        if (*pp == n) {
            *pp = n->next;
            break;
        }
    }
    if (n->waiting_rcvid) MsgError(n->waiting_rcvid, EBADF);
    free(n->ring);
    free(n);
}

typedef struct {
    struct _io_devctl_reply o;
    CAN_MSG m;
} rx_reply_t;

static void reply_frame(int rcvid, const CAN_MSG *m) {
    rx_reply_t r;
    memset(&r, 0, sizeof r);
    r.o.ret_val = EOK;
    r.o.nbytes = sizeof(CAN_MSG);
    r.m = *m;
    MsgReply(rcvid, EOK, &r, sizeof r);
}

/* Puts a frame on the bus: every node but the sender gets it. */
static void broadcast(node_t *from, const CAN_MSG *m) {
    stats.transmitted_frames++;
    for (node_t *n = nodes; n; n = n->next) {
        if (n == from) continue;
        if (n->waiting_rcvid) {
            reply_frame(n->waiting_rcvid, m);
            n->waiting_rcvid = 0;
            stats.received_frames++;
            continue;
        }
        if (n->count == depth) { /* full: the oldest frame is lost, as on a controller's overrun */
            n->head = (n->head + 1) % depth;
            n->count--;
            n->dropped++;
            stats.sw_receive_q_full++;
        }
        n->ring[(n->head + n->count) % depth] = *m;
        n->count++;
        stats.received_frames++;
    }
}

static bool frame_valid(const CAN_MSG *m) {
    if (m->len > CAN_MSG_DATA_MAX_CAN) return false;
    if (m->ext.is_extended_mid ? m->mid > 0x1FFFFFFFu : m->mid > 0x7FFu) return false;
    return true;
}

static int transmit(node_t *from, const CAN_MSG *in) {
    if (!frame_valid(in)) {
        stats.form_errors++;
        stats.total_frame_errors++;
        if (verbose) fprintf(stderr, "vcand: refused malformed frame mid=0x%x len=%u\n", in->mid, in->len);
        return EINVAL;
    }
    CAN_MSG m = *in;
    m.ext.timestamp = now_ms32();
    memset(m.dat + m.len, 0, CAN_MSG_DATA_MAX - m.len);
    broadcast(from, &m);
    return EOK;
}

static int io_devctl(resmgr_context_t *ctp, io_devctl_t *msg, node_t *n) {
    int st = iofunc_devctl_default(ctp, msg, &n->ocb);
    if (st != _RESMGR_DEFAULT) return st;
    void *data = _DEVCTL_DATA(msg->i);
    size_t have = (size_t)ctp->info.msglen - sizeof(msg->i);
    unsigned nbytes = 0;
    switch (msg->i.dcmd) {
        case CAN_DEVCTL_TX_FRAME_RAW: {
            if (have < sizeof(CAN_MSG)) return EINVAL;
            CAN_MSG m;
            memcpy(&m, data, sizeof m);
            int r = transmit(n, &m);
            if (r != EOK) return r;
            break;
        }
        case CAN_DEVCTL_RX_FRAME_RAW_NOBLOCK:
        case CAN_DEVCTL_RX_FRAME_RAW_BLOCK:
            if (n->count > 0) {
                memcpy(data, &n->ring[n->head], sizeof(CAN_MSG));
                n->head = (n->head + 1) % depth;
                n->count--;
                nbytes = sizeof(CAN_MSG);
                break;
            }
            if (msg->i.dcmd == CAN_DEVCTL_RX_FRAME_RAW_NOBLOCK) return EAGAIN;
            if (n->waiting_rcvid) return EBUSY;
            n->waiting_rcvid = ctp->rcvid;
            return _RESMGR_NOREPLY;
        case CAN_DEVCTL_GET_STATS:
            memcpy(data, &stats, sizeof stats);
            nbytes = sizeof stats;
            break;
        case CAN_DEVCTL_GET_INFO: {
            CAN_DEVCTL_INFO info;
            memset(&info, 0, sizeof info);
            snprintf(info.description, sizeof info.description, "Four Eyes virtual CAN bus");
            info.msgq_size = depth;
            info.mode = CANDEV_MODE_RAW_FRAME;
            info.bit_rate = 500000;
            info.num_tx_mboxes = 1;
            info.num_rx_mboxes = 1;
            memcpy(data, &info, sizeof info);
            nbytes = sizeof info;
            break;
        }
        default:
            return ENOTTY;
    }
    memset(&msg->o, 0, sizeof(msg->o));
    msg->o.ret_val = EOK;
    msg->o.nbytes = nbytes;
    return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o) + nbytes);
}

/* write() takes whole CAN_MSG structures, as libcan's raw mode does. */
static int io_write(resmgr_context_t *ctp, io_write_t *msg, node_t *n) {
    int st = iofunc_write_verify(ctp, msg, &n->ocb, NULL);
    if (st != EOK) return st;
    size_t len = _IO_WRITE_GET_NBYTES(msg);
    if (len == 0 || len % sizeof(CAN_MSG) != 0 || len > 16 * sizeof(CAN_MSG)) {
        stats.form_errors++;
        stats.total_frame_errors++;
        return EINVAL;
    }
    CAN_MSG buf[16];
    if (resmgr_msgget(ctp, buf, len, sizeof(msg->i)) != (ssize_t)len) return EFAULT;
    for (size_t i = 0; i < len / sizeof(CAN_MSG); i++) {
        int r = transmit(n, &buf[i]);
        if (r != EOK) return r;
    }
    _IO_SET_WRITE_NBYTES(ctp, len);
    return EOK;
}

static int io_close_ocb(resmgr_context_t *ctp, void *reserved, node_t *n) {
    (void)reserved;
    iofunc_ocb_detach(ctp, &n->ocb);
    node_free(n);
    return EOK;
}

static int io_unblock(resmgr_context_t *ctp, io_pulse_t *msg, node_t *n) {
    int rcvid = msg->value.sival_int; /* the client that wants out */
    (void)ctp;
    if (n->waiting_rcvid == rcvid) {
        n->waiting_rcvid = 0;
        MsgError(rcvid, EINTR);
    }
    return _RESMGR_NOREPLY;
}

int main(int argc, char **argv) {
    const char *path = "/dev/vcan0";
    int opt;
    while ((opt = getopt(argc, argv, "p:q:v")) != -1) {
        switch (opt) {
            case 'p': path = optarg; break;
            case 'q': depth = (unsigned)atoi(optarg); break;
            case 'v': verbose = 1; break;
            default: fprintf(stderr, "usage: vcand [-p path] [-q depth] [-v]\n"); return 2;
        }
    }
    if (depth < 4 || depth > MAX_DEPTH) depth = 128;

    dispatch_t *dpp = dispatch_create_channel(-1, DISPATCH_FLAG_NOLOCK);
    if (dpp == NULL) {
        perror("vcand: dispatch_create_channel");
        return 1;
    }
    static resmgr_connect_funcs_t cfuncs;
    static resmgr_io_funcs_t ifuncs;
    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &cfuncs, _RESMGR_IO_NFUNCS, &ifuncs);
    ifuncs.devctl = io_devctl;
    ifuncs.write = io_write;
    ifuncs.close_ocb = io_close_ocb;
    ifuncs.unblock = io_unblock;

    static iofunc_funcs_t ocb_funcs = {.nfuncs = _IOFUNC_NFUNCS, .ocb_calloc = node_calloc, .ocb_free = node_free};
    static iofunc_mount_t mount = {.funcs = &ocb_funcs, .conf = IOFUNC_PC_ACL};
    iofunc_attr_init(&attr, S_IFCHR | 0666, NULL, NULL);
    attr.mount = &mount;

    resmgr_attr_t rattr;
    memset(&rattr, 0, sizeof rattr);
    rattr.nparts_max = 2;
    rattr.msg_max_size = 4096;
    if (secpol_resmgr_attach(NULL, dpp, &rattr, path, _FTYPE_ANY, 0, &cfuncs, &ifuncs, &attr, NULL) == -1) {
        fprintf(stderr, "vcand: attach %s: %s\n", path, strerror(errno));
        return 1;
    }
    fprintf(stderr, "vcand: virtual CAN bus at %s, %u frames per node\n", path, depth);

    dispatch_context_t *ctp = dispatch_context_alloc(dpp);
    for (;;) {
        if (dispatch_block(ctp) == NULL) {
            if (errno == EINTR) continue;
            perror("vcand: dispatch_block");
            return 1;
        }
        dispatch_handler(ctp);
    }
}
