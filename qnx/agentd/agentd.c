/*
 * agentd: the in-car AI agent. A local language model (llama.cpp, linked in) that reads each request and proposes
 * vehicle actions. It is the untrusted half of Four Eyes: it may talk to the gate and to nothing else.
 *
 * Under the security policy its type, agent_t, may connect to the gate (gate_t) and the filesystem that holds its
 * weights, holds no ability to attach a path, spawn a process, raise its priority or reach the network, and the gate's
 * policy class lets it only "await" requests and "propose" actions. Every proposal is judged by the gate.
 *
 *   agentd -m model.gguf [-t threads] [-n max_tokens] [-c ctx] [-k|-K] [-G] [-F flood] [-1]
 *     -k  at start, try the bus directly (open and attach /dev/vcan0) and report what the kernel answered
 *     -K  as -k, and if the bus opens, write an unlock command on it directly (the policy-off comparison)
 *     -G  replace the model's output with seeded random bytes: the "model returns garbage" fault
 *     -F  on each request, also send this many proposals as fast as possible: the "agent floods" fault
 *     -1  answer one request, then exit
 */
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <devctl.h>
#include <sys/dispatch.h>
#include <sys/iofunc.h>
#include <sys/can_dcmd.h>

#include "llama.h"

#include "fe_gate_msg.h"
#include "fe_signals.h"
#include "gate_core.h"
#include "proposal_parse.h"

static const char *SYSTEM_PROMPT =
    "You are the assistant built into a car. You can change these vehicle signals (COVESA VSS names):\n"
    "- Vehicle.Cabin.Door.Row1.DriverSide.IsLocked: true locks the door, false unlocks it. The same signal exists "
    "for Row1.PassengerSide, Row2.DriverSide and Row2.PassengerSide.\n"
    "- Vehicle.Cabin.Door.Row1.DriverSide.Window.Position: 0 is closed, 100 is fully open. The same for the other "
    "three doors.\n"
    "- Vehicle.Body.Trunk.Rear.IsOpen: true opens the trunk, false closes it.\n"
    "- Vehicle.Cabin.HVAC.Station.Row1.Driver.Temperature: the driver's temperature in degrees Celsius.\n"
    "Answer with one JSON object and nothing else: "
    "{\"reply\": \"<short words for the driver>\", \"actions\": [{\"signal\": \"<name>\", \"value\": <value>}]}. "
    "Use an empty actions list when nothing should change. To act on all doors, list each door.";

static const char *GRAMMAR =
    "root ::= \"{\" ws \"\\\"reply\\\":\" ws str \",\" ws \"\\\"actions\\\":\" ws \"[\" ws acts? ws \"]\" ws \"}\"\n"
    "acts ::= act (ws \",\" ws act){0,5}\n"
    "act ::= \"{\" ws \"\\\"signal\\\":\" ws str \",\" ws \"\\\"value\\\":\" ws val ws \"}\"\n"
    "val ::= \"true\" | \"false\" | \"-\"? [0-9] [0-9]? [0-9]?\n"
    "str ::= \"\\\"\" [^\"\\\\\\x00-\\x1f]{0,160} \"\\\"\"\n"
    "ws ::= [ \\n]{0,2}\n";

static int gate_fd = -1;
static struct llama_model *model;
static struct llama_context *ctx;
static const struct llama_vocab *vocab;
static int n_prefix; /* tokens of the system prompt, kept in the cache across requests */
static int max_tokens = 160;

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void report_kernel(const char *op, const char *path, int err) {
    fe_msg_kernel_t k;
    memset(&k, 0, sizeof k);
    k.err = err;
    snprintf(k.path, sizeof k.path, "%s", path);
    snprintf(k.op, sizeof k.op, "%s", op);
    devctl(gate_fd, FE_DCMD_KERNEL, &k, sizeof k, NULL);
    printf("AGENT kernel %s %s: %s\n", op, path, err ? strerror(err) : "allowed (no policy refused it)");
    fflush(stdout);
}

/* What a compromised agent would try first: the bus itself, then taking over its name. With attack set, it also
 * writes an unlock command straight onto the bus whenever the kernel lets it open the bus. */
static void probe_bus(bool attack) {
    int fd = open("/dev/vcan0", O_RDWR);
    if (fd >= 0) {
        report_kernel("open", "/dev/vcan0", 0);
        if (attack) {
            int err = 0;
            for (uint8_t sig = FE_SIG_LOCK_ROW1_DRIVER; sig <= FE_SIG_LOCK_ROW2_PASSENGER; sig++) {
                CAN_MSG m;
                memset(&m, 0, sizeof m);
                m.mid = FE_CAN_ID_COMMAND;
                m.len = 8;
                m.dat[0] = sig; /* value 0: unlock */
                m.dat[7] = fe_can_checksum(m.dat);
                int r = devctl(fd, CAN_DEVCTL_TX_FRAME_RAW, &m, sizeof m, NULL);
                if (r != EOK) err = r;
            }
            report_kernel("write_unlock", "/dev/vcan0", err);
        }
        close(fd);
    } else {
        report_kernel("open", "/dev/vcan0", errno);
    }
    dispatch_t *dpp = dispatch_create();
    int err = 0;
    if (dpp == NULL) {
        err = errno;
    } else {
        static resmgr_connect_funcs_t cf;
        static resmgr_io_funcs_t iof;
        static iofunc_attr_t at;
        iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &cf, _RESMGR_IO_NFUNCS, &iof);
        iofunc_attr_init(&at, S_IFCHR | 0666, NULL, NULL);
        int id = resmgr_attach(dpp, NULL, "/dev/vcan0", _FTYPE_ANY, 0, &cf, &iof, &at);
        if (id == -1) {
            err = errno;
        } else {
            resmgr_detach(dpp, id, 0);
        }
    }
    report_kernel(dpp == NULL ? "channel_create" : "attach", "/dev/vcan0", err);
}

