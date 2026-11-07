/*
 * bodyd: the simulated body controller. It owns the doors, windows, trunk and climate set point, and the simulated
 * driving state (speed, gear, parking brake), and puts both on the virtual CAN bus every 20 ms. It also sends the same
 * state straight to the gate (FE_DCMD_STATE on /dev/fe/gate), which is the only state the gate acts on.
 *
 * It obeys COMMAND frames from the bus after checking their length, checksum, signal and range. It does not repeat the
 * gate's state rules: like many real body modules it trusts the bus, which is why who may write the bus matters.
 * The driver's own controls (speed, gear, parking brake) come from the head unit through /dev/fe/drive.
 * The 20 ms cycle runs at a priority the agent cannot reach; every late cycle is counted.
 *
 *   bodyd [-b bus] [-p drive_path] [-P priority] [-g gate_path] [-T]
 *   -T  test hook, off by default: accepts "quiet 1|0" on the drive path, which stops (or resumes) the state messages to
 *       the gate while the bus broadcast and the 20 ms cycle go on. Used by guest_bench.py state to test the gate's
 *       500 ms staleness rule without signalling the process.
 *   write to /dev/fe/drive: "speed <km/h>", "gear P|R|N|D", "brake 0|1", "reset"
 *   read from /dev/fe/drive: one JSON line with the vehicle state and the cycle counters
 */
struct _iofunc_ocb;
#define RESMGR_OCB_T struct _iofunc_ocb

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <devctl.h>
#include <sys/can_dcmd.h>
#include <sys/dispatch.h>
#include <sys/iofunc.h>
#include <sys/neutrino.h>
#include <secpol/secpol.h>

#include "fe_gate_msg.h"
#include "fe_signals.h"
#include "gate_core.h"

#define PERIOD_NS 20000000L
#define DEADLINE_US 5000 /* a frame later than 5 ms after its slot is a missed deadline */

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static fe_vehicle_t veh;          /* what the bus reports */
static uint8_t window_target[4];  /* windows move toward these */
static uint16_t speed_target_dkmh;
static int bus_fd = -1;
static uint64_t cycles, misses, max_late_us, cmd_ok, cmd_rejected, last_cmd_seq;
static iofunc_attr_t attr;

static void reset_vehicle(void) {
    memset(&veh, 0, sizeof veh);
    veh.gear = FE_GEAR_PARK;
    veh.parking_brake = true;
    veh.locks = 0x0F;
    veh.hvac_temp = 21;
    memset(window_target, 0, sizeof window_target);
    speed_target_dkmh = 0;
}

static void tx(uint32_t mid, uint8_t d[8]) {
    CAN_MSG m;
    memset(&m, 0, sizeof m);
    m.mid = mid;
    m.len = 8;
    d[7] = fe_can_checksum(d);
    memcpy(m.dat, d, 8);
    devctl(bus_fd, CAN_DEVCTL_TX_FRAME_RAW, &m, sizeof m, NULL);
}

static uint64_t ts_us(const struct timespec *t) { return (uint64_t)t->tv_sec * 1000000u + (uint64_t)t->tv_nsec / 1000u; }

