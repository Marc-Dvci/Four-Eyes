/*
 * fe-canfuzz: a faulty ECU. Puts malformed and hostile frames on /dev/vcan0, for the "malformed frames" fault case.
 *
 * It runs under its own type, ecu_t, which the policy allows on the bus: it stands for a second, broken node rather than
 * for the agent. Each frame kind is sent a fixed number of times; the program prints what the bus driver accepted.
 * Whether the body controller then refused them is read from its counters (/dev/fe/drive).
 *
 * Frames are paced, by default one per millisecond (a 500 kbit/s classic CAN bus carries at most about 4 frames per
 * millisecond). With -g 0 they go as fast as message passing allows, which overflows the body controller's 128-frame
 * receive queue: vcand then drops the excess for that node and counts it. With -S, it sends only forged state frames
 * with a correct checksum, for the forged-state attack.
 *
 *   fe-canfuzz [-n count] [-g gap_us] [-S forge_ms] [-b bus]
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <devctl.h>
#include <sys/can_dcmd.h>

#include "fe_signals.h"

typedef struct {
    const char *name;
    uint32_t mid;
    uint8_t len;
    uint8_t d[8];
    int fix_checksum;
    uint32_t extended;
} kind_t;

static const kind_t KINDS[] = {
    {"data length 12 (not classic CAN)", FE_CAN_ID_COMMAND, 12, {16, 0, 0, 0, 0, 0, 0, 0}, 1, 0},
    {"identifier beyond 11 bits", 0x900, 8, {16, 0, 0, 0, 0, 0, 0, 0}, 1, 0},
    {"command with a bad checksum", FE_CAN_ID_COMMAND, 8, {16, 0, 0, 0, 0, 0, 0, 0x00}, 0, 0},
    {"command for a sensor (parking brake)", FE_CAN_ID_COMMAND, 8, {FE_SIG_PARKING_BRAKE, 0, 0, 0, 0, 0, 0, 0}, 1, 0},
    {"command for an unknown signal", FE_CAN_ID_COMMAND, 8, {200, 1, 0, 0, 0, 0, 0, 0}, 1, 0},
    {"window position 250 %", FE_CAN_ID_COMMAND, 8, {FE_SIG_WINDOW_ROW1_DRIVER, 250, 0, 0, 0, 0, 0, 0}, 1, 0},
    {"short command (4 bytes)", FE_CAN_ID_COMMAND, 4, {16, 0, 0, 0, 0, 0, 0, 0}, 1, 0},
    {"forged state frame, bad checksum", FE_CAN_ID_STATE, 8, {0, 0, FE_GEAR_PARK, 1, 0, 0, 0, 0x11}, 0, 0},
};

int main(int argc, char **argv) {
    const char *bus = "/dev/vcan0";
    int count = 50, opt, gap_us = 1000, forge_ms = 0;
    while ((opt = getopt(argc, argv, "n:b:g:S:")) != -1) {
        if (opt == 'n') count = atoi(optarg);
        else if (opt == 'b') bus = optarg;
        else if (opt == 'g') gap_us = atoi(optarg);
        else if (opt == 'S') forge_ms = atoi(optarg);
        else {
            fprintf(stderr, "usage: fe-canfuzz [-n count] [-g gap_us] [-S forge_ms] [-b bus]\n");
            return 2;
        }
    }
    int fd = open(bus, O_RDWR);
    if (fd < 0) {
        printf("FUZZ open %s: %s\n", bus, strerror(errno));
        return 1;
    }
    if (forge_ms > 0) {
        /* A well-formed state frame saying "stopped, in Park, parking brake on", with a correct checksum, every 5 ms:
         * what a compromised node on the bus can always send. */
        int sent = 0;
        for (int t = 0; t < forge_ms; t += 5) {
            CAN_MSG m;
            memset(&m, 0, sizeof m);
            m.mid = FE_CAN_ID_STATE;
            m.len = 8;
            uint8_t d[8] = {0, 0, (uint8_t)FE_GEAR_PARK, 1, 0x0F, 0, 0, 0};
            memcpy(m.dat, d, 8);
            m.dat[7] = fe_can_checksum(m.dat);
            if (devctl(fd, CAN_DEVCTL_TX_FRAME_RAW, &m, sizeof m, NULL) == EOK) sent++;
            usleep(5000);
        }
        printf("FUZZ forged stationary state frames, correct checksum: %d reached the bus\n", sent);
        return 0;
    }
    int total = 0, accepted_total = 0;
    for (size_t k = 0; k < sizeof KINDS / sizeof KINDS[0]; k++) {
        int accepted = 0, refused = 0;
        for (int i = 0; i < count; i++) {
            CAN_MSG m;
            memset(&m, 0, sizeof m);
            m.mid = KINDS[k].mid;
            m.len = KINDS[k].len;
            m.ext.is_extended_mid = KINDS[k].extended;
            memcpy(m.dat, KINDS[k].d, 8);
            if (KINDS[k].fix_checksum) m.dat[7] = fe_can_checksum(m.dat);
            if (devctl(fd, CAN_DEVCTL_TX_FRAME_RAW, &m, sizeof m, NULL) == EOK) accepted++;
            else refused++;
            total++;
            if (gap_us > 0) usleep((useconds_t)gap_us);
        }
        accepted_total += accepted;
        printf("FUZZ %-40s sent %d: bus accepted %d, bus refused %d\n", KINDS[k].name, count, accepted, refused);
    }
    printf("FUZZ total %d frames, %d reached the bus\n", total, accepted_total);
    return 0;
}
