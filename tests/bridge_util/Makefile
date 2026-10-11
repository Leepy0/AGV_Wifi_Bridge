# 브리지 공통 호스트 시험 (ESP-IDF 없이 gcc): make -C tests/bridge_util
CC     ?= cc
CFLAGS ?= -std=gnu17 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter
MAIN   := ../../main
INC    := -I$(MAIN) -I$(MAIN)/../dsp/common -Istubs
BIN    := build

TESTS  := $(BIN)/test_bridge_util $(BIN)/test_ota_start $(BIN)/test_cmd

all: run

$(BIN):
	mkdir -p $(BIN)

$(BIN)/test_bridge_util: test_bridge_util.c $(MAIN)/bridge_util.h | $(BIN)
	$(CC) $(CFLAGS) $(INC) -o $@ test_bridge_util.c

$(BIN)/test_ota_start: test_ota_start.c $(MAIN)/ota.c $(MAIN)/ota.h $(MAIN)/bridge_util.h | $(BIN)
	$(CC) $(CFLAGS) $(INC) -o $@ test_ota_start.c $(MAIN)/ota.c

$(BIN)/test_cmd: test_cmd.c $(MAIN)/cmd.c $(MAIN)/cmd.h | $(BIN)
	$(CC) $(CFLAGS) $(INC) -o $@ test_cmd.c $(MAIN)/cmd.c

run: $(TESTS)
	@set -e; for t in $(TESTS); do ./$$t; done

clean:
	rm -rf $(BIN)

.PHONY: all run clean