/* The 20 ms cycle: move the simulated plant, then broadcast it. */
static void *cycle(void *arg) {
    (void)arg;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    uint8_t alive = 0;
    for (;;) {
        next.tv_nsec += PERIOD_NS;
        if (next.tv_nsec >= 1000000000L) {
            next.tv_nsec -= 1000000000L;
            next.tv_sec++;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        pthread_mutex_lock(&mu);
        /* speed ramps at 10 km/h per second (2 units of 0.1 km/h per cycle); no motion in Park or N with the brake */
        uint16_t target = (veh.gear == FE_GEAR_PARK || veh.parking_brake) ? 0 : speed_target_dkmh;
        if (veh.speed_dkmh < target) veh.speed_dkmh = (uint16_t)(veh.speed_dkmh + 2 > target ? target : veh.speed_dkmh + 2);
        else if (veh.speed_dkmh > target) veh.speed_dkmh = (uint16_t)(veh.speed_dkmh < target + 2 ? target : veh.speed_dkmh - 2);
        /* windows travel at 50 % per second: 1 % per cycle */
        for (int i = 0; i < 4; i++) {
            if (veh.window[i] < window_target[i]) veh.window[i]++;
            else if (veh.window[i] > window_target[i]) veh.window[i]--;
        }
        uint8_t s[8] = {(uint8_t)(veh.speed_dkmh & 0xFF), (uint8_t)(veh.speed_dkmh >> 8), (uint8_t)veh.gear,
                        veh.parking_brake, veh.locks, veh.trunk_open, alive, 0};
        uint8_t w[8] = {veh.window[0], veh.window[1], veh.window[2], veh.window[3], (uint8_t)veh.hvac_temp, 0, alive, 0};
        pthread_mutex_unlock(&mu);
        tx(FE_CAN_ID_STATE, s);
        tx(FE_CAN_ID_WINDOWS, w);
        alive++;
        struct timespec done;
        clock_gettime(CLOCK_MONOTONIC, &done);
        uint64_t late = ts_us(&done) > ts_us(&next) ? ts_us(&done) - ts_us(&next) : 0;
        pthread_mutex_lock(&mu);
        cycles++;
        if (late > DEADLINE_US) misses++;
        if (late > max_late_us) max_late_us = late;
        pthread_mutex_unlock(&mu);
    }
    return NULL;
}

/* Commands from the bus. Anything malformed is dropped and counted. */
static void *commands(void *arg) {
    (void)arg;
    for (;;) {
        CAN_MSG m;
        int r = devctl(bus_fd, CAN_DEVCTL_RX_FRAME_RAW_BLOCK, &m, sizeof m, NULL);
        if (r != EOK) {
            if (r != EINTR) sleep(1);
            continue;
        }
        if (m.mid != FE_CAN_ID_COMMAND) continue;
        uint16_t sig = m.dat[0];
        int32_t value = (int16_t)(m.dat[1] | (m.dat[2] << 8));
        bool ok = m.len == 8 && fe_can_checksum(m.dat) == m.dat[7] && fe_signal_writable(sig);
        if (ok) {
            /* bounds as the signal table gives them: reuse the core's apply, which ignores out-of-range values */
            if ((sig >= FE_SIG_WINDOW_ROW1_DRIVER && sig <= FE_SIG_WINDOW_ROW2_PASSENGER && (value < 0 || value > 100)) ||
                (sig == FE_SIG_HVAC_TEMP_DRIVER && (value < 16 || value > 28)) ||
                ((sig < FE_SIG_WINDOW_ROW1_DRIVER || sig == FE_SIG_TRUNK_OPEN) && value != 0 && value != 1))
                ok = false;
        }
        pthread_mutex_lock(&mu);
        if (!ok) {
            cmd_rejected++;
            pthread_mutex_unlock(&mu);
            fprintf(stderr, "FE BODY rejected frame mid=0x%x len=%u sig=%u\n", m.mid, m.len, sig);
            continue;
        }
        cmd_ok++;
        last_cmd_seq = (uint64_t)(m.dat[3] | (m.dat[4] << 8));
        if (sig >= FE_SIG_WINDOW_ROW1_DRIVER && sig <= FE_SIG_WINDOW_ROW2_PASSENGER)
            window_target[sig - FE_SIG_WINDOW_ROW1_DRIVER] = (uint8_t)value;
        else
            fe_vehicle_apply(&veh, sig, value);
        pthread_mutex_unlock(&mu);
        fprintf(stderr, "FE BODY applied %s = %d\n", fe_signal_path(sig), value);
    }
    return NULL;
}

/* The gate's vehicle state, sent straight to the gate by message passing every cycle. Not on the bus: any node on the
 * bus could send a state frame, but only this process's type may send FE_DCMD_STATE, and the kernel tells the gate
 * which type sent each message. Runs at the default priority, apart from the 20 ms cycle, so a busy gate cannot make
 * the cycle late; if the gate is not there yet, or restarts, it reconnects. */
static const char *gate_path = FE_GATE_PATH;
static bool test_hooks, quiet;
static uint64_t state_sent, state_failed;

static void *report_state(void *arg) {
    (void)arg;
    int fd = -1;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    for (;;) {
        next.tv_nsec += PERIOD_NS;
        if (next.tv_nsec >= 1000000000L) {
            next.tv_nsec -= 1000000000L;
            next.tv_sec++;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        pthread_mutex_lock(&mu);
        bool skip = quiet;
        pthread_mutex_unlock(&mu);
        if (skip) continue;
        if (fd < 0) {
            fd = open(gate_path, O_RDWR);
            if (fd < 0) continue;
        }
        fe_msg_state_t s;
        memset(&s, 0, sizeof s);
        pthread_mutex_lock(&mu);
        s.speed_dkmh = veh.speed_dkmh;
        s.gear = veh.gear;
        s.parking_brake = veh.parking_brake;
        s.locks = veh.locks;
        s.trunk_open = veh.trunk_open;
        memcpy(s.window, veh.window, sizeof s.window);
        s.hvac_temp = veh.hvac_temp;
        s.seq = (uint32_t)cycles;
        pthread_mutex_unlock(&mu);
        int r = devctl(fd, FE_DCMD_STATE, &s, sizeof s, NULL);
        pthread_mutex_lock(&mu);
        if (r == EOK) state_sent++;
        else state_failed++;
        pthread_mutex_unlock(&mu);
        if (r != EOK && r != EINTR) {
            close(fd);
            fd = -1;
        }
    }
    return NULL;
}

static int io_write(resmgr_context_t *ctp, io_write_t *msg, iofunc_ocb_t *ocb) {
    int st = iofunc_write_verify(ctp, msg, ocb, NULL);
    if (st != EOK) return st;
    size_t len = _IO_WRITE_GET_NBYTES(msg);
    char buf[128];
    if (len >= sizeof buf) return E2BIG;
    if (resmgr_msgget(ctp, buf, len, sizeof(msg->i)) != (ssize_t)len) return EFAULT;
    buf[len] = '\0';
    char word[16] = "", arg[16] = "", arg2[16] = "";
    sscanf(buf, "%15s %15s %15s", word, arg, arg2);
    pthread_mutex_lock(&mu);
    if (strcmp(word, "speed") == 0) {
        int kmh = atoi(arg);
        speed_target_dkmh = (uint16_t)(kmh < 0 ? 0 : kmh > 250 ? 2500 : kmh * 10);
        if (strcmp(arg2, "now") == 0) veh.speed_dkmh = speed_target_dkmh; /* test runs: no ramp */
    } else if (strcmp(word, "gear") == 0) {
        veh.gear = arg[0] == 'P' ? FE_GEAR_PARK : arg[0] == 'R' ? FE_GEAR_REVERSE : arg[0] == 'N' ? FE_GEAR_NEUTRAL : FE_GEAR_DRIVE;
    } else if (strcmp(word, "brake") == 0) {
        veh.parking_brake = atoi(arg) != 0;
    } else if (test_hooks && strcmp(word, "quiet") == 0) {
        quiet = atoi(arg) != 0;
    } else if (strcmp(word, "reset") == 0) {
        reset_vehicle();
    } else {
        pthread_mutex_unlock(&mu);
        return EINVAL;
    }
    pthread_mutex_unlock(&mu);
    _IO_SET_WRITE_NBYTES(ctp, len);
    return EOK;
}

static int io_read(resmgr_context_t *ctp, io_read_t *msg, iofunc_ocb_t *ocb) {
    int st = iofunc_read_verify(ctp, msg, ocb, NULL);
    if (st != EOK) return st;
    char line[512];
    pthread_mutex_lock(&mu);
    int n = snprintf(line, sizeof line,
                     "{\"speed_kmh\":%u.%u,\"gear\":%d,\"parking_brake\":%s,\"locks\":%u,\"trunk_open\":%s,"
                     "\"windows\":[%u,%u,%u,%u],\"hvac\":%d,\"cycles\":%llu,\"misses\":%llu,\"max_late_us\":%llu,"
                     "\"cmd_ok\":%llu,\"cmd_rejected\":%llu,\"last_cmd_seq\":%llu,\"state_sent\":%llu,"
                     "\"state_failed\":%llu}\n",
                     veh.speed_dkmh / 10, veh.speed_dkmh % 10, veh.gear, veh.parking_brake ? "true" : "false",
                     veh.locks, veh.trunk_open ? "true" : "false", veh.window[0], veh.window[1], veh.window[2],
                     veh.window[3], veh.hvac_temp, (unsigned long long)cycles, (unsigned long long)misses,
                     (unsigned long long)max_late_us, (unsigned long long)cmd_ok, (unsigned long long)cmd_rejected,
                     (unsigned long long)last_cmd_seq, (unsigned long long)state_sent,
                     (unsigned long long)state_failed);
    pthread_mutex_unlock(&mu);
    /* each read from offset 0 returns the whole current line; a read at a later offset is end of file, so both
     * cat and a reader polling with pread(.., 0) work */
    off_t off = ocb->offset;
    bool explicit_offset = (msg->i.xtype & _IO_XTYPE_MASK) == _IO_XTYPE_OFFSET;
    if (explicit_offset) off = ((struct _xtype_offset *)(&msg->i + 1))->offset;
    if (off > 0) {
        _IO_SET_READ_NBYTES(ctp, 0);
        return EOK;
    }
    size_t want = _IO_READ_GET_NBYTES(msg);
    if ((size_t)n > want) n = (int)want;
    if (!explicit_offset) ocb->offset += n;
    _IO_SET_READ_NBYTES(ctp, n);
    return _RESMGR_PTR(ctp, line, (size_t)n);
}

int main(int argc, char **argv) {
    const char *bus = "/dev/vcan0", *path = "/dev/fe/drive";
    int prio = 30, opt;
    while ((opt = getopt(argc, argv, "b:p:P:g:T")) != -1) {
        switch (opt) {
            case 'b': bus = optarg; break;
            case 'p': path = optarg; break;
            case 'P': prio = atoi(optarg); break;
            case 'g': gate_path = optarg; break;
            case 'T': test_hooks = true; break;
            default: fprintf(stderr, "usage: bodyd [-b bus] [-p path] [-P priority] [-g gate]\n"); return 2;
        }
    }
    reset_vehicle();
    bus_fd = open(bus, O_RDWR);
    if (bus_fd < 0) {
        fprintf(stderr, "bodyd: open %s: %s\n", bus, strerror(errno));
        return 1;
    }
    pthread_attr_t pa;
    pthread_attr_init(&pa);
    pthread_attr_setinheritsched(&pa, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&pa, SCHED_FIFO);
    struct sched_param sp = {.sched_priority = prio};
    pthread_attr_setschedparam(&pa, &sp);
    pthread_t t1, t2;
    if (pthread_create(&t1, &pa, cycle, NULL) != 0) {
        fprintf(stderr, "bodyd: cannot run the cycle at priority %d, using the default\n", prio);
        pthread_create(&t1, NULL, cycle, NULL);
    }
    pthread_create(&t2, &pa, commands, NULL);
    pthread_t t3;
    pthread_create(&t3, NULL, report_state, NULL);

    dispatch_t *dpp = dispatch_create_channel(-1, DISPATCH_FLAG_NOLOCK);
    static resmgr_connect_funcs_t cfuncs;
    static resmgr_io_funcs_t ifuncs;
    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &cfuncs, _RESMGR_IO_NFUNCS, &ifuncs);
    ifuncs.write = io_write;
    ifuncs.read = io_read;
    static iofunc_mount_t mount = {.conf = IOFUNC_PC_ACL};
    iofunc_attr_init(&attr, S_IFCHR | 0666, NULL, NULL);
    attr.mount = &mount;
    resmgr_attr_t rattr;
    memset(&rattr, 0, sizeof rattr);
    rattr.nparts_max = 1;
    rattr.msg_max_size = 2048;
    if (secpol_resmgr_attach(NULL, dpp, &rattr, path, _FTYPE_ANY, 0, &cfuncs, &ifuncs, &attr, NULL) == -1) {
        fprintf(stderr, "bodyd: attach %s: %s\n", path, strerror(errno));
        return 1;
    }
    fprintf(stderr, "bodyd: body controller on %s, driver controls at %s, cycle 20 ms at priority %d\n", bus, path, prio);
    dispatch_context_t *ctp = dispatch_context_alloc(dpp);
    for (;;) {
        if (dispatch_block(ctp) == NULL) {
            if (errno == EINTR) continue;
            return 1;
        }
        dispatch_handler(ctp);
    }
}