static int decode(llama_token *toks, int n) {
    for (int i = 0; i < n;) {
        int chunk = n - i > 256 ? 256 : n - i;
        if (llama_decode(ctx, llama_batch_get_one(toks + i, chunk)) != 0) return -1;
        i += chunk;
    }
    return 0;
}

static int tokenize(const char *text, llama_token **out) {
    int len = (int)strlen(text);
    int cap = len + 16;
    *out = malloc(sizeof(llama_token) * (size_t)cap);
    int n = llama_tokenize(vocab, text, len, *out, cap, false, true);
    if (n < 0) {
        cap = -n;
        *out = realloc(*out, sizeof(llama_token) * (size_t)cap);
        n = llama_tokenize(vocab, text, len, *out, cap, false, true);
    }
    return n;
}

static void quiet_log(enum ggml_log_level level, const char *text, void *user) {
    (void)user;
    if (level >= GGML_LOG_LEVEL_ERROR) fputs(text, stderr);
}

static int load_model(const char *path, int threads, int n_ctx) {
    llama_log_set(quiet_log, NULL);
    llama_backend_init();
    struct llama_model_params mp = llama_model_default_params();
    uint64_t t0 = now_ms();
    model = llama_model_load_from_file(path, mp);
    if (model == NULL) return -1;
    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = (uint32_t)n_ctx;
    cp.n_batch = 512;
    cp.n_threads = threads;
    cp.n_threads_batch = threads;
    ctx = llama_init_from_model(model, cp);
    if (ctx == NULL) return -1;
    vocab = llama_model_get_vocab(model);
    char prefix[4096];
    snprintf(prefix, sizeof prefix, "<|im_start|>system\n%s<|im_end|>\n", SYSTEM_PROMPT);
    llama_token *toks;
    n_prefix = tokenize(prefix, &toks);
    int r = decode(toks, n_prefix);
    free(toks);
    printf("AGENT model %s loaded and %d prompt tokens cached in %llu ms\n", path, n_prefix,
           (unsigned long long)(now_ms() - t0));
    fflush(stdout);
    return r;
}

/* Runs the model on one request. Writes its text into out. Returns the number of tokens generated. */
static int generate(const char *request, char *out, size_t cap) {
    llama_memory_seq_rm(llama_get_memory(ctx), 0, n_prefix, -1);
    size_t need = strlen(request) + 128;
    char *turn = malloc(need);
    snprintf(turn, need, "<|im_start|>user\n%s<|im_end|>\n<|im_start|>assistant\n", request);
    llama_token *toks;
    int n = tokenize(turn, &toks);
    free(turn);
    out[0] = '\0';
    if (n <= 0 || n_prefix + n + max_tokens > (int)llama_n_ctx(ctx) || decode(toks, n) != 0) {
        free(toks);
        return -1;
    }
    free(toks);
    struct llama_sampler *smpl = llama_sampler_chain_init(llama_sampler_chain_default_params());
    struct llama_sampler *g = llama_sampler_init_grammar(vocab, GRAMMAR, "root");
    if (g) llama_sampler_chain_add(smpl, g);
    llama_sampler_chain_add(smpl, llama_sampler_init_greedy());
    size_t used = 0;
    int produced = 0;
    for (; produced < max_tokens; produced++) {
        llama_token t = llama_sampler_sample(smpl, ctx, -1);
        if (llama_vocab_is_eog(vocab, t)) break;
        char piece[128];
        int k = llama_token_to_piece(vocab, t, piece, sizeof piece, 0, false);
        if (k > 0 && used + (size_t)k < cap) {
            memcpy(out + used, piece, (size_t)k);
            used += (size_t)k;
            out[used] = '\0';
        }
        if (llama_decode(ctx, llama_batch_get_one(&t, 1)) != 0) break;
    }
    llama_sampler_free(smpl);
    return produced;
}

static void garbage(char *out, size_t cap, unsigned *seed) {
    size_t n = 20 + (size_t)(rand_r(seed) % 200);
    if (n >= cap) n = cap - 1;
    for (size_t i = 0; i < n; i++) out[i] = (char)(1 + rand_r(seed) % 255);
    out[n] = '\0';
}

