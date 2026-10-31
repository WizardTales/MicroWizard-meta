# The benchmarks of WizardTales/microservice-benchmarks, with meta.
#
#   make -C bench                         against ../src, into bench/build
#   make -C bench SRC=/other/src B=build-other   against another tree's src
#
# The library is lowered from $(SRC) into $(B)/lowered, as the top Makefile
# does it; the bench programs beside it. bench/run.sh runs them.

META ?= $(shell command -v meta 2>/dev/null || echo ../../../metalanguage/meta)
SRC ?= ../src
B ?= build
CC ?= cc
CFLAGS ?= -O2 -g
WARN := -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
        -Wno-missing-field-initializers -Wno-sign-compare -Wno-discarded-qualifiers \
        -Wno-format-truncation -Wno-unused-variable -Wno-unused-but-set-variable
# a connection's read buffer starts at 64 KiB, not meta_tcp.h's 4: a peer with
# sixty-four calls in flight has more than 4 KiB of them in the socket, and a
# reader that takes them in one recv answers them in one write (serve)
WARN += -DMETA_TCP_BUFFER=65536

RT_INCLUDE := $(shell $(META) -print-config | sed -n 's/^runtime-include=//p')
RT_SOURCES := $(shell $(META) -print-config | sed -n 's/^runtime-sources=//p')
RT_ARCHIVE := $(shell $(META) -print-config | sed -n 's/^runtime-archive=//p')
RT_OBJECTS := $(patsubst %.c,$(B)/rt/%.o,$(notdir $(RT_SOURCES)))

LIB := msgpack visigoth balance swim mesh pattern service echo client
LIB_LOWERED := $(patsubst %,$(B)/lowered/%.c,$(LIB)) $(B)/lowered/add.c

all: $(B)/bench-local $(B)/bench-client $(B)/bench-node $(B)/loadgen

$(B)/lowered/%.c: $(SRC)/%.c $(SRC)/*.h $(RT_INCLUDE)/*.h
	@mkdir -p $(B)/lowered
	$(META) -s -emit $@ -I $(SRC) $< 2>$(B)/lowered/$*.log || \
	  { cat $(B)/lowered/$*.log; rm -f $@; exit 1; }

$(B)/lowered/%.c: %.c $(SRC)/*.h *.h $(RT_INCLUDE)/*.h
	@mkdir -p $(B)/lowered
	$(META) -s -emit $@ -I . -I $(SRC) $< 2>$(B)/lowered/$*.log || \
	  { cat $(B)/lowered/$*.log; rm -f $@; exit 1; }

# src/main.c with its main renamed, then addnode.c
$(B)/node.c: $(SRC)/main.c addnode.c
	@mkdir -p $(B)
	sed 's/^int main(int argc, char \*\*argv) {$$/static int mwm_node_main(int argc, char **argv) {/' $(SRC)/main.c > $@
	grep -q '^static int mwm_node_main' $@
	cat addnode.c >> $@

$(B)/lowered/node.c: $(B)/node.c $(SRC)/*.h *.h $(RT_INCLUDE)/*.h
	@mkdir -p $(B)/lowered
	$(META) -s -emit $@ -I . -I $(SRC) $< 2>$(B)/lowered/node.log || \
	  { cat $(B)/lowered/node.log; rm -f $@; exit 1; }

$(B)/rt/%.o:
	@mkdir -p $(B)/rt
	$(CC) -std=gnu11 -O2 -I $(RT_INCLUDE) -c $(filter %/$*.c,$(RT_SOURCES)) -o $@

# a newer runtime rebuilds them, as in the top Makefile
$(RT_OBJECTS): $(RT_SOURCES) $(wildcard $(RT_INCLUDE)/*.h)

LINK = $(CC) -std=gnu11 $(CFLAGS) $(WARN) -I . -I $(SRC) -I $(RT_INCLUDE) -o $@ $^ \
       $(RT_ARCHIVE) -lm -lpthread

$(B)/bench-local: $(B)/lowered/local.c $(LIB_LOWERED) $(RT_OBJECTS)
	$(LINK)

$(B)/bench-client: $(B)/lowered/caller.c $(LIB_LOWERED) $(RT_OBJECTS)
	$(LINK)

$(B)/bench-node: $(B)/lowered/node.c $(LIB_LOWERED) $(RT_OBJECTS)
	$(LINK)

# plain C, no meta: the load generator of loadgen.sh
$(B)/loadgen: loadgen.c
	@mkdir -p $(B)
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -o $@ $< -lpthread

clean:
	rm -rf build build-*

.PHONY: all clean
