# microwizard-meta is written in meta, which lowers to C; the C is compiled.
#
# Each source is lowered on its own into build/lowered/, beside which the
# runtime meta ships is compiled once . Headers in
# src/ are plain C, read as they are by both meta and the C compiler.
#
#   make                  build/mwm-node and the unit tests
#   make lib              build/libmwm.a: the library and the node (node.h)
#                         for a service of its own, linked with meta's runtime
#   make check            the unit tests (test/run.sh runs everything)
#   make META=/path/meta  against another meta
#   make SAN=1            the same under ASan and UBSan, in build-san/

META ?= $(shell command -v meta 2>/dev/null || echo ../../metalanguage/meta)
CC ?= cc
CFLAGS ?= -O2 -g

# SAN=1: everything, the runtime included, built with the sanitizers into a
# directory of its own, so the two builds never mix
ifeq ($(SAN),1)
B := build-san
SANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer \
            -fno-sanitize-recover=undefined
CFLAGS := -O1 -g $(SANFLAGS)
else
B := build
SANFLAGS :=
endif
WARN := -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
        -Wno-missing-field-initializers -Wno-sign-compare -Wno-discarded-qualifiers \
        -Wno-format-truncation -Wno-unused-variable -Wno-unused-but-set-variable
# a connection's read buffer starts at 64 KiB, not meta_tcp.h's 4: a peer with
# sixty-four calls in flight has more than 4 KiB of them in the socket, and a
# reader that takes them in one recv answers them in one write (serve)
WARN += -DMETA_TCP_BUFFER=65536

RT_INCLUDE := $(shell $(META) -print-config | sed -n 's/^runtime-include=//p')
RT_SOURCES := $(shell $(META) -print-config | sed -n 's/^runtime-sources=//p')
RT_OBJECTS := $(patsubst %.c,$(B)/rt/%.o,$(notdir $(RT_SOURCES)))
RT_ARCHIVE := $(shell $(META) -print-config | sed -n 's/^runtime-archive=//p')

# the TCP side needs meta_tcp.h, the loop select over descriptors and
# meta_signalFd: metalanguage parser-rewrite from 8fed00c
ifneq ($(MAKECMDGOALS),clean)
ifeq ($(wildcard $(RT_INCLUDE)/meta_tcp.h),)
$(error $(META) has no meta_tcp.h - build against parser-rewrite: META=../../metalanguage/meta)
endif
# a reader gives its carrier back after a turn (serve): meta_yield
ifeq ($(shell grep -l meta_yield $(RT_INCLUDE)/meta_tasks.h 2>/dev/null),)
$(error $(META) has no meta_yield - build against a newer parser-rewrite)
endif
endif

LIB := msgpack visigoth balance swim mesh pattern service echo client
LIB_LOWERED := $(patsubst %,build/lowered/%.c,$(LIB))
# the node itself (node.h): in mwm-node and in libmwm.a, not in the tests
NODE := main
NODE_LOWERED := $(patsubst %,build/lowered/%.c,$(NODE))
TESTS := msgpack_test visigoth_test balance_test swim_test mesh_test pattern_test service_test \
         client_test

all: $(B)/mwm-node $(patsubst %,$(B)/%,$(TESTS))

build/lowered/%.c: src/%.c src/*.h $(RT_INCLUDE)/*.h $(META)
	@mkdir -p build/lowered
	$(META) -s -emit $@ -I src $< 2>build/lowered/$*.log || \
	  { cat build/lowered/$*.log; rm -f $@; exit 1; }
	@! grep -E ': (error|warning|note):' build/lowered/$*.log || \
	  { echo "meta has something to say about $<"; rm -f $@; exit 1; }

build/lowered/%.c: test/unit/%.c src/*.h $(RT_INCLUDE)/*.h $(META)
	@mkdir -p build/lowered
	$(META) -s -emit $@ -I src $< 2>build/lowered/$*.log || \
	  { cat build/lowered/$*.log; rm -f $@; exit 1; }
	@! grep -E ': (error|warning|note):' build/lowered/$*.log || \
	  { echo "meta has something to say about $<"; rm -f $@; exit 1; }

$(B)/rt/%.o:
	@mkdir -p $(B)/rt
	$(CC) -std=gnu11 -O2 $(SANFLAGS) -I $(RT_INCLUDE) -c $(filter %/$*.c,$(RT_SOURCES)) -o $@

$(RT_OBJECTS): $(RT_SOURCES) $(wildcard $(RT_INCLUDE)/*.h)

LINK = $(CC) -std=gnu11 $(CFLAGS) $(WARN) -I src -I $(RT_INCLUDE) -o $@ $^ \
       $(RT_ARCHIVE) -lm -lpthread

$(B)/mwm-node: build/lowered/mwm-node.c $(NODE_LOWERED) $(LIB_LOWERED) $(RT_OBJECTS)
	$(LINK)

# The library for a service of its own: everything mwm-node is but its
# main, as objects in one archive. The service links it with meta's runtime
# - the same meta it is lowered with - and reads the headers in src/.
$(B)/obj/%.o: build/lowered/%.c
	@mkdir -p $(B)/obj
	$(CC) -std=gnu11 $(CFLAGS) $(WARN) -I src -I $(RT_INCLUDE) -c $< -o $@

$(B)/libmwm.a: $(patsubst %,$(B)/obj/%.o,$(LIB) $(NODE))
	rm -f $@
	ar rcs $@ $^

lib: $(B)/libmwm.a

$(B)/%_test: build/lowered/%_test.c $(LIB_LOWERED) $(RT_OBJECTS)
	$(LINK)

# client_test three times: as 1.6.2 (--no-patient), patient - the default -
# and patient with a short request timeout
check: $(patsubst %,$(B)/%,$(TESTS))
	@for t in $(filter-out client_test,$(TESTS)); do ./$(B)/$$t test/fixtures || exit 1; done
	@./$(B)/client_test test/fixtures --no-patient
	@./$(B)/client_test test/fixtures
	@./$(B)/client_test test/fixtures --short

clean:
	rm -rf build build-san

.PHONY: all lib check clean
