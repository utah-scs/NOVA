SRC := $(wildcard *.c)
OBJ := $(foreach f,$(SRC),$(f:%.c=%.o))

CFLAGS += -Wall -O2 -I../../deps/ubpf/vm/inc -target bpf

ARCH := $(shell uname -m)

ifeq ($(ARCH),aarch64)
  CFLAGS += -I/usr/include/aarch64-linux-gnu/ 
endif

all: $(OBJ)

#bpf: CFLAGS += -target bpf
#bpf: $(OBJ)

%.o: %.c
	clang $(CFLAGS) -c $< -o $@

.PHONY: clean

clean:
	rm -rf *.o
