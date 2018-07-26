
CFLAGS=-Wall -Werror -O2 -std=c11 -DEMCULATOR_MAIN=1
LDFLAGS=$(CFLAGS)

.PHONY: all clean

all: emculator

clean:
	rm -rf emculator *.o

emculator: emculator.o machine.o terminal.o
