# bashful: bash -> LLVM IR compiler, written in bash and compiled by itself, + the embedded C runtime
RT      := $(CURDIR)/runtime
OPT     := -O2
SRCS    := compiler/util.sh compiler/parser.sh compiler/check.sh compiler/emit.sh compiler/arith.sh \
           compiler/test.sh compiler/gen.sh compiler/compile.sh compiler/rtlink.sh compiler/readline.sh compiler/repl.sh compiler/main.sh
SHLIB   := $(wildcard shlib/*.sh)
RTSRCS  := $(wildcard runtime/*.c runtime/*.h runtime/*.def)

all: bashful

# the compiler as one bash script (bashful compiles a single file: there is no `source`)
# (shlib/NAME.sh, the builtins written in bash, become SHLIB_NAME='<source>')
build/bashful.sh: $(SRCS) $(SHLIB) Makefile
	@mkdir -p build
	@{ printf '#!/usr/bin/env bash\n# bashful compiler: generated from compiler/*.sh by make\n'; \
	  printf "RUNTIME_DIR='%s'\nOPT='%s'\n" '$(RT)' '$(OPT)'; \
	  for f in $(SHLIB); do n=$${f##*/}; printf "SHLIB_%s='" "$${n%.sh}"; sed "s/'/'\\\\''/g" "$$f"; printf "'\n"; done; \
	  cat $(SRCS); printf 'main "$$@"\n'; } > $@.tmp
	@chmod +x $@.tmp && mv $@.tmp $@

# stage 1: the previous native compiler (if there is a working one) or, from scratch, the system bash running
#          the compiler on its own source (slow: bash is quadratic on long strings)
# stage 2: the new compiler compiles itself
bashful: build/bashful.sh $(RTSRCS)
	@if [ -x ./bashful ] && ./bashful --strict -q build/bashful.sh -o build/stage1 2> /dev/null; then \
		echo "stage 1: compiled by the previous ./bashful"; \
	else \
		echo "stage 1: compiled by bash (bootstrap, takes a minute or two)"; \
		bash build/bashful.sh --strict -q build/bashful.sh -o build/stage1; \
	fi
	./build/stage1 --strict -q build/bashful.sh -o build/stage2
	@mv build/stage2 $@ && rm -f build/stage1

# from scratch, with nothing but bash and clang
bootstrap:
	rm -f bashful
	$(MAKE) bashful

# self-hosting check: the native compiler must generate exactly the IR the bash-interpreted one generates
selfcheck: bashful
	bash build/bashful.sh --strict --emit-ir build/bashful.sh -o build/stage0.ll
	./bashful --strict --emit-ir build/bashful.sh -o build/stage1.ll
	cmp build/stage0.ll build/stage1.ll && echo "selfcheck: stage0 and stage1 generate identical IR"

# correctness cases + benchmarks: every script must produce exactly what bash produces
test: bashful
	./tests/bench.sh tests/cases tests/bench

bench: bashful
	./tests/bench.sh -n 3 tests/bench

# how long compiling takes (binaries are not run)
compile-bench: bashful
	./tests/compile_bench.sh -n 5 tests/bench

clean:
	rm -rf bashful bashful.tmp build

.PHONY: all bootstrap selfcheck test bench compile-bench clean
