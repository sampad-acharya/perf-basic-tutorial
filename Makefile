# Usage: make help
#
# Override on the command line, e.g.  make stat MODE=branch_random CORE=4

CXX            ?= g++
CXXFLAGS       ?= -O2 -g -fno-omit-frame-pointer -Wall
MODE           ?= all
CORE           ?= 2
FLAMEGRAPH_DIR ?= $(HOME)/FlameGraph

PIN := taskset -c $(CORE)

.PHONY: all help run stat record report flamegraph clean

all: perflab

perflab: perflab.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

help:
	@echo "make                  build ./perflab"
	@echo "make run MODE=hot     run one mode (default MODE=all)"
	@echo "make stat MODE=hot    perf stat on one mode"
	@echo "make record           perf record -g on MODE, writes perf.data"
	@echo "make report           text report of perf.data"
	@echo "make flamegraph       record MODE and write flame.svg"
	@echo "make clean            remove build and perf output"
	@echo
	@echo "Variables: MODE=$(MODE) CORE=$(CORE) FLAMEGRAPH_DIR=$(FLAMEGRAPH_DIR)"

run: perflab
	$(PIN) ./perflab $(MODE)

stat: perflab
	$(PIN) perf stat ./perflab $(MODE)

record: perflab
	$(PIN) perf record -g -o perf.data ./perflab $(MODE)

report:
	perf report -i perf.data --stdio --children --sort symbol | head -40

# Brendan Gregg's FlameGraph scripts, fetched once on first use.
$(FLAMEGRAPH_DIR):
	git clone --depth 1 https://github.com/brendangregg/FlameGraph $@

flamegraph: record | $(FLAMEGRAPH_DIR)
	perf script -i perf.data \
	  | $(FLAMEGRAPH_DIR)/stackcollapse-perf.pl \
	  | $(FLAMEGRAPH_DIR)/flamegraph.pl --title "perflab $(MODE)" > flame.svg
	@echo "wrote flame.svg -- open it in a browser"

clean:
	rm -f perflab perf.data perf.data.old flame.svg
