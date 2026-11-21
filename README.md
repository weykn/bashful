# bashful

Compiles bash scripts into native executables that behave exactly like bash but faster
(10x to 900x on the benchmarks).

Scripts go through LLVM IR, and common tools (`cat`, `grep`, `sort`, ...) are built into the executable,
so loops, `$(...)`, subshells and pipelines run without forking. If a script uses something bashful can't
compile yet, the whole script runs through the system bash instead: still correct, just not faster.

The compiler is written in bash and compiles itself.

## Build

Needs `bash`, `clang` and `llvm-link` (`ld.lld` is used when installed).

```sh
make              # build ./bashful
```

## Usage

```sh
./bashful main.sh -o main    # compile
./bashful -r main.sh         # compile and run
./bashful -c 'echo hi'       # compile and run a command
```

| Option | |
|---|---|
| `-o`, `--output FILE` | write the executable to `FILE` |
| `-r`, `--run` | compile and run, passing the remaining arguments to the script |
| `-c`, `--command STR` | compile and run `STR` |
| `--emit-ir`, `--emit-bc` | write LLVM IR / bitcode instead of an executable |
| `--strict` | fail on unsupported features instead of falling back to bash |
| `--no-embed` | run `cat`, `grep`, `sort`, ... from the system instead of the built-in copies |
| `-q`, `--quiet` | no note when falling back to bash |

## Tests

```sh
make test                  # every script's stdout and exit status must match bash
make bench                 # run times against bash, best of 3
make compile-bench         # how long compiling takes
```
