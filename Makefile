CXX ?= c++
CC ?= cc
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wno-missing-field-initializers
QBEFLAGS ?= -std=c99 -O2 -Wno-parentheses -Wno-implicit-fallthrough

SRC := $(wildcard src/*.cpp)
HDR := $(wildcard src/*.hpp)
# the backend, embedded (MIT — src/qbe/LICENSE)
QBESRC := $(wildcard src/qbe/*.c) $(wildcard src/qbe/arm64/*.c) \
          $(wildcard src/qbe/amd64/*.c) $(wildcard src/qbe/rv64/*.c)
QBEOBJ := $(QBESRC:.c=.o)

simplec: $(SRC) $(HDR) $(QBEOBJ)
	$(CXX) $(CXXFLAGS) $(SRC) $(QBEOBJ) -o $@

QBEHDR := src/qbe/all.h src/qbe/ops.h src/qbe/config.h

src/qbe/%.o: src/qbe/%.c $(QBEHDR)
	$(CC) $(QBEFLAGS) -Isrc/qbe -c $< -o $@

.PHONY: clean test bench
clean:
	rm -rf simplec build perf/build $(QBEOBJ)

test: simplec
	sh tests/run_tests.sh
	sh tests/safety/run_safety.sh

bench: simplec
	python3 perf/run.py
