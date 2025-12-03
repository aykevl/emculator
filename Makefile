
CFLAGS=-Wall -Werror -O2 -std=c11 -DEMCULATOR_MAIN=1
LDFLAGS=$(CFLAGS)

.PHONY: all clean wasm

all: emculator

clean:
	rm -rf emculator machine.wasm *.o

emculator: emculator.o machine.o terminal.o

wasm: machine.wasm

machine.wasm: machine.c
	wasi-clang -o machine.wasm machine.c -Wall -Werror -O2 -std=c11 -mexec-model=reactor -Wl,--strip-debug -Wl,--compress-relocations -Wl,--export-dynamic
