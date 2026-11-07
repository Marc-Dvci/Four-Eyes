/*
 * fe-bridge: the head unit's link to the host, over standard input and output (the host runs it through ssh).
 *
 * It opens requests at the gate on behalf of the driver, passes the driver's controls to the body controller, and
 * streams the gate's record and the vehicle state back. Its type may open requests and read the record; it cannot
 * propose an action and cannot reach the bus.
 *
 * Input lines:
 *   turn driver|message <hex of UTF-8 text>
 *   drive speed <km/h> | drive gear P|R|N|D | drive brake 0|1 | drive reset
 *   quit
 * Output lines:
 *   T <turn id> <invisible code points counted by the gate>   after each accepted turn
 *   E <errno text>                                            when a command is refused
 *   R <record line>                                           every new line of the gate's record
 *   S <vehicle state JSON>                                    every 100 ms
 */
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>
#include <devctl.h>

#include "fe_gate_msg.h"
#include "gate_core.h"

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void submit(int gate, const char *origin, const char *hex) {
    static fe_msg_turn_t t;
    memset(&t, 0, sizeof t);
    t.origin = strcmp(origin, "driver") == 0 ? FE_ORIGIN_DRIVER : FE_ORIGIN_MESSAGE;
    size_t n = 0;
    for (const char *p = hex; p[0] && p[1] && n < FE_TEXT_MAX; p += 2) {
        int hi = hexval(p[0]), lo = hexval(p[1]);
        if (hi < 0 || lo < 0) break;
        t.text[n++] = (char)(hi << 4 | lo);
    }
    t.text_len = (uint32_t)n;
    int r = devctl(gate, FE_DCMD_SUBMIT_TURN, &t, sizeof t, NULL);
    if (r != EOK) {
        printf("E %s\n", strerror(r));
        return;
    }
    fe_msg_turn_reply_t *out = (fe_msg_turn_reply_t *)&t;
    printf("T %u %u\n", out->turn_id, out->invisible);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    int gate = open(FE_GATE_PATH, O_RDWR);
    if (gate < 0) {
        printf("E open %s: %s\n", FE_GATE_PATH, strerror(errno));
        return 1;
    }
    int drive = open("/dev/fe/drive", O_RDWR);
    if (drive < 0) printf("E open /dev/fe/drive: %s\n", strerror(errno));
    static fe_msg_read_t rd;
    uint64_t since = 0;
    /* start from the newest lines only: the host asks for history with "history" */
    {
        memset(&rd, 0, sizeof rd);
        ((fe_msg_read_req_t *)&rd)->since_seq = UINT64_MAX;
        if (devctl(gate, FE_DCMD_READ, &rd, sizeof rd, NULL) == EOK) since = rd.next_seq;
    }
    char line[4096];
    for (;;) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(0, &fds);
        struct timeval tv = {0, 100000};
        int r = select(1, &fds, NULL, NULL, &tv);
        if (r > 0) {
            if (fgets(line, sizeof line, stdin) == NULL) break;
            line[strcspn(line, "\r\n")] = '\0';
            char *cmd = strtok(line, " ");
            if (cmd == NULL) continue;
            if (strcmp(cmd, "quit") == 0) break;
            if (strcmp(cmd, "turn") == 0) {
                char *origin = strtok(NULL, " ");
                char *hex = strtok(NULL, " ");
                submit(gate, origin ? origin : "message", hex ? hex : "");
            } else if (strcmp(cmd, "drive") == 0 && drive >= 0) {
                char *rest = strtok(NULL, "");
                if (rest && write(drive, rest, strlen(rest)) < 0) printf("E drive: %s\n", strerror(errno));
            } else if (strcmp(cmd, "history") == 0) {
                since = 0;
            } else {
                printf("E unknown command\n");
            }
        }
        /* new record lines */
        for (int k = 0; k < 8; k++) {
            memset(&rd, 0, offsetof(fe_msg_read_t, data));
            ((fe_msg_read_req_t *)&rd)->since_seq = since;
            if (devctl(gate, FE_DCMD_READ, &rd, sizeof rd, NULL) != EOK) break;
            if (rd.n_bytes == 0) break;
            char *p = rd.data, *end = rd.data + rd.n_bytes;
            while (p < end) {
                char *nl = memchr(p, '\n', (size_t)(end - p));
                if (nl == NULL) break;
                printf("R %.*s\n", (int)(nl - p), p);
                p = nl + 1;
            }
            since = rd.next_seq;
        }
        if (drive >= 0) {
            char s[600];
            ssize_t n = pread(drive, s, sizeof s - 1, 0);
            if (n > 0) {
                s[n] = '\0';
                s[strcspn(s, "\n")] = '\0';
                printf("S %s\n", s);
            }
        }
    }
    return 0;
}
