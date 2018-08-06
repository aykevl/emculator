
EMCC_CFLAGS=-Wall -Werror -Os -std=c11 -s WASM=1 -s SIDE_MODULE=0 -s "BINARYEN_METHOD='native-wasm'" -s TOTAL_MEMORY=2MB -s TOTAL_STACK=64KB
CFLAGS=-Wall -Werror -O2 -std=c11 -DEMCULATOR_MAIN=1
LDFLAGS=$(CFLAGS)

.PHONY: all clean web

all: emculator

clean:
	rm -rf emculator *.o web/machine.*

emculator: emculator.o machine.o terminal.o

web: web/machine.js

web/machine.js: machine.c
	emcc $^ $(EMCC_CFLAGS) -o $@
