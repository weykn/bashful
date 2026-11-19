# Bashful Compiler: bash -> LLVM IR compiler (written in C) + embedded runtime
CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wno-unused-parameter -Wno-misleading-indentation
RT      := $(CURDIR)/runtime

all: bashful

bashful: compiler/bashful.c runtime/tools.def runtime/bash_runtime.h
	$(CC) $(CFLAGS) -I$(RT) -DRUNTIME_DIR='"$(RT)"' $< -o $@

test: bashful
	./tests/run_tests.sh

clean:
	rm -f bashful tests/out/*

.PHONY: all test clean
