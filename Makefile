CC ?= gcc
CFLAGS ?= -Wall -Wextra -Werror -std=c11 -D_GNU_SOURCE -pthread -Iinclude
ENGINE_BIN := bin/engine
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

.PHONY: all engine module tests clean

all: engine tests module

engine: $(ENGINE_BIN)

$(ENGINE_BIN): src/engine.c include/osj_uapi.h | bin logs
	$(CC) $(CFLAGS) -o $@ src/engine.c

tests: tests/test_memory_overflow

tests/test_memory_overflow: tests/test_memory_overflow.c
	$(CC) $(CFLAGS) -o $@ $<

module:
	@if [ ! -d "$(KDIR)" ]; then \
		echo "Kernel build tree not found at $(KDIR). Build this target on Linux with kernel headers installed."; \
		exit 1; \
	fi
	$(MAKE) -C $(KDIR) M=$(PWD)/kernel modules

bin:
	mkdir -p bin

logs:
	mkdir -p logs

clean:
	rm -rf bin logs tests/test_memory_overflow
	$(MAKE) -C $(KDIR) M=$(PWD)/kernel clean || true