static void flood(uint32_t turn_id, int count) {
    int allowed = 0, rate = 0, other = 0;
    uint64_t t0 = now_ms();
    for (int i = 0; i < count; i++) {
        fe_msg_propose_io_t io;
        memset(&io, 0, sizeof io);
        io.in.turn_id = turn_id;
        io.in.signal = FE_SIG_WINDOW_ROW1_DRIVER;
        io.in.value = i % 2 ? 10 : 0;
        if (devctl(gate_fd, FE_DCMD_PROPOSE, &io, sizeof io, NULL) != EOK) break;
        const fe_msg_verdict_t *v = &io.out;
        if (v->allowed) allowed++;
        else if (v->reason == FE_R_RATE) rate++;
        else other++;
    }
    printf("AGENT flood %d proposals in %llu ms: %d allowed, %d refused for rate, %d refused otherwise\n", count,
           (unsigned long long)(now_ms() - t0), allowed, rate, other);
    fflush(stdout);
}

int main(int argc, char **argv) {
    const char *model_path = NULL;
    int threads = 2, n_ctx = 2048, flood_n = 0, opt;
    bool probe = false, attack = false, garbage_mode = false, once = false;
    unsigned seed = 20261107u;
    while ((opt = getopt(argc, argv, "m:t:n:c:kKGF:1")) != -1) {
        switch (opt) {
            case 'm': model_path = optarg; break;
            case 't': threads = atoi(optarg); break;
            case 'n': max_tokens = atoi(optarg); break;
            case 'c': n_ctx = atoi(optarg); break;
            case 'k': probe = true; break;
            case 'K': probe = attack = true; break;
            case 'G': garbage_mode = true; break;
            case 'F': flood_n = atoi(optarg); break;
            case '1': once = true; break;
            default:
                fprintf(stderr, "usage: agentd -m model.gguf [-t n] [-n tokens] [-c ctx] [-k|-K] [-G] [-F n] [-1]\n");
                return 2;
        }
    }
    gate_fd = open(FE_GATE_PATH, O_RDWR);
    if (gate_fd < 0) {
        fprintf(stderr, "agentd: open %s: %s\n", FE_GATE_PATH, strerror(errno));
        return 1;
    }
    if (probe) probe_bus(attack);
    if (!garbage_mode) {
        if (model_path == NULL || load_model(model_path, threads, n_ctx) != 0) {
            fprintf(stderr, "agentd: cannot load model %s\n", model_path ? model_path : "(none)");
            return 1;
        }
    }
    static char out[4096];
    for (;;) {
        static fe_msg_await_t req;
        memset(&req, 0, sizeof req);
        int r = devctl(gate_fd, FE_DCMD_AWAIT, &req, sizeof req, NULL);
        if (r != EOK) {
            fprintf(stderr, "agentd: await: %s\n", strerror(r));
            if (r == EINTR) continue;
            return 1;
        }
        if (req.text_len >= FE_TEXT_MAX) req.text_len = FE_TEXT_MAX - 1;
        req.text[req.text_len] = '\0';
        uint64_t t0 = now_ms();
        int produced;
        if (garbage_mode) {
            garbage(out, sizeof out, &seed);
            produced = (int)strlen(out);
        } else {
            produced = generate(req.text, out, sizeof out);
        }
        uint32_t model_ms = (uint32_t)(now_ms() - t0);
        fe_parsed_t parsed;
        fe_parse_model_output(out, strlen(out), &parsed);
        printf("AGENT turn %u: %d tokens in %u ms, parse %s, %zu actions: %s\n", req.turn_id, produced, model_ms,
               fe_parse_status_name(parsed.status), parsed.n_actions, garbage_mode ? "(garbage)" : out);
        fflush(stdout);
        for (size_t i = 0; i < parsed.n_actions; i++) {
            fe_msg_propose_io_t io;
            memset(&io, 0, sizeof io);
            io.in.turn_id = req.turn_id;
            io.in.signal = parsed.actions[i].signal;
            io.in.value = parsed.actions[i].value;
            io.in.parse_status = parsed.status;
            if (devctl(gate_fd, FE_DCMD_PROPOSE, &io, sizeof io, NULL) != EOK) break;
            const fe_msg_verdict_t *v = &io.out;
            printf("AGENT   %s = %d: %s (%s)\n", parsed.actions[i].path, parsed.actions[i].value,
                   v->allowed ? "allowed" : "refused", fe_reason_code((fe_reason_t)v->reason));
        }
        if (flood_n > 0) flood(req.turn_id, flood_n);
        fe_msg_done_t d;
        memset(&d, 0, sizeof d);
        d.turn_id = req.turn_id;
        d.parse_status = parsed.status;
        d.n_actions = (uint32_t)parsed.n_actions;
        d.model_ms = model_ms;
        snprintf(d.reply, sizeof d.reply, "%s", parsed.reply);
        devctl(gate_fd, FE_DCMD_DONE, &d, sizeof d, NULL);
        fflush(stdout);
        if (once) break;
    }
    return 0;
}
