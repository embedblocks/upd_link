# Host tests for COMM.
#   make        build and run everything
#   make pure   pure-function tests only (HT-01..HT-09), with ASan/UBSan
#   make posix  comm.c over Linux loopback, both wait variants x three tick rates
CC      ?= gcc
OUT     ?= ./build
COMM     = ..
CFLAGS   = -std=gnu11 -Wall -Wextra -Werror -g -Istubs -I$(COMM)/include -I$(COMM)/private
SAN      = -fsanitize=address,undefined -fno-omit-frame-pointer
WRAP     = -Wl,--wrap=select,--wrap=recvmsg,--wrap=sendmsg
PURE_SRC = $(COMM)/src/comm_pure.c stubs/stubs.c

VARIANTS = select_1000 select_300 select_100 rcvtimeo_1000 rcvtimeo_300 rcvtimeo_100
POSIX    = $(addprefix $(OUT)/test_posix_,$(VARIANTS))

all: pure posix

$(OUT):
	mkdir -p $(OUT)

$(OUT)/test_pure: test_pure.c $(PURE_SRC) | $(OUT)
	$(CC) $(CFLAGS) -O1 $(SAN) $^ -o $@

$(OUT)/test_posix_select_%: test_posix.c $(COMM)/src/comm.c $(PURE_SRC) | $(OUT)
	$(CC) $(CFLAGS) -O2 -DconfigTICK_RATE_HZ=$* $(WRAP) $^ -o $@ -lpthread

$(OUT)/test_posix_rcvtimeo_%: test_posix.c $(COMM)/src/comm.c $(PURE_SRC) | $(OUT)
	$(CC) $(CFLAGS) -O2 -DconfigTICK_RATE_HZ=$* -DCOMM_WAIT_SO_RCVTIMEO=1 $(WRAP) $^ -o $@ -lpthread

pure: $(OUT)/test_pure
	$(OUT)/test_pure

posix: $(POSIX)
	@set -e; for t in $(POSIX); do echo; $$t; done

clean:
	rm -rf $(OUT)

.PHONY: all pure posix clean
