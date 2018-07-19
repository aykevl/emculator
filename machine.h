
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

typedef struct {
	bool n; // negative
	bool z; // zero
	bool c; // carry
	bool v; // overflow
} flags_t;

#define MACHINE_BACKTRACE_LEN (100)

typedef struct {
	// Regular registers (r0 .. r15)
	union {
		struct {
			uint32_t r0;
			uint32_t r1;
			uint32_t r2;
			uint32_t r3;
			uint32_t r4;
			uint32_t r5;
			uint32_t r6;
			uint32_t r7;
			uint32_t r8;
			uint32_t r9;
			uint32_t r10;
			uint32_t r11;
			uint32_t r12;
			union {
				uint32_t r13;
				uint32_t sp;
			};
			union {
				uint32_t r14;
				uint32_t lr;
			};
			union {
				uint32_t r15;
				uint32_t pc;
			};
		};
		uint32_t regs[16];
	};

	// CPU flags
	flags_t psr;

	// ROM/flash area
	union {
		uint32_t *image32;
		uint16_t *image16;
		uint8_t  *image8;
	};
	size_t image_size;

	// RAM area
	union {
		uint32_t *mem32;
		uint16_t *mem16;
		uint8_t  *mem8;
	};
	size_t mem_size;

	// The NVIC peripheral
	struct {
		uint32_t ip[8]; // interrupt priority
	} nvic;

	// Statistics and backtrace depth.
	// Warning: call_depth may not fit in the backtrace! So check before
	// indexing.
	int call_depth;
	uint32_t backtrace[MACHINE_BACKTRACE_LEN];

	// misc
	int loglevel;
} machine_t;

typedef enum {
	WIDTH_8,
	WIDTH_16,
	WIDTH_32,
} width_t;

typedef enum {
	LOAD,
	STORE,
} transfer_type_t;

enum {
	ERR_OK,        // no error
	ERR_EXIT,      // program has exited (should not normally happen on a MCU)
	ERR_OTHER,     // other (already handled) error
	ERR_UNDEFINED, // undefined instruction
};

enum {
	LOG_NONE,     // only log critical errors
	LOG_WARN,     // log warnings
	LOG_CALLS,    // log all branch/stack related instructions
	LOG_CALLS_SP, // log calls and registers at stack moves
	LOG_INSTRS,   // log everything
};

void run_emulator(uint32_t *image, size_t image_size, uint32_t *ram, size_t ram_size, int loglevel);
