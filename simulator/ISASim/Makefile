# Makefile for isaSim, the Project 1 instruction level simulator.
#
#   make         build ./isaSim
#   make test    run every benchmark and compare its .mem.out with .mem.expected
#   make clean   remove ./isaSim and the files the simulator writes

CC     = gcc
CFLAGS = -std=c99 -Wall -Wextra -pedantic -O2
LDLIBS = -lm

# program:memory-file pairs that "make test" runs
BENCHMARKS = testAll:testAll \
             maxFinderInt:maxFinderInt maxFinderInt:maxFinderIntNeg \
             maxFinderFP:maxFinderFP maxFinderFP:maxFinderFPNeg

isaSim: isaSim.c
	$(CC) $(CFLAGS) -o $@ isaSim.c $(LDLIBS)

test: isaSim
	@status=0; \
	for pair in $(BENCHMARKS); do \
	    prog=$${pair%%:*}; mem=$${pair##*:}; \
	    if ./isaSim -r $$prog.asm $$mem.mem > /dev/null \
	       && cmp -s $$mem.mem.out $$mem.mem.expected; then \
	        echo "PASS  $$prog.asm with $$mem.mem"; \
	    else \
	        echo "FAIL  $$prog.asm with $$mem.mem"; status=1; \
	    fi; \
	done; \
	exit $$status

clean:
	rm -f isaSim *.mem.out *.instrCount

.PHONY: test clean
