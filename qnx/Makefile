# Builds Four Eyes' QNX 8.0 programs for x86_64 with the SDP's GCC. Source qnxsdp-env.sh first.
#
#   make -C qnx O=<build dir> LLAMA_BUILD=<llama.cpp build dir>
#
# The agent links llama.cpp's static libraries, built by llm/build_llama.sh at the revision pinned there.

O ?= $(HOME)/Four-Eyes-work/build/x86_64
LLAMA_REV := aa94f2086
LLAMA_SRC ?= $(HOME)/Four-Eyes-work/llama.cpp-$(LLAMA_REV)
LLAMA_BUILD ?= $(HOME)/Four-Eyes-work/build/llama-qnx-$(LLAMA_REV)

CC := $(QNX_HOST)/usr/bin/x86_64-pc-nto-qnx8.0.0-gcc
CXX := $(QNX_HOST)/usr/bin/x86_64-pc-nto-qnx8.0.0-g++
CFLAGS := -std=gnu11 -O2 -g -Wall -Wextra -Wno-unused-parameter -D_QNX_SOURCE -Iinclude -Icore
LLAMA_LIBS := $(LLAMA_BUILD)/src/libllama.a $(LLAMA_BUILD)/ggml/src/libggml.a \
	$(LLAMA_BUILD)/ggml/src/libggml-cpu.a $(LLAMA_BUILD)/ggml/src/libggml-base.a

CORE := core/gate_core.c core/proposal_parse.c core/record.c core/sha256.c
PROGS := $(O)/vcand $(O)/gated $(O)/bodyd $(O)/agentd $(O)/fe-bridge $(O)/fe-canfuzz

all: $(PROGS)

$(O):
	mkdir -p $(O)

$(O)/fe-canfuzz: canfuzz/fe-canfuzz.c | $(O)
	$(CC) $(CFLAGS) -o $@ $^

$(O)/vcand: vcand/vcand.c | $(O)
	$(CC) $(CFLAGS) -o $@ $^ -lsecpol

$(O)/gated: gated/gated.c $(CORE) | $(O)
	$(CC) $(CFLAGS) -o $@ $^ -lsecpol

$(O)/bodyd: bodyd/bodyd.c core/gate_core.c | $(O)
	$(CC) $(CFLAGS) -o $@ $^ -lsecpol

$(O)/fe-bridge: bridge/fe-bridge.c core/gate_core.c | $(O)
	$(CC) $(CFLAGS) -o $@ $^

$(O)/agentd.o: agentd/agentd.c | $(O)
	$(CC) $(CFLAGS) -I$(LLAMA_SRC)/include -I$(LLAMA_SRC)/ggml/include -c -o $@ $<

$(O)/agentd: $(O)/agentd.o $(CORE) | $(O)
	$(CC) $(CFLAGS) -c core/gate_core.c -o $(O)/a_gate_core.o
	$(CC) $(CFLAGS) -c core/proposal_parse.c -o $(O)/a_proposal_parse.o
	$(CXX) -o $@ $(O)/agentd.o $(O)/a_gate_core.o $(O)/a_proposal_parse.o $(LLAMA_LIBS) -lm

.PHONY: all
